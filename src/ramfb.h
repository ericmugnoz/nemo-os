// ramfb.h — Nemo OS
#ifndef RAMFB_H
#define RAMFB_H

#include <stdint.h>
#include <stdbool.h>

bool ramfb_init(void);

uint32_t fb_width(void);
uint32_t fb_height(void);

// Color en formato 0x00RRGGBB
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t fb_get_pixel(uint32_t x, uint32_t y);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_draw_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t color);
void fb_draw_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t color);
void fb_draw_rect_border(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

// Pega un icono RGBA de tamaño size x size, mezclando transparencia.
void fb_blit_icon(uint32_t x, uint32_t y, uint32_t size, const uint8_t *rgba);

// Copia el fotograma terminado (dibujado con las funciones de arriba)
// al framebuffer real que se muestra en pantalla. Llamar UNA VEZ al
// final de cada fotograma, tras terminar todo el dibujo.
void fb_present(void);

#endif
