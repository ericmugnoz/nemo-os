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

#include "gpio.h"
#include "memoria.h"
#include "tasks.h"
#include "cpu.h"
#include "bkl.h"
#include "timer.h"
#include "smp.h"
#include "medir.h"
#include "loader.h"
#include "input.h"
#include "wm.h"
#include "uart.h"
#include "gadgets.h"
#include "kernel.h"
#include "mmu.h"
#include "nemofs.h"
#include "net.h"

#define TASK_STACK_SIZE (16 * 1024)
// 16MB por tarea -- antes eran 6MB (y 64KB antes de eso), pero una
// auditoria detallada de la memoria ESTATICA que usa el propio
// compilador autohospedado (nbc.pro, que TAMBIEN corre como una tarea
// .pro mas) revelo que 6MB dejaba muy poco margen real: solo su pool
// de nodos de sintaxis (nblibc.c) ya son 4MB, mas ~830KB de buffers
// en nbc_main.c, mas ~360KB entre las tablas internas de codegen.c y
// el ensamblador -- unos 5.2MB SOLO de datos, dejando apenas ~800KB
// para el codigo compilado en si (.text+.rodata) de un proyecto de
// ~370KB de fuente en C repartido en 11 archivos.
//
// Sobre el .bss: el cargador comprueba que el CODIGO (.text+.rodata)
// quepa aqui, pero no puede sumar el .bss del programa -- esa
// informacion se pierde al empaquetar con objcopy, que produce un
// blob de codigo puro.
//
// ESO YA NO ES PELIGROSO, aunque este comentario decia que si. Lo era
// antes de que existiera la MMU: entonces un programa que se pasara
// escribia en el area de la SIGUIENTE tarea, en silencio. Hoy cada
// tarea tiene su propia tabla de traduccion (mmu_build_task_tables,
// mas abajo) y SOLO su area esta marcada como memoria de usuario;
// todo lo demas queda como kernel. Un programa que se desborde recibe
// un Data Abort, y exceptions.c retira esa tarea dejando el sistema
// en pie.
//
// Y el mensaje ya es claro: la cabecera NEXE version 3 (24 bytes)
// lleva la memoria TOTAL que necesita el programa, incluido su .bss,
// y el cargador rechaza con numeros ("necesita X KB y el area es de
// Y KB") al que no cabe. Los .pro de version 1/2 (16 bytes) no llevan
// ese dato y siguen cargando como antes. Los genera el compilador de
// Nemo Basic; los programas del sistema, empaquetados con objcopy,
// siguen en version 1.
//
// Con los 512MB de RAM de QEMU, 16MB x 8 tareas (128MB en total)
// sigue sin ser problema real, y da un margen mucho mas solido.
#define TASK_PROGRAM_AREA_SIZE (16 * 1024 * 1024)

// Indice especial que representa "el contexto del propio kernel" (el
// bucle principal de kernel_main) dentro de la ronda de planificacion
// -- asi el kernel tambien puede ceder el control a las tareas, y
// recibirlo de vuelta, con el mismo mecanismo que usan las tareas
// entre si.
#define KERNEL_CTX MAX_TASKS

extern void task_switch(uint64_t *old_sp_save, uint64_t new_sp);
extern void syscall_reiniciar_dibujo(int32_t tarea);   // syscall.c

typedef struct {
    bool used;
    bool finished;
    int32_t window_idx;      // -1 = modo consola: aun sin ventana grafica propia
    int32_t console_window;  // ventana a la que redirigir SYS_WRITE_STRING (-1 = ninguna, va a UART)
    char program_name[32];   // para el titulo si se crea la ventana perezosamente
    void (*entry)(void);
    char launch_arg[TASK_LAUNCH_ARG_MAX];
    // Nucleo que la esta ejecutando ahora mismo, o -1 si ninguno.
    // Con varios nucleos, una tarea no puede correr en dos a la vez:
    // el planificador se salta las que ya estan en otro (ctx_is_ready).
    int32_t nucleo;
    // Latido del reloj en que despertar, o 0 si no esta dormida. Mientras
    // el reloj no llegue, el planificador ni la considera (ctx_is_ready):
    // duerme de verdad, en vez de ceder el turno en bucle solo para mirar
    // la hora. Lo usan Pump y Delay (task_dormir_hasta).
    uint64_t despertar_en;
    // Cuantas veces se ha usado este hueco de tarea. Los recursos del
    // kernel (fuentes, imagenes, archivos abiertos...) apuntan a su dueño
    // como "hueco + generacion": asi, si el hueco lo reutiliza OTRO
    // programa, lo que dejo sin liberar el anterior se reconoce como
    // huerfano y se puede recuperar. Ver syscall.c (recurso_huerfano).
    uint32_t generacion;
} task_t;

static task_t tasks[MAX_TASKS];
// Pila guardada de cada contexto: una por tarea y, al final, una por
// NUCLEO para su contexto de reposo (el que corre cuando ese nucleo no
// tiene ninguna tarea). Antes habia una sola casilla de "contexto del
// kernel"; con varios nucleos cambiando de tarea, el segundo guardaria
// sus registros encima de los del primero.
//
// El del nucleo 0 sigue siendo KERNEL_CTX (= MAX_TASKS), el que hace el
// trabajo del sistema en task_yield: IDLE_CTX(0) == KERNEL_CTX.
#define IDLE_CTX(c) (MAX_TASKS + (c))
static uint64_t all_saved_sp[MAX_TASKS + MAX_CPUS];
// Turnos de planificador recibidos por cada tarea desde que arranco
// el sistema -- para el "monitor de CPU": en un planificador
// cooperativo puro no existe el concepto de "ocioso" de un sistema
// operativo normal, asi que esto es lo mas honesto que se puede medir:
// que proporcion de los turnos se lleva cada tarea, no un porcentaje
// de ocupacion real.
static uint64_t task_turn_count[MAX_TASKS];
// La tarea que esta ejecutando CADA NUCLEO.
//
// Antes era una sola variable para todo el sistema, porque solo habia
// un nucleo. Con varios, cada uno ejecuta una tarea distinta, asi que
// la pregunta "que tarea soy" necesita una respuesta por nucleo.
//
// El #define de abajo hace que todo el codigo que ya usaba current_ctx
// (26 sitios) siga igual sin tocarlo: lee y escribe la entrada del
// nucleo que lo esta ejecutando. Con un solo nucleo, cpu_id() es
// siempre 0 y el comportamiento es exactamente el de antes.
//
// Empieza en KERNEL_CTX para TODOS los nucleos desde la primera
// instruccion, igual que la variable unica de antes. Importa: si el
// array empezara a cero (lo normal en .bss), antes de tasks_init el
// kernel se creeria la tarea 0 -- y cualquier cosa que preguntara "que
// tarea soy" durante el arranque recibiria una respuesta falsa.
static int32_t current_ctx_por_nucleo[MAX_CPUS] = { [0 ... MAX_CPUS - 1] = KERNEL_CTX };
#define current_ctx (current_ctx_por_nucleo[cpu_id()])

__attribute__((aligned(16)))
static uint8_t task_stacks[MAX_TASKS][TASK_STACK_SIZE];

