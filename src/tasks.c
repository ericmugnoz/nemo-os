// tasks.c — Nemo OS
//
// Planificador cooperativo. "Cooperativo" significa que una tarea
// solo cede el control cuando ELLA MISMA decide hacerlo (llamando a
// task_yield, normalmente escondido dentro de una syscall bloqueante
// como SYS_PUMP o SYS_READ_CHAR_WAIT) -- no hay interrupcion de
// temporizador que la eche a la fuerza. Es mucho mas simple que la
// multitarea preventiva de un SO de verdad, y para nuestro caso
// (programas que ceden el control constantemente esperando
// teclado/raton) funciona perfectamente.
//
// Cada tarea tiene su PROPIA pila y su PROPIA zona de codigo cargado
// -- antes, con un solo programa activo a la vez, podiamos compartir
// una unica area de 64KB para el codigo; ahora que varios programas
// pueden estar vivos simultaneamente, cada uno necesita la suya.

#include "tasks.h"
#include "loader.h"
#include "input.h"
#include "wm.h"
#include "uart.h"
#include "gadgets.h"

#define TASK_STACK_SIZE (16 * 1024)
// 16MB por tarea -- antes eran 6MB (y 64KB antes de eso), pero una
// auditoria detallada de la memoria ESTATICA que usa el propio
// compilador autohospedado (nbc.pro, que TAMBIEN corre como una tarea
// .pro mas) revelo que 6MB dejaba muy poco margen real: solo su pool
// de nodos de sintaxis (nblibc.c) ya son 4MB, mas ~830KB de buffers
// en nbc_main.c, mas ~360KB entre las tablas internas de codegen.c y
// el ensamblador -- unos 5.2MB SOLO de datos, dejando apenas ~800KB
// para el codigo compilado en si (.text+.rodata) de un proyecto de
// ~370KB de fuente en C repartido en 11 archivos. LIMITACION REAL
// IMPORTANTE de nuestro cargador: comprueba que el codigo (.text+
// .rodata) quepa en esta area, pero NO suma el tamaño de .bss del
// programa (esa informacion se pierde en el empaquetado actual,
// blob de codigo puro via objcopy) -- si algun programa excede este
// limite combinado, escribiria silenciosamente en la memoria de la
// SIGUIENTE tarea, no fallaria con un error claro. Con los 512MB de
// RAM de QEMU, 16MB x 8 tareas (128MB en total) sigue sin ser
// problema real, y da un margen mucho mas solido.
#define TASK_PROGRAM_AREA_SIZE (16 * 1024 * 1024)

// Indice especial que representa "el contexto del propio kernel" (el
// bucle principal de kernel_main) dentro de la ronda de planificacion
// -- asi el kernel tambien puede ceder el control a las tareas, y
// recibirlo de vuelta, con el mismo mecanismo que usan las tareas
// entre si.
#define KERNEL_CTX MAX_TASKS

extern void task_switch(uint64_t *old_sp_save, uint64_t new_sp);

typedef struct {
    bool used;
    bool finished;
    int32_t window_idx;      // -1 = modo consola: aun sin ventana grafica propia
    int32_t console_window;  // ventana a la que redirigir SYS_WRITE_STRING (-1 = ninguna, va a UART)
    char program_name[32];   // para el titulo si se crea la ventana perezosamente
    void (*entry)(void);
    char launch_arg[32];
} task_t;

static task_t tasks[MAX_TASKS];
static uint64_t all_saved_sp[MAX_TASKS + 1]; // [MAX_TASKS] = contexto kernel
static int32_t current_ctx = KERNEL_CTX;

__attribute__((aligned(16)))
static uint8_t task_stacks[MAX_TASKS][TASK_STACK_SIZE];

__attribute__((aligned(4096)))
static uint8_t task_program_areas[MAX_TASKS][TASK_PROGRAM_AREA_SIZE];

static bool ctx_is_ready(int32_t idx) {
    if (idx == KERNEL_CTX) return true; // el kernel siempre esta "listo"
    return tasks[idx].used && !tasks[idx].finished;
}

