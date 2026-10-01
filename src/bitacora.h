// bitacora.h — Nemo OS
//
// Guarda TODO lo que se manda al UART y lo vuelca a /NEMO.LOG en la
// particion FAT de arranque.
//
// POR QUE EXISTE: el diagnostico del sistema --la
// enumeracion del USB, la negociacion de la red, las excepciones-- ya
// se escribia entero por el UART. Pero eso solo se lee con un cable
// serie enganchado a los pines GPIO, y en cuanto la Pi se mete dentro
// de una caja cerrada esos pines dejan de ser alcanzables. El sistema
// se queda mudo justo cuando mas falta hace que hable.
//
// Escribiendolo tambien en la FAT, se saca la tarjeta, se mete en
// cualquier ordenador y se lee. Para un 1.0 que va a instalar gente
// sin adaptador serie, esa es la diferencia entre "se me colgo" y un
// informe con el que se puede trabajar.
#ifndef BITACORA_H
#define BITACORA_H

#include <stdint.h>
#include <stdbool.h>

// Anade un caracter. La llama uart_putc, asi que no hay que llamarla
// a mano desde ningun sitio.
void bitacora_anadir(char c);

// Escribe lo acumulado en NEMO.LOG de la FAT de arranque. Se llama
// cuando el arranque ya ha terminado de diagnosticar (despues del USB
// y de la red). Devuelve false si no hay FAT o si falla la escritura.
bool bitacora_volcar(void);

// Cuanto se lleva guardado, y si se lleno el bufer.
uint32_t bitacora_largo(void);
bool bitacora_se_lleno(void);

#endif
