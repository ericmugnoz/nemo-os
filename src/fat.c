// fat.c — Nemo OS
//
// Driver FAT16/32. A diferencia de NemoFS (que diseñamos nosotros desde
// cero), FAT es un formato EXTERNO con una especificación exacta que hay
// que respetar al milímetro -- si no, tu Mac no podrá leer el disco.
//
// Conceptos clave de FAT:
//   - BPB (BIOS Parameter Block): los primeros bytes del disco describen
//     el layout entero (tamaño de sector, de cluster, cuantas copias de
//     la tabla FAT hay, etc.)
//   - La "tabla FAT" es como el bitmap de NemoFS, pero en vez de 1 bit
//     por bloque, tiene una ENTRADA por cluster que apunta al SIGUIENTE
//     cluster del archivo (es una lista enlazada, no un array de bits).
//     El valor especial "fin de cadena" (EOC) marca el último cluster.
//   - Un archivo o carpeta es una cadena de clusters enlazados a través
//     de la tabla FAT, empezando por el "primer cluster" que guarda su
//     entrada de directorio.
//
// LIMITACION CONSCIENTE DE ESTA v1: solo soportamos el directorio raiz
// (sin subcarpetas) y nombres cortos 8.3 (sin nombres largos VFAT). Es
// suficiente para intercambiar archivos sueltos con el Mac; ampliar a
// subcarpetas es sencillo mas adelante reutilizando la misma logica de
// "recorrer una cadena de clusters".

#include "fat.h"
#include "disk.h"
#include "uart.h"

#define DIR_ENTRY_SIZE 32
#define ATTR_DIRECTORY 0x10
#define ATTR_LONG_NAME 0x0F
#define ATTR_VOLUME_ID 0x08

static uint8_t used_disk = 0;

static uint32_t bytes_per_sector;
static uint32_t sectors_per_cluster;
static uint32_t reserved_sector_count;
static uint32_t num_fats;
static uint32_t fat_size;           // sectores por copia de la FAT
static uint32_t total_sectors;
static uint32_t first_fat_sector;
static uint32_t first_data_sector;
static uint32_t count_of_clusters;
static bool is_fat32;

// -- solo FAT16 --
static uint32_t root_dir_start_sector;
static uint32_t root_dir_sector_count;
static uint32_t root_entry_count;

// -- solo FAT32 --
static uint32_t root_cluster;

#define EOC_MARK_16 0xFFFF
#define EOC_MARK_32 0x0FFFFFFF
#define FREE_CLUSTER 0

// ---- utilidades ----

static uint16_t read16(const uint8_t *buf, uint32_t off) {
    return (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
}
static uint32_t read32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}
static void write16(uint8_t *buf, uint32_t off, uint16_t val) {
    buf[off] = val & 0xFF;
    buf[off + 1] = (val >> 8) & 0xFF;
}
static void write32(uint8_t *buf, uint32_t off, uint32_t val) {
    buf[off] = val & 0xFF;
    buf[off + 1] = (val >> 8) & 0xFF;
    buf[off + 2] = (val >> 16) & 0xFF;
    buf[off + 3] = (val >> 24) & 0xFF;
}

static bool read_sector(uint32_t sector, uint8_t *buf) {
    return disk_read_sector_n(used_disk, sector, buf);
}
static bool write_sector(uint32_t sector, const uint8_t *buf) {
    return disk_write_sector_n(used_disk, sector, buf);
}

// Convierte un nombre normal ("hello.txt") al formato 8.3 de FAT
// (11 bytes: 8 para el nombre + 3 para la extension, en mayusculas y
// rellenado con espacios). Es una conversion simplificada -- asume
// nombres ASCII razonables.
static void to_fat_83(const char *name, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';

    int i = 0, j = 0;
    while (name[i] != '\0' && name[i] != '.' && j < 8) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[j++] = (uint8_t)c;
        i++;
    }
    while (name[i] != '\0' && name[i] != '.') i++; // saltar el resto del nombre si era mas largo
    if (name[i] == '.') {
        i++;
        int k = 8;
        while (name[i] != '\0' && k < 11) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[k++] = (uint8_t)c;
            i++;
        }
    }
}

// Convierte el formato 8.3 de vuelta a algo legible: "HELLO   TXT" -> "HELLO.TXT"
static void from_fat_83(const uint8_t raw[11], char *out) {
    int j = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) out[j++] = (char)raw[i];
    if (raw[8] != ' ') {
        out[j++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) out[j++] = (char)raw[i];
    }
    out[j] = '\0';
}

static bool name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

// ---- nombres largos (VFAT/LFN) ----
//
// Un archivo con nombre largo se guarda como VARIAS entradas de
// directorio de 32 bytes ANTES de su entrada 8.3 normal: cada una
// lleva hasta 13 caracteres (en UTF-16, aunque nosotros solo
// manejamos ASCII/Latin1 basico) y un atributo especial (0x0F) que
// hace que los lectores FAT normales las ignoren -- solo un lector
// que entienda VFAT (como el Finder, o nosotros ahora) las junta con
// la entrada 8.3 que las sigue para reconstruir el nombre completo.
// Las entradas se escriben en orden INVERSO (la que lleva los ULTIMOS
// caracteres va primero, marcada con el bit 0x40 en su numero de
// secuencia) justo antes de la entrada 8.3, que sigue existiendo
// siempre como nombre corto de compatibilidad.

// Suma de comprobacion del nombre corto 8.3 -- cada entrada LFN la
// lleva, para que un lector pueda confirmar que de verdad pertenecen
// a la entrada 8.3 que las sigue (y no a una entrada borrada a medias).
static uint8_t lfn_checksum(const uint8_t short_name11[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name11[i]);
    }
    return sum;
}

// Posiciones (offset en bytes) de los 13 caracteres UTF-16 dentro de
// una entrada LFN: 5 en "Name1", 6 en "Name2", 2 en "Name3".
static const int LFN_CHAR_OFFSETS[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};

