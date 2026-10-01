// sound_pi4.c — Nemo OS, Raspberry Pi 4 (BCM2711)
//
// Sonido por el JACK de 3,5 mm. En la Pi 4 ese jack lo mueve el bloque
// PWM1 por los pines GPIO 40 y 41 en funcion alternativa 0 (PWM1_0 =
// izquierda, PWM1_1 = derecha). No son pines de la tira de 40: van por
// dentro de la placa al conector, y nadie mas los usa, asi que este driver
// los toma para el y no pasan por el reparto de pines de gpio.c (que solo
// llega al 27).
//
// El sonido es PWM: el jack lleva un filtro paso bajo, asi que un tren de
// pulsos a 44 kHz cuyo ancho sigue la onda suena como la onda. No es alta
// fidelidad -- son unos 10 bits de amplitud -- pero es todo el audio
// analogico que la placa sabe hacer sin un DAC por fuera.
//
// ---- El reloj es de los dos, no de este ----
//
// PWM0 (los pines 12/13/18/19 de GPIOPwm) y PWM1 (esto) cuelgan del MISMO
// reloj del gestor de relojes. Por eso aqui NO se programa: se pide a
// gpio.c con gpio_pwm_reloj_hz(), que lo deja en los 54 MHz del oscilador,
// y el rango se calcula a partir de lo que devuelva. Si alguien vuelve a
// meter un divisor en un sitio, el otro se desafina: en Linux ese mismo
// fallo se oye como un siseo agudo en el jack en cuanto se activa el PWM
// de un pin.
//
// ---- V1: sin DMA ----
//
// sound_play_blocking() llena la FIFO del PWM a mano y BLOQUEA hasta que
// termina, que es justo lo que promete sound.h y lo que ya hace el driver
// de QEMU. Cuesta un nucleo mientras suena, y un retraso largo del
// planificador puede vaciar la FIFO y dar un chasquido. La version con DMA
// -- sonido de fondo mientras el programa sigue -- es el paso siguiente y
// se apoya en todo esto.

#include <stdint.h>
#include <stdbool.h>
#include "sound.h"
#include "gpio.h"
#include "timer.h"

void uart_puts(const char *s);

// Las pruebas del host apuntan estas bases a memoria falsa.
#ifndef PWM1_BASE
#define PWM1_BASE  0xFE20C800UL   // el SEGUNDO bloque de PWM; el primero esta en 0xFE20C000
#endif
#ifndef GPIO_BASE
#define GPIO_BASE  0xFE200000UL
#endif

static inline volatile uint32_t *pwm(uint32_t off)  { return (volatile uint32_t *)(uintptr_t)(PWM1_BASE + off); }
static inline volatile uint32_t *gpr(uint32_t off)  { return (volatile uint32_t *)(uintptr_t)(GPIO_BASE + off); }

#define PWM_CTL   0x00
#define PWM_STA   0x04
#define PWM_RNG1  0x10
#define PWM_DAT1  0x14
#define PWM_FIF1  0x18   // una sola FIFO: los datos se reparten entre los dos canales por turnos
#define PWM_RNG2  0x20
#define PWM_DAT2  0x24

#define CTL_PWEN1 (1u << 0)
#define CTL_USEF1 (1u << 5)    // el canal se alimenta de la FIFO, no del registro DAT
#define CTL_CLRF  (1u << 6)    // vaciar la FIFO (se autolimpia)
#define CTL_MSEN1 (1u << 7)    // marca-espacio: un pulso por periodo, del ancho del valor
#define CTL_PWEN2 (1u << 8)
#define CTL_USEF2 (1u << 13)
#define CTL_MSEN2 (1u << 15)

