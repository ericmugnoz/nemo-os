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

// Atajos que operan sobre el disco 0 -- así el código que ya usa la
// API antigua (como nemofs.c) no necesita cambiar.
static inline bool disk_read_sector(uint64_t sector, void *buf) {
    return disk_read_sector_n(0, sector, buf);
}
static inline bool disk_write_sector(uint64_t sector, const void *buf) {
    return disk_write_sector_n(0, sector, buf);
}
static inline uint64_t disk_capacity_sectors(void) {
    return disk_capacity_sectors_n(0);
}

#endif
