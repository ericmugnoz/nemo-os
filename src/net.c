// net.c -- Nemo OS
// La capa de red del sistema: ARP, IPv4, ICMP, y el reparto de lo que
// llega a quien le toca. Debajo tiene una tarjeta cualquiera vista a
// traves de nic.h (el Ethernet integrado en la Pi 4, una virtio-net en
// QEMU) y encima, cada protocolo de transporte en su propio archivo.
//
// Todo lo de aqui es SONDEO PURO, sin interrupciones: task_yield()
// (tasks.c) llama a net_poll() cien veces por segundo, el mismo patron
// que ya usa la entrada de teclado y raton. Ninguna funcion de este
// archivo espera a nada.
//
// LO QUE HACE
//
//   * Responde ARP ("quien tiene mi IP"). La tabla de direcciones
//     aprendidas, con su caducidad, esta en arp.c.
//   * Responde al ping: un ICMP echo request dirigido a nuestra IP.
//   * Envia hacia fuera. net_enviar_ipv4() y net_enviar_udp() eligen
//     el salto siguiente segun la mascara (la otra maquina esta en
//     nuestra red, o hay que entregarlo a la pasarela), resuelven su
//     MAC por ARP y entregan la trama. Admiten direcciones de
//     DIFUSION, que es lo que permite hablar con una red en la que
//     todavia no se conoce a nadie.
//   * Reparte lo que llega. El UDP del puerto 68 va al cliente DHCP
//     (dhcp.c) y el resto a los sockets de los programas
//     (udp_sock.c). El TCP se reparte entre las dos mitades que
//     conviven: la que ESCUCHA (tcp.c, que sirve la shell remota) y la
//     que CONECTA (tcp_cliente.c, debajo del cliente HTTP).
//   * Consigue la configuracion: se la pide al router por DHCP y, si
//     no hay router, usa una direccion de reserva que calcula sola.
//     El porque de las dos esta mas abajo, en "Configuracion de red
//     de esta placa".
//
// LO QUE NO HACE
//
//   * Fragmentacion IP, ni IPv6.
//   * Mandar pings por iniciativa propia: solo los contesta.
//   * Nombres. No hay resolutor de DNS en ninguna parte del sistema:
//     net_get_dns() dice que servidor nos dio el DHCP, pero nadie le
//     pregunta nunca nada. Cada programa de red lleva dentro la
//     direccion escrita en numeros.

#include <stdint.h>
#include <stdbool.h>
#include "net.h"
#include "tcp.h"
#include "tcp_cliente.h"
#include "descarga.h"
#include "udp.h"
#include "udp_sock.h"
#include "dhcp.h"
#include "arp.h"
#include "nic.h"
#include "timer.h"
#include "bitacora.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);

static void uart_put_dec(uint32_t v) {
    char d[12]; int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0 && n < 11) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) uart_putc(d[--n]);
}
static void uart_put_ip(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) { uart_put_dec(ip[i]); if (i < 3) uart_putc('.'); }
}
static void uart_put_mac(const uint8_t mac[6]) {
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        uart_putc(hex[mac[i] >> 4]); uart_putc(hex[mac[i] & 0xF]);
        if (i < 5) uart_putc(':');
    }
}

// ---------------------------------------------------------------------
// Configuracion de red de esta placa.
//
// La IP se le PIDE al router (ver dhcp.c). La direccion escrita aqui a mano
// es lo que se usa si no hay nadie a quien pedirsela.
//
// Las dos cosas hacen falta, y no son alternativas:
//
//   - Con router: DHCP da IP, mascara, pasarela y DNS. La placa es una
//     maquina mas de la red y se puede llegar a ella desde cualquier
//     sitio de la casa.
//   - Sin router, cable directo a un Mac: no hay DHCP y no lo va a
//     haber. Ahi entra la IP de reserva, en el rango 169.254.0.0/16
//     (RFC 3927), que es justo el que macOS se autoasigna solo en esa
//     situacion. Con eso el ping funciona sin tocar nada en el Mac.
//
// Por eso el orden es: pedirla, y si nadie contesta en ocho segundos,
// la de reserva. Lo que funcionaba antes de existir DHCP sigue
// funcionando exactamente igual.
//
// La IP de reserva se CALCULA a partir de la MAC; ver ip_de_reserva(),
// mas abajo, y el porque.
// ---------------------------------------------------------------------
static uint8_t mi_mac[6];

// Lo que esta en uso AHORA. Empieza a cero: sin direccion. Es el
// estado correcto mientras se pregunta, y el que hace que no
// respondamos a nombre de una IP que todavia no es nuestra.
static uint8_t mi_ip[4]       = { 0, 0, 0, 0 };
static uint8_t mi_mascara[4]  = { 0, 0, 0, 0 };
static uint8_t mi_pasarela[4] = { 0, 0, 0, 0 };
static uint8_t mi_dns[4]      = { 0, 0, 0, 0 };

static uint16_t leer16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void escribir16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static uint16_t ip_checksum(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t suma = 0;
    while (len > 1) {
        suma += ((uint32_t)p[0] << 8) | p[1];
        p += 2; len -= 2;
    }
    if (len == 1) suma += (uint32_t)p[0] << 8;
    while (suma >> 16) suma = (suma & 0xFFFF) + (suma >> 16);
    return (uint16_t)~suma;
}

// ---------------------------------------------------------------------
// Cabecera Ethernet: 14 bytes -- 6 MAC destino, 6 MAC origen, 2 tipo.
// ---------------------------------------------------------------------
#define ETH_HDR_LEN 14
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IPV4 0x0800

// ---------------------------------------------------------------------
// ARP (RFC 826) -- 28 bytes para Ethernet+IPv4.
// ---------------------------------------------------------------------
#define ARP_LEN 28
#define ARP_HTYPE_ETH 1
#define ARP_PTYPE_IP  0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

