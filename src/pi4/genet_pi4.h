// genet_pi4.h -- Nemo OS, Raspberry Pi 4
// Driver minimo del controlador Ethernet integrado (GENET v5, BCM2711).
// Fase 1 del roadmap de red: solo la capa fisica -- enviar y recibir
// una trama Ethernet cruda. ARP/IP/ICMP van encima, en otro archivo.
#ifndef GENET_PI4_H
#define GENET_PI4_H

#include <stdint.h>
#include <stdbool.h>

// Detecta el chip, resetea el MAC, sube el enlace por el PHY (dos
// intentos de negociacion con espera, como hace cualquier driver
// real -- el cable tarda un momento en negociar). true si hay enlace
// arriba al terminar. Nunca hace mas de un intento de deteccion del
// chip; si el GENET no responde como se espera, no toca nada mas.
bool genet_init_pi4(void);

// Direccion MAC de esta placa, leida de los fusibles del SoC durante
// el arranque (ver genet_get_mac_from_otp) -- 6 bytes en 'out'.
void genet_get_mac(uint8_t out[6]);

bool genet_link_up(void);
uint32_t genet_link_speed_mbps(void); // 10/100/1000, 0 si no hay enlace

// Envia una trama Ethernet completa (cabecera + carga), tal cual --
// esta capa no construye ni interpreta cabeceras. true si el
// hardware confirmo la transmision.
bool genet_send(const uint8_t *frame, uint32_t len);

// Sondea si hay una trama recibida esperando. Si la hay, la copia a
// 'out' (hasta 'max_len' bytes) y devuelve su longitud real; 0 si no
// hay ninguna. Nunca bloquea -- para llamar en cada vuelta del bucle
// principal, igual que el resto de dispositivos de entrada.
uint32_t genet_recv(uint8_t *out, uint32_t max_len);

// Escribe por el UART lo que dice el HARDWARE, no lo que
// cree el driver. Hace falta para separar dos fallos que desde arriba
// se parecen --"no llega nada" y "llega y lo tiramos"-- y que se
// arreglan en sitios completamente distintos.
//
// El numero que decide es el INDICE DE PRODUCTOR del anillo de
// recepcion: lo lleva el GENET, y solo avanza cuando el chip ha
// escrito una trama en la memoria. Si no se mueve mientras alguien
// hace ping, no esta llegando nada al chip y el problema esta en el
// cable o mas abajo. Si se mueve y aun asi no respondemos, la trama
// llega y la perdemos nosotros.
//
// Los 16 bits altos de ese mismo registro son el contador de tramas
// que el hardware DESCARTO por no caber en el anillo -- otra cosa que
// desde arriba no se ve de ninguna manera.
void genet_diagnostico(void);

#endif
