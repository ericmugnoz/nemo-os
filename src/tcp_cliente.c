// tcp_cliente.c -- Nemo OS. Ver tcp_cliente.h para el alcance y el
// porque de cada simplificacion.

#include "tcp_cliente.h"
#include "tcp_comun.h"
#include "net.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);

static void put_dec(uint32_t v) {
    char d[12]; int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0 && n < 11) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) uart_putc(d[--n]);
}
static void put_ip(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) { put_dec(ip[i]); if (i < 3) uart_putc('.'); }
}

// Reintentos del SYN: la peticion inicial se puede perder como
// cualquier otra, y sin reintento la conexion se quedaria colgada para
// siempre por un solo paquete extraviado.
#define SYN_REINTENTO_MS 500
#define SYN_MAX_INTENTOS 6         // 3 segundos en total

// Puerto local. Se elige uno del rango "efimero" (49152-65535, el que
// la IANA reserva para quien llama) y se cambia en cada conexion: si se
// reutilizara, un segmento retrasado de la conexion anterior podria
// colarse en la siguiente.
#define PUERTO_BASE 49152

static tcpcli_estado_t estado = TCPCLI_CERRADO;
static uint8_t  ip_remota[4];
static uint16_t puerto_remoto, puerto_local;
static uint32_t snd_nxt, rcv_nxt;
static uint32_t isn = 0x5E4D3C2B;
static uint64_t ms_syn;
static int      intentos_syn;
static bool     fin_recibido;

static uint8_t  buf[TCPCLI_BUF];
static uint32_t buf_ini = 0, buf_fin = 0;   // [ini, fin) es lo que hay sin leer

static uint32_t libre(void) { return TCPCLI_BUF - buf_fin; }

// Lo que anunciamos como ventana es lo que nos queda libre, tope 65535
// (el campo son 16 bits). Anunciar un numero fijo mayor que el hueco
// real es invitar al otro extremo a mandar lo que no cabe.
// El campo de ventana de TCP son 16 bits, asi que con un buffer de
// 65536 hay un byte que no se puede anunciar nunca. No es un error ni
// merece arreglo: es exactamente el limite que hizo inventar la opcion
// de ESCALADO DE VENTANA, que sirve para buffers de megabytes y aqui no
// hace falta.
static uint16_t ventana(void) {
    uint32_t l = libre();
    return l > 65535u ? 65535u : (uint16_t)l;
}

// Compacta lo ya leido para recuperar sitio. Se hace aqui y no al leer
// porque mover memoria solo compensa cuando hace falta el hueco.
static void compactar(void) {
    if (buf_ini == 0) return;
    uint32_t n = buf_fin - buf_ini;
    for (uint32_t i = 0; i < n; i++) buf[i] = buf[buf_ini + i];
    buf_ini = 0;
    buf_fin = n;
}

// Devuelve si el segmento SALIO de verdad. Importa mas de lo que
// parece: net_enviar_ipv4() dice false cuando todavia esta resolviendo
// la MAC del destino por ARP, y eso pasa SIEMPRE con el primer paquete
// a una maquina nueva. Dar por enviado lo que no salio y avanzar el
// numero de secuencia dejaria la conexion desincronizada para siempre,
// con el otro extremo esperando unos bytes que no existen.
static bool mandar(uint8_t flags, bool con_mss, const uint8_t *datos, uint32_t len) {
    uint8_t seg[TCP_MIN_HDR + 4 + TCP_MSS];
    uint8_t mi_ip[4];
    net_get_ip(mi_ip);
    uint32_t n = tcpc_construir(seg, mi_ip, ip_remota, puerto_local, puerto_remoto,
                                snd_nxt, rcv_nxt, flags, ventana(), con_mss, datos, len);
    return net_enviar_ipv4(ip_remota, TCP_PROTO_NUM, seg, n);
}

