// timer.c — Nemo OS
//
// ARM tiene un "Generic Timer" integrado en la CPU (no es un periférico
// aparte como el UART). Usamos el timer virtual (CNTV), que es accesible
// desde EL1 sin complicaciones de seguridad.
//
// Funciona así: le decimos "avísame dentro de N ciclos" (CNTV_TVAL_EL0),
// y cuando ese contador llega a cero, dispara la IRQ 27. Cada vez que
// atendemos la interrupción, hay que volver a armar el contador para el
// siguiente "tick" — si no, solo dispara una vez.

#include "timer.h"
#include "gic.h"
#include "uart.h"

static uint64_t ticks = 0;
static uint32_t ticks_per_interval;

// Cuántos "ticks" del reloj de la CPU equivalen a nuestro intervalo.
// Con FRECUENCIA/100 conseguimos un tick cada 10ms (100 veces por segundo).
#define TICKS_PER_SECOND 100

void timer_init(void) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    ticks_per_interval = (uint32_t)(freq / TICKS_PER_SECOND);

    // Programamos el primer disparo
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"((uint64_t)ticks_per_interval));

    // Activamos el timer (bit 0 = enable, bit 1 = mask -> lo dejamos a 0
    // para que SÍ nos interrumpa)
    uint64_t ctl = 1;
    __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(ctl));

    gic_enable_irq(TIMER_IRQ);
}

void timer_irq_handler(void) {
    ticks++;

    // Re-armamos el timer para el siguiente tick — si no lo hacemos,
    // esto era un disparo único, no periódico.
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"((uint64_t)ticks_per_interval));

    // Cada segundo completo (100 ticks de 10ms), avisamos por UART.
    // Esto es temporal, solo para comprobar que el timer funciona de
    // verdad — más adelante esto alimentará al scheduler.
    if (ticks % TICKS_PER_SECOND == 0) {
        uart_puts("tick\n");
    }
}

uint64_t timer_get_ticks(void) {
    return ticks;
}
