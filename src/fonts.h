// fonts.h -- Nemo OS
// Fuentes proporcionales pre-rasterizadas (formato NFNT/NFNP, ver
// herramientas/nfnt_pack.py). Sustituyen a la 5x7 de font5x7.c para el
// texto de los programas: antialiasing, anchura por glifo, Latin-1
// completo (acentos, ñ, ¿¡), negrita y cursiva de verdad, varios
// tamaños. La 5x7 sigue existiendo como fuente "sistema" y para el
// propio escritorio (titulos, gadgets), que no cambia con esto.
#ifndef FONTS_H
#define FONTS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t codepoint;
    uint8_t avance, ancho, alto;
    int8_t dx, dy;
    uint8_t pad[3];
    uint32_t offset;
} __attribute__((packed)) nfnt_glifo_t;

typedef struct {
    char magic[4];
    uint16_t version;
    char familia[8];
    uint16_t tamano;
    uint16_t alto_linea;
    int16_t ascent;
    uint8_t negrita, cursiva;
    uint16_t num_glifos;
    uint32_t offset_datos;
    uint32_t reservado;
} __attribute__((packed)) nfnt_cabecera_t;

typedef struct {
    const nfnt_cabecera_t *cab;
    const nfnt_glifo_t *glifos;
    const uint8_t *datos;
} nfnt_cara_t;

// Carga el paquete (en el sitio, sin copiar: el blob tiene que seguir
// existiendo). Devuelve el numero de caras, 0 si no es un paquete valido.
uint32_t fonts_init(const uint8_t *paquete, uint32_t longitud);
uint32_t fonts_num_caras(void);

// Busca la cara mas parecida: familia exacta (o "sans" si no existe),
// estilo exacto si lo hay (si no hay cursiva de esa familia, se usa la
// recta del mismo peso), y el tamaño MAS CERCANO al pedido. NULL solo
// si no hay ningun paquete cargado.
const nfnt_cara_t *fonts_find(const char *familia, uint32_t tamano, bool negrita, bool cursiva);

const nfnt_glifo_t *fonts_glifo(const nfnt_cara_t *cara, uint32_t codepoint);
uint32_t fonts_alto_linea(const nfnt_cara_t *cara);
int32_t  fonts_ascent(const nfnt_cara_t *cara);
uint32_t fonts_avance(const nfnt_cara_t *cara, uint32_t codepoint); // 0 si no hay glifo ni sustituto

// Ancho en pixeles de una cadena UTF-8 (bytes invalidos se toman como Latin-1).
uint32_t fonts_ancho_texto(const nfnt_cara_t *cara, const char *utf8);

// Dibuja una cadena UTF-8 con (x, y) = esquina superior izquierda de la
// linea, mezclando con lo que ya hay (antialiasing) a traves de los
// callbacks -- asi sirve igual para un bufer de ventana del kernel que
// para una prueba en el host. Devuelve el ancho avanzado.
typedef uint32_t (*fonts_get_pixel_t)(void *ctx, int32_t x, int32_t y);
typedef void     (*fonts_put_pixel_t)(void *ctx, int32_t x, int32_t y, uint32_t color);
uint32_t fonts_dibujar(const nfnt_cara_t *cara, int32_t x, int32_t y, const char *utf8, uint32_t color,
                       fonts_get_pixel_t get, fonts_put_pixel_t put, void *ctx);

// Decodifica el siguiente codepoint UTF-8 de *s, avanzando el puntero.
// Un byte que no forma UTF-8 valido se devuelve tal cual (Latin-1).
uint32_t fonts_utf8_siguiente(const char **s);

#endif
