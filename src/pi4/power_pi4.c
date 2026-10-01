// power_pi4.c — Nemo OS (implementacion de power.h para Raspberry Pi 4)
//
// BUG REAL CORREGIDO: power.c (la version compartida, pensada para
// QEMU) reinicia con una llamada PSCI por HVC -- eso funciona en QEMU
// porque la maquina "virt" emula un firmware/hipervisor en EL3 que
// atiende esa llamada. Una Raspberry Pi 4 real arrancada en bare
// metal NO tiene ese firmware EL3 escuchando HVC (a menos que se use
// ARM Trusted Firmware de verdad, que este proyecto no usa) -- la
// llamada simplemente no hace nada, de ahi que "Reiniciar" no
// funcionara nunca en hardware real.
//
// El reinicio de verdad en un Broadcom BCM2711 (el SoC de la Pi 4) se
// hace por hardware, a traves del bloque de gestion de energia (PM):
// se arma el watchdog con un plazo minimo y se le pide un reinicio
// completo -- el propio watchdog, al vencer casi al instante, resetea
// el chip entero. Es la tecnica estandar en proyectos bare-metal para
// Raspberry Pi (Circle, rpi4-osdev, y otros la usan igual).
//
// Los registros del PM exigen una "contraseña" (0x5A) en el byte alto
// de cada escritura -- sin ella, el hardware ignora la escritura por
// completo (proteccion contra escrituras accidentales a un bloque que
// puede resetear toda la placa).

#include "power.h"
#include <stdint.h>

#define PERIPHERAL_BASE 0xFE000000UL   // misma base que uart_pi4.c/mailbox_pi4.c
#define PM_BASE         (PERIPHERAL_BASE + 0x100000)

#define PM_RSTC (*(volatile uint32_t *)(PM_BASE + 0x1c))
#define PM_WDOG (*(volatile uint32_t *)(PM_BASE + 0x24))

#define PM_PASSWORD           0x5A000000UL
#define PM_RSTC_WRCFG_FULL_RESET 0x00000020UL
#define PM_RSTC_WRCFG_CLR         0xFFFFFFCFUL

void power_reset(void) {
    // Plazo del watchdog lo mas corto posible (unidades de ~1/16us) --
    // el reinicio llega casi al instante.
    PM_WDOG = PM_PASSWORD | 1;
    uint32_t rstc = PM_RSTC;
    PM_RSTC = PM_PASSWORD | (rstc & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL_RESET;

    // El watchdog dispara el reset en cuanto vence -- no deberiamos
    // llegar mucho mas alla de aqui. Si por lo que sea no llega,
    // mejor quedarse esperando que seguir ejecutando cualquier otra
    // cosa a medias.
    while (1) { __asm__ volatile("wfe"); }
}

// Un "apagado" de verdad (cortar la alimentacion de 5V) no esta al
// alcance del propio nucleo ARM en una Pi 4 sin hablar con el chip de
// gestion de energia externo (algo bastante mas alla de lo que este
// proyecto necesita ahora mismo) -- lo mas honesto que se puede hacer
// en bare metal es dejar el procesador detenido, de forma segura, a
// la espera de que se desenchufe a mano. Circle y otros proyectos
// bare-metal para Pi hacen lo mismo por el mismo motivo.
void power_shutdown(void) {
    __asm__ volatile("msr daifset, #0xf"); // deshabilita todas las interrupciones antes de detenerse
    while (1) { __asm__ volatile("wfe"); }
}
