// nb_codebuf.h — búfer de código máquina + etiquetas, para el
// generador de código de Nemo-Blitz 2.0.
//
// Dos problemas que resuelve, ninguno relacionado con nb_encode.c (la
// codificación de cada instrucción por separado ya está resuelta ahí):
//
// 1. El código generado no se sabe de antemano cuánto va a ocupar --
//    este búfer crece con nb_alloc/nb_realloc, igual que cualquier
//    otra estructura dinámica de este compilador. Nada de un tamaño
//    fijo "que debería bastar".
//
// 2. Un salto hacia ADELANTE (el `If` que salta a después de su
//    bloque, por ejemplo) se genera ANTES de saber a qué posición
//    exacta va a saltar -- esa posición todavía no existe en el
//    código. Una "etiqueta" (nb_label_t) representa ese destino
//    pendiente: se puede emitir un salto hacia ella antes de saber
//    dónde cae, y cuando por fin se define (nb_label_define), se
//    vuelve atrás y se corrige cada salto que la usó, con el offset
//    real ya calculable.

#ifndef NB_CODEBUF_H
#define NB_CODEBUF_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t *words;
    uint32_t count;
    uint32_t cap;
} nb_codebuf_t;

void nb_codebuf_init(nb_codebuf_t *cb);
void nb_codebuf_free(nb_codebuf_t *cb); // libera el buffer en si (no las etiquetas -- esas se liberan aparte)

// Posicion actual (en PALABRAS de instruccion, no bytes) -- lo que
// nb_label_define usa como destino de una etiqueta.
uint32_t nb_codebuf_here(const nb_codebuf_t *cb);

// Añade una palabra de 32 bits (una instruccion ya codificada por
// nb_encode.c) al final. Devuelve la posicion donde quedo.
uint32_t nb_codebuf_emit(nb_codebuf_t *cb, uint32_t word);

// Añade 'len' bytes crudos (NO instrucciones -- un bloque de runtime
// ya resuelto, por ejemplo) al final, palabra a palabra. 'len' DEBE
// ser multiplo de 4 -- todo lo que se incrusta asi son bloques de
// codigo ARM64, que siempre lo son. Devuelve la posicion (en
// palabras) donde empezo.
uint32_t nb_codebuf_emit_bytes(nb_codebuf_t *cb, const uint8_t *bytes, uint32_t len);

// Rellena con NOP hasta que la posicion actual (en BYTES: here()*4) sea
// multiplo de 'byte_align'.
//
// Se usa para colocar el bloque de runtime en un desplazamiento multiplo de
// 4096. Las referencias internas del bloque (adrp y las llamadas entre sus
// funciones) vienen YA resueltas desde nb_elf_extract.py, calculadas sobre
// esa suposicion: adrp trabaja con paginas de 4 KB, asi que mientras el
// bloque empiece en un multiplo de 4096 esas referencias siguen siendo
// validas, caiga donde caiga dentro del .pro. Si empezara en otro sitio,
// apuntarian todas desviadas.
void nb_codebuf_align(nb_codebuf_t *cb, uint32_t byte_align);

// Sobreescribe una palabra YA emitida -- para corregir un salto una
// vez que su etiqueta se resuelve.
void nb_codebuf_patch(nb_codebuf_t *cb, uint32_t pos, uint32_t word);

// ---- Etiquetas ----

typedef struct nb_label_ref {
    uint32_t pos;   // posicion (en palabras) de la instruccion de salto a corregir
    int32_t kind;   // NB_LABEL_KIND_* -- que forma de salto es, para saber como recodificarla
    int32_t extra;  // registro (cbz/cbnz) o condicion (bcond); sin uso en b/bl
    struct nb_label_ref *next;
} nb_label_ref_t;

#define NB_LABEL_KIND_B     0
#define NB_LABEL_KIND_BL    1
#define NB_LABEL_KIND_CBZ   2
#define NB_LABEL_KIND_CBNZ  3
#define NB_LABEL_KIND_BCOND 4
#define NB_LABEL_KIND_ADRP     5 // 'extra' guarda el registro, no un rt/cond -- direccion de una posicion de CODIGO (no de datos)
#define NB_LABEL_KIND_ADD_LO12 6 // el 'add' que siempre acompaña al adrp anterior, mismo registro

typedef struct {
    uint32_t target;         // posicion definitiva, o NB_LABEL_UNDEFINED si aun no se definio
    nb_label_ref_t *pending; // saltos ya emitidos que esperan a que se defina
} nb_label_t;

#define NB_LABEL_UNDEFINED 0xFFFFFFFFu

// Crea una etiqueta sin definir todavia. Reservada con nb_alloc, como
// el resto de estructuras del propio compilador -- se libera con
// nb_label_free cuando ya no hace falta (normalmente, al terminar de
// generar la funcion/programa que la uso).
nb_label_t *nb_label_new(void);
void nb_label_free(nb_label_t *label);

// Marca la etiqueta en la posicion ACTUAL del buffer, y corrige de
// inmediato cada salto pendiente que la usaba.
void nb_label_define(nb_codebuf_t *cb, nb_label_t *label);

// Igual que nb_label_define, pero con una posicion EXPLICITA en vez
// de "la posicion actual del buffer" -- para cuando el destino real
// (una funcion dentro de un bloque ya incrustado, por ejemplo) no
// coincide con donde se está escribiendo ahora mismo.
void nb_label_define_at(nb_codebuf_t *cb, nb_label_t *label, uint32_t target_word);

// Las cinco formas de salto que existen en nb_encode.c. Si la
// etiqueta ya esta definida (un salto HACIA ATRAS, como el principio
// de un bucle), calculan el offset y emiten la instruccion final
// directamente. Si no (HACIA ADELANTE), emiten un huevo vacio y
// apuntan una referencia pendiente para cuando se defina.
void nb_emit_b(nb_codebuf_t *cb, nb_label_t *label);
void nb_emit_bl(nb_codebuf_t *cb, nb_label_t *label);
void nb_emit_cbz(nb_codebuf_t *cb, int32_t rt, nb_label_t *label);
void nb_emit_cbnz(nb_codebuf_t *cb, int32_t rt, nb_label_t *label);
void nb_emit_bcond(nb_codebuf_t *cb, int32_t cond, nb_label_t *label);

// Calcula la direccion absoluta de una posicion del propio CODIGO
// (no de datos, a diferencia de nb_emit_global_addr) en el registro
// 'reg' -- adrp+add, con resolucion diferida si la etiqueta todavia
// no esta definida. La usan Gosub/Return, para construir su propia
// pila de direcciones de retorno sin depender de x30/bl/ret (que no
// sobrevive si el codigo de la etiqueta llama a cualquier funcion
// antes del Return correspondiente).
void nb_emit_code_addr(nb_codebuf_t *cb, int32_t reg, nb_label_t *label);

#endif
