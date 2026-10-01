// smp.c -- despertar los nucleos secundarios (fase 3 del SMP).
//
// En esta fase los nucleos 1-3 se despiertan, montan su propio entorno
// y se presentan por la UART, pero NO reciben trabajo: se quedan
// dormidos en wfe. Sirve para comprobar, sin riesgo para el resto del
// sistema, que el arranque de un secundario funciona de principio a
// fin. Repartir tareas es la fase 5.
//
// ---- Que tiene que hacer un nucleo al despertar ----
//
// Cada nucleo tiene sus PROPIOS registros de sistema, aunque compartan
// la memoria. Al despertar, un secundario no hereda nada del nucleo 0:
//   - pila propia (pila_secundarios, abajo)
//   - su MMU: MAIR, TCR, TTBR0 y SCTLR son registros de cada nucleo.
//     Las TABLAS son las mismas para todos (las construyo el nucleo 0);
//     cada uno solo tiene que apuntar a ellas.
//   - su tabla de vectores (VBAR_EL1), para atrapar sus excepciones
//   - acceso a la coma flotante (CPACR_EL1), en el ensamblador
//
// ---- Como se despierta un nucleo ----
//
//   QEMU    -- PSCI: una llamada al firmware (hvc) que enciende el nucleo
//              y lo hace empezar en la direccion que le digamos.
//   Pi 4    -- spin tables: los nucleos esperan en el firmware leyendo
//              una direccion fija; se escribe ahi a donde deben saltar
//              (fase 3b, ver despertar_nucleo mas abajo).
#include <stdint.h>
#include "smp.h"
#include "cpu.h"
#include "mmu.h"
#include "uart.h"
#include "tasks.h"
#include "bkl.h"
#include "timer.h"   // timer_flujo_eventos(): el despertador de cada nucleo

extern void exceptions_init(void);

// Numero decimal por la UART. El kernel no tiene una version comun:
// cada archivo que la necesita lleva su copia local (disk.c, input.c,
// loader.c...), y esta es la de smp.c.
static void uart_put_dec(uint64_t v) {
    char buf[21];
    int n = 0;
    if (v == 0) { uart_puts("0"); return; }
    while (v > 0 && n < 20) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    char una[2] = { 0, 0 };
    while (n > 0) { una[0] = buf[--n]; uart_puts(una); }
}
extern void secundario_entrada(void);   // boot.s (QEMU) o start_pi4.S (Pi 4)

// Una pila por nucleo. El ensamblador de entrada calcula la de cada uno
// como pila_secundarios + (nucleo+1)*SMP_PILA, es decir, el FINAL de su
// trozo (la pila crece hacia abajo). SMP_PILA tiene que coincidir con el
// desplazamiento que usa boot.s (lsl #14).
__attribute__((aligned(16)))
uint8_t pila_secundarios[MAX_CPUS][SMP_PILA];

// Cada secundario pone aqui un 1 cuando ha terminado de arrancar.
// volatile: el nucleo 0 la lee en bucle mientras otro nucleo la escribe.
static volatile uint32_t nucleo_vivo[MAX_CPUS];

// Cuantos nucleos arrancaron de verdad, contando el 0. Lo consulta el
// planificador: el nucleo 0 solo deja de ejecutar programas si hay
// secundarios que lo hagan.
static volatile uint32_t nucleos_en_marcha = 1;
uint32_t smp_nucleos_en_marcha(void) { return nucleos_en_marcha; }