static uint32_t manejar_arp(const uint8_t *frame, uint32_t len, uint8_t *salida) {
    if (len < ETH_HDR_LEN + ARP_LEN) return 0;
    const uint8_t *arp = frame + ETH_HDR_LEN;

    uint16_t htype = leer16(arp + 0), ptype = leer16(arp + 2);
    uint8_t hlen = arp[4], plen = arp[5];
    uint16_t op = leer16(arp + 6);
    const uint8_t *mac_origen = arp + 8;
    const uint8_t *ip_origen  = arp + 14;
    const uint8_t *ip_destino = arp + 24;

    if (htype != ARP_HTYPE_ETH || ptype != ARP_PTYPE_IP || hlen != 6 || plen != 4) return 0;

    // Apuntar SIEMPRE quien es quien, sea cual sea la
    // operacion y vaya el paquete dirigido a nosotros o no. Una
    // peticion ARP lleva dentro el remite de quien pregunta, asi que es
    // informacion gratis que llega sola: en una red con movimiento, la
    // tabla se llena sin haber preguntado una sola vez. Y las
    // RESPUESTAS solo sirven para esto -- son justo lo que llega cuando
    // somos nosotros los que preguntamos.
    arp_aprender(ip_origen, mac_origen, timer_get_ticks() * 10);

    if (op != ARP_OP_REQUEST) return 0;   // una respuesta ya cumplio arriba
    for (int i = 0; i < 4; i++) if (ip_destino[i] != mi_ip[i]) return 0; // no es para nosotros

    uart_puts("net: ARP -- quien tiene "); uart_put_ip(mi_ip);
    uart_puts("? responde "); uart_put_mac(mi_mac); uart_puts("\n");

    // Construir la respuesta: mismo formato, con origen y destino
    // intercambiados y OP=REPLY.
    uint8_t *e = salida;
    for (int i = 0; i < 6; i++) e[i] = mac_origen[i];       // MAC destino = quien pregunto
    for (int i = 0; i < 6; i++) e[6 + i] = mi_mac[i];       // MAC origen = nosotros
    escribir16(e + 12, ETHERTYPE_ARP);

    uint8_t *a = salida + ETH_HDR_LEN;
    escribir16(a + 0, ARP_HTYPE_ETH);
    escribir16(a + 2, ARP_PTYPE_IP);
    a[4] = 6; a[5] = 4;
    escribir16(a + 6, ARP_OP_REPLY);
    for (int i = 0; i < 6; i++) a[8 + i] = mi_mac[i];        // MAC remitente = nosotros
    for (int i = 0; i < 4; i++) a[14 + i] = mi_ip[i];        // IP remitente = nosotros
    for (int i = 0; i < 6; i++) a[18 + i] = mac_origen[i];   // MAC destino = quien pregunto
    for (int i = 0; i < 4; i++) a[24 + i] = ip_origen[i];    // IP destino = quien pregunto

    return ETH_HDR_LEN + ARP_LEN;
}

// ---------------------------------------------------------------------
// IPv4 (RFC 791) -- cabecera de 20 bytes sin opciones (lo unico que
// generamos y lo unico que aceptamos; con opciones, se descarta).
// ---------------------------------------------------------------------
#define IP_MIN_HDR_LEN 20
#define IP_PROTO_ICMP 1

// ---------------------------------------------------------------------
// ICMP (RFC 792) -- echo request/reply, 8 bytes de cabecera + carga.
// ---------------------------------------------------------------------
#define ICMP_HDR_LEN 8
#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

// Rellena cabecera Ethernet + IPv4 de una respuesta a 'frame' (el
// paquete recibido): MACs e IPs invertidas, protocolo y longitud de
// carga dados. Devuelve el puntero donde empieza la carga en 'salida'.
// Lo usan tanto el ping (ICMP) como TCP.
static uint8_t *construir_cabeceras_respuesta(const uint8_t *frame, uint8_t *salida,
                                              uint8_t protocolo, uint32_t carga_len) {
    const uint8_t *eth_origen = frame + 6;
    const uint8_t *ip = frame + ETH_HDR_LEN;
    const uint8_t *ip_origen = ip + 12;
    const uint8_t *ip_destino = ip + 16;

    uint8_t *e = salida;
    for (int i = 0; i < 6; i++) e[i] = eth_origen[i];
    for (int i = 0; i < 6; i++) e[6 + i] = mi_mac[i];
    escribir16(e + 12, ETHERTYPE_IPV4);

    uint8_t *out_ip = salida + ETH_HDR_LEN;
    out_ip[0] = 0x45; out_ip[1] = 0;
    escribir16(out_ip + 2, (uint16_t)(IP_MIN_HDR_LEN + carga_len));
    escribir16(out_ip + 4, leer16(ip + 4));
    escribir16(out_ip + 6, 0x4000); // DF: no fragmentar (nunca generamos mas de una trama)
    out_ip[8] = 64;
    out_ip[9] = protocolo;
    escribir16(out_ip + 10, 0);
    for (int i = 0; i < 4; i++) out_ip[12 + i] = ip_destino[i];
    for (int i = 0; i < 4; i++) out_ip[16 + i] = ip_origen[i];
    escribir16(out_ip + 10, ip_checksum(out_ip, IP_MIN_HDR_LEN));
    return out_ip + IP_MIN_HDR_LEN;
}

// Construye la respuesta ICMP completa (Ethernet+IP+ICMP) para un
// echo request ya validado. 'salida' tiene que tener sitio para lo
// mismo que 'len' (la respuesta nunca es mas grande que la petición).
static uint32_t responder_ping(const uint8_t *frame, uint32_t len, uint8_t *salida) {
    const uint8_t *eth_origen = frame + 6;
    const uint8_t *ip = frame + ETH_HDR_LEN;
    uint32_t ihl = (uint32_t)(ip[0] & 0x0F) * 4;
    uint32_t ip_total_len = leer16(ip + 2);
    if (ip_total_len > len - ETH_HDR_LEN) ip_total_len = len - ETH_HDR_LEN; // por si la trama trae relleno
    const uint8_t *icmp = ip + ihl;
    uint32_t icmp_len = ip_total_len - ihl;
    if (icmp_len < ICMP_HDR_LEN) return 0;

    const uint8_t *ip_origen = ip + 12;
    const uint8_t *ip_destino = ip + 16;

    uint8_t *e = salida;
    for (int i = 0; i < 6; i++) e[i] = eth_origen[i];
    for (int i = 0; i < 6; i++) e[6 + i] = mi_mac[i];
    escribir16(e + 12, ETHERTYPE_IPV4);

    uint8_t *out_ip = salida + ETH_HDR_LEN;
    out_ip[0] = 0x45; out_ip[1] = 0; // version 4, IHL 5 (20 bytes), sin ToS
    escribir16(out_ip + 2, (uint16_t)(IP_MIN_HDR_LEN + icmp_len));
    escribir16(out_ip + 4, leer16(ip + 4)); // mismo identificador que la peticion, por cortesia
    escribir16(out_ip + 6, 0); // sin fragmentar
    out_ip[8] = 64;            // TTL
    out_ip[9] = IP_PROTO_ICMP;
    escribir16(out_ip + 10, 0); // checksum, se calcula despues
    for (int i = 0; i < 4; i++) out_ip[12 + i] = ip_destino[i]; // origen = nosotros (destino de la peticion)
    for (int i = 0; i < 4; i++) out_ip[16 + i] = ip_origen[i];  // destino = quien pregunto
    uint16_t ip_cs = ip_checksum(out_ip, IP_MIN_HDR_LEN);
    escribir16(out_ip + 10, ip_cs);

    uint8_t *out_icmp = out_ip + IP_MIN_HDR_LEN;
    out_icmp[0] = ICMP_ECHO_REPLY;
    out_icmp[1] = 0;
    escribir16(out_icmp + 2, 0); // checksum, se calcula despues
    // Identificador, numero de secuencia y carga: se copian TAL CUAL
    // de la peticion -- es lo que hace cualquier ping real para poder
    // emparejar peticion/respuesta en el otro extremo.
    for (uint32_t i = 4; i < icmp_len; i++) out_icmp[i] = icmp[i];
    uint16_t icmp_cs = ip_checksum(out_icmp, icmp_len);
    escribir16(out_icmp + 2, icmp_cs);

    return ETH_HDR_LEN + IP_MIN_HDR_LEN + icmp_len;
}

