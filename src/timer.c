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

    // Y el flujo de eventos en el nucleo 0. Hasta ahora solo lo activaban los
    // secundarios, asi que el nucleo 0 -- el que atiende raton, teclado y
    // pantalla -- dormia en su wfe hasta la SIGUIENTE interrupcion del reloj:
    // hasta 10 ms por vuelta. Con el flujo de eventos despierta cada ~1,2 ms.
    timer_flujo_eventos();
}

// CNTKCTL_EL1: EVNTEN (bit 2) activa el flujo; EVNTI (bits 7:4) elige que bit
// del contador lo dispara -- con el 15, un evento cada 2^16 pulsos: ~1,05 ms
// en QEMU (62,5 MHz) y ~1,2 ms en la Pi 4 (54 MHz). EVNTDIR (bit 3) a 0: al
// pasar ese bit de 0 a 1. El registro es de CADA nucleo, asi que cada uno
// tiene que llamar a esto por su cuenta. Se lee y se modifica para no pisar
// los demas bits.
void timer_flujo_eventos(void) {
    uint64_t cntkctl;
    __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(cntkctl));
    cntkctl &= ~((0xFUL << 4) | (1UL << 3));
    cntkctl |= (1UL << 2) | (15UL << 4);
    __asm__ volatile("msr cntkctl_el1, %0" :: "r"(cntkctl));
    __asm__ volatile("isb" ::: "memory");
}

// Dos wfe, no uno. En ARM, volver de una excepcion (ERET) deja ACTIVADO el
// registro de eventos a proposito, para no perder despertares: el primer wfe
// se lo gasta y vuelve al instante. El segundo es el que duerme de verdad,
// hasta el proximo evento del contador (~1,2 ms) o hasta cualquier
// interrupcion -- el raton, el disco --, que es justo lo que interesa: si
// llega algo antes, se despierta antes.
//
// Si el flujo de eventos no estuviera activo en este nucleo, el segundo wfe
// dormiria hasta la proxima interrupcion del reloj, o sea el comportamiento
// de siempre. Degrada bien: nunca duerme MAS que antes.
void timer_dormir_corto(void) {
    __asm__ volatile("wfe");
    __asm__ volatile("wfe");
}

void timer_irq_handler(void) {
    ticks++;

    // Re-armamos el timer para el siguiente tick — si no lo hacemos,
    // esto era un disparo único, no periódico.
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"((uint64_t)ticks_per_interval));

    // El "tick" por UART cada segundo ya cumplio su funcion (confirmar
    // que el timer dispara). Se retira: inundaba la consola serie y
    // tapaba los mensajes de arranque que hay que leer. El contador
    // `ticks` sigue alimentando al scheduler igual que antes.
}

uint64_t timer_get_ticks(void) {
    return ticks;
}

// Microsegundos desde el arranque. El contador generico de ARM es libre
// y monotono, asi que no hace falta ninguna interrupcion.
//
// El orden de la cuenta importa: (cnt / freq) * 1000000 perderia toda la
// parte decimal de los segundos, y cnt * 1000000 se desbordaria en unas
// cinco horas con una frecuencia de 62,5 MHz. Se parte la cuenta en
// segundos enteros y resto, y asi ni se desborda ni se pierde precision.
uint64_t timer_micros(void) {
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) return 0;
    uint64_t seg = cnt / freq;
    uint64_t resto = cnt - seg * freq;
    return seg * 1000000ULL + (resto * 1000000ULL) / freq;
}