// Los dos canales quietos en el valor de DAT, sin FIFO. Es como se deja la
// salida entre sonido y sonido: en el centro, sin tension continua encima
// del altavoz y sin depender de RPTL.
#define CTL_QUIETO (CTL_PWEN1 | CTL_MSEN1 | CTL_PWEN2 | CTL_MSEN2)
// Y los dos leyendo de la FIFO, que es como se reproduce.
#define CTL_SONANDO (CTL_QUIETO | CTL_USEF1 | CTL_USEF2)

#define STA_FULL1 (1u << 0)
#define STA_EMPT1 (1u << 1)
#define STA_WERR1 (1u << 2)    // se escribio con la FIFO llena
#define STA_RERR1 (1u << 3)
#define STA_GAPO  (0xFu << 4)  // a un canal se le acabaron los datos: eso es un chasquido
#define STA_BERR  (1u << 8)

#define GPFSEL4   0x10         // pines 40..49, 3 bits cada uno

#define SONIDO_HZ 44100u       // lo que promete sound.h

static bool listo = false;
static uint32_t rango = 0;     // pasos de amplitud = reloj / 44100

// Una muestra de 16 bits con signo al ancho de pulso que le toca.
// Se deja fuera de 'static' para que la prueba del host la mida sola.
uint32_t sound_pwm_muestra(int16_t s, uint32_t r) {
    uint32_t sin_signo = (uint32_t)((int32_t)s + 32768);   // 0..65535, el silencio en 32768
    return (sin_signo * r) >> 16;                          // 0..r-1, el silencio en r/2
}

// La prueba del host se queda con lo que va a la FIFO; en la placa, va a la FIFO.
#ifdef NEMO_PRUEBA_SONIDO
void sonido_prueba_fifo(uint32_t v);
#define ESCRIBIR_FIFO(v) sonido_prueba_fifo(v)
#else
#define ESCRIBIR_FIFO(v) (*pwm(PWM_FIF1) = (v))
#endif

static void decir_numero(const char *antes, uint64_t v, const char *despues) {
    char n[24]; int k = 0;
    if (v == 0) n[k++] = '0';
    while (v) { n[k++] = (char)('0' + (v % 10)); v /= 10; }
    char t[24]; int j = 0;
    while (k) t[j++] = n[--k];
    t[j] = 0;
    uart_puts(antes); uart_puts(t); uart_puts(despues);
}

static void pines_al_jack(void) {
    // GPIO 40 y 41 a ALT0. Estan los dos en GPFSEL4: el 40 en los bits 0-2
    // y el 41 en los 3-5.
    uint32_t v = *gpr(GPFSEL4);
    v &= ~((7u << 0) | (7u << 3));
    v |= ((uint32_t)GPIO_ALT0 << 0) | ((uint32_t)GPIO_ALT0 << 3);
    *gpr(GPFSEL4) = v;
}

bool sound_init(void) {
    uint32_t reloj = gpio_pwm_reloj_hz();
    if (reloj == 0) {
        uart_puts("sonido: no hay reloj de PWM; el jack se queda mudo\n");
        return false;
    }
    rango = reloj / SONIDO_HZ;        // 54 MHz / 44100 = 1224 pasos, 44.118 Hz de verdad
    if (rango < 2) {
        uart_puts("sonido: el reloj del PWM es demasiado lento para 44 kHz\n");
        return false;
    }

    // Los pines se conectan al jack AL FINAL, no al principio (el mismo
    // motivo que en sound_shutdown, al reves). Configurando primero, el pin
    // pasaria un rato en ALT0 con el PWM parado, o sea clavado a cero, y al
    // encenderlo saltaria de golpe al 50%: otro escalon, otro golpe. Dejando
    // el bloque ya en marcha y en silencio, cuando el pin se conecta lo que
    // sale por el es ya el 50% de siempre y no hay salto.
    *pwm(PWM_CTL) = 0;                                    // parado mientras se configura
    *pwm(PWM_STA) = STA_WERR1 | STA_RERR1 | STA_GAPO | STA_BERR;   // los errores viejos, fuera
    *pwm(PWM_RNG1) = rango;
    *pwm(PWM_RNG2) = rango;
    *pwm(PWM_DAT1) = rango / 2;                           // silencio, por si alguien mira
    *pwm(PWM_DAT2) = rango / 2;
    *pwm(PWM_CTL) = CTL_CLRF;                             // FIFO vacia antes de empezar
    *pwm(PWM_CTL) = CTL_QUIETO;                           // en silencio, esperando

    pines_al_jack();

    listo = true;
    uart_puts("sonido: jack de 3,5 mm por PWM1 (GPIO 40/41)\n");
    decir_numero("  reloj del PWM (Hz): ", reloj, "\n");
    decir_numero("  rango (pasos):      ", rango, "\n");
    decir_numero("  frecuencia real:    ", reloj / rango, " Hz\n");
    return true;
}

