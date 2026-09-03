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
int32_t dialog_open_file(int32_t window_idx, uint32_t start_dir, char *out_name, uint32_t out_name_max);

// Igual, pero permite ademas escribir un nombre nuevo (o hacer clic
// en un archivo existente para sobrescribirlo). Devuelve el inodo
// (existente o recien creado), o -1 si cancela.
int32_t dialog_save_file(int32_t window_idx, uint32_t start_dir, char *out_name, uint32_t out_name_max);

#endif
