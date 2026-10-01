// net.h -- Nemo OS
// ARP + IPv4 + ICMP + UDP + TCP -- ver net.c para el porque y el
// alcance (solo Ethernet, solo IPv4; TCP solo en escucha; UDP solo
// para DHCP de momento).
#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>

// Puerto TCP en el que Nemo OS escucha (Fase 3a: eco; Fase 3b: la
// shell). 2323 en vez del 23 clasico de telnet a proposito -- no es
// telnet (no negocia opciones), y asi no choca con nada del Mac.
#define NET_PUERTO_SHELL 2323

// Llamar una vez, despues de que el enlace este arriba (genet_link_up()
// debe devolver true). Si no hay enlace, net_poll() no hace nada.
void net_init(void);

// Sondea una trama recibida y responde si hace falta (ARP, ping).
// Sin bloqueo -- para llamar en cada vuelta, igual que input_poll().
void net_poll(void);

void net_get_ip(uint8_t out[4]);

// El resto de la configuracion. Vienen a cero mientras no haya red, y
// pueden venir a cero tambien con red: con la IP de reserva (cable
// directo, sin router) no hay ni pasarela ni DNS, y no hacen falta.
void net_get_mascara(uint8_t out[4]);
void net_get_pasarela(uint8_t out[4]);
void net_get_dns(uint8_t out[4]);

// true si la IP la dio un servidor DHCP; false si es la de reserva.
bool net_ip_por_dhcp(void);

// Manda una carga IPv4 a otra maquina. Decide el salto siguiente por
// la mascara (destino directo, o la pasarela), resuelve su MAC por ARP
// y la envia. Devuelve false si todavia NO se ha enviado --tipicamente
// porque falta resolver la MAC, y en ese caso la pregunta ya ha salido
// y el paquete queda esperando-- de modo que quien llama sepa que tiene
// que reintentar. Decir que si sin haber enviado nada es la clase de
// mentira que se paga cara.
bool net_enviar_ipv4(const uint8_t destino[4], uint8_t proto,
                     const uint8_t *carga, uint32_t len);

// Manda un datagrama UDP. Pone la cabecera (con su suma de comprobacion,
// que necesita la IP propia -- de ahi que esto este aqui y no en
// udp_sock.c) y lo entrega. Devuelve false si todavia no salio, por lo
// mismo que net_enviar_ipv4: hay que reintentar.
//
// Admite direcciones de DIFUSION (255.255.255.255, o la de la propia red
// con los bits de host a uno), que van a la MAC de difusion sin preguntar
// por ARP. Es lo que permite que dos maquinas se encuentren sin conocerse.
bool net_enviar_udp(const uint8_t destino[4], uint16_t puerto_destino,
                    uint16_t puerto_origen, const uint8_t *carga, uint32_t len);

bool net_ready(void);

#endif
