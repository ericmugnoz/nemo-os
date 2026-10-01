// nemofs.c — Nemo OS
//
// Sistema de archivos propio. Diseño deliberadamente simple:
//
//   Sector 0:              Superbloque
//   Sectores siguientes:   Tabla de inodos (tamaño decidido al formatear)
//   Sectores siguientes:   Bitmap de bloques de datos libres/ocupados
//   Resto del disco:       Bloques de datos (1 bloque = 1 sector = 512B)
//
// Cada inodo describe UN archivo o UNA carpeta. Una carpeta es, en el
// fondo, un archivo cuyo contenido es una lista de índices de inodos
// hijos -- así toda la maquinaria de "escribir/leer bloques" se
// reutiliza sin duplicar código entre archivos y carpetas.
//
// VERSION 2 (roadmap, punto 4). Dos cambios respecto a v1, los dos
// compatibles con los discos ya formateados:
//
//  1. La tabla de inodos ya era "de tamaño fijo" (256, guardado en el
//     superbloque y respetado en tiempo de ejecucion), pero 256 fijo
//     es un techo bajo para un disco de gigas. Ahora se ESCALA al
//     formatear (un inodo por cada 32 sectores, entre 256 y 16384).
//     Un disco viejo sigue con sus 256 -- la tabla no puede crecer en
//     el sitio sin mover el bitmap y los datos. Y la reserva de inodos
//     y de bloques usa un mapa en memoria y una pista de "siguiente
//     libre", en vez de recorrer el disco entero cada vez.
//
//  2. Tamaño maximo de archivo: era 268288 bytes (12 bloques directos
//     + 4 indirectos simples), y lua.bin ocupaba ya el 91%. Se añade
//     un bloque DOBLEMENTE indirecto (128 punteros a bloques de 128
//     punteros): 8 MB mas, 8.656.896 bytes en total. El puntero vive
//     en 4 de los bytes 'reserved' del inodo -- pero en los discos
//     viejos esos bytes llevan basura de la pila (los inodos nunca se
//     inicializaban a cero), asi que NO se pueden interpretar sin
//     mas. Solucion: un campo 'features' en el superbloque (offset 36,
//     que en un disco viejo vale 0 porque el sector se escribia sobre
//     ceros) y una actualizacion en el sitio al montar: si falta el
//     bit, se ponen a cero los 'reserved' de todos los inodos, se
//     marca el bit, y el disco queda en v2 sin perder nada.
//
// No usamos memcpy/strcmp de la librería estándar (no tenemos libc
// enlazada); las pocas utilidades de cadenas que necesitamos están
// escritas a mano.

#include "nemofs.h"
#include "disk.h"
#include "uart.h"
#include "heap.h"

#define NEMOFS_MAGIC 0x4F4D454EUL // "NEMO" en little-endian, con la 'N' repetida a proposito

#define NEMOFS_FEAT_DINDIRECT 0x1u  // bit de 'features': los 'dindirect' de los inodos son fiables

#define MIN_INODES 256
#define MAX_INODES 16384            // 2MB de tabla; de sobra, y el mapa en memoria cabe (2KB)
#define SECTORS_PER_INODE 32        // al formatear: un inodo por cada 32 sectores (16KB)
#define INODES_PER_SECTOR (SECTOR_SIZE / 128)

#define DIRECT_BLOCKS 12
#define BLOCKS_PER_INDIRECT (SECTOR_SIZE / 4) // 128 punteros de 4 bytes por bloque

// Cuantos sectores seguidos se piden como mucho en una
// sola lectura. 64 son 32 KB: bastante para que el comando salga
// barato, poco para que una peticion no monopolice la tarjeta. Es el
// mismo tope que acepta el driver de la Pi 4 en un CMD18.
#define NEMOFS_TIRADA_MAX 64

// Sitio para juntar una lectura que no empieza en el borde de un
// sector. 16 sectores = 8 KB: suficiente para una fila entera de una
// imagen de 1920 de ancho (7680 bytes) en un solo comando.
#define NEMOFS_ACOPIO 16
#define SINGLE_INDIRECTS 4
#define BLOCKS_SINGLE (DIRECT_BLOCKS + SINGLE_INDIRECTS * BLOCKS_PER_INDIRECT)          // 524
#define BLOCKS_MAX    (BLOCKS_SINGLE + BLOCKS_PER_INDIRECT * BLOCKS_PER_INDIRECT)        // 16908
// v3 (roadmap): el mapa de bloques libres ya NO tiene un tope fijo --
// se reserva con kmalloc, del tamaño exacto que pide el disco real
// (256 bytes de bitmap por cada 1MB de datos), en vez de un array
// estatico de 64KB que capaba cualquier disco a 256MB. Si algun dia
// el disco es tan enorme que ni el heap del kernel (64MB, ver
// heap.c) pudiera con su bitmap, se recorta 'data_blocks' a lo que
// SI cabe -- el mismo espiritu que el limite de v1, solo que ahora
// el techo es "lo que de verdad cabe en RAM", no un numero fijo.
// El techo del mapa es un PRESUPUESTO DE RAM, no de disco. Antes eran 32 MB
// "de cordura", pensando en cuanto disco se podia describir; el problema es
// que el mapa vive en el heap del kernel, que son 64 MB en total, y con una
// tarjeta de 128 GB se llevaba 31,7 MB -- la mitad. En los 32 MB restantes
// tenian que caber las imagenes (hasta 24 MB), los lienzos de las ventanas y
// las tareas, asi que el sistema se quedaba sin memoria segun se usaba: los
// programas se cargaban y morian al arrancar, y con cada reinicio salia
// distinto. En QEMU no se veia porque el disco es pequeño.
//
// 4 MB de mapa describen 16 GB de datos, de sobra para este sistema (la
// instalacion entera son unos pocos MB), y dejan el 94% del heap para lo
// demas. En una tarjeta mas grande, NemoFS usa 16 GB y el resto se queda
// sin usar: es mejor trato que quedarse sin RAM.
#define MAX_BITMAP_BYTES (4 * 1024 * 1024)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t total_sectors;
    uint32_t inode_table_start;
    uint32_t inode_count;
    uint32_t bitmap_start;
    uint32_t bitmap_sectors;
    uint32_t data_start;
    uint32_t data_blocks;
    uint32_t root_inode;
    uint32_t features;               // NEMOFS_FEAT_*; 0 en discos formateados por v1
} nemofs_superblock_t;

// 128 bytes exactos: 4 inodos por sector. Los campos hasta 'indirect4'
// son los de v1, en el mismo orden -- un disco viejo se lee igual.
typedef struct __attribute__((packed)) {
    uint8_t type;                    // NEMOFS_TYPE_*
    char name[NEMOFS_MAX_NAME + 1];  // 32 bytes
    uint32_t parent;                 // índice de inodo del padre
    uint32_t size;                   // bytes usados (archivos) / no usado igual en dirs
    uint32_t num_children;           // solo relevante para carpetas
    uint32_t direct[DIRECT_BLOCKS];  // sectores de datos, 0 = sin asignar
    uint32_t indirect;               // bloques 12..139: sector con 128 punteros, 0 = sin asignar
    uint32_t indirect2;              // bloques 140..267
    uint32_t indirect3;              // bloques 268..395
    uint32_t indirect4;              // bloques 396..523
    uint32_t dindirect;              // bloques 524..16907: sector con 128 punteros a sectores de 128 punteros (v2)
    uint8_t reserved[15];
} nemofs_inode_t;
_Static_assert(sizeof(nemofs_inode_t) == 128, "el inodo tiene que medir 128 bytes");
_Static_assert(sizeof(nemofs_superblock_t) <= SECTOR_SIZE, "el superbloque tiene que caber en un sector");

