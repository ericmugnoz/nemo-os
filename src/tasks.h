// tasks.h — Nemo OS
#ifndef TASKS_H
#define TASKS_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_TASKS 8

void tasks_init(void);

// Carga un programa .pro desde NemoFS y lo lanza como una tarea
// independiente, con su propia pila.
//   window_idx: ventana grafica ya creada, o -1 para "modo consola"
//     (sin ventana propia todavia -- se crea sola, perezosamente, la
//     primera vez que el programa llame a un comando grafico o de
//     gadgets; ver task_ensure_window).
//   arg: argumento de texto opcional (ej. un archivo a abrir), o NULL.
//   console_window: ventana a la que redirigir SYS_WRITE_STRING (la
//     de quien lanzo el programa, tipicamente una shell), o -1 para
//     que Print vaya a la UART como hasta ahora.
// Devuelve el indice de la tarea, o -1 si no hay hueco o el programa
// no se pudo cargar.
int32_t task_spawn_from_file(const char *filename, uint32_t parent_inode, int32_t window_idx,
                              const char *arg, int32_t console_window);

// Devuelve el argumento de lanzamiento de la tarea que esta
// ejecutando en este momento (cadena vacia si no tenia ninguno).
const char *task_get_launch_arg(void);

// Cede el control a la siguiente tarea lista (ronda circular),
// pasando antes por un "pump" del sistema (raton, ventanas,
// redibujado). Hay que llamarla periodicamente desde dentro de una
// tarea (via syscall) o desde el bucle principal del kernel.
void task_yield(void);

// Devuelve la ventana asociada a la tarea que esta ejecutando en este
// momento, o -1 si no hay ninguna tarea corriendo (estamos en el
// contexto del kernel) o si esa tarea todavia no tiene ventana propia
// (modo consola).
int32_t task_get_current_window(void);

// Termina (marca como finalizada) la tarea dueña de 'window_idx', si
// la hay -- lo usa wm.c cuando el usuario cierra con la X una ventana
// SIN modo de eventos, para no dejar la tarea corriendo huerfana en
// segundo plano tras cerrar su ventana.
void task_kill_by_window(int32_t window_idx);

// Devuelve la ventana a la que la tarea actual redirige su Print
// (SYS_WRITE_STRING), o -1 si no tiene ninguna asignada (va a la UART).
int32_t task_get_console_window(void);

// Si la tarea actual todavia no tiene ventana grafica propia, le crea
// una AHORA (la primera vez que de verdad hace falta, al llamar a
// cualquier comando grafico o de gadgets). Si ya tenia una, solo la
// devuelve. Devuelve -1 si no hay ninguna tarea corriendo o si la
// creacion fallo.
int32_t task_ensure_window(void);

// Solo para depuracion (ver el volcado de excepciones en
// exceptions.c): nombre del programa actual y direccion base de su
// codigo, para poder calcular el desplazamiento exacto dentro del
// binario cuando algo falla.
void task_get_debug_info(const char **out_name, uint64_t *out_base);

#endif
