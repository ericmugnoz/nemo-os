// nb_include.c -- Include "archivo.nb". Ver nb_include.h.
#include "nb_include.h"
#include <stddef.h>

extern void *nb_alloc(uint32_t n);
extern void nb_free(void *p);

static char minus(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static bool iguales_ci(const char *a, const char *b) {
    while (*a && *b) { if (minus(*a) != minus(*b)) return false; a++; b++; }
    return *a == *b;
}
static void copiar(char *dst, const char *src, uint32_t max) {
    uint32_t i = 0; while (src[i] && i + 1 < max) { dst[i] = src[i]; i++; } dst[i] = 0;
}
static void poner_error(nb_inc_t *r, const char *a, const char *b, const char *c, uint32_t archivo, uint32_t linea) {
    uint32_t k = 0;
    // (se copian una a una, sin una lista de punteros: con -O2 podria acabar
    // como tabla en datos, que nbc.pro no admite)
    for (const char *p = a; p && *p && k < sizeof r->error - 1; p++) r->error[k++] = *p;
    for (const char *p = b; p && *p && k < sizeof r->error - 1; p++) r->error[k++] = *p;
    for (const char *p = c; p && *p && k < sizeof r->error - 1; p++) r->error[k++] = *p;
    r->error[k] = 0;
    r->error_archivo = archivo; r->error_linea = linea;
}

// ---- el texto de salida, y su mapa de lineas ----
static bool asegurar(nb_inc_t *r, uint32_t mas) {
    if (r->largo + mas + 1 <= r->cap) return true;
    uint32_t nc = r->cap ? r->cap : 4096;
    while (r->largo + mas + 1 > nc) nc *= 2;
    char *n = (char *)nb_alloc(nc);
    if (!n) return false;
    for (uint32_t i = 0; i < r->largo; i++) n[i] = r->texto[i];
    if (r->texto) nb_free(r->texto);
    r->texto = n; r->cap = nc;
    return true;
}
static bool anadir_linea(nb_inc_t *r, const char *linea, uint32_t n, uint32_t archivo, uint32_t num) {
    if (!asegurar(r, n + 1)) return false;
    for (uint32_t i = 0; i < n; i++) r->texto[r->largo++] = linea[i];
    r->texto[r->largo++] = '\n';
    r->texto[r->largo] = 0;
    if (r->nlineas == r->cap_lineas) {
        uint32_t nc = r->cap_lineas ? r->cap_lineas * 2 : 256;
        uint16_t *na = (uint16_t *)nb_alloc(nc * (uint32_t)sizeof(uint16_t));
        uint32_t *nn = (uint32_t *)nb_alloc(nc * (uint32_t)sizeof(uint32_t));
        if (!na || !nn) return false;
        for (uint32_t i = 0; i < r->nlineas; i++) { na[i] = r->lin_archivo[i]; nn[i] = r->lin_num[i]; }
        if (r->lin_archivo) nb_free(r->lin_archivo);
        if (r->lin_num) nb_free(r->lin_num);
        r->lin_archivo = na; r->lin_num = nn; r->cap_lineas = nc;
    }
    r->lin_archivo[r->nlineas] = (uint16_t)archivo;
    r->lin_num[r->nlineas] = num;
    r->nlineas++;
    return true;
}

// ¿Es la linea un Include? Devuelve 1 (y el nombre), 0 si no, -1 si esta mal escrito
static int es_include(const char *l, uint32_t n, char *nombre) {
    uint32_t i = 0;
    while (i < n && (l[i] == ' ' || l[i] == '\t')) i++;
    const char *kw = "include";
    for (uint32_t k = 0; k < 7; k++) { if (i + k >= n || minus(l[i + k]) != kw[k]) return 0; }
    i += 7;
    if (i < n && l[i] != ' ' && l[i] != '\t' && l[i] != '"') return 0;   // "Included", "IncludeX": no es
    while (i < n && (l[i] == ' ' || l[i] == '\t')) i++;
    if (i >= n || l[i] != '"') return -1;
    i++;
    uint32_t k = 0;
    while (i < n && l[i] != '"') { if (k + 1 >= NB_INC_NOMBRE) return -1; nombre[k++] = l[i++]; }
    nombre[k] = 0;
    if (i >= n || k == 0) return -1;
    i++;
    while (i < n && (l[i] == ' ' || l[i] == '\t')) i++;
    if (i < n && l[i] != ';') return -1;                 // detras, solo un comentario
    return 1;
}

static bool expandir_archivo(nb_inc_t *r, uint32_t archivo, const char *fuente, uint32_t largo,
                             nb_inc_leer_fn leer, void *ctx, uint32_t *pila, uint32_t profundidad) {
    pila[profundidad] = archivo;
    uint32_t num = 0, i = 0;
    while (i < largo) {
        uint32_t ini = i;
        while (i < largo && fuente[i] != '\n') i++;
        uint32_t fin = i;
        if (fin > ini && fuente[fin - 1] == '\r') fin--;
        if (i < largo) i++;                                  // el salto de linea
        num++;
        char nombre[NB_INC_NOMBRE];
        int inc = es_include(fuente + ini, fin - ini, nombre);
        if (inc < 0) { poner_error(r, "Include espera el nombre de un archivo entre comillas: Include \"utiles.nb\"", "", "", archivo, num); return false; }
        if (inc == 0) { if (!anadir_linea(r, fuente + ini, fin - ini, archivo, num)) goto sin_memoria; continue; }
        // un Include: la linea deja paso al contenido del archivo
        bool en_pila = false;
        for (uint32_t k = 0; k <= profundidad; k++) if (iguales_ci(r->nombres[pila[k]], nombre)) en_pila = true;
        if (en_pila) { poner_error(r, "Include en bucle: ", nombre, " se incluye a si mismo (directa o indirectamente)", archivo, num); return false; }
        bool ya = false;
        for (uint32_t k = 0; k < r->n_nombres; k++) if (iguales_ci(r->nombres[k], nombre)) ya = true;
        if (ya) { if (!anadir_linea(r, "", 0, archivo, num)) goto sin_memoria; continue; }   // una sola vez
        if (r->n_nombres >= NB_INC_MAX_ARCHIVOS) { poner_error(r, "demasiados archivos con Include", "", "", archivo, num); return false; }
        if (profundidad + 1 >= NB_INC_PROFUNDIDAD) { poner_error(r, "demasiados Include uno dentro de otro", "", "", archivo, num); return false; }
        uint32_t lg = 0;
        char *otro = leer(nombre, &lg, ctx);
        if (!otro) { poner_error(r, "Include: no se encuentra ", nombre, "", archivo, num); return false; }
        uint32_t idx = r->n_nombres++;
        copiar(r->nombres[idx], nombre, NB_INC_NOMBRE);
        bool bien = expandir_archivo(r, idx, otro, lg, leer, ctx, pila, profundidad + 1);
        nb_free(otro);
        if (!bien) return false;
    }
    return true;
sin_memoria:
    poner_error(r, "Include: sin memoria para el programa", "", "", archivo, num);
    return false;
}

bool nb_inc_expandir(nb_inc_t *r, const char *nombre_principal, const char *fuente, uint32_t largo,
                     nb_inc_leer_fn leer, void *ctx) {
    r->texto = NULL; r->largo = 0; r->cap = 0;
    r->lin_archivo = NULL; r->lin_num = NULL; r->nlineas = 0; r->cap_lineas = 0;
    r->n_nombres = 1; r->error[0] = 0; r->error_archivo = 0; r->error_linea = 0;
    copiar(r->nombres[0], nombre_principal ? nombre_principal : "", NB_INC_NOMBRE);
    uint32_t pila[NB_INC_PROFUNDIDAD];
    if (!asegurar(r, largo + 64)) { poner_error(r, "Include: sin memoria para el programa", "", "", 0, 0); return false; }
    r->texto[0] = 0;
    return expandir_archivo(r, 0, fuente, largo, leer, ctx, pila, 0);
}

void nb_inc_origen(const nb_inc_t *r, uint32_t linea, const char **archivo, uint32_t *linea_orig, bool *es_principal) {
    if (linea >= 1 && linea <= r->nlineas) {
        uint32_t a = r->lin_archivo[linea - 1];
        *archivo = r->nombres[a]; *linea_orig = r->lin_num[linea - 1]; *es_principal = (a == 0);
    } else {
        *archivo = r->nombres[0]; *linea_orig = linea; *es_principal = true;
    }
}

void nb_inc_libre(nb_inc_t *r) {
    if (r->texto) nb_free(r->texto);
    if (r->lin_archivo) nb_free(r->lin_archivo);
    if (r->lin_num) nb_free(r->lin_num);
    r->texto = NULL; r->lin_archivo = NULL; r->lin_num = NULL;
}