__attribute__((aligned(4096)))
// Alineado a 2MB: la MMU da permisos por bloques de ese tamaño (ver
// mmu_pi4.c). Cada hueco tiene su propia tabla de traduccion, donde
// SOLO su area de 16MB es memoria de usuario; las de los otros huecos,
// y todo lo demas de este archivo (task_stacks, tasks[]...), son
// memoria de kernel, inaccesible desde una tarea.
// La reserva de memoria para tareas es de tamaño FIJO, independiente
// del numero de huecos. Antes era MAX_TASKS x 16MB, asi que subir los
// huecos habria multiplicado la reserva: con 16 huecos, 256MB, y el
// kernel no cabria en los 512MB de QEMU. Ahora los huecos y la memoria
// son cosas separadas: los huecos dicen CUANTAS tareas puede haber; la
// reserva, CUANTA memoria tienen entre todas.
#define TASK_POOL_BYTES (128u * 1024u * 1024u)
__attribute__((aligned(2 * 1024 * 1024)))
static uint8_t task_program_areas[TASK_POOL_BYTES];

// ---- Memoria POR TAREA, de tamaño variable ----
//
// La reserva de arriba sigue siendo la misma (MAX_TASKS x 16MB), pero
// ya no se reparte a trozos fijos de 16MB: se trata como un unico
// bloque de trozos de 2MB, y cada tarea pide los que necesita. Un
// programa pequeño ocupa 2MB en vez de 16; uno grande puede pasar de
// 16MB si los demas le dejan sitio.
//
// 2MB es el grano natural: las tablas de la MMU mapean en bloques de
// 2MB, asi que una region nunca puede empezar ni acabar a mitad de uno.
#define REGION_CHUNK      (2u * 1024u * 1024u)

// ---- DE DONDE SALE LA RESERVA (fase 3 de la memoria,) ----
//
// Antes, la reserva era siempre el array de 128 MB de arriba, dentro de
// la propia imagen del kernel: en una Pi de 4 GB, los programas se
// repartian el 3 % de la placa. Ahora, al arrancar (elegir_reserva, desde
// tasks_init), la reserva es la MAYOR region de RAM por encima del GB del
// kernel que haya detectado memoria.c: unos 3 GB tanto en QEMU (-m 4G)
// como en una Pi de 4 GB. El array de 128 MB solo se usa si no hay RAM
// por encima (QEMU con -m 512M, o una Pi si el mailbox no respondiera).
//
// Tope: algo menos de 4 GB, porque SYS_RAM_POOL devuelve lo usado y el
// total en bytes, cada uno en 32 bits. Sobra para las placas de hoy.
#define REGION_CHUNKS_MAX 4096u     // 4096 x 2 MB = 8 GB
// Fase 4: la reserva ya no es UNA region sino una lista de SEGMENTOS, uno
// por cada region de RAM por encima del kernel (en una Pi de 8 GB: de 1 a
// 4 GB y de 4 a 8 GB). Cada segmento es contiguo; una zona de programa
// nunca cruza de uno a otro. region_used[] es comun: el segmento s ocupa
// las posiciones [primero, primero + trozos).
typedef struct { uint64_t base; uint32_t trozos; uint32_t primero; } segmento_t;
static segmento_t segmentos[MEMORIA_MAX_REGIONES];
static int num_segmentos = 0;
static uint32_t total_trozos = 0;

// Tamaño por defecto para los programas que NO declaran cuanta memoria
// necesitan (cabecera version 1/2): todos los del sistema. Se mantiene
// en 16MB, lo mismo que antes, para no cambiar nada para ellos.
#define DEFAULT_TASK_SIZE (16u * 1024u * 1024u)

// Espacio para la pila de usuario, por encima de lo que el programa
// declara. mem_size cuenta codigo, datos y .bss, que van al principio
// del area; la pila crece hacia abajo desde el FINAL, asi que necesita
// su propio sitio.
#define TASK_STACK_RESERVE (1u * 1024u * 1024u)

static bool     region_used[REGION_CHUNKS_MAX];
static uint64_t task_area_base[MAX_TASKS];
static uint32_t task_area_size[MAX_TASKS];

// ---- Zonas extra (fase 4 de la memoria) ----
//
// Ademas de su zona principal (la que se reserva al cargarla, del tamaño
// fijo que dice su cabecera), una tarea puede pedir mas memoria MIENTRAS
// CORRE, con SYS_MEM_PEDIR: el kernel reserva otra zona en la reserva,
// este donde este, la marca como suya en sus tablas y le devuelve la
// direccion. Asi un programa pequeño se queda pequeño y uno grande crece
// hasta donde haya memoria. El asignador de Lua (nemo_alloc.c) las usa
// para hacer crecer su monton cuando se le acaba.
//
// Direcciones virtuales = fisicas, como siempre: una zona extra es un
// trozo de RAM que pasa a ser del programa, sin traducciones. Se liberan
// todas a la vez cuando la tarea termina.
#define MAX_EXTRAS 32
static uint64_t extra_base[MAX_TASKS][MAX_EXTRAS];
static uint32_t extra_size[MAX_TASKS][MAX_EXTRAS];

// Busca 'size' bytes CONTIGUOS libres (ya redondeado a trozos de 2MB).
// Devuelve la direccion, o 0 si no hay hueco.
static uint64_t region_alloc(uint32_t size) {
    uint32_t need = (size + REGION_CHUNK - 1) / REGION_CHUNK;
    if (need == 0) return 0;
    for (int g = 0; g < num_segmentos; g++) {
        const segmento_t *sg = &segmentos[g];
        if (need > sg->trozos) continue;
        bool *usado = &region_used[sg->primero];
        for (uint32_t start = 0; start + need <= sg->trozos; start++) {
            uint32_t k = 0;
            while (k < need && !usado[start + k]) k++;
            if (k == need) {
                for (uint32_t j = 0; j < need; j++) usado[start + j] = true;
                return sg->base + (uint64_t)start * REGION_CHUNK;
            }
            start += k;   // saltar el trozo ocupado que corto la busqueda
        }
    }
    return 0;
}

// Lo usado y el total de la reserva, en KB (en bytes no cabia en 32 bits
// con mas de 4 GB de reserva).
void task_pool_usage_kb(uint32_t *used_kb, uint32_t *total_kb) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < total_trozos; i++) if (region_used[i]) n++;
    if (used_kb) *used_kb = n * (REGION_CHUNK / 1024);
    if (total_kb) *total_kb = total_trozos * (REGION_CHUNK / 1024);
}

static void region_free(uint64_t base, uint32_t size) {
    if (base == 0 || size == 0) return;
    for (int g = 0; g < num_segmentos; g++) {
        const segmento_t *sg = &segmentos[g];
        uint64_t fin = sg->base + (uint64_t)sg->trozos * REGION_CHUNK;
        if (base < sg->base || base >= fin) continue;
        uint32_t start = (uint32_t)((base - sg->base) / REGION_CHUNK);
        uint32_t n = (size + REGION_CHUNK - 1) / REGION_CHUNK;
        for (uint32_t j = 0; j < n && start + j < sg->trozos; j++) region_used[sg->primero + start + j] = false;
        return;
    }
}

// Elige la reserva de memoria de programas. Ver el comentario de arriba.
static void poner_dec(uint64_t v) {
    char t[24]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) { char c[2] = { t[--k], 0 }; uart_puts(c); }
}
static void poner_hex(uint64_t v) {
    uart_puts("0x");
    for (int i = 36; i >= 0; i -= 4) {
        uint32_t d = (uint32_t)(v >> i) & 0xF;
        char c[2] = { (char)(d < 10 ? '0' + d : 'a' + d - 10), 0 }; uart_puts(c);
    }
}

