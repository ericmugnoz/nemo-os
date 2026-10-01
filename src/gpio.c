// gpio.c -- Nemo OS: los pines GPIO. Ver gpio.h.
#include <stdint.h>
#include <stdbool.h>
#include "gpio.h"
#include "tasks.h"

void uart_puts(const char *s);

// ---- Dueño de cada pin: tarea + generacion (como los recursos del kernel) ----
typedef struct { int32_t tarea; uint32_t gen; int32_t modo; } pin_t;
static pin_t pines[GPIO_ULTIMO + 1];

static bool reservado(int32_t pin) { return pin == 14 || pin == 15; }   // la UART de la terminal

#define MODO_PWM 4                  // interno: el pin lo lleva el PWM por hardware
#define MODO_ALT 5                  // interno: el pin lo lleva otro controlador (I2C...)
static int32_t canal_de(int32_t pin) { return (pin == 12 || pin == 18) ? 0 : (pin == 13 || pin == 19) ? 1 : -1; }
static int32_t pin_en_canal[2] = { -1, -1 };   // que pin usa cada canal de PWM
static bool valido(int32_t pin) { return pin >= GPIO_PRIMERO && pin <= GPIO_ULTIMO; }

// ¿Puede la tarea actual quedarse (o seguir usando) este pin?
static bool es_mio_o_libre(int32_t pin, int32_t tarea, uint32_t gen) {
    pin_t *p = &pines[pin];
    if (p->tarea < 0) return true;                               // libre
    if (p->tarea == tarea && p->gen == gen) return true;         // mio
    return !task_viva(p->tarea, p->gen);                         // su dueño ya no existe
}

#ifdef NEMO_QEMU
// ---- QEMU: pines SIMULADOS ----
// Una salida recuerda su valor; una entrada lee lo que marcaria su
// resistencia interna al aire (arriba 1, abajo o sin resistencia 0).
static int32_t sim_valor[GPIO_ULTIMO + 1];

static void anunciar(int32_t pin, const char *que) {
    char t[4] = { (char)('0' + pin / 10), (char)('0' + pin % 10), 0, 0 };
    uart_puts("gpio (simulado): pin "); uart_puts(pin < 10 ? t + 1 : t); uart_puts(" "); uart_puts(que); uart_puts("\n");
}
static void hw_modo(int32_t pin, int32_t modo) {
    static const char *nombres[] = { "entrada", "salida", "entrada con resistencia a positivo", "entrada con resistencia a masa" };
    sim_valor[pin] = 0;
    anunciar(pin, nombres[modo]);
}
static void hw_escribir(int32_t pin, int32_t valor) {
    sim_valor[pin] = valor;
    anunciar(pin, valor ? "= 1" : "= 0");
}
static int32_t hw_leer(int32_t pin) {
    if (pines[pin].modo == GPIO_SALIDA) return sim_valor[pin];
    if (pines[pin].modo == MODO_PWM) return 0;
    return pines[pin].modo == GPIO_ENTRADA_ARRIBA ? 1 : 0;
}
static void hw_pwm(int32_t pin, int32_t hz, int32_t diez) {
    char t[64]; int k = 0;
    const char *a = "PWM "; while (*a) t[k++] = *a++;
    char n[12]; int m = 0; int32_t v = hz; do { n[m++] = (char)('0' + v % 10); v /= 10; } while (v); while (m) t[k++] = n[--m];
    a = " Hz, "; while (*a) t[k++] = *a++;
    v = diez / 100; do { n[m++] = (char)('0' + v % 10); v /= 10; } while (v); while (m) t[k++] = n[--m];
    t[k++] = ','; t[k++] = (char)('0' + (diez / 10) % 10); t[k++] = (char)('0' + diez % 10);
    a = " %"; while (*a) t[k++] = *a++; t[k] = 0;
    anunciar(pin, t);
}
static void hw_pwm_parar(int32_t pin) { anunciar(pin, "PWM parado"); }
static void hw_alt(int32_t pin, uint32_t f) { (void)f; anunciar(pin, "cedido a un bus (I2C o SPI)"); }
// En QEMU no hay PWM de verdad, asi que tampoco hay reloj que compartir.
uint32_t gpio_pwm_reloj_hz(void) { return 0; }
#else
// ---- Raspberry Pi 4 (BCM2711): los registros de verdad ----
#ifndef GPIO_BASE
#define GPIO_BASE 0xFE200000UL   // (redefinible: las pruebas lo apuntan a una memoria falsa)
#endif
static inline volatile uint32_t *reg(uint32_t off) { return (volatile uint32_t *)(uintptr_t)(GPIO_BASE + off); }
#define GPFSEL0      0x00   // funcion: 3 bits por pin, 10 pines por registro
#define GPSET0       0x1C   // escribir 1 en el bit del pin -> salida a 1
#define GPCLR0       0x28   // escribir 1 en el bit del pin -> salida a 0
#define GPLEV0       0x34   // nivel actual de cada pin
#define PUP_PDN0     0xE4   // resistencias (propio del BCM2711): 2 bits por pin, 16 pines
                            // por registro. 00 ninguna, 01 a positivo, 10 a masa

