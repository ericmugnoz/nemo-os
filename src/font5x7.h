// font5x7.h — Nemo OS
// Fuente bitmap propia, 5x7 pixeles por caracter. Cubre espacio,
// digitos, puntuacion basica y mayusculas -- suficiente para titulos
// de ventana, botones y el reloj. Las minusculas se convierten a
// mayusculas automaticamente al dibujar (ver text.c).
#ifndef FONT5X7_H
#define FONT5X7_H

#include <stdint.h>

#define FONT_WIDTH 5
#define FONT_HEIGHT 7

// Indexado por codigo ASCII (0-127). Cada caracter son 7 bytes (uno
// por fila); en cada byte, el bit 4 es la columna mas a la izquierda
// y el bit 0 la mas a la derecha. Los caracteres no definidos quedan
// a cero (en blanco) por inicializacion automatica de C.
extern const uint8_t font5x7[128][7];

#endif
