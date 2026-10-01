// nic.h -- Nemo OS
// La tarjeta de red, vista desde net.c: cuatro funciones y nada mas.
// En la Pi 4 las da genet_pi4.c (el Ethernet integrado); en QEMU,
// virtio_net.c (una tarjeta virtio-net en la maquina virt). net.c,
// tcp.c y netshell.c no saben cual de las dos hay debajo -- por eso
// toda la pila de red se puede probar en QEMU sin tocar la placa.
#ifndef NIC_H
#define NIC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef NEMO_QEMU
#include "virtio_net.h"
static inline bool     nic_init(void)                              { return virtio_net_init(); }
static inline bool     nic_link_up(void)                           { return virtio_net_link_up(); }
static inline void     nic_get_mac(uint8_t out[6])                 { virtio_net_get_mac(out); }
static inline bool     nic_send(const uint8_t *f, uint32_t len)    { return virtio_net_send(f, len); }
static inline uint32_t nic_recv(uint8_t *out, uint32_t max)        { return virtio_net_recv(out, max); }
// virtio-net no tiene el problema que motivo esto (en QEMU el "cable"
// no falla), asi que aqui no hay nada que contar.
static inline void     nic_diagnostico(void)                       { }
#else
#include "genet_pi4.h"
static inline bool     nic_init(void)                              { return genet_init_pi4(); }
static inline bool     nic_link_up(void)                           { return genet_link_up(); }
static inline void     nic_get_mac(uint8_t out[6])                 { genet_get_mac(out); }
static inline bool     nic_send(const uint8_t *f, uint32_t len)    { return genet_send(f, len); }
static inline uint32_t nic_recv(uint8_t *out, uint32_t max)        { return genet_recv(out, max); }
static inline void     nic_diagnostico(void)                       { genet_diagnostico(); }
#endif

#endif
