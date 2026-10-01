// text.h — Nemo OS
#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>

// 'scale' multiplica el tamaño de cada pixel de la fuente (1 = 5x7
// pixeles reales, 2 = 10x14, etc.)
void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t color, uint32_t scale);
// Igual, pero por codigo ya decodificado (0-255: ASCII y Latin-1).
void fb_draw_codigo(uint32_t x, uint32_t y, uint32_t cod, uint32_t color, uint32_t scale);
// Siguiente caracter de una cadena UTF-8; avanza el puntero.
uint32_t utf8_siguiente(const char **str);
void fb_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t color, uint32_t scale);

// Ancho en pixeles que ocupara un string dibujado con esta escala --
// util para centrar texto o calcular donde poner lo siguiente.
uint32_t text_width(const char *str, uint32_t scale);

#endif