static void anadir_segmento(uint64_t base, uint64_t trozos) {
    if (num_segmentos >= MEMORIA_MAX_REGIONES || total_trozos >= REGION_CHUNKS_MAX) return;
    if (trozos > REGION_CHUNKS_MAX - total_trozos) trozos = REGION_CHUNKS_MAX - total_trozos;
    segmentos[num_segmentos].base = base;
    segmentos[num_segmentos].trozos = (uint32_t)trozos;
    segmentos[num_segmentos].primero = total_trozos;
    num_segmentos++;
    total_trozos += (uint32_t)trozos;
}

// Elige la reserva: TODAS las regiones de RAM por encima del GB del
// kernel de al menos 64 MB, en el orden de memoria.c (de direcciones bajas
// a altas; los programas se colocan primero en la mas baja). Si no hay
// ninguna, el array de 128 MB de siempre.
static void elegir_reserva(void) {
    const uint64_t T = REGION_CHUNK;
    uint64_t fin_kernel = memoria_fin_gb_kernel();
    num_segmentos = 0; total_trozos = 0;
    for (int i = 0; i < memoria_num_regiones(); i++) {
        const memoria_region_t *r = memoria_region(i);
        uint64_t b = r->base, f = r->base + r->tam;
        if (b < fin_kernel) b = fin_kernel;          // nunca el GB del kernel
        b = (b + T - 1) & ~(T - 1);                  // trozos enteros de 2 MB
        f &= ~(T - 1);
        if (f > b && f - b >= 64ULL * 1024 * 1024) anadir_segmento(b, (f - b) / T);
    }
    if (num_segmentos == 0) {
        anadir_segmento((uint64_t)task_program_areas, TASK_POOL_BYTES / T);
        uart_puts("tareas: sin RAM por encima del kernel, memoria de programas dentro del kernel\n");
    }
    uart_puts("tareas: memoria de programas: ");
    poner_dec((uint64_t)total_trozos * 2);
    uart_puts(" MB en ");
    poner_dec((uint64_t)num_segmentos);
    uart_puts(num_segmentos == 1 ? " tramo\n" : " tramos\n");
    for (int g = 0; g < num_segmentos; g++) {
        uart_puts("  tramo "); poner_dec((uint64_t)g); uart_puts(": ");
        poner_dec((uint64_t)segmentos[g].trozos * 2); uart_puts(" MB desde ");
        poner_hex(segmentos[g].base); uart_puts("\n");
    }
}

// Devuelve a la reserva las zonas extra de una tarea. Devuelve cuantos MB.
static uint32_t liberar_extras(int32_t slot) {
    uint32_t mb = 0;
    for (int i = 0; i < MAX_EXTRAS; i++) {
        if (extra_base[slot][i] == 0) continue;
        mb += extra_size[slot][i] >> 20;
        region_free(extra_base[slot][i], extra_size[slot][i]);
        extra_base[slot][i] = 0;
        extra_size[slot][i] = 0;
    }
    return mb;
}

// SYS_MEM_PEDIR: una zona mas de memoria para la tarea actual, de al
// menos 'bytes' (se redondea a trozos de 2 MB), puesta a cero -- podria
// haber datos de un programa anterior. Devuelve su direccion, o 0 si no
// hay sitio en la reserva o la tarea ya tiene todas sus zonas extra.
uint64_t task_pedir_memoria(uint64_t bytes) {
    int32_t slot = current_ctx;
    if (slot < 0 || slot >= MAX_TASKS || bytes == 0) return 0;
    if (bytes > 3ULL * 1024 * 1024 * 1024) return 0;       // una zona cabe en 32 bits
    uint32_t tam = (uint32_t)((bytes + REGION_CHUNK - 1) & ~((uint64_t)REGION_CHUNK - 1));
    int libre = -1;
    for (int i = 0; i < MAX_EXTRAS; i++) if (extra_base[slot][i] == 0) { libre = i; break; }
    if (libre < 0) return 0;
    uint64_t base = region_alloc(tam);
    if (base == 0) return 0;
    // Apuntarla YA como suya: si la tarea muriera mientras se limpia, la
    // recogida de siempre (liberar_extras) la devolveria.
    extra_base[slot][libre] = base;
    extra_size[slot][libre] = tam;
    // Ponerla a cero SIN el candado grande (fase 5): con zonas de cientos
    // de MB, limpiarlas con el candado congelaba todo el sistema durante
    // la limpieza. Es seguro: la zona ya esta reservada (region_alloc) y
    // nadie mas la puede recibir, esta tarea esta DENTRO de esta llamada
    // (no puede pedir otra a la vez), y como sigue corriendo en este
    // nucleo, ni la recogida ni la reutilizacion de su hueco pueden
    // tocarla (las dos exigen que no la este ejecutando ningun nucleo).
    bkl_soltar();
    uint64_t *p = (uint64_t *)(uintptr_t)base;
    for (uint64_t k = 0; k < tam / 8; k++) p[k] = 0;
    bkl_tomar();
    mmu_task_marcar_usuario(slot, base, tam);
    uart_puts("tasks: '"); uart_puts(tasks[slot].program_name);
    uart_puts("' crece: zona extra de "); poner_dec(tam >> 20);
    uart_puts(" MB en "); poner_hex(base); uart_puts("\n");
    return base;
}

// Devuelve a la reserva la memoria de los programas que ya terminaron.
//
// Antes, la region de un programa cerrado solo se liberaba cuando OTRO
// programa reutilizaba su hueco de tarea (task_spawn_from_file): hasta
// entonces, sus megas -- montón de Lua incluido, con toda su basura --
// seguian reservados. Ahora el nucleo 0 la recoge en cada vuelta de su
// bucle principal, en cuanto es seguro: la tarea ha terminado Y ningun
// nucleo la esta ejecutando (puede seguir un instante en el suyo
// mientras sale). Se llama con el candado grande, como todo el kernel.
//
// La tabla de la MMU del hueco sigue mapeando esa region como de usuario,
// pero ningun programa corre ya con ella; cuando el hueco se reutilice,
// mmu_set_task_area la rehace e invalida la TLB.
void tasks_recoger_terminadas(void) {
    for (int32_t i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].used || !tasks[i].finished || tasks[i].nucleo >= 0) continue;
        if (task_area_size[i] == 0) continue;               // ya recogida
        uint32_t mb = task_area_size[i] >> 20;
        region_free(task_area_base[i], task_area_size[i]);
        task_area_base[i] = 0;
        task_area_size[i] = 0;
        mb += liberar_extras(i);                            // y sus zonas extra
        gpio_liberar_tarea(i);                              // y sus pines, a entrada
        char t[12]; int k = 0; uint32_t v = mb;
        uart_puts("tasks: memoria de '"); uart_puts(tasks[i].program_name); uart_puts("' devuelta (");
        do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k) { char c[2] = { t[--k], 0 }; uart_puts(c); }
        uart_puts(" MB)\n");
    }
}

