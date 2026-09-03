// timer.h — Nemo OS
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

// El timer virtual de ARM (CNTV) genera la IRQ 27 en el GIC (PPI 11).
#define TIMER_IRQ 27

void timer_init(void);
void timer_irq_handler(void);
uint64_t timer_get_ticks(void);

#endif
