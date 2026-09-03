// nemofs.c — Nemo OS
//
// Sistema de archivos propio. Diseño deliberadamente simple:
//
//   Sector 0:              Superbloque
//   Sectores siguientes:   Tabla de inodos (tamaño fijo)
//   Sectores siguientes:   Bitmap de bloques de datos libres/ocupados
//   Resto del disco:       Bloques de datos (1 bloque = 1 sector = 512B)
//
// Cada inodo describe UN archivo o UNA carpeta. Una carpeta es, en el
// fondo, un archivo cuyo contenido es una lista de índices de inodos
// hijos -- así toda la maquinaria de "escribir/leer bloques" se
// reutiliza sin duplicar código entre archivos y carpetas.
//
// No usamos memcpy/strcmp de la librería estándar (no tenemos libc
// enlazada); las pocas utilidades de cadenas que necesitamos están
// escritas a mano al final de este archivo.

#include "nemofs.h"
#include "disk.h"
#include "uart.h"

#define NEMOFS_MAGIC 0x4F4D454EUL // "NEMO" en little-endian, con la 'N' repetida a proposito

#define MAX_INODES 256
#define DIRECT_BLOCKS 12
#define BLOCKS_PER_INDIRECT (SECTOR_SIZE / 4) // 128 punteros de 4 bytes por bloque
#define MAX_BITMAP_BYTES (64 * 1024) // soporta discos de hasta ~256MB en v1

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
} nemofs_superblock_t;

// Exactamente 128 bytes -- así 4 inodos caben en cada sector de 512
// bytes sin partirse entre sectores, lo cual simplifica mucho la
// lectura/escritura.
//
// LIMITE DE v1 SUPERADO POR CUARTA VEZ: con tres bloques indirectos,
// el tamaño maximo era 202752 bytes -- pero nbc.pro YA COMPILADO CON
// EL TOOLCHAIN REAL (no una estimacion) ocupa 207854 bytes, por
// encima de ese limite (¡confirmado con la compilacion real en Mac,
// no una suposicion!). Añadimos un CUARTO bloque indirecto, tomando
// otros 4 bytes de 'reserved' (23->19, la estructura sigue midiendo
// 128 bytes en total). Con esto, el tamaño maximo pasa de 202752 a
// (12 + 128 + 128 + 128 + 128) * 512 = 268288 bytes (~262KB) -- deja
// margen real (~60KB) frente a los 207854 bytes que necesita nbc.pro.
typedef struct __attribute__((packed)) {
    uint8_t type;                    // NEMOFS_TYPE_*
    char name[NEMOFS_MAX_NAME + 1];  // 32 bytes
    uint32_t parent;                 // índice de inodo del padre
    uint32_t size;                   // bytes usados (archivos) / no usado igual en dirs
    uint32_t num_children;           // solo relevante para carpetas
    uint32_t direct[DIRECT_BLOCKS];  // sectores de datos, 0 = sin asignar
    uint32_t indirect;               // sector con 128 punteros mas, 0 = sin asignar
    uint32_t indirect2;              // un segundo sector con 128 punteros mas, 0 = sin asignar
    uint32_t indirect3;              // un tercer sector con 128 punteros mas, 0 = sin asignar
    uint32_t indirect4;              // un cuarto sector con 128 punteros mas, 0 = sin asignar
    uint8_t reserved[19];
} nemofs_inode_t;

static nemofs_superblock_t sb;
static uint8_t bitmap_cache[MAX_BITMAP_BYTES];

// ---- utilidades de cadenas, sin depender de libc ----

static bool str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b;
}

// Igual que str_eq, pero sin distinguir mayusculas de minusculas --
// la usamos para nombres de archivo, porque el disco FAT suele
// guardar los nombres en mayusculas (formato 8.3) y no queremos que
// eso rompa la busqueda si el usuario escribe el nombre en minusculas.
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

// ---- acceso a inodos (lectura/escritura sector a sector) ----

static bool read_inode(uint32_t idx, nemofs_inode_t *out) {
    if (idx >= sb.inode_count) return false;

    uint32_t inodes_per_sector = 512 / sizeof(nemofs_inode_t);
    uint32_t sector = sb.inode_table_start + (idx / inodes_per_sector);
    uint32_t offset = idx % inodes_per_sector;

    static uint8_t buf[SECTOR_SIZE];
    if (!disk_read_sector(sector, buf)) return false;

    nemofs_inode_t *inodes = (nemofs_inode_t *)buf;
    *out = inodes[offset];
    return true;
}