static bool ctx_is_ready(int32_t idx) {
    // Contextos de reposo: cada nucleo solo puede volver al SUYO. El
    // KERNEL_CTX (reposo del nucleo 0) nunca debe elegirlo un secundario:
    // es el contexto con el que el nucleo 0 hace el trabajo del sistema, y
    // si lo ejecutaran dos nucleos a la vez se pisarian la pila.
    if (idx >= MAX_TASKS) return idx == IDLE_CTX(cpu_id());
    // El nucleo 0 es SOLO para el sistema (ventanas, teclado, raton, red,
    // redibujado) si hay secundarios que ejecuten los programas. Antes su
    // ronda tambien cogia programas cuando le sobraba tiempo, como cuando
    // era el unico nucleo; pero su trabajo del sistema solo ocurre cuando
    // la tarea que ejecuta cede el turno, asi que un programa atascado en
    // el nucleo 0 congelaba el sistema ENTERO (paso con fuente.nb). Ahora
    // un programa atascado se lleva su nucleo, y el sistema sigue vivo.
    // Si ningun secundario arranco, el 0 sigue ejecutandolo todo.
    if (cpu_id() == 0 && smp_nucleos_en_marcha() > 1) return false;
    if (!tasks[idx].used || tasks[idx].finished) return false;
    // Dormida hasta un latido que aun no ha llegado: no esta lista.
    if (tasks[idx].despertar_en != 0 && timer_get_ticks() < tasks[idx].despertar_en) return false;
    // Lista solo si no la esta ejecutando OTRO nucleo. La que corre en
    // este mismo nucleo si cuenta: es la tarea actual, y el bucle de
    // task_yield la reconoce (next == current_ctx) para no saltar a si
    // misma.
    return tasks[idx].nucleo < 0 || tasks[idx].nucleo == (int32_t)cpu_id();
}

// Stub de salida que se copia al final del area de programa de cada
// tarea. Un programa termina haciendo 'ret' desde su _start (la
// convencion de siempre, ver la nota grande en syscall.h) -- pero
// desde EL0 no se puede "volver" a codigo de kernel. Asi que x30 se
// deja apuntando a ESTE stub, dentro del area de la tarea (la unica
// memoria que EL0 puede ejecutar): el 'ret' del programa cae aqui, y
// el stub pide SYS_EXIT. Ningun programa existente tiene que cambiar
// ni una linea. Vive en .rodata del kernel solo como PLANTILLA; se
// copia (12 bytes) y se sincroniza la cache de instrucciones antes
// de usarlo.
__asm__(
    ".section .rodata\n"
    ".balign 4\n"
    ".global task_exit_stub_template\n"
    "task_exit_stub_template:\n"
    "    mov x8, #0\n"            // SYS_EXIT (ver syscall.h)
    "    svc #0\n"
    "    b .\n"                   // por si el kernel volviera (no debe)
    ".global task_exit_stub_template_end\n"
    "task_exit_stub_template_end:\n"
    ".text\n");
extern const uint8_t task_exit_stub_template[];
extern const uint8_t task_exit_stub_template_end[];

// Salta a EL0 de verdad: pila de usuario, direccion de entrada, modo
// EL0t, y eret. No vuelve nunca -- a partir de aqui el programa solo
// entra en el kernel por syscalls o excepciones.
static void enter_el0(uint64_t entry, uint64_t user_sp, uint64_t exit_stub) __attribute__((noreturn));
static void enter_el0(uint64_t entry, uint64_t user_sp, uint64_t exit_stub) {
    // SPSR_EL1 para la tarea: M[3:0]=0000 (EL0t), y la misma mascara
    // de interrupciones que lleva el kernel (D y F enmascaradas, IRQ y
    // SError habilitadas, ver kernel_main): 0x240. Con IRQ habilitada
    // en EL0 el temporizador sigue llegando, ahora por la entrada
    // "EL0 AArch64 IRQ" de la tabla de vectores.
    uint64_t spsr = (1UL << 9) | (1UL << 6);

    // Soltar el candado grande del kernel (bkl.c): es la PRIMERA vez que
    // esta tarea vuelve a un programa, y no pasa por la salida normal de
    // una excepcion (exceptions.s), que es la que lo suelta siempre.
    // Primero enmascarar las interrupciones y DESPUES soltar, igual que
    // alli: al reves, una interrupcion colada en medio correria codigo
    // del kernel sin el candado. (El asm de abajo vuelve a enmascarar;
    // es inofensivo repetirlo.)
    __asm__ volatile("msr daifset, #2" ::: "memory");
    bkl_soltar();

    // Enmascarar IRQ ANTES de tocar elr_el1/spsr_el1 -- exactamente el
    // mismo motivo que RESTORE_STATE en exceptions.s: una interrupcion
    // entre esas escrituras y el eret los sobreescribiria con el punto
    // interrumpido, y el eret saltaria a un sitio equivocado. El eret
    // restaura la mascara real (IRQ habilitada) desde el SPSR.
    __asm__ volatile(
        "msr daifset, #2\n"
        "msr sp_el0, %0\n"
        "msr elr_el1, %1\n"
        "msr spsr_el1, %2\n"
        "mov x30, %3\n"
        "eret\n"
        :
        : "r"(user_sp), "r"(entry), "r"(spsr), "r"(exit_stub)
        : "x30", "memory");
    __builtin_unreachable();
}

// Punto de entrada real de toda tarea nueva -- ver task_spawn_from_file
// para como "saltamos" aqui la primera vez que se le da tiempo. Corre
// en EL1 sobre la pila de kernel de la tarea; termina con un eret a
// EL0 y no vuelve.
static void task_trampoline(void) {
    task_t *t = &tasks[current_ctx];

    uart_puts("tasks: task_trampoline saltando a la tarea '");
    uart_puts(t->program_name);
    uart_puts("' (contexto=");
    { char n[4]; int i=0; int32_t v=current_ctx; if(v==0){n[i++]='0';} while(v>0 && i<4){n[i++]=(char)('0'+(v%10)); v/=10;} while(i>0) uart_putc(n[--i]); }
    // En que nucleo arranca: desde la fase 5b del SMP, cualquiera de los
    // cuatro puede coger una tarea nueva, y esta linea es la forma mas
    // sencilla de verlo.
    uart_puts(") en EL0, nucleo ");
    uart_putc((char)('0' + cpu_id()));
    uart_puts("...\n");

    // Stub de salida en los ultimos 16 bytes del area; pila de usuario
    // justo debajo, creciendo hacia abajo (asi nunca pisa el stub).
    uint8_t *area = (uint8_t *)task_area_base[current_ctx];
    uint8_t *stub = area + task_area_size[current_ctx] - 16;
    uint32_t stub_len = (uint32_t)(task_exit_stub_template_end - task_exit_stub_template);
    for (uint32_t i = 0; i < stub_len && i < 16; i++) stub[i] = task_exit_stub_template[i];
    sync_icache(stub, 16);

    enter_el0((uint64_t)t->entry, (uint64_t)stub, (uint64_t)stub);
}

// Termina la tarea ACTUAL. Lo llama SYS_EXIT (el final normal de un
// programa, via el stub de salida) y tambien el manejador de
// excepciones cuando una tarea en EL0 falla (acceso a memoria
// invalido, instruccion ilegal): en vez de colgar el sistema entero,
// se retira solo la tarea culpable. Nunca vuelve.
// Aviso por la terminal cuando una tarea termina, con cuantas quedan
// vivas. El kernel ya anunciaba cada arranque, pero no los finales, y
// asi no habia forma de ver desde fuera si un programa cerrado seguia
// vivo en segundo plano (con sus fuentes, imagenes y memoria).
static void anunciar_fin(int32_t slot, const char *como) {
    char num[12]; int n;
    uart_puts("tasks: termina la tarea ");
    n = 0; { uint32_t v = (uint32_t)slot; char t[12]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) num[n++] = t[--k]; } num[n] = 0;
    uart_puts(num);
    uart_puts(" '"); uart_puts(tasks[slot].program_name); uart_puts("' (");
    uart_puts(como);
    uint32_t vivas = 0;
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i].used && !tasks[i].finished) vivas++;
    n = 0; { uint32_t v = vivas; char t[12]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) num[n++] = t[--k]; } num[n] = 0;
    uart_puts(") -- quedan "); uart_puts(num); uart_puts(" vivas\n");
}

