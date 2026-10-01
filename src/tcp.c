// tcp.c -- Nemo OS
// TCP minimo (RFC 793), Fase 3a del roadmap de red. Encima de net.c
// (IPv4), que le pasa cada segmento dirigido a nuestra IP y envia lo
// que este archivo devuelva.
//
// SIMPLIFICACIONES CONSCIENTES -- las mismas decisiones que ya se
// tomaron en el resto del sistema (Lua en vez de Python, NIMG en vez
// de PNG): lo minimo que funciona de verdad, documentado, en vez de
// una implementacion completa que tardaria meses.
//
//  * UNA conexion a la vez. De sobra para una shell remota, y evita
//    toda la gestion de multiples conexiones simultaneas.
//  * SIN retransmision. Es un cable directo Pi4-Mac -- no hay perdida
//    de paquetes esperable. Una TCP de produccion la necesita (sin
//    ella, un paquete perdido cuelga la conexion para siempre); esta
//    no, por ahora. Si se pierde algo, el cliente vera la conexion
//    colgada y tendra que reconectar.
//  * Ventana FIJA (1024 bytes), sin ajuste dinamico.
//  * SIN reordenacion: se asume que los segmentos llegan en orden. Un
//    segmento fuera de secuencia se descarta (y se re-ACKea lo que si
//    tenemos, que es lo que provoca que el otro extremo lo reenvie).
//  * Cada segmento entrante produce COMO MUCHO un segmento saliente --
//    encaja con el modelo "una trama entra, una trama sale" de
//    net_poll(). El ACK se lleva a caballo ("piggyback") sobre los
//    datos que enviemos, como hace cualquier TCP real.
//
// La APLICACION esta separada del protocolo: tcp_on_data() decide que
// hacer con los datos recibidos y que devolver. Ahora es un ECO (lo
// que llega, se devuelve tal cual) -- la prueba verificable de que el
// protocolo funciona, con `nc` desde el Mac. La Fase 3b cambia esa
// unica funcion por la shell, y el protocolo no se toca.

#include <stdint.h>
#include <stdbool.h>
#include "tcp.h"
#include "tcp_comun.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);

static void uart_put_dec(uint32_t v) {
    char d[12]; int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0 && n < 11) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) uart_putc(d[--n]);
}

// Estas cuatro y el checksum vivian aqui. Ahora estan en
// tcp_comun.c, compartidas con el lado que CONECTA (tcp_cliente.c).
// Se dejan como envoltorios de una linea para no tocar ni un renglon de
// la maquina de estados de abajo, que funciona.
static uint16_t leer16(const uint8_t *p) { return tcpc_leer16(p); }
static uint32_t leer32(const uint8_t *p) { return tcpc_leer32(p); }
static void escribir16(uint8_t *p, uint16_t v) { tcpc_escribir16(p, v); }
static void escribir32(uint8_t *p, uint32_t v) { tcpc_escribir32(p, v); }

// ---------------------------------------------------------------------
// Checksum de TCP: como el de IP (complemento a uno de 16 bits con
// acarreo plegado), pero sobre una PSEUDO-CABECERA que no viaja en el
// paquete -- IP origen, IP destino, un cero, el protocolo (6) y la
// longitud del segmento TCP -- seguida del segmento entero. Sin la
// pseudo-cabecera, el otro extremo descarta cada segmento en silencio.
// ---------------------------------------------------------------------
static uint16_t tcp_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4], const uint8_t *seg, uint32_t seg_len) {
    return tcpc_checksum(src_ip, dst_ip, seg, seg_len);
}

// ---------------------------------------------------------------------
// Cabecera TCP: 20 bytes minimo (data offset en palabras de 4 bytes,
// en el nibble alto del byte 12). Opciones despues, hasta el offset.
// ---------------------------------------------------------------------
#define FLAG_FIN 0x01
#define FLAG_SYN 0x02
#define FLAG_RST 0x04
#define FLAG_PSH 0x08
#define FLAG_ACK 0x10

#define VENTANA 1024
#define MSS 1460

// ---------------------------------------------------------------------
// Estado de la UNICA conexion.
// ---------------------------------------------------------------------
// FIN_WAIT_1/2: cierre ACTIVO, iniciado por nosotros (la aplicacion
// pidio cerrar, p.ej. el comando 'exit' de la shell). Enviamos FIN,
// esperamos su ACK (FIN_WAIT_1 -> FIN_WAIT_2) y su FIN (-> ACK final y
// de vuelta a escuchar, saltandonos TIME_WAIT: con un solo puerto y
// un solo cliente no aporta nada esperar).
typedef enum { CERRADO, ESCUCHANDO, SYN_RECIBIDO, ESTABLECIDO, ULTIMO_ACK, FIN_WAIT_1, FIN_WAIT_2 } estado_t;

