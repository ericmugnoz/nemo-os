// Lee la particion de arranque de la imagen con el fat.c DE NEMO OS.
// El constructor y el verificador son los dos de Python y de la misma mano;
// esto es el otro lado: si el driver del sistema la monta y saca los
// archivos byte a byte, la Pi la va a poder arrancar.
//
//   gcc -O1 -w -Isrc -o /tmp/leer_imagen herramientas/leer_imagen_host.c src/fat.c
//   /tmp/leer_imagen imagen.img <sector_de_inicio>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "fat.h"

static FILE *img;
static uint64_t base_sector;     // donde empieza la particion dentro de la imagen
static uint64_t sectores;

bool disk_read_sector_n(uint8_t d, uint64_t s, void *buf) {
    (void)d;
    if (s >= sectores) return false;
    if (fseeko(img, (off_t)((base_sector + s) * 512), SEEK_SET) != 0) return false;
    memset(buf, 0, 512);
    fread(buf, 1, 512, img);      // un hueco del archivo disperso lee ceros
    return true;
}
bool disk_write_sector_n(uint8_t d, uint64_t s, const void *b) { (void)d;(void)s;(void)b; return false; }  // solo lectura
bool disk_read_sectors_n(uint8_t d, uint64_t s, uint32_t n, void *buf) {
    for (uint32_t i = 0; i < n; i++) if (!disk_read_sector_n(d, s+i, (uint8_t*)buf + i*512)) return false;
    return true;
}
uint64_t disk_capacity_sectors_n(uint8_t d) { (void)d; return sectores; }
bool disk_init(void) { return true; }
uint32_t disk_count(void) { return 1; }
void uart_putc(char c) { putchar(c); }
void uart_puts(const char *s) { fputs(s, stdout); }

int main(int argc, char **argv) {
    if (argc < 3) { printf("uso: %s imagen.img sector_inicio [sectores]\n", argv[0]); return 2; }
    img = fopen(argv[1], "rb");
    if (!img) { printf("no se pudo abrir %s\n", argv[1]); return 2; }
    base_sector = strtoull(argv[2], NULL, 10);
    sectores = (argc > 3) ? strtoull(argv[3], NULL, 10) : (1024ull * 1024 * 1024 / 512);

    printf("Montando la particion del sector %llu con el fat.c de Nemo OS:\n", (unsigned long long)base_sector);
    if (!fat_mount(0)) { printf("FALLA: el driver del sistema NO monta esta particion\n"); return 1; }

    fat_dirent_t ents[64];
    uint32_t n = fat_list_root(ents, 64);
    printf("\nraiz: %u entradas\n", n);
    int fallos = 0;
    for (uint32_t i = 0; i < n; i++) {
        printf("  %-32s %10u bytes%s\n", ents[i].name, ents[i].size, ents[i].is_dir ? "  (carpeta)" : "");
    }
    // leer kernel8.img entero por el driver y comparar con el original
    fat_dirent_t k;
    if (!fat_find_root("kernel8.img", &k)) {
        printf("\nFALLA: el driver no encuentra kernel8.img\n"); return 1;
    }
    printf("\nleyendo kernel8.img (%u bytes) con el driver...\n", k.size);
    uint8_t *buf = malloc(k.size);
    fat_cursor_t cur = {0, 0};
    uint32_t total = 0;
    while (total < k.size) {
        uint32_t trozo = k.size - total; if (trozo > 65536) trozo = 65536;
        uint32_t leidos = 0;
        if (!fat_read_at(&k, &cur, total, buf + total, trozo, &leidos) || leidos == 0) break;
        total += leidos;
    }
    printf("  leidos %u de %u bytes\n", total, k.size);
    if (total != k.size) { printf("FALLA: lectura incompleta\n"); fallos++; }

    FILE *orig = fopen(argv[4] ? argv[4] : "kernel8.img", "rb");
    if (!orig) { printf("  (sin el original para comparar)\n"); }
    else {
        fseeko(orig, 0, SEEK_END); long os = ftello(orig); fseeko(orig, 0, SEEK_SET);
        uint8_t *ob = malloc(os);
        fread(ob, 1, os, orig); fclose(orig);
        if ((long)total != os) { printf("FALLA: tamaños distintos (%u contra %ld)\n", total, os); fallos++; }
        else if (memcmp(buf, ob, total) != 0) { printf("FALLA: el contenido NO coincide\n"); fallos++; }
        else printf("  identico al original, byte a byte\n");
        free(ob);
    }
    free(buf);
    printf("\n%s\n", fallos ? "HAY FALLOS" : "El driver de Nemo OS lee la imagen correctamente");
    return fallos ? 1 : 0;
}
