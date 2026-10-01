// spinlock.h -- candado de espera activa, para cuando haya varios
// nucleos ejecutando el kernel.
//
// Un candado protege un trozo de estado compartido: solo un nucleo a la
// vez puede tenerlo cogido. Los demas que lo pidan esperan hasta que se
// suelte.
//
// ---- Por que ldaxr/stxr y no las instrucciones atomicas modernas ----
//
// Ni el Cortex-A72 de la Raspberry Pi 4 ni el Cortex-A53 que emula QEMU
// tienen las instrucciones atomicas de ARMv8.1 (LSE: swp, cas, ldadd).
// Son ARMv8.0, y ahi la unica forma de hacer una operacion atomica es la
// pareja "exclusiva":
//
//   ldaxr  -- lee el valor Y marca la direccion como vigilada
//   stxr   -- escribe SOLO si nadie ha tocado esa direccion desde el
//             ldaxr; si alguien la toco, falla y devuelve 1
//
// Si stxr falla, otro nucleo se ha colado: se vuelve a intentar.
//
// ---- Por que funciona entre nucleos ----
//
// Hace falta que la memoria sea Normal e Inner Shareable, para que los
// nucleos vean la misma linea de cache y el monitor exclusivo sea
// global. Las dos MMU (mmu.c y mmu_pi4.c) ya la marcan asi: comprobado
// al empezar el trabajo de SMP.
//
// ---- Orden de memoria ----
//
// ldaxr es una lectura con semantica "acquire": nada de lo que viene
// despues (lo que el candado protege) puede adelantarse a ella.
// stlr, al soltar, es una escritura "release": nada de lo que hubo
// dentro puede retrasarse hasta despues de ella. Sin esto, la CPU podria
// reordenar los accesos y otro nucleo veria el estado a medias aunque
// el candado "funcionara".
//
// ---- Esperar sin quemar la CPU: wfe ----
//
// Mientras el candado esta cogido, el que espera hace wfe, que duerme
// el nucleo hasta que ocurre un "evento". Y aqui viene lo elegante: el
// ldaxr ha dejado la direccion vigilada, y cuando el dueño la escribe al
// soltar, el hardware genera ese evento solo -- no hace falta un sev.
// El sevl inicial hace que el PRIMER wfe no duerma, para que se lea el
// candado al menos una vez antes de esperar.
//
// Definir SPIN_SIN_WFE quita la espera con wfe y deja un bucle puro.
// Solo sirve para la prueba en el Mac: el kernel lo usa siempre con
// wfe.
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

