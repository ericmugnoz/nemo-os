// smp.h -- varios nucleos. Ver smp.c.
#ifndef SMP_H
#define SMP_H
#include <stdint.h>

// Pila de cada nucleo secundario. TIENE que coincidir con el "lsl #14"
// de secundario_entrada en boot.s (2^14 = 16384).
#define SMP_PILA 16384

// Despierta los nucleos 1-3. Lo llama el nucleo 0 al final del arranque.
void smp_arrancar(void);

// Primer codigo en C de un nucleo secundario (lo llama boot.s).
void secundario_main(uint32_t nucleo);

// Cuantos nucleos arrancaron, contando el 0. Ver smp.c.
uint32_t smp_nucleos_en_marcha(void);

#endif
