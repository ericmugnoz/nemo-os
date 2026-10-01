// gic.c — Nemo OS
//
// El GIC (Generic Interrupt Controller) es el "enrutador" de interrupciones
// de ARM. Tiene dos partes con las que trabajamos:
//   - Distribuidor (GICD): decide qué interrupciones están activas y con
//     qué prioridad, es compartido entre todos los cores.
//   - Interfaz de CPU (GICC): la parte que "entrega" la interrupción a
//     este core en concreto.
//
// En la máquina "virt" de QEMU (GICv2), estas son las direcciones fijas.

#include "gic.h"

#define GICD_BASE 0x08000000UL
#define GICC_BASE 0x08010000UL

// Registros del distribuidor
#define GICD_CTLR         (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER(n) (*(volatile uint32_t *)(GICD_BASE + 0x100 + 4 * (n)))
#define GICD_TYPER        (*(volatile uint32_t *)(GICD_BASE + 0x004))
#define GICD_ITARGETSR(n) (*(volatile uint32_t *)(GICD_BASE + 0x800 + 4 * (n)))
#define GICD_IPRIORITYR(n) (*(volatile uint8_t  *)(GICD_BASE + 0x400 + (n)))

// Registros de la interfaz de CPU
#define GICC_CTLR (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR  (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR  (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR (*(volatile uint32_t *)(GICC_BASE + 0x010))

void gic_init(void) {
    // A que nucleo va cada interrupcion de DISPOSITIVO (las SPI, de la
    // 32 en adelante): al nucleo 0, siempre.
    //
    // Con una sola CPU esto no hacia falta: QEMU entrega todo al unico
    // nucleo que hay. Con varias (-smp 4, desde el trabajo de SMP), el
    // destino de cada SPI empieza VACIO, y una interrupcion sin destino
    // no llega a ningun nucleo -- se pierde en silencio. Hoy no afecta
    // (la unica activa es la del reloj, 27, que es privada de cada
    // nucleo y no pasa por aqui; teclado y red van por sondeo), pero el
    // dia que alguien active una de dispositivo no le llegaria nunca.
    //
    // Cada registro ITARGETSR lleva el destino de 4 interrupciones, un
    // byte cada una; el bit 0 del byte es el nucleo 0. GICD_TYPER dice
    // cuantas lineas de interrupcion tiene este GIC.
    uint32_t lineas = ((GICD_TYPER & 0x1F) + 1) * 32;
    for (uint32_t irq = 32; irq < lineas; irq += 4) {
        GICD_ITARGETSR(irq / 4) = 0x01010101u;
    }

    // Activamos el distribuidor (grupo 0, el "normal" para no-seguro)
    GICD_CTLR = 1;

    // Prioridad mínima que aceptamos: 0xFF = aceptamos todas las
    // prioridades posibles (cuanto más bajo el número, más prioridad)
    GICC_PMR = 0xFF;

    // Activamos la interfaz de CPU
    GICC_CTLR = 1;
}

void gic_enable_irq(uint32_t irq_id) {
    // Cada bit de ISENABLER habilita una interrupción; hay 32 IDs por
    // cada registro de 32 bits, por eso dividimos entre 32.
    uint32_t reg = irq_id / 32;
    uint32_t bit = irq_id % 32;
    GICD_ISENABLER(reg) = (1 << bit);

    // Prioridad media por defecto para esta interrupción
    GICD_IPRIORITYR(irq_id) = 0x80;
}

uint32_t gic_ack_irq(void) {
    // Leer IAR nos da el ID de la interrupción activa y "la reclama"
    // para este core.
    return GICC_IAR & 0x3FF;
}

void gic_end_irq(uint32_t irq_id) {
    // Le decimos al GIC que ya hemos terminado de atenderla.
    GICC_EOIR = irq_id;
}