// ---------------------------------------------------------------------
// Hablar por iniciativa propia
//
// Todo lo de arriba CONTESTA: coge una trama que llego, le da la
// vuelta a las direcciones y la devuelve. DHCP es lo contrario --
// empieza la conversacion-- y para eso hace falta construir una trama
// entera desde cero, sin ninguna de la que copiar. Esto es esa pieza.
//
// Va a gritos, y tiene que ir a gritos: cuando se manda, no se sabe ni
// la propia IP ni quien es el servidor ni donde esta.
// ---------------------------------------------------------------------
static uint8_t buf_dhcp[700] __attribute__((aligned(4)));

static void enviar_dhcp(const uint8_t *carga, uint32_t carga_len) {
    if (carga_len == 0 || ETH_HDR_LEN + IP_MIN_HDR_LEN + UDP_HDR_LEN + carga_len > sizeof buf_dhcp)
        return;

    static const uint8_t sin_ip[4]   = { 0, 0, 0, 0 };
    static const uint8_t a_gritos[4] = { 255, 255, 255, 255 };

    uint8_t *e = buf_dhcp;
    for (int i = 0; i < 6; i++) e[i] = 0xFF;              // a toda la red
    for (int i = 0; i < 6; i++) e[6 + i] = mi_mac[i];
    escribir16(e + 12, ETHERTYPE_IPV4);

    uint8_t *udp = buf_dhcp + ETH_HDR_LEN + IP_MIN_HDR_LEN;
    for (uint32_t i = 0; i < carga_len; i++) udp[UDP_HDR_LEN + i] = carga[i];
    uint32_t udp_len = udp_empaquetar(udp, carga_len, sin_ip, a_gritos,
                                      DHCP_PUERTO_CLIENTE, DHCP_PUERTO_SERVIDOR);

    uint8_t *ip = buf_dhcp + ETH_HDR_LEN;
    ip[0] = 0x45; ip[1] = 0;
    escribir16(ip + 2, (uint16_t)(IP_MIN_HDR_LEN + udp_len));
    escribir16(ip + 4, 0);
    escribir16(ip + 6, 0);     // sin DF: un DHCP puede pasar por sitios raros
    ip[8] = 64;                // TTL
    ip[9] = UDP_PROTO;
    escribir16(ip + 10, 0);
    for (int i = 0; i < 4; i++) ip[12 + i] = sin_ip[i];
    for (int i = 0; i < 4; i++) ip[16 + i] = a_gritos[i];
    escribir16(ip + 10, ip_checksum(ip, IP_MIN_HDR_LEN));

    nic_send(buf_dhcp, ETH_HDR_LEN + IP_MIN_HDR_LEN + udp_len);
}

// ---------------------------------------------------------------------
// Mandarle algo a OTRA maquina
//
// Aqui aparecen las tres cosas que responder nunca necesito, y por eso
// esto no existia:
//
//   1. Decidir por donde sale. Si el destino esta en mi subred, se le
//      habla directamente. Si no, hay que entregarselo a la pasarela --
//      y entonces la MAC que hace falta es la DE LA PASARELA, no la del
//      destino final, que puede estar al otro lado del mundo.
//   2. Averiguar su MAC. Eso es preguntar a la red y esperar.
//   3. Guardar el paquete mientras tanto.
//
// La cola de espera es de UN paquete, a proposito. Un sistema que habla
// con un proxy no tiene veinte conversaciones a medias; y si llegara un
// segundo paquete antes de resolver el primero, perder el primero es
// exactamente lo que hace una red: el nivel de arriba reintenta. Poner
// una cola de verdad aqui seria construir para un problema que este
// sistema todavia no tiene.
// ---------------------------------------------------------------------
// Sube a true cuando ya hay direccion (la del router o la de
// reserva). Hasta entonces la pila escucha, pero no es nadie todavia.
static bool red_lista = false;

static uint8_t buf_salida[1536] __attribute__((aligned(4)));

static struct {
    bool     lleno;
    uint8_t  destino[4];      // a quien iba (no el salto siguiente)
    uint8_t  proto;
    uint32_t len;
    uint64_t ms;
    uint8_t  carga[1400];
} espera;

static bool misma_subred(const uint8_t ip[4]) {
    if (mi_mascara[0] == 0) return true;   // sin mascara, todo es local
    for (int i = 0; i < 4; i++)
        if ((ip[i] & mi_mascara[i]) != (mi_ip[i] & mi_mascara[i])) return false;
    return true;
}

// Una direccion de DIFUSION: la que va a todas las maquinas de la red.
// Hay dos formas de escribirla y las dos valen:
//   255.255.255.255  -- "a todos", sin saber ni en que red estas
//   192.168.1.255    -- "a todos los de mi red", con los bits de host a uno
//
// Hace falta distinguirlas porque una difusion NO se pregunta por ARP: no
// hay una maquina con esa direccion a la que preguntarle su MAC. Sin esto,
// salto_siguiente decidia que 255.255.255.255 no es de mi subred, la
// mandaba a la pasarela, y el router la tiraba -- o sea que dos Nemo OS en
// la misma red no podian encontrarse, y habria que teclear la IP del otro
// a mano para jugar. Es el primer muro que te encuentras montando un
// multijugador.
static bool es_difusion(const uint8_t ip[4]) {
    if (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255) return true;
    if (mi_mascara[0] == 0) return false;      // sin mascara no hay "mi red"
    if (!misma_subred(ip)) return false;
    // Los bits que la mascara deja libres (los de host), todos a uno.
    for (int i = 0; i < 4; i++)
        if ((uint8_t)(ip[i] | mi_mascara[i]) != 0xFF) return false;
    return true;
}

