// uart.c — Nemo OS (UART para QEMU, maquina "virt")
//
// FALTABA ESTE ARCHIVO ENTERO -- ausente del arbol fuente desde
// siempre (confirmado: ni siquiera esta en el .zip original del
// proyecto, no es un caso de "se perdio al copiar"), por eso el
// enlazado de QEMU fallaba con decenas de "undefined reference to
// uart_puts/uart_putc" -- venia declarada en uart.h y usada por medio
// kernel (kernel.c, syscall.c, tasks.c, exceptions.c, disk.c, fat.c,
// loader.c, input.c, fwcfg.c...) pero nunca se habia escrito la
// implementacion para esta plataforma.
//
// Mismo chip PL011 que en la Pi 4 real (identica disposicion de
// registros DR/FR/IBRD/FBRD/LCRH/CR -- ver uart_pi4.c y el Capitulo 8
// de la guia bare-metal) -- lo unico que cambia es la direccion base:
// la maquina "virt" de QEMU mapea el UART0 en 0x09000000 (confirmado
// por el comentario ya existente en mmu.c: "aqui viven el UART en
// 0x09000000 y el GIC en 0x08000000..0x08010000"), y no hace falta
// configurar ningun pin GPIO -- el UART virtual ya esta "cableado" de
// fabrica, a diferencia de la Pi 4 real.
//
// uart_init() se deja disponible por si hace falta en el futuro, pero
// HOY NO LA LLAMA NADIE (ni aqui ni en la Pi 4): QEMU arranca su UART
// emulada ya en un estado utilizable sin necesidad de programarla por
// software, y en la Pi 4 real es el propio firmware (con
// enable_uart=1 en config.txt) quien la deja configurada antes de
// saltar al kernel -- ver la guia bare-metal, seccion 5.

#include <stdint.h>

#define UART0_BASE 0x09000000UL

#define UART0_DR    (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART0_FR    (*(volatile uint32_t *)(UART0_BASE + 0x18))
#define UART0_IBRD  (*(volatile uint32_t *)(UART0_BASE + 0x24))
#define UART0_FBRD  (*(volatile uint32_t *)(UART0_BASE + 0x28))
#define UART0_LCRH  (*(volatile uint32_t *)(UART0_BASE + 0x2C))
#define UART0_CR    (*(volatile uint32_t *)(UART0_BASE + 0x30))
#define UART0_ICR   (*(volatile uint32_t *)(UART0_BASE + 0x44))

#define UART0_FR_TXFF (1 << 5)   // FIFO de transmision llena

void uart_init(void) {
    UART0_CR = 0;              // desactivar antes de reconfigurar
    UART0_ICR = 0x7FF;         // limpiar interrupciones pendientes
    UART0_IBRD = 26;           // 115200 baudios a 48 MHz (mismo calculo que la Pi 4 real)
    UART0_FBRD = 3;
    UART0_LCRH = 0x70;         // 8 bits, sin paridad, FIFO habilitada
    UART0_CR = 0x301;          // UART + TX + RX habilitados
}

#include "bitacora.h"

void uart_putc(char c) {
    bitacora_anadir(c);   // ver bitacora.h
    while (UART0_FR & UART0_FR_TXFF) {
        // espera activa -- aceptable aqui, es la pieza mas basica
        // del sistema, sin nada mas que hacer mientras tanto
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
