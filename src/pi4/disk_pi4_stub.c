// disk_pi4_stub.c — Nemo OS, Raspberry Pi 4
//
// STUB TEMPORAL, no una decision de diseño definitiva. El driver
// real de disco para la Pi 4 (controlador Arasan EMMC2) esta
// pendiente de una sesion dedicada -- ver la bitacora del port,
// entrada del, para la investigacion ya hecha y por que
// se pospuso con cuidado en vez de adaptar algo a medias.
//
// Este stub simplemente dice "no hay disco disponible", de la
// misma forma segura que el propio disk_init() de QEMU ya maneja
// el caso de no encontrar ningun dispositivo virtio-blk -- el
// resto de kernel.c ya sabe seguir adelante sin disco (salta el
// bloque de nemofs_mount()/fat_mount(), sigue hasta el framebuffer
// y el gestor de ventanas con total normalidad).

#include "disk.h"

bool disk_init(void) {
    // Sin driver EMMC2 todavia -- honesto, no silencioso.
    return false;
}

uint32_t disk_count(void) {
    return 0;
}

bool disk_read_sector_n(uint8_t disk, uint64_t sector, void *buf) {
    (void)disk; (void)sector; (void)buf;
    return false;
}

bool disk_write_sector_n(uint8_t disk, uint64_t sector, const void *buf) {
    (void)disk; (void)sector; (void)buf;
    return false;
}

uint64_t disk_capacity_sectors_n(uint8_t disk) {
    (void)disk;
    return 0;
}
