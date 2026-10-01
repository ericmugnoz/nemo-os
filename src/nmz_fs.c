// nmz_fs.c -- Nemo OS
// El puente entre el desempaquetador (nmz.c) y NemoFS.
//
// ESTA APARTE A PROPOSITO. nmz.c no incluye nemofs.h ni conoce ningun
// sistema de archivos, y por eso su prueba de host puede compilarlo tal
// cual contra memoria. En cuanto se le metio este puente dentro, la
// prueba dejo de enlazar -- que es exactamente la senal de que se
// estaba perdiendo lo que hacia util esa separacion.
//
// Un archivo de veinte lineas es un precio barato por conservarla.
#include "nmz.h"
#include "nemofs.h"

int32_t nmz_leer_fs(void *ctx, uint32_t off, void *buf, uint32_t len) {
    nmz_ctx_t *c = (nmz_ctx_t *)ctx;
    return nemofs_read_at(c->inodo_paquete, off, buf, len);
}

int32_t nmz_crear_fs(void *ctx, const char *nombre) {
    nmz_ctx_t *c = (nmz_ctx_t *)ctx;
    // Si ya estaba de un paquete anterior, se sustituye. Anadir detras
    // daria un archivo que parece bueno y esta corrupto por delante.
    int32_t viejo = nemofs_find_child(c->dir_destino, nombre);
    if (viejo >= 0) nemofs_delete(c->dir_destino, nombre);
    return nemofs_create(c->dir_destino, nombre, NEMOFS_TYPE_FILE);
}

bool nmz_escribir_fs(void *ctx, int32_t id, const void *buf, uint32_t len) {
    (void)ctx;
    return id >= 0 && nemofs_append((uint32_t)id, buf, len);
}