void task_exit_current(void) {
    if (current_ctx == KERNEL_CTX || current_ctx < 0 || current_ctx >= MAX_TASKS) {
        uart_puts("tasks: task_exit_current fuera de una tarea -- deteniendo\n");
        while (1) { __asm__ volatile("wfe"); }
    }
    task_t *t = &tasks[current_ctx];
    if (t->window_idx >= 0) {
        gadgets_free_window(t->window_idx);
        wm_destroy_window(t->window_idx);
    }
    t->finished = true;
    anunciar_fin(current_ctx, "salio ella misma");
    while (1) {
        task_yield(); // el planificador ya no vuelve a darnos tiempo
    }
}

void tasks_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].nucleo = -1;
        tasks[i].used = false;
        tasks[i].finished = false;
    }
    current_ctx = KERNEL_CTX;
    gadgets_init();

    // Fase 2 de la separacion kernel/programas: una tabla de traduccion
    // por hueco de tarea, donde solo SU area de programa es memoria de
    // usuario -- ni el kernel ni las areas de las otras tareas. Se
    // construyen aqui una sola vez (mmu_init ya corrio, kernel_main la
    // llama antes) y task_yield cambia de tabla en cada cambio de
    // tarea. Ver mmu_pi4.c.
    // Al arrancar, ningun hueco tiene memoria de usuario: cada tarea
    // recibe su propia region al lanzarse (mmu_set_task_area).
    mmu_build_task_tables((uint64_t)task_program_areas, 0, MAX_TASKS);
    elegir_reserva();
}

// Interpretes registrados: un archivo .lua no es un ejecutable NEXE,
// asi que lanzarlo directamente fallaria en el loader. En su lugar se
// lanza LUA.PRO con "script.lua [argumentos]" como argumento -- igual
// que hace Linux con binfmt_misc. Como este es el UNICO punto por el
// que pasa todo lanzamiento (shell 'run', doble clic del explorador,
// iconos del escritorio, SYS_LAUNCH_PROGRAM), basta con hacerlo aqui.
static bool ends_with_lua(const char *name) {
    uint32_t n = 0; while (name[n]) n++;
    if (n < 4) return false;
    const char *e = name + n - 4;
    return e[0] == '.' && (e[1] == 'l' || e[1] == 'L') && (e[2] == 'u' || e[2] == 'U') && (e[3] == 'a' || e[3] == 'A');
}

int32_t task_spawn_from_file(const char *filename, uint32_t parent_inode, int32_t window_idx,
                              const char *arg, int32_t console_window) {
    static char lua_arg[128];
    if (ends_with_lua(filename)) {
        // BUG REAL CORREGIDO: la version anterior solo pasaba el
        // nombre suelto del script ("script.lua"), perdiendo por
        // completo en que carpeta vivia -- LUA.PRO, al leerlo, solo
        // sabe buscar por nombre en la raiz (ver
        // nemo_plat_read_file en nemo_platform_nemo.c, que llama a
        // SYS_FILE_OPEN con parent=0 fijo). Un .lua en cualquier otra
        // carpeta nunca se encontraba. La carpeta ORIGINAL del script
        // (antes de reescribir parent_inode para encontrar LUA.PRO)
        // se codifica ahora al principio del argumento como
        // "INODO:nombre.lua argumentos" -- nemo_main.c la separa y
        // usa una lectura consciente de esa carpeta en vez de la
        // busqueda por nombre de siempre.
        uint32_t p = 0;
        uint32_t carpeta_script = parent_inode;
        // Si el .lua no esta en la carpeta desde la que se lanza, se
        // busca en /ACCESORIOS (donde el kernel instala los programas
        // en Lua al arrancar) -- asi 'run reloj.lua' vale desde
        // cualquier carpeta, igual que un .pro de PROGRAMAS.
        if (nemofs_find_child(parent_inode, filename) < 0) {
            uint32_t acc = kernel_get_accesorios_dir();
            if (nemofs_find_child(acc, filename) >= 0) carpeta_script = acc;
        }
        char inodo_txt[12]; int ni = 0;
        if (carpeta_script == 0) { inodo_txt[ni++] = '0'; }
        else { char tmp[12]; int nt = 0; uint32_t v = carpeta_script;
               while (v > 0 && nt < 11) { tmp[nt++] = (char)('0' + (v % 10)); v /= 10; }
               while (nt > 0) inodo_txt[ni++] = tmp[--nt]; }
        for (int i = 0; i < ni && p < sizeof(lua_arg) - 2; i++) lua_arg[p++] = inodo_txt[i];
        if (p < sizeof(lua_arg) - 2) lua_arg[p++] = ':';
        for (uint32_t i = 0; filename[i] && p < sizeof(lua_arg) - 2; i++) lua_arg[p++] = filename[i];
        if (arg && arg[0]) {
            lua_arg[p++] = ' ';
            for (uint32_t i = 0; arg[i] && p < sizeof(lua_arg) - 1; i++) lua_arg[p++] = arg[i];
        }
        lua_arg[p] = '\0';
        filename = "LUA.PRO";
        arg = lua_arg;
        // LUA.PRO vive en /PROGRAMAS, no donde este el propio script --
        // reutiliza el inodo ya resuelto una vez en el arranque (ver
        // g_programas_dir/kernel_get_programas_dir en kernel.c) en vez
        // de repetir la busqueda en cada lanzamiento.
        parent_inode = kernel_get_programas_dir();
    }
    int32_t slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        // Un hueco terminado solo se reutiliza si NINGUN nucleo lo esta
        // ejecutando. Al cerrar una app se marca terminada al momento,
        // pero si corre en otro nucleo sigue hasta su proxima llamada al
        // sistema; reutilizar su hueco en ese intervalo liberaria y
        // reasignaria su memoria mientras aun la esta usando.
        if (!tasks[i].used || (tasks[i].finished && tasks[i].nucleo < 0)) { slot = i; break; }
    }
    if (slot < 0) {
        uart_puts("tasks: no hay hueco para una tarea nueva\n");
        return -1;
    }

    // Si el hueco lo uso antes otra tarea (ya terminada), devolver su
    // region ANTES de pedir la nueva. Este es el unico sitio donde un
    // hueco se recicla, asi que es el unico donde hay que liberar: si
    // no, cada lanzamiento dejaria atras la region del anterior y la
    // reserva se agotaria.
    if (task_area_base[slot] != 0) {
        region_free(task_area_base[slot], task_area_size[slot]);
        task_area_base[slot] = 0;
        task_area_size[slot] = 0;
    }
    liberar_extras(slot);   // por si no las recogio tasks_recoger_terminadas
    gpio_liberar_tarea(slot);

    // Limpiamos a cero el hueco ANTES de cargar el codigo nuevo -- si
    // no, un programa que espere que su propia zona .bss (variables
    // globales, buffers internos...) empiece en cero se encontraria
    // con basura dejada por el programa ANTERIOR que uso este mismo
    // hueco. El formato .pro tampoco incluye los bytes de .bss en el
    // archivo (es zona reservada, no datos) -- asi que esta limpieza
    // es la unica garantia real de que arranca en cero. Limpiamos de
    // 8 en 8 bytes (no byte a byte) porque con huecos de 6MB, un
    // bucle de un solo byte por vuelta seria notablemente lento.
    // Cuanta memoria darle. Los programas de version 3 lo declaran en
    // la cabecera; los demas reciben el tamaño por defecto.
    uint32_t declarado = loader_required_size(filename, parent_inode);
    uint32_t area_size;
    if (declarado == 0) {
        area_size = DEFAULT_TASK_SIZE;
    } else {
        uint64_t con_pila = (uint64_t)declarado + TASK_STACK_RESERVE;
        area_size = (uint32_t)(((con_pila + REGION_CHUNK - 1) / REGION_CHUNK) * REGION_CHUNK);
    }
    uint64_t area_base = region_alloc(area_size);
    if (area_base == 0) {
        uart_puts("tasks: no hay memoria contigua libre para '");
        uart_puts(filename);
        uart_puts("'\n");
        return -1;
    }
    task_area_base[slot] = area_base;
    task_area_size[slot] = area_size;

    uart_puts("tasks: limpiando hueco de memoria...\n");
    uint64_t *area64 = (uint64_t *)area_base;
    for (uint32_t i = 0; i < area_size / 8; i++) area64[i] = 0;

    // La tabla de este hueco pasa a mapear SOLO su region nueva, y se
    // invalida la TLB de su ASID (ver mmu_set_task_area).
    mmu_set_task_area(slot, area_base, area_size);

    uart_puts("tasks: hueco limpio, cargando '");
    uart_puts(filename);
    // El ARGUMENTO tambien al registro: cuando LUA.PRO "sale ella misma"
    // sin decir nada, lo primero que hace falta saber es QUE script se le
    // pidio abrir -- sin esto el arranque no lo contaba en ninguna parte.
    if (arg && arg[0]) {
        uart_puts("' con argumento '");
        uart_puts(arg);
    }
    uart_puts("'...\n");

    void (*entry)(void) = 0;
    if (!loader_load_into(filename, parent_inode, (uint8_t *)area_base, area_size, &entry)) {
        // devolver la region: la tarea no llego a existir
        region_free(area_base, area_size);
        task_area_base[slot] = 0;
        task_area_size[slot] = 0;
        return -1;
    }
    uart_puts("tasks: archivo cargado en memoria correctamente, preparando la tarea...\n");

    tasks[slot].used = true;
    tasks[slot].finished = false;
    // Ningun nucleo la ejecuta todavia. Importa ponerlo aqui: el hueco
    // puede venir de una tarea terminada, y si conservara su marca vieja
    // el planificador creeria que otro nucleo la esta ejecutando y no la
    // cogeria nunca.
    tasks[slot].nucleo = -1;
    tasks[slot].despertar_en = 0;   // despierta (el hueco puede venir de una tarea dormida)
    tasks[slot].generacion++;       // un programa nuevo en este hueco: lo del anterior ya es huerfano
    // Estado de dibujo limpio (Origin, SetBuffer, Viewport, fuente...):
    // si no, heredaria el de la tarea que ocupo antes este hueco. Ver la
    // nota junto a estado_dibujo en syscall.c.
    syscall_reiniciar_dibujo(slot);
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
        while (arg[i] != '\0' && i < TASK_LAUNCH_ARG_MAX - 1) { tasks[slot].launch_arg[i] = arg[i]; i++; }
    }
    tasks[slot].launch_arg[i] = '\0';
    uart_puts("tasks: paso 3 (argumento copiado) hecho\n");

    // Preparamos un "contexto falso" en la cima de la pila nueva: los
    // registros x19-x28, x29, y d8-d15 no importan (nadie los ha
    // usado todavia), pero x30 (el registro de enlace) SI importa --
    // es la direccion a la que "volveremos" la primera vez que
    // task_switch() haga su 'ret' hacia esta tarea. Poniendo ahi
    // task_trampoline, conseguimos que arranque el programa de
    // verdad la primera vez que le demos tiempo. El tamaño (176
    // bytes = 20 registros + SP_EL0 + relleno, de 8 bytes cada uno)
    // tiene que coincidir EXACTO con lo que tasks_switch.s espera
    // encontrarse al restaurar.
    uint8_t *stack_top = task_stacks[slot] + TASK_STACK_SIZE;
    stack_top = (uint8_t *)((uint64_t)stack_top & ~0xFULL);
    uint64_t *frame = (uint64_t *)(stack_top - 176);
    for (int i = 0; i < 22; i++) frame[i] = 0; // x19-x28, x29, d8-d15, sp_el0, relleno
    frame[11] = (uint64_t)task_trampoline;      // x30 (link register)
    uart_puts("tasks: paso 4 (marco de pila preparado) hecho\n");

    all_saved_sp[slot] = (uint64_t)frame;

    if (window_idx >= 0) {
        wm_set_owns_content(window_idx, true);
    }

    return slot;
}

