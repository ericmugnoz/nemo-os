// exceptions.c — Nemo OS
// Manejadores en C llamados desde exceptions.s tras guardar el estado.

#include <stdint.h>
#include "uart.h"
#include "gic.h"
#include "timer.h"
#include "syscall.h"
#include "tasks.h"

void handle_unexpected(void) {
    uart_puts("EXCEPCION INESPERADA - deteniendo\n");
    while (1) { __asm__ volatile("wfe"); }
}

// Imprime un valor de 64 bits en hexadecimal por UART. Es una utilidad
// mínima -- todavía no tenemos algo como printf.
static void uart_put_hex64(uint64_t val) {
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (val >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10);
        uart_putc(c);
    }
}

// Layout EXACTO de lo que SAVE_STATE guarda en la pila (ver
// exceptions.s) -- nos permite leer los argumentos de una syscall
// (x0-x5) y el numero de syscall (x8), y escribir el valor de
// retorno de vuelta en x0 para que el programa lo vea al continuar.
typedef struct {
    uint64_t x0, x1;
    uint64_t x2, x3;
    uint64_t x4, x5;
    uint64_t x6, x7;
    uint64_t x8, x9;
    uint64_t x10, x11;
    uint64_t x12, x13;
    uint64_t x14, x15;
    uint64_t x16, x17;
    uint64_t x18, x19;
    uint64_t x20, x21;
    uint64_t x22, x23;
    uint64_t x24, x25;
    uint64_t x26, x27;
    uint64_t x28, x29;
    uint64_t x30, elr;
    uint64_t spsr;
} trap_frame_t;

#define ESR_EC_SVC64 0x15

static void handle_sync_error(uint64_t esr, trap_frame_t *frame);

void handle_sync(trap_frame_t *frame) {
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    uint32_t ec = (esr >> 26) & 0x3F;

    if (ec == ESR_EC_SVC64) {
        // Una tarea que ya fue terminada (por ejemplo, al cerrar su
        // ventana) no vuelve nunca de esta llamada: cede el turno para
        // siempre. Antes seguia ejecutandose hasta ceder el turno por su
        // cuenta -- y un programa atascado en un bucle, que no llega a
        // Pump, no lo cedia nunca: cerrar su ventana no liberaba su
        // nucleo. Asi, basta con que haga cualquier llamada al sistema.
        // (Un bucle SIN ninguna llamada seguiria sin poder pararse: eso
        // necesitaria interrupciones del reloj en los secundarios.)
        if (task_actual_terminada()) {
            for (;;) task_yield();
        }
        // Una syscall no es un error -- despachamos y volvemos
        // normalmente. ELR_EL1 ya apunta a la instruccion siguiente
        // al 'svc' (asi lo define la arquitectura), no hace falta
        // ajustarlo.
        frame->x0 = syscall_dispatch(frame->x8, frame->x0, frame->x1, frame->x2, frame->x3, frame->x4, frame->x5);
        return;
    }

    handle_sync_error(esr, frame);
}

