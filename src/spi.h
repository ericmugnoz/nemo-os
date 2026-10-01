// spi.h -- Nemo OS: el bus SPI0 de la Raspberry Pi 4. Ver spi.c.
//
// GPIO 11 = reloj (SCLK), 10 = MOSI (sale), 9 = MISO (entra), 8 = CE0 y
// 7 = CE1 (seleccion de cada aparato). Full duplex: por cada byte que
// sale, entra otro.
#pragma once
#include <stdint.h>

#define SPI_MAX      4096        // bytes por transferencia
#define SPI_HZ_MIN   100000      // 100 kHz: 4 KB en unos 330 ms, como mucho
#define SPI_HZ_MAX   50000000    // 50 MHz

#define SPI_ERR_ARG  (-1)        // aparato, modo, velocidad o longitud fuera de rango
                                 // (-2 y -3: los de gpio.h, si sus pines no estan libres)
#define SPI_ERR_BUS  (-7)        // la transferencia no termino

// Envia 'n' bytes de 'tx' al aparato 'chip' (0 = CE0, 1 = CE1) y deja los
// que llegan en 'rx' (NULL: se descartan). 'modo' 0..3 (polaridad y fase
// del reloj, segun la hoja de datos del aparato). Devuelve n, o error.
int32_t spi_transferir(uint32_t chip, uint32_t modo, uint32_t hz,
                       const uint8_t *tx, uint8_t *rx, uint32_t n);