static void hw_funcion(int32_t pin, uint32_t f) {           // 0 entrada, 1 salida
    uint32_t off = GPFSEL0 + (uint32_t)(pin / 10) * 4, sh = (uint32_t)(pin % 10) * 3;
    uint32_t v = *reg(off);
    v &= ~(7u << sh); v |= (f & 7u) << sh;
    *reg(off) = v;
}
static void hw_resistencia(int32_t pin, uint32_t r) {       // 0 ninguna, 1 arriba, 2 abajo
    uint32_t off = PUP_PDN0 + (uint32_t)(pin / 16) * 4, sh = (uint32_t)(pin % 16) * 2;
    uint32_t v = *reg(off);
    v &= ~(3u << sh); v |= (r & 3u) << sh;
    *reg(off) = v;
}
static void hw_modo(int32_t pin, int32_t modo) {
    if (modo == GPIO_SALIDA) {
        hw_resistencia(pin, 0);
        *reg(GPCLR0) = 1u << pin;          // empezar a 0: nada se enciende al configurarlo
        hw_funcion(pin, 1);
    } else {
        hw_funcion(pin, 0);
        hw_resistencia(pin, modo == GPIO_ENTRADA_ARRIBA ? 1 : modo == GPIO_ENTRADA_ABAJO ? 2 : 0);
    }
}
static void hw_escribir(int32_t pin, int32_t valor) { *reg(valor ? GPSET0 : GPCLR0) = 1u << pin; }

// ---- PWM por hardware (BCM2711) ----
// Reloj del PWM en el gestor de relojes: cada escritura lleva la
// contraseña 0x5A en el byte alto. Se configura una vez, la primera vez
// que se usa el PWM.
//
// EL RELOJ ES UNO PARA LOS DOS BLOQUES. La BCM2711 tiene dos bloques de
// PWM: el PWM0, que lleva estos pines (12, 13, 18, 19), y el PWM1, que
// lleva el jack de audio de 3,5 mm por los pines 40 y 41. Los dos cuelgan
// del MISMO reloj del gestor, asi que el divisor no es de nadie en
// particular: quien lo cambie se lleva por delante al otro. Es un problema
// conocido -- en Linux pasa igual, y se ve como un siseo agudo en el jack
// en cuanto alguien activa el PWM de un pin.
//
// La salida es un reloj RAPIDO y fijo del que los dos calculan SU rango.
// 27 MHz: el oscilador de 54 MHz entre dos.
//
// Por que 27 y no 54: probando en la placa, con el oscilador sin dividir el
// bloque de PWM no seguia el ritmo -- el sonido salia estirado hasta quedar
// en un golpe grave. 27 MHz es la frecuencia con la que el PWM de la Pi 4
// esta probado. Aun asi, un pin gana resolucion frente al MHz de antes: a
// 1 kHz, 27.000 pasos de ciclo en vez de 1.000.
#define PWM_RELOJ_HZ 27000000u   // 54 MHz / 2
#ifndef CM_BASE
#define CM_BASE  0xFE101000UL   // (redefinibles: las pruebas los apuntan a memoria falsa)
#endif
#ifndef PWM_BASE
#define PWM_BASE 0xFE20C000UL
#endif
static inline volatile uint32_t *cm(uint32_t off)  { return (volatile uint32_t *)(uintptr_t)(CM_BASE + off); }
static inline volatile uint32_t *pwm(uint32_t off) { return (volatile uint32_t *)(uintptr_t)(PWM_BASE + off); }
#define CM_PWMCTL   0xA0
#define CM_PWMDIV   0xA4
#define CM_PASSWD   0x5A000000u
#define CM_ENAB     (1u << 4)
#define CM_BUSY     (1u << 7)
#define CM_SRC_OSC  1u           // oscilador: 54 MHz en la Pi 4
#define PWM_CTL     0x00
#define PWM_RNG(c)  ((c) == 0 ? 0x10u : 0x20u)
#define PWM_DAT(c)  ((c) == 0 ? 0x14u : 0x24u)
#define PWM_PWEN(c) ((c) == 0 ? (1u << 0) : (1u << 8))     // canal activo
#define PWM_MSEN(c) ((c) == 0 ? (1u << 7) : (1u << 15))    // modo marca-espacio (el de servos y LEDs)
#define PWM_CANAL(c) (0xFFu << ((c) * 8))                   // todos los bits de control de un canal
static bool reloj_pwm_listo = false;

