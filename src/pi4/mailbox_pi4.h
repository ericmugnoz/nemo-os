// mailbox_pi4.h -- interfaz de mailbox_pi4.c, ya probado en la Fase 2
#ifndef MAILBOX_PI4_H
#define MAILBOX_PI4_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t ancho;
    uint32_t alto;
    uint32_t pitch;
    uint8_t *buffer;
    uint32_t ok;
    // Lo que el firmware CONCEDIO, no lo que se le pidio: 1 = RGB, 0 = BGR.
    // Si concede RGB, el cambio de rojo por azul de fb_present sobra, y ese
    // cambio es la mitad del coste de una composicion.
    uint32_t orden_pixel;
} framebuffer_t;

// Pide al VideoCore un framebuffer del tamaño dado. Ya verificado en
// hardware real (Fase 2): direcciones de mailbox, formato de mensaje,
// y conversion de direccion GPU->ARM.
framebuffer_t fb_init_pi4(uint32_t ancho, uint32_t alto);

// Direccion MAC de esta placa, grabada de fabrica y leida por el
// firmware de los fusibles del SoC -- ninguna otra forma de obtenerla
// sin pasar por el VideoCore. Etiqueta estandar 0x00010003 ("Get
// board MAC address"), 6 bytes de respuesta. true si la llamada fue
// bien Y el firmware devolvio algo que no es todo ceros (una placa
// sin MAC grabada, o un fallo silencioso, se detectan igual).
bool mailbox_get_mac_pi4(uint8_t mac[6]);

// Revision de la placa (bits 20-22: tamaño de la RAM). Ver memoria.c.
bool mailbox_revision_placa_pi4(uint32_t *revision);

// Frecuencia de un reloj del chip, en Hz (0 si no contesta). id 4 = el del nucleo.
uint32_t mailbox_reloj_pi4(uint32_t id);

#endif
