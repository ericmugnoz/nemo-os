// nemofs_tool.c -- Nemo OS, herramienta de host
// Lee y escribe archivos en una imagen de disco NemoFS (el disk.img de
// QEMU) desde el ordenador de desarrollo, usando el MISMO nemofs.c del
// kernel -- asi lo que se escribe es exactamente lo que Nemo OS espera.
//
//   nemofs_tool disk.img ls [CARPETA]              lista (por defecto la raiz)
//   nemofs_tool disk.img put archivo [CARPETA]     copia un archivo del host al disco
//   nemofs_tool disk.img get archivo [CARPETA]     copia un archivo del disco al host (mismo nombre)
//   nemofs_tool disk.img mkdir NOMBRE [CARPETA]    crea una carpeta
//   nemofs_tool disk.img del archivo [CARPETA]     borra
//
// CARPETA es una ruta con '/' desde la raiz: PRUEBAS, DOCUMENTOS/NOTAS.
//
// Compilar (desde la raiz del proyecto):
//   gcc -O2 -w -Isrc -Iherramientas -o herramientas/nemofs_tool \
//       herramientas/nemofs_tool.c src/nemofs.c src/heap.c
//
// SOLO con QEMU parado: si QEMU tiene el disco montado a la vez, los dos
// escriben sobre la misma imagen y se corrompe. Si el disco es de una
// version anterior de NemoFS, montarlo aqui lo actualiza en el sitio
// igual que haria el kernel (es el mismo codigo).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "nemofs.h"

// ---- disco: un archivo del host ----
static FILE *img = 0;
static uint64_t sectores = 0;
bool disk_read_sector_n(uint8_t d, uint64_t s, void *buf) {
    (void)d; if (s >= sectores) return false;
    fseek(img, (long)(s * 512), SEEK_SET); return fread(buf, 1, 512, img) == 512;
}
bool disk_write_sector_n(uint8_t d, uint64_t s, const void *buf) {
    (void)d; if (s >= sectores) return false;
    fseek(img, (long)(s * 512), SEEK_SET); return fwrite(buf, 1, 512, img) == 512;
}
uint64_t disk_capacity_sectors_n(uint8_t d) { (void)d; return sectores; }
void uart_puts(const char *s) { fputs(s, stderr); }
void uart_putc(char c) { fputc(c, stderr); }

static int32_t resolver_carpeta(const char *ruta) {
    if (!ruta || !*ruta || strcmp(ruta, "/") == 0) return NEMOFS_ROOT_INODE;
    uint32_t actual = NEMOFS_ROOT_INODE;
    char copia[256]; strncpy(copia, ruta, 255); copia[255] = 0;
    for (char *p = strtok(copia, "/"); p; p = strtok(0, "/")) {
        int32_t hijo = nemofs_find_child(actual, p);
        if (hijo < 0) { fprintf(stderr, "no existe la carpeta: %s\n", p); return -1; }
        if (nemofs_type_by_inode((uint32_t)hijo) != NEMOFS_TYPE_DIR) { fprintf(stderr, "no es una carpeta: %s\n", p); return -1; }
        actual = (uint32_t)hijo;
    }
    return (int32_t)actual;
}

static const char *nombre_base(const char *ruta) { const char *b = strrchr(ruta, '/'); return b ? b + 1 : ruta; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: nemofs_tool disk.img ls|put|get|mkdir|del ...\n"); return 1; }
    img = fopen(argv[1], "r+b");
    if (!img) { perror(argv[1]); return 1; }
    fseek(img, 0, SEEK_END); sectores = (uint64_t)ftell(img) / 512; fseek(img, 0, SEEK_SET);
    if (!nemofs_mount()) { fprintf(stderr, "no se pudo montar %s\n", argv[1]); return 1; }

    const char *cmd = argv[2];
    if (strcmp(cmd, "ls") == 0) {
        int32_t dir = resolver_carpeta(argc > 3 ? argv[3] : 0); if (dir < 0) return 1;
        static nemofs_dirent_t ents[1536];
        uint32_t n = nemofs_list_dir((uint32_t)dir, ents, 1536);
        for (uint32_t i = 0; i < n && i < 1536; i++)
            printf("%s %8u  %s\n", ents[i].type == NEMOFS_TYPE_DIR ? "[DIR]" : "     ", ents[i].size, ents[i].name);
        printf("(%u entradas)\n", n);
    } else if (strcmp(cmd, "put") == 0) {
        if (argc < 4) { fprintf(stderr, "uso: put archivo [CARPETA]\n"); return 1; }
        int32_t dir = resolver_carpeta(argc > 4 ? argv[4] : 0); if (dir < 0) return 1;
        FILE *f = fopen(argv[3], "rb"); if (!f) { perror(argv[3]); return 1; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc((size_t)n + 1); fread(buf, 1, (size_t)n, f); fclose(f);
        const char *nombre = nombre_base(argv[3]);
        int32_t ino = nemofs_find_child((uint32_t)dir, nombre);
        if (ino < 0) ino = nemofs_create((uint32_t)dir, nombre, NEMOFS_TYPE_FILE);
        if (ino < 0) { fprintf(stderr, "no se pudo crear %s\n", nombre); return 1; }
        if (!nemofs_write_file((uint32_t)ino, buf, (uint32_t)n)) { fprintf(stderr, "fallo escribiendo %s (%ld bytes)\n", nombre, n); return 1; }
        printf("%s -> %s (%ld bytes)\n", argv[3], nombre, n);
    } else if (strcmp(cmd, "get") == 0) {
        if (argc < 4) { fprintf(stderr, "uso: get archivo [CARPETA]\n"); return 1; }
        int32_t dir = resolver_carpeta(argc > 4 ? argv[4] : 0); if (dir < 0) return 1;
        int32_t ino = nemofs_find_child((uint32_t)dir, argv[3]);
        if (ino < 0) { fprintf(stderr, "no existe: %s\n", argv[3]); return 1; }
        static uint8_t buf[8 * 1024 * 1024];
        int32_t n = nemofs_read_file((uint32_t)ino, buf, sizeof(buf));
        if (n < 0) { fprintf(stderr, "no se pudo leer\n"); return 1; }
        FILE *f = fopen(argv[3], "wb"); fwrite(buf, 1, (size_t)n, f); fclose(f);
        printf("%s (%d bytes)\n", argv[3], n);
    } else if (strcmp(cmd, "mkdir") == 0) {
        if (argc < 4) { fprintf(stderr, "uso: mkdir NOMBRE [CARPETA]\n"); return 1; }
        int32_t dir = resolver_carpeta(argc > 4 ? argv[4] : 0); if (dir < 0) return 1;
        if (nemofs_find_child((uint32_t)dir, argv[3]) >= 0) { printf("ya existe: %s\n", argv[3]); return 0; }
        if (nemofs_create((uint32_t)dir, argv[3], NEMOFS_TYPE_DIR) < 0) { fprintf(stderr, "no se pudo crear\n"); return 1; }
        printf("carpeta %s creada\n", argv[3]);
    } else if (strcmp(cmd, "del") == 0) {
        if (argc < 4) { fprintf(stderr, "uso: del archivo [CARPETA]\n"); return 1; }
        int32_t dir = resolver_carpeta(argc > 4 ? argv[4] : 0); if (dir < 0) return 1;
        if (!nemofs_delete((uint32_t)dir, argv[3])) { fprintf(stderr, "no se pudo borrar %s\n", argv[3]); return 1; }
        printf("%s borrado\n", argv[3]);
    } else { fprintf(stderr, "comando desconocido: %s\n", cmd); return 1; }
    fclose(img);
    return 0;
}
