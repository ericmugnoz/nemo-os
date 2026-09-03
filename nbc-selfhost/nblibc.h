// nblibc.h — reemplazos minimos de libc para que el compilador y el
// ensamblador puedan compilarse en modo freestanding (con
// -ffreestanding -nostdlib, igual que el resto de Nemo OS) y correr
// como un .pro normal dentro del propio sistema.
//
// stdint.h/stdbool.h SI se pueden incluir (son solo typedefs del
// propio compilador, no dependen de ninguna libreria en tiempo de
// ejecucion) -- lo que no podemos usar es string.h, ctype.h,
// stdlib.h (malloc/atof/exit) ni stdio.h (FILE*, fprintf) de verdad.

#ifndef NBLIBC_H
#define NBLIBC_H

#include <stdint.h>
#include <stdbool.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

// -- memoria --
void *nb_memcpy(void *dst, const void *src, uint32_t n);
void *nb_memset(void *dst, int value, uint32_t n);

// -- cadenas --
uint32_t nb_strlen(const char *s);
int nb_strcmp(const char *a, const char *b);
int nb_strncmp(const char *a, const char *b, uint32_t n);
char *nb_strncpy(char *dst, const char *src, uint32_t n); // SIEMPRE termina en '\0' (a diferencia de la real)

// -- clasificacion de caracteres --
bool nb_isalpha(char c);
bool nb_isdigit(char c);
bool nb_isalnum(char c);
bool nb_isxdigit(char c);
bool nb_isspace(char c);
char nb_toupper(char c);
char nb_tolower(char c);

// -- numeros --
// Convierte una cadena decimal (con signo opcional) a entero de 64
// bits. Sustituye a atoi/atol -- nunca a atof, ya que el propio
// compilador no usa coma flotante en ningun sitio (el kernel corre
// con -mgeneral-regs-only, sin FPU).
int64_t nb_atoi64(const char *s);

// Convierte un entero a texto decimal, escribiendo en 'buf' (de
// tamaño 'buf_size'). Complementa a nb_atoi64.
void nb_itoa(int64_t value, char *buf, uint32_t buf_size);

// -- asignacion de memoria --
// Un unico asignador de "avance" (bump allocator) sobre un buffer
// estatico grande -- no hace falta liberar memoria individualmente,
// ya que todo el programa vive una sola pasada de compilacion y
// termina. nb_alloc_reset() vacia el buffer entero (para reiniciar
// entre una compilacion y la siguiente, si el programa se reutiliza).
void nb_alloc_reset(void);
void *nb_alloc(uint32_t size);

#endif
