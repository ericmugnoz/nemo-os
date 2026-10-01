// i2c.c -- Nemo OS: el bus I2C 1 de la Raspberry Pi 4 (GPIO 2 = SDA, GPIO 3 = SCL).
//
// El BSC ("Broadcom Serial Controller") del BCM2711, a 100 kHz. Cada
// transferencia es de escritura o de lectura; leer un registro de un
// sensor son dos seguidas (escribir su numero, leer su valor), con una
// parada entre medias: lo que acepta la inmensa mayoria de aparatos.
//
// El primer programa que usa el bus se queda los pines 2 y 3 (con las
// reglas de dueño de gpio.c); al terminar, vuelven a ser entradas.
//
// En QEMU no hay bus: se SIMULA una memoria EEPROM de 256 bytes en la
// direccion 0x50 (como una 24C02) -- el primer byte de una escritura
// fija la posicion, los siguientes se guardan; una lectura devuelve desde
// la posicion, avanzando. Cualquier otra direccion no responde, como en un
// bus vacio. Cada transferencia se anuncia por la terminal.
#include <stdint.h>
#include <stdbool.h>
#include "i2c.h"
#include "gpio.h"

void uart_puts(const char *s);

static void poner_hex2(char *t, uint32_t v) { const char *h = "0123456789abcdef"; t[0] = h[(v >> 4) & 15]; t[1] = h[v & 15]; }
static void poner_dec(char *t, uint32_t v, int *k) { char n[12]; int m = 0; do { n[m++] = (char)('0' + v % 10); v /= 10; } while (v); while (m) t[(*k)++] = n[--m]; }

static int32_t reclamar_pines(void) {
    int32_t r = gpio_reclamar(2, GPIO_ALT0);
    if (r < 0) return r;
    return gpio_reclamar(3, GPIO_ALT0);
}

#ifdef NEMO_QEMU
// ---- QEMU: una EEPROM simulada en 0x50 ----
static uint8_t eeprom[256];
static uint8_t eeprom_pos = 0;

static void anunciar(const char *que, uint32_t dir, uint32_t n, bool responde) {
    char t[80]; int k = 0;
    for (const char *a = "i2c (simulado): "; *a; a++) t[k++] = *a;
    for (const char *a = que; *a; a++) t[k++] = *a;
    t[k++] = ' '; poner_dec(t, n, &k);
    for (const char *a = " bytes, direccion 0x"; *a; a++) t[k++] = *a;
    poner_hex2(t + k, dir); k += 2;
    for (const char *a = responde ? "" : " -- nadie responde"; *a; a++) t[k++] = *a;
    t[k++] = '\n'; t[k] = 0;
    uart_puts(t);
}

int32_t i2c_escribir(uint32_t dir, const uint8_t *datos, uint32_t n) {
    if (dir > 0x7F || n == 0 || n > I2C_MAX) return I2C_ERR_ARG;
    int32_t r = reclamar_pines(); if (r < 0) return r;
    anunciar("escribe", dir, n, dir == 0x50);
    if (dir != 0x50) return I2C_ERR_NADIE;
    eeprom_pos = datos[0];
    for (uint32_t i = 1; i < n; i++) eeprom[eeprom_pos++] = datos[i];
    return (int32_t)n;
}

int32_t i2c_leer(uint32_t dir, uint8_t *datos, uint32_t n) {
    if (dir > 0x7F || n == 0 || n > I2C_MAX) return I2C_ERR_ARG;
    int32_t r = reclamar_pines(); if (r < 0) return r;
    anunciar("lee", dir, n, dir == 0x50);
    if (dir != 0x50) return I2C_ERR_NADIE;
    for (uint32_t i = 0; i < n; i++) datos[i] = eeprom[eeprom_pos++];
    return (int32_t)n;
}

#else
// ---- Raspberry Pi 4: el BSC1 ----
#ifndef BSC_LEER
#define BSC1_BASE 0xFE804000UL
#define BSC_LEER(off)       (*(volatile uint32_t *)(uintptr_t)(BSC1_BASE + (off)))
#define BSC_ESCRIBIR(off, v) (*(volatile uint32_t *)(uintptr_t)(BSC1_BASE + (off)) = (v))
#endif
#include "mailbox_pi4.h"

