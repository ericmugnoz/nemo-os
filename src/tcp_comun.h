// tcp_comun.h -- Nemo OS
//
// Lo que comparten las dos mitades de TCP: la que ESCUCHA (tcp.c, la
// shell de red) y la que CONECTA (tcp_cliente.c, el navegador).
//
// POR QUE ESTAN SEPARADAS. Las dos hablan el mismo
// protocolo pero tienen formas opuestas. La que escucha es reactiva
// pura: llega un segmento, sale una respuesta, y no existe ningun
// momento en que hable por su cuenta. La que conecta empieza ella
// --manda un SYN sin que nadie le haya dicho nada-- y ademas tiene que
// GUARDAR lo que recibe para que la aplicacion lo lea cuando quiera, en
// vez de contestar en el acto.
//
// Meter las dos en un archivo habria obligado a convertir el estado
// global de tcp.c en una tabla de conexiones y a reescribir su maquina
// de estados, que funciona y no tiene pruebas que la respalden. Aqui se
// separa lo unico que de verdad es comun --construir y comprobar
// segmentos-- y cada mitad se queda con su forma. Nada duplicado.
#ifndef TCP_COMUN_H
#define TCP_COMUN_H

#include <stdint.h>
#include <stdbool.h>

#define TCP_PROTO_NUM 6
#define TCP_MIN_HDR   20

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

// Tamano maximo de segmento que anunciamos. 1460 = 1500 de la trama
// Ethernet menos 20 de IP y 20 de TCP.
#define TCP_MSS 1460

uint16_t tcpc_leer16(const uint8_t *p);
uint32_t tcpc_leer32(const uint8_t *p);
void     tcpc_escribir16(uint8_t *p, uint16_t v);
void     tcpc_escribir32(uint8_t *p, uint32_t v);

// El checksum de TCP cubre una PSEUDO-CABECERA que no viaja en el
// paquete: las dos IP, un cero, el protocolo y la longitud. Sin ella el
// otro extremo descarta cada segmento en silencio, que es de los
// fallos mas desagradables de encontrar.
uint16_t tcpc_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                       const uint8_t *seg, uint32_t seg_len);

// Rellena un segmento completo en 'out' y devuelve su longitud.
// 'ventana' se pasa desde fuera porque las dos mitades no anuncian lo
// mismo: la que escucha contesta con lo que le cabe de una vez, y la
// que descarga anuncia lo que le queda libre en su buffer.
uint32_t tcpc_construir(uint8_t *out, const uint8_t mi_ip[4], const uint8_t su_ip[4],
                        uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                        uint8_t flags, uint16_t ventana, bool con_mss,
                        const uint8_t *datos, uint32_t datos_len);

#endif
