// tcp_cliente.h -- Nemo OS
//
// El lado de TCP que EMPIEZA la conversacion. El otro lado --esperar a que
// alguien llame-- esta en tcp.c, que es lo que usa la shell de red; esto es
// lo que hace falta para llamar nosotros.
//
// La diferencia no es de protocolo --los segmentos son los mismos-- sino
// de forma:
//
//   Escuchar:  llega un segmento -> sale una respuesta. Nunca habla
//              solo, y lo que recibe lo contesta en el acto.
//   Conectar:  manda un SYN sin que nadie le haya dicho nada, y lo que
//              recibe lo GUARDA para que la aplicacion lo lea cuando
//              pueda. Necesita un buffer y un reloj.
//
// Simplificaciones, todas a proposito y todas con el mismo motivo: esto
// habla con un proxy en la red local, no con internet.
//
//   * UNA conexion a la vez. Un navegador que baja una pagina de golpe
//     (un paquete, ver el diseno del proxy) no necesita mas.
//   * SIN retransmision de lo que enviamos. En una LAN no se pierde
//     casi nada, y si se pierde, el nivel de arriba reintenta la
//     peticion entera. Lo que SI se maneja es que el otro extremo
//     retransmita: sus duplicados se descartan por numero de secuencia.
//   * SIN reordenacion. Un segmento que llega fuera de orden se
//     descarta y se vuelve a pedir con un ACK del que faltaba.
//   * La ventana que anunciamos es LO QUE QUEDA LIBRE en el buffer, no
//     un numero fijo. Eso es lo que impide que el otro extremo nos
//     mande mas de lo que podemos guardar -- que con una descarga de
//     verdad deja de ser teorico.
#ifndef TCP_CLIENTE_H
#define TCP_CLIENTE_H

#include <stdint.h>
#include <stdbool.h>

// 64 KB de buffer de recepcion. Una pagina web reducida con sus
// imagenes en .nimg pasa de eso, asi que la aplicacion tiene que ir
// vaciandolo mientras descarga; el buffer no es la descarga entera,
// es el colchon entre lo que llega y lo que se consume.
#define TCPCLI_BUF 65536

typedef enum {
    TCPCLI_CERRADO,
    TCPCLI_CONECTANDO,   // SYN enviado, esperando SYN-ACK
    TCPCLI_ABIERTA,
    TCPCLI_CERRANDO,     // FIN enviado
    TCPCLI_FALLO         // no se pudo conectar, o el otro extremo corto
} tcpcli_estado_t;

// Abre una conexion. No envia nada todavia: el SYN sale en la primera
// llamada a tcpcli_tick(). Devuelve false si ya habia una conexion en
// marcha.
bool tcpcli_conectar(const uint8_t ip[4], uint16_t puerto, uint64_t ms);

// Llamar en cada vuelta. Se encarga del SYN inicial, de los reintentos
// mientras se conecta y del plazo maximo. No envia nada por si misma:
// usa net_enviar_ipv4(), igual que todo lo que sale de esta maquina.
void tcpcli_tick(uint64_t ms);

// Procesa un segmento dirigido a nuestro puerto local. 'src_ip' y
// 'dst_ip' son las del paquete recibido.
void tcpcli_manejar(const uint8_t *seg, uint32_t seg_len,
                    const uint8_t src_ip[4], const uint8_t dst_ip[4], uint64_t ms);

// true si el segmento es para esta conexion (puerto e IP). net.c lo
// pregunta antes de entregarselo, para no robarselo a la shell.
bool tcpcli_es_mio(uint16_t dport, uint16_t sport, const uint8_t src_ip[4]);

// Envia datos. Solo con la conexion abierta. Devuelve los bytes que se
// pudieron enviar -- puede ser menos de 'len' si no caben en un
// segmento, y entonces hay que volver a llamar con el resto.
uint32_t tcpcli_enviar(const uint8_t *datos, uint32_t len, uint64_t ms);

// Saca hasta 'max' bytes de lo recibido. 0 si no hay nada todavia.
uint32_t tcpcli_leer(uint8_t *out, uint32_t max);

// Cuanto hay esperando sin leer.
uint32_t tcpcli_pendiente(void);

// Cierra por nuestra parte (FIN). Lo que quede sin leer se puede seguir
// leyendo.
void tcpcli_cerrar(uint64_t ms);

// Olvida la conexion actual, este en el estado que este. Manda un RST
// para que el otro extremo no se quede esperando eternamente a una
// maquina que ya no va a contestar -- dejarlo colgado es de mala
// educacion y ademas ata un puerto suyo durante minutos.
//
// Hace falta de verdad: sin esto, un navegador al que le falla una
// pagina a media descarga no podria pedir otra hasta que la conexion
// rota se rindiera sola.
void tcpcli_abandonar(void);

tcpcli_estado_t tcpcli_estado(void);

// true si el otro extremo ya cerro y no va a llegar nada mas. Es lo que
// le dice a un cliente HTTP sin Content-Length que la respuesta termino.
bool tcpcli_fin_recibido(void);

const char *tcpcli_estado_nombre(void);

#endif