// Punto de entrada real de toda tarea nueva -- ver task_spawn_from_file
// para como "saltamos" aqui la primera vez que se le da tiempo.
static void task_trampoline(void) {
    task_t *t = &tasks[current_ctx];

    uart_puts("tasks: task_trampoline saltando a la tarea '");
    uart_puts(t->program_name);
    uart_puts("' (contexto=");
    { char n[4]; int i=0; int32_t v=current_ctx; if(v==0){n[i++]='0';} while(v>0 && i<4){n[i++]=(char)('0'+(v%10)); v/=10;} while(i>0) uart_putc(n[--i]); }
    uart_puts(")...\n");

    t->entry();

    // El programa ha vuelto (termino su _start). Cerramos su ventana
    // si tenia una, y la marcamos como terminada para que el
    // planificador nunca vuelva a darle tiempo -- su hueco quedara
    // libre para una tarea futura.
    if (t->window_idx >= 0) {
        gadgets_free_window(t->window_idx);
        wm_destroy_window(t->window_idx);
    }
    t->finished = true;

    while (1) {
        task_yield(); // nunca deberiamos volver a ejecutar codigo de aqui en adelante
    }
}

void tasks_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].used = false;
        tasks[i].finished = false;
    }
    current_ctx = KERNEL_CTX;
    gadgets_init();
}

int32_t task_spawn_from_file(const char *filename, uint32_t parent_inode, int32_t window_idx,
                              const char *arg, int32_t console_window) {
    int32_t slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].used || tasks[i].finished) { slot = i; break; }
    }
    if (slot < 0) {
        uart_puts("tasks: no hay hueco para una tarea nueva\n");
        return -1;
    }

    // Limpiamos a cero el hueco ANTES de cargar el codigo nuevo -- si
    // no, un programa que espere que su propia zona .bss (variables
    // globales, buffers internos...) empiece en cero se encontraria
    // con basura dejada por el programa ANTERIOR que uso este mismo
    // hueco. El formato .pro tampoco incluye los bytes de .bss en el
    // archivo (es zona reservada, no datos) -- asi que esta limpieza
    // es la unica garantia real de que arranca en cero. Limpiamos de
    // 8 en 8 bytes (no byte a byte) porque con huecos de 6MB, un
    // bucle de un solo byte por vuelta seria notablemente lento.
    uart_puts("tasks: limpiando hueco de memoria...\n");
    uint64_t *area64 = (uint64_t *)task_program_areas[slot];
    for (uint32_t i = 0; i < TASK_PROGRAM_AREA_SIZE / 8; i++) area64[i] = 0;
    uart_puts("tasks: hueco limpio, cargando '");
    uart_puts(filename);
    uart_puts("'...\n");

    void (*entry)(void) = 0;
    if (!loader_load_into(filename, parent_inode, task_program_areas[slot], TASK_PROGRAM_AREA_SIZE, &entry)) {
        return -1;
    }
    uart_puts("tasks: archivo cargado en memoria correctamente, preparando la tarea...\n");

    tasks[slot].used = true;
    tasks[slot].finished = false;
    tasks[slot].window_idx = window_idx;       // -1 = modo consola, se crea sola cuando haga falta
    tasks[slot].console_window = console_window; // -1 = sin redireccion, Print va a la UART
    tasks[slot].entry = entry;
    uart_puts("tasks: paso 1 (campos basicos) hecho\n");

    int i = 0;
    while (filename[i] != '\0' && i < 31) { tasks[slot].program_name[i] = filename[i]; i++; }
    tasks[slot].program_name[i] = '\0';
    uart_puts("tasks: paso 2 (nombre copiado) hecho\n");

    i = 0;
    if (arg) {
        while (arg[i] != '\0' && i < 31) { tasks[slot].launch_arg[i] = arg[i]; i++; }
    }
    tasks[slot].launch_arg[i] = '\0';
    uart_puts("tasks: paso 3 (argumento copiado) hecho\n");

    // Preparamos un "contexto falso" en la cima de la pila nueva: los
    // registros x19-x28, x29, y d8-d15 no importan (nadie los ha
    // usado todavia), pero x30 (el registro de enlace) SI importa --
    // es la direccion a la que "volveremos" la primera vez que
    // task_switch() haga su 'ret' hacia esta tarea. Poniendo ahi
    // task_trampoline, conseguimos que arranque el programa de
    // verdad la primera vez que le demos tiempo. El tamaño (160
    // bytes = 20 registros de 8 bytes) tiene que coincidir EXACTO con
    // lo que tasks_switch.s espera encontrarse al restaurar.
    uint8_t *stack_top = task_stacks[slot] + TASK_STACK_SIZE;
    stack_top = (uint8_t *)((uint64_t)stack_top & ~0xFULL);
    uint64_t *frame = (uint64_t *)(stack_top - 160);
    for (int i = 0; i < 20; i++) frame[i] = 0; // x19-x28, x29, d8-d15
    frame[11] = (uint64_t)task_trampoline;      // x30 (link register)
    uart_puts("tasks: paso 4 (marco de pila preparado) hecho\n");

    all_saved_sp[slot] = (uint64_t)frame;

    if (window_idx >= 0) {
        wm_set_owns_content(window_idx, true);
    }

    return slot;
}