// Extrae hasta 13 caracteres de una entrada LFN en dst (aproximando a
// ASCII cualquier caracter fuera de ese rango con '?'). Devuelve
// cuantos escribio antes de encontrar el terminador nulo (o 13 si no
// lo encontro dentro de esta entrada -- el nombre sigue en la
// siguiente entrada de menor numero de secuencia).
static int lfn_entry_chars(const uint8_t *e, char *dst) {
    for (int i = 0; i < 13; i++) {
        uint16_t lo = e[LFN_CHAR_OFFSETS[i]];
        uint16_t hi = e[LFN_CHAR_OFFSETS[i] + 1];
        if (lo == 0 && hi == 0) return i; // terminador nulo
        dst[i] = (hi == 0) ? (char)lo : '?';
    }
    return 13;
}

// Escribe hasta 13 caracteres (desde src+start) en una entrada LFN,
// como UTF-16 (simplemente byte + 0x00, ya que solo manejamos ASCII).
// Si el nombre termina antes de llenar los 13, rellena con el
// terminador nulo y luego 0xFFFF (relleno estandar de VFAT).
static void lfn_entry_write_chars(uint8_t *e, const char *src, int start, int total_len) {
    for (int i = 0; i < 13; i++) {
        int pos = start + i;
        int off = LFN_CHAR_OFFSETS[i];
        if (pos < total_len) {
            e[off] = (uint8_t)src[pos];
            e[off + 1] = 0;
        } else if (pos == total_len) {
            e[off] = 0; e[off + 1] = 0; // terminador
        } else {
            e[off] = 0xFF; e[off + 1] = 0xFF; // relleno
        }
    }
}

// ¿Cabe 'name' en un 8.3 puro (base <=8, extension <=3), sin perder
// nada al convertirlo? Si es asi, no hace falta gastar entradas LFN.
static bool fits_pure_83(const char *name) {
    int i = 0, base_len = 0;
    while (name[i] && name[i] != '.') { base_len++; i++; }
    if (base_len == 0 || base_len > 8) return false;
    if (name[i] == '.') {
        i++;
        int ext_len = 0;
        while (name[i]) { ext_len++; i++; }
        if (ext_len > 3) return false;
    }
    return true;
}