static void hw_reloj_pwm(void) {
    if (reloj_pwm_listo) return;
    *cm(CM_PWMCTL) = CM_PASSWD | (*cm(CM_PWMCTL) & ~CM_ENAB);           // parar el reloj...
    for (int i = 0; i < 100000 && (*cm(CM_PWMCTL) & CM_BUSY); i++) { }  // ...y esperar a que pare
    *cm(CM_PWMDIV) = CM_PASSWD | (2u << 12);                            // 54 MHz / 2 = 27 MHz
    *cm(CM_PWMCTL) = CM_PASSWD | CM_SRC_OSC;
    *cm(CM_PWMCTL) = CM_PASSWD | CM_SRC_OSC | CM_ENAB;
    for (int i = 0; i < 100000 && !(*cm(CM_PWMCTL) & CM_BUSY); i++) { } // esperar a que arranque
    reloj_pwm_listo = true;
}

// Para el driver de sonido, que cuelga del otro bloque de PWM y necesita
// este mismo reloj en marcha. Lo enciende si hacia falta y dice a que va.
uint32_t gpio_pwm_reloj_hz(void) { hw_reloj_pwm(); return PWM_RELOJ_HZ; }

static void hw_pwm(int32_t pin, int32_t hz, int32_t diez) {
    int32_t c = canal_de(pin);
    hw_reloj_pwm();
    uint32_t rango = PWM_RELOJ_HZ / (uint32_t)hz;                       // tics del periodo
    uint32_t dato = (uint32_t)(((uint64_t)rango * (uint32_t)diez) / 10000u);
    uint32_t ctl = *pwm(PWM_CTL) & ~PWM_CANAL(c);                        // el otro canal, intacto
    *pwm(PWM_CTL) = ctl;                                                 // este, parado mientras se cambia
    *pwm(PWM_RNG(c)) = rango;
    *pwm(PWM_DAT(c)) = dato;
    hw_resistencia(pin, 0);
    hw_funcion(pin, pin < 14 ? 4u : 2u);                                 // ALT0 (12, 13) o ALT5 (18, 19)
    *pwm(PWM_CTL) = ctl | PWM_MSEN(c) | PWM_PWEN(c);
}

static void hw_alt(int32_t pin, uint32_t f) { hw_resistencia(pin, 0); hw_funcion(pin, f); }

static void hw_pwm_parar(int32_t pin) {
    int32_t c = canal_de(pin);
    *pwm(PWM_CTL) = *pwm(PWM_CTL) & ~PWM_CANAL(c);
    hw_funcion(pin, 0);                                                  // de vuelta a entrada
}
static int32_t hw_leer(int32_t pin) { return (int32_t)((*reg(GPLEV0) >> pin) & 1u); }
#endif

void gpio_init(void) {
    // No se toca el hardware al arrancar: los pines quedan como los deja
    // el firmware (entradas). Solo se marcan todos libres.
    for (int32_t i = 0; i <= GPIO_ULTIMO; i++) { pines[i].tarea = -1; pines[i].gen = 0; pines[i].modo = GPIO_ENTRADA; }
    pin_en_canal[0] = pin_en_canal[1] = -1;
}

