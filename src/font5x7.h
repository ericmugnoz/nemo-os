// font5x7.h — Nemo OS
// Fuente bitmap propia, 5x7 pixeles por caracter. Desde
// cubre el ASCII imprimible ENTERO (mayusculas, minusculas, digitos y
// signos) y los caracteres del español en sus posiciones de Latin-1:
// vocales acentuadas, ñ, Ñ, ü, Ü, ¿, ¡, º, ª, ç y las comillas
// angulares. Antes solo tenia mayusculas y el kernel convertia las
// minusculas al dibujar -- por eso la interfaz salia toda en caja alta.
#ifndef FONT5X7_H
#define FONT5X7_H

#include <stdint.h>

#define FONT_WIDTH 5
#define FONT_HEIGHT 7

// Indexado por codigo (0-255: ASCII abajo, Latin-1 arriba). Cada
// caracter son 7 bytes (uno
// por fila); en cada byte, el bit 4 es la columna mas a la izquierda
// y el bit 0 la mas a la derecha. Los caracteres no definidos quedan
// a cero (en blanco) por inicializacion automatica de C.
extern const uint8_t font5x7[256][7];

// Los signos de encima de 255 (comillas de imprenta, flechas, euro...)
// no caben en una tabla indexada por byte: van aparte.
typedef struct { uint32_t cod; uint8_t filas[7]; } glifo_extra_t;
extern const glifo_extra_t font5x7_extras[];
extern const uint32_t font5x7_num_extras;

// El glifo de un codigo, mire donde mire. NULL si no existe.
const uint8_t *font5x7_glifo(uint32_t cod);

#endif
