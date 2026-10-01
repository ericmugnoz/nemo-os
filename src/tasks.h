// tasks.h — Nemo OS
#ifndef TASKS_H
#define TASKS_H

#include <stdint.h>
#include <stdbool.h>
#include "mmu.h"

// Numero de tareas que pueden existir a la vez. Era 8; con la memoria
// ya repartida por tamaño, lo que limitaba de verdad era esto.
//
// Cada hueco cuesta poco: 16KB de pila de kernel y 8KB de tablas de
// MMU. La memoria de las tareas NO depende de este numero -- sale de
// una reserva fija de 128MB (TASK_POOL_BYTES en tasks.c).
//
// El techo real lo pone el ASID: la MMU usa ASID de 8 bits y el de
// cada hueco es hueco+1, asi que caben hasta 255.
#define MAX_TASKS 16
_Static_assert(MAX_TASKS <= MMU_MAX_TASK_CONTEXTS, "cada tarea necesita su tabla de traduccion (ver mmu.h)");

void tasks_init(void);
// Un nucleo secundario entra en el planificador. Ver tasks.c.
void tasks_entrar_nucleo(uint32_t nucleo);
// ¿La tarea de esta ventana corre ahora en otro nucleo? Ver tasks.c.
bool tasks_ventana_en_otro_nucleo(int32_t ventana);
// Memoria de la reserva de tareas en uso y total, en bytes.
// Memoria de programas usada y total, en KB (en bytes no cabe en 32 bits).
void task_pool_usage_kb(uint32_t *used_kb, uint32_t *total_kb);

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
// El argumento con que se lanza un programa ("carpeta:nombre",
// "@carpeta run programa.pro"...). Eran 32 caracteres en tres sitios (la cola
// de wm.c, kernel.c y la tarea), y con un nombre largo se cortaba: el
// programa no encontraba su archivo. Una sola medida para los tres.
#define TASK_LAUNCH_ARG_MAX 64
const char *task_get_launch_arg(void);

// Cede el control a la siguiente tarea lista (ronda circular),
// pasando antes por un "pump" del sistema (raton, ventanas,
// redibujado). Hay que llamarla periodicamente desde dentro de una
// tarea (via syscall) o desde el bucle principal del kernel.
void task_yield(void);
// ¿La tarea actual ya fue terminada? Ver tasks.c.
bool task_actual_terminada(void);
// Generacion de un hueco de tarea y si su programa sigue vivo. Ver tasks.c.
// SYS_MEM_PEDIR: otra zona de memoria para la tarea actual (fase 4).
uint64_t task_pedir_memoria(uint64_t bytes);
// Devuelve la memoria de los programas terminados (nucleo 0, bucle principal).
void tasks_recoger_terminadas(void);
uint32_t task_generacion(int32_t slot);
bool task_viva(int32_t slot, uint32_t generacion);
// Dormir la tarea actual hasta ese latido del reloj. Ver tasks.c.
void task_dormir_hasta(uint64_t latido);

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
// Para el gestor de tareas -- ver la documentacion junto a las
// implementaciones en tasks.c.
uint32_t task_list_dump(uint8_t *out, uint32_t max_entries);
bool task_kill_by_slot(int32_t slot);
uint32_t task_count_used(void);
// Ver la documentacion junto a la implementacion en tasks.c.
bool task_owns_range(int32_t ctx, uint64_t addr, uint64_t len);
int32_t task_get_current_slot(void);
// Termina la tarea actual (SYS_EXIT, o muerte por fallo en EL0 desde
// exceptions.c). Nunca vuelve.
void task_exit_current(void) __attribute__((noreturn));

// Devuelve la ventana a la que la tarea actual redirige su Print
// (SYS_WRITE_STRING), o -1 si no tiene ninguna asignada (va a la UART).
int32_t task_get_console_window(void);

// Si la tarea actual todavia no tiene ventana grafica propia, le crea
// una AHORA (la primera vez que de verdad hace falta, al llamar a
// cualquier comando grafico o de gadgets). Si ya tenia una, solo la
// devuelve. Devuelve -1 si no hay ninguna tarea corriendo o si la
// creacion fallo.
int32_t task_ensure_window(void);

// Cierra la ventana de la tarea actual y libera sus controles, sin terminar
// la tarea. Un CreateWindow o cualquier comando grafico posterior vuelve a
// crear una. Devuelve 1 si habia ventana, 0 si no.
int32_t task_close_window(void);

// Solo para depuracion (ver el volcado de excepciones en
// exceptions.c): nombre del programa actual y direccion base de su
// codigo, para poder calcular el desplazamiento exacto dentro del
// binario cuando algo falla.
void task_get_debug_info(const char **out_name, uint64_t *out_base);

#endif
