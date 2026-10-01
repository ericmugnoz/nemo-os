// dialog.h — Nemo OS
#ifndef DIALOG_H
#define DIALOG_H

#include <stdint.h>

// Dialogo comun de "Abrir archivo" y "Guardar como", al estilo de los
// "common dialogs" de Windows (GetOpenFileName) o NSOpenPanel de
// macOS: en vez de que cada programa reimplemente su propia
// navegacion de carpetas, el kernel la ofrece una vez, y cualquier
// programa la reutiliza via syscall.
//
// Ambas funciones se dibujan DENTRO de la ventana de la tarea que las
// invoca, y BLOQUEAN cediendo el control a otras tareas mientras el
// usuario navega (igual que SYS_READ_CHAR_WAIT) -- el resto del
// sistema sigue respondiendo mientras tanto.

// Navega desde 'start_dir' hasta que el usuario elige un archivo
// existente (clic) o cancela (Esc). Devuelve el inodo elegido, o -1
// si cancela; si hay exito, escribe el nombre en 'out_name'.
// Devuelve, empaquetado en 64 bits, la carpeta real donde se
// encontro el archivo (bits 63:32) y su inodo (bits 31:0) -- mismo
// patron que SYS_GET_EVENT_INFO. -1 (todo el valor de 64 bits) si se
// cancela. Un llamador que solo necesite el inodo puede seguir
// truncando el resultado a 32 bits, como hacia antes: el inodo ocupa
// siempre los bits bajos.
int64_t dialog_open_file(int32_t window_idx, uint32_t start_dir, char *out_name, uint32_t out_name_max);

// Igual, pero permite ademas escribir un nombre nuevo (o hacer clic
// en un archivo existente para sobrescribirlo). Devuelve el inodo
// (existente o recien creado), o -1 si cancela.
// Igual que dialog_open_file: carpeta real en los bits altos, inodo
// en los bajos.
int64_t dialog_save_file(int32_t window_idx, uint32_t start_dir, char *out_name, uint32_t out_name_max);

#endif
