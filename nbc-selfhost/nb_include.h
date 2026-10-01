// nb_include.h -- Include "archivo.nb"
//
// Antes de compilar, cada linea
//     Include "utiles.nb"
// se sustituye por el contenido de ese archivo (que puede tener sus propios
// Include). El resultado se compila como un solo programa. Para cada linea del
// texto resultante se recuerda de que archivo y de que linea viene, para que
// los errores digan "utiles.nb, linea 12" y no un numero del texto unido.
//
//   - un archivo que ya se incluyo no se incluye otra vez (una sola vez);
//   - un Include en bucle (a incluye b, que incluye a) es un error;
//   - un archivo que no se encuentra es un error, con su nombre.
//
// Lo usan los dos compiladores (nbc.pro en Nemo OS y nbc_driver en el Mac):
// cada uno le pasa su forma de leer archivos.
//
// OJO (nbc.pro): sin tablas estaticas con punteros -- los nombres van en
// arrays de caracteres (ver la nota de nb_firma_t en nb_codegen.c).
#ifndef NB_INCLUDE_H
#define NB_INCLUDE_H
#include <stdint.h>
#include <stdbool.h>

#define NB_INC_MAX_ARCHIVOS 64
#define NB_INC_NOMBRE       32      // como los nombres de NemoFS
#define NB_INC_PROFUNDIDAD  16

// Lee 'nombre' (tal como va entre comillas en el Include). Devuelve un
// buffer reservado con nb_alloc y su largo, o NULL si no existe.
typedef char *(*nb_inc_leer_fn)(const char *nombre, uint32_t *largo, void *ctx);

typedef struct {
    char *texto;                    // el programa entero, ya sin Include (nb_alloc)
    uint32_t largo, cap;
    uint16_t *lin_archivo;          // por cada linea del texto: indice en 'nombres'
    uint32_t *lin_num;              //                           y su linea original (desde 1)
    uint32_t nlineas, cap_lineas;
    char nombres[NB_INC_MAX_ARCHIVOS][NB_INC_NOMBRE];   // [0] = el programa principal
    uint32_t n_nombres;
    char error[160];                // si nb_inc_expandir devuelve false
    uint32_t error_archivo, error_linea;
} nb_inc_t;

// Expande los Include de 'fuente' (el programa principal, 'nombre_principal').
bool nb_inc_expandir(nb_inc_t *r, const char *nombre_principal, const char *fuente, uint32_t largo,
                     nb_inc_leer_fn leer, void *ctx);
// De que archivo y linea viene la linea 'linea' (desde 1) del texto expandido
void nb_inc_origen(const nb_inc_t *r, uint32_t linea, const char **archivo, uint32_t *linea_orig, bool *es_principal);
void nb_inc_libre(nb_inc_t *r);

#endif
