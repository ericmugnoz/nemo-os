// power.c — Nemo OS
//
// PSCI (Power State Coordination Interface) es el mecanismo estandar
// de ARM para pedirle al firmware/hipervisor que apague o reinicie la
// maquina -- es lo mismo que usa Linux por debajo. Se invoca con una
// instruccion HVC (Hypervisor Call), pasando el "ID de funcion" PSCI
// en x0. QEMU implementa esto para la maquina "virt" -- el "conducto"
// exacto (HVC o SMC) depende de la configuracion del CPU virtual;
// para nuestra configuracion (cortex-a53 sin firmware EL3 propio),
// HVC es el que funciona.

#include "power.h"
#include <stdint.h>

#define PSCI_SYSTEM_OFF   0x84000008UL
#define PSCI_SYSTEM_RESET 0x84000009UL

static void psci_call(uint64_t function_id) {
    register uint64_t x0 __asm__("x0") = function_id;
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
}

void power_shutdown(void) {
    psci_call(PSCI_SYSTEM_OFF);
    // Si por lo que sea la llamada no apaga la maquina, no seguimos
    // ejecutando nada mas.
    while (1) { __asm__ volatile("wfe"); }
}

void power_reset(void) {
    psci_call(PSCI_SYSTEM_RESET);
    while (1) { __asm__ volatile("wfe"); }
}
