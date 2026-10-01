// ramfb.h — Nemo OS
#ifndef RAMFB_H
#define RAMFB_H

#include <stdint.h>
#include <stdbool.h>

// want_w/want_h: resolucion deseada (0,0 = usar la de siempre, la mas
// segura). En QEMU se ignoran (ramfb.c usa siempre su tamaño fijo);
// en la Pi 4 (ramfb_pi4.c) se piden de verdad al VideoCore, con
// reintento automatico a la resolucion segura si el firmware no
// puede conceder la pedida.
bool ramfb_init(uint32_t want_w, uint32_t want_h);

uint32_t fb_width(void);
uint32_t fb_height(void);

// Color en formato 0x00RRGGBB
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t fb_get_pixel(uint32_t x, uint32_t y);
// Copia una fila de pixeles al back buffer, recortada (x puede ser negativa).
void fb_blit_row(int32_t x, int32_t y, const uint32_t *src, uint32_t n);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_draw_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t color);
void fb_draw_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t color);
void fb_draw_rect_border(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

// Pega un icono RGBA de tamaño size x size, mezclando transparencia.
void fb_blit_icon(uint32_t x, uint32_t y, uint32_t size, const uint8_t *rgba);
// Igual, pero cada pixel de origen se dibuja como un bloque scale x
// scale (scale=1 es identico a fb_blit_icon).
void fb_blit_icon_scaled(uint32_t x, uint32_t y, uint32_t size, uint32_t scale, const uint8_t *rgba);

// Copia el fotograma terminado (dibujado con las funciones de arriba)
// al framebuffer real que se muestra en pantalla. Llamar UNA VEZ al
// final de cada fotograma, tras terminar todo el dibujo.
void fb_present(void);

// Publica SOLO un rectangulo del bufer de pantalla. Para lo que cambia en un
// trocito y no justifica copiar la pantalla entera -- el cursor del raton, que
// mide 12x19 y se mueve constantemente. Recorta solo; si el rectangulo se sale
// de la pantalla, se queda con la parte que cabe.
void fb_present_rect(int32_t x, int32_t y, int32_t w, int32_t h);

#endif
