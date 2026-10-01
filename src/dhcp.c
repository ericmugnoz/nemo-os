// dhcp.c -- Nemo OS
// Ver dhcp.h para el porque y la forma de la conversacion.
//
// El formato del mensaje viene de BOOTP, que es de 1985, y se nota:
// 236 bytes de campos fijos en los que hoy casi todo va a cero, y
// detras las "opciones", que es donde esta de verdad la informacion.
// Las opciones empiezan por cuatro bytes magicos (99, 130, 83, 99 --
// 0x63825363) que distinguen un DHCP de un BOOTP de los de antes.
//
//   0   op      1 = peticion (cliente), 2 = respuesta (servidor)
//   1   htype   1 = Ethernet
//   2   hlen    6 = longitud de una MAC
//   3   hops    0
//   4   xid     identificador de ESTA conversacion, 4 bytes
//   8   secs    segundos desde que empece a pedir
//   10  flags   bit 15 = "contestame a gritos, aun no tengo IP"
//   12  ciaddr  mi IP actual (cero mientras no tengo)
//   16  yiaddr  "tu IP" -- la que el servidor me asigna
//   20  siaddr  IP del siguiente servidor (arranque por red; no se usa)
//   24  giaddr  IP del agente de reenvio (no se usa)
//   28  chaddr  mi MAC, rellenada con ceros hasta 16 bytes
//   44  sname   64 bytes de nombre de servidor (cero)
//   108 file    128 bytes de nombre de archivo de arranque (cero)
//   236 cookie  los cuatro bytes magicos
//   240 opciones, cada una: [codigo][longitud][datos...], fin con 255
//
// Cada opcion se identifica por un numero. Las que importan aqui:
//
//   1   mascara de subred      51  duracion del prestamo
//   3   pasarela (router)      53  tipo de mensaje (1=DISCOVER...)
//   6   servidores DNS         54  quien es el servidor que contesta
//   55  lista de lo que pido   61  quien soy yo
//
// Un detalle que muerde: hay servidores que exigen que el mensaje mida
// al menos 300 bytes, porque el BOOTP original era de tamano fijo. Se
// rellena con ceros hasta ahi.

#include "dhcp.h"

#define DHCP_FIJO_LEN   236
#define DHCP_MIN_LEN    300

#define OP_PETICION     1
#define OP_RESPUESTA    2

#define MSG_DISCOVER    1
#define MSG_OFFER       2
#define MSG_REQUEST     3
#define MSG_ACK         5
#define MSG_NAK         6

#define OPT_MASCARA     1
#define OPT_PASARELA    3
#define OPT_DNS         6
#define OPT_IP_PEDIDA   50
#define OPT_PRESTAMO    51
#define OPT_TIPO        53
#define OPT_SERVIDOR    54
#define OPT_LISTA       55
#define OPT_CLIENTE     61
#define OPT_FIN         255

// Reintentos: cada 2 segundos, cuatro veces. Ocho segundos es de
// sobra para cualquier router domestico; mas que eso y lo que hay es
// que no hay servidor, no que sea lento. Pasado el tope, RENDIDO, y
// net.c vuelve a la direccion de reserva -- que es justamente el caso de
// la placa conectada directa a un Mac, donde no hay ningun DHCP y
// tampoco hace falta.
#define REINTENTO_MS    2000
#define MAX_INTENTOS    4

static dhcp_estado_t estado = DHCP_PARADO;
static uint8_t  mi_mac[6];
static uint32_t xid;
static uint64_t ms_inicio;
static uint64_t ms_ultimo_envio;
static int      intentos;

static uint8_t ip_ofrecida[4];
static uint8_t servidor[4];
static uint8_t cfg_ip[4], cfg_mascara[4], cfg_pasarela[4], cfg_dns[4];
static uint32_t prestamo_s;

static void poner32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t sacar32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static void cuatro(uint8_t *d, const uint8_t *o) { for (int i = 0; i < 4; i++) d[i] = o[i]; }
static void cero(uint8_t *p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = 0; }