void task_yield(void) {
    // Doble bufer de las ventanas (wm.c): una tarea que cede el turno
    // esta en un punto entre fotogramas, asi que lo que ha dibujado es un
    // fotograma completo y se puede mostrar. Es exactamente lo que pasaba
    // con un solo nucleo: la pantalla enseñaba lo que la tarea tenia
    // dibujado en su ultima cesion. Si no ha dibujado nada desde la ultima
    // vez, wm_publicar_contenido no copia nada.
    if (current_ctx >= 0 && current_ctx < MAX_TASKS && tasks[current_ctx].window_idx >= 0) {
        wm_publicar_contenido(tasks[current_ctx].window_idx);
    }

    // El "tick" del sistema -- teclado, raton, red, ventanas, gadgets,
    // redibujado -- y el wfe se hacen UNA VEZ POR VUELTA, en el turno
    // del kernel, no cada vez que una tarea cede el control.
    //
    // Antes iban en CADA task_yield, y el wfe duerme hasta el siguiente
    // latido del reloj (hasta 10 ms a 100 Hz). Con una tarea no se
    // notaba; con ocho, cada una dormia su trozo antes de pasar a la
    // siguiente y una vuelta completa costaba ~80 ms. El explorador
    // solo recibe turno una vez por vuelta, asi que un clic esperaba
    // todo eso: cada app abierta añadia 10 ms a cada clic, y el sistema
    // se volvia mas lento cuantas mas cosas habia abiertas.
    //
    // El kernel participa en la ronda exactamente una vez por vuelta
    // (ctx_is_ready lo da siempre por listo), asi que es el sitio justo
    // para hacer el trabajo del sistema y dormir un poco. Una vuelta
    // entera cuesta ahora un solo wfe, tenga las tareas que tenga.
    //
    // Lo que no cambia: las tareas que esperan algo (Input$, WaitEvent,
    // Delay...) siguen cediendo en bucle y ven la entrada nueva en la
    // vuelta siguiente, igual que antes.
    if (current_ctx == KERNEL_CTX) {
        // ---- Leer la entrada rapido, pintar a ritmo fijo ----
        //
        // Antes todo esto iba junto, una vez por vuelta, y la vuelta duraba lo
        // que durase el wfe: hasta 10 ms. Ahora el wfe dura ~1,2 ms (flujo de
        // eventos, ver timer_dormir_corto), asi que las vueltas son ~8 veces
        // mas frecuentes. Eso es bueno para el RATON -- se lee ocho veces mas
        // a menudo -- y malo para todo lo demas si se compusiera igual de
        // seguido: componer tiene el candado grande casi todo su rato, y a 800
        // fotogramas por segundo no entraria nadie mas. Ese fallo ya paso una
        // vez (130 composiciones por segundo, el candado cogido el 97% del
        // tiempo), y de ahi salio el wfe hasta el latido, que arreglaba la
        // composicion a costa de la entrada.
        //
        // Asi que cada cosa a su ritmo: la entrada en CADA vuelta, la pantalla
        // a 60 por segundo, y la red a 100 como siempre. Si algun dia hace
        // falta, estos dos numeros son el mando.
        #define COMPOSICIONES_POR_SEGUNDO 60u
        #define RED_POR_SEGUNDO          100u
        static uint64_t proxima_pantalla_us = 0;
        static uint64_t proxima_red_us = 0;

        input_poll();               // el raton y el teclado, siempre

        uint64_t ahora_us = timer_micros();
        if (ahora_us >= proxima_red_us) {
            proxima_red_us = ahora_us + 1000000u / RED_POR_SEGUNDO;
            net_poll(); // la pila de red (net.c), en los dos builds -- ver nic.h
        }
        if (ahora_us >= proxima_pantalla_us) {
            proxima_pantalla_us = ahora_us + 1000000u / COMPOSICIONES_POR_SEGUNDO;
            wm_update();
            gadgets_update_and_draw();
            gadgets_check_timers();
            gadgets_check_hotkeys();
            wm_draw_if_needed();
        }
        medir_informar_si_toca();   // medir.h: informe de rendimiento cada ~2 s

        // Dormir SIN el candado grande. Este wfe dura hasta el siguiente
        // latido del reloj (hasta 10 ms); con el candado cogido, cualquier
        // otro nucleo que quisiera entrar en el kernel se quedaria
        // esperando todo ese tiempo, en cada vuelta.
        //
        // Mientras tanto puede llegar una interrupcion: su manejador
        // (irq_stub en exceptions.s) coge el candado si falta, asi que no
        // corre kernel sin el. Y bkl_tomar/bkl_soltar enmascaran las
        // interrupciones mientras trabajan, para que ninguna se cuele a
        // mitad (ver bkl.c).
        bkl_soltar();
        // Dormir un rato CORTO: hasta el proximo evento del contador (~1,2 ms)
        // o hasta cualquier interrupcion, lo que llegue antes.
        //
        // Antes se dormia hasta que AVANZABA el latido de 100 Hz, o sea hasta
        // 10 ms. Eso hacia falta porque componer y leer el raton iban juntos:
        // dormir poco era componer sin parar y no dejar el candado a nadie.
        // Ahora van separados (el ritmo de la pantalla se limita arriba), asi
        // que se puede dormir poco sin ese efecto -- y el raton se lee ocho
        // veces mas a menudo.
        //
        // Por que dos wfe y no uno, y por que degrada bien si el flujo de
        // eventos no esta activo: en timer_dormir_corto (src/timer.c).
        timer_dormir_corto();
        bkl_tomar();
    } else if (current_ctx == IDLE_CTX(cpu_id())) {
        // Reposo de un nucleo SECUNDARIO (fase 5b): no hace el trabajo del
        // sistema -- ventanas, teclado, red y redibujado siguen siendo del
        // nucleo 0, porque las interrupciones de los dispositivos le
        // llegan a el. Solo duerme sin el candado hasta que su propio flujo
        // de eventos del temporizador lo despierte (cada ~1 ms, ver
        // secundario_main en smp.c) y vuelve a mirar si hay alguna tarea.
        bkl_soltar();
        __asm__ volatile("wfe");
        bkl_tomar();
    }

    // Buscamos la siguiente tarea lista, en ronda circular, incluyendo
    // el contexto de reposo de ESTE nucleo como un participante mas. Las
    // posiciones de la ronda van de 0 a MAX_TASKS; la ultima representa
    // el reposo propio (KERNEL_CTX en el nucleo 0, IDLE_CTX(n) en los
    // demas).
    int32_t reposo = IDLE_CTX(cpu_id());
    int32_t pos = (current_ctx >= MAX_TASKS) ? MAX_TASKS : current_ctx;
    int32_t next = current_ctx;
    for (int i = 0; i <= MAX_TASKS; i++) {
        pos = (pos + 1) % (MAX_TASKS + 1);
        int32_t candidato = (pos == MAX_TASKS) ? reposo : pos;
        if (ctx_is_ready(candidato)) { next = candidato; break; }
    }

    if (next == current_ctx) {
        return; // nadie mas a quien cambiar -- seguimos donde estabamos
    }

    int32_t prev = current_ctx;
    current_ctx = next;

    // La tarea que se deja ya no corre en ningun nucleo; la nueva, en
    // este. Se marca ANTES del salto y con el candado grande cogido: otro
    // nucleo solo podria coger la tarea que dejamos despues de tener el
    // candado, y este nucleo no lo suelta hasta volver al programa de la
    // nueva -- para entonces los registros de la anterior ya estan
    // guardados del todo. Asi nadie la coge a medio guardar.
    if (prev >= 0 && prev < MAX_TASKS) tasks[prev].nucleo = -1;
    if (next >= 0 && next < MAX_TASKS) tasks[next].nucleo = (int32_t)cpu_id();
    if (next >= 0 && next < MAX_TASKS) task_turn_count[next]++;
    // Tabla de traduccion de la tarea a la que vamos (la del kernel si
    // es el contexto de kernel). Todo lo que corre entre aqui y el eret
    // que devuelve el control a esa tarea en EL0 es codigo y datos de
    // kernel, identicos en todas las tablas -- da igual cual este
    // activa mientras tanto.
    mmu_switch_context(next >= MAX_TASKS ? -1 : next);   // reposo de cualquier nucleo: tabla del kernel
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
    if (out_base) *out_base = task_area_base[current_ctx];
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
            anunciar_fin(i, "cerrada con su ventana");
            return;
        }
    }
}