// Genera un nombre corto 8.3 razonable para un nombre largo que no
// cabe puro ("sprite.nimg" -> "SPRITE~1.NIM"), probando sufijos
// numericos crecientes hasta encontrar uno que no colisione con un
// archivo ya existente.
static void generate_short_name(const char *long_name, uint8_t out11[11]) {
    char base[7]; int bn = 0;
    char ext[4]; int en = 0;

    int i = 0;
    while (long_name[i] && long_name[i] != '.' && bn < 6) {
        char c = long_name[i];
        if (c == ' ') { i++; continue; }
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        base[bn++] = c;
        i++;
    }
    while (long_name[i] && long_name[i] != '.') i++;
    if (long_name[i] == '.') {
        i++;
        while (long_name[i] && en < 3) {
            char c = long_name[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            ext[en++] = c;
            i++;
        }
    }

    for (int suffix = 1; suffix <= 999; suffix++) {
        char numbuf[4]; int nlen = 0;
        { int s = suffix, tn = 0; char tmp[4];
          if (s == 0) { tmp[tn++] = '0'; }
          while (s > 0) { tmp[tn++] = (char)('0' + s % 10); s /= 10; }
          for (int k = tn - 1; k >= 0; k--) numbuf[nlen++] = tmp[k];
        }

        int max_base = 8 - 1 - nlen; // sitio para '~' + numero
        if (max_base > 6) max_base = 6;
        int use_base = bn > max_base ? max_base : bn;

        for (int k = 0; k < 11; k++) out11[k] = ' ';
        for (int k = 0; k < use_base; k++) out11[k] = (uint8_t)base[k];
        out11[use_base] = '~';
        for (int k = 0; k < nlen; k++) out11[use_base + 1 + k] = (uint8_t)numbuf[k];
        for (int k = 0; k < en; k++) out11[8 + k] = (uint8_t)ext[k];

        char candidate[13];
        from_fat_83(out11, candidate);
        fat_dirent_t existing;
        if (!fat_find_root(candidate, &existing)) return; // libre, nos lo quedamos
    }
    // No deberia pasar nunca en la practica (999 colisiones seguidas)
    // -- nos quedamos con el ultimo intento igualmente.
}

// ---- tabla FAT: leer/escribir la entrada de un cluster ----

static uint32_t get_fat_entry(uint32_t cluster) {
    static uint8_t buf[SECTOR_SIZE];
    if (is_fat32) {
        uint32_t fat_offset = cluster * 4;
        uint32_t sector = first_fat_sector + (fat_offset / bytes_per_sector);
        uint32_t offset = fat_offset % bytes_per_sector;
        if (!read_sector(sector, buf)) return EOC_MARK_32;
        return read32(buf, offset) & 0x0FFFFFFF;
    } else {
        uint32_t fat_offset = cluster * 2;
        uint32_t sector = first_fat_sector + (fat_offset / bytes_per_sector);
        uint32_t offset = fat_offset % bytes_per_sector;
        if (!read_sector(sector, buf)) return EOC_MARK_16;
        return read16(buf, offset);
    }
}

static void set_fat_entry(uint32_t cluster, uint32_t value) {
    static uint8_t buf[SECTOR_SIZE];
    if (is_fat32) {
        uint32_t fat_offset = cluster * 4;
        uint32_t sector = first_fat_sector + (fat_offset / bytes_per_sector);
        uint32_t offset = fat_offset % bytes_per_sector;
        if (!read_sector(sector, buf)) return;
        uint32_t existing = read32(buf, offset);
        uint32_t new_value = (existing & 0xF0000000) | (value & 0x0FFFFFFF);
        write32(buf, offset, new_value);
        // FAT tiene 'num_fats' copias identicas -- las actualizamos todas
        for (uint32_t f = 0; f < num_fats; f++) {
            write_sector(sector + f * fat_size, buf);
        }
    } else {
        uint32_t fat_offset = cluster * 2;
        uint32_t sector = first_fat_sector + (fat_offset / bytes_per_sector);
        uint32_t offset = fat_offset % bytes_per_sector;
        if (!read_sector(sector, buf)) return;
        write16(buf, offset, (uint16_t)value);
        for (uint32_t f = 0; f < num_fats; f++) {
            write_sector(sector + f * fat_size, buf);
        }
    }
}

static bool is_eoc(uint32_t entry) {
    return is_fat32 ? (entry >= 0x0FFFFFF8) : (entry >= 0xFFF8);
}

static uint32_t alloc_cluster(void) {
    for (uint32_t c = 2; c < count_of_clusters + 2; c++) {
        if (get_fat_entry(c) == FREE_CLUSTER) {
            set_fat_entry(c, is_fat32 ? EOC_MARK_32 : EOC_MARK_16);
            return c;
        }
    }
    return 0; // disco lleno
}

// Libera TODA la cadena de clusters de un archivo -- la usan
// fat_delete_file y fat_write_file (al sobrescribir) para no dejar
// clusters huerfanos ocupados sin ningun archivo que los reclame.
//
// LIMITE DE SEGURIDAD REAL, EN TODOS LOS BUCLES DE CADENA DE CLUSTERS
// DE ESTE ARCHIVO: si la tabla FAT estuviera corrupta (por ejemplo,
// un ciclo -- el cluster A apunta al B, el B apunta de vuelta al A),
// un bucle que solo comprueba "fin de cadena o cero" nunca terminaria
// -- el sistema se quedaria colgado ("como bloqueado", exactamente el
// sintoma real reportado). Ningun archivo/carpeta VALIDO puede tener
// una cadena mas larga que el numero total de clusters del disco (no
// puede repetir un cluster sin ser un ciclo), asi que ese numero
// (con margen) es un limite SEGURO y con base real, no un numero
// magico arbitrario.
static void free_cluster_chain(uint32_t first_cluster) {
    uint32_t cluster = first_cluster;
    uint32_t guard = count_of_clusters + 16;
    while (!is_eoc(cluster) && cluster != 0 && guard-- > 0) {
        uint32_t next = get_fat_entry(cluster);
        set_fat_entry(cluster, FREE_CLUSTER);
        cluster = next;
    }
}

static uint32_t cluster_to_sector(uint32_t cluster) {
    return first_data_sector + (cluster - 2) * sectors_per_cluster;
}

bool fat_format(uint8_t disk_index);

// ---- montaje y formateo ----

bool fat_mount(uint8_t disk_index) {
    used_disk = disk_index;

    static uint8_t buf[SECTOR_SIZE];
    if (!read_sector(0, buf)) {
        uart_puts("fat: no se pudo leer el sector de arranque\n");
        return false;
    }

    // Firma obligatoria de sector de arranque valido
    if (buf[510] != 0x55 || buf[511] != 0xAA) {
        uart_puts("fat: firma de arranque no encontrada, formateando...\n");
        if (!fat_format(disk_index)) return false;
        return fat_mount(disk_index); // reintentamos montar tras formatear
    }

    bytes_per_sector = read16(buf, 0x0B);
    sectors_per_cluster = buf[0x0D];
    reserved_sector_count = read16(buf, 0x0E);
    num_fats = buf[0x10];
    root_entry_count = read16(buf, 0x11);

    uint32_t total_sectors_16 = read16(buf, 0x13);
    uint32_t fat_size_16 = read16(buf, 0x16);
    uint32_t total_sectors_32 = read32(buf, 0x20);
    uint32_t fat_size_32 = read32(buf, 0x24);

    total_sectors = (total_sectors_16 != 0) ? total_sectors_16 : total_sectors_32;
    fat_size = (fat_size_16 != 0) ? fat_size_16 : fat_size_32;

    root_dir_sector_count = ((root_entry_count * DIR_ENTRY_SIZE) + (bytes_per_sector - 1)) / bytes_per_sector;
    first_fat_sector = reserved_sector_count;
    root_dir_start_sector = first_fat_sector + (num_fats * fat_size);
    first_data_sector = root_dir_start_sector + root_dir_sector_count;

    uint32_t data_sectors = total_sectors - first_data_sector;
    count_of_clusters = data_sectors / sectors_per_cluster;

    if (count_of_clusters < 4085) {
        uart_puts("fat: FAT12 detectado, no soportado en esta version\n");
        return false;
    }
    is_fat32 = (count_of_clusters >= 65525);

    if (is_fat32) {
        root_cluster = read32(buf, 0x2C);
    }

    uart_puts("fat: montado (");
    uart_puts(is_fat32 ? "FAT32" : "FAT16");
    uart_puts("), disco ");
    if (disk_index == 0) uart_putc('0'); else uart_putc('1');
    uart_puts("\n");

    return true;
}

bool fat_format(uint8_t disk_index) {
    used_disk = disk_index;

    uint64_t total = disk_capacity_sectors_n(disk_index);
    if (total == 0) {
        uart_puts("fat: no se pudo leer la capacidad del disco\n");
        return false;
    }

    bytes_per_sector = SECTOR_SIZE;
    sectors_per_cluster = 1; // clusters pequeños -- necesario para superar
                              // el umbral de FAT32 (>=65525 clusters) en
                              // discos pequeños como el nuestro
    reserved_sector_count = 32;
    num_fats = 2;
    total_sectors = (uint32_t)total;

    // El tamaño de la FAT depende de cuantos clusters hay, pero cuantos
    // clusters hay depende del tamaño de la FAT (le quita espacio al
    // area de datos). Convergemos con unas pocas iteraciones -- es
    // mucho mas simple que resolver la ecuacion algebraicamente y
    // funciona perfectamente para nuestro proposito.
    fat_size = 1;
    for (int iter = 0; iter < 10; iter++) {
        uint32_t data_sectors = total_sectors - reserved_sector_count - (num_fats * fat_size);
        uint32_t clusters = data_sectors / sectors_per_cluster;
        uint32_t needed = ((clusters + 2) * 4 + bytes_per_sector - 1) / bytes_per_sector;
        if (needed == fat_size) break;
        fat_size = needed;
    }

    first_fat_sector = reserved_sector_count;
    first_data_sector = first_fat_sector + (num_fats * fat_size);
    count_of_clusters = (total_sectors - first_data_sector) / sectors_per_cluster;
    is_fat32 = true; // nuestro formateador solo crea FAT32
    root_cluster = 2;

    // -- Escribimos el sector de arranque (BPB) --
    static uint8_t buf[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) buf[i] = 0;

    buf[0] = 0xEB; buf[1] = 0x58; buf[2] = 0x90; // salto (no ejecutamos codigo de arranque real)
    const char *oem = "NEMOOS  ";
    for (int i = 0; i < 8; i++) buf[3 + i] = (uint8_t)oem[i];

    write16(buf, 0x0B, (uint16_t)bytes_per_sector);
    buf[0x0D] = (uint8_t)sectors_per_cluster;
    write16(buf, 0x0E, (uint16_t)reserved_sector_count);
    buf[0x10] = (uint8_t)num_fats;
    write16(buf, 0x11, 0);      // root_entry_count = 0 en FAT32
    write16(buf, 0x13, 0);      // total_sectors_16 = 0 (usamos el campo de 32 bits)
    buf[0x15] = 0xF8;           // media descriptor: disco "fijo"
    write16(buf, 0x16, 0);      // fat_size_16 = 0 en FAT32
    write16(buf, 0x18, 32);     // sectores por pista (valor convencional)
    write16(buf, 0x1A, 8);      // numero de cabezas (valor convencional)
    write32(buf, 0x1C, 0);      // sectores ocultos
    write32(buf, 0x20, total_sectors);

    write32(buf, 0x24, fat_size);   // fat_size_32
    write16(buf, 0x28, 0);          // ext_flags
    write16(buf, 0x2A, 0);          // fs_version
    write32(buf, 0x2C, root_cluster);
    write16(buf, 0x30, 1);          // FSInfo esta en el sector 1
    write16(buf, 0x32, 6);          // copia de respaldo del boot sector, sector 6
    buf[0x40] = 0x80;               // drive number (0x80 = disco "duro")
    buf[0x42] = 0x29;               // boot signature (indica que hay volume_id/label/fstype validos)
    write32(buf, 0x43, 0x4E454D4F); // volume id (cualquier valor sirve, usamos "NEMO")
    const char *label = "NEMO SHARE ";
    for (int i = 0; i < 11; i++) buf[0x47 + i] = (uint8_t)label[i];
    const char *fstype = "FAT32   ";
    for (int i = 0; i < 8; i++) buf[0x52 + i] = (uint8_t)fstype[i];

    buf[510] = 0x55;
    buf[511] = 0xAA;

    if (!write_sector(0, buf)) return false;

    // macOS es bastante mas estricto que nuestro propio driver a la
    // hora de validar un FAT32 antes de montarlo. Dos piezas que nuestro
    // driver no necesita pero macOS sí comprueba:

    // 1) El sector FSInfo (indicado por el campo en 0x30, que pusimos a
    // 1). Debe tener firmas concretas -- si esta a ceros, algunos
    // sistemas rechazan el volumen por "inconsistente".
    static uint8_t fsinfo[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) fsinfo[i] = 0;
    write32(fsinfo, 0x000, 0x41615252);   // LeadSig
    write32(fsinfo, 0x1E4, 0x61417272);   // StrucSig
    write32(fsinfo, 0x1E8, 0xFFFFFFFF);   // Free_Count: "desconocido"
    write32(fsinfo, 0x1EC, 0xFFFFFFFF);   // Nxt_Free: "desconocido"
    fsinfo[0x1FE] = 0x55;
    fsinfo[0x1FF] = 0xAA;
    if (!write_sector(1, fsinfo)) return false;

    // 2) La copia de respaldo del sector de arranque (indicada por el
    // campo en 0x32, que pusimos a 6) -- debe ser identica al sector 0.
    if (!write_sector(6, buf)) return false;

    // -- Vaciamos ambas copias de la tabla FAT, con las entradas
    // reservadas 0 y 1 marcadas segun la convencion FAT32 --
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) buf[i] = 0;
    write32(buf, 0, 0x0FFFFFF8); // cluster 0: copia del media descriptor
    write32(buf, 4, 0x0FFFFFFF); // cluster 1: reservado, marcado EOC
    for (uint32_t f = 0; f < num_fats; f++) {
        if (!write_sector(first_fat_sector + f * fat_size, buf)) return false;
    }
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) buf[i] = 0;
    for (uint32_t f = 0; f < num_fats; f++) {
        for (uint32_t s = 1; s < fat_size; s++) {
            if (!write_sector(first_fat_sector + f * fat_size + s, buf)) return false;
        }
    }

    // El cluster 2 es la carpeta raiz -- lo marcamos como EOC (un solo
    // cluster, vacio) y lo dejamos a ceros en disco (un directorio
    // vacio se representa con el primer byte de la primera entrada a 0)
    set_fat_entry(root_cluster, EOC_MARK_32);
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) buf[i] = 0;
    uint32_t root_sector = cluster_to_sector(root_cluster);
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (!write_sector(root_sector + s, buf)) return false;
    }

    uart_puts("fat: disco formateado como FAT32 correctamente.\n");
    return true;
}