static nemofs_superblock_t sb;
// Reservados con kmalloc en tiempo de montaje/formateo, del tamaño
// EXACTO que pide el disco real -- ver alloc_bitmap_arrays().
static uint8_t *bitmap_cache = 0;
static uint8_t *bitmap_dirty = 0;   // un byte por SECTOR del bitmap, no por bloque de datos
static uint32_t bitmap_cache_bytes = 0;
static uint32_t bitmap_dirty_bytes = 0;
static uint32_t next_free_block_hint = 0;

// Reserva (o vuelve a reservar, liberando lo anterior) los dos
// arrays del bitmap para 'sectors' sectores de bitmap -- el tamaño
// que ya viene decidido en el superbloque (al montar) o que se
// acaba de calcular (al formatear). false si el kernel no tiene
// tanta memoria libre -- el llamador decide si probar con menos.
static bool alloc_bitmap_arrays(uint32_t sectors) {
    uint32_t cache_bytes = sectors * SECTOR_SIZE;
    uint8_t *new_cache = (uint8_t *)kmalloc(cache_bytes);
    if (!new_cache) return false;
    uint8_t *new_dirty = (uint8_t *)kmalloc(sectors);
    if (!new_dirty) { kfree(new_cache); return false; }

    if (bitmap_cache) kfree(bitmap_cache);
    if (bitmap_dirty) kfree(bitmap_dirty);
    bitmap_cache = new_cache;
    bitmap_dirty = new_dirty;
    bitmap_cache_bytes = cache_bytes;
    bitmap_dirty_bytes = sectors;
    return true;
}

static uint8_t inode_map[MAX_INODES / 8];   // 1 bit por inodo, 1 = ocupado (solo en memoria)
static uint32_t next_free_inode_hint = 0;

// ---- utilidades de cadenas, sin depender de libc ----

