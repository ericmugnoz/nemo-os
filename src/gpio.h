// gpio.h -- Nemo OS: los pines GPIO del conector de la Raspberry Pi 4.
//
// Pines del 2 al 27 (los del conector de 40 patillas). El 14 y el 15
// estan RESERVADOS: son la UART de la terminal, y reconfigurarlos la
// dejaria muda. Cada pin tiene DUEÑO (tarea + generacion, como los
// recursos del kernel en syscall.c): el primer programa que lo
// configura se lo queda, y ningun otro puede cambiarlo mientras aquel
// viva. Cuando un programa termina, sus pines vuelven a entrada sin
// resistencia (gpio_liberar_tarea): asi no se queda un LED encendido ni
// un pin empujando corriente cuando se cierra lo que lo controlaba.
//
// En QEMU no hay pines: se SIMULAN (se recuerdan modos y valores, y cada
// cambio se anuncia por la terminal), para poder probar un programa
// antes de conectar nada.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define GPIO_PRIMERO 2
#define GPIO_ULTIMO  27

#define GPIO_ENTRADA         0   // entrada, sin resistencia
#define GPIO_SALIDA          1
#define GPIO_ENTRADA_ARRIBA  2   // entrada con resistencia interna a positivo (lee 1 al aire)
#define GPIO_ENTRADA_ABAJO   3   // entrada con resistencia interna a masa (lee 0 al aire)

// Resultados negativos (los mismos para las tres operaciones)
#define GPIO_ERR_PIN       (-1)  // fuera del conector (2..27) o modo invalido
#define GPIO_ERR_RESERVADO (-2)  // 14/15: la terminal
#define GPIO_ERR_OCUPADO   (-3)  // es de otro programa, que sigue vivo
#define GPIO_ERR_MODO      (-4)  // escribir en un pin que no es salida
#define GPIO_ERR_CANAL     (-5)  // PWM: el otro pin de ese canal ya lo usa (12 y 18 comparten; 13 y 19 tambien)

void gpio_init(void);
int32_t gpio_modo(int32_t pin, int32_t modo);       // 0, o error
int32_t gpio_escribir(int32_t pin, int32_t valor);  // 0, o error
int32_t gpio_leer(int32_t pin);                     // 0/1, o error (leer no necesita ser dueño)
void gpio_liberar_tarea(int32_t slot);              // al terminar un programa

// PWM por hardware: solo en los pines 12, 13, 18 y 19, con dos
// canales (12 y 18 comparten el 1; 13 y 19, el 2). Frecuencia de 10 Hz a
// 100 kHz; ciclo en DIEZMILESIMAS (0 = apagado, 10000 = siempre encendido):
// un servo a 50 Hz se posiciona en pasos de 2 microsegundos. Deja el pin
// en modo PWM; gpio_modo() sobre el pin, o que el programa termine, lo para.
int32_t gpio_pwm(int32_t pin, int32_t hz, int32_t diezmilesimas);

// Para otros controladores del kernel (I2C,): el programa actual
// se queda el pin en una funcion alternativa (0-7, la del BCM2711), con las
// mismas reglas de dueño. Al terminar el programa vuelve a entrada.
#define GPIO_ALT0 4
int32_t gpio_reclamar(int32_t pin, uint32_t funcion);

// El reloj del PWM lo comparten los dos bloques de la BCM2711: el PWM0, que
// lleva los pines de arriba, y el PWM1, que lleva el jack de audio. Lo
// enciende si hacia falta y devuelve su frecuencia en Hz, para que el driver
// de sonido calcule su rango a partir de ella en vez de suponerla. Devuelve
// 0 donde no hay PWM de verdad (QEMU).
uint32_t gpio_pwm_reloj_hz(void);