// Primer codigo en C de un nucleo secundario. Llega aqui desde
// secundario_entrada (boot.s) con la pila ya montada y en EL1, con la
// MMU todavia APAGADA.
void secundario_main(uint32_t nucleo) {
    // Primero la MMU: hasta que no este encendida, este nucleo lee la
    // memoria sin cache, y podria no ver lo que el nucleo 0 tiene en la
    // suya. Por eso nucleo_vivo se escribe DESPUES.
    mmu_activar_en_este_nucleo();
    exceptions_init();

    uart_puts("smp: nucleo ");
    uart_put_dec(nucleo);
    uart_puts(" en marcha\n");

    nucleo_vivo[nucleo] = 1;
    __asm__ volatile("dsb ish" ::: "memory");   // que el nucleo 0 lo vea ya

    // ---- Fase 5b: a trabajar ----
    //
    // Flujo de eventos del temporizador: el propio contador del sistema
    // genera un "evento" periodico que despierta el wfe de ESTE nucleo.
    // Es lo que despierta a un secundario en reposo para que mire si hay
    // tareas nuevas.
    //
    // Por que no un sev desde el nucleo 0: sev despierta a TODOS los
    // nucleos, incluido el que lo lanza, y el nucleo 0 dejaria de dormir
    // en su propio wfe. El flujo de eventos es de cada nucleo y no molesta
    // a nadie mas.
    //
    // Los bits exactos estan en timer_flujo_eventos() (src/timer.c), que es
    // la misma llamada que hace el nucleo 0 en timer_init: el registro
    // CNTKCTL_EL1 es de cada nucleo, pero la receta es una sola.
    timer_flujo_eventos();

    // Entrar en el planificador, como hace el nucleo 0 con su bucle
    // principal: con el candado cogido (este nucleo va a ejecutar kernel)
    // y en SU contexto de reposo. task_yield hara el resto: dormir cuando
    // no haya trabajo y saltar a una tarea cuando la haya.
    //
    // bkl_tomar puede esperar un rato: el nucleo 0 tiene el candado
    // durante todo su arranque y no lo suelta hasta su primer wfe o su
    // primer salto a una tarea.
    bkl_tomar();
    tasks_entrar_nucleo(nucleo);
    for (;;) task_yield();
}

// ---- Cache: vaciar un rango de memoria hasta la RAM ----
//
// "dc civac" limpia (escribe a RAM si esta sucia) e invalida (tira)
// cada linea de cache del rango. Hace falta en dos sitios del
// despertar, siempre por lo mismo: un nucleo que acaba de arrancar
// tiene la cache APAGADA y lee y escribe la RAM directamente, mientras
// que el nucleo 0 tiene la suya encendida y puede tener copias de esa
// misma memoria que la RAM todavia no ha visto.
//
// El tamaño de linea se lee de CTR_EL0 (DminLine: log2 de palabras de
// 4 bytes) en vez de suponerlo, para que valga en las dos CPU.
static void vaciar_cache(const void *ptr, uint64_t bytes) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t linea = 4UL << ((ctr >> 16) & 0xF);
    uint64_t p = (uint64_t)ptr & ~(linea - 1);
    uint64_t fin = (uint64_t)ptr + bytes;
    for (; p < fin; p += linea) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