// ---- Candado POR TURNOS (ticket lock) ----
//
// La primera version era un candado "de quien llegue antes": al soltarlo,
// lo cogia el primero que consiguiera escribir en el. Con varios nucleos
// eso resulto injusto en la practica: el nucleo que suelta sigue
// despierto y con el dato en su cache, y si vuelve a pedirlo enseguida
// casi siempre gana a los que esperan, que antes tienen que despertar.
// Medido en la Pi: el nucleo 0 componia la pantalla, soltaba el candado
// un instante y lo volvia a coger -- lo tuvo el 97% del tiempo, y los
// juegos en los otros nucleos casi nunca entraban.
//
// Ahora funciona como la maquina de numeros de una tienda:
//   - 'siguiente' es el proximo numero que se reparte
//   - 'atendiendo' es el numero al que le toca ahora
// Para coger el candado, un nucleo saca numero (incrementa 'siguiente'
// de forma atomica, quedandose con el valor que habia) y espera a que
// 'atendiendo' llegue a su numero. Para soltarlo, el dueño incrementa
// 'atendiendo'. Quien suelta y vuelve a pedir saca un numero NUEVO, detras
// de los que ya esperaban: no puede colarse.
//
// Los dos contadores comparten una palabra de 32 bits -- 'atendiendo' en
// la mitad baja, 'siguiente' en la alta -- para que sacar numero y mirar
// a quien le toca sea UNA sola lectura atomica. Son de 16 bits y dan la
// vuelta al llegar a 65536; no importa, porque solo se comparan entre si
// y nunca puede haber 65536 nucleos esperando a la vez.
typedef struct {
    volatile uint16_t atendiendo;   // mitad baja (desplazamiento 0)
    volatile uint16_t siguiente;    // mitad alta (desplazamiento 2)
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

static inline void spin_init(spinlock_t *l) { l->atendiendo = 0; l->siguiente = 0; }

#if defined(__aarch64__)

static inline void spin_lock(spinlock_t *l) {
    uint32_t antes, nuevo, fallo, turno, ahora;
    __asm__ volatile(
        // 1) Sacar numero: leer la palabra entera y escribirla con
        //    'siguiente' + 1, en una pareja exclusiva (si otro nucleo la
        //    toca en medio, stxr falla y se repite).
        "1: ldaxr   %w[antes], [%[l]]\n"
        "   add     %w[nuevo], %w[antes], #0x10000\n"
        "   stxr    %w[fallo], %w[nuevo], [%[l]]\n"
        "   cbnz    %w[fallo], 1b\n"
        // Mi numero es la mitad alta de lo que habia; a quien atendian,
        // la mitad baja. Si coinciden, el candado era mio ya.
        "   lsr     %w[turno], %w[antes], #16\n"
        "   and     %w[ahora], %w[antes], #0xffff\n"
        "   cmp     %w[ahora], %w[turno]\n"
        "   b.eq    3f\n"
        // 2) Esperar mi turno. ldaxrh deja vigilada la mitad 'atendiendo':
        //    cuando el dueño la escriba al soltar, el hardware despierta
        //    este wfe. El sevl hace que el primer wfe no duerma, por si el
        //    candado se solto entre la lectura de arriba y esta.
        "   sevl\n"
        "2: wfe\n"
        "   ldaxrh  %w[ahora], [%[l]]\n"
        "   cmp     %w[ahora], %w[turno]\n"
        "   b.ne    2b\n"
        "3:\n"
        : [antes] "=&r"(antes), [nuevo] "=&r"(nuevo), [fallo] "=&r"(fallo),
          [turno] "=&r"(turno), [ahora] "=&r"(ahora)
        : [l] "r"(l)
        : "memory", "cc");
}

// Coger el candado solo si esta libre AHORA ('atendiendo' == 'siguiente':
// nadie lo tiene ni lo espera). Devuelve 1 si lo cogio.
static inline int spin_trylock(spinlock_t *l) {
    uint32_t antes, nuevo, fallo, alto, bajo;
    __asm__ volatile(
        "1: ldaxr   %w[antes], [%[l]]\n"
        "   lsr     %w[alto], %w[antes], #16\n"
        "   and     %w[bajo], %w[antes], #0xffff\n"
        "   cmp     %w[alto], %w[bajo]\n"
        "   b.ne    2f\n"                       // ocupado: no se toca
        "   add     %w[nuevo], %w[antes], #0x10000\n"
        "   stxr    %w[fallo], %w[nuevo], [%[l]]\n"
        "   cbnz    %w[fallo], 1b\n"
        "   mov     %w[fallo], #0\n"
        "   b       3f\n"
        "2: clrex\n"                            // soltar la vigilancia del ldaxr
        "   mov     %w[fallo], #1\n"
        "3:\n"
        : [antes] "=&r"(antes), [nuevo] "=&r"(nuevo), [fallo] "=&r"(fallo),
          [alto] "=&r"(alto), [bajo] "=&r"(bajo)
        : [l] "r"(l)
        : "memory", "cc");
    return fallo == 0;
}

static inline void spin_unlock(spinlock_t *l) {
    // Solo el dueño escribe 'atendiendo', asi que basta leerla, sumar 1 y
    // escribirla con semantica release (stlrh): nada de lo que hubo
    // dentro puede retrasarse hasta despues. Y al escribir la mitad que
    // los que esperan tienen vigilada, se despierta su wfe.
    uint32_t v;
    __asm__ volatile(
        "   ldrh    %w[v], [%[l]]\n"
        "   add     %w[v], %w[v], #1\n"
        "   stlrh   %w[v], [%[l]]\n"
        : [v] "=&r"(v)
        : [l] "r"(l)
        : "memory");
}

#else
// Fuera de ARM64 (solo para pruebas en otra maquina): el mismo algoritmo
// con los atomicos de C11. El kernel NUNCA usa esta rama.
static inline void spin_lock(spinlock_t *l) {
    uint16_t turno = __atomic_fetch_add(&l->siguiente, (uint16_t)1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&l->atendiendo, __ATOMIC_ACQUIRE) != turno) { }
}
static inline int spin_trylock(spinlock_t *l) {
    uint16_t at = __atomic_load_n(&l->atendiendo, __ATOMIC_ACQUIRE);
    uint16_t esperado = at;
    return __atomic_compare_exchange_n(&l->siguiente, &esperado, (uint16_t)(at + 1),
                                       0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static inline void spin_unlock(spinlock_t *l) {
    __atomic_store_n(&l->atendiendo, (uint16_t)(l->atendiendo + 1), __ATOMIC_RELEASE);
}
#endif

#endif // SPINLOCK_H