// ---- directorio raiz: listar / buscar ----

// Version GENERAL: lista el contenido de CUALQUIER directorio dado su
// primer cluster -- 'cluster == 0' es un valor centinela especial que
// significa "la region fija del directorio raiz de FAT16" (en FAT16,
// SOLO la raiz vive en una region de tamaño fijo fuera del area de
// datos normal; TODOS los demas directorios -- incluida la raiz de
// FAT32, y CUALQUIER subcarpeta en FAT16 o FAT32 -- son cadenas de
// cluster normales, exactamente igual que un archivo). Un cluster 0
// nunca es un numero de cluster real valido (0 y 1 estan siempre
// reservados por la especificacion FAT), asi que este valor centinela
// nunca colisiona con una carpeta de verdad.
uint32_t fat_list_dir(uint32_t cluster, fat_dirent_t *out, uint32_t max_count) {
    uint32_t found = 0;
    static uint8_t buf[SECTOR_SIZE];

    // Acumulador del nombre largo en curso -- las entradas LFN
    // aparecen ANTES de su entrada 8.3, en orden inverso de
    // secuencia, asi que vamos rellenando este buffer por posicion
    // (no por orden de llegada) segun el numero de secuencia de cada
    // entrada, y lo consumimos en cuanto llega la entrada 8.3 que
    // sigue -- siempre que su checksum coincida con el que llevaban
    // las entradas LFN (si no coincide, algo esta corrupto o a
    // medias, y usamos el nombre corto como respaldo).
    char lfn_buf[256];
    bool lfn_pending = false;
    uint8_t lfn_checksum_seen = 0;

    // BUG REAL CORREGIDO: 'cluster == 0' es nuestro valor centinela
    // para "la raiz", pero la raiz se representa de forma DISTINTA
    // segun el formato -- region fija en FAT16, cluster normal
    // (root_cluster) en FAT32. Antes esta funcion asumia SIEMPRE la
    // region fija de FAT16 al ver un 0, sin comprobar 'is_fat32' --
    // en un disco FAT32 (el caso mas comun hoy en dia), esto leia
    // variables que nunca se rellenan para FAT32 (siempre a cero),
    // devolviendo la raiz VACIA aunque tuviera archivos de verdad.
    if (cluster == 0 && is_fat32) {
        cluster = root_cluster; // seguimos abajo, tratandolo como una cadena de cluster normal
    }

    if (cluster == 0) {
        for (uint32_t s = 0; s < root_dir_sector_count; s++) {
            if (!read_sector(root_dir_start_sector + s, buf)) break;
            for (uint32_t e = 0; e < bytes_per_sector / DIR_ENTRY_SIZE; e++) {
                uint8_t *entry = &buf[e * DIR_ENTRY_SIZE];
                if (entry[0] == 0x00) return found;
                if (entry[0] == 0xE5) { lfn_pending = false; continue; }
                if (entry[11] == ATTR_LONG_NAME) {
                    uint8_t seq = entry[0] & 0x1F;
                    if (seq >= 1 && seq <= 19) {
                        int base = (seq - 1) * 13;
                        char chars[13];
                        int n = lfn_entry_chars(entry, chars);
                        for (int k = 0; k < n && base + k < 255; k++) lfn_buf[base + k] = chars[k];
                        if (n < 13 && base + n < 256) lfn_buf[base + n] = '\0';
                        lfn_checksum_seen = entry[13];
                        lfn_pending = true;
                    }
                    continue;
                }
                if (entry[11] & ATTR_VOLUME_ID) { lfn_pending = false; continue; }

                if (found < max_count) {
                    if (lfn_pending && lfn_checksum_seen == lfn_checksum(entry)) {
                        int k = 0;
                        while (lfn_buf[k] != '\0' && k < FAT_NAME_LEN - 1) { out[found].name[k] = lfn_buf[k]; k++; }
                        out[found].name[k] = '\0';
                    } else {
                        from_fat_83(entry, out[found].name);
                    }
                    out[found].is_dir = (entry[11] & ATTR_DIRECTORY) != 0;
                    out[found].size = read32(entry, 28);
                    uint32_t hi = read16(entry, 20);
                    uint32_t lo = read16(entry, 26);
                    out[found].first_cluster = (hi << 16) | lo;
                }
                lfn_pending = false;
                found++;
            }
        }
        return found;
    }

    // Cadena de clusters normal -- vale tanto para la raiz de FAT32
    // como para CUALQUIER subcarpeta en FAT16 o FAT32. Cada
    // subcarpeta incluye ADEMAS dos entradas especiales al principio,
    // "." (ella misma) y ".." (su padre) -- las omitimos del listado
    // (el explorador ya lleva su propia pila de directorios para
    // "subir", no necesita verlas como archivos navegables).
    uint32_t c = cluster;
    uint32_t guard = count_of_clusters + 16;
    while (!is_eoc(c) && c != 0 && guard-- > 0) {
        uint32_t sector = cluster_to_sector(c);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!read_sector(sector + s, buf)) return found;
            for (uint32_t e = 0; e < bytes_per_sector / DIR_ENTRY_SIZE; e++) {
                uint8_t *entry = &buf[e * DIR_ENTRY_SIZE];
                if (entry[0] == 0x00) return found;
                if (entry[0] == 0xE5) { lfn_pending = false; continue; }
                if (entry[0] == '.') { lfn_pending = false; continue; } // "." y ".."
                if (entry[11] == ATTR_LONG_NAME) {
                    uint8_t seq = entry[0] & 0x1F;
                    if (seq >= 1 && seq <= 19) {
                        int base = (seq - 1) * 13;
                        char chars[13];
                        int n = lfn_entry_chars(entry, chars);
                        for (int k = 0; k < n && base + k < 255; k++) lfn_buf[base + k] = chars[k];
                        if (n < 13 && base + n < 256) lfn_buf[base + n] = '\0';
                        lfn_checksum_seen = entry[13];
                        lfn_pending = true;
                    }
                    continue;
                }
                if (entry[11] & ATTR_VOLUME_ID) { lfn_pending = false; continue; }

                if (found < max_count) {
                    if (lfn_pending && lfn_checksum_seen == lfn_checksum(entry)) {
                        int k = 0;
                        while (lfn_buf[k] != '\0' && k < FAT_NAME_LEN - 1) { out[found].name[k] = lfn_buf[k]; k++; }
                        out[found].name[k] = '\0';
                    } else {
                        from_fat_83(entry, out[found].name);
                    }
                    out[found].is_dir = (entry[11] & ATTR_DIRECTORY) != 0;
                    out[found].size = read32(entry, 28);
                    uint32_t hi = read16(entry, 20);
                    uint32_t lo = read16(entry, 26);
                    out[found].first_cluster = (hi << 16) | lo;
                }
                lfn_pending = false;
                found++;
            }
        }
        c = get_fat_entry(c);
    }
    return found;
}

