// arp.h -- Nemo OS
// La tabla ARP: a que direccion de hardware corresponde cada IP.
//
// POR QUE HASTA AHORA NO HACIA FALTA. Toda la pila de red
// se habia construido para CONTESTAR: llega una trama, se le da la
// vuelta a las direcciones y se devuelve. Y para contestar no hay nada
// que resolver -- la MAC del destinatario viene escrita en la trama que
// acaba de llegar, como el remite de un sobre.
//
// Hablar primero es otra cosa. Para mandarle algo a 192.168.1.1 hay que
// averiguar su MAC, y eso se hace preguntando a toda la red y esperando
// la respuesta. Mientras tanto el paquete que querias mandar tiene que
// esperar en algun sitio. De ahi que esto traiga tres cosas que la pila
// no tenia: una tabla, una cola de espera y un reloj.
//
// Alcance a proposito: solo la propia subred. Para salir de ella hay que
// preguntar por la MAC de la PASARELA, no por la del destino final --
// pero esa decision la toma quien encamina (net.c), no esta tabla.
#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include <stdbool.h>

// Ocho entradas. En una red domestica un aparato habla con dos o tres
// maquinas, no con treinta; y cuando se llena se reemplaza la mas vieja,
// que en la practica es la que ya no hace falta.
#define ARP_ENTRADAS 8

// Cuanto vale una entrada antes de volver a preguntar. Dos minutos es
// lo que usa casi todo el mundo: suficiente para no preguntar sin parar,
// y poco para enterarse si una IP cambia de aparato.
#define ARP_CADUCIDAD_MS 120000

// Apunta que 'ip' esta en 'mac'. Se llama con CUALQUIER paquete ARP que
// pase --peticiones incluidas, no solo respuestas-- porque una peticion
// lleva dentro el remite de quien pregunta, y es informacion gratis.
void arp_aprender(const uint8_t ip[4], const uint8_t mac[6], uint64_t ms);

// Busca la MAC de 'ip'. false si no se sabe o si lo que se sabia ya
// caduco. No pregunta: preguntar es cosa de quien llama.
bool arp_buscar(const uint8_t ip[4], uint8_t mac_out[6], uint64_t ms);

// Olvida todo (cambio de red, IP nueva). Lo que valia para la red
// anterior no vale para esta.
void arp_vaciar(void);

// Construye la trama ENTERA (Ethernet + ARP, 42 bytes) que pregunta
// "quien tiene 'buscada'". Va a difusion, porque no se sabe a quien
// preguntar -- que es justo el problema.
uint32_t arp_construir_peticion(uint8_t *out, const uint8_t mi_mac[6],
                                const uint8_t mi_ip[4], const uint8_t buscada[4]);

// Cuantas entradas validas hay ahora mismo (para el registro).
uint32_t arp_cuantas(uint64_t ms);

#endif
