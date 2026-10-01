// medir.h -- medicion de rendimiento del SMP, para diagnostico.
//
// Cada ~2 segundos el nucleo 0 escribe por la UART cuanto han tardado las
// composiciones de pantalla y cuanto ha esperado cada nucleo por el
// candado grande. Sirve para saber DONDE se va el tiempo cuando algo va
// lento, en vez de adivinarlo.
//
// Para desactivarlo, poner MEDIR_RENDIMIENTO a 0: todas las llamadas
// desaparecen al compilar.
#ifndef MEDIR_H
#define MEDIR_H

#include <stdint.h>

#define MEDIR_RENDIMIENTO 0   // 1 para volver a medir (ver arriba)
// APAGADO otra vez, ya con la respuesta. Se encendio porque con
// el monitor del sistema abierto el raton iba a tirones, y habia tres
// candidatos —-componer, presentar, y esperar el candado grande—- que solo
// estas cifras distinguen. Encontro dos fallos que nadie habria adivinado:
//
//   1. DiskUsedBlocks() recorria 33,5 millones de bits del mapa de bloques
//      con el candado grande cogido: 79 ms de una sola vez, dos veces por
//      repintado del monitor. Ver nemofs_disk_usage.
//   2. Las diez syscalls de dibujo pedian redibujado de PANTALLA ENTERA, asi
//      que el rectangulo sucio era siempre toda la pantalla. Se delato porque
//      fb_present se quedaba clavado en 3,07 ms hiciera el programa lo que
//      hiciera. Ver la nota de "Graficos y ventana" en syscall.c.
//
// Composicion con el monitor abierto: 46,4 ms -> 7,0 ms -> 2,95 ms.
// Y la moraleja: los 79 ms del primero TAPABAN al segundo.

static inline uint64_t medir_ahora(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v) :: "memory");
    return v;
}

#if MEDIR_RENDIMIENTO
void medir_composicion(uint64_t pulsos);                 // wm.c
void medir_publicacion(void);                            // wm.c
void medir_espera_candado(uint32_t nucleo, uint64_t pulsos);  // bkl.c
void medir_retencion_candado(uint32_t nucleo, uint64_t pulsos); // bkl.c: cuanto lo TUVO
void medir_presentacion(uint64_t pulsos);                // wm.c: fb_present dentro de cada composicion
void medir_informar_si_toca(void);                       // tasks.c, turno del nucleo 0
#else
static inline void medir_composicion(uint64_t p) { (void)p; }
static inline void medir_publicacion(void) { }
static inline void medir_espera_candado(uint32_t n, uint64_t p) { (void)n; (void)p; }
static inline void medir_retencion_candado(uint32_t n, uint64_t p) { (void)n; (void)p; }
static inline void medir_presentacion(uint64_t p) { (void)p; }
static inline void medir_informar_si_toca(void) { }
#endif

#endif