bool sound_available(void) { return listo; }

// ---- El tope es de TIEMPO, no de vueltas ----
//
// La primera version contaba vueltas de espera por muestra. Con el reloj mal
// puesto eso no acota nada: cada muestra se gastaba su cuenta entera y un
// sonido de medio segundo dejaba el sistema colgado, porque este driver
// corre con el candado grande del kernel cogido. Ahora se mira el reloj de
// microsegundos de verdad: si la reproduccion pasa del triple de lo que el
// sonido dura, algo va mal y se corta. Un reloj equivocado cuesta un aviso
// por la UART, no un apagon.
static uint64_t limite_us = 0;
static bool se_acabo_el_tiempo(void) { return timer_micros() > limite_us; }

// Mete un valor en la FIFO esperando a que haya hueco.
static bool meter(uint32_t v) {
    while (*pwm(PWM_STA) & STA_FULL1) {
        if (se_acabo_el_tiempo()) return false;
    }
    ESCRIBIR_FIFO(v);
    return true;
}

void sound_play_blocking(const int16_t *samples, uint32_t frame_count) {
    if (!listo || !samples || frame_count == 0) return;

    uint32_t centro = rango / 2;
    uint64_t empezo = timer_micros();
    uint64_t dura_us = ((uint64_t)frame_count * 1000000u) / SONIDO_HZ;
    limite_us = empezo + dura_us * 3 + 100000u;      // el triple, y 100 ms de gracia

    *pwm(PWM_CTL) = CTL_CLRF;                        // la FIFO, limpia
    *pwm(PWM_CTL) = CTL_SONANDO;

    bool cortado = false;
    for (uint32_t i = 0; i < frame_count; i++) {
        // Una sola FIFO alimenta los dos canales por turnos: primero la
        // izquierda, luego la derecha. Si se escribe una sola de las dos,
        // los canales se cruzan a partir de ahi -- por eso las dos van
        // juntas y se corta el bucle entero si una falla.
        if (!meter(sound_pwm_muestra(samples[i * 2 + 0], rango))) { cortado = true; break; }
        if (!meter(sound_pwm_muestra(samples[i * 2 + 1], rango))) { cortado = true; break; }
    }

    // Terminar en silencio: cuatro FOTOGRAMAS, no cuatro valores, que la FIFO
    // reparte por turnos y un numero impar cambiaria izquierda por derecha.
    if (!cortado) {
        for (int i = 0; i < 4; i++) {
            if (!meter(centro) || !meter(centro)) { cortado = true; break; }
        }
    }

    // Esperar a que salga lo que queda: "bloquea hasta que termina de sonar"
    // seria mentira por los ultimos milisegundos si se devolviera el control
    // con la FIFO aun llena.
    while (!(*pwm(PWM_STA) & STA_EMPT1)) {
        if (se_acabo_el_tiempo()) { cortado = true; break; }
    }

    // Y la salida quieta en el centro: los canales dejan de leer de la FIFO y
    // se quedan en lo que diga DAT. Sin esto, una FIFO vacia manda la salida
    // a cero, que en una onda centrada es el extremo de abajo -- un chasquido
    // al acabar y tension continua en el altavoz mientras no suena nada.
    *pwm(PWM_DAT1) = centro;
    *pwm(PWM_DAT2) = centro;
    *pwm(PWM_CTL) = CTL_QUIETO;

    // El parte, SIEMPRE, no solo cuando corta. "Ha sonado" y "ha sonado al
    // ritmo correcto" no son lo mismo: un sonido tres veces lento tambien
    // suena, solo que mal, y por la UART se ve en un vistazo lo que de oido
    // cuesta decidir. El numero que importa es el de fotogramas por segundo:
    // si no ronda los 44100, el PWM no va al ritmo que este driver cree.
    uint64_t tardo = timer_micros() - empezo;
    if (tardo == 0) tardo = 1;
    decir_numero("sonido: ", frame_count, " fotogramas");
    decir_numero(" en ", tardo, " us");
    decir_numero(" (esperado ", dura_us, " us),");
    decir_numero(" ", ((uint64_t)frame_count * 1000000u) / tardo, " por segundo");
    uart_puts(cortado ? " -- CORTADO POR TIEMPO\n" : "\n");
}