// ---------------------------------------------------------------------
// Construir un mensaje. DISCOVER y REQUEST se diferencian en tres
// cosas: el tipo, y si llevan o no la IP pedida y el servidor elegido.
// ---------------------------------------------------------------------
static uint32_t construir(uint8_t tipo, uint64_t ms, uint8_t *out, uint32_t max) {
    if (max < DHCP_MIN_LEN) return 0;
    cero(out, DHCP_MIN_LEN);

    out[0] = OP_PETICION;
    out[1] = 1;              // Ethernet
    out[2] = 6;              // seis bytes de MAC
    out[3] = 0;              // hops
    poner32(out + 4, xid);

    uint32_t segs = (uint32_t)((ms - ms_inicio) / 1000);
    if (segs > 0xFFFF) segs = 0xFFFF;
    out[8] = (uint8_t)(segs >> 8); out[9] = (uint8_t)segs;

    // Bit de difusion. Sin el, el servidor contesta a la IP que acaba
    // de asignarme -- que yo TODAVIA NO TENGO configurada, asi que la
    // descartaria. Windows y Linux se la juegan porque su pila acepta
    // ese caso especial; aqui se pide a gritos y punto.
    out[10] = 0x80; out[11] = 0x00;

    for (int i = 0; i < 6; i++) out[28 + i] = mi_mac[i];

    poner32(out + 236, 0x63825363);   // los cuatro bytes magicos

    uint8_t *o = out + 240;
    *o++ = OPT_TIPO;    *o++ = 1; *o++ = tipo;

    // Quien soy: el 1 de delante dice "lo que sigue es una MAC de
    // Ethernet". Es lo que hace que el router te reconozca como el
    // mismo aparato entre arranques y te devuelva la misma IP.
    *o++ = OPT_CLIENTE; *o++ = 7; *o++ = 1;
    for (int i = 0; i < 6; i++) *o++ = mi_mac[i];

    if (tipo == MSG_REQUEST) {
        *o++ = OPT_IP_PEDIDA; *o++ = 4; for (int i = 0; i < 4; i++) *o++ = ip_ofrecida[i];
        *o++ = OPT_SERVIDOR;  *o++ = 4; for (int i = 0; i < 4; i++) *o++ = servidor[i];
    }

    // Lo que quiero que me manden. Sin pedirlo, muchos servidores dan
    // solo la IP y se quedan tan anchos -- y sin mascara ni pasarela
    // la IP no sirve para salir de la propia subred.
    *o++ = OPT_LISTA; *o++ = 3;
    *o++ = OPT_MASCARA; *o++ = OPT_PASARELA; *o++ = OPT_DNS;

    *o++ = OPT_FIN;

    uint32_t len = (uint32_t)(o - out);
    return len < DHCP_MIN_LEN ? DHCP_MIN_LEN : len;   // el relleno ya esta a cero
}

// ---------------------------------------------------------------------
// Leer las opciones de una respuesta. Recorrer esto a lo bruto es la
// forma clasica de leer fuera del buffer: la longitud de cada opcion
// la escribe OTRA maquina, asi que se comprueba en cada paso.
// ---------------------------------------------------------------------
static bool opciones_leer(const uint8_t *msg, uint32_t len, uint8_t *tipo_out) {
    *tipo_out = 0;
    if (len < 240) return false;
    if (sacar32(msg + 236) != 0x63825363) return false;

    uint32_t i = 240;
    while (i < len) {
        uint8_t codigo = msg[i];
        if (codigo == OPT_FIN) break;
        if (codigo == 0) { i++; continue; }        // relleno
        if (i + 1 >= len) return false;            // longitud cortada
        uint8_t n = msg[i + 1];
        if (i + 2 + n > len) return false;         // datos cortados
        const uint8_t *d = msg + i + 2;

        switch (codigo) {
            case OPT_TIPO:     if (n >= 1) *tipo_out = d[0]; break;
            case OPT_MASCARA:  if (n >= 4) cuatro(cfg_mascara, d); break;
            case OPT_PASARELA: if (n >= 4) cuatro(cfg_pasarela, d); break;
            // Puede venir mas de un DNS; con el primero basta.
            case OPT_DNS:      if (n >= 4) cuatro(cfg_dns, d); break;
            case OPT_SERVIDOR: if (n >= 4) cuatro(servidor, d); break;
            case OPT_PRESTAMO: if (n >= 4) prestamo_s = sacar32(d); break;
            default: break;
        }
        i += 2 + n;
    }
    return *tipo_out != 0;
}

// ---------------------------------------------------------------------
void dhcp_iniciar(const uint8_t mac[6], uint32_t semilla) {
    for (int i = 0; i < 6; i++) mi_mac[i] = mac[i];
    // El xid solo tiene que ser distinto del de otras conversaciones
    // que anden por la red a la vez. Mezclar la MAC con la semilla da
    // eso sin necesidad de un generador de verdad.
    xid = semilla ^ ((uint32_t)mac[2] << 24) ^ ((uint32_t)mac[3] << 16) ^
                    ((uint32_t)mac[4] << 8)  ^  (uint32_t)mac[5];
    if (xid == 0) xid = 0x4E454D4F;   // "NEMO", por si sale cero
    estado = DHCP_PARADO;
    intentos = 0;
    prestamo_s = 0;
    cero(ip_ofrecida, 4); cero(servidor, 4);
    cero(cfg_ip, 4); cero(cfg_mascara, 4); cero(cfg_pasarela, 4); cero(cfg_dns, 4);
}

