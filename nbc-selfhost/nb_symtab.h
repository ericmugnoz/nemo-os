// nb_symtab.h — tabla de símbolos de Nemo-Blitz 2.0.
//
// Dos tablas, con un problema de fondo distinto cada una:
//
// GLOBALES: viven en una región de datos que se coloca justo DESPUÉS
// de todo el código, una vez se sabe su tamaño final -- así que
// cuando el código necesita la dirección de una global (adrp+add),
// esa dirección relativa (página de destino menos página de la
// propia instrucción adrp) TODAVÍA NO SE PUEDE CALCULAR mientras se
// genera el código: no se sabe cuánto código habrá detrás. Es el
// mismo problema que un salto hacia una etiqueta que aún no existe
// (nb_codebuf.h) -- aquí se resuelve igual: se anota cada referencia
// pendiente, y se corrigen TODAS de una vez cuando el tamaño del
// código por fin se conoce (nb_symtab_resolve_globals).
//
// LOCALES: viven en la pila de la propia función, en un
// desplazamiento fijo respecto al puntero de marco (x29) que SÍ se
// conoce en el momento de generar el acceso -- no hace falta ningún
// parcheo diferido. Se reinicia una tabla local nueva por cada
// función que se compila (los nombres no se comparten entre
// funciones distintas).

#ifndef NB_SYMTAB_H
#define NB_SYMTAB_H

#include <stdint.h>
#include <stdbool.h>
#include "nb_codebuf.h"

typedef struct nb_string nb_string_t;

// ---- Globales ----

typedef struct {
    nb_string_t *name;   // nombre de la variable, con su sufijo $/# incluido (son nombres distintos)
    uint32_t offset;      // desplazamiento en bytes dentro de la region de datos
} nb_global_entry_t;

// Cada vez que el codigo generado necesita la DIRECCION de una
// global, se emite un par adrp+add con inmediatos en CERO (de
// relleno) y se anota aqui -- nb_symtab_resolve_globals corrige los
// dos, uno por uno, cuando el tamaño del codigo ya se conoce.
typedef struct {
    uint32_t adrp_pos;    // posicion (en palabras) de la instruccion adrp
    uint32_t add_pos;     // posicion de la instruccion add :lo12: inmediatamente despues
    uint32_t reg;         // registro usado en las dos instrucciones (el mismo en ambas)
    uint32_t var_offset;  // desplazamiento de la variable dentro de la region de datos
} nb_global_patch_t;

typedef struct {
    uint32_t offset;
    uint8_t *bytes; // copia propia, reservada con nb_alloc
    uint32_t len;
} nb_literal_entry_t;

typedef struct {
    uint32_t offset_to_write;
    uint32_t target_offset;
} nb_data_patch_t;

typedef struct {
    nb_global_entry_t *entries;
    uint32_t count, cap;

    nb_global_patch_t *patches;
    uint32_t patch_count, patch_cap;

    nb_literal_entry_t *literals;
    uint32_t literal_count, literal_cap;

    nb_data_patch_t *data_patches;
    uint32_t data_patch_count, data_patch_cap;

    uint32_t next_offset; // siguiente hueco libre en la region de datos, en bytes

    // Posicion (bytes desde el inicio del codigo) donde vive de
    // verdad la region de datos dentro del .pro. Antes los datos iban
    // siempre AL FINAL de todo, detras del bloque de runtime y de sus
    // megas de .bss; ahora van justo detras del codigo y ANTES del
    // bloque, para que el .bss quede lo ultimo y no haga falta
    // escribirlo en el archivo (el area de cada tarea ya viene a cero
    // desde task_spawn_from_file). 0 = sin fijar todavia.
    uint32_t data_region_pos;
} nb_symtab_t;

void nb_symtab_init(nb_symtab_t *st);
void nb_symtab_free(nb_symtab_t *st);

// Busca una global ya declarada por nombre -- NULL si no existe
// todavia. Comparacion de CONTENIDO de la cadena (nb_string_eq), no
// de puntero -- dos nb_string_t con el mismo texto pero reservas
// distintas deben encontrarse igual.
nb_global_entry_t *nb_symtab_find_global(nb_symtab_t *st, nb_string_t *name);

// Declara una global nueva (asume que no existe ya -- comprobarlo con
// nb_symtab_find_global antes es responsabilidad del llamador) y le
// asigna el siguiente hueco de 8 bytes en la region de datos. Se
// reclama 'name' -- la tabla pasa a ser su dueña.
nb_global_entry_t *nb_symtab_add_global(nb_symtab_t *st, nb_string_t *name);

