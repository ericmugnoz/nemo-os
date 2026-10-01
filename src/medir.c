// medir.c -- ver medir.h.
#include <stdint.h>
#include "medir.h"
#include "cpu.h"
#include "uart.h"

#if MEDIR_RENDIMIENTO

// Acumulados desde el ultimo informe. Los de composicion y publicacion
// solo los toca el kernel con el candado cogido; los de espera, cada
// nucleo los suyos. No hace falta mas proteccion.
static uint64_t comp_n, comp_total, comp_max;
static uint64_t publicaciones;
static uint64_t espera_total[MAX_CPUS], espera_n[MAX_CPUS], espera_max[MAX_CPUS];
static uint64_t retencion_total[MAX_CPUS], retencion_max[MAX_CPUS];
static uint64_t present_total, present_n;
static uint64_t ultimo_informe;

void medir_composicion(uint64_t p) {
    comp_n++; comp_total += p;
    if (p > comp_max) comp_max = p;
}
void medir_publicacion(void) { publicaciones++; }
void medir_retencion_candado(uint32_t n, uint64_t p) {
    if (n >= MAX_CPUS) return;
    retencion_total[n] += p;
    if (p > retencion_max[n]) retencion_max[n] = p;
}
void medir_presentacion(uint64_t p) { present_total += p; present_n++; }
void medir_espera_candado(uint32_t n, uint64_t p) {
    if (n >= MAX_CPUS) return;
    espera_n[n]++; espera_total[n] += p;
    if (p > espera_max[n]) espera_max[n] = p;
}

static void dec(uint64_t v) {
    char b[21]; int n = 0;
    if (v == 0) { uart_puts("0"); return; }
    while (v > 0 && n < 20) { b[n++] = (char)('0' + v % 10); v /= 10; }
    char u[2] = {0, 0};
    while (n > 0) { u[0] = b[--n]; uart_puts(u); }
}
// microsegundos a partir de pulsos del contador
static uint64_t us(uint64_t pulsos, uint64_t frq) { return frq ? (pulsos * 1000000ULL) / frq : 0; }

void medir_informar_si_toca(void) {
    uint64_t frq; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    uint64_t ahora = medir_ahora();
    if (ultimo_informe == 0) { ultimo_informe = ahora; return; }
    if (ahora - ultimo_informe < 2 * frq) return;   // cada ~2 s

    uart_puts("medir: composiciones ");
    dec(comp_n);
    uart_puts(", media ");
    dec(comp_n ? us(comp_total / comp_n, frq) : 0);
    uart_puts(" us, max ");
    dec(us(comp_max, frq));
    uart_puts(" us | publicaciones ");
    dec(publicaciones);
    uart_puts("\nmedir: espera por el candado (total en 2 s / maxima):");
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        uart_puts(" n"); dec(c); uart_puts("=");
        dec(us(espera_total[c], frq) / 1000); uart_puts("ms/");
        dec(us(espera_max[c], frq) / 1000); uart_puts("ms");
    }
    uart_puts("\nmedir: candado RETENIDO por cada nucleo (total en 2 s / la vez mas larga):");
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        uart_puts(" n"); dec(c); uart_puts("=");
        dec(us(retencion_total[c], frq) / 1000); uart_puts("ms/");
        dec(us(retencion_max[c], frq) / 1000); uart_puts("ms");
    }
    uart_puts("\nmedir: de cada composicion, fb_present tarda de media ");
    dec(present_n ? us(present_total / present_n, frq) : 0);
    uart_puts(" us\n");

    for (uint32_t c = 0; c < MAX_CPUS; c++) retencion_total[c] = retencion_max[c] = 0;
    present_total = present_n = 0;
    comp_n = comp_total = comp_max = 0;
    publicaciones = 0;
    for (uint32_t c = 0; c < MAX_CPUS; c++) espera_total[c] = espera_n[c] = espera_max[c] = 0;
    ultimo_informe = ahora;
}
#endif
