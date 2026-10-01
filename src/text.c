// text.c — Nemo OS
// Dibuja texto sobre el framebuffer usando font5x7.
//
// La fuente ya tiene minusculas y los caracteres del
// español, asi que se dibuja lo que viene: antes se convertia todo a
// mayusculas porque no habia glifos, y la interfaz entera salia en caja
// alta sin que nadie lo hubiera decidido.
//
// Las cadenas llegan en UTF-8 (es lo que escriben los programas y lo que
// guardan los archivos). La ñ son DOS bytes, no uno: sin decodificar,
// una palabra con ñ se dibujaba con dos simbolos raros y ademas descuadraba
// el ancho, que es lo que rompe cualquier texto centrado.

#include "text.h"
#include "font5x7.h"
#include "ramfb.h"

// Siguiente caracter de una cadena UTF-8. Avanza 'str' y devuelve el
// codigo; solo se traduce lo que la fuente puede dibujar (hasta 0xFF).
uint32_t utf8_siguiente(const char **str) {
    const unsigned char *p = (const unsigned char *)*str;
    unsigned char c = *p;
    if (c < 0x80) { *str = (const char *)(p + 1); return c; }
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        uint32_t v = ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
        *str = (const char *)(p + 2);
        return v;
    }
    // Tres bytes: ahi viven las comillas de imprenta, la vinyeta y las
    // flechas, que la fuente SI tiene (en su tabla aparte).
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        uint32_t v = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *str = (const char *)(p + 3);
        return v;
    }
    // Cuatro bytes (emoji y demas): se salta entero y sale un hueco. Mejor
    // un espacio que media letra y el resto de la linea descolocada.
    if ((c & 0xF8) == 0xF0) { *str = (const char *)(p + 4); return ' '; }
    *str = (const char *)(p + 1);
    return ' ';
}

void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t color, uint32_t scale) {
    fb_draw_codigo(x, y, (unsigned char)c, color, scale);
}

void fb_draw_codigo(uint32_t x, uint32_t y, uint32_t cod, uint32_t color, uint32_t scale) {
    const uint8_t *rows = font5x7_glifo(cod);
    if (!rows) return;

    for (int ry = 0; ry < FONT_HEIGHT; ry++) {
        uint8_t bits = rows[ry];
        for (int rx = 0; rx < FONT_WIDTH; rx++) {
            if (bits & (1 << (FONT_WIDTH - 1 - rx))) {
                fb_fill_rect(x + rx * scale, y + ry * scale, scale, scale, color);
            }
        }
    }
}

void fb_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t color, uint32_t scale) {
    uint32_t cursor_x = x;
    while (*str) {
        uint32_t cod = utf8_siguiente(&str);
        fb_draw_codigo(cursor_x, y, cod, color, scale);
        cursor_x += (FONT_WIDTH + 1) * scale; // +1 columna de espacio entre letras
    }
}

uint32_t text_width(const char *str, uint32_t scale) {
    // Se cuentan CARACTERES, no bytes: en UTF-8 la ñ son dos bytes y
    // contarlos daria un ancho de mas, con lo que todo lo centrado se
    // descoloca.
    uint32_t len = 0;
    while (*str) { utf8_siguiente(&str); len++; }
    if (len == 0) return 0;
    return len * (FONT_WIDTH + 1) * scale - scale; // sin el espacio sobrante final
}
