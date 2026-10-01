// nb_convert.c — conversiones numero<->cadena de Nemo-Blitz 2.0
// (Str$ y Val). Mismo estilo que nb_string.c: sin libc, con
// nb_alloc/nb_string_new por debajo.
//
// Simplificacion deliberada en nb_float_to_str: SIEMPRE 6 decimales
// fijos (como un printf "%.6f" clasico), sin intentar el "minimo de
// digitos que representa el valor exacto" de un formateador de coma
// flotante maduro (eso es un problema bastante mas delicado --
// algoritmos como Grisu/Ryu existen precisamente porque hacerlo bien
// no es trivial). Documentado aqui, no oculto: 3.5 se muestra como
// "3.500000", no "3.5".

#include <stdint.h>
#include <stdbool.h>

typedef struct nb_string nb_string_t;
extern nb_string_t *nb_string_new(const char *data, uint32_t len);
extern uint32_t nb_string_len(nb_string_t *s);
extern const char *nb_string_cstr(nb_string_t *s);

// ---- Str$ ----

nb_string_t *nb_int_to_str(int64_t v) {
    char buf[24];
    int32_t pos = 24;
    bool neg = v < 0;
    // uint64_t evita el desbordamiento de negar INT64_MIN (que no
    // tiene positivo equivalente representable en int64_t).
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (u == 0) buf[--pos] = '0';
    while (u > 0) {
        buf[--pos] = (char)('0' + (u % 10));
        u /= 10;
    }
    if (neg) buf[--pos] = '-';
    return nb_string_new(buf + pos, (uint32_t)(24 - pos));
}

nb_string_t *nb_float_to_str(double v) {
    bool neg = v < 0;
    double av = neg ? -v : v;
    int64_t ip = (int64_t)av; // parte entera (trunca hacia 0 -- av ya es positivo)
    double frac = av - (double)ip;
    int64_t fp = (int64_t)(frac * 1000000.0 + 0.5); // redondeo al sexto decimal
    if (fp >= 1000000) { fp -= 1000000; ip += 1; }   // acarreo (ej. 0.9999995 -> 1.000000)

    char buf[64];
    int32_t pos = 64;
    for (int32_t i = 0; i < 6; i++) {
        buf[--pos] = (char)('0' + (fp % 10));
        fp /= 10;
    }
    buf[--pos] = '.';
    if (ip == 0) buf[--pos] = '0';
    while (ip > 0) {
        buf[--pos] = (char)('0' + (ip % 10));
        ip /= 10;
    }
    if (neg) buf[--pos] = '-';
    return nb_string_new(buf + pos, (uint32_t)(64 - pos));
}

// ---- Val ----
//
// Las dos ignoran espacios/tabulaciones al principio, aceptan un
// signo opcional, y se detienen en el primer caracter que ya no
// encaja -- igual que el Val() de BlitzPlus real (no es un error leer
// "12abc", da 12 y para ahi).

int64_t nb_str_to_int(nb_string_t *s) {
    const char *p = nb_string_cstr(s);
    uint32_t len = nb_string_len(s);
    uint32_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    bool neg = false;
    if (i < len && (p[i] == '+' || p[i] == '-')) { neg = (p[i] == '-'); i++; }
    int64_t v = 0;
    while (i < len && p[i] >= '0' && p[i] <= '9') { v = v * 10 + (p[i] - '0'); i++; }
    return neg ? -v : v;
}

double nb_str_to_float(nb_string_t *s) {
    const char *p = nb_string_cstr(s);
    uint32_t len = nb_string_len(s);
    uint32_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    bool neg = false;
    if (i < len && (p[i] == '+' || p[i] == '-')) { neg = (p[i] == '-'); i++; }
    double v = 0.0;
    while (i < len && p[i] >= '0' && p[i] <= '9') { v = v * 10.0 + (double)(p[i] - '0'); i++; }
    if (i < len && p[i] == '.') {
        i++;
        double scale = 0.1;
        while (i < len && p[i] >= '0' && p[i] <= '9') {
            v += (double)(p[i] - '0') * scale;
            scale *= 0.1;
            i++;
        }
    }
    return neg ? -v : v;
}
