// fonts.c -- Nemo OS
// Ver fonts.h y herramientas/nfnt_pack.py (el formato esta documentado ahi).
// Sin coma flotante (el build de QEMU compila con -mgeneral-regs-only) y
// sin libc: la mezcla alpha es aritmetica entera.

#include "fonts.h"

static const uint8_t *g_paquete = 0;
static uint32_t g_longitud = 0;
static uint32_t g_num = 0;
#define MAX_CARAS 64
static nfnt_cara_t g_caras[MAX_CARAS];

static uint32_t leer32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool str_eq8(const char *a, const char *b8) {
    // compara 'a' (C) con un campo de 8 bytes relleno con ceros
    for (int i = 0; i < 8; i++) {
        char c = b8[i];
        if (a[i] == '\0' && c == '\0') return true;
        if (a[i] != c) return false;
    }
    return a[8] == '\0';
}

uint32_t fonts_init(const uint8_t *paquete, uint32_t longitud) {
    g_num = 0; g_paquete = 0; g_longitud = 0;
    if (!paquete || longitud < 16) return 0;
    if (paquete[0] != 'N' || paquete[1] != 'F' || paquete[2] != 'N' || paquete[3] != 'P') return 0;
    uint32_t n = leer32(paquete + 4);
    if (n > MAX_CARAS) n = MAX_CARAS;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = leer32(paquete + 16 + i * 8), len = leer32(paquete + 20 + i * 8);
        if (off + len > longitud || len < sizeof(nfnt_cabecera_t)) continue;
        const nfnt_cabecera_t *c = (const nfnt_cabecera_t *)(paquete + off);
        if (c->magic[0] != 'N' || c->magic[1] != 'F' || c->magic[2] != 'N' || c->magic[3] != 'T') continue;
        if (c->offset_datos > len || (uint32_t)sizeof(nfnt_cabecera_t) + (uint32_t)c->num_glifos * sizeof(nfnt_glifo_t) > c->offset_datos) continue;
        g_caras[g_num].cab = c;
        g_caras[g_num].glifos = (const nfnt_glifo_t *)((const uint8_t *)c + sizeof(nfnt_cabecera_t));
        g_caras[g_num].datos = (const uint8_t *)c + c->offset_datos;
        g_num++;
    }
    g_paquete = paquete; g_longitud = longitud;
    return g_num;
}

uint32_t fonts_num_caras(void) { return g_num; }

static bool familia_existe(const char *familia) {
    for (uint32_t i = 0; i < g_num; i++) if (str_eq8(familia, g_caras[i].cab->familia)) return true;
    return false;
}

const nfnt_cara_t *fonts_find(const char *familia, uint32_t tamano, bool negrita, bool cursiva) {
    if (g_num == 0) return 0;
    if (!familia || !familia_existe(familia)) familia = "sans";
    if (!familia_existe(familia)) familia = g_caras[0].cab->familia; // lo que haya
    // Tres pasadas de tolerancia: estilo exacto; sin cursiva; cualquier estilo.
    for (int pasada = 0; pasada < 3; pasada++) {
        const nfnt_cara_t *mejor = 0;
        uint32_t mejor_dist = 0xFFFFFFFF;
        for (uint32_t i = 0; i < g_num; i++) {
            const nfnt_cara_t *c = &g_caras[i];
            if (!str_eq8(familia, c->cab->familia)) continue;
            bool n = c->cab->negrita != 0, k = c->cab->cursiva != 0;
            if (pasada == 0 && (n != negrita || k != cursiva)) continue;
            if (pasada == 1 && (n != negrita || k)) continue;
            uint32_t d = (c->cab->tamano > tamano) ? c->cab->tamano - tamano : tamano - c->cab->tamano;
            if (d < mejor_dist) { mejor_dist = d; mejor = c; }
        }
        if (mejor) return mejor;
    }
    return &g_caras[0];
}

const nfnt_glifo_t *fonts_glifo(const nfnt_cara_t *cara, uint32_t cp) {
    if (!cara) return 0;
    uint32_t lo = 0, hi = cara->cab->num_glifos;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint32_t c = cara->glifos[mid].codepoint;
        if (c == cp) return &cara->glifos[mid];
        if (c < cp) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static const nfnt_glifo_t *glifo_o_sustituto(const nfnt_cara_t *cara, uint32_t cp) {
    const nfnt_glifo_t *g = fonts_glifo(cara, cp);
    if (!g) g = fonts_glifo(cara, '?');
    return g;
}

uint32_t fonts_alto_linea(const nfnt_cara_t *cara) { return cara ? cara->cab->alto_linea : 0; }
int32_t  fonts_ascent(const nfnt_cara_t *cara)     { return cara ? cara->cab->ascent : 0; }
uint32_t fonts_avance(const nfnt_cara_t *cara, uint32_t cp) {
    const nfnt_glifo_t *g = glifo_o_sustituto(cara, cp);
    return g ? g->avance : 0;
}

uint32_t fonts_utf8_siguiente(const char **s) {
    const uint8_t *p = (const uint8_t *)*s;
    uint8_t b0 = p[0];
    if (b0 < 0x80) { *s += 1; return b0; }
    int n = 0; uint32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) { n = 1; cp = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { n = 2; cp = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { n = 3; cp = b0 & 0x07; }
    else { *s += 1; return b0; }               // byte suelto: Latin-1
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += 1; return b0; } // secuencia rota: Latin-1
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += n + 1;
    return cp;
}

uint32_t fonts_ancho_texto(const nfnt_cara_t *cara, const char *utf8) {
    if (!cara || !utf8) return 0;
    uint32_t w = 0;
    while (*utf8) w += fonts_avance(cara, fonts_utf8_siguiente(&utf8));
    return w;
}

static inline uint32_t mezclar(uint32_t fg, uint32_t bg, uint32_t a) {
    // a: 0..255. Por canal: (fg*a + bg*(255-a)) / 255, con /255 aproximado por (x + (x>>8) + 1) >> 8
    uint32_t ia = 255 - a;
    uint32_t r = ((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * ia;
    uint32_t g = ((fg >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * ia;
    uint32_t b = (fg & 0xFF) * a + (bg & 0xFF) * ia;
    r = (r + (r >> 8) + 1) >> 8; g = (g + (g >> 8) + 1) >> 8; b = (b + (b >> 8) + 1) >> 8;
    return (r << 16) | (g << 8) | b;
}

uint32_t fonts_dibujar(const nfnt_cara_t *cara, int32_t x, int32_t y, const char *utf8, uint32_t color,
                       fonts_get_pixel_t get, fonts_put_pixel_t put, void *ctx) {
    if (!cara || !utf8) return 0;
    int32_t cx = x;
    while (*utf8) {
        uint32_t cp = fonts_utf8_siguiente(&utf8);
        const nfnt_glifo_t *g = glifo_o_sustituto(cara, cp);
        if (!g) continue;
        const uint8_t *bits = cara->datos + g->offset;
        for (uint32_t gy = 0; gy < g->alto; gy++) {
            int32_t py = y + g->dy + (int32_t)gy;
            if (py < 0) continue;
            for (uint32_t gx = 0; gx < g->ancho; gx++) {
                uint8_t a = bits[gy * g->ancho + gx];
                if (a == 0) continue;
                int32_t px = cx + g->dx + (int32_t)gx;
                if (px < 0) continue;
                if (a == 255) put(ctx, px, py, color);
                else put(ctx, px, py, mezclar(color, get(ctx, px, py), a));
            }
        }
        cx += g->avance;
    }
    return (uint32_t)(cx - x);
}
