// disk.h — Nemo OS
#ifndef DISK_H
#define DISK_H

#include <stdint.h>
#include <stdbool.h>

#define SECTOR_SIZE 512
#define MAX_DISKS 2

// Busca y configura TODOS los dispositivos virtio-blk que encuentre
// (hasta MAX_DISKS). Devuelve 'true' si encontró al menos uno.
bool disk_init(void);

uint32_t disk_count(void);

bool disk_read_sector_n(uint8_t disk, uint64_t sector, void *buf);
bool disk_write_sector_n(uint8_t disk, uint64_t sector, const void *buf);
uint64_t disk_capacity_sectors_n(uint8_t disk);

// Lee 'n' sectores SEGUIDOS de una vez. El que llama debe
// tener sitio para n*512 bytes y el destino alineado a 4. Existe porque
// en la Pi 4 cada lectura suelta es un comando completo a la tarjeta:
// pedir la tirada entera de golpe se lo ahorra. En QEMU es una peticion
// virtio mas larga, sin mas.
bool disk_read_sectors_n(uint8_t disk, uint64_t sector, uint32_t n, void *buf);

// El reparto real de la tarjeta, para el particionador:
//   indice 0..3 = entrada de la tabla de particiones (MBR)
// Devuelve false si esa entrada esta vacia o no hay tabla.
bool disk_particion(int indice, uint8_t *tipo, uint64_t *inicio, uint64_t *sectores);
// La capacidad FISICA de la tarjeta (no la de una particion)
uint64_t disk_capacidad_fisica(void);
// Donde vive NemoFS ahora mismo, y si sale de la tabla o del reparto de siempre
void disk_rango_nemofs(uint64_t *inicio, uint64_t *sectores, bool *de_tabla);

// Estirar la particion de NemoFS hasta el final de la tarjeta.
// Devuelve un codigo: 0 = hecho; 1 = no hay nada que ganar; 2 = la tabla no
// sirve; 3 = hay algo detras de NemoFS; 4 = no se pudo leer o escribir;
// 5 = NemoFS no puede crecer sin reformatear.
// Deja en *sectores_nuevos los sectores que tendra la particion.
int disk_estirar_nemofs(uint64_t *sectores_nuevos);

// Atajos que operan sobre el disco 0 -- así el código que ya usa la
// API antigua (como nemofs.c) no necesita cambiar.
static inline bool disk_read_sector(uint64_t sector, void *buf) {
    return disk_read_sector_n(0, sector, buf);
}
static inline bool disk_read_sectors(uint64_t sector, uint32_t n, void *buf) {
    return disk_read_sectors_n(0, sector, n, buf);
}
static inline bool disk_write_sector(uint64_t sector, const void *buf) {
    return disk_write_sector_n(0, sector, buf);
}
static inline uint64_t disk_capacity_sectors(void) {
    return disk_capacity_sectors_n(0);
}

#endif
