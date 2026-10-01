// nmz.h -- Nemo OS
// Desempaquetar un .nmz: el paquete con una pagina web dentro que
// devuelve el proxy (ver herramientas/FORMATO_NMZ.md).
//
// El formato, en corto. Todo little-endian:
//
//     0   "NMZ1"
//     4   n_archivos      uint32
//     8   por cada uno:   nombre_len uint16, nombre, tam uint32
//         y despues los contenidos, en el mismo orden y sin relleno.
//
// El primero es siempre index.html.
//
// ESTO NO TOCA EL SISTEMA DE ARCHIVOS. Trabaja contra tres funciones que
// le pasan desde fuera: leer del paquete, crear un archivo y escribir en
// el. En el kernel las pone NemoFS; fuera del kernel, en el ordenador de
// desarrollo, pueden escribir en memoria y ya esta. No es purismo -- es
// lo que permite provocar sin placa todos los paquetes
// retorcidos que un proxy honrado no va a mandar nunca, y son justo los
// que importan: los nombres son texto que viene de fuera, y un nombre
// con '..' dentro es una forma de escribir donde no toca.
#ifndef NMZ_H
#define NMZ_H

#include <stdint.h>
#include <stdbool.h>

// Topes de cordura. Un paquete honrado no se acerca ni de lejos; uno
// corrupto o malintencionado se para aqui en vez de intentar reservar
// cuatro gigabytes porque un campo venia a 0xFFFFFFFF.
#define NMZ_MAX_ARCHIVOS 64
#define NMZ_MAX_ARCHIVO  (8u * 1024 * 1024)
#define NMZ_MAX_NOMBRE   31

typedef struct {
    // Lee 'len' bytes desde el desplazamiento 'off' del paquete.
    // Devuelve cuantos leyo de verdad, o negativo si fallo.
    int32_t (*leer)(void *ctx, uint32_t off, void *buf, uint32_t len);
    // Crea (o sustituye) un archivo y devuelve un identificador, o -1.
    int32_t (*crear)(void *ctx, const char *nombre);
    // Anade al final del archivo.
    bool (*escribir)(void *ctx, int32_t id, const void *buf, uint32_t len);
    void *ctx;
} nmz_io_t;

// Motivo del ultimo fallo, en una linea.
const char *nmz_motivo(void);

// Desempaqueta. Devuelve cuantos archivos escribio, o -1 si el paquete
// no es valido. Si 'primero' no es NULL, deja ahi el nombre del primer
// archivo -- el que hay que abrir.
int32_t nmz_desempaquetar(const nmz_io_t *io, uint32_t tam_paquete,
                          char *primero, uint32_t primero_max);

// ---------------------------------------------------------------------
// El puente con NemoFS. Vive aqui y no en syscall.c porque es parte de
// como se desempaqueta, no de como se llama a un syscall.
// ---------------------------------------------------------------------
typedef struct { uint32_t inodo_paquete; uint32_t dir_destino; } nmz_ctx_t;

int32_t nmz_leer_fs(void *ctx, uint32_t off, void *buf, uint32_t len);
int32_t nmz_crear_fs(void *ctx, const char *nombre);
bool    nmz_escribir_fs(void *ctx, int32_t id, const void *buf, uint32_t len);

// true si el nombre se puede escribir sin peligro: ni rutas, ni
// referencias a la carpeta de arriba, ni vacio, ni demasiado largo.
// Publica porque merece probarse sola.
bool nmz_nombre_seguro(const char *n, uint32_t largo);

#endif