static bool write_inode(uint32_t idx, const nemofs_inode_t *in) {
    if (idx >= sb.inode_count) return false;

    uint32_t inodes_per_sector = 512 / sizeof(nemofs_inode_t);
    uint32_t sector = sb.inode_table_start + (idx / inodes_per_sector);
    uint32_t offset = idx % inodes_per_sector;

    static uint8_t buf[SECTOR_SIZE];
    if (!disk_read_sector(sector, buf)) return false; // leemos primero para no pisar los otros 3 inodos del sector

    nemofs_inode_t *inodes = (nemofs_inode_t *)buf;
    inodes[offset] = *in;

    return disk_write_sector(sector, buf);
}

static int32_t alloc_inode(void) {
    nemofs_inode_t tmp;
    for (uint32_t i = 0; i < sb.inode_count; i++) {
        if (!read_inode(i, &tmp)) return -1;
        if (tmp.type == NEMOFS_TYPE_FREE) {
            return (int32_t)i;
        }
    }
    return -1; // tabla de inodos llena
}

// ---- bitmap de bloques de datos ----

static void bitmap_flush_byte(uint32_t byte_index) {
    uint32_t sector_in_bitmap = byte_index / SECTOR_SIZE;
    disk_write_sector(sb.bitmap_start + sector_in_bitmap,
                       &bitmap_cache[sector_in_bitmap * SECTOR_SIZE]);
}

static int32_t alloc_block(void) {
    for (uint32_t i = 0; i < sb.data_blocks; i++) {
        uint32_t byte = i / 8;
        uint8_t bit = i % 8;
        if (!(bitmap_cache[byte] & (1 << bit))) {
            bitmap_cache[byte] |= (1 << bit);
            bitmap_flush_byte(byte);
            return (int32_t)(sb.data_start + i);
        }
    }
    return -1; // disco lleno
}

// Libera un bloque de datos previamente reservado con alloc_block --
// lo necesita nemofs_delete para no ir dejando bloques huerfanos cada
// vez que se borra un archivo.
static void free_block(uint32_t sector) {
    if (sector < sb.data_start) return;
    uint32_t i = sector - sb.data_start;
    if (i >= sb.data_blocks) return;
    uint32_t byte = i / 8;
    uint8_t bit = i % 8;
    bitmap_cache[byte] &= (uint8_t)~(1 << bit);
    bitmap_flush_byte(byte);
}

// ---- formateo y montaje ----

