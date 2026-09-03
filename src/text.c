// text.c — Nemo OS
// Dibuja texto sobre el framebuffer usando font5x7. Solo tenemos
// glifos en mayuscula, asi que las minusculas se convierten
// automaticamente -- una limitacion simple y conocida de esta v1.

#include "text.h"
#include "font5x7.h"
#include "ramfb.h"

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t color, uint32_t scale) {
    char uc = to_upper(c);
    if (uc < 0 || uc > 127) return;

    const uint8_t *rows = font5x7[(int)uc];
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
        fb_draw_char(cursor_x, y, *str, color, scale);
        cursor_x += (FONT_WIDTH + 1) * scale; // +1 columna de espacio entre letras
        str++;
    }
}

uint32_t text_width(const char *str, uint32_t scale) {
    uint32_t len = 0;
    while (str[len]) len++;
    if (len == 0) return 0;
    return len * (FONT_WIDTH + 1) * scale - scale; // sin el espacio sobrante final
}
