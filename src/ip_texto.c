// ip_texto.c -- Nemo OS. Ver ip_texto.h.

#include "ip_texto.h"

bool ip_desde_texto(const char *s, uint8_t out[4]) {
    if (!s) return false;

    uint32_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;

    uint8_t v[4];
    for (int parte = 0; parte < 4; parte++) {
        if (s[i] < '0' || s[i] > '9') return false;

        // Ceros por delante: se rechazan. "010" lo lee unas herramientas
        // como 8 (octal) y otras como 10, y de esa ambiguedad se ha
        // tirado para colar direcciones que parecen otras. Un "0" solo
        // si vale, que es un numero legitimo.
        if (s[i] == '0' && s[i + 1] >= '0' && s[i + 1] <= '9') return false;

        uint32_t n = 0, cifras = 0;
        while (s[i] >= '0' && s[i] <= '9') {
            n = n * 10 + (uint32_t)(s[i] - '0');
            i++;
            if (++cifras > 3) return false;   // ningun numero de una IP tiene cuatro cifras
        }
        if (n > 255) return false;
        v[parte] = (uint8_t)n;

        if (parte < 3) {
            if (s[i] != '.') return false;    // faltan trozos
            i++;
        }
    }

    while (s[i] == ' ' || s[i] == '\t') i++;
    // Lo que sobre detras invalida el conjunto: "1.2.3.4.5" no es una IP
    // con un adorno, es otra cosa. Aceptarla leyendo solo el principio
    // seria adivinar.
    if (s[i] != '\0') return false;

    for (int k = 0; k < 4; k++) out[k] = v[k];
    return true;
}

uint32_t ip_a_texto(const uint8_t ip[4], char *out, uint32_t max) {
    if (!out || max < IP_TEXTO_MAX) {
        // Escribir lo que quepa daria una IP distinta y con pinta de
        // buena: "192.168.1.40" recortado a ocho letras es "192.168.",
        // y peor, a nueve es "192.168.1" -- que ip_desde_texto rechaza,
        // pero un ojo humano lee como una red. Mejor nada.
        if (out && max > 0) out[0] = '\0';
        return 0;
    }
    uint32_t k = 0;
    for (int parte = 0; parte < 4; parte++) {
        uint8_t n = ip[parte];
        if (n >= 100) out[k++] = (char)('0' + n / 100);
        if (n >= 10)  out[k++] = (char)('0' + (n / 10) % 10);
        out[k++] = (char)('0' + n % 10);
        if (parte < 3) out[k++] = '.';
    }
    out[k] = '\0';
    return k;
}
