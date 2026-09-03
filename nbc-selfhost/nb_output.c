// nb_output.c — ver nb_output.h

#include "nb_output.h"
#include "nblibc.h"
#include <stdarg.h>

NBOut nb_stdout_placeholder;

static char *g_buf;
static uint32_t g_capacity;
static uint32_t g_pos;

void nb_output_begin(char *buffer, uint32_t capacity) {
    g_buf = buffer;
    g_capacity = capacity;
    g_pos = 0;
    if (g_capacity > 0) g_buf[0] = '\0';
}

uint32_t nb_output_length(void) { return g_pos; }

static void out_putc(char c) {
    if (g_pos + 1 < g_capacity) { // deja sitio siempre para el '\0' final
        g_buf[g_pos++] = c;
        g_buf[g_pos] = '\0';
    }
}

static void out_puts(const char *s) {
    while (*s != '\0') out_putc(*s++);
}

static void out_put_int64(int64_t v) {
    if (v < 0) { out_putc('-'); v = -v; }
    char digits[24];
    int n = 0;
    if (v == 0) { out_putc('0'); return; }
    while (v > 0 && n < 24) { digits[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) out_putc(digits[--n]);
}

void nb_fprintf(NBOut *out, const char *fmt, ...) {
    (void)out; // siempre escribimos al buffer activo, ver nb_output_begin
    va_list args;
    va_start(args, fmt);

    for (uint32_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') { out_putc(fmt[i]); continue; }
        i++;
        if (fmt[i] == 's') {
            out_puts(va_arg(args, const char *));
        } else if (fmt[i] == 'd') {
            out_put_int64(va_arg(args, int)); // los "int" varargs se promocionan a int de todos modos
        } else if (fmt[i] == 'l' && fmt[i+1] == 'l' && fmt[i+2] == 'd') {
            out_put_int64(va_arg(args, long long));
            i += 2;
        } else if (fmt[i] == '%') {
            out_putc('%');
        } else {
            // especificador no soportado -- lo dejamos tal cual para
            // que sea facil de detectar si algun dia hace falta
            out_putc('%');
            out_putc(fmt[i]);
        }
    }

    va_end(args);
}
