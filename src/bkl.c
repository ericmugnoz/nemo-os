// bkl.c -- el candado grande del kernel (Big Kernel Lock, fase 4 SMP).
//
// Un solo candado para todo el kernel. El codigo de los PROGRAMAS puede
// correr en los cuatro nucleos a la vez; el codigo del KERNEL, en uno
// cada vez. Asi el kernel sigue siendo correcto sin revisar una a una
// sus ~150 variables globales, que se escribieron pensando en un solo
// nucleo. Es lo que hicieron Linux y FreeBSD en su primer SMP.
//
// ---- La regla ----
//
// Un nucleo tiene el candado SIEMPRE que ejecuta codigo del kernel:
//   - lo coge al entrar desde un programa (exceptions.s, entradas EL0)
//   - lo suelta justo antes de volver a un programa (exceptions.s y
//     enter_el0 en tasks.c)
//   - el nucleo 0 lo coge una vez al arrancar (kernel_main), porque
//     desde ese momento ya esta ejecutando kernel
//
// ---- El cambio de tarea ----
//
// task_yield salta de una tarea a otra ESTANDO DENTRO del kernel. Con
// esta regla no hay que hacer nada especial: el nucleo sigue teniendo
// el candado durante el salto, y la tarea a la que salta tambien estaba
// dentro del kernel -- lo soltara cuando vuelva a su programa. El
// candado es del NUCLEO, no de la tarea. Por eso se guarda que nucleo
// lo tiene, y no que tarea.
//
// ---- Vigilancia ----
//
// Un candado de espera activa no sabe quien lo tiene: si un nucleo lo
// pide dos veces, se queda esperandose a si mismo para siempre, y el
// sistema se cuelga sin decir nada. Aqui se guarda el dueño para
// convertir esos errores en un mensaje por la UART:
//   - coger un candado que ESTE MISMO nucleo ya tiene
//   - soltar un candado que este nucleo NO tiene
// Los dos serian fallos de logica en las entradas y salidas del kernel.
// Se avisa un numero limitado de veces para no inundar la UART si el
// fallo se repite en cada llamada al sistema.
#include <stdint.h>
#include "bkl.h"
#include "medir.h"
#include "spinlock.h"
#include "cpu.h"
#include "uart.h"

static spinlock_t bkl = SPINLOCK_INIT;

// Nucleo que tiene el candado, o -1 si esta libre. Solo lo escribe el
// dueño (al cogerlo y al soltarlo), asi que un nucleo puede leerlo sin
// candado para preguntarse "¿lo tengo YO?": si la respuesta es si, nadie
// mas puede estar cambiandolo.
static volatile int32_t bkl_duenio = -1;

// Cuando lo cogio cada nucleo, para medir cuanto lo retiene (medir.c).
static uint64_t bkl_desde[MAX_CPUS];

static uint32_t avisos = 0;
#define MAX_AVISOS 5

static void avisar(const char *que) {
    if (avisos >= MAX_AVISOS) return;
    avisos++;
    char n[2] = { (char)('0' + cpu_id()), 0 };
    uart_puts("BKL: el nucleo ");
    uart_puts(n);
    uart_puts(que);
    if (avisos == MAX_AVISOS) uart_puts("BKL: (no se avisara mas)\n");
}

// ---- Interrupciones enmascaradas MIENTRAS se coge o se suelta ----
//
// Desde la fase 5a el nucleo 0 suelta el candado mientras duerme (el wfe
// de task_yield), y en esa ventana una interrupcion entra por el
// manejador del kernel, que coge el candado si el nucleo no lo tiene
// (bkl_tomar_si_falta, abajo). Si esa interrupcion llegara JUSTO a mitad
// de coger o soltar -- con el spinlock ya cogido pero bkl_duenio aun sin
// escribir, o al reves -- el manejador creeria que el nucleo no lo tiene,
// pediria el spinlock y se esperaria a si mismo para siempre.
//
// Por eso las dos operaciones enmascaran las interrupciones mientras
// trabajan y dejan la mascara como estaba. Donde ya estaban enmascaradas
// (las entradas EL0, enter_el0) no cambia nada.
static inline uint64_t irq_enmascarar(void) {
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");
    return daif;
}
static inline void irq_restaurar(uint64_t daif) {
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

void bkl_tomar(void) {
    uint64_t daif = irq_enmascarar();
    int32_t yo = (int32_t)cpu_id();
    if (bkl_duenio == yo) {
        // Cogerlo otra vez colgaria este nucleo para siempre.
        avisar(" pide el candado que YA tiene -- se ignora para no colgarse\n");
        irq_restaurar(daif);
        return;
    }
    uint64_t t0 = medir_ahora();
    spin_lock(&bkl);
    medir_espera_candado((uint32_t)yo, medir_ahora() - t0);
    bkl_desde[yo] = medir_ahora();
    bkl_duenio = yo;
    irq_restaurar(daif);
}

void bkl_soltar(void) {
    uint64_t daif = irq_enmascarar();
    int32_t yo = (int32_t)cpu_id();
    if (bkl_duenio != yo) {
        avisar(" suelta un candado que NO tiene -- se ignora\n");
        irq_restaurar(daif);
        return;
    }
    medir_retencion_candado((uint32_t)yo, medir_ahora() - bkl_desde[yo]);
    bkl_duenio = -1;
    spin_unlock(&bkl);
    irq_restaurar(daif);
}

// Para el manejador de interrupciones del KERNEL (irq_stub, EL1).
//
// Normalmente, si el kernel recibe una interrupcion es porque estaba
// trabajando, y ya tiene el candado: no hay que hacer nada. La excepcion
// es la ventana en que el nucleo 0 duerme SIN el (wfe de task_yield):
// ahi el manejador correria kernel sin candado mientras otro nucleo
// podria estar dentro. Esta funcion lo coge solo si falta, y devuelve 1
// si lo cogio, para que el manejador sepa si tiene que soltarlo al salir.
//
// Se llama con las interrupciones ya enmascaradas (estamos atendiendo
// una) y no avisa si el nucleo ya lo tiene: eso aqui es lo normal.
int bkl_tomar_si_falta(void) {
    int32_t yo = (int32_t)cpu_id();
    if (bkl_duenio == yo) return 0;
    uint64_t t0 = medir_ahora();
    spin_lock(&bkl);
    medir_espera_candado((uint32_t)yo, medir_ahora() - t0);
    bkl_desde[yo] = medir_ahora();
    bkl_duenio = yo;
    return 1;
}