static bool nemofs_format(void) {
    uint64_t total = disk_capacity_sectors();
    if (total == 0) {
        uart_puts("nemofs: no se pudo leer la capacidad del disco\n");
        return false;
    }

    uint32_t inode_table_start = 1;
    uint32_t inode_table_sectors = (MAX_INODES * sizeof(nemofs_inode_t)) / SECTOR_SIZE;
    uint32_t bitmap_start = inode_table_start + inode_table_sectors;

    uint32_t remaining = (uint32_t)total - bitmap_start;
    uint32_t bitmap_bytes = (remaining + 7) / 8;
    if (bitmap_bytes > MAX_BITMAP_BYTES) {
        bitmap_bytes = MAX_BITMAP_BYTES; // disco mayor de lo que soportamos en v1
    }
    uint32_t bitmap_sectors = (bitmap_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t data_start = bitmap_start + bitmap_sectors;
    uint32_t data_blocks = (uint32_t)total - data_start;

    sb.magic = NEMOFS_MAGIC;
    sb.total_sectors = (uint32_t)total;
    sb.inode_table_start = inode_table_start;
    sb.inode_count = MAX_INODES;
    sb.bitmap_start = bitmap_start;
    sb.bitmap_sectors = bitmap_sectors;
    sb.data_start = data_start;
    sb.data_blocks = data_blocks;
    sb.root_inode = NEMOFS_ROOT_INODE;

    // Escribimos el superbloque (sector 0). Usamos un buffer de sector
    // completo y "superponemos" la struct en él, en vez de depender de
    // memcpy (que no tenemos disponible sin libc).
    static uint8_t sector_buf[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) sector_buf[i] = 0;
    *(nemofs_superblock_t *)sector_buf = sb;
    if (!disk_write_sector(0, sector_buf)) return false;

    // Vaciamos toda la tabla de inodos (todos "libres")
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) sector_buf[i] = 0;
    for (uint32_t s = 0; s < inode_table_sectors; s++) {
        if (!disk_write_sector(inode_table_start + s, sector_buf)) return false;
    }

    // Vaciamos el bitmap (todo libre) tanto en RAM como en disco
    for (uint32_t i = 0; i < MAX_BITMAP_BYTES; i++) bitmap_cache[i] = 0;
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) sector_buf[i] = 0;
    for (uint32_t s = 0; s < bitmap_sectors; s++) {
        if (!disk_write_sector(bitmap_start + s, sector_buf)) return false;
    }

    // Creamos la carpeta raíz como inodo 0
    nemofs_inode_t root;
    root.type = NEMOFS_TYPE_DIR;
    str_copy_n(root.name, "/", sizeof(root.name));
    root.parent = NEMOFS_ROOT_INODE; // la raíz es su propio padre
    root.size = 0;
    root.num_children = 0;
    for (int i = 0; i < DIRECT_BLOCKS; i++) root.direct[i] = 0;
    root.indirect = 0;
    root.indirect2 = 0;
    root.indirect3 = 0;
    root.indirect4 = 0;

    if (!write_inode(NEMOFS_ROOT_INODE, &root)) return false;

    uart_puts("nemofs: disco formateado correctamente.\n");
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

    for (uint32_t s = 0; s < sb.bitmap_sectors; s++) {
        if (!disk_read_sector(sb.bitmap_start + s, &bitmap_cache[s * SECTOR_SIZE])) {
            uart_puts("nemofs: fallo leyendo el bitmap\n");
            return false;
        }
    }

    uart_puts("nemofs: montado correctamente.\n");
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

        if (block_index >= DIRECT_BLOCKS) break; // no debería pasar en v1
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

    int32_t new_idx = alloc_inode();
    if (new_idx < 0) return -1;

    nemofs_inode_t new_inode;
    new_inode.type = type;
    str_copy_n(new_inode.name, name, sizeof(new_inode.name));
    new_inode.parent = parent;
    new_inode.size = 0;
    new_inode.num_children = 0;
    for (int i = 0; i < DIRECT_BLOCKS; i++) new_inode.direct[i] = 0;
    new_inode.indirect = 0;
    new_inode.indirect2 = 0;
    new_inode.indirect3 = 0;
    new_inode.indirect4 = 0;

    if (!write_inode((uint32_t)new_idx, &new_inode)) return -1;

    // Añadimos el nuevo inodo como hijo del padre
    uint32_t entries_per_block = SECTOR_SIZE / sizeof(uint32_t);
    uint32_t block_index = parent_inode.num_children / entries_per_block;
    uint32_t offset = parent_inode.num_children % entries_per_block;

    if (block_index >= DIRECT_BLOCKS) {
        return -1; // carpeta llena (limite de v1)
    }

    static uint8_t block_buf[SECTOR_SIZE];
    if (parent_inode.direct[block_index] == 0) {
        int32_t new_block = alloc_block();
        if (new_block < 0) return -1;
        parent_inode.direct[block_index] = (uint32_t)new_block;
        for (uint32_t i = 0; i < SECTOR_SIZE; i++) block_buf[i] = 0;
    } else {
        if (!disk_read_sector(parent_inode.direct[block_index], block_buf)) return -1;
    }

    ((uint32_t *)block_buf)[offset] = (uint32_t)new_idx;
    if (!disk_write_sector(parent_inode.direct[block_index], block_buf)) return -1;

    parent_inode.num_children++;
    if (!write_inode(parent, &parent_inode)) return -1;

    return new_idx;
}

// Lee/escribe la entrada de hijo en la posicion 'pos' dentro del
// listado de la carpeta 'parent_inode' -- lo usa nemofs_delete para
// poder quitar una entrada del medio de la lista.
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
    return disk_write_sector(parent_inode->direct[block_index], buf);
}