static void handle_sync_error(uint64_t esr, trap_frame_t *frame) {
    uint64_t far;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    uint32_t ec = (esr >> 26) & 0x3F;

    uart_puts("EXCEPCION SINCRONA\n");

    // Nombre del programa y desplazamiento exacto dentro de su
    // propio codigo -- mucho mas util que la direccion absoluta (que
    // cambia cada vez que se carga) para localizar el fallo en el
    // .elf o el .s.
    const char *prog_name;
    uint64_t prog_base;
    task_get_debug_info(&prog_name, &prog_base);
    uart_puts("  programa = ");
    uart_puts(prog_name);
    uart_puts("\n  desplazamiento dentro del programa = ");
    if (frame->elr >= prog_base) {
        uart_put_hex64(frame->elr - prog_base);
    } else {
        uart_puts("(la direccion no cae dentro de este programa)");
    }
    uart_putc('\n');

    uart_puts("  ELR_EL1 = "); // direccion EXACTA de la instruccion que fallo
    uart_put_hex64(frame->elr);
    uart_puts("\n  ESR_EL1 = ");
    uart_put_hex64(esr);
    uart_puts("\n  FAR_EL1 = ");
    uart_put_hex64(far);
    uart_puts("\n  EC (clase de excepcion) = ");
    uart_put_hex64(ec);

    switch (ec) {
        case 0x25: uart_puts(" -> Data Abort (mismo nivel EL: fallo DEL KERNEL)\n"); break;
        case 0x24: uart_puts(" -> Data Abort desde EL0 (la tarea toco memoria que no es suya)\n"); break;
        case 0x21: uart_puts(" -> Instruction Abort (mismo nivel EL: fallo DEL KERNEL)\n"); break;
        case 0x20: uart_puts(" -> Instruction Abort desde EL0 (la tarea salto a codigo que no es suyo)\n"); break;
        case 0x00: uart_puts(" -> Motivo desconocido\n"); break;
        case 0x0E: uart_puts(" -> Instruccion ilegal (Illegal Execution State)\n"); break;
        case 0x07: uart_puts(" -> Acceso a FP/SIMD atrapado (CPACR_EL1.FPEN?)\n"); break;
        default:   uart_puts(" -> (ver manual ARM para este codigo EC)\n"); break;
    }

    uart_puts("  x0="); uart_put_hex64(frame->x0);
    uart_puts(" x1="); uart_put_hex64(frame->x1);
    uart_puts(" x2="); uart_put_hex64(frame->x2);
    uart_putc('\n');

    // Fase 1 de la separacion kernel/programas: si la excepcion vino
    // de EL0 (SPSR.M[3:0]=0000), es una TAREA la que fallo, no el
    // kernel -- se retira solo esa tarea y el sistema sigue. Esto es
    // exactamente el punto 3 del roadmap ("que hace el manejador ante
    // un fallo real de una tarea"): antes de la Fase 1 todo corria en
    // EL1 y era imposible distinguir un fallo de tarea de uno del
    // kernel, asi que solo cabia colgar todo. Si vino de EL1 sigue
    // siendo un fallo del propio kernel y se detiene, como siempre.
    if ((frame->spsr & 0xF) == 0) {
        uart_puts("La tarea fallo en EL0 -- se retira SOLO esa tarea, el sistema sigue.\n");
        task_exit_current(); // no vuelve
    }

    uart_puts("Fallo en EL1 (kernel). Deteniendo.\n");
    while (1) { __asm__ volatile("wfe"); }
}

void handle_irq(void) {
    // Preguntamos al GIC qué interrupción ha llegado
    uint32_t irq_id = gic_ack_irq();

    if (irq_id == TIMER_IRQ) {
        timer_irq_handler();
    }
    // (más adelante: teclado, disco, etc. se añaden aquí como más 'if')

    gic_end_irq(irq_id);
}

void handle_fiq(void) {
    uart_puts("FIQ recibida (no usada todavia)\n");
}

void handle_serror(void) {
    uart_puts("SERROR - error grave de sistema\n");

    // Leer el registro de sindrome completo -- nos dice el motivo
    // EXACTO del fallo, en vez de solo saber que ocurrio uno. El
    // campo EC (bits 31:26) clasifica el tipo de excepcion, y el
    // campo ISS (bits 24:0) da detalles especificos segun ese tipo.
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    uart_puts("ESR_EL1 = ");
    uart_put_hex64(esr);
    uart_puts(" (EC=");
    uart_put_hex64((esr >> 26) & 0x3F);
    uart_puts(" ISS=");
    uart_put_hex64(esr & 0x1FFFFFF);
    uart_puts(")\n");

    // FAR_EL1 -- la direccion de fallo, si el tipo de excepcion la
    // proporciona (no todos los EC la rellenan).
    uint64_t far;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    uart_puts("FAR_EL1 = ");
    uart_put_hex64(far);
    uart_puts("\n");

    uart_puts("deteniendo\n");
    while (1) { __asm__ volatile("wfe"); }
}