// La direccion que se usa cuando NADIE reparte IPs.
//
// Era una constante, la misma para todas las placas. Con router no
// importaba --el DHCP da direcciones distintas-- pero la IP de reserva
// existe precisamente para el caso contrario: CABLE DIRECTO entre dos
// maquinas, sin router. Y ese es justo el escenario de una partida a
// dos: dos Nemo OS unidos por un cable cogerian LA MISMA direccion.
//
// Y el sintoma seria de los malos. Un programa que se reconoce a si
// mismo comparando la IP del remitente con la propia --como hay que
// hacer, porque un mensaje por difusion vuelve a quien lo manda--
// descartaria los paquetes del OTRO jugador tomandolos por su eco. Sin
// error, sin traza: simplemente no se ven.
//
// Asi que se saca de la MAC, que es distinta en cada placa de fabrica.
// En una Raspberry Pi los tres primeros bytes son iguales para todas
// (el fabricante) y los TRES ULTIMOS son del aparato: los tres se
// mezclan en un solo numero y de ahi salen los dos bytes de la IP.
//
// POR QUE LOS TRES Y NO SOLO LOS DOS ULTIMOS. La primera version hacia
// 'mac[4] % 254' y 'mac[5] % 254', y su propia prueba la delato: una MAC
// terminada en 00:00 y otra en fe:fe daban LA MISMA direccion, porque
// 0 y 254 dan el mismo resto. Mezclando los tres bytes primero, la
// pareja (byte alto, byte bajo) es distinta para 64516 valores
// consecutivos del numero mezclado -- y consecutivos es justo lo que
// son las MAC de dos placas compradas juntas, que es el caso que de
// verdad va a pasar.
//
// Se queda dentro de 169.254.1.1 - 169.254.254.254, que es lo que
// RFC 3927 deja usar: el primer y el ultimo /24 del rango estan
// reservados, y ningun byte se pone a 0 ni a 255.
//
// La mascara NO se toca: sigue a cero, o sea "todo cuenta como local".
// Ponerle 255.255.0.0 seria mas correcto sobre el papel y rompería el
// caso de un Mac con una direccion fija en otro rango, que hoy
// funciona.
static void ip_de_reserva(const uint8_t mac[6], uint8_t out[4]) {
#ifdef NEMO_QEMU
    // Red de usuario de QEMU (slirp): la subred es 10.0.2.0/24 y el
    // invitado se espera en 10.0.2.15 -- es a donde `hostfwd` reenvia
    // las conexiones del host, y por eso aqui NO se calcula nada. El
    // ping desde el host no funciona con slirp (no reenvia ICMP); TCP
    // si. Ademas slirp TRAE SERVIDOR DHCP y reparte precisamente esta
    // direccion, asi que esta reserva casi nunca se usa: QEMU es el
    // banco de pruebas del cliente DHCP, sin placa y sin router.
    //
    // Consecuencia que conviene saber: dos QEMU unidos por
    // "-netdev socket" (sin DHCP) SI chocarian. Para probar dos
    // maquinas de verdad hacen falta dos placas.
    (void)mac;
    out[0] = 10; out[1] = 0; out[2] = 2; out[3] = 15;
#else
    uint32_t h = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
    out[0] = 169;
    out[1] = 254;
    out[2] = (uint8_t)(1 + (h / 254) % 254);
    out[3] = (uint8_t)(1 + h % 254);
#endif
}

// A quien hay que entregarle FISICAMENTE el paquete.
static bool salto_siguiente(const uint8_t destino[4], uint8_t salto[4]) {
    if (misma_subred(destino)) {
        for (int i = 0; i < 4; i++) salto[i] = destino[i];
        return true;
    }
    if (mi_pasarela[0] == 0) return false;   // fuera de la subred y sin pasarela: no hay por donde
    for (int i = 0; i < 4; i++) salto[i] = mi_pasarela[i];
    return true;
}

static bool enviar_con_mac(const uint8_t mac[6], const uint8_t destino[4],
                           uint8_t proto, const uint8_t *carga, uint32_t len) {
    if (ETH_HDR_LEN + IP_MIN_HDR_LEN + len > sizeof buf_salida) return false;

    uint8_t *e = buf_salida;
    for (int i = 0; i < 6; i++) e[i] = mac[i];
    for (int i = 0; i < 6; i++) e[6 + i] = mi_mac[i];
    escribir16(e + 12, ETHERTYPE_IPV4);

    uint8_t *ip = buf_salida + ETH_HDR_LEN;
    ip[0] = 0x45; ip[1] = 0;
    escribir16(ip + 2, (uint16_t)(IP_MIN_HDR_LEN + len));
    // Identificador distinto en cada paquete. Solo importa para
    // reensamblar fragmentos, y aqui nunca fragmentamos, pero mandarlo
    // siempre a cero es de las cosas que hacen que un cortafuegos te
    // mire raro.
    static uint16_t id = 1;
    escribir16(ip + 4, id++);
    escribir16(ip + 6, 0x4000);   // no fragmentar
    ip[8] = 64;                   // TTL
    ip[9] = proto;
    escribir16(ip + 10, 0);
    for (int i = 0; i < 4; i++) ip[12 + i] = mi_ip[i];
    for (int i = 0; i < 4; i++) ip[16 + i] = destino[i];
    escribir16(ip + 10, ip_checksum(ip, IP_MIN_HDR_LEN));

    for (uint32_t i = 0; i < len; i++) ip[IP_MIN_HDR_LEN + i] = carga[i];
    return nic_send(buf_salida, ETH_HDR_LEN + IP_MIN_HDR_LEN + len);
}

