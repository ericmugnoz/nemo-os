// bkl.h -- el candado grande del kernel (Big Kernel Lock). Ver bkl.c.
#ifndef BKL_H
#define BKL_H

// Coger el candado. Se llama al entrar en el kernel desde un programa
// (EL0) y una vez al arrancar el kernel en el nucleo 0.
void bkl_tomar(void);

// Soltarlo. Se llama justo antes de volver a un programa, SIEMPRE con
// las interrupciones ya enmascaradas.
void bkl_soltar(void);

// Para el manejador de interrupciones del kernel: coge el candado solo
// si este nucleo no lo tiene, y devuelve 1 si lo cogio. Ver bkl.c.
int bkl_tomar_si_falta(void);

#endif