static bool str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static char to_upper_ascii(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}
static bool str_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (to_upper_ascii(*a) != to_upper_ascii(*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static void str_copy_n(char *dst, const char *src, uint32_t max_len) {
    uint32_t i = 0;
    while (src[i] != '\0' && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void zero_bytes(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

static void uart_put_dec(uint32_t v) {
    char d[12]; int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0 && n < 11) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) uart_putc(d[--n]);
}

static bool aviso_disco_rechaza = false;

// Escribir un sector, DICIENDOLO si la tarjeta lo rechaza.
//
// Una tarjeta gastada hace fallar todas las escrituras a partir de cierto
// punto, y sin este aviso el sistema lo cuenta como ochenta lineas de
// "fallo creando X" -- exactamente el mismo sintoma que quedarse sin
// sitio. Y engaña: el patron es MUY ORDENADO (una escritura falla y a
// partir de ahi falla todo), razonando que una tarjeta rota daria errores
// erraticos. Es al reves: una tarjeta que deja de aceptar escrituras las
// rechaza TODAS, y eso es lo mas ordenado que hay.
//
// Ahora se distinguen solas: sin sitio, sale "SIN BLOQUES LIBRES" con sus
// numeros; tarjeta que no acepta, sale esta. Y si no sale ninguna, el
// problema es otro.
static bool escribir_sector(uint32_t sector, const void *buf) {
    if (disk_write_sector(sector, buf)) return true;
    if (!aviso_disco_rechaza) {
        aviso_disco_rechaza = true;
        uart_puts("nemofs: LA TARJETA RECHAZA LA ESCRITURA en el sector ");
        uart_put_dec(sector);
        uart_puts(".\n        No es falta de sitio: el disco dice que no puede escribir.\n"
                  "        Suele ser una tarjeta gastada o con el seguro de escritura\n"
                  "        puesto. Pruebala en otra tarjeta antes de buscar en el codigo.\n");
    }
    return false;
}

// ---- acceso a inodos (lectura/escritura sector a sector) ----

static bool read_inode(uint32_t idx, nemofs_inode_t *out) {
    if (idx >= sb.inode_count) return false;
    uint32_t sector = sb.inode_table_start + (idx / INODES_PER_SECTOR);
    uint32_t offset = idx % INODES_PER_SECTOR;
    static uint8_t buf[SECTOR_SIZE];
    if (!disk_read_sector(sector, buf)) return false;
    nemofs_inode_t *inodes = (nemofs_inode_t *)buf;
    *out = inodes[offset];
    return true;
}

static bool write_inode(uint32_t idx, const nemofs_inode_t *in) {
    if (idx >= sb.inode_count) return false;
    uint32_t sector = sb.inode_table_start + (idx / INODES_PER_SECTOR);
    uint32_t offset = idx % INODES_PER_SECTOR;
    static uint8_t buf[SECTOR_SIZE];
    if (!disk_read_sector(sector, buf)) return false; // leemos primero para no pisar los otros 3 inodos del sector
    nemofs_inode_t *inodes = (nemofs_inode_t *)buf;
    inodes[offset] = *in;
    return escribir_sector(sector, buf);
}

int32_t nemofs_type_by_inode(uint32_t idx) {
    nemofs_inode_t inode;
    if (!read_inode(idx, &inode)) return -1;
    if (inode.type == NEMOFS_TYPE_FREE) return -1;
    return (int32_t)inode.type;
}

// ---- CUANTOS BLOQUES ESTAN OCUPADOS ----
//
// FALLO REAL DE RENDIMIENTO, medido en la placa. Esto contaba los bloques
// ocupados **bit a bit, recorriendo el bitmap entero, en cada llamada**, y
// con el candado grande cogido. En QEMU la particion era pequeña y no se
// notaba; en la tarjeta real el mapa llega a su techo de MAX_BITMAP_BYTES =
// 4 MB, o sea 33,5 MILLONES de bits que recorrer. Medido: unos 79 ms por
// llamada, y mientras el nucleo 0 no podia componer la pantalla.
//
// Se veia como "el raton va a tirones cuando el monitor del sistema esta
// abierto". El monitor llama a DiskUsedBlocks y a DiskTotalBlocks, que son la
// MISMA syscall 250: dos recorridos completos por repintado. Las medidas: con
// el monitor abierto la composicion pasaba de 6 ms a 46 ms de media (max 86),
// mientras fb_present seguia clavado en 3,1 ms -- o sea que el tiempo no se
// iba en dibujar ni en presentar, se iba esperando este bucle.
//
// Ahora la cuenta se lleva al dia: se hace UNA vez al montar y se sube o baja
// en alloc_block/free_block, que son los dos unicos sitios de todo el fichero
// que tocan el bitmap. Consultarla es leer una variable.
static uint32_t bloques_usados = 0;

// Solo para el recuento inicial del montaje. Por bytes, con tabla: no hace
// falta ser listo aqui, corre una vez.
static void recontar_bloques_usados(void) {
    static const uint8_t bits_por_byte[16] = { 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4 };
    uint32_t used = 0;
    uint32_t bytes_enteros = sb.data_blocks / 8;
    for (uint32_t b = 0; b < bytes_enteros; b++) {
        uint8_t v = bitmap_cache[b];
        used += bits_por_byte[v & 0xF];
        used += bits_por_byte[v >> 4];
    }
    // Los bits sueltos del final: el bitmap tiene bytes enteros, pero
    // data_blocks puede no ser multiplo de 8, y lo que sobra no cuenta.
    for (uint32_t i = bytes_enteros * 8; i < sb.data_blocks; i++) {
        if (bitmap_cache[i / 8] & (1 << (i % 8))) used++;
    }
    bloques_usados = used;
}

void nemofs_disk_usage(uint32_t *total_blocks, uint32_t *used_blocks) {
    *total_blocks = sb.data_blocks;
    *used_blocks = bloques_usados;
}

// ---- mapa de inodos en memoria ----

static inline bool inode_used(uint32_t i) { return (inode_map[i / 8] >> (i % 8)) & 1; }
static inline void inode_mark(uint32_t i, bool used) {
    if (used) inode_map[i / 8] |= (uint8_t)(1 << (i % 8));
    else      inode_map[i / 8] &= (uint8_t)~(1 << (i % 8));
}

// Recorre la tabla UNA vez al montar, sector a sector (4 inodos por
// lectura), y anota cuales estan ocupados. Si 'limpiar_reserved', pone
// a cero los bytes 'reserved' y 'dindirect' de los inodos ocupados y
// reescribe el sector: la actualizacion v1 -> v2.
static bool build_inode_map(bool limpiar_reserved) {
    zero_bytes(inode_map, sizeof(inode_map));
    static uint8_t buf[SECTOR_SIZE];
    uint32_t sectors = (sb.inode_count + INODES_PER_SECTOR - 1) / INODES_PER_SECTOR;
    for (uint32_t s = 0; s < sectors; s++) {
        if (!disk_read_sector(sb.inode_table_start + s, buf)) return false;
        nemofs_inode_t *inodes = (nemofs_inode_t *)buf;
        bool dirty = false;
        for (uint32_t k = 0; k < INODES_PER_SECTOR; k++) {
            uint32_t idx = s * INODES_PER_SECTOR + k;
            if (idx >= sb.inode_count) break;
            if (inodes[k].type != NEMOFS_TYPE_FREE) {
                inode_mark(idx, true);
                if (limpiar_reserved) {
                    inodes[k].dindirect = 0;
                    zero_bytes(inodes[k].reserved, sizeof(inodes[k].reserved));
                    dirty = true;
                }
            }
        }
        if (dirty && !escribir_sector(sb.inode_table_start + s, buf)) return false;
    }
    next_free_inode_hint = 0;
    return true;
}

// Cuando el disco se llena hay que DECIR DE QUE, y una sola vez.
//
// Sin esto, una tarjeta llena suelta ochenta lineas de "fallo creando X"
// seguidas, todas iguales, sin una sola pista de si faltaban inodos,
// bloques, o si la tarjeta no aceptaba la escritura: tres causas muy
// distintas con el mismo mensaje. Cada una lo dice con sus numeros la
// primera vez que pasa.
static bool aviso_sin_inodos = false;
static bool aviso_sin_bloques = false;

static int32_t alloc_inode(void) {
    for (uint32_t n = 0; n < sb.inode_count; n++) {
        uint32_t i = (next_free_inode_hint + n) % sb.inode_count;
        if (!inode_used(i)) {
            inode_mark(i, true);
            next_free_inode_hint = i + 1;
            return (int32_t)i;
        }
    }
    if (!aviso_sin_inodos) {
        aviso_sin_inodos = true;
        uart_puts("nemofs: SIN INODOS LIBRES -- la tabla tiene ");
        uart_put_dec(sb.inode_count);
        uart_puts(" y estan todos ocupados. A partir de aqui, crear cualquier\n"
                  "        archivo o carpeta fallara. El numero de inodos se fija al\n"
                  "        formatear, en proporcion al tamaño de la particion.\n");
    }
    return -1; // tabla de inodos llena
}

// ---- bitmap de bloques de datos ----

static void bitmap_flush(void) {
    for (uint32_t s = 0; s < sb.bitmap_sectors; s++) {
        if (bitmap_dirty[s]) {
            escribir_sector(sb.bitmap_start + s, &bitmap_cache[s * SECTOR_SIZE]);
            bitmap_dirty[s] = 0;
        }
    }
}

// ---------------------------------------------------------------------
// Crecer sin reformatear
//
// Solo se puede si el mapa de bloques, que se reservo al formatear, tiene
// sitio para los bloques nuevos. Si lo tiene, crecer es cambiar dos numeros
// del superbloque: ni se mueve un byte de datos ni se toca el mapa (los bits
// de la zona nueva ya estan a cero desde el formateo, es decir, libres).
// Crece HASTA DONDE PUEDA, no todo o nada. Antes, pedir mas de lo que el mapa
// describe hacia que no creciera en absoluto: en una tarjeta de 128 GB, el
// particionador pedia la tarjeta entera, no cabia, y se quedaba sin hacer
// nada -- pareciendo que el mapa "se habia quedado corto" cuando lo que pasa
// es que tiene un techo a proposito (ver MAX_BITMAP_BYTES). Ahora se recorta
// la peticion a lo que el mapa describe y se crece eso.
bool nemofs_puede_crecer(uint32_t nuevos_sectores, uint32_t *bloques_nuevos) {
    if (nuevos_sectores <= sb.total_sectors) return false;
    if (nuevos_sectores <= sb.data_start) return false;
    uint64_t caben = (uint64_t)sb.bitmap_sectors * SECTOR_SIZE * 8;   // un bit por bloque
    uint64_t nuevos_bloques = (uint64_t)nuevos_sectores - sb.data_start;
    if (nuevos_bloques > caben) nuevos_bloques = caben;               // hasta donde llegue el mapa
    if (nuevos_bloques <= sb.data_blocks) return false;               // no se ganaria nada
    if (bloques_nuevos) *bloques_nuevos = (uint32_t)nuevos_bloques;
    return true;
}

// Devuelve los bloques que tiene despues de crecer, o 0 si no se pudo.
uint32_t nemofs_crecer(uint32_t nuevos_sectores) {
    uint32_t nuevos_bloques = 0;
    if (!nemofs_puede_crecer(nuevos_sectores, &nuevos_bloques)) return 0;
    uint32_t antes_sect = sb.total_sectors, antes_bloq = sb.data_blocks;
    // El total se calcula DESDE los bloques que de verdad caben, no desde lo
    // que se pidio: si se guardara lo pedido, el superbloque diria que hay
    // mas disco del que el mapa sabe describir.
    sb.total_sectors = sb.data_start + nuevos_bloques;
    sb.data_blocks = nuevos_bloques;

    static uint8_t sector_buf[SECTOR_SIZE];
    zero_bytes(sector_buf, SECTOR_SIZE);
    *(nemofs_superblock_t *)sector_buf = sb;
    if (!escribir_sector(0, sector_buf)) {      // si falla, se deja como estaba
        sb.total_sectors = antes_sect;
        sb.data_blocks = antes_bloq;
        uart_puts("nemofs: no se pudo escribir el superbloque al crecer\n");
        return 0;
    }
    bitmap_flush();
    uint32_t antes = antes_bloq;
    uart_puts("nemofs: crecido de ");
    uart_put_dec(antes);
    uart_puts(" a ");
    uart_put_dec(sb.data_blocks);
    uart_puts(" bloques\n");
    return sb.data_blocks;
}


static int32_t alloc_block(void) {
    for (uint32_t n = 0; n < sb.data_blocks; n++) {
        uint32_t i = (next_free_block_hint + n) % sb.data_blocks;
        uint32_t byte = i / 8;
        uint8_t bit = (uint8_t)(i % 8);
        if (!(bitmap_cache[byte] & (1 << bit))) {
            bitmap_cache[byte] |= (uint8_t)(1 << bit);
            bitmap_dirty[byte / SECTOR_SIZE] = 1;
            bloques_usados++;          // ver nemofs_disk_usage
            next_free_block_hint = i + 1;
            return (int32_t)(sb.data_start + i);
        }
    }
    if (!aviso_sin_bloques) {
        aviso_sin_bloques = true;
        uart_puts("nemofs: SIN BLOQUES LIBRES -- ");
        uart_put_dec(bloques_usados);
        uart_puts(" de ");
        uart_put_dec(sb.data_blocks);
        uart_puts(" bloques de 512 bytes ocupados (");
        uart_put_dec(sb.data_blocks / 2048);
        uart_puts(" MB de particion). A partir de aqui, escribir fallara.\n");
    }
    return -1; // disco lleno
}

static void free_block(uint32_t sector) {
    if (sector < sb.data_start) return;
    uint32_t i = sector - sb.data_start;
    if (i >= sb.data_blocks) return;
    uint32_t byte = i / 8;
    uint8_t bit = (uint8_t)(i % 8);
    // Se comprueba que estaba ocupado ANTES de restar: liberar dos veces el
    // mismo bloque es legal aqui (no rompe nada) y descontaria de menos.
    if (bitmap_cache[byte] & (1 << bit)) {
        bitmap_cache[byte] &= (uint8_t)~(1 << bit);
        bitmap_dirty[byte / SECTOR_SIZE] = 1;
        if (bloques_usados > 0) bloques_usados--;   // ver nemofs_disk_usage
    }
    if (i < next_free_block_hint) next_free_block_hint = i;
}

// ---- traduccion "bloque N del archivo" -> sector de disco ----
//
// UNA sola funcion para lectura, escritura y borrado, en vez de la
// cadena de if/else repetida tres veces de v1. Con 'allocate', crea
// por el camino los bloques de punteros y el bloque de datos que
// falten. Cachea el ultimo sector de punteros cargado (y, para la
// doble indireccion, el superior) para no releerlo por cada bloque.

typedef struct {
    uint32_t sector;        // 0 = nada cargado
    bool dirty;
    uint8_t buf[SECTOR_SIZE];
} ptr_block_t;

typedef struct {
    nemofs_inode_t *inode;
    bool allocate;
    ptr_block_t top;        // bloque de punteros de primer nivel (indirectN o dindirect)
    ptr_block_t inner;      // bloque de segundo nivel (solo doble indireccion)
} block_mapper_t;

static bool ptr_block_flush(ptr_block_t *p) {
    if (p->sector != 0 && p->dirty) {
        if (!escribir_sector(p->sector, p->buf)) return false;
        p->dirty = false;
    }
    return true;
}

// Deja cargado en 'p' el sector 'sector' (escribiendo antes el que
// hubiera, si cambio). Si sector==0 y allocate, reserva uno nuevo a
// cero y lo devuelve en *sector_ref.
static bool ptr_block_load(ptr_block_t *p, uint32_t *sector_ref, bool allocate) {
    if (*sector_ref == 0) {
        if (!allocate) return false;
        int32_t nb = alloc_block();
        if (nb < 0) return false;
        if (!ptr_block_flush(p)) return false;
        *sector_ref = (uint32_t)nb;
        p->sector = (uint32_t)nb;
        zero_bytes(p->buf, SECTOR_SIZE);
        p->dirty = true;                 // el bloque nuevo hay que escribirlo aunque no cambie mas
        return true;
    }
    if (p->sector == *sector_ref) return true;
    if (!ptr_block_flush(p)) return false;
    if (!disk_read_sector(*sector_ref, p->buf)) return false;
    p->sector = *sector_ref;
    p->dirty = false;
    return true;
}

static void mapper_init(block_mapper_t *m, nemofs_inode_t *inode, bool allocate) {
    m->inode = inode; m->allocate = allocate;
    m->top.sector = 0; m->top.dirty = false;
    m->inner.sector = 0; m->inner.dirty = false;
}

static bool mapper_finish(block_mapper_t *m) {
    bool ok = ptr_block_flush(&m->inner);
    if (!ptr_block_flush(&m->top)) ok = false;
    return ok;
}

// *out = sector del bloque 'b' (0 si no existe y no se pide reservar).
// Devuelve false solo ante un error de disco o disco lleno.
static bool map_block(block_mapper_t *m, uint32_t b, uint32_t *out) {
    nemofs_inode_t *in = m->inode;
    uint32_t *slot;

    if (b < DIRECT_BLOCKS) {
        slot = &in->direct[b];
    } else if (b < BLOCKS_SINGLE) {
        uint32_t k = (b - DIRECT_BLOCKS) / BLOCKS_PER_INDIRECT;         // que indirecto simple, 0..3
        uint32_t off = (b - DIRECT_BLOCKS) % BLOCKS_PER_INDIRECT;
        uint32_t *ref = (k == 0) ? &in->indirect : (k == 1) ? &in->indirect2 : (k == 2) ? &in->indirect3 : &in->indirect4;
        if (!ptr_block_load(&m->top, ref, m->allocate)) { *out = 0; return !m->allocate; }
        slot = &((uint32_t *)m->top.buf)[off];
    } else if (b < BLOCKS_MAX) {
        uint32_t rel = b - BLOCKS_SINGLE;
        uint32_t k = rel / BLOCKS_PER_INDIRECT;                          // entrada del bloque superior, 0..127
        uint32_t off = rel % BLOCKS_PER_INDIRECT;
        if (!ptr_block_load(&m->top, &in->dindirect, m->allocate)) { *out = 0; return !m->allocate; }
        uint32_t *inner_ref = &((uint32_t *)m->top.buf)[k];
        uint32_t before = *inner_ref;
        if (!ptr_block_load(&m->inner, inner_ref, m->allocate)) { *out = 0; return !m->allocate; }
        if (*inner_ref != before) m->top.dirty = true;                    // se reservo un bloque interior nuevo
        slot = &((uint32_t *)m->inner.buf)[off];
    } else {
        *out = 0;
        return false; // mas alla del tamaño maximo
    }

    if (*slot == 0 && m->allocate) {
        int32_t nb = alloc_block();
        if (nb < 0) { *out = 0; return false; }
        *slot = (uint32_t)nb;
        if (b >= BLOCKS_SINGLE) m->inner.dirty = true;
        else if (b >= DIRECT_BLOCKS) m->top.dirty = true;
    }
    *out = *slot;
    return true;
}

// Libera TODOS los bloques de datos y de punteros de un inodo.
static void free_all_blocks(nemofs_inode_t *in) {
    static uint8_t top[SECTOR_SIZE], inner[SECTOR_SIZE];
    for (int i = 0; i < DIRECT_BLOCKS; i++) { if (in->direct[i]) free_block(in->direct[i]); in->direct[i] = 0; }
    uint32_t *singles[SINGLE_INDIRECTS] = { &in->indirect, &in->indirect2, &in->indirect3, &in->indirect4 };
    for (int k = 0; k < SINGLE_INDIRECTS; k++) {
        if (*singles[k] == 0) continue;
        if (disk_read_sector(*singles[k], top)) {
            uint32_t *p = (uint32_t *)top;
            for (uint32_t i = 0; i < BLOCKS_PER_INDIRECT; i++) if (p[i]) free_block(p[i]);
        }
        free_block(*singles[k]);
        *singles[k] = 0;
    }
    if (in->dindirect != 0) {
        if (disk_read_sector(in->dindirect, top)) {
            uint32_t *pt = (uint32_t *)top;
            for (uint32_t i = 0; i < BLOCKS_PER_INDIRECT; i++) {
                if (pt[i] == 0) continue;
                if (disk_read_sector(pt[i], inner)) {
                    uint32_t *pi = (uint32_t *)inner;
                    for (uint32_t j = 0; j < BLOCKS_PER_INDIRECT; j++) if (pi[j]) free_block(pi[j]);
                }
                free_block(pt[i]);
            }
        }
        free_block(in->dindirect);
        in->dindirect = 0;
    }
}

// ---- formateo y montaje ----

static bool nemofs_format(void) {
    uint64_t total = disk_capacity_sectors();
    if (total == 0) {
        uart_puts("nemofs: no se pudo leer la capacidad del disco\n");
        return false;
    }
    if (total > 0xFFFFFFFFULL) total = 0xFFFFFFFFULL;

    // Inodos proporcionales al disco, redondeados a sector completo.
    uint32_t inode_count = (uint32_t)(total / SECTORS_PER_INODE);
    if (inode_count < MIN_INODES) inode_count = MIN_INODES;
    if (inode_count > MAX_INODES) inode_count = MAX_INODES;
    inode_count = (inode_count + INODES_PER_SECTOR - 1) / INODES_PER_SECTOR * INODES_PER_SECTOR;

    uint32_t inode_table_start = 1;
    uint32_t inode_table_sectors = inode_count / INODES_PER_SECTOR;
    uint32_t bitmap_start = inode_table_start + inode_table_sectors;

    // El mapa de bloques se reserva para la TARJETA ENTERA, no
    // solo para esta particion: asi, si luego se estira la particion hasta el
    // final del disco, el mapa ya tiene sitio y no hay que mover los datos
    // (que es imposible sin reformatear, porque el mapa va DELANTE de ellos).
    // Cuesta 256 bytes por cada MB de tarjeta: 32 MB para una de 128 GB.
    uint64_t fisico = disk_capacidad_fisica();
    if (fisico < total) fisico = total;
    if (fisico > 0xFFFFFFFFULL) fisico = 0xFFFFFFFFULL;
    uint32_t remaining = (uint32_t)fisico - bitmap_start;
    uint32_t bitmap_bytes = (remaining + 7) / 8;
    if (bitmap_bytes > MAX_BITMAP_BYTES) {
        bitmap_bytes = MAX_BITMAP_BYTES;   // presupuesto de RAM: ver arriba
    }
    uint32_t bitmap_sectors = (bitmap_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;

    // Reservar el bitmap de verdad, del tamaño que se acaba de
    // calcular. Si el heap del kernel no diera para tanto (disco
    // enorme, heap pequeño), se reduce a la mitad y se reintenta --
    // el disco queda mas pequeño de lo posible, no sin formatear.
    while (!alloc_bitmap_arrays(bitmap_sectors)) {
        if (bitmap_sectors <= 1) {
            uart_puts("nemofs: no hay memoria del kernel para el bitmap, ni el minimo\n");
            return false;
        }
        bitmap_sectors /= 2;
        bitmap_bytes = bitmap_sectors * SECTOR_SIZE;
        uart_puts("nemofs: heap insuficiente para el bitmap completo, reduciendo...\n");
    }

    uint32_t data_start = bitmap_start + bitmap_sectors;
    uint32_t data_blocks = (uint32_t)total - data_start;
    if (data_blocks > bitmap_bytes * 8) data_blocks = bitmap_bytes * 8; // solo lo que el bitmap puede describir

    sb.magic = NEMOFS_MAGIC;
    sb.total_sectors = (uint32_t)total;
    sb.inode_table_start = inode_table_start;
    sb.inode_count = inode_count;
    sb.bitmap_start = bitmap_start;
    sb.bitmap_sectors = bitmap_sectors;
    sb.data_start = data_start;
    sb.data_blocks = data_blocks;
    sb.root_inode = NEMOFS_ROOT_INODE;
    sb.features = NEMOFS_FEAT_DINDIRECT;

    static uint8_t sector_buf[SECTOR_SIZE];
    zero_bytes(sector_buf, SECTOR_SIZE);
    *(nemofs_superblock_t *)sector_buf = sb;
    if (!escribir_sector(0, sector_buf)) return false;

    zero_bytes(sector_buf, SECTOR_SIZE);
    for (uint32_t s = 0; s < inode_table_sectors; s++) {
        if (!escribir_sector(inode_table_start + s, sector_buf)) return false;
    }
    for (uint32_t s = 0; s < bitmap_sectors; s++) {
        if (!escribir_sector(bitmap_start + s, sector_buf)) return false;
    }
    zero_bytes(bitmap_cache, bitmap_cache_bytes);
    zero_bytes(bitmap_dirty, bitmap_dirty_bytes);
    zero_bytes(inode_map, sizeof(inode_map));
    next_free_block_hint = 0;
    next_free_inode_hint = 0;

    nemofs_inode_t root;
    zero_bytes(&root, sizeof(root));
    root.type = NEMOFS_TYPE_DIR;
    str_copy_n(root.name, "/", sizeof(root.name));
    root.parent = NEMOFS_ROOT_INODE;
    if (!write_inode(NEMOFS_ROOT_INODE, &root)) return false;
    inode_mark(NEMOFS_ROOT_INODE, true);

    uart_puts("nemofs: formateado (");
    uart_put_dec(inode_count);
    uart_puts(" inodos, ");
    uart_put_dec(data_blocks);
    uart_puts(" bloques de datos).\n");
    return true;
}

bool nemofs_mount(void) {
    static uint8_t sector_buf[SECTOR_SIZE];
    if (!disk_read_sector(0, sector_buf)) {
        uart_puts("nemofs: no se pudo leer el superbloque\n");
        return false;
    }

    nemofs_superblock_t *candidate = (nemofs_superblock_t *)sector_buf;
    if (candidate->magic != NEMOFS_MAGIC) {
        uart_puts("nemofs: no se encontro un sistema de archivos valido, formateando...\n");
        return nemofs_format();
    }

    sb = *candidate;
    if (sb.inode_count > MAX_INODES) {
        uart_puts("nemofs: el disco declara mas inodos de los que este kernel soporta\n");
        return false;
    }

    // El tamaño del bitmap de ESTE disco ya quedo decidido para
    // siempre quien lo formateo (v1: 64KB fijos = 256MB de datos; v3
    // en adelante: proporcional al disco real, ver nemofs_format) --
    // aqui solo se reserva la memoria para encajar exactamente eso,
    // no se recalcula nada.
    // Un disco formateado por una version anterior puede traer un bitmap
    // enorme (una tarjeta de 128 GB pedia 32 MB, la mitad del heap). Se
    // monta igual -- los datos son validos y se leen bien -- pero hay que
    // DECIRLO: si no, el sistema se queda sin memoria mucho mas tarde, con
    // un sintoma que no se parece en nada a su causa.
    if ((uint64_t)sb.bitmap_sectors * SECTOR_SIZE > MAX_BITMAP_BYTES) {
        uart_puts("nemofs: AVISO -- este disco se formateo con un mapa de bloques\n"
                  "        mucho mayor de lo que ahora se considera razonable. Se lleva\n"
                  "        una buena parte del heap del kernel y el sistema puede\n"
                  "        quedarse sin memoria al usarlo. Reescribe la tarjeta con una\n"
                  "        imagen nueva (make imagen) para formatearla de nuevo.\n");
    }
    if (!alloc_bitmap_arrays(sb.bitmap_sectors)) {
        uart_puts("nemofs: no hay memoria del kernel para el bitmap de este disco\n");
        return false;
    }
    for (uint32_t s = 0; s < sb.bitmap_sectors; s++) {
        if (!disk_read_sector(sb.bitmap_start + s, &bitmap_cache[s * SECTOR_SIZE])) {
            uart_puts("nemofs: fallo leyendo el bitmap\n");
            return false;
        }
    }
    zero_bytes(bitmap_dirty, bitmap_dirty_bytes);
    next_free_block_hint = 0;
    // El unico recorrido completo del bitmap en toda la vida del sistema.
    // A partir de aqui la cuenta se lleva al dia. Ver nemofs_disk_usage.
    recontar_bloques_usados();

    bool actualizar = !(sb.features & NEMOFS_FEAT_DINDIRECT);
    if (!build_inode_map(actualizar)) {
        uart_puts("nemofs: fallo leyendo la tabla de inodos\n");
        return false;
    }
    if (actualizar) {
        sb.features |= NEMOFS_FEAT_DINDIRECT;
        zero_bytes(sector_buf, SECTOR_SIZE);
        *(nemofs_superblock_t *)sector_buf = sb;
        if (!escribir_sector(0, sector_buf)) {
            uart_puts("nemofs: fallo escribiendo el superbloque actualizado\n");
            return false;
        }
        uart_puts("nemofs: disco actualizado a v2 (doble indireccion, archivos de hasta 8MB).\n");
    }

    // Las tres cifras que de verdad hacen falta para saber si algo cabe, y
    // Se dicen las tres cifras: inodos, bloques de datos y cuantos estan
    // ocupados ya. Con solo el numero de inodos, un disco que se llena en el
    // primer arranque no se distingue de uno que no.
    uart_puts("nemofs: montado correctamente (");
    uart_put_dec(sb.inode_count);
    uart_puts(" inodos, ");
    uart_put_dec(sb.data_blocks);
    uart_puts(" bloques = ");
    uart_put_dec(sb.data_blocks / 2048);
    uart_puts(" MB, ");
    uart_put_dec(bloques_usados / 2048);
    uart_puts(" MB en uso).\n");
    return true;
}

// ---- operaciones de archivos y carpetas ----

int32_t nemofs_find_child(uint32_t parent, const char *name) {
    nemofs_inode_t parent_inode;
    if (!read_inode(parent, &parent_inode)) return -1;
    if (parent_inode.type != NEMOFS_TYPE_DIR) return -1;

    static uint8_t block_buf[SECTOR_SIZE];
    uint32_t entries_per_block = SECTOR_SIZE / sizeof(uint32_t);
    int32_t last_block_loaded = -1;

    for (uint32_t i = 0; i < parent_inode.num_children; i++) {
        uint32_t block_index = i / entries_per_block;
        uint32_t offset = i % entries_per_block;

        if (block_index >= DIRECT_BLOCKS) break;
        uint32_t sector = parent_inode.direct[block_index];
        if (sector == 0) break;

        if ((int32_t)block_index != last_block_loaded) {
            if (!disk_read_sector(sector, block_buf)) return -1;
            last_block_loaded = (int32_t)block_index;
        }

        uint32_t child_idx = ((uint32_t *)block_buf)[offset];
        nemofs_inode_t child;
        if (!read_inode(child_idx, &child)) continue;
        if (str_eq_ci(child.name, name)) {
            return (int32_t)child_idx;
        }
    }

    return -1;
}

int32_t nemofs_create(uint32_t parent, const char *name, uint8_t type) {
    if (nemofs_find_child(parent, name) != -1) {
        return -1; // ya existe un archivo/carpeta con ese nombre
    }

    nemofs_inode_t parent_inode;
    if (!read_inode(parent, &parent_inode)) return -1;
    if (parent_inode.type != NEMOFS_TYPE_DIR) return -1;

    // Añadirlo como hijo del padre: comprobar hueco ANTES de gastar
    // un inodo.
    uint32_t entries_per_block = SECTOR_SIZE / sizeof(uint32_t);
    uint32_t block_index = parent_inode.num_children / entries_per_block;
    uint32_t offset = parent_inode.num_children % entries_per_block;
    if (block_index >= DIRECT_BLOCKS) {
        return -1; // carpeta llena (12 * 128 = 1536 entradas)
    }

    int32_t new_idx = alloc_inode();
    if (new_idx < 0) return -1;

    nemofs_inode_t new_inode;
    zero_bytes(&new_inode, sizeof(new_inode));   // v2: todo a cero, 'reserved' incluido
    new_inode.type = type;
    str_copy_n(new_inode.name, name, sizeof(new_inode.name));
    new_inode.parent = parent;
    if (!write_inode((uint32_t)new_idx, &new_inode)) { inode_mark((uint32_t)new_idx, false); return -1; }

    static uint8_t block_buf[SECTOR_SIZE];
    if (parent_inode.direct[block_index] == 0) {
        int32_t new_block = alloc_block();
        if (new_block < 0) { inode_mark((uint32_t)new_idx, false); return -1; }
        parent_inode.direct[block_index] = (uint32_t)new_block;
        zero_bytes(block_buf, SECTOR_SIZE);
    } else {
        if (!disk_read_sector(parent_inode.direct[block_index], block_buf)) return -1;
    }

    ((uint32_t *)block_buf)[offset] = (uint32_t)new_idx;
    if (!escribir_sector(parent_inode.direct[block_index], block_buf)) return -1;

    parent_inode.num_children++;
    if (!write_inode(parent, &parent_inode)) return -1;
    bitmap_flush();

    return new_idx;
}

static bool get_child_slot(const nemofs_inode_t *parent_inode, uint32_t pos, uint32_t *out_val) {
    uint32_t entries_per_block = SECTOR_SIZE / sizeof(uint32_t);
    uint32_t block_index = pos / entries_per_block;
    uint32_t offset = pos % entries_per_block;
    if (block_index >= DIRECT_BLOCKS || parent_inode->direct[block_index] == 0) return false;

    static uint8_t buf[SECTOR_SIZE];
    if (!disk_read_sector(parent_inode->direct[block_index], buf)) return false;
    *out_val = ((uint32_t *)buf)[offset];
    return true;
}

static bool set_child_slot(const nemofs_inode_t *parent_inode, uint32_t pos, uint32_t val) {
    uint32_t entries_per_block = SECTOR_SIZE / sizeof(uint32_t);
    uint32_t block_index = pos / entries_per_block;
    uint32_t offset = pos % entries_per_block;
    if (block_index >= DIRECT_BLOCKS || parent_inode->direct[block_index] == 0) return false;

    static uint8_t buf[SECTOR_SIZE];
    if (!disk_read_sector(parent_inode->direct[block_index], buf)) return false;
    ((uint32_t *)buf)[offset] = val;
    return escribir_sector(parent_inode->direct[block_index], buf);
}

bool nemofs_rename(uint32_t parent, const char *old_name, const char *new_name) {
    if (new_name[0] == '\0') return false;
    int32_t target = nemofs_find_child(parent, old_name);
    if (target < 0) return false;
    if (str_eq(old_name, new_name)) return true; // nada que hacer
    if (nemofs_find_child(parent, new_name) >= 0) return false; // ya existe algo con ese nombre

    nemofs_inode_t target_inode;
    if (!read_inode((uint32_t)target, &target_inode)) return false;
    str_copy_n(target_inode.name, new_name, sizeof(target_inode.name));
    return write_inode((uint32_t)target, &target_inode);
}

bool nemofs_delete(uint32_t parent, const char *name) {
    int32_t target = nemofs_find_child(parent, name);
    if (target < 0) return false;

    nemofs_inode_t target_inode;
    if (!read_inode((uint32_t)target, &target_inode)) return false;

    if (target_inode.type == NEMOFS_TYPE_DIR && target_inode.num_children > 0) {
        return false; // carpeta no vacia
    }

    free_all_blocks(&target_inode);

    // Quitamos su entrada de la carpeta padre -- intercambiamos con la
    // ULTIMA entrada de la lista en vez de desplazar todo lo demas.
    nemofs_inode_t parent_inode;
    if (read_inode(parent, &parent_inode) && parent_inode.num_children > 0) {
        uint32_t pos_to_remove = parent_inode.num_children;
        for (uint32_t p = 0; p < parent_inode.num_children; p++) {
            uint32_t val;
            if (get_child_slot(&parent_inode, p, &val) && val == (uint32_t)target) {
                pos_to_remove = p;
                break;
            }
        }
        if (pos_to_remove < parent_inode.num_children) {
            uint32_t last_pos = parent_inode.num_children - 1;
            if (last_pos != pos_to_remove) {
                uint32_t last_val;
                if (get_child_slot(&parent_inode, last_pos, &last_val)) {
                    set_child_slot(&parent_inode, pos_to_remove, last_val);
                }
            }
            parent_inode.num_children--;
            write_inode(parent, &parent_inode);
        }
    }

    nemofs_inode_t freed;
    zero_bytes(&freed, sizeof(freed));
    freed.type = NEMOFS_TYPE_FREE;
    write_inode((uint32_t)target, &freed);
    inode_mark((uint32_t)target, false);
    if ((uint32_t)target < next_free_inode_hint) next_free_inode_hint = (uint32_t)target;
    bitmap_flush();

    return true;
}

bool nemofs_write_file(uint32_t inode_idx, const void *buf, uint32_t size) {
    uint32_t max_size = (sb.features & NEMOFS_FEAT_DINDIRECT) ? BLOCKS_MAX * SECTOR_SIZE : BLOCKS_SINGLE * SECTOR_SIZE;
    if (size > max_size) return false;

    nemofs_inode_t inode;
    if (!read_inode(inode_idx, &inode)) return false;
    if (inode.type != NEMOFS_TYPE_FILE) return false;

    // Se escribe el archivo ENTERO: liberar lo que tuviera y reservar
    // de nuevo. Mas simple que reutilizar bloques en el sitio, y
    // corrige una fuga de v1: al reescribir con menos datos, los
    // bloques sobrantes se quedaban ocupados para siempre.
    free_all_blocks(&inode);

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t blocks_needed = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;

    block_mapper_t m;
    mapper_init(&m, &inode, true);
    static uint8_t block_buf[SECTOR_SIZE];
    for (uint32_t b = 0; b < blocks_needed; b++) {
        uint32_t sector;
        if (!map_block(&m, b, &sector) || sector == 0) { mapper_finish(&m); bitmap_flush(); return false; }

        uint32_t offset_in_file = b * SECTOR_SIZE;
        uint32_t chunk = size - offset_in_file;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
        zero_bytes(block_buf, SECTOR_SIZE);
        for (uint32_t i = 0; i < chunk; i++) block_buf[i] = src[offset_in_file + i];
        if (!escribir_sector(sector, block_buf)) { mapper_finish(&m); bitmap_flush(); return false; }
    }
    if (!mapper_finish(&m)) return false;

    inode.size = size;
    if (!write_inode(inode_idx, &inode)) return false;
    bitmap_flush();
    return true;
}

// Añade bytes AL FINAL de un archivo, sin reescribirlo entero.
//
// POR QUE: hasta ahora escribir era todo o nada, asi que cualquiera que
// quisiera crear un archivo grande tenia que tenerlo ENTERO en memoria.
// El explorador reservaba 4MB de su tarea (de 16MB) solo para copiar, y
// aun asi no llegaba al techo de 8,25MB de un archivo de NemoFS. Con
// esto se copia por trozos y el buffer baja a unos pocos KB.
//
// A diferencia de nemofs_write_file, aqui NO se liberan los bloques que
// ya tiene el archivo: map_block devuelve el sector que ya estuviera
// asignado y solo reserva cuando la casilla esta a cero.
//
// El caso delicado es el PRIMER bloque: si el archivo acababa a mitad de
// un sector, ese sector ya tiene datos buenos y hay que leerlo,
// modificarle solo la cola y volver a escribirlo. Escribirlo sin leer
// primero llenaria de ceros lo que ya habia.
bool nemofs_append(uint32_t inode_idx, const void *buf, uint32_t size) {
    if (size == 0) return true;

    nemofs_inode_t inode;
    if (!read_inode(inode_idx, &inode)) return false;
    if (inode.type != NEMOFS_TYPE_FILE) return false;

    uint32_t max_size = (sb.features & NEMOFS_FEAT_DINDIRECT) ? BLOCKS_MAX * SECTOR_SIZE : BLOCKS_SINGLE * SECTOR_SIZE;
    if (size > max_size - inode.size) return false;   // sin desbordar la suma

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t pos = inode.size;        // donde empieza lo nuevo
    uint32_t escrito = 0;

    block_mapper_t m;
    mapper_init(&m, &inode, true);
    static uint8_t append_buf[SECTOR_SIZE];

    while (escrito < size) {
        uint32_t b = pos / SECTOR_SIZE;
        uint32_t dentro = pos % SECTOR_SIZE;
        uint32_t trozo = SECTOR_SIZE - dentro;
        if (trozo > size - escrito) trozo = size - escrito;

        uint32_t sector;
        if (!map_block(&m, b, &sector) || sector == 0) { mapper_finish(&m); bitmap_flush(); return false; }

        if (dentro > 0) {
            // El sector ya tenia datos validos: leer, cambiar solo la
            // cola, y devolverlo entero.
            if (!disk_read_sector(sector, append_buf)) { mapper_finish(&m); bitmap_flush(); return false; }
        } else {
            zero_bytes(append_buf, SECTOR_SIZE);
        }
        for (uint32_t i = 0; i < trozo; i++) append_buf[dentro + i] = src[escrito + i];
        if (!escribir_sector(sector, append_buf)) { mapper_finish(&m); bitmap_flush(); return false; }

        pos += trozo;
        escrito += trozo;
    }
    if (!mapper_finish(&m)) return false;

    inode.size = pos;
    if (!write_inode(inode_idx, &inode)) return false;
    bitmap_flush();
    return true;
}

// Escribe el archivo SOLO si su contenido es distinto del que ya tiene.
//
// La usa el kernel al arrancar, que instala en la tarjeta todos los
// programas del sistema (shell, explorador, editor, el compilador, Lua y
// sus ejemplos...). Antes los reescribia TODOS en cada arranque: cientos
// de KB de escrituras en la SD cada vez que se encendia la Pi. Eso desgasta
// la tarjeta y, sobre todo, abre en cada arranque una ventana en la que un
// corte de corriente la puede dejar corrompida -- que fue lo que paso.
//
// Leer primero no tiene ninguno de esos dos problemas: leer no desgasta
// la tarjeta ni la puede dejar a medias. Y casi siempre el contenido es el
// mismo, asi que casi nunca hay que escribir.
//
// Se lee un byte mas de lo esperado a proposito: si el archivo de la
// tarjeta es MAS LARGO que el nuevo, la lectura devuelve mas bytes de los
// esperados y se detecta como distinto.
//
// Si no hay memoria para comparar, escribe sin mas: mejor escribir de mas
// que dejar un programa viejo en la tarjeta.
bool nemofs_write_file_if_changed(uint32_t inode_idx, const void *buf, uint32_t size) {
    uint8_t *actual = (uint8_t *)kmalloc((size_t)size + 1);
    if (actual) {
        int32_t leidos = nemofs_read_file(inode_idx, actual, size + 1);
        bool igual = (leidos == (int32_t)size);
        const uint8_t *nuevo = (const uint8_t *)buf;
        for (uint32_t k = 0; igual && k < size; k++) {
            if (actual[k] != nuevo[k]) igual = false;
        }
        kfree(actual);
        if (igual) return true;   // ya estaba igual: nada que escribir
    }
    return nemofs_write_file(inode_idx, buf, size);
}

// Cuanto mide un archivo, por inodo. Parece una obviedad y no existia: para
// saber el tamaño habia que listar la carpeta PADRE entera y buscar la
// entrada por inodo (ver file_size_by_name en syscall.c, que lo dice en un
// comentario). Hace falta de verdad para poder reservar un buffer del tamaño
// del archivo ANTES de leerlo, en vez de reservar un maximo fijo.
//
// Devuelve -1 si el inodo no existe o no es un archivo, para que "vacio" (0)
// y "no se puede" se distingan.
int32_t nemofs_file_size(uint32_t inode_idx) {
    nemofs_inode_t inode;
    if (!read_inode(inode_idx, &inode)) return -1;
    if (inode.type != NEMOFS_TYPE_FILE) return -1;
    return (int32_t)inode.size;
}

int32_t nemofs_read_file(uint32_t inode_idx, void *buf, uint32_t max_size) {
    nemofs_inode_t inode;
    if (!read_inode(inode_idx, &inode)) return -1;
    if (inode.type != NEMOFS_TYPE_FILE) return -1;

    uint32_t to_read = inode.size;
    if (to_read > max_size) to_read = max_size;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t blocks = (to_read + SECTOR_SIZE - 1) / SECTOR_SIZE;

    block_mapper_t m;
    mapper_init(&m, &inode, false);
    static uint8_t block_buf[SECTOR_SIZE];
    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t sector;
        if (!map_block(&m, b, &sector)) return -1;
        if (sector == 0) break;
        if (!disk_read_sector(sector, block_buf)) return -1;

        uint32_t offset_in_file = b * SECTOR_SIZE;
        uint32_t chunk = to_read - offset_in_file;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
        for (uint32_t i = 0; i < chunk; i++) dst[offset_in_file + i] = block_buf[i];
    }
    return (int32_t)to_read;
}

// Lee hasta 'len' bytes desde 'offset' (fase 5 de la memoria). Mismo
// criterio que fat_read_at: devuelve los leidos (0 en el final o mas
// alla), -1 si falla. Los archivos de NemoFS no pasan de ~8 MB
// (BLOCKS_MAX), pero asi los programas tienen una sola forma de leer por
// partes en los dos volumenes.
int32_t nemofs_read_at(uint32_t inode_idx, uint32_t offset, void *buf, uint32_t len) {
    nemofs_inode_t inode;
    if (!read_inode(inode_idx, &inode)) return -1;
    if (inode.type != NEMOFS_TYPE_FILE) return -1;
    if (offset >= inode.size || len == 0) return 0;
    if (len > inode.size - offset) len = inode.size - offset;
    block_mapper_t m;
    mapper_init(&m, &inode, false);
    static uint8_t acopio[NEMOFS_ACOPIO * SECTOR_SIZE];
    uint8_t *dst = (uint8_t *)buf;
    uint32_t hecho = 0;
    while (hecho < len) {
        uint32_t pos = offset + hecho;
        uint32_t b = pos / SECTOR_SIZE, en_bloque = pos % SECTOR_SIZE;
        uint32_t sector;
        if (!map_block(&m, b, &sector) || sector == 0) return -1;
        uint32_t queda = len - hecho;
        if (en_bloque == 0 && queda >= SECTOR_SIZE && ((uintptr_t)(dst + hecho) & 3u) == 0) {
            // Mirar cuantos bloques siguientes caen SEGUIDOS en el disco
            // y pedirlos todos de una vez. En un archivo recien escrito
            // suelen ir corridos, asi que esto convierte cientos de
            // lecturas sueltas en unas pocas. Asomarse es barato: el
            // mapeador ya tiene el bloque de indices en memoria, no
            // vuelve al disco salvo al cambiar de indirecto.
            uint32_t seguidos = 1;
            uint32_t tope = queda / SECTOR_SIZE;
            if (tope > NEMOFS_TIRADA_MAX) tope = NEMOFS_TIRADA_MAX;
            while (seguidos < tope) {
                uint32_t otro;
                if (!map_block(&m, b + seguidos, &otro)) break;
                if (otro != sector + seguidos) break;    // deja de ir seguido
                seguidos++;
            }
            if (seguidos > 1) {
                if (!disk_read_sectors(sector, seguidos, dst + hecho)) return -1;
            } else {
                if (!disk_read_sector(sector, dst + hecho)) return -1;   // directo al destino
            }
            hecho += seguidos * SECTOR_SIZE;
        } else {
            // El trozo no empieza en el borde de un sector, o el destino
            // no esta alineado. Pasa MUCHO mas de lo que parece: una
            // fila de un .nimg empieza en el byte 12, por la cabecera,
            // asi que NINGUNA cae en el borde. Antes esto iba sector a
            // sector -- tres comandos para leer una fila de 1024 bytes.
            // Ahora se traen de una vez todos los sectores seguidos que
            // hagan falta para cubrir lo que queda.
            uint32_t hacen_falta = (en_bloque + queda + SECTOR_SIZE - 1) / SECTOR_SIZE;
            if (hacen_falta > NEMOFS_ACOPIO) hacen_falta = NEMOFS_ACOPIO;
            uint32_t seguidos = 1;
            while (seguidos < hacen_falta) {
                uint32_t otro;
                if (!map_block(&m, b + seguidos, &otro)) break;
                if (otro != sector + seguidos) break;
                seguidos++;
            }
            if (seguidos > 1) {
                if (!disk_read_sectors(sector, seguidos, acopio)) return -1;
            } else {
                if (!disk_read_sector(sector, acopio)) return -1;
            }
            uint32_t n = seguidos * SECTOR_SIZE - en_bloque;
            if (n > queda) n = queda;
            for (uint32_t i = 0; i < n; i++) dst[hecho + i] = acopio[en_bloque + i];
            hecho += n;
        }
    }
    return (int32_t)hecho;
}

uint32_t nemofs_list_dir(uint32_t parent, nemofs_dirent_t *out, uint32_t max_count) {
    nemofs_inode_t parent_inode;
    if (!read_inode(parent, &parent_inode)) return 0;
    if (parent_inode.type != NEMOFS_TYPE_DIR) return 0;

    static uint8_t block_buf[SECTOR_SIZE];
    uint32_t entries_per_block = SECTOR_SIZE / sizeof(uint32_t);
    int32_t last_block_loaded = -1;

    for (uint32_t i = 0; i < parent_inode.num_children; i++) {
        uint32_t block_index = i / entries_per_block;
        uint32_t offset = i % entries_per_block;

        if (block_index >= DIRECT_BLOCKS) break;
        uint32_t sector = parent_inode.direct[block_index];
        if (sector == 0) break;

        if ((int32_t)block_index != last_block_loaded) {
            if (!disk_read_sector(sector, block_buf)) break;
            last_block_loaded = (int32_t)block_index;
        }

        uint32_t child_idx = ((uint32_t *)block_buf)[offset];
        if (i < max_count) {
            nemofs_inode_t child;
            if (read_inode(child_idx, &child)) {
                out[i].inode = child_idx;
                out[i].type = child.type;
                str_copy_n(out[i].name, child.name, sizeof(out[i].name));
                out[i].size = child.size;
            }
        }
    }

    return parent_inode.num_children;
}