// ---- Apagar sin chasquido ----
//
// BUG REAL CORREGIDO (se oia como un golpe seco en los auriculares al apagar
// o reiniciar). La primera version hacia esto:
//
//     DAT = rango/2  ->  CTL = 0  ->  pines a entrada
//
// y el comentario decia que ese orden era el bueno. Es el malo, y la razon
// esta en el circuito del jack. En silencio el PWM esta al 50%, asi que la
// tension MEDIA en el pin es la mitad de la de alimentacion. Poner CTL = 0
// mientras el pin sigue en ALT0 no lo deja "en silencio": lo deja CLAVADO A
// CERO. La media salta de la mitad a cero de golpe, y como la salida del jack
// esta acoplada por condensador, ese escalon pasa entero al auricular: un
// golpe a todo volumen.
//
// El arreglo no es cambiar de sitio el escalon, es QUITARLO: se baja el ancho
// de pulso desde el centro hasta cero poco a poco (64 ms), que es una rampa de
// unos 15 Hz -- por debajo de lo que se oye --, y solo cuando el pin ya esta
// de media a cero se suelta y se para el bloque. Asi ningun paso de los tres
// mueve la tension de golpe.
//
// Los pines se sueltan ANTES de parar el PWM, no despues: con el pin en
// entrada (alta impedancia) ya no hay nada que empujar, asi que CTL = 0 no
// puede producir ningun salto. Al reves quedaba el escalon de antes.
#define APAGADO_PASOS  128u
#define APAGADO_US     500u     // 128 x 500 us = 64 ms de rampa

void sound_shutdown(void) {
    if (!listo) return;
    listo = false;

    // Los dos canales tirando de DAT, no de la FIFO: si quedaba algo dentro de
    // la FIFO sonaria por encima de la rampa.
    *pwm(PWM_CTL) = CTL_QUIETO | CTL_CLRF;
    *pwm(PWM_CTL) = CTL_QUIETO;

    uint32_t centro = rango / 2;
    for (uint32_t i = APAGADO_PASOS; i > 0; i--) {
        uint32_t d = (centro * (i - 1)) / APAGADO_PASOS;   // del centro a cero
        *pwm(PWM_DAT1) = d;
        *pwm(PWM_DAT2) = d;
        uint64_t hasta = timer_micros() + APAGADO_US;
        while (timer_micros() < hasta) { }
    }

    uint32_t v = *gpr(GPFSEL4);      // GPIO 40 y 41 de vuelta a entrada
    v &= ~((7u << 0) | (7u << 3));
    *gpr(GPFSEL4) = v;

    *pwm(PWM_CTL) = 0;               // ya no empuja nada: los dos canales, parados

    uart_puts("sonido: salida de audio apagada sin chasquido\n");
}
