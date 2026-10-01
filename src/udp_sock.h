// udp_sock.h -- Nemo OS
// Sockets UDP para los programas: abrir un puerto, recibir datagramas y
// saber quien mando cada uno.
//
// POR QUE UDP Y NO TCP PARA ESTO. Lo pidio un juego multijugador, y para
// eso UDP no es el pariente pobre de TCP: es el correcto. Un juego manda
// "estoy aqui" cien veces por segundo a tres maquinas a la vez. Con TCP
// serian tres conexiones que mantener, y un paquete perdido pararia los
// siguientes hasta retransmitirlo -- justo lo que no quieres, porque para
// cuando llegara ya estaria viejo. Con UDP, un paquete perdido es una
// posicion perdida y la siguiente llega en 10 ms. Ademas, debajo solo hay
// UN cliente TCP en todo el sistema (ver descarga.h); UDP no tiene esa
// limitacion porque no hay nada que mantener.
//
// ESTO NO TOCA LA RED. Se le entrega lo que llego (net.c) y el guarda; para
// mandar, quien llama usa net_enviar_udp(). Por la misma razon que dhcp.c y
// nmz.c: asi se prueba entero en el Mac, con datagramas inventados, sin
// placa y sin cable. Y lo que hay aqui lo necesita -- es logica que falla
// EN SILENCIO (ver la nota del remitente, abajo).
#ifndef UDP_SOCK_H
#define UDP_SOCK_H

#include <stdint.h>
#include <stdbool.h>

// Cuatro sockets, ocho datagramas en cola cada uno, 1 KB por datagrama:
// 32 KB de memoria fija. Un juego manda paquetes de cuarenta bytes y una
// lectura de sensor cabe en veinte; 1 KB es de sobra y tener tope evita
// que una maquina hablando sin parar se coma el monton.
#define UDPS_MAX_SOCKETS   4
#define UDPS_COLA          8
#define UDPS_MAX_DATAGRAMA 1024

// Deja todos los sockets cerrados. Se llama desde net_init().
void udps_init(void);

// Abre un socket en 'puerto'. Con puerto 0 se elige uno libre por encima
// de 49152, que es lo que quiere quien solo va a mandar y recibir la
// contestacion. Devuelve el identificador (0..3) o negativo:
//   -1 no quedan sockets
//   -2 ese puerto ya esta abierto
//   -3 ese puerto es del sistema (68, el del DHCP)
int32_t udps_abrir(uint16_t puerto);

void udps_cerrar(int32_t h);

// El puerto local de un socket, que es el que hay que poner como origen
// al mandar. 0 si el identificador no vale.
uint16_t udps_puerto(int32_t h);

// Le entrega a los sockets un datagrama recien llegado. Devuelve true si
// algun socket lo cogio. La llama net.c; nadie mas.
bool udps_entregar(uint16_t puerto_destino, const uint8_t origen_ip[4],
                   uint16_t origen_puerto, const uint8_t *carga, uint32_t len);

// Saca el siguiente datagrama de la cola. Devuelve cuantos bytes copio, o
// -1 si no hay nada (o el identificador no vale). Un datagrama VACIO es
// legal en UDP y devuelve 0, que por eso no significa "no hay nada".
//
// Si el datagrama no cabe en 'max', se copia lo que cabe y el resto SE
// PIERDE: el datagrama se consume igual, no se queda medio en la cola.
// Es lo que hace UDP en cualquier sistema, y la alternativa --dejar el
// resto para la siguiente lectura-- convertiria un mensaje en dos y nadie
// sabria donde acaba cada uno.
int32_t udps_recibir(int32_t h, uint8_t *out, uint32_t max);

// Quien mando EL DATAGRAMA QUE ACABA DE SACAR udps_recibir -- no el
// ultimo que llego.
//
// La diferencia es todo el asunto. Con cuatro jugadores mandando a la vez,
// "el ultimo que llego" cambia entre que lees el paquete y miras de quien
// era, y entonces le contestas al que no era. No da error, no se cae
// nada: los jugadores ven cosas raras de vez en cuando y no hay forma de
// averiguar por que. Por eso el remitente se guarda al sacar el datagrama
// y se queda quieto hasta el siguiente.
void udps_origen(int32_t h, uint8_t ip[4], uint16_t *puerto);

// Cuantos datagramas se han tirado por tener la cola llena. Se expone a
// proposito: si un juego va a tirones, esta es la cifra que dice si es
// que el programa no esta vaciando lo bastante rapido o es otra cosa.
uint32_t udps_perdidos(int32_t h);

// Cuantos datagramas esperan en la cola. 0 = no hay nada que leer.
//
// POR QUE HACE FALTA, Y NO ES UN LUJO. Sin esto no hay
// forma de escribir el bucle que vacia la cola:
//
//   * udps_recibir devuelve -1 cuando no hay nada, pero quien llama
//     desde Nemo Basic o Lua recibe una CADENA, y un datagrama vacio
//     --que es legal-- da la misma cadena vacia.
//   * Y udps_origen se queda con el ultimo remitente ENTREGADO: despues
//     de leer uno, ya nunca vuelve a cero, asi que tampoco sirve para
//     saber si quedaba algo.
//
// El primer bucle que escribi con esa API no terminaba nunca: el juego
// se colgaba en cuanto le llegaba el primer mensaje. Con esto, el bucle
// es "mientras haya pendientes, saca uno", que es lo que cualquiera
// escribiria de primeras.
uint32_t udps_pendientes(int32_t h);

#endif
