// virtio_net.h -- Nemo OS, QEMU virt
// Tarjeta de red virtio-net por virtio-mmio. Misma interfaz que
// genet_pi4.h -- ver nic.h.
#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include <stdbool.h>

bool     virtio_net_init(void);
bool     virtio_net_link_up(void);
void     virtio_net_get_mac(uint8_t out[6]);
bool     virtio_net_send(const uint8_t *frame, uint32_t len);
uint32_t virtio_net_recv(uint8_t *out, uint32_t max_len);

#endif
