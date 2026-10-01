// descarga.c -- Nemo OS. Ver descarga.h.

#include "descarga.h"
#include "http.h"
#include "nemofs.h"

#define VISTA_MAX DESCARGA_VISTA

static uint8_t  vista[VISTA_MAX];
static uint32_t vista_len;
static uint32_t bajado;
static int32_t  destino = -1;
static bool     fallo_escribir;
static const char *motivo_propio = "";

bool descarga_empezar(const uint8_t ip[4], uint16_t puerto, const char *ruta,
                      uint32_t dir, const char *nombre) {
    if (descarga_estado() == DESCARGA_EN_MARCHA) return false;

    vista_len = 0;
    bajado = 0;
    destino = -1;
    fallo_escribir = false;
    motivo_propio = "";
    http_abandonar();

    if (nombre && nombre[0]) {
        int32_t viejo = nemofs_find_child(dir, nombre);
        if (viejo >= 0) nemofs_delete(dir, nombre);
        destino = nemofs_create(dir, nombre, NEMOFS_TYPE_FILE);
        if (destino < 0) { motivo_propio = "no se pudo crear el archivo"; return false; }
    }

    if (!http_get(ip, puerto, ruta, "nemo")) {
        motivo_propio = "no se pudo empezar";
        return false;
    }
    return true;
}

bool descarga_pedir(const uint8_t ip[4], uint16_t puerto, const char *ruta,
                    const uint8_t *cuerpo, uint32_t cuerpo_len) {
    if (descarga_estado() == DESCARGA_EN_MARCHA) return false;

    vista_len = 0;
    bajado = 0;
    destino = -1;              // a memoria: no se crea ningun archivo
    fallo_escribir = false;
    motivo_propio = "";
    http_abandonar();

    bool bien = cuerpo ? http_post(ip, puerto, ruta, "nemo", cuerpo, cuerpo_len)
                       : http_get(ip, puerto, ruta, "nemo");
    if (!bien) {
        // El motivo de http.c es mas concreto que cualquiera que pueda
        // poner aqui ("la peticion no cabe"), asi que no se pisa: solo se
        // rellena si viene vacio.
        if (!http_motivo()[0]) motivo_propio = "no se pudo empezar";
        return false;
    }
    return true;
}

void descarga_tick(uint64_t ms) {
    http_tick(ms);

    // 4 KB por vuelta, no 512: cada nemofs_append toca el inodo y sus
    // bloques, asi que escribir de ocho en ocho veces menos sale mucho
    // mas barato y no cuesta nada en memoria.
    uint8_t tmp[4096];
    uint32_t n;
    while ((n = http_leer(tmp, sizeof tmp)) > 0) {
        for (uint32_t i = 0; i < n && vista_len < VISTA_MAX; i++) vista[vista_len++] = tmp[i];
        bajado += n;
        if (destino >= 0 && !fallo_escribir) {
            if (!nemofs_append((uint32_t)destino, tmp, n)) {
                // Seguir bajando lo que no se puede guardar es gastar
                // red para tirarla.
                fallo_escribir = true;
                motivo_propio = "no se pudo escribir (disco lleno?)";
                http_abandonar();
            }
        }
    }
}

descarga_estado_t descarga_estado(void) {
    if (fallo_escribir) return DESCARGA_FALLO;
    switch (http_estado()) {
        case HTTP_PARADO: return DESCARGA_PARADA;
        case HTTP_LISTO:  return DESCARGA_LISTA;
        case HTTP_FALLO:  return DESCARGA_FALLO;
        default:          return DESCARGA_EN_MARCHA;
    }
}

uint32_t    descarga_bytes(void)  { return bajado; }
uint32_t    descarga_total(void)  { return http_largo_declarado(); }
uint32_t    descarga_codigo(void) { return http_codigo(); }
int32_t     descarga_inodo(void)  { return destino; }

const char *descarga_motivo(void) {
    // El fallo propio manda sobre el de HTTP: si no se pudo escribir,
    // eso es lo que hay que contar, no que la conexion se abandonara --
    // que fue consecuencia y no causa.
    if (motivo_propio[0]) return motivo_propio;
    return http_motivo();
}

uint32_t descarga_vista(uint8_t *out, uint32_t max) {
    uint32_t n = vista_len < max ? vista_len : max;
    for (uint32_t i = 0; i < n; i++) out[i] = vista[i];
    return n;
}

void descarga_abandonar(void) {
    http_abandonar();
    vista_len = 0;
    bajado = 0;
    destino = -1;
    fallo_escribir = false;
    motivo_propio = "";
}
