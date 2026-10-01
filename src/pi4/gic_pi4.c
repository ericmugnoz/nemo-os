// gic_pi4.c — Nemo OS, Raspberry Pi 4
//
// El GIC-400 de la Pi 4 es, en el diseño de sus registros, el
// mismo GICv2 estandar de ARM que ya usa gic.c en QEMU -- mismos
// desplazamientos (GICD_CTLR en +0x000, GICD_ISENABLER en +0x100,
// GICC_IAR en +0x00C, etc.), definidos por la propia arquitectura,
// no por cada fabricante. Lo unico que cambia de verdad son las
// direcciones base.
//
// Verificado contra el device tree oficial de BCM2711: el GIC vive
// en el area de "perifericos locales ARM", una region MMIO
// separada del bloque principal de perifericos (el mismo que usan
// mailbox/UART/GPIO en 0xFE000000). El device tree remapea la
// direccion local 0x40000000 a la direccion fisica real
// 0xFF800000; el GIC-400 en si empieza en el desplazamiento
// 0x41000 dentro de esa region, dando:
//   GICD real = 0xFF800000 + 0x41000 = 0xFF841000
//   GICC real = 0xFF800000 + 0x42000 = 0xFF842000
// Confirmado de forma cruzada contra multiples tutoriales bare-metal
// especificos de Pi 4 y contra un volcado real de device tree de
// una Raspberry Pi 4 -- no asumido por analogia con otro modelo.

#include "gic.h"

#define GICD_BASE 0xFF841000UL
#define GICC_BASE 0xFF842000UL

#define GICD_CTLR         (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER(n) (*(volatile uint32_t *)(GICD_BASE + 0x100 + 4 * (n)))
#define GICD_TYPER        (*(volatile uint32_t *)(GICD_BASE + 0x004))
#define GICD_ITARGETSR(n) (*(volatile uint32_t *)(GICD_BASE + 0x800 + 4 * (n)))
#define GICD_IPRIORITYR(n) (*(volatile uint8_t  *)(GICD_BASE + 0x400 + (n)))

#define GICC_CTLR (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR  (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR  (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR (*(volatile uint32_t *)(GICC_BASE + 0x010))

void gic_init(void) {
    // Todas las interrupciones de DISPOSITIVO (SPI, de la 32 en adelante)
    // van al nucleo 0. Con varios nucleos despiertos, un destino vacio
    // significa que la interrupcion no llega a nadie. Mismo arreglo que
    // en gic.c (QEMU); ver alli la explicacion completa.
    uint32_t lineas = ((GICD_TYPER & 0x1F) + 1) * 32;
    for (uint32_t irq = 32; irq < lineas; irq += 4) {
        GICD_ITARGETSR(irq / 4) = 0x01010101u;
    }
    GICD_CTLR = 1;
    GICC_PMR = 0xFF;
    GICC_CTLR = 1;
}

void gic_enable_irq(uint32_t irq_id) {
    uint32_t reg = irq_id / 32;
    uint32_t bit = irq_id % 32;
    GICD_ISENABLER(reg) = (1 << bit);
    GICD_IPRIORITYR(irq_id) = 0x80;
}

uint32_t gic_ack_irq(void) {
    return GICC_IAR & 0x3FF;
}

void gic_end_irq(uint32_t irq_id) {
    GICC_EOIR = irq_id;
}
