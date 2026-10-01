// dhcp.h -- Nemo OS
// Cliente DHCP (RFC 2131): pedirle al router la IP, la mascara, la
// pasarela y el DNS en vez de llevarlos escritos a mano.
//
// Por que hace falta: hasta ahora la IP de Nemo OS era fija, del rango
// link-local, elegida para el caso de la Pi conectada DIRECTO a un Mac.
// Eso funciona en la mesa y no funciona en ningun otro sitio:
// enchufada a un router de casa, la placa queda en una subred distinta
// a la de todo lo demas y no habla con nada. DHCP es lo que convierte
// la placa en una maquina mas de la red.
//
// La conversacion son cuatro mensajes y siempre los mismos:
//
//   DISCOVER  (yo, a gritos)   -- "hay alguien que reparta IPs?"
//   OFFER     (el servidor)    -- "te ofrezco esta"
//   REQUEST   (yo, a gritos)   -- "me quedo esa, y que se entere el resto"
//   ACK       (el servidor)    -- "tuya, y con ella van mascara, router y DNS"
//
// Los dos mios van a gritos (difusion) a proposito: cuando los mando
// TODAVIA NO TENGO IP, asi que no puedo hablar con nadie en concreto,
// y el REQUEST tiene ademas que enterar a los demas servidores DHCP de
// que su oferta queda rechazada.
//
// ESTE ARCHIVO NO TOCA EL HARDWARE. Produce los bytes que hay que
// enviar y consume los que llegan; quien los mete en un UDP y los saca
// por la tarjeta es net.c. Eso no es purismo: es lo que permite probar
// el cliente entero en el Mac, dandole respuestas fabricadas a mano,
// sin placa y sin router.
#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>
#include <stdbool.h>

#define DHCP_PUERTO_CLIENTE 68
#define DHCP_PUERTO_SERVIDOR 67

typedef enum {
    DHCP_PARADO,     // sin empezar
    DHCP_BUSCANDO,   // DISCOVER enviado, esperando OFFER
    DHCP_PIDIENDO,   // REQUEST enviado, esperando ACK
    DHCP_LISTO,      // configurado
    DHCP_RENDIDO     // nadie contesto -- net.c usa la de reserva
} dhcp_estado_t;

// Arranca la conversacion. 'semilla' identifica esta conversacion
// frente a las demas de la red (el campo xid); se pasa desde fuera en
// vez de sacarla de un reloj aqui dentro para que las pruebas den
// siempre el mismo resultado.
void dhcp_iniciar(const uint8_t mac[6], uint32_t semilla);

// Llamar en cada vuelta con los milisegundos desde el arranque. Si
// toca (re)enviar algo, escribe la carga UDP en 'salida' y devuelve su
// longitud; 0 = no hay nada que enviar ahora.
uint32_t dhcp_tick(uint64_t ms, uint8_t *salida, uint32_t max);

// Procesa una carga UDP que llego al puerto 68. Devuelve lo que haya
// que enviar a continuacion (el REQUEST, al recibir un OFFER), o 0.
uint32_t dhcp_recibir(const uint8_t *carga, uint32_t len, uint64_t ms,
                      uint8_t *salida, uint32_t max);

dhcp_estado_t dhcp_estado(void);

// Solo tienen valor con el estado en DHCP_LISTO. Cualquiera puede
// venir a cero si el servidor no lo mando.
void dhcp_config(uint8_t ip[4], uint8_t mascara[4], uint8_t pasarela[4], uint8_t dns[4]);

// Nombre del estado, para el log.
const char *dhcp_estado_nombre(void);

#endif
