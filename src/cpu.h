// cpu.h -- quien soy: el numero de nucleo que ejecuta este codigo.
//
// Con un solo nucleo esta pregunta no existia. Con varios, cada uno
// ejecuta una tarea distinta, y todo lo que dependa de "la tarea
// actual" necesita saber primero en que nucleo esta.
#ifndef CPU_H
#define CPU_H

#include <stdint.h>

// La Raspberry Pi 4 tiene 4 nucleos; QEMU se lanza con -smp 4 para
// imitarla. Los dos exponen el numero de nucleo en los bits 0-7 de
// MPIDR_EL1 (Aff0), de 0 a 3 -- el mismo campo que ya usa start_pi4.S
// para dejar aparcados los nucleos 1-3.
#define MAX_CPUS 4

static inline uint32_t cpu_id(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    // El & (MAX_CPUS-1) garantiza que nunca se indexe fuera de un
    // array por nucleo. En estos dos equipos Aff0 ya va de 0 a 3, asi
    // que no esconde nada; solo impide que un valor inesperado escriba
    // en memoria ajena.
    return (uint32_t)(mpidr & 0xFF) & (MAX_CPUS - 1);
}

#endif // CPU_H