// Para el gestor de tareas -- vuelca hasta 'max_entries' tareas vivas
// en 'out', 48 bytes cada una: slot(4) + window_idx(4) + turnos(4,
// solo 32 bits bajos -- de sobra para lo que un contador de turnos
// necesita mostrar) + reservado(4) + nombre(32, con \0). Devuelve
// cuantas se escribieron.
uint32_t task_list_dump(uint8_t *out, uint32_t max_entries) {
    uint32_t n = 0;
    for (int i = 0; i < MAX_TASKS && n < max_entries; i++) {
        if (!tasks[i].used || tasks[i].finished) continue;
        uint8_t *e = out + n * 48;
        uint32_t slot = (uint32_t)i, win = (uint32_t)tasks[i].window_idx, turnos = (uint32_t)task_turn_count[i];
        for (int b = 0; b < 4; b++) e[0 + b] = (uint8_t)(slot >> (b * 8));
        for (int b = 0; b < 4; b++) e[4 + b] = (uint8_t)(win >> (b * 8));
        for (int b = 0; b < 4; b++) e[8 + b] = (uint8_t)(turnos >> (b * 8));
        for (int b = 0; b < 4; b++) e[12 + b] = 0; // reservado
        int j = 0;
        while (tasks[i].program_name[j] != '\0' && j < 31) { e[16 + j] = (uint8_t)tasks[i].program_name[j]; j++; }
        e[16 + j] = 0;
        n++;
    }
    return n;
}

// Mata una tarea por su 'slot' (el mismo indice que devuelve
// task_list_dump). Hace las MISMAS tres cosas que el cierre por la X
// de una ventana sin modo evento (ver el comentario junto a esa logica
// en wm.c): marcar 'finished' no basta por si solo -- nada reaprovecha
// eso automaticamente, asi que sin destruir tambien la ventana y sus
// gadgets aqui, quedaria una ventana fantasma en pantalla, viva
// visualmente aunque la tarea ya no lo este. true si se encontro y
// estaba viva.
bool task_kill_by_slot(int32_t slot) {
    if (slot < 0 || slot >= MAX_TASKS) return false;
    if (!tasks[slot].used || tasks[slot].finished) return false;
    int32_t win = tasks[slot].window_idx;
    tasks[slot].finished = true;
    anunciar_fin(slot, "matada desde fuera");
    if (win >= 0) {
        gadgets_free_window(win);
        wm_destroy_window(win);
    }
    return true;
}