#ifdef NEMO_QEMU
// PSCI CPU_ON (version de 64 bits, funcion 0xC4000003):
//   x1 = el nucleo a encender (su MPIDR; en la maquina virt, Aff0 = numero)
//   x2 = direccion donde empezara a ejecutar
//   x3 = un valor que le llega en x0 (no lo usamos)
// Devuelve 0 si fue bien. QEMU implementa PSCI el mismo cuando arranca
// el kernel directamente, sin firmware, y lo atiende por hvc porque el
// kernel corre en EL1 (el Makefile no activa la virtualizacion).
static int64_t despertar_nucleo(uint32_t n) {
    register uint64_t x0 __asm__("x0") = 0xC4000003UL;
    register uint64_t x1 __asm__("x1") = n;
    register uint64_t x2 __asm__("x2") = (uint64_t)secundario_entrada;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int64_t)x0;
}
#else
// Raspberry Pi 4 -- "spin tables".
//
// El firmware de la Pi (el armstub que carga en la direccion 0) deja los
// nucleos 1-3 esperando en un bucle con wfe, cada uno vigilando SU
// direccion: 0xE0 el nucleo 1, 0xE8 el 2, 0xF0 el 3 (0xD8 seria el 0).
// Cuando lee ahi algo distinto de cero, salta a esa direccion, en EL2 y
// con la MMU apagada.
//
// LA TRAMPA: ese nucleo lee con la cache APAGADA, directamente de RAM.
// Nosotros escribimos con la cache encendida, asi que la direccion se
// queda en NUESTRA cache y la RAM sigue a cero: el nucleo esperaria
// para siempre. Por eso hay que vaciar esa linea a RAM antes del sev.
// (La memoria baja esta mapeada como normal y cacheable en nuestra MMU:
// ver build_tables en mmu_pi4.c.)
static int64_t despertar_nucleo(uint32_t n) {
    static const uint64_t direccion_spin[MAX_CPUS] = { 0xD8, 0xE0, 0xE8, 0xF0 };
    volatile uint64_t *spin = (volatile uint64_t *)(uintptr_t)direccion_spin[n];
    *spin = (uint64_t)secundario_entrada;
    vaciar_cache((const void *)spin, sizeof(uint64_t));
    __asm__ volatile("sev" ::: "memory");   // despertar a los que esperan en wfe
    return 0;   // las spin tables no devuelven nada: se sabe por nucleo_vivo
}
#endif

// Contador del sistema, independiente de las interrupciones: sirve para
// poner un limite de tiempo a la espera aunque el reloj del kernel no
// este corriendo.
static uint64_t contador(void)   { uint64_t v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
static uint64_t frecuencia(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }

void smp_arrancar(void) {
    uint32_t vivos = 1;   // el nucleo 0, que es quien ejecuta esto

    // De uno en uno: se despierta un nucleo y se espera a que se
    // presente antes de despertar el siguiente. Asi dos nucleos nunca
    // escriben a la vez por la UART y los mensajes salen limpios.
    for (uint32_t n = 1; n < MAX_CPUS; n++) {
        // Vaciar de NUESTRA cache la pila de ese nucleo antes de
        // despertarlo. Al arrancar, el nucleo 0 puso el .bss a cero con
        // la cache encendida: esas lineas pueden seguir sucias aqui. El
        // secundario escribe su pila directamente en RAM (cache
        // apagada), y si luego esta cache devolviera la copia vieja a
        // RAM, le machacaria la pila; y en cuanto el secundario
        // encienda su cache, podria leer nuestra copia vieja en vez de
        // lo que el mismo escribio -- por ejemplo, una direccion de
        // retorno a cero. QEMU no emula caches y no lo necesita, pero
        // en la Pi real puede colgar el arranque, y es lo correcto en
        // las dos.
        vaciar_cache(pila_secundarios[n], SMP_PILA);

        int64_t r = despertar_nucleo(n);
        if (r != 0) {
            uart_puts("smp: no se pudo encender el nucleo ");
            uart_put_dec(n);
            uart_puts(" (codigo ");
            if (r < 0) { uart_puts("-"); uart_put_dec((uint64_t)(-r)); }
            else uart_put_dec((uint64_t)r);
            uart_puts(") -- en QEMU, se lanzo con -smp 4?\n");
            continue;
        }
        // Esperar como mucho un segundo a que se presente.
        uint64_t limite = contador() + frecuencia();
        while (!nucleo_vivo[n] && contador() < limite) { }
        if (nucleo_vivo[n]) vivos++;
        else { uart_puts("smp: el nucleo "); uart_put_dec(n); uart_puts(" no respondio\n"); }
    }
    uart_puts("smp: ");
    uart_put_dec(vivos);
    uart_puts(" de ");
    uart_put_dec(MAX_CPUS);
    uart_puts(" nucleos en marcha (el 0 para el sistema, el resto para los programas)\n");
    nucleos_en_marcha = vivos;
}