uint32_t fat_list_root(fat_dirent_t *out, uint32_t max_count) {
    return fat_list_dir(is_fat32 ? root_cluster : 0, out, max_count);
}

bool fat_find_in_dir(uint32_t cluster, const char *name, fat_dirent_t *out) {
    static fat_dirent_t entries[128];
    uint32_t count = fat_list_dir(cluster, entries, 128);
    for (uint32_t i = 0; i < count && i < 128; i++) {
        if (name_eq(entries[i].name, name)) {
            *out = entries[i];
            return true;
        }
    }
    return false;
}

bool fat_find_root(const char *name, fat_dirent_t *out) {
    return fat_find_in_dir(is_fat32 ? root_cluster : 0, name, out);
}

// ---- lectura y escritura de archivos ----

bool fat_read_file(const fat_dirent_t *entry, void *buf, uint32_t max_size, uint32_t *out_size) {
    if (entry->is_dir) return false;

    uint32_t to_read = entry->size;
    if (to_read > max_size) to_read = max_size;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t bytes_done = 0;
    uint32_t cluster = entry->first_cluster;
    static uint8_t sector_buf[SECTOR_SIZE];
    uint32_t guard = count_of_clusters + 16;

    while (!is_eoc(cluster) && cluster != 0 && bytes_done < to_read && guard-- > 0) {
        uint32_t sector = cluster_to_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster && bytes_done < to_read; s++) {
            if (!read_sector(sector + s, sector_buf)) {
                *out_size = bytes_done;
                return false;
            }
            uint32_t chunk = to_read - bytes_done;
            if (chunk > bytes_per_sector) chunk = bytes_per_sector;
            for (uint32_t i = 0; i < chunk; i++) {
                dst[bytes_done + i] = sector_buf[i];
            }
            bytes_done += chunk;
        }
        cluster = get_fat_entry(cluster);
    }

    *out_size = bytes_done;
    return true;
}