// Borra un archivo o una carpeta VACIA de 'parent'. Borrar una
// carpeta con contenido dentro no esta soportado todavia (habria que
// hacerlo recursivamente) -- se queda para una version futura.
bool nemofs_delete(uint32_t parent, const char *name) {
    int32_t target = nemofs_find_child(parent, name);
    if (target < 0) return false;

    nemofs_inode_t target_inode;
    if (!read_inode((uint32_t)target, &target_inode)) return false;

    if (target_inode.type == NEMOFS_TYPE_DIR && target_inode.num_children > 0) {
        return false; // carpeta no vacia
    }

    // Liberamos los bloques de datos del archivo (directos + los dos
    // indirectos, si los tenia)
    for (int i = 0; i < DIRECT_BLOCKS; i++) {
        if (target_inode.direct[i] != 0) free_block(target_inode.direct[i]);
    }
    if (target_inode.indirect != 0) {
        static uint8_t ind_buf[SECTOR_SIZE];
        if (disk_read_sector(target_inode.indirect, ind_buf)) {
            uint32_t *ptrs = (uint32_t *)ind_buf;
            for (uint32_t i = 0; i < BLOCKS_PER_INDIRECT; i++) {
                if (ptrs[i] != 0) free_block(ptrs[i]);
            }
        }
        free_block(target_inode.indirect);
    }
    if (target_inode.indirect2 != 0) {
        static uint8_t ind_buf2[SECTOR_SIZE];
        if (disk_read_sector(target_inode.indirect2, ind_buf2)) {
            uint32_t *ptrs = (uint32_t *)ind_buf2;
            for (uint32_t i = 0; i < BLOCKS_PER_INDIRECT; i++) {
                if (ptrs[i] != 0) free_block(ptrs[i]);
            }
        }
        free_block(target_inode.indirect2);
    }
    if (target_inode.indirect3 != 0) {
        static uint8_t ind_buf3[SECTOR_SIZE];
        if (disk_read_sector(target_inode.indirect3, ind_buf3)) {
            uint32_t *ptrs = (uint32_t *)ind_buf3;
            for (uint32_t i = 0; i < BLOCKS_PER_INDIRECT; i++) {
                if (ptrs[i] != 0) free_block(ptrs[i]);
            }
        }
        free_block(target_inode.indirect3);
    }
    if (target_inode.indirect4 != 0) {
        static uint8_t ind_buf4[SECTOR_SIZE];
        if (disk_read_sector(target_inode.indirect4, ind_buf4)) {
            uint32_t *ptrs = (uint32_t *)ind_buf4;
            for (uint32_t i = 0; i < BLOCKS_PER_INDIRECT; i++) {
                if (ptrs[i] != 0) free_block(ptrs[i]);
            }
        }
        free_block(target_inode.indirect4);
    }

    // Quitamos su entrada de la carpeta padre -- intercambiamos con la
    // ULTIMA entrada de la lista en vez de desplazar todo lo demas, es
    // mucho mas simple y el orden de los hijos no importa para nada.
    nemofs_inode_t parent_inode;
    if (read_inode(parent, &parent_inode) && parent_inode.num_children > 0) {
        uint32_t pos_to_remove = parent_inode.num_children; // centinela "no encontrado"
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

    // Marcamos el inodo como libre para que alloc_inode lo reutilice
    nemofs_inode_t freed;
    freed.type = NEMOFS_TYPE_FREE;
    freed.name[0] = '\0';
    freed.parent = 0;
    freed.size = 0;
    freed.num_children = 0;
    for (int i = 0; i < DIRECT_BLOCKS; i++) freed.direct[i] = 0;
    freed.indirect = 0;
    freed.indirect2 = 0;
    freed.indirect3 = 0;
    freed.indirect4 = 0;
    write_inode((uint32_t)target, &freed);

    return true;
}

bool nemofs_write_file(uint32_t inode_idx, const void *buf, uint32_t size) {
    uint32_t max_size = (DIRECT_BLOCKS + BLOCKS_PER_INDIRECT * 4) * SECTOR_SIZE;
    if (size > max_size) {
        return false; // supera el tamano maximo de archivo (12 directos + 4 indirectos)
    }

    nemofs_inode_t inode;
    if (!read_inode(inode_idx, &inode)) return false;
    if (inode.type != NEMOFS_TYPE_FILE) return false;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t blocks_needed = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;

    static uint8_t block_buf[SECTOR_SIZE];
    static uint8_t indirect_buf[SECTOR_SIZE];
    static uint8_t indirect2_buf[SECTOR_SIZE];
    static uint8_t indirect3_buf[SECTOR_SIZE];
    static uint8_t indirect4_buf[SECTOR_SIZE];
    bool indirect_loaded = false, indirect_dirty = false;
    bool indirect2_loaded = false, indirect2_dirty = false;
    bool indirect3_loaded = false, indirect3_dirty = false;
    bool indirect4_loaded = false, indirect4_dirty = false;

    for (uint32_t b = 0; b < blocks_needed; b++) {
        uint32_t *slot;

        if (b < DIRECT_BLOCKS) {
            slot = &inode.direct[b];
        } else if (b < DIRECT_BLOCKS + BLOCKS_PER_INDIRECT) {
            // Bloques 12-139: los punteros viven DENTRO del primer
            // bloque indirecto, no en el propio inodo.
            if (inode.indirect == 0) {
                int32_t nb = alloc_block();
                if (nb < 0) return false;
                inode.indirect = (uint32_t)nb;
                for (uint32_t i = 0; i < SECTOR_SIZE; i++) indirect_buf[i] = 0;
                indirect_loaded = true;
            }
            if (!indirect_loaded) {
                if (!disk_read_sector(inode.indirect, indirect_buf)) return false;
                indirect_loaded = true;
            }
            slot = &((uint32_t *)indirect_buf)[b - DIRECT_BLOCKS];
            indirect_dirty = true;
        } else if (b < DIRECT_BLOCKS + BLOCKS_PER_INDIRECT + BLOCKS_PER_INDIRECT) {
            // Bloques 140-267: en el SEGUNDO bloque indirecto.
            if (inode.indirect2 == 0) {
                int32_t nb = alloc_block();
                if (nb < 0) return false;
                inode.indirect2 = (uint32_t)nb;
                for (uint32_t i = 0; i < SECTOR_SIZE; i++) indirect2_buf[i] = 0;
                indirect2_loaded = true;
            }
            if (!indirect2_loaded) {
                if (!disk_read_sector(inode.indirect2, indirect2_buf)) return false;
                indirect2_loaded = true;
            }
            slot = &((uint32_t *)indirect2_buf)[b - DIRECT_BLOCKS - BLOCKS_PER_INDIRECT];
            indirect2_dirty = true;
        } else if (b < DIRECT_BLOCKS + BLOCKS_PER_INDIRECT * 3) {
            // Bloques 268-395: en el TERCER bloque indirecto.
            if (inode.indirect3 == 0) {
                int32_t nb = alloc_block();
                if (nb < 0) return false;
                inode.indirect3 = (uint32_t)nb;
                for (uint32_t i = 0; i < SECTOR_SIZE; i++) indirect3_buf[i] = 0;
                indirect3_loaded = true;
            }
            if (!indirect3_loaded) {
                if (!disk_read_sector(inode.indirect3, indirect3_buf)) return false;
                indirect3_loaded = true;
            }
            slot = &((uint32_t *)indirect3_buf)[b - DIRECT_BLOCKS - BLOCKS_PER_INDIRECT - BLOCKS_PER_INDIRECT];
            indirect3_dirty = true;
        } else {
            // Bloques 396-523: en el CUARTO bloque indirecto.
            if (inode.indirect4 == 0) {
                int32_t nb = alloc_block();
                if (nb < 0) return false;
                inode.indirect4 = (uint32_t)nb;
                for (uint32_t i = 0; i < SECTOR_SIZE; i++) indirect4_buf[i] = 0;
                indirect4_loaded = true;
            }
            if (!indirect4_loaded) {
                if (!disk_read_sector(inode.indirect4, indirect4_buf)) return false;
                indirect4_loaded = true;
            }
            slot = &((uint32_t *)indirect4_buf)[b - DIRECT_BLOCKS - BLOCKS_PER_INDIRECT * 3];
            indirect4_dirty = true;
        }

        if (*slot == 0) {
            int32_t new_block = alloc_block();
            if (new_block < 0) return false;
            *slot = (uint32_t)new_block;
        }

        uint32_t offset_in_file = b * SECTOR_SIZE;
        uint32_t chunk = size - offset_in_file;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;

        for (uint32_t i = 0; i < SECTOR_SIZE; i++) block_buf[i] = 0;
        for (uint32_t i = 0; i < chunk; i++) {
            block_buf[i] = src[offset_in_file + i];
        }

        if (!disk_write_sector(*slot, block_buf)) return false;
    }

    if (indirect_dirty) {
        if (!disk_write_sector(inode.indirect, indirect_buf)) return false;
    }
    if (indirect2_dirty) {
        if (!disk_write_sector(inode.indirect2, indirect2_buf)) return false;
    }
    if (indirect3_dirty) {
        if (!disk_write_sector(inode.indirect3, indirect3_buf)) return false;
    }
    if (indirect4_dirty) {
        if (!disk_write_sector(inode.indirect4, indirect4_buf)) return false;
    }

    inode.size = size;
    return write_inode(inode_idx, &inode);
}

int32_t nemofs_read_file(uint32_t inode_idx, void *buf, uint32_t max_size) {
    nemofs_inode_t inode;
    if (!read_inode(inode_idx, &inode)) return -1;
    if (inode.type != NEMOFS_TYPE_FILE) return -1;

    uint32_t to_read = inode.size;
    if (to_read > max_size) to_read = max_size;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t blocks = (to_read + SECTOR_SIZE - 1) / SECTOR_SIZE;

    static uint8_t block_buf[SECTOR_SIZE];
    static uint8_t indirect_buf[SECTOR_SIZE];
    static uint8_t indirect2_buf[SECTOR_SIZE];
    static uint8_t indirect3_buf[SECTOR_SIZE];
    static uint8_t indirect4_buf[SECTOR_SIZE];
    bool indirect_loaded = false;
    bool indirect2_loaded = false;
    bool indirect3_loaded = false;
    bool indirect4_loaded = false;

    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t sector;

        if (b < DIRECT_BLOCKS) {
            sector = inode.direct[b];
        } else if (b < DIRECT_BLOCKS + BLOCKS_PER_INDIRECT) {
            if (inode.indirect == 0) break;
            if (!indirect_loaded) {
                if (!disk_read_sector(inode.indirect, indirect_buf)) return -1;
                indirect_loaded = true;
            }
            sector = ((uint32_t *)indirect_buf)[b - DIRECT_BLOCKS];
        } else if (b < DIRECT_BLOCKS + BLOCKS_PER_INDIRECT + BLOCKS_PER_INDIRECT) {
            if (inode.indirect2 == 0) break;
            if (!indirect2_loaded) {
                if (!disk_read_sector(inode.indirect2, indirect2_buf)) return -1;
                indirect2_loaded = true;
            }
            sector = ((uint32_t *)indirect2_buf)[b - DIRECT_BLOCKS - BLOCKS_PER_INDIRECT];
        } else if (b < DIRECT_BLOCKS + BLOCKS_PER_INDIRECT * 3) {
            if (inode.indirect3 == 0) break;
            if (!indirect3_loaded) {
                if (!disk_read_sector(inode.indirect3, indirect3_buf)) return -1;
                indirect3_loaded = true;
            }
            sector = ((uint32_t *)indirect3_buf)[b - DIRECT_BLOCKS - BLOCKS_PER_INDIRECT - BLOCKS_PER_INDIRECT];
        } else {
            if (inode.indirect4 == 0) break;
            if (!indirect4_loaded) {
                if (!disk_read_sector(inode.indirect4, indirect4_buf)) return -1;
                indirect4_loaded = true;
            }
            sector = ((uint32_t *)indirect4_buf)[b - DIRECT_BLOCKS - BLOCKS_PER_INDIRECT * 3];
        }

        if (sector == 0) break;
        if (!disk_read_sector(sector, block_buf)) return -1;

        uint32_t offset_in_file = b * SECTOR_SIZE;
        uint32_t chunk = to_read - offset_in_file;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;

        for (uint32_t i = 0; i < chunk; i++) {
            dst[offset_in_file + i] = block_buf[i];
        }
    }

    return (int32_t)to_read;
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
