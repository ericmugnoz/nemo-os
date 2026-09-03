// assembler.h — ensamblador Nemo-AS
#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include <stdint.h>
#include <stdbool.h>

// Ensambla el codigo fuente completo. Escribe el binario final
// (seccion .text seguida de .rodata -- SIN los bytes de .bss, que no
// hacen falta: el cargador de Nemo OS ya limpia a cero el hueco de
// memoria de cada tarea antes de cargar el codigo, asi que las
// direcciones de .bss simplemente caen justo despues del archivo,
// en memoria ya puesta a cero) en 'out_buf' (de tamaño 'out_buf_size').
// Devuelve el tamaño final en bytes, o -1 si hubo un error (se
// imprime un mensaje por stderr).
int64_t assemble(const char *source, uint8_t *out_buf, uint32_t out_buf_size);

#endif
