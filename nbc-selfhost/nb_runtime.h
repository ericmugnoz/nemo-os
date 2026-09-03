// nb_runtime.h — interfaz minima que necesita el compilador/ensamblador
// para reportar un error fatal y terminar. Tiene DOS implementaciones
// intercambiables:
//   - nb_runtime_host.c: para probar aqui mismo con gcc normal (usa
//     fprintf(stderr,...) y exit(1) de verdad).
//   - nb_runtime_nemo.c: para la version final que corre dentro de
//     Nemo OS (usara SYS_WRITE_STRING + SYS_EXIT).
// El resto del compilador (parser.c, codegen.c...) llama siempre a
// nb_fatal() sin saber -ni le importa- cual de las dos hay debajo.

#ifndef NB_RUNTIME_H
#define NB_RUNTIME_H

// Imprime un mensaje de error (con el numero de linea y, si se da,
// el texto encontrado) y termina el programa. Nunca vuelve.
void nb_fatal(int line, const char *msg, const char *found) __attribute__((noreturn));

#endif
