// i2c.h -- Nemo OS: el bus I2C 1 (GPIO 2 = SDA, GPIO 3 = SCL), a 100 kHz. Ver i2c.c.
#pragma once
#include <stdint.h>

#define I2C_MAX 256            // bytes por transferencia (a 100 kHz, unos 23 ms)

#define I2C_ERR_ARG   (-1)     // direccion de mas de 7 bits, o longitud 0 o de mas de 256
                               // (-2 y -3: los de gpio.h, si los pines 2/3 no estan libres)
#define I2C_ERR_NADIE (-6)     // nadie contesto en esa direccion
#define I2C_ERR_BUS   (-7)     // el bus no termino (reloj retenido, o colgado)

// Devuelven los bytes transferidos, o un error.
int32_t i2c_escribir(uint32_t dir, const uint8_t *datos, uint32_t n);
int32_t i2c_leer(uint32_t dir, uint8_t *datos, uint32_t n);
