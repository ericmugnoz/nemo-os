// gic.h — Nemo OS
// GICv2, direcciones fijas en la máquina "virt" de QEMU (las mismas que
// usa UTM cuando emula esta placa).
#ifndef GIC_H
#define GIC_H

#include <stdint.h>

void gic_init(void);
void gic_enable_irq(uint32_t irq_id);
uint32_t gic_ack_irq(void);
void gic_end_irq(uint32_t irq_id);

#endif
