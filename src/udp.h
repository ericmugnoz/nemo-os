// udp.h -- Nemo OS
// UDP (RFC 768) sobre IPv4. La cabecera son ocho bytes y el protocolo
// entero cabe en una tarjeta: puerto origen, puerto destino, longitud
// y suma de comprobacion. No hay conexion, ni orden, ni reintentos --
// eso es lo que lo hace util para DHCP y DNS, que resuelven lo suyo
// por su cuenta.
#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include <stdbool.h>

#define UDP_PROTO 17
#define UDP_HDR_LEN 8

// Escribe la cabecera UDP en 'out' delante de una carga que YA esta
// en out+8, y devuelve la longitud total (8 + carga_len).
//
// La suma de comprobacion cubre una "pseudo-cabecera" que no viaja por
// el cable: las dos IP y el protocolo. Es una comprobacion de que el
// datagrama llego a la maquina a la que iba, no solo de que los bytes
// estan intactos. En IPv4 es opcional (se puede dejar a cero), pero
// calcularla cuesta quince lineas y ahorra el dia en que un router
// entregue un paquete donde no debe.
uint32_t udp_empaquetar(uint8_t *out, uint32_t carga_len,
                        const uint8_t src_ip[4], const uint8_t dst_ip[4],
                        uint16_t src_port, uint16_t dst_port);

// Lee la cabecera de un datagrama recibido. Devuelve false si esta
// cortado, si la longitud declarada no cuadra, o si la suma de
// comprobacion no sale -- salvo que venga a cero, que segun la norma
// significa "no la calcule", y se acepta.
// *carga y *carga_len apuntan DENTRO de 'seg', sin copiar nada.
bool udp_desempaquetar(const uint8_t *seg, uint32_t seg_len,
                       const uint8_t src_ip[4], const uint8_t dst_ip[4],
                       uint16_t *src_port, uint16_t *dst_port,
                       const uint8_t **carga, uint32_t *carga_len);

#endif