int32_t gpio_modo(int32_t pin, int32_t modo) {
    if (!valido(pin) || modo < GPIO_ENTRADA || modo > GPIO_ENTRADA_ABAJO) return GPIO_ERR_PIN;
    if (reservado(pin)) return GPIO_ERR_RESERVADO;
    int32_t t = task_get_current_slot();
    if (t < 0 || t >= MAX_TASKS) return GPIO_ERR_OCUPADO;           // solo los programas tienen pines
    uint32_t g = task_generacion(t);
    if (!es_mio_o_libre(pin, t, g)) return GPIO_ERR_OCUPADO;
    if (pines[pin].modo == MODO_PWM) { hw_pwm_parar(pin); pin_en_canal[canal_de(pin)] = -1; }
    pines[pin].tarea = t; pines[pin].gen = g; pines[pin].modo = modo;
    hw_modo(pin, modo);
    return 0;
}

int32_t gpio_reclamar(int32_t pin, uint32_t funcion) {
    if (!valido(pin)) return GPIO_ERR_PIN;
    if (reservado(pin)) return GPIO_ERR_RESERVADO;
    int32_t t = task_get_current_slot();
    if (t < 0 || t >= MAX_TASKS) return GPIO_ERR_OCUPADO;
    uint32_t g = task_generacion(t);
    if (!es_mio_o_libre(pin, t, g)) return GPIO_ERR_OCUPADO;
    if (pines[pin].tarea == t && pines[pin].gen == g && pines[pin].modo == MODO_ALT) return 0;   // ya lo tenia
    if (pines[pin].modo == MODO_PWM) { hw_pwm_parar(pin); pin_en_canal[canal_de(pin)] = -1; }
    pines[pin].tarea = t; pines[pin].gen = g; pines[pin].modo = MODO_ALT;
    hw_alt(pin, funcion);
    return 0;
}

int32_t gpio_pwm(int32_t pin, int32_t hz, int32_t diez) {
    if (!valido(pin) || canal_de(pin) < 0) return GPIO_ERR_PIN;          // solo 12, 13, 18, 19
    if (hz < 10 || hz > 100000 || diez < 0 || diez > 10000) return GPIO_ERR_PIN;
    int32_t t = task_get_current_slot();
    if (t < 0 || t >= MAX_TASKS) return GPIO_ERR_OCUPADO;
    uint32_t g = task_generacion(t);
    if (!es_mio_o_libre(pin, t, g)) return GPIO_ERR_OCUPADO;
    int32_t c = canal_de(pin), otro = pin_en_canal[c];
    if (otro >= 0 && otro != pin && pines[otro].modo == MODO_PWM &&
        pines[otro].tarea >= 0 && task_viva(pines[otro].tarea, pines[otro].gen)) return GPIO_ERR_CANAL;
    pines[pin].tarea = t; pines[pin].gen = g; pines[pin].modo = MODO_PWM;
    pin_en_canal[c] = pin;
    hw_pwm(pin, hz, diez);
    return 0;
}

int32_t gpio_escribir(int32_t pin, int32_t valor) {
    if (!valido(pin)) return GPIO_ERR_PIN;
    if (reservado(pin)) return GPIO_ERR_RESERVADO;
    int32_t t = task_get_current_slot();
    if (t < 0 || t >= MAX_TASKS) return GPIO_ERR_OCUPADO;
    pin_t *p = &pines[pin];
    if (p->tarea != t || p->gen != task_generacion(t)) return GPIO_ERR_OCUPADO;   // hay que configurarlo antes
    if (p->modo != GPIO_SALIDA) return GPIO_ERR_MODO;
    hw_escribir(pin, valor ? 1 : 0);
    return 0;
}

int32_t gpio_leer(int32_t pin) {
    if (!valido(pin)) return GPIO_ERR_PIN;
    if (reservado(pin)) return GPIO_ERR_RESERVADO;
    return hw_leer(pin);                                            // leer no cambia nada: cualquiera
}

void gpio_liberar_tarea(int32_t slot) {
    for (int32_t i = GPIO_PRIMERO; i <= GPIO_ULTIMO; i++) {
        if (pines[i].tarea != slot) continue;
        pines[i].tarea = -1;
        if (pines[i].modo == MODO_PWM) {                            // parar su PWM
            hw_pwm_parar(i);
            pin_en_canal[canal_de(i)] = -1;
            pines[i].modo = GPIO_ENTRADA;
        }
        if (pines[i].modo != GPIO_ENTRADA) {                        // de vuelta a entrada sin resistencia
            pines[i].modo = GPIO_ENTRADA;
            hw_modo(i, GPIO_ENTRADA);
        }
    }
}