void task_yield(void) {
    // Un "tick" del sistema cada vez que alguien cede el control --
    // asi el raton, las ventanas y el redibujado siguen vivos sin
    // importar cual de las tareas este actualmente en marcha.
    input_poll();
    wm_update();
    gadgets_update_and_draw();
    gadgets_check_timers();
    gadgets_check_hotkeys();
    wm_draw_if_needed();
    __asm__ volatile("wfe");

    // Buscamos la siguiente tarea lista, en ronda circular, incluyendo
    // el contexto del kernel como un participante mas.
    int32_t next = current_ctx;
    for (int i = 0; i <= MAX_TASKS; i++) {
        next = (next + 1) % (MAX_TASKS + 1);
        if (ctx_is_ready(next)) break;
    }

    if (next == current_ctx) {
        return; // nadie mas a quien cambiar -- seguimos donde estabamos
    }

    int32_t prev = current_ctx;
    current_ctx = next;
    task_switch(&all_saved_sp[prev], all_saved_sp[next]);
    // Cuando volvamos aqui, sera porque alguien nos ha vuelto a dar
    // tiempo -- seguimos con normalidad, como si nada hubiera pasado.
}

int32_t task_get_current_window(void) {
    if (current_ctx == KERNEL_CTX || current_ctx < 0 || current_ctx >= MAX_TASKS) return -1;
    return tasks[current_ctx].window_idx;
}

const char *task_get_launch_arg(void) {
    if (current_ctx == KERNEL_CTX || current_ctx < 0 || current_ctx >= MAX_TASKS) return "";
    return tasks[current_ctx].launch_arg;
}

// Solo para depuracion: nombre del programa que esta corriendo ahora
// mismo, y la direccion base donde se cargo su codigo -- asi el
// volcado de una excepcion puede mostrar el desplazamiento EXACTO
// dentro del programa, en vez de tener que adivinarlo correlacionando
// direcciones a mano.
void task_get_debug_info(const char **out_name, uint64_t *out_base) {
    if (current_ctx == KERNEL_CTX || current_ctx < 0 || current_ctx >= MAX_TASKS) {
        if (out_name) *out_name = "(kernel)";
        if (out_base) *out_base = 0;
        return;
    }
    if (out_name) *out_name = tasks[current_ctx].program_name;
    if (out_base) *out_base = (uint64_t)task_program_areas[current_ctx];
}

int32_t task_get_console_window(void) {
    if (current_ctx == KERNEL_CTX || current_ctx < 0 || current_ctx >= MAX_TASKS) return -1;
    return tasks[current_ctx].console_window;
}

// Marca como terminada la tarea que sea dueña de 'window_idx', si la
// hay. La llama wm.c cuando el usuario cierra con la X una ventana
// SIN modo de eventos (por ejemplo, un programa Graphics() clasico) --
// sin esto, la tarea de fondo se quedaria corriendo para siempre,
// ocupando su hueco del planificador sin que nadie mas lo pueda usar,
// aunque su ventana ya no exista.
void task_kill_by_window(int32_t window_idx) {
    if (window_idx < 0) return;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].used && !tasks[i].finished && tasks[i].window_idx == window_idx) {
            tasks[i].finished = true;
            return;
        }
    }
}

// Si la tarea actual todavia no tiene ventana grafica propia (nacio
// en "modo consola", redirigiendo su Print a quien la lanzo), le
// creamos una AHORA, la primera vez que de verdad hace falta -- al
// llamar a cualquier comando grafico o de gadgets. A partir de aqui,
// esta tarea ya no es "un programa de consola", es una app con
// ventana, aunque su Print siga yendo a la consola que la lanzo.
int32_t task_ensure_window(void) {
    if (current_ctx == KERNEL_CTX || current_ctx < 0 || current_ctx >= MAX_TASKS) return -1;
    task_t *t = &tasks[current_ctx];
    if (t->window_idx >= 0) return t->window_idx; // ya tenia una

    int32_t win = wm_create_window(300, 150, 400, 280, t->program_name);
    if (win >= 0) {
        wm_set_owns_content(win, true);
        t->window_idx = win;
    }
    return win;
}