static estado_t estado = CERRADO;
static uint16_t puerto_local = 0;
static uint16_t puerto_remoto = 0;
static uint8_t  ip_remota[4];
static uint32_t snd_nxt = 0;   // siguiente numero de secuencia que enviaremos
static uint32_t rcv_nxt = 0;   // siguiente numero de secuencia que esperamos recibir
static uint32_t isn_contador = 0x1A2B3C4D; // "aleatorio" de andar por casa, ver tcp_escuchar

// La aplicacion vive fuera de este archivo -- ver los tres ganchos
// tcp_app_* en tcp.h. En la Fase 3a era un eco (ahora solo en la
// bateria de pruebas del host); desde la Fase 3b es la shell de red
// (netshell.c). El protocolo no sabe cual de las dos es.

static void volver_a_escuchar(void) {
    estado = ESCUCHANDO;
    puerto_remoto = 0;
    snd_nxt = 0; rcv_nxt = 0;
}

void tcp_escuchar(uint16_t puerto) {
    puerto_local = puerto;
    volver_a_escuchar();
    uart_puts("tcp: escuchando en el puerto "); uart_put_dec(puerto); uart_puts("\n");
}

bool tcp_conectado(void) { return estado == ESTABLECIDO; }

// Rellena un segmento TCP en 'out' (cabecera + opcional MSS + datos).
// Devuelve la longitud total. El checksum se calcula aqui con las IPs
// que nos pasan (las del paquete que estamos RESPONDIENDO, invertidas).
static uint32_t construir_segmento(uint8_t *out, const uint8_t mi_ip[4], const uint8_t su_ip[4],
                                   uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                                   uint8_t flags, bool con_mss, const uint8_t *datos, uint32_t datos_len) {
    // La ventana de este lado es fija: la shell contesta lo que le cabe
    // de una vez y no acumula nada. El lado que descarga si la calcula
    // (ver tcp_cliente.c), porque ahi si hay un buffer que se llena.
    return tcpc_construir(out, mi_ip, su_ip, sport, dport, seq, ack, flags,
                          VENTANA, con_mss, datos, datos_len);
}

// Un RST para lo que no nos interesa (puerto cerrado, o basura en una
// conexion). Con ACK, apuntando justo despues de lo que enviaron -- asi
// el otro extremo sabe que fue rechazado y no se queda esperando.
static uint32_t construir_rst(uint8_t *out, const uint8_t mi_ip[4], const uint8_t su_ip[4],
                              uint16_t sport, uint16_t dport, uint32_t su_seq, uint32_t su_len) {
    return construir_segmento(out, mi_ip, su_ip, sport, dport, 0, su_seq + su_len,
                              FLAG_RST | FLAG_ACK, false, 0, 0);
}