uint32_t dhcp_tick(uint64_t ms, uint8_t *salida, uint32_t max) {
    if (estado == DHCP_LISTO || estado == DHCP_RENDIDO) return 0;

    if (estado == DHCP_PARADO) {
        ms_inicio = ms;
        estado = DHCP_BUSCANDO;
        intentos = 1;
        ms_ultimo_envio = ms;
        return construir(MSG_DISCOVER, ms, salida, max);
    }

    if (ms - ms_ultimo_envio < REINTENTO_MS) return 0;

    if (intentos >= MAX_INTENTOS) {
        estado = DHCP_RENDIDO;
        return 0;
    }
    intentos++;
    ms_ultimo_envio = ms;
    // Se reintenta lo que tocara: el DISCOVER si aun no hay oferta, o
    // el REQUEST si la hubo y el ACK se perdio.
    return construir(estado == DHCP_BUSCANDO ? MSG_DISCOVER : MSG_REQUEST, ms, salida, max);
}

uint32_t dhcp_recibir(const uint8_t *carga, uint32_t len, uint64_t ms,
                      uint8_t *salida, uint32_t max) {
    if (estado != DHCP_BUSCANDO && estado != DHCP_PIDIENDO) return 0;
    if (len < DHCP_FIJO_LEN) return 0;

    if (carga[0] != OP_RESPUESTA) return 0;
    // Sin esto, dos maquinas arrancando a la vez en la misma red se
    // roban las respuestas la una a la otra.
    if (sacar32(carga + 4) != xid) return 0;
    for (int i = 0; i < 6; i++) if (carga[28 + i] != mi_mac[i]) return 0;

    uint8_t tipo = 0;
    if (!opciones_leer(carga, len, &tipo)) return 0;

    if (tipo == MSG_OFFER && estado == DHCP_BUSCANDO) {
        cuatro(ip_ofrecida, carga + 16);          // yiaddr
        if (ip_ofrecida[0] == 0 && ip_ofrecida[1] == 0 &&
            ip_ofrecida[2] == 0 && ip_ofrecida[3] == 0) return 0;
        estado = DHCP_PIDIENDO;
        intentos = 1;
        ms_ultimo_envio = ms;
        return construir(MSG_REQUEST, ms, salida, max);
    }

    if (tipo == MSG_ACK && estado == DHCP_PIDIENDO) {
        cuatro(cfg_ip, carga + 16);
        // Un servidor puede confirmar una IP distinta de la ofrecida.
        // Manda la del ACK, no la del OFFER.
        if (cfg_ip[0] == 0 && cfg_ip[1] == 0 && cfg_ip[2] == 0 && cfg_ip[3] == 0)
            cuatro(cfg_ip, ip_ofrecida);
        // Si no mando mascara, deducirla de la clase de la IP es lo que
        // hacia todo el mundo antes de CIDR y sigue acertando en una
        // red domestica.
        if (cfg_mascara[0] == 0) {
            if (cfg_ip[0] < 128)      { cfg_mascara[0] = 255; }
            else if (cfg_ip[0] < 192) { cfg_mascara[0] = 255; cfg_mascara[1] = 255; }
            else                      { cfg_mascara[0] = 255; cfg_mascara[1] = 255; cfg_mascara[2] = 255; }
        }
        estado = DHCP_LISTO;
        return 0;
    }

    if (tipo == MSG_NAK) {
        // El servidor rechaza lo pedido (tipico al cambiar de red con
        // una IP vieja guardada). Se vuelve a empezar desde cero.
        estado = DHCP_PARADO;
        intentos = 0;
        cero(ip_ofrecida, 4);
        return 0;
    }

    return 0;
}

dhcp_estado_t dhcp_estado(void) { return estado; }

void dhcp_config(uint8_t ip[4], uint8_t mascara[4], uint8_t pasarela[4], uint8_t dns[4]) {
    cuatro(ip, cfg_ip); cuatro(mascara, cfg_mascara);
    cuatro(pasarela, cfg_pasarela); cuatro(dns, cfg_dns);
}

const char *dhcp_estado_nombre(void) {
    switch (estado) {
        case DHCP_PARADO:   return "parado";
        case DHCP_BUSCANDO: return "buscando servidor";
        case DHCP_PIDIENDO: return "pidiendo la IP";
        case DHCP_LISTO:    return "listo";
        default:            return "rendido";
    }
}