// Cuantos de los MAX_TASKS huecos de tarea estan ocupados ahora mismo
// -- para el monitor de "RAM" (que en realidad es memoria de tareas,
// ver la nota junto a SYS_RAM_TASKS en syscall.h).
uint32_t task_count_used(void) {
    uint32_t n = 0;
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i].used && !tasks[i].finished) n++;
    return n;
}

// Validacion de punteros que llegan por syscall (roadmap, punto
// pendiente de la Fase 1/2 de separacion kernel/programas). La MMU
// por si sola NO basta: protege a las tareas ENTRE SI y del kernel,
// pero el propio kernel, corriendo en EL1 para atender una syscall,
// tiene acceso de lectura/escritura a TODA la RAM sin restriccion
// (AP=00 en la tabla de cualquier tarea da a EL1 acceso completo,
// necesario para que el kernel pueda cargar programas, gestionar
// ventanas ajenas, etc.) -- asi que un puntero que una tarea pase a
// una syscall, si apunta a memoria de otra tarea o del kernel, la
// MMU lo deja pasar sin mas. La comprobacion tiene que ser por
// software, aqui, ANTES de que el kernel toque ese puntero.
//
// 'ctx' es MAX_TASKS o KERNEL_CTX para "sin restriccion" (llamadas
// internas del kernel que no vienen de una tarea real).
// Slot (0..MAX_TASKS-1) de la tarea que esta ejecutandose ahora
// mismo, o KERNEL_CTX si es codigo del propio kernel. Para
// task_owns_range() desde syscall.c, que no ve 'current_ctx' (es
// privada de este archivo).
int32_t task_get_current_slot(void) { return current_ctx; }

// Para los recursos del kernel (syscall.c): la generacion de un hueco, y
// si el programa que tenia esa generacion sigue vivo.
uint32_t task_generacion(int32_t slot) {
    return (slot >= 0 && slot < MAX_TASKS) ? tasks[slot].generacion : 0;
}
bool task_viva(int32_t slot, uint32_t generacion) {
    return slot >= 0 && slot < MAX_TASKS && tasks[slot].used && !tasks[slot].finished
        && tasks[slot].generacion == generacion;
}

// ¿La tarea que esta haciendo esta llamada al sistema ya ha sido
// terminada (por ejemplo, cerrando su ventana)? Ver handle_sync.
bool task_actual_terminada(void) {
    return current_ctx >= 0 && current_ctx < MAX_TASKS && tasks[current_ctx].finished;
}

// Dormir la tarea actual hasta el latido 'latido' del reloj.
//
// La tarea cede el turno UNA vez y el planificador no la vuelve a mirar
// hasta que el reloj llegue (ctx_is_ready). Antes, Pump y Delay cedian el
// turno en bucle y la tarea se despertaba muchas veces por latido solo
// para comprobar la hora; con varios nucleos eso eran cientos de vueltas
// por segundo por cada programa en reposo, peleando por el candado.
//
// El bucle es una red de seguridad: si por lo que sea la tarea volviera
// antes de tiempo, sigue durmiendo.
void task_dormir_hasta(uint64_t latido) {
    if (current_ctx < 0 || current_ctx >= MAX_TASKS) {
        // El kernel no es una tarea: no puede dormir asi. Cede y ya.
        task_yield();
        return;
    }
    tasks[current_ctx].despertar_en = latido;
    while (timer_get_ticks() < latido) task_yield();
    tasks[current_ctx].despertar_en = 0;
}

// ¿La tarea dueña de esta ventana se esta ejecutando AHORA MISMO en otro
// nucleo? Para el compositor (wm_draw_if_needed): si es asi, puede estar
// a medio dibujar un fotograma, y su lienzo no se debe publicar todavia.
// Si no, esta parada en un punto de cesion y su dibujo esta completo.
bool tasks_ventana_en_otro_nucleo(int32_t ventana) {
    int32_t yo = (int32_t)cpu_id();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].used && !tasks[i].finished && tasks[i].window_idx == ventana &&
            tasks[i].nucleo >= 0 && tasks[i].nucleo != yo) {
            return true;
        }
    }
    return false;
}

// Un nucleo secundario entra en el planificador (fase 5b). Hasta aqui su
// current_ctx valia KERNEL_CTX, el valor inicial de todos: se cambia a SU
// contexto de reposo, porque KERNEL_CTX es el del nucleo 0 y los dos no
// pueden compartirlo. Se llama con el candado grande cogido.
void tasks_entrar_nucleo(uint32_t nucleo) {
    current_ctx_por_nucleo[nucleo] = IDLE_CTX(nucleo);
}

bool task_owns_range(int32_t ctx, uint64_t addr, uint64_t len) {
    if (ctx == KERNEL_CTX || ctx < 0 || ctx >= MAX_TASKS) return true;
    if (len == 0) return true;
    uint64_t base = task_area_base[ctx];
    uint64_t end = base + task_area_size[ctx];
    if (base == 0) return false;   // hueco sin region: nada es suyo
    uint64_t addr_end = addr + len;
    if (addr_end < addr) return false;              // desbordamiento: addr+len dio la vuelta
    if (addr >= base && addr_end <= end) return true;
    // o entero dentro de una de sus zonas extra (fase 4)
    for (int i = 0; i < MAX_EXTRAS; i++) {
        uint64_t b = extra_base[ctx][i];
        if (b == 0) continue;
        if (addr >= b && addr_end <= b + extra_size[ctx][i]) return true;
    }
    return false;
}

// Si la tarea actual todavia no tiene ventana grafica propia (nacio
// en "modo consola", redirigiendo su Print a quien la lanzo), le
// creamos una AHORA, la primera vez que de verdad hace falta -- al
// llamar a cualquier comando grafico o de gadgets. A partir de aqui,
// esta tarea ya no es "un programa de consola", es una app con
// ventana, aunque su Print siga yendo a la consola que la lanzo.
// Cierra la ventana de la tarea que llama, sin terminar la tarea.
//
// Cada tarea tiene UNA ventana, y hasta ahora solo desaparecia cuando el
// programa entero terminaba. Con esto un programa puede quitarla y seguir
// trabajando -- guardar lo que estaba haciendo, imprimir por consola, o
// abrir otra ventana distinta con CreateWindow, que al no haber ninguna la
// crea de nuevo.
//
// SE LIBERAN TAMBIEN SUS CONTROLES. Si no, los botones de la ventana que se
// acaba de cerrar se quedarian apuntando a un hueco de ventana que el
// gestor puede volver a repartir, y aparecerian encima de la ventana de
// OTRO programa. No daria ningun error: solo botones ajenos en una ventana
// que no es suya.
//
// Devuelve 1 si habia ventana que cerrar, 0 si no.
int32_t task_close_window(void) {
    if (current_ctx == KERNEL_CTX || current_ctx < 0 || current_ctx >= MAX_TASKS) return 0;
    task_t *t = &tasks[current_ctx];
    if (t->window_idx < 0) return 0;
    gadgets_free_window(t->window_idx);
    wm_destroy_window(t->window_idx);
    // El -1 es lo que hace que task_ensure_window cree una nueva la
    // proxima vez. Sin el, el programa seguiria dibujando en un hueco
    // que ya no es suyo.
    t->window_idx = -1;
    return 1;
}

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
