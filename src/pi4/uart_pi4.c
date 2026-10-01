// uart_pi4.c -- UART real para Raspberry Pi 4
//
// El propio chip PL011 es EL MISMO que ya conoces de QEMU
// (documentado en Cartas de navegacion del Nautilus, Capitulo 8)
// -- incluida la disposicion de registros DR/FR/IBRD/FBRD/LCRH/CR.
// Lo unico que cambia de verdad es:
//   1. La direccion base (aqui, no la de QEMU)
//   2. Que hay que configurar los pines GPIO 14/15 a su funcion
//      alternativa ALT0 antes de que el UART hable con el mundo
//      exterior -- en QEMU esto no hacia falta, el UART virtual
//      ya estaba "cableado" de fabrica.

#include <stdint.h>

// Base de perifericos en "Low Peripheral Mode", el modo por
// defecto de la Pi 4 -- verificado contra la documentacion
// oficial de Broadcom y multiples proyectos bare-metal reales.
#define PERIPHERAL_BASE   0xFE000000UL

#define GPIO_BASE         (PERIPHERAL_BASE + 0x200000)
#define UART0_BASE        (PERIPHERAL_BASE + 0x201000)

// --- Registros GPIO que necesitamos ---
#define GPFSEL1           (*(volatile uint32_t *)(GPIO_BASE + 0x04))

// --- Registros PL011 (UART0) -- identicos en nombre y offset a
//     los ya documentados en el Capitulo 8, solo cambia la base ---
#define UART0_DR          (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART0_FR          (*(volatile uint32_t *)(UART0_BASE + 0x18))
#define UART0_IBRD        (*(volatile uint32_t *)(UART0_BASE + 0x24))
#define UART0_FBRD        (*(volatile uint32_t *)(UART0_BASE + 0x28))
#define UART0_LCRH        (*(volatile uint32_t *)(UART0_BASE + 0x2C))
#define UART0_CR          (*(volatile uint32_t *)(UART0_BASE + 0x30))
#define UART0_IMSC        (*(volatile uint32_t *)(UART0_BASE + 0x38))
#define UART0_ICR         (*(volatile uint32_t *)(UART0_BASE + 0x44))

#define UART0_FR_TXFF     (1 << 5)   // FIFO de transmision llena

static void esperar_ciclos(volatile uint32_t n) {
    while (n--) {
        asm volatile("nop");
    }
}

void uart_init(void) {
    // 1. Desactivar el UART antes de reconfigurarlo -- evita
    //    enviar basura a mitad de una reconfiguracion.
    UART0_CR = 0;

    // 2. Configurar GPIO 14 (TXD0) y GPIO 15 (RXD0) a su funcion
    //    alternativa ALT0, que es la que los conecta al PL011.
    //    GPFSEL1 usa 3 bits por pin; GPIO14 empieza en el bit 12,
    //    GPIO15 en el bit 15. El codigo ALT0 es 0b100.
    uint32_t val = GPFSEL1;
    val &= ~((7 << 12) | (7 << 15));   // limpiar los bits de ambos pines
    val |= (4 << 12) | (4 << 15);       // ALT0 para GPIO14 y GPIO15
    GPFSEL1 = val;

    esperar_ciclos(150);   // margen de estabilizacion, igual que
                             // recomiendan la mayoria de referencias
                             // bare-metal de la propia Broadcom

    // 3. Limpiar y enmascarar interrupciones -- en esta primera
    //    fase no las usamos, solo escritura por sondeo (polling).
    UART0_ICR = 0x7FF;

    // 4. Configurar la velocidad: 115200 baudios.
    //    El PL011 de la Pi 4 usa un reloj fijo de 48 MHz para el
    //    UART (independiente del reloj del nucleo ARM, a
    //    diferencia del Mini-UART). La formula estandar:
    //      divisor = 48000000 / (16 * 115200) = 26.041666...
    //      IBRD = parte entera = 26
    //      FBRD = round(0.041666 * 64) = 3
    UART0_IBRD = 26;
    UART0_FBRD = 3;

    // 5. Formato de linea: 8 bits, sin paridad, 1 bit de parada,
    //    FIFO habilitada (0x70 = WLEN=11 en bits 5-6, FEN en bit 4)
    UART0_LCRH = 0x70;

    // 6. Habilitar UART, transmision, y recepcion (bits 0, 8, 9)
    UART0_CR = 0x301;
}

#include "bitacora.h"

void uart_putc(char c) {
    // Todo lo que sale por el UART se guarda tambien en la bitacora,
    // que al final del arranque se escribe en NEMO.LOG de la FAT --
    // ver bitacora.h. Con la Pi dentro de una caja cerrada, los pines
    // del serie no se pueden alcanzar y esta es la unica forma de
    // leer el diagnostico.
    bitacora_anadir(c);
    // Esperar mientras la FIFO de transmision este llena
    while (UART0_FR & UART0_FR_TXFF) {
        // espera activa -- documentada como tal en el Capitulo 8,
        // aceptable aqui porque es la primera pieza del sistema,
        // sin nada mas que hacer mientras tanto
    }
    UART0_DR = (uint32_t)c;
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');   // los terminales esperan CR antes de LF
        }
        uart_putc(*s);
        s++;
    }
}