bool tcpcli_conectar(const uint8_t ip[4], uint16_t puerto, uint64_t ms) {
    // 'ms' no se usa: el primer tick manda el SYN inmediatamente, mire
    // la hora que mire. Se mantiene en la firma porque todo lo demas de
    // esta interfaz la lleva, y una excepcion suelta se olvida.
    (void)ms;
    if (estado != TCPCLI_CERRADO && estado != TCPCLI_FALLO) return false;

    for (int i = 0; i < 4; i++) ip_remota[i] = ip[i];
    puerto_remoto = puerto;
    // Un puerto distinto cada vez, dentro del rango efimero.
    static uint16_t siguiente = 0;
    puerto_local = (uint16_t)(PUERTO_BASE + (siguiente++ % 16000));
    isn += 0x9E37;                  // numero de secuencia inicial distinto por conexion
    snd_nxt = isn;
    rcv_nxt = 0;
    buf_ini = buf_fin = 0;
    fin_recibido = false;
    intentos_syn = 0;
    ms_syn = 0;
    estado = TCPCLI_CONECTANDO;

    uart_puts("tcp-cli: conectando a "); put_ip(ip);
    uart_putc(':'); put_dec(puerto);
    uart_puts(" desde el puerto "); put_dec(puerto_local); uart_puts("\n");
    return true;
}

void tcpcli_tick(uint64_t ms) {
    if (estado != TCPCLI_CONECTANDO) return;

    if (intentos_syn > 0 && ms - ms_syn < SYN_REINTENTO_MS) return;

    if (intentos_syn >= SYN_MAX_INTENTOS) {
        uart_puts("tcp-cli: no contesta -- conexion fallida\n");
        estado = TCPCLI_FALLO;
        return;
    }
    intentos_syn++;
    ms_syn = ms;
    // El SYN se reenvia con el MISMO numero de secuencia: es el mismo
    // SYN otra vez, no uno nuevo. Cambiarlo confundiria al otro extremo
    // si el primero si habia llegado.
    mandar(TCP_SYN, true, 0, 0);
}

bool tcpcli_es_mio(uint16_t dport, uint16_t sport, const uint8_t src_ip[4]) {
    if (estado == TCPCLI_CERRADO) return false;
    if (dport != puerto_local || sport != puerto_remoto) return false;
    for (int i = 0; i < 4; i++) if (src_ip[i] != ip_remota[i]) return false;
    return true;
}

void tcpcli_manejar(const uint8_t *seg, uint32_t seg_len,
                    const uint8_t src_ip[4], const uint8_t dst_ip[4], uint64_t ms) {
    (void)ms;
    if (seg_len < TCP_MIN_HDR) return;
    uint32_t doff = (uint32_t)(seg[12] >> 4) * 4;
    if (doff < TCP_MIN_HDR || doff > seg_len) return;
    if (tcpc_checksum(src_ip, dst_ip, seg, seg_len) != 0) return;

    uint32_t seq = tcpc_leer32(seg + 4);
    uint32_t ack = tcpc_leer32(seg + 8);
    uint8_t  flags = seg[13];
    const uint8_t *datos = seg + doff;
    uint32_t datos_len = seg_len - doff;

    if (flags & TCP_RST) {
        uart_puts("tcp-cli: el otro extremo corto (RST)\n");
        estado = TCPCLI_FALLO;
        return;
    }

    if (estado == TCPCLI_CONECTANDO) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK) && ack == snd_nxt + 1) {
            snd_nxt += 1;               // nuestro SYN ocupaba un numero
            rcv_nxt = seq + 1;          // el suyo tambien
            estado = TCPCLI_ABIERTA;
            uart_puts("tcp-cli: conexion abierta\n");
            mandar(TCP_ACK, false, 0, 0);   // tercer paso del saludo
        }
        return;
    }

    if (estado != TCPCLI_ABIERTA && estado != TCPCLI_CERRANDO) return;

    // Datos. Solo se acepta lo que viene JUSTO a continuacion de lo
    // ultimo: sin reordenacion, un hueco no se puede rellenar despues.
    // Un segmento repetido (el otro extremo retransmitiendo porque
    // nuestro ACK se perdio) cae aqui con seq anterior y se descarta,
    // pero se le vuelve a confirmar -- que es lo que estaba esperando.
    bool algo_nuevo = false;
    if (datos_len > 0) {
        if (seq == rcv_nxt) {
            compactar();
            uint32_t cabe = libre();
            uint32_t n = datos_len < cabe ? datos_len : cabe;
            for (uint32_t i = 0; i < n; i++) buf[buf_fin + i] = datos[i];
            buf_fin += n;
            rcv_nxt += n;              // solo se confirma lo que se guardo
            algo_nuevo = true;
        } else {
            // Fuera de orden o repetido: se reconfirma por donde vamos.
            mandar(TCP_ACK, false, 0, 0);
            return;
        }
    }

    if ((flags & TCP_FIN) && seq + datos_len == rcv_nxt) {
        rcv_nxt += 1;
        fin_recibido = true;
        algo_nuevo = true;
        uart_puts("tcp-cli: el otro extremo termino de enviar (FIN)\n");
        if (mandar(TCP_ACK | TCP_FIN, false, 0, 0)) {
            snd_nxt += 1;
            estado = TCPCLI_CERRANDO;
        }
        return;
    }

    if (estado == TCPCLI_CERRANDO && (flags & TCP_ACK) && ack == snd_nxt) {
        uart_puts("tcp-cli: cerrada\n");
        estado = TCPCLI_CERRADO;
        return;
    }

    if (algo_nuevo) mandar(TCP_ACK, false, 0, 0);
}