bool net_enviar_ipv4(const uint8_t destino[4], uint8_t proto,
                     const uint8_t *carga, uint32_t len) {
    if (!red_lista || len > sizeof espera.carga) return false;

    // Difusion: derecha a la MAC de difusion, sin ARP. Y sin esperar a
    // nadie, asi que esto SI puede decir que si a la primera.
    if (es_difusion(destino)) {
        static const uint8_t a_todos[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        return enviar_con_mac(a_todos, destino, proto, carga, len);
    }

    uint8_t salto[4], mac[6];
    if (!salto_siguiente(destino, salto)) return false;

    uint64_t ms = timer_get_ticks() * 10;
    if (arp_buscar(salto, mac, ms)) return enviar_con_mac(mac, destino, proto, carga, len);

    // No sabemos su MAC: preguntar y guardar el paquete. Se devuelve
    // false porque NO se ha enviado -- quien llama decide si reintenta.
    // Decirle que si habria sido mentira, y de las que se pagan caras.
    espera.lleno = true;
    for (int i = 0; i < 4; i++) espera.destino[i] = destino[i];
    espera.proto = proto;
    espera.len = len;
    espera.ms = ms;
    for (uint32_t i = 0; i < len; i++) espera.carga[i] = carga[i];

    uint32_t n = arp_construir_peticion(buf_salida, mi_mac, mi_ip, salto);
    nic_send(buf_salida, n);
    return false;
}

// Si habia un paquete esperando y ya sabemos la MAC, sale ahora. Se
// llama tras procesar cada trama, que es cuando puede haber llegado la
// respuesta ARP que faltaba.
static void reintentar_espera(void) {
    if (!espera.lleno) return;
    uint64_t ms = timer_get_ticks() * 10;

    // Un segundo esperando es que nadie va a contestar. Tirarlo es
    // mejor que guardarlo para siempre: quien lo mando ya lo dio por
    // perdido hace rato.
    if (ms - espera.ms > 1000) { espera.lleno = false; return; }

    uint8_t salto[4], mac[6];
    if (!salto_siguiente(espera.destino, salto)) { espera.lleno = false; return; }
    if (!arp_buscar(salto, mac, ms)) return;

    espera.lleno = false;
    enviar_con_mac(mac, espera.destino, espera.proto, espera.carga, espera.len);
}

// Le pasa al cliente DHCP lo que llego y manda lo que conteste.
static uint8_t carga_dhcp[600];
static void atender_dhcp(const uint8_t *carga, uint32_t len) {
    uint32_t n = dhcp_recibir(carga, len, timer_get_ticks() * 10,
                              carga_dhcp, sizeof carga_dhcp);
    if (n) enviar_dhcp(carga_dhcp, n);
}

static uint32_t manejar_ipv4(const uint8_t *frame, uint32_t len, uint8_t *salida) {
    if (len < ETH_HDR_LEN + IP_MIN_HDR_LEN) return 0;
    const uint8_t *ip = frame + ETH_HDR_LEN;

    uint8_t version = (uint8_t)(ip[0] >> 4);
    uint32_t ihl = (uint32_t)(ip[0] & 0x0F) * 4;
    if (version != 4 || ihl < IP_MIN_HDR_LEN || ihl > 60) return 0;
    if (len < ETH_HDR_LEN + ihl) return 0;

    if (ip_checksum(ip, ihl) != 0) {
        uart_puts("net: IPv4 -- checksum de cabecera invalido, descartada\n");
        return 0;
    }

    const uint8_t *ip_destino = ip + 16;
    uint8_t protocolo = ip[9];
    uint32_t ip_total_len = leer16(ip + 2);
    if (ip_total_len < ihl || ETH_HDR_LEN + ip_total_len > len) return 0;

    bool para_mi = true;
    for (int i = 0; i < 4; i++) if (ip_destino[i] != mi_ip[i]) { para_mi = false; break; }

    // Mientras se negocia la IP hay que aceptar cosas que en cualquier
    // otro momento se descartarian: las respuestas del servidor DHCP
    // vienen a la direccion de difusion, porque cuando preguntamos
    // todavia no teniamos ninguna direccion propia a la que
    // contestarnos. Esta puerta se abre SOLO para eso: solo UDP, solo
    // al puerto 68, y solo mientras la negociacion esta en marcha.
    if (!para_mi) {
        bool negociando = (dhcp_estado() == DHCP_BUSCANDO || dhcp_estado() == DHCP_PIDIENDO);
        // La otra cosa que llega sin ir dirigida a nosotros y SI nos
        // interesa: un UDP por difusion. Es como una maquina
        // dice "hay partida aqui" a una red en la que todavia no conoce a
        // nadie. Solo UDP, y solo si de verdad es una direccion de
        // difusion -- no cualquier paquete que pase por delante.
        bool difusion_udp = (protocolo == UDP_PROTO && es_difusion(ip_destino));
        if (!difusion_udp && !(negociando && protocolo == UDP_PROTO)) return 0;
    }

    if (protocolo == UDP_PROTO) {
        const uint8_t *carga; uint32_t carga_len; uint16_t sp, dp;
        if (!udp_desempaquetar(ip + ihl, ip_total_len - ihl, ip + 12, ip_destino,
                               &sp, &dp, &carga, &carga_len)) return 0;
        if (dp == DHCP_PUERTO_CLIENTE) {
            // La respuesta del cliente DHCP (el REQUEST) no puede salir
            // por el camino normal: ese construye la contestacion
            // reflejando las cabeceras del paquete recibido, y un DHCP
            // se manda a gritos, no de vuelta a quien escribio. Se
            // envia aqui mismo, con su propio buffer, y esta funcion
            // devuelve 0 para que nadie mande nada mas.
            atender_dhcp(carga, carga_len);
            return 0;
        }
        // Y lo demas, a los sockets de los programas. El
        // orden importa: el DHCP se atiende ANTES, porque su puerto se
        // reserva y udps_abrir(68) se rechaza -- si se preguntara primero
        // a los sockets, un programa podria quedarse con las respuestas
        // del router.
        //
        // Se acepta tambien lo que venga por difusion, y por eso este
        // trozo esta ANTES del "de aqui en adelante, solo lo nuestro":
        // asi es como una maquina anuncia "hay partida aqui" a toda la red
        // sin conocer a nadie todavia.
        if (para_mi || es_difusion(ip_destino))
            udps_entregar(dp, ip + 12, sp, carga, carga_len);
        return 0;   // los sockets no contestan solos: contesta el programa
    }

    if (!para_mi) return 0;   // de aqui en adelante, solo lo nuestro

    if (protocolo == TCP_PROTO) {
        const uint8_t *seg0 = ip + ihl;
        if (ip_total_len - ihl >= 4) {
            uint16_t sport = leer16(seg0 + 0), dport = leer16(seg0 + 2);
            // Dos mitades de TCP conviven: la que ESCUCHA (tcp.c, la
            // shell en el 2323) y la que CONECTA (tcp_cliente.c, el
            // navegador). Se reparten por puerto e IP, y se pregunta
            // primero al cliente para no robarle sus segmentos a la
            // shell ni al reves.
            if (tcpcli_es_mio(dport, sport, ip + 12)) {
                tcpcli_manejar(seg0, ip_total_len - ihl, ip + 12, ip_destino,
                               timer_get_ticks() * 10);
                return 0;   // el cliente envia por su cuenta, no por aqui
            }
        }
    }

    if (protocolo == TCP_PROTO) {
        // TCP (ver tcp.c): el segmento va detras de la cabecera IP. La
        // respuesta TCP se construye en el sitio exacto de 'salida' donde
        // ira la carga, y luego se ponen las cabeceras delante -- asi no
        // hay copia intermedia.
        const uint8_t *seg = ip + ihl;
        uint32_t seg_len = ip_total_len - ihl;
        uint8_t *carga = salida + ETH_HDR_LEN + IP_MIN_HDR_LEN;
        uint32_t tcp_len = tcp_manejar(seg, seg_len, ip + 12, ip + 16, carga, 1536 - ETH_HDR_LEN - IP_MIN_HDR_LEN);
        if (tcp_len == 0) return 0;
        construir_cabeceras_respuesta(frame, salida, TCP_PROTO, tcp_len);
        return ETH_HDR_LEN + IP_MIN_HDR_LEN + tcp_len;
    }

    // Cualquier otra cosa. UDP y TCP ya se atendieron mas arriba, asi
    // que aqui solo puede caer un protocolo que no conocemos: se
    // descarta sin mas. No somos un router y no tenemos nada que
    // contestar en su nombre.
    if (protocolo != IP_PROTO_ICMP) return 0;
    if (ip_total_len < ihl + ICMP_HDR_LEN) return 0;

    const uint8_t *icmp = ip + ihl;
    if (icmp[0] != ICMP_ECHO_REQUEST) return 0; // solo respondemos a ping

    uint32_t icmp_len = ip_total_len - ihl;
    if (ip_checksum(icmp, icmp_len) != 0) {
        uart_puts("net: ICMP -- checksum invalido, descartado\n");
        return 0;
    }

    uart_puts("net: ping recibido de "); uart_put_ip(ip + 12); uart_puts(" -- respondiendo\n");
    return responder_ping(frame, len, salida);
}

// ---------------------------------------------------------------------
// Interfaz publica
// ---------------------------------------------------------------------
static uint8_t buf_rx[1536] __attribute__((aligned(4)));
static uint8_t buf_tx[1536] __attribute__((aligned(4)));

// ARP "gratuito" (RFC 5227): un anuncio a toda la red de "esta IP es
// de esta MAC", sin que nadie lo haya preguntado. Lo hace cualquier
// sistema al levantar el enlace. Aqui sirve para que el Mac tenga ya
// la asociacion en su tabla ARP antes de la primera conexion -- justo
// tras un arranque de la Pi, macOS pasa por una ventana rara en la que
// el primer SYN sale mal formado (checksum sin completar por el
// adaptador); quitarle la resolucion ARP del camino critico reduce lo
// que tiene que hacer en ese instante. Es solo una ayuda: si el Mac
// sigue mandando un SYN roto, se descarta igual y el reintenta.
static void enviar_arp_gratuito(void) {
    uint8_t *e = buf_tx;
    for (int i = 0; i < 6; i++) e[i] = 0xFF;               // broadcast
    for (int i = 0; i < 6; i++) e[6 + i] = mi_mac[i];
    escribir16(e + 12, ETHERTYPE_ARP);
    uint8_t *a = buf_tx + ETH_HDR_LEN;
    escribir16(a + 0, ARP_HTYPE_ETH);
    escribir16(a + 2, ARP_PTYPE_IP);
    a[4] = 6; a[5] = 4;
    escribir16(a + 6, ARP_OP_REQUEST);                     // anuncio: request con sender==target
    for (int i = 0; i < 6; i++) a[8 + i] = mi_mac[i];
    for (int i = 0; i < 4; i++) a[14 + i] = mi_ip[i];
    for (int i = 0; i < 6; i++) a[18 + i] = 0;
    for (int i = 0; i < 4; i++) a[24 + i] = mi_ip[i];
    nic_send(buf_tx, ETH_HDR_LEN + ARP_LEN);
}

// Segunda mitad del arranque: se ejecuta cuando ya hay direccion, sea
// la que dio el router o la de reserva. Hasta aqui la pila esta
// escuchando pero no es nadie todavia.
static void terminar_de_arrancar(void) {
    red_lista = true;
    uart_puts("net: listo, IP = "); uart_put_ip(mi_ip);
    if (mi_mascara[0]) { uart_puts(" mascara "); uart_put_ip(mi_mascara); }
    if (mi_pasarela[0]) { uart_puts(" pasarela "); uart_put_ip(mi_pasarela); }
    if (mi_dns[0]) { uart_puts(" DNS "); uart_put_ip(mi_dns); }
    uart_puts(", MAC = "); uart_put_mac(mi_mac); uart_puts("\n");
    enviar_arp_gratuito();
    tcp_escuchar(NET_PUERTO_SHELL);

    // Volcar la bitacora OTRA VEZ.
    //
    // El volcado del arranque ocurre en kernel.c, y eso basta mientras todo
    // lo que haya que diagnosticar pase antes. La red no cumple eso:
    // negociar una IP lleva segundos y termina mucho despues de que
    // el kernel haya escrito NEMO.LOG y siga con lo suyo. El resultado
    // era un registro que se cortaba justo en
    // "preguntando al router por una IP" y no decia nunca como acabo --
    // precisamente la linea que hacia falta leer.
    //
    // Esta es la primera vez que algo interesante se decide DESPUES del
    // arranque, y no sera la ultima: el volcado deja de ser un punto
    // final del arranque y pasa a ser algo que se repite cuando hay
    // novedades que contar.
    bitacora_volcar();
}

// Si la negociacion ya esta en marcha. Hace falta porque el enlace
// puede subir DESPUES de net_init (se enchufa el cable con la placa ya
// encendida): en ese caso nadie habria llamado a dhcp_iniciar, y el
// cliente saldria a preguntar con la MAC sin rellenar.
static bool dhcp_arrancado = false;

static void empezar_a_pedir_ip(void) {
    nic_get_mac(mi_mac);
    uart_puts("net: preguntando al router por una IP (DHCP)...\n");
    dhcp_iniciar(mi_mac, (uint32_t)timer_get_ticks());
    dhcp_arrancado = true;
}

void net_init(void) {
    red_lista = false;      // todavia no: primero hay que tener direccion
    udps_init();
    if (!nic_link_up()) return;
    empezar_a_pedir_ip();
}

// Manda un datagrama UDP. Vive aqui y no en udp_sock.c porque
// necesita mi_ip para la pseudo-cabecera de la suma de comprobacion, y esa
// es de net.c; udp_sock.c se queda sin saber que existe una red, que es lo
// que permite probarlo entero en el Mac.
//
// Devuelve false si no salio -- tipicamente porque falta resolver la MAC
// del destino por ARP, y entonces hay que volver a llamar. Igual que
// net_enviar_ipv4, y por el mismo motivo: decir que si sin haber enviado
// nada es la clase de mentira que se paga cara.
bool net_enviar_udp(const uint8_t destino[4], uint16_t puerto_destino,
                    uint16_t puerto_origen, const uint8_t *carga, uint32_t len) {
    static uint8_t buf[UDP_HDR_LEN + UDPS_MAX_DATAGRAMA];
    if (len > UDPS_MAX_DATAGRAMA) return false;
    for (uint32_t i = 0; i < len; i++) buf[UDP_HDR_LEN + i] = carga[i];
    uint32_t n = udp_empaquetar(buf, len, mi_ip, destino,
                                puerto_origen, puerto_destino);
    return net_enviar_ipv4(destino, UDP_PROTO, buf, n);
}

// Lleva la negociacion adelante y decide con que direccion se arranca.
// Devuelve true cuando ya hay uno u otro desenlace.
static bool dhcp_avanzar(void) {
    uint64_t ms = timer_get_ticks() * 10;
    uint32_t n = dhcp_tick(ms, carga_dhcp, sizeof carga_dhcp);
    if (n) enviar_dhcp(carga_dhcp, n);

    if (dhcp_estado() == DHCP_LISTO) {
        dhcp_config(mi_ip, mi_mascara, mi_pasarela, mi_dns);
        uart_puts("net: el router dio una IP por DHCP\n");
        terminar_de_arrancar();
        return true;
    }
    if (dhcp_estado() == DHCP_RENDIDO) {
        ip_de_reserva(mi_mac, mi_ip);
        uart_puts("net: nadie reparte IPs aqui -- usando la de reserva "
                  "(cable directo a otro ordenador, que es justo para lo que esta)\n");
        // Se dice cual, porque es la unica forma de saber que dos placas
        // unidas por un cable cogieron direcciones DISTINTAS.
        uart_puts("net: reserva = ");
        uart_put_ip(mi_ip);
        uart_puts(" (sacada de la MAC ");
        uart_put_mac(mi_mac);
        uart_puts(")\n");
        terminar_de_arrancar();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------
// Vigilancia de los primeros dos minutos
//
// Se anade para un caso concreto: la placa dice tener el enlace arriba
// a 1 Gb/s y un ping desde otro ordenador no obtiene respuesta. Desde
// arriba, "no llega nada" y "llega y lo perdemos" se ven exactamente
// igual, y se arreglan en sitios distintos. Esto los separa.
//
// Cada cinco segundos, y SOLO durante los dos primeros minutos, se
// apunta lo que ha pasado y se vuelca el registro a la tarjeta. El
// limite de dos minutos no es pereza: cada volcado es una escritura en
// la SD, y dejar eso corriendo para siempre castiga la tarjeta de todo
// el que instale el sistema a cambio de un dato que solo interesa
// mientras se depura.
// El registro a la tarjeta solo durante los dos primeros minutos: cada
// volcado es una escritura en la SD, y dejarlo corriendo para siempre
// castiga la tarjeta de todo el que instale el sistema. Por el UART se
// informa siempre, que no cuesta nada -- en QEMU es lo que se lee.
#define VOLCADO_HASTA_MS 120000
#define INFORME_CADA_MS    2000
#define SILENCIO_CADA_MS  30000

static uint32_t n_tramas = 0, n_arp = 0, n_ipv4 = 0, n_otras = 0;
static uint32_t n_enviadas = 0, n_fallos_envio = 0;
static uint64_t n_bytes_rx = 0, n_bytes_tx = 0;

static uint64_t ms_ultimo_informe = 0;
static uint32_t tramas_en_corte = 0;
static uint64_t bytes_en_corte = 0;
static uint32_t pico_tramas_s = 0, pico_kbs = 0;
static uint64_t ms_ultimo_silencio = 0;

// Informa de la VELOCIDAD, no solo del total. Un total acumulado no
// dice si la red va rapida o lenta; lo que hace falta para decidir
// donde esta el cuello de botella son tramas por segundo y KB por
// segundo, medidos sobre un intervalo corto.
//
// El pico se guarda aparte porque una transferencia dura poco y es
// facil perderse la linea buena mirando el terminal.
static void vigilar(void) {
    uint64_t ms = timer_get_ticks() * 10;

    if (ms_ultimo_informe == 0) {   // primera vez: solo fijar el punto de partida
        ms_ultimo_informe = ms;
        ms_ultimo_silencio = ms;
        tramas_en_corte = n_tramas;
        bytes_en_corte = n_bytes_rx;
        return;
    }

    uint64_t dt = ms - ms_ultimo_informe;
    if (dt < INFORME_CADA_MS) return;

    uint32_t tramas = n_tramas - tramas_en_corte;
    uint64_t bytes  = n_bytes_rx - bytes_en_corte;
    ms_ultimo_informe = ms;
    tramas_en_corte = n_tramas;
    bytes_en_corte = n_bytes_rx;

    if (tramas == 0) {
        // Silencio. Se informa de vez en cuando igualmente, y con los
        // contadores del propio chip: un "no ha llegado nada" con el
        // hardware delante es un dato, no una falta de dato. Asi se
        // distingue "nadie manda" de "llega y lo perdemos".
        if (ms > VOLCADO_HASTA_MS) return;
        if (ms - ms_ultimo_silencio < SILENCIO_CADA_MS) return;
        ms_ultimo_silencio = ms;
        uart_puts("net: a los "); uart_put_dec((uint32_t)(ms / 1000));
        uart_puts(" s -- sin trafico. Totales: recibidas "); uart_put_dec(n_tramas);
        uart_puts(", enviadas "); uart_put_dec(n_enviadas);
        uart_puts("\n");
        nic_diagnostico();
        bitacora_volcar();
        return;
    }

    uint32_t tramas_s = (uint32_t)((uint64_t)tramas * 1000u / dt);
    uint32_t kbs      = (uint32_t)(bytes * 1000u / dt / 1024u);
    if (tramas_s > pico_tramas_s) pico_tramas_s = tramas_s;
    if (kbs > pico_kbs) pico_kbs = kbs;

    uart_puts("net: "); uart_put_dec(tramas_s); uart_puts(" tramas/s, ");
    uart_put_dec(kbs); uart_puts(" KB/s  (pico ");
    uart_put_dec(pico_tramas_s); uart_puts(" tramas/s, ");
    uart_put_dec(pico_kbs); uart_puts(" KB/s)  total recibido ");
    uart_put_dec((uint32_t)(n_bytes_rx / 1024)); uart_puts(" KB, enviado ");
    uart_put_dec((uint32_t)(n_bytes_tx / 1024)); uart_puts(" KB\n");

    ms_ultimo_silencio = ms;
    if (ms < VOLCADO_HASTA_MS) bitacora_volcar();
}

// ---------------------------------------------------------------------
// Cuantas tramas se atienden por vuelta
//
// Cuantas tramas se atienden por vuelta. Con UNA, y sabiendo que net_poll()
// lo llama el turno del kernel 100 veces por segundo (RED_POR_SEGUNDO, en
// tasks.c), el techo son 100 tramas por segundo: unos 146 KB/s como maximo
// absoluto, con un enlace Gigabit debajo. El cable no tiene nada que ver.
//
// Ahora se vacia el anillo: se leen tramas hasta que no quede ninguna,
// con un tope por vuelta. El tope existe para que una rafaga de trafico
// no se coma el turno y deje al escritorio sin repintar -- la red es
// importante, pero no mas que el raton. 64 tramas por vuelta a 100 Hz
// son 6400 tramas/s, unos 9 MB/s: de sobra para lo que viene, y sin
// riesgo de acaparar.
//
// Es el mismo patron que ya usa el sondeo del USB, y la razon de fondo
// es la misma: el hardware acumula trabajo en un anillo, y quien lo
// atiende tiene que vaciarlo, no coger una pieza y marcharse.
//
// Para MEDIR el antes y el despues no hace falta otra version del
// archivo: poner este numero a 1 reproduce exactamente el
// comportamiento viejo, una trama por vuelta.
//
// Detalle del contrato de nic_recv(): devuelve 0 tanto si el anillo
// esta vacio como si la trama que habia no tenia carga util, asi que
// una trama vacia corta el vaciado antes de tiempo. No es un problema
// --la vuelta siguiente, 10 ms despues, sigue por donde iba-- pero
// conviene saberlo antes de perseguirlo como si fuera un fallo.
#define MAX_TRAMAS_POR_VUELTA 64

void net_poll(void) {
    // Mientras se negocia hay que seguir leyendo tramas -- las
    // respuestas del servidor llegan por el mismo sitio que todo lo
    // demas-- pero sin responder a nada mas.
    if (!red_lista) {
        if (!nic_link_up()) return;
        if (!dhcp_arrancado) empezar_a_pedir_ip();   // el cable llego tarde
        dhcp_avanzar();
        for (uint32_t i = 0; i < MAX_TRAMAS_POR_VUELTA; i++) {
            uint32_t nl = nic_recv(buf_rx, sizeof(buf_rx));
            if (nl == 0) break;
            n_tramas++; n_bytes_rx += nl;
            if (nl < ETH_HDR_LEN) continue;
            uint16_t et = leer16(buf_rx + 12);
            if (et == ETHERTYPE_IPV4) { n_ipv4++; manejar_ipv4(buf_rx, nl, buf_tx); }
            else if (et == ETHERTYPE_ARP) n_arp++;
            else n_otras++;
        }
        vigilar();
        return;
    }

    // El lado que conecta necesita que le den turno: manda el SYN y lo
    // reintenta. Nadie le va a hablar primero, asi que sin esto no
    // arrancaria nunca.
    tcpcli_tick(timer_get_ticks() * 10);
    descarga_tick(timer_get_ticks() * 10);    // vacia lo que baje por HTTP

    for (uint32_t i = 0; i < MAX_TRAMAS_POR_VUELTA; i++) {
        uint32_t len = nic_recv(buf_rx, sizeof(buf_rx));
        if (len == 0) break;                 // el anillo esta vacio: hemos terminado
        n_tramas++; n_bytes_rx += len;
        if (len < ETH_HDR_LEN) continue;     // trama enana: tirarla y SEGUIR vaciando

        uint16_t ethertype = leer16(buf_rx + 12);
        uint32_t resp_len = 0;

        if (ethertype == ETHERTYPE_ARP) {
            n_arp++;
            resp_len = manejar_arp(buf_rx, len, buf_tx);
        } else if (ethertype == ETHERTYPE_IPV4) {
            n_ipv4++;
            resp_len = manejar_ipv4(buf_rx, len, buf_tx);
        } else {
            n_otras++;
        }

        if (resp_len > 0) {
            if (nic_send(buf_tx, resp_len)) {
                n_enviadas++; n_bytes_tx += resp_len;
            } else {
                n_fallos_envio++;
                uart_puts("net: nic_send FALLO (el hardware no confirmo la transmision), ");
                uart_put_dec(resp_len); uart_puts(" bytes perdidos\n");
            }
        }

        // Puede que esa trama fuera justo la respuesta ARP que
        // esperabamos. Si habia un paquete guardado, sale ahora.
        reintentar_espera();
    }
    vigilar();
}

void net_get_ip(uint8_t out[4])       { for (int i = 0; i < 4; i++) out[i] = mi_ip[i]; }
void net_get_mascara(uint8_t out[4])  { for (int i = 0; i < 4; i++) out[i] = mi_mascara[i]; }
void net_get_pasarela(uint8_t out[4]) { for (int i = 0; i < 4; i++) out[i] = mi_pasarela[i]; }
void net_get_dns(uint8_t out[4])      { for (int i = 0; i < 4; i++) out[i] = mi_dns[i]; }
bool net_ip_por_dhcp(void) { return dhcp_estado() == DHCP_LISTO; }
bool net_ready(void) { return red_lista; }
