// spi.c -- Nemo OS: el bus SPI0 de la Raspberry Pi 4. Ver spi.h.
//
// El programa que lo usa se queda los pines 9, 10, 11 y el selector de
// su aparato (8 para CE0, 7 para CE1), con las reglas de dueño de
// gpio.c; al terminar, vuelven a ser entradas.
//
// En QEMU no hay bus: se SIMULAN dos aparatos. En CE0, un conversor
// analogico MCP3008 (8 canales de 10 bits): a la orden {1, 0x80 | canal
// << 4, 0} responde con el valor en los dos ultimos bytes; los valores
// simulados son fijos por canal (canal * 128 + 37) para poder comprobar
// un programa. En CE1, un "espejo": devuelve lo mismo que recibe. Cada
// transferencia se anuncia por la terminal.
#include <stdint.h>
#include <stdbool.h>
#include "spi.h"
#include "gpio.h"

void uart_puts(const char *s);

static int32_t reclamar_pines(uint32_t chip) {
    static const int32_t comunes[3] = { 9, 10, 11 };
    for (int i = 0; i < 3; i++) { int32_t r = gpio_reclamar(comunes[i], GPIO_ALT0); if (r < 0) return r; }
    return gpio_reclamar(chip == 0 ? 8 : 7, GPIO_ALT0);
}

static bool argumentos_validos(uint32_t chip, uint32_t modo, uint32_t hz, uint32_t n) {
    return chip <= 1 && modo <= 3 && hz >= SPI_HZ_MIN && hz <= SPI_HZ_MAX && n >= 1 && n <= SPI_MAX;
}

#ifdef NEMO_QEMU
// ---- QEMU: un MCP3008 en CE0 y un espejo en CE1 ----
uint32_t spi_simulado_valor(uint32_t canal) { return (canal * 128u + 37u) & 1023u; }

static void anunciar(uint32_t chip, uint32_t n) {
    char t[80]; int k = 0;
    for (const char *a = "spi (simulado): "; *a; a++) t[k++] = *a;
    char d[12]; int m = 0; uint32_t v = n; do { d[m++] = (char)('0' + v % 10); v /= 10; } while (v); while (m) t[k++] = d[--m];
    for (const char *a = chip == 0 ? " bytes con CE0 (conversor MCP3008)" : " bytes con CE1 (espejo)"; *a; a++) t[k++] = *a;
    t[k++] = '\n'; t[k] = 0;
    uart_puts(t);
}

int32_t spi_transferir(uint32_t chip, uint32_t modo, uint32_t hz, const uint8_t *tx, uint8_t *rx, uint32_t n) {
    if (!argumentos_validos(chip, modo, hz, n)) return SPI_ERR_ARG;
    int32_t r = reclamar_pines(chip); if (r < 0) return r;
    anunciar(chip, n);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t entra = 0;
        if (chip == 1) entra = tx[i];                         // el espejo
        else if (i >= 2 && tx[0] == 1 && (tx[1] & 0x80)) {    // MCP3008, lectura simple
            uint32_t valor = spi_simulado_valor((tx[1] >> 4) & 7u);
            entra = (i == 2) ? (uint8_t)(valor & 0xFF) : 0;
        } else if (i == 1 && tx[0] == 1 && (tx[1] & 0x80)) {
            entra = (uint8_t)((spi_simulado_valor((tx[1] >> 4) & 7u) >> 8) & 3u);
        }
        if (rx) rx[i] = entra;
    }
    return (int32_t)n;
}

#else
// ---- Raspberry Pi 4: SPI0 ----
#ifndef SPI_LEER
#define SPI0_BASE 0xFE204000UL
#define SPI_LEER(off)        (*(volatile uint32_t *)(uintptr_t)(SPI0_BASE + (off)))
#define SPI_ESCRIBIR(off, v) (*(volatile uint32_t *)(uintptr_t)(SPI0_BASE + (off)) = (v))
#endif
#include "mailbox_pi4.h"

#define SPI_CS    0x00
#define SPI_FIFO  0x04
#define SPI_CLK   0x08
#define CS_CPHA   (1u << 2)
#define CS_CPOL   (1u << 3)
#define CS_CLEAR  (3u << 4)     // vaciar las dos colas
#define CS_TA     (1u << 7)     // transferencia activa (baja el selector)
#define CS_DONE   (1u << 16)
#define CS_RXD    (1u << 17)    // hay datos recibidos
#define CS_TXD    (1u << 18)    // hay sitio para enviar
#define ESPERA_MAX 20000000

static uint32_t reloj_nucleo = 0;

int32_t spi_transferir(uint32_t chip, uint32_t modo, uint32_t hz, const uint8_t *tx, uint8_t *rx, uint32_t n) {
    if (!argumentos_validos(chip, modo, hz, n)) return SPI_ERR_ARG;
    int32_t r = reclamar_pines(chip); if (r < 0) return r;
    if (reloj_nucleo == 0) { reloj_nucleo = mailbox_reloj_pi4(4); if (reloj_nucleo == 0) reloj_nucleo = 500000000u; }
    uint32_t div = (reloj_nucleo + hz - 1) / hz;             // redondeando hacia arriba: nunca mas rapido de lo pedido
    if (div & 1u) div++;                                     // el divisor ha de ser par
    if (div < 2) div = 2;
    if (div > 65534) div = 65534;
    SPI_ESCRIBIR(SPI_CLK, div);
    uint32_t base = chip | ((modo & 1u) ? CS_CPHA : 0) | ((modo & 2u) ? CS_CPOL : 0);
    SPI_ESCRIBIR(SPI_CS, base | CS_CLEAR);
    SPI_ESCRIBIR(SPI_CS, base | CS_TA);
    uint32_t enviados = 0, recibidos = 0;
    int espera = 0;
    while (recibidos < n) {
        uint32_t s = SPI_LEER(SPI_CS);
        // no adelantarse mas de 16 bytes a lo recibido: la cola de entrada no se desborda
        while (enviados < n && enviados - recibidos < 16 && (s & CS_TXD)) { SPI_ESCRIBIR(SPI_FIFO, tx[enviados++]); s = SPI_LEER(SPI_CS); }
        while (recibidos < n && (s & CS_RXD)) {
            uint8_t b = (uint8_t)SPI_LEER(SPI_FIFO);
            if (rx) rx[recibidos] = b;
            recibidos++;
            s = SPI_LEER(SPI_CS);
        }
        if (++espera > ESPERA_MAX) break;
    }
    espera = 0;
    while (!(SPI_LEER(SPI_CS) & CS_DONE) && ++espera < ESPERA_MAX) { }
    SPI_ESCRIBIR(SPI_CS, base);                              // fin: sube el selector
    return recibidos == n ? (int32_t)n : SPI_ERR_BUS;
}
#endif