uint32_t tcpcli_enviar(const uint8_t *datos, uint32_t len, uint64_t ms) {
    (void)ms;
    if (estado != TCPCLI_ABIERTA || len == 0) return 0;
    uint32_t n = len > TCP_MSS ? TCP_MSS : len;
    // Solo se da por enviado --y solo se avanza el numero de
    // secuencia-- si el paquete salio de verdad. Devolver 0 hace que
    // quien llama vuelva a intentarlo, que es lo correcto cuando lo
    // unico que falta es una respuesta ARP.
    if (!mandar(TCP_ACK | TCP_PSH, false, datos, n)) return 0;
    snd_nxt += n;
    return n;
}

uint32_t tcpcli_leer(uint8_t *out, uint32_t max) {
    uint32_t hay = buf_fin - buf_ini;
    uint32_t n = hay < max ? hay : max;
    for (uint32_t i = 0; i < n; i++) out[i] = buf[buf_ini + i];
    buf_ini += n;
    if (buf_ini == buf_fin) buf_ini = buf_fin = 0;   // vacio: volver al principio
    return n;
}

uint32_t tcpcli_pendiente(void) { return buf_fin - buf_ini; }

void tcpcli_cerrar(uint64_t ms) {
    (void)ms;
    if (estado != TCPCLI_ABIERTA) return;
    if (!mandar(TCP_ACK | TCP_FIN, false, 0, 0)) return;   // aun no salio: se reintenta
    snd_nxt += 1;
    estado = TCPCLI_CERRANDO;
    uart_puts("tcp-cli: cerrando por nuestra parte\n");
}

void tcpcli_abandonar(void) {
    if (estado == TCPCLI_CONECTANDO || estado == TCPCLI_ABIERTA || estado == TCPCLI_CERRANDO) {
        mandar(TCP_RST, false, 0, 0);
        uart_puts("tcp-cli: conexion abandonada\n");
    }
    estado = TCPCLI_CERRADO;
    buf_ini = buf_fin = 0;
    fin_recibido = false;
}

tcpcli_estado_t tcpcli_estado(void) { return estado; }
bool tcpcli_fin_recibido(void) { return fin_recibido; }

const char *tcpcli_estado_nombre(void) {
    switch (estado) {
        case TCPCLI_CERRADO:    return "cerrada";
        case TCPCLI_CONECTANDO: return "conectando";
        case TCPCLI_ABIERTA:    return "abierta";
        case TCPCLI_CERRANDO:   return "cerrando";
        default:                return "fallo";
    }
}
