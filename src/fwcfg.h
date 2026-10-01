// fwcfg.h — Nemo OS
#ifndef FWCFG_H
#define FWCFG_H

#include <stdint.h>
#include <stdbool.h>

// Busca un archivo de configuracion por nombre (ej. "etc/ramfb").
// Devuelve su clave selectora y tamaño si lo encuentra.
bool fw_cfg_find_file(const char *name, uint16_t *out_key, uint32_t *out_size);

// Escribe 'length' bytes en el archivo identificado por 'key', usando
// el interfaz DMA de fw_cfg (la unica forma de escribir en QEMU
// moderno).
bool fw_cfg_dma_write(uint16_t key, const void *buf, uint32_t length);

#endif
