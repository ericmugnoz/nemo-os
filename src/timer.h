// timer.h — Nemo OS
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

// El timer virtual de ARM (CNTV) genera la IRQ 27 en el GIC (PPI 11).
#define TIMER_IRQ 27

void timer_init(void);
void timer_irq_handler(void);
uint64_t timer_get_ticks(void);
// Microsegundos desde el arranque, leidos del contador generico de ARM
// (cntvct_el0). NO es el contador del planificador: ese va a 100 Hz y
// solo da saltos de 10 ms. Este va a la frecuencia del sistema (decenas
// de MHz), no depende de interrupciones y sirve para medir de verdad.
uint64_t timer_micros(void);

// ---- Despertares finos, sin tocar el latido de 100 Hz ----
//
// El latido (timer_get_ticks) vale 10 ms y NO se puede cambiar: es la unidad
// publica del sistema. Los programas ya compilados convierten milisegundos a
// latidos ellos mismos -- nb_codegen.c emite un udiv por 10 para Delay -- y
// tambien cuentan en latidos SYS_GET_TICKS, CreateTimer, el doble clic y los
// tooltips. Subirlo a 1000 haria que todos los Delay de todos los programas
// que ya existen duraran la decima parte.
//
// Para que el kernel pueda despertarse mas a menudo sin mover esa unidad se
// usa el FLUJO DE EVENTOS del contador de ARM: el propio contador lanza un
// evento periodico (~1,2 ms en la Pi 4) que despierta el wfe de su nucleo,
// SIN ninguna interrupcion de mas. Es lo que ya hacen los nucleos
// secundarios en smp.c; esto lo pone al alcance de todos.
void timer_flujo_eventos(void);   // activarlo en ESTE nucleo
void timer_dormir_corto(void);    // wfe hasta el proximo evento o interrupcion

#endif