#define BSC_C     0x00
#define BSC_S     0x04
#define BSC_DLEN  0x08
#define BSC_A     0x0C
#define BSC_FIFO  0x10
#define BSC_DIV   0x14
#define C_I2CEN   (1u << 15)
#define C_ST      (1u << 7)     // empezar
#define C_CLEAR   (3u << 4)     // vaciar la cola
#define C_READ    (1u << 0)
#define S_CLKT    (1u << 9)     // un aparato retuvo el reloj demasiado
#define S_ERR     (1u << 8)     // nadie contesto (sin ACK)
#define S_RXD     (1u << 5)     // hay datos en la cola
#define S_TXD     (1u << 4)     // hay sitio en la cola
#define S_DONE    (1u << 1)
#define ESPERA_MAX 20000000     // unos segundos de sondeo, como mucho

static bool divisor_listo = false;

static void preparar_divisor(void) {
    if (divisor_listo) return;
    uint32_t nucleo = mailbox_reloj_pi4(4);          // reloj del nucleo, en Hz
    if (nucleo == 0) nucleo = 500000000u;            // lo habitual en la Pi 4
    uint32_t div = nucleo / 100000u;                 // 100 kHz
    if (div < 2) div = 2;
    BSC_ESCRIBIR(BSC_DIV, div & 0xFFFEu);            // el divisor ha de ser par
    divisor_listo = true;
}

// Una transferencia; devuelve bytes, o error.
static int32_t transferir(uint32_t dir, uint8_t *datos, uint32_t n, bool leer) {
    BSC_ESCRIBIR(BSC_S, S_CLKT | S_ERR | S_DONE);    // limpiar lo de la anterior
    BSC_ESCRIBIR(BSC_C, C_I2CEN | C_CLEAR);
    BSC_ESCRIBIR(BSC_A, dir);
    BSC_ESCRIBIR(BSC_DLEN, n);
    uint32_t i = 0;
    if (!leer) while (i < n && (BSC_LEER(BSC_S) & S_TXD)) BSC_ESCRIBIR(BSC_FIFO, datos[i++]);   // llenar la cola
    BSC_ESCRIBIR(BSC_C, C_I2CEN | C_ST | (leer ? C_READ : 0));
    uint32_t s = 0;
    int espera = 0;
    for (;;) {
        s = BSC_LEER(BSC_S);
        if (leer) { while (i < n && (s & S_RXD)) { datos[i++] = (uint8_t)BSC_LEER(BSC_FIFO); s = BSC_LEER(BSC_S); } }
        else      { while (i < n && (s & S_TXD)) { BSC_ESCRIBIR(BSC_FIFO, datos[i++]); s = BSC_LEER(BSC_S); } }
        if (s & (S_DONE | S_ERR | S_CLKT)) break;
        if (++espera > ESPERA_MAX) break;
    }
    if (leer) while (i < n && (BSC_LEER(BSC_S) & S_RXD)) datos[i++] = (uint8_t)BSC_LEER(BSC_FIFO);   // lo que quede
    BSC_ESCRIBIR(BSC_S, S_CLKT | S_ERR | S_DONE);
    BSC_ESCRIBIR(BSC_C, C_I2CEN | C_CLEAR);
    if (s & S_ERR) return I2C_ERR_NADIE;
    if ((s & S_CLKT) || !(s & S_DONE)) return I2C_ERR_BUS;
    return (int32_t)i;
}

int32_t i2c_escribir(uint32_t dir, const uint8_t *datos, uint32_t n) {
    if (dir > 0x7F || n == 0 || n > I2C_MAX) return I2C_ERR_ARG;
    int32_t r = reclamar_pines(); if (r < 0) return r;
    preparar_divisor();
    return transferir(dir, (uint8_t *)datos, n, false);
}

int32_t i2c_leer(uint32_t dir, uint8_t *datos, uint32_t n) {
    if (dir > 0x7F || n == 0 || n > I2C_MAX) return I2C_ERR_ARG;
    int32_t r = reclamar_pines(); if (r < 0) return r;
    preparar_divisor();
    return transferir(dir, datos, n, true);
}
#endif