// Reserva 'len' bytes en la MISMA region de datos que las globales
// (comparten un unico contador de posicion -- se intercalan segun el
// orden en que van apareciendo en el codigo fuente, sin que eso
// importe: nb_symtab_build_data_image reconstruye la imagen completa
// al final, con el contenido de cada literal en su sitio exacto y
// ceros en el resto). El llamador conserva la propiedad de 'bytes'
// (se copia aqui dentro). Devuelve el desplazamiento asignado, para
// pedir su direccion despues con nb_emit_global_addr igual que con
// cualquier otra cosa de la region de datos.
uint32_t nb_symtab_add_literal_bytes(nb_symtab_t *st, const uint8_t *bytes, uint32_t len);

// Construye la imagen COMPLETA de la region de datos, del tamaño que
// ocupa hasta ahora (el mismo que ve cualquier direccion ya resuelta
// con nb_emit_global_addr): el contenido real de cada literal en su
// desplazamiento exacto, y CERO en el resto de huecos (los que
// corresponden a variables globales, que no necesitan contenido
// inicial -- lo que el cargador ya entrega a cero les vale). El
// llamador es dueño del array devuelto (reservado con nb_alloc) y
// debe liberarlo con nb_free cuando termine de escribirlo en el
// binario final. *out_len recibe el tamaño.
uint8_t *nb_symtab_build_data_image(nb_symtab_t *st, uint32_t code_size_words, uint32_t *out_len);

// Reserva un "parcheo dato-sobre-dato": cuando se construya la imagen
// final (nb_symtab_build_data_image), en el desplazamiento
// 'offset_to_write' se escribira la DIRECCION ABSOLUTA de otro punto
// de la propia region de datos ('target_offset'), calculada ya con el
// tamaño final del codigo -- para cuando algo dentro de los propios
// datos (un valor de Data que es una cadena, por ejemplo) necesita
// guardar la direccion de otro dato, no un valor fijo. A diferencia
// de una direccion en CODIGO (adrp+add), esto es simplemente escribir
// 8 bytes en la imagen -- no hace falta codificar ninguna instruccion,
// solo saber el numero final.
void nb_symtab_add_data_patch(nb_symtab_t *st, uint32_t offset_to_write, uint32_t target_offset);

// Emite adrp+add hacia la global de 'offset' dentro de la region de
// datos, con inmediatos en cero, y registra el parcheo pendiente. El
// resultado (la DIRECCION) queda en el registro 'reg' indicado.
void nb_emit_global_addr(nb_symtab_t *st, nb_codebuf_t *cb, uint32_t reg, uint32_t var_offset);

// Corrige TODOS los parcheos pendientes de una vez, ahora que
// 'code_size_words' (el tamaño final de todo el codigo, en palabras
// de 4 bytes) ya se conoce -- la region de datos empieza justo
// despues. Se llama UNA VEZ, al terminar de generar todo el codigo
// del programa.
void nb_symtab_resolve_globals(nb_symtab_t *st, nb_codebuf_t *cb, uint32_t code_size_words);

// ---- Locales (una tabla nueva por cada funcion) ----

typedef struct {
    nb_string_t *name;   // NO es dueña -- ver la nota en nb_local_scope_add
    int32_t offset;       // desplazamiento en bytes respecto a x29 (POSITIVO --
                           // x29 apunta a la base del marco, justo donde vive el
                           // par x29/x30 guardado; las locales van justo encima)
} nb_local_entry_t;

typedef struct {
    nb_local_entry_t *entries;
    uint32_t count, cap;
    int32_t next_offset; // siguiente hueco libre respecto a x29, empieza en +16 y sube de 8 en 8
} nb_local_scope_t;

void nb_local_scope_init(nb_local_scope_t *sc);
void nb_local_scope_free(nb_local_scope_t *sc); // libera el array -- NO las cadenas (no es su dueña)

// NULL si no existe en este ambito.
nb_local_entry_t *nb_local_scope_find(nb_local_scope_t *sc, nb_string_t *name);

// Declara una local nueva. NO se reclama 'name' -- el llamador
// conserva su propiedad (normalmente sigue viva en el nodo del AST
// de donde vino, que ya es su dueño; la tabla local solo guarda una
// referencia prestada para comparar nombres, nunca la libera).
nb_local_entry_t *nb_local_scope_add(nb_local_scope_t *sc, nb_string_t *name);

#endif