// Busca 'count' huecos LIBRES y CONSECUTIVOS en el directorio raiz
// (entradas borradas o nunca usadas) -- una entrada 8.3 normal solo
// necesita 1, pero un nombre largo necesita 1 + tantas entradas LFN
// como haga falta, y TIENEN que quedar pegadas justo antes de la
// entrada 8.3 (asi es como un lector VFAT de verdad, como el Finder,
// las reconoce como pertenecientes al mismo archivo). Si el
// directorio esta lleno y es FAT32, le añade un cluster nuevo -- en
// ese caso, cualquier racha a medias en el cluster viejo se descarta
// y se usa el cluster nuevo entero (siempre tiene sitio de sobra
// para cualquier nombre razonable).
static bool find_free_dir_slots(uint32_t count, uint32_t *out_sector, uint32_t *out_offset) {
    static uint8_t buf[SECTOR_SIZE];
    uint32_t run_sector = 0, run_offset = 0, run_len = 0;

    if (!is_fat32) {
        for (uint32_t s = 0; s < root_dir_sector_count; s++) {
            if (!read_sector(root_dir_start_sector + s, buf)) return false;
            for (uint32_t e = 0; e < bytes_per_sector / DIR_ENTRY_SIZE; e++) {
                uint8_t *entry = &buf[e * DIR_ENTRY_SIZE];
                bool free_slot = (entry[0] == 0x00 || entry[0] == 0xE5);
                if (free_slot) {
                    if (run_len == 0) { run_sector = root_dir_start_sector + s; run_offset = e * DIR_ENTRY_SIZE; }
                    run_len++;
                    if (run_len >= count) { *out_sector = run_sector; *out_offset = run_offset; return true; }
                } else {
                    run_len = 0;
                }
            }
        }
        return false; // directorio raiz FAT16 lleno (area de tamaño fijo, no crece)
    }

    uint32_t cluster = root_cluster;
    uint32_t last_cluster = cluster;
    uint32_t guard = count_of_clusters + 16;
    while (!is_eoc(cluster) && cluster != 0 && guard-- > 0) {
        uint32_t sector = cluster_to_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!read_sector(sector + s, buf)) return false;
            for (uint32_t e = 0; e < bytes_per_sector / DIR_ENTRY_SIZE; e++) {
                uint8_t *entry = &buf[e * DIR_ENTRY_SIZE];
                bool free_slot = (entry[0] == 0x00 || entry[0] == 0xE5);
                if (free_slot) {
                    if (run_len == 0) { run_sector = sector + s; run_offset = e * DIR_ENTRY_SIZE; }
                    run_len++;
                    if (run_len >= count) { *out_sector = run_sector; *out_offset = run_offset; return true; }
                } else {
                    run_len = 0;
                }
            }
        }
        last_cluster = cluster;
        cluster = get_fat_entry(cluster);
    }

    // No habia suficientes huecos seguidos: extendemos la carpeta con
    // un cluster nuevo, y usamos ese entero (descartando cualquier
    // racha a medias en el cluster viejo -- un hueco a caballo entre
    // dos clusters no seria de verdad contiguo en disco).
    uint32_t new_cluster = alloc_cluster();
    if (new_cluster == 0) return false;
    set_fat_entry(last_cluster, new_cluster);

    for (uint32_t i = 0; i < SECTOR_SIZE; i++) buf[i] = 0;
    uint32_t sector = cluster_to_sector(new_cluster);
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (!write_sector(sector + s, buf)) return false;
    }

    *out_sector = sector;
    *out_offset = 0;
    return true;
}

