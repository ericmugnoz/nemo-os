// tcp.h -- Nemo OS
// TCP minimo, una conexion, sin retransmision -- ver tcp.c para las
// simplificaciones y el porque de cada una.
#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <stdbool.h>

#define TCP_PROTO 6

// Abre el puerto en escucha (pasivo). Solo se puede escuchar en UN
// puerto -- llamar otra vez cambia de puerto y cierra lo que hubiera.
void tcp_escuchar(uint16_t puerto);

bool tcp_conectado(void);

// Procesa un segmento TCP ya extraido de IPv4 (net.c hace eso) y, si
// hay que responder, rellena 'out' con el segmento TCP de respuesta
// (SIN cabecera IP -- net.c la pone) y devuelve su longitud. 0 = no
// responder nada. src_ip/dst_ip: los del paquete RECIBIDO.
uint32_t tcp_manejar(const uint8_t *seg, uint32_t seg_len,
                     const uint8_t src_ip[4], const uint8_t dst_ip[4],
                     uint8_t *out, uint32_t out_max);

// --- Ganchos de la APLICACION, implementados fuera de tcp.c ---
// (netshell.c en el kernel; un eco en la bateria de pruebas del host).
// Al establecerse una conexion: escribir un saludo en 'salida' (hasta
// 'max' bytes) y devolver su longitud, o 0 para no enviar nada.
uint32_t tcp_app_on_connect(uint8_t *salida, uint32_t max);

// Datos recibidos: procesarlos y escribir la respuesta en 'salida'
// (hasta 'max'), devolver su longitud (0 = nada que responder ahora).
// Poner *cerrar a true para que TCP cierre la conexion tras enviar la
// respuesta (cierre activo, p.ej. el comando 'exit').
uint32_t tcp_app_on_data(const uint8_t *datos, uint32_t len, uint8_t *salida, uint32_t max, bool *cerrar);

#endif