uint32_t tcp_manejar(const uint8_t *seg, uint32_t seg_len, const uint8_t src_ip[4], const uint8_t dst_ip[4],
                     uint8_t *out, uint32_t out_max) {
    if (seg_len < TCP_MIN_HDR) return 0;
    uint32_t doff = (uint32_t)(seg[12] >> 4) * 4;
    if (doff < TCP_MIN_HDR || doff > seg_len) return 0;

    if (tcp_checksum(src_ip, dst_ip, seg, seg_len) != 0) {
        // Volcado para diagnostico: el checksum del otro extremo (una pila
        // TCP real) es correcto con toda seguridad, asi que si no cuadra
        // es que algo en lo que recibimos, o en como lo sumamos, difiere
        // de lo que se probo en el host. Los bytes reales lo resuelven.
        uart_puts("tcp: checksum invalido, descartado. seg_len="); uart_put_dec(seg_len);
        uart_puts(" doff="); uart_put_dec(doff);
        uart_puts(" src="); uart_put_dec(src_ip[0]); uart_putc('.'); uart_put_dec(src_ip[1]); uart_putc('.'); uart_put_dec(src_ip[2]); uart_putc('.'); uart_put_dec(src_ip[3]);
        uart_puts(" dst="); uart_put_dec(dst_ip[0]); uart_putc('.'); uart_put_dec(dst_ip[1]); uart_putc('.'); uart_put_dec(dst_ip[2]); uart_putc('.'); uart_put_dec(dst_ip[3]);
        uart_puts("\ntcp: bytes: ");
        const char *hex = "0123456789abcdef";
        uint32_t n = seg_len < 80 ? seg_len : 80;
        for (uint32_t i = 0; i < n; i++) { uart_putc(hex[seg[i] >> 4]); uart_putc(hex[seg[i] & 0xF]); uart_putc(' '); }
        uart_puts("\n");
        return 0;
    }

    uint16_t sport = leer16(seg + 0);   // puerto del que nos habla (remoto)
    uint16_t dport = leer16(seg + 2);   // puerto nuestro
    uint32_t seq = leer32(seg + 4);
    uint32_t ack = leer32(seg + 8);
    uint8_t flags = seg[13];
    const uint8_t *datos = seg + doff;
    uint32_t datos_len = seg_len - doff;

    if (out_max < TCP_MIN_HDR + 4 + VENTANA) return 0; // sitio para cabecera+MSS+datos de eco

    // Puerto que no es el nuestro, o no estamos escuchando nada: RST.
    if (estado == CERRADO || dport != puerto_local) {
        if (flags & FLAG_RST) return 0; // nunca se responde a un RST con otro RST
        return construir_rst(out, dst_ip, src_ip, dport, sport, seq, datos_len + ((flags & FLAG_SYN) ? 1 : 0));
    }

    // Un RST del otro extremo, en cualquier estado con conexion: se cierra.
    if ((flags & FLAG_RST) && estado != ESCUCHANDO) {
        uart_puts("tcp: RST recibido, conexion cerrada\n");
        volver_a_escuchar();
        return 0;
    }

    // Un SYN nuevo cuando ya habia conexion: el cliente se reinicio
    // (o un `nc` nuevo sin cerrar el anterior). Se olvida la anterior y
    // se atiende esta -- mucho mas comodo para probar que quedarse
    // colgado en un estado que ya no representa a nadie.
    if ((flags & FLAG_SYN) && !(flags & FLAG_ACK) && estado != ESCUCHANDO) {
        uart_puts("tcp: SYN nuevo con conexion abierta, reiniciando\n");
        volver_a_escuchar();
    }

    switch (estado) {
    case ESCUCHANDO:
        if ((flags & FLAG_SYN) && !(flags & FLAG_ACK)) {
            puerto_remoto = sport;
            for (int i = 0; i < 4; i++) ip_remota[i] = src_ip[i];
            rcv_nxt = seq + 1;                 // el SYN ocupa un numero de secuencia
            isn_contador += 64000;             // ISN distinto por conexion (RFC 793 pide que crezca)
            snd_nxt = isn_contador;
            estado = SYN_RECIBIDO;
            uart_puts("tcp: SYN de "); uart_put_dec(src_ip[0]); uart_putc('.'); uart_put_dec(src_ip[1]); uart_putc('.');
            uart_put_dec(src_ip[2]); uart_putc('.'); uart_put_dec(src_ip[3]); uart_puts(":"); uart_put_dec(sport);
            uart_puts(" -- SYN-ACK\n");
            uint32_t n = construir_segmento(out, dst_ip, src_ip, puerto_local, puerto_remoto,
                                            snd_nxt, rcv_nxt, FLAG_SYN | FLAG_ACK, true, 0, 0);
            snd_nxt += 1;                      // nuestro SYN tambien ocupa uno
            return n;
        }
        return 0; // cualquier otra cosa en ESCUCHANDO se ignora

    case SYN_RECIBIDO:
        if ((flags & FLAG_ACK) && ack == snd_nxt && seq == rcv_nxt) {
            estado = ESTABLECIDO;
            uart_puts("tcp: conexion ESTABLECIDA\n");
            // El tercer paso del saludo puede traer ya datos (raro, pero
            // legal). Se caen al caso ESTABLECIDO de abajo.
            if (datos_len == 0 && !(flags & FLAG_FIN)) {
                // Conexion recien abierta y sin nada que procesar: la
                // aplicacion puede saludar (la shell manda su banner y
                // el prompt). Va como datos con ACK, un segmento normal.
                static uint8_t saludo[VENTANA];
                uint32_t n_saludo = tcp_app_on_connect(saludo, sizeof(saludo));
                if (n_saludo == 0) return 0;
                uint32_t n = construir_segmento(out, dst_ip, src_ip, puerto_local, puerto_remoto,
                                                snd_nxt, rcv_nxt, FLAG_ACK | FLAG_PSH, false, saludo, n_saludo);
                snd_nxt += n_saludo;
                return n;
            }
        } else {
            return 0;
        }
        // fallthrough intencionado: procesar datos/FIN que acompanen al ACK
        // (gcc: la etiqueta de abajo lo hace explicito)
        __attribute__((fallthrough));

    case ESTABLECIDO: {
        if (sport != puerto_remoto) return 0; // otro cliente hablandole a un puerto ocupado: se ignora

        // Solo aceptamos lo que va justo a continuacion de lo ultimo
        // recibido. Un segmento repetido o adelantado se descarta y se
        // re-ACKea rcv_nxt -- eso le dice al otro extremo exactamente que
        // tenemos, y reenvia lo que falte (asi funciona TCP sin que nosotros
        // tengamos que guardar nada fuera de orden).
        if (seq != rcv_nxt) {
            uart_puts("tcp: segmento fuera de secuencia (seq="); uart_put_dec(seq);
            uart_puts(", esperaba "); uart_put_dec(rcv_nxt); uart_puts(", "); uart_put_dec(datos_len);
            uart_puts(" bytes) -- re-ACK\n");
            return construir_segmento(out, dst_ip, src_ip, puerto_local, puerto_remoto,
                                      snd_nxt, rcv_nxt, FLAG_ACK, false, 0, 0);
        }

        bool fin = (flags & FLAG_FIN) != 0;
        uint32_t respuesta_len = 0;
        static uint8_t resp_datos[VENTANA];

        bool cerrar_nosotros = false;
        if (datos_len > 0) {
            uart_puts("tcp: datos recibidos, "); uart_put_dec(datos_len);
            uart_puts(" bytes (seq="); uart_put_dec(seq); uart_puts(")\n");
            rcv_nxt += datos_len;
            respuesta_len = tcp_app_on_data(datos, datos_len, resp_datos, sizeof(resp_datos), &cerrar_nosotros);
        }

        if (fin) {
            // El cliente cierra. Se ACKea su FIN y se manda el nuestro en
            // el MISMO segmento (con los ultimos datos, si los hay) -- pasa
            // directamente a ULTIMO_ACK, saltandose CLOSE_WAIT, que aqui no
            // aporta nada porque no tenemos nada pendiente que decir.
            rcv_nxt += 1;
            uart_puts("tcp: FIN del cliente -- cerrando\n");
            uint32_t n = construir_segmento(out, dst_ip, src_ip, puerto_local, puerto_remoto,
                                            snd_nxt, rcv_nxt, FLAG_FIN | FLAG_ACK | (respuesta_len ? FLAG_PSH : 0),
                                            false, resp_datos, respuesta_len);
            snd_nxt += respuesta_len + 1;
            estado = ULTIMO_ACK;
            return n;
        }

        if (datos_len > 0) {
            if (cerrar_nosotros) {
                // La aplicacion quiere cerrar (p.ej. 'exit'): ultimos datos
                // + nuestro FIN en el mismo segmento. Cierre activo.
                uart_puts("tcp: la aplicacion pide cerrar -- FIN\n");
                uint32_t n = construir_segmento(out, dst_ip, src_ip, puerto_local, puerto_remoto,
                                                snd_nxt, rcv_nxt, FLAG_ACK | FLAG_FIN | (respuesta_len ? FLAG_PSH : 0),
                                                false, resp_datos, respuesta_len);
                snd_nxt += respuesta_len + 1;
                estado = FIN_WAIT_1;
                return n;
            }
            // ACK a caballo sobre la respuesta de la aplicacion.
            uint32_t n = construir_segmento(out, dst_ip, src_ip, puerto_local, puerto_remoto,
                                            snd_nxt, rcv_nxt, FLAG_ACK | (respuesta_len ? FLAG_PSH : 0),
                                            false, resp_datos, respuesta_len);
            snd_nxt += respuesta_len;
            return n;
        }
        return 0; // ACK puro del cliente (confirmando algo nuestro): nada que responder
    }

    case FIN_WAIT_1:
    case FIN_WAIT_2: {
        if (sport != puerto_remoto) return 0;
        if (estado == FIN_WAIT_1 && (flags & FLAG_ACK) && ack == snd_nxt) estado = FIN_WAIT_2;
        // Datos que el cliente aun tuviera en vuelo: se confirman y se
        // descartan (la aplicacion ya cerro). Su FIN: se confirma y se
        // vuelve a escuchar.
        if (seq == rcv_nxt && (datos_len > 0 || (flags & FLAG_FIN))) {
            rcv_nxt += datos_len + ((flags & FLAG_FIN) ? 1 : 0);
            uint32_t n = construir_segmento(out, dst_ip, src_ip, puerto_local, puerto_remoto,
                                            snd_nxt, rcv_nxt, FLAG_ACK, false, 0, 0);
            if (flags & FLAG_FIN) {
                uart_puts("tcp: FIN del cliente tras el nuestro -- cerrada, de vuelta a escuchar\n");
                volver_a_escuchar();
            }
            return n;
        }
        return 0;
    }

    case ULTIMO_ACK:
        if ((flags & FLAG_ACK) && ack == snd_nxt) {
            uart_puts("tcp: cerrada limpiamente, de vuelta a escuchar\n");
            volver_a_escuchar();
        }
        return 0;

    default:
        return 0;
    }
}
