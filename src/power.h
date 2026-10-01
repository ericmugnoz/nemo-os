// power.h — Nemo OS
#ifndef POWER_H
#define POWER_H

// Apaga o reinicia la maquina de verdad, via PSCI (Power State
// Coordination Interface) -- el mecanismo estandar de ARM para esto,
// el mismo que usa Linux. Ninguna de las dos funciones vuelve.
void power_shutdown(void);
void power_reset(void);

#endif
