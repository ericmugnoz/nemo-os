// nb_output.h — sustituto de FILE*/fprintf para codegen.c
//
// codegen.c escribe cientos de veces con fprintf(out, "...", args) --
// en vez de reescribir cada llamada, implementamos nb_fprintf con la
// MISMA forma (mismo primer argumento, mismo patron de formato+
// argumentos variables), asi que el cambio en codegen.c se reduce a
// un renombrado mecanico de 'fprintf' a 'nb_fprintf'.
//
// Solo soporta los especificadores que codegen.c usa de verdad:
// %s (cadena), %d (entero de 32 bits), %lld (entero de 64 bits), %%.

#ifndef NB_OUTPUT_H
#define NB_OUTPUT_H

#include <stdint.h>

// El "out" que se le pasa a nb_fprintf no se usa para nada (todas las
// llamadas escriben al MISMO buffer activo) -- se mantiene solo para
// que la firma coincida con fprintf(FILE*, ...) y el codigo de
// codegen.c no necesite tocarse mas que el nombre de la funcion.
typedef struct { int unused; } NBOut;
extern NBOut nb_stdout_placeholder;

// Prepara un buffer nuevo como destino de todas las escrituras
// siguientes. Hay que llamarlo antes de generar codigo.
void nb_output_begin(char *buffer, uint32_t capacity);

// Longitud actual de lo escrito (para saber cuanto ocupa el .s
// resultante en memoria).
uint32_t nb_output_length(void);

// El propio "fprintf" -- ver nota arriba sobre por que existe.
void nb_fprintf(NBOut *out, const char *fmt, ...);

#endif