// Escribe las entradas de directorio de un archivo (las LFN que
// hagan falta, seguidas de la 8.3 de verdad) en el primer hueco
// contiguo que encuentre. Si 'name' cabe puro en 8.3, no gasta
// ninguna entrada LFN -- se comporta exactamente igual que antes
// para esos casos.
static bool write_directory_entry(const char *name, uint32_t first_cluster, uint32_t size) {
    uint8_t name83[11];
    bool need_lfn = !fits_pure_83(name);

    if (need_lfn) generate_short_name(name, name83);
    else to_fat_83(name, name83);

    int name_len = 0;
    while (name[name_len]) name_len++;

    uint32_t num_lfn = need_lfn ? (uint32_t)((name_len + 12) / 13) : 0;
    uint32_t needed_slots = num_lfn + 1;

    uint32_t dir_sector, dir_offset;
    if (!find_free_dir_slots(needed_slots, &dir_sector, &dir_offset)) return false;

    uint32_t entries_per_sector = bytes_per_sector / DIR_ENTRY_SIZE;
    uint32_t cur_sector = dir_sector;
    uint32_t cur_entry_idx = dir_offset / DIR_ENTRY_SIZE;
    static uint8_t sbuf[SECTOR_SIZE];

    if (need_lfn) {
        uint8_t checksum = lfn_checksum(name83);
        for (uint32_t i = 0; i < num_lfn; i++) {
            uint32_t seq = num_lfn - i; // la primera que escribimos lleva los ULTIMOS caracteres
            uint8_t seq_byte = (uint8_t)seq;
            if (i == 0) seq_byte |= 0x40; // marca la entrada "logicamente ultima" de la secuencia

            if (!read_sector(cur_sector, sbuf)) return false;
            uint8_t *entry = &sbuf[cur_entry_idx * DIR_ENTRY_SIZE];
            entry[0] = seq_byte;
            lfn_entry_write_chars(entry, name, (int)((seq - 1) * 13), name_len);
            entry[11] = ATTR_LONG_NAME;
            entry[12] = 0;
            entry[13] = checksum;
            entry[26] = 0; entry[27] = 0;
            if (!write_sector(cur_sector, sbuf)) return false;

            cur_entry_idx++;
            if (cur_entry_idx >= entries_per_sector) { cur_entry_idx = 0; cur_sector++; }
        }
    }

    if (!read_sector(cur_sector, sbuf)) return false;
    uint8_t *entry = &sbuf[cur_entry_idx * DIR_ENTRY_SIZE];
    for (int i = 0; i < 11; i++) entry[i] = name83[i];
    entry[11] = 0x20; // atributo: archivo normal (ARCHIVE)
    for (int i = 12; i < 26; i++) entry[i] = 0; // timestamps a cero, simplificacion de v1
    write16(entry, 20, (uint16_t)(first_cluster >> 16));
    write16(entry, 26, (uint16_t)(first_cluster & 0xFFFF));
    write32(entry, 28, size);
    return write_sector(cur_sector, sbuf);
}

bool fat_create_file(const char *name, const void *buf, uint32_t size) {
    fat_dirent_t existing;
    if (fat_find_root(name, &existing)) {
        return false; // ya existe -- usa fat_write_file para sobrescribir
    }

    uint32_t bytes_per_cluster = bytes_per_sector * sectors_per_cluster;
    uint32_t clusters_needed = (size + bytes_per_cluster - 1) / bytes_per_cluster;
    if (clusters_needed == 0) clusters_needed = 1; // hasta un archivo vacio ocupa 1 cluster

    uint32_t first_cluster = 0;
    uint32_t prev_cluster = 0;
    const uint8_t *src = (const uint8_t *)buf;
    static uint8_t sector_buf[SECTOR_SIZE];
    uint32_t bytes_written = 0;

    for (uint32_t c = 0; c < clusters_needed; c++) {
        uint32_t new_cluster = alloc_cluster();
        if (new_cluster == 0) return false; // disco lleno

        if (first_cluster == 0) {
            first_cluster = new_cluster;
        } else {
            set_fat_entry(prev_cluster, new_cluster);
        }
        prev_cluster = new_cluster;

        uint32_t sector = cluster_to_sector(new_cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            for (uint32_t i = 0; i < SECTOR_SIZE; i++) sector_buf[i] = 0;
            uint32_t remaining = size - bytes_written;
            uint32_t chunk = remaining < bytes_per_sector ? remaining : bytes_per_sector;
            for (uint32_t i = 0; i < chunk; i++) {
                sector_buf[i] = src[bytes_written + i];
            }
            if (!write_sector(sector + s, sector_buf)) return false;
            bytes_written += chunk;
        }
    }

    return write_directory_entry(name, first_cluster, size);
}

// Borra un archivo (y sus entradas LFN precedentes, si las tenia) y
// libera su cadena de clusters. Recorre el directorio con la MISMA
// logica de acumulacion de nombre largo que fat_list_root, pero
// ademas recuerda DONDE EN DISCO vive cada entrada LFN pendiente, por
// si resultan pertenecer al archivo que estamos borrando.
bool fat_delete_file(const char *name) {
    static uint8_t buf[SECTOR_SIZE];
    char lfn_buf[256];
    bool lfn_pending = false;
    uint8_t lfn_checksum_seen = 0;

    #define MAX_PENDING_LFN 20
    uint32_t pend_sector[MAX_PENDING_LFN];
    uint32_t pend_offset[MAX_PENDING_LFN];
    int pend_count = 0;

    if (!is_fat32) {
        for (uint32_t s = 0; s < root_dir_sector_count; s++) {
            uint32_t sector = root_dir_start_sector + s;
            if (!read_sector(sector, buf)) return false;
            for (uint32_t e = 0; e < bytes_per_sector / DIR_ENTRY_SIZE; e++) {
                uint8_t *entry = &buf[e * DIR_ENTRY_SIZE];
                uint32_t off = e * DIR_ENTRY_SIZE;
                if (entry[0] == 0x00) return false;
                if (entry[0] == 0xE5) { lfn_pending = false; pend_count = 0; continue; }
                if (entry[11] == ATTR_LONG_NAME) {
                    uint8_t seq = entry[0] & 0x1F;
                    if (seq >= 1 && seq <= 19) {
                        int base = (seq - 1) * 13;
                        char chars[13];
                        int n = lfn_entry_chars(entry, chars);
                        for (int k = 0; k < n && base + k < 255; k++) lfn_buf[base + k] = chars[k];
                        if (n < 13 && base + n < 256) lfn_buf[base + n] = '\0';
                        lfn_checksum_seen = entry[13];
                        lfn_pending = true;
                        if (pend_count < MAX_PENDING_LFN) { pend_sector[pend_count] = sector; pend_offset[pend_count] = off; pend_count++; }
                    }
                    continue;
                }
                if (entry[11] & ATTR_VOLUME_ID) { lfn_pending = false; pend_count = 0; continue; }

                char resolved[FAT_NAME_LEN];
                if (lfn_pending && lfn_checksum_seen == lfn_checksum(entry)) {
                    int k = 0;
                    while (lfn_buf[k] != '\0' && k < FAT_NAME_LEN - 1) { resolved[k] = lfn_buf[k]; k++; }
                    resolved[k] = '\0';
                } else {
                    from_fat_83(entry, resolved);
                    pend_count = 0; // el LFN pendiente, si lo habia, no era suyo -- no lo tocamos
                }

                if (name_eq(resolved, name)) {
                    uint32_t hi = read16(entry, 20);
                    uint32_t lo = read16(entry, 26);
                    free_cluster_chain((hi << 16) | lo);
                    for (int p = 0; p < pend_count; p++) {
                        static uint8_t pbuf[SECTOR_SIZE];
                        if (read_sector(pend_sector[p], pbuf)) {
                            pbuf[pend_offset[p]] = 0xE5;
                            write_sector(pend_sector[p], pbuf);
                        }
                    }
                    entry[0] = 0xE5;
                    return write_sector(sector, buf);
                }

                lfn_pending = false;
                pend_count = 0;
            }
        }
        return false;
    }

    uint32_t cluster = root_cluster;
    uint32_t guard = count_of_clusters + 16;
    while (!is_eoc(cluster) && cluster != 0 && guard-- > 0) {
        uint32_t csector = cluster_to_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            uint32_t sector = csector + s;
            if (!read_sector(sector, buf)) return false;
            for (uint32_t e = 0; e < bytes_per_sector / DIR_ENTRY_SIZE; e++) {
                uint8_t *entry = &buf[e * DIR_ENTRY_SIZE];
                uint32_t off = e * DIR_ENTRY_SIZE;
                if (entry[0] == 0x00) return false;
                if (entry[0] == 0xE5) { lfn_pending = false; pend_count = 0; continue; }
                if (entry[11] == ATTR_LONG_NAME) {
                    uint8_t seq = entry[0] & 0x1F;
                    if (seq >= 1 && seq <= 19) {
                        int base = (seq - 1) * 13;
                        char chars[13];
                        int n = lfn_entry_chars(entry, chars);
                        for (int k = 0; k < n && base + k < 255; k++) lfn_buf[base + k] = chars[k];
                        if (n < 13 && base + n < 256) lfn_buf[base + n] = '\0';
                        lfn_checksum_seen = entry[13];
                        lfn_pending = true;
                        if (pend_count < MAX_PENDING_LFN) { pend_sector[pend_count] = sector; pend_offset[pend_count] = off; pend_count++; }
                    }
                    continue;
                }
                if (entry[11] & ATTR_VOLUME_ID) { lfn_pending = false; pend_count = 0; continue; }

                char resolved[FAT_NAME_LEN];
                if (lfn_pending && lfn_checksum_seen == lfn_checksum(entry)) {
                    int k = 0;
                    while (lfn_buf[k] != '\0' && k < FAT_NAME_LEN - 1) { resolved[k] = lfn_buf[k]; k++; }
                    resolved[k] = '\0';
                } else {
                    from_fat_83(entry, resolved);
                    pend_count = 0;
                }

                if (name_eq(resolved, name)) {
                    uint32_t hi = read16(entry, 20);
                    uint32_t lo = read16(entry, 26);
                    free_cluster_chain((hi << 16) | lo);
                    for (int p = 0; p < pend_count; p++) {
                        static uint8_t pbuf[SECTOR_SIZE];
                        if (read_sector(pend_sector[p], pbuf)) {
                            pbuf[pend_offset[p]] = 0xE5;
                            write_sector(pend_sector[p], pbuf);
                        }
                    }
                    entry[0] = 0xE5;
                    return write_sector(sector, buf);
                }

                lfn_pending = false;
                pend_count = 0;
            }
        }
        cluster = get_fat_entry(cluster);
    }
    return false;
}

// Sobrescribe un archivo: si ya existia, lo borra primero (libera su
// cadena de clusters vieja y sus entradas de directorio) y lo vuelve
// a crear limpio -- mas simple y menos propenso a bugs que intentar
// actualizarlo "en el sitio".
bool fat_write_file(const char *name, const void *buf, uint32_t size) {
    fat_dirent_t existing;
    if (fat_find_root(name, &existing)) {
        if (!fat_delete_file(name)) return false;
    }
    return fat_create_file(name, buf, size);
}
