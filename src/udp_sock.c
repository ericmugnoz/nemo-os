// udp_sock.c -- Nemo OS. Ver udp_sock.h.

#include "udp_sock.h"

// El puerto del cliente DHCP. Esta escrito aqui y no incluido de dhcp.h a
// proposito: este archivo no depende de nada del sistema, y es lo que
// permite compilarlo tal cual en la prueba del Mac.
#define PUERTO_DHCP_CLIENTE 68

typedef struct {
    uint8_t  datos[UDPS_MAX_DATAGRAMA];
    uint32_t len;
    uint8_t  ip[4];
    uint16_t puerto;
} datagrama_t;

typedef struct {
    bool     abierto;
    uint16_t puerto;
    datagrama_t cola[UDPS_COLA];
    uint32_t cabeza, cuantos;    // cola circular
    uint32_t perdidos;
    // El remitente del ultimo datagrama ENTREGADO A LA APLICACION. Ver la
    // nota larga de udp_sock.h: guardarlo por socket y no por llegada es
    // la diferencia entre contestarle al jugador correcto y al que no era.
    uint8_t  ultimo_ip[4];
    uint16_t ultimo_puerto;
} socket_t;

static socket_t sk[UDPS_MAX_SOCKETS];

void udps_init(void) {
    for (uint32_t i = 0; i < UDPS_MAX_SOCKETS; i++) {
        sk[i].abierto = false;
        sk[i].puerto = 0;
        sk[i].cabeza = sk[i].cuantos = 0;
        sk[i].perdidos = 0;
        sk[i].ultimo_puerto = 0;
        for (int j = 0; j < 4; j++) sk[i].ultimo_ip[j] = 0;
    }
}

static bool valido(int32_t h) {
    return h >= 0 && h < (int32_t)UDPS_MAX_SOCKETS && sk[h].abierto;
}

static bool puerto_ocupado(uint16_t p) {
    for (uint32_t i = 0; i < UDPS_MAX_SOCKETS; i++)
        if (sk[i].abierto && sk[i].puerto == p) return true;
    return false;
}

int32_t udps_abrir(uint16_t puerto) {
    // El 68 es del cliente DHCP, y net.c lo atiende antes de llegar aqui:
    // un socket en ese puerto no recibiria nada y quien lo abriera se
    // pasaria la tarde buscando el fallo en su programa.
    if (puerto == PUERTO_DHCP_CLIENTE) return -3;

    if (puerto == 0) {
        // Un puerto libre de los altos, para quien solo quiere mandar y
        // recibir la contestacion. Se empieza en un sitio distinto cada
        // vez para que dos programas seguidos no se peleen por el mismo.
        static uint16_t siguiente = 49152;
        for (uint32_t intento = 0; intento < 16384; intento++) {
            uint16_t p = siguiente++;
            if (siguiente < 49152) siguiente = 49152;   // dio la vuelta
            if (!puerto_ocupado(p)) { puerto = p; break; }
        }
        if (puerto == 0) return -2;
    } else if (puerto_ocupado(puerto)) {
        // Dos sockets en el mismo puerto se repartirian los datagramas
        // segun el orden de la tabla, o sea por casualidad. Mejor un no
        // claro.
        return -2;
    }

    for (uint32_t i = 0; i < UDPS_MAX_SOCKETS; i++) {
        if (sk[i].abierto) continue;
        sk[i].abierto = true;
        sk[i].puerto = puerto;
        sk[i].cabeza = sk[i].cuantos = 0;
        sk[i].perdidos = 0;
        sk[i].ultimo_puerto = 0;
        for (int j = 0; j < 4; j++) sk[i].ultimo_ip[j] = 0;
        return (int32_t)i;
    }
    return -1;
}

void udps_cerrar(int32_t h) {
    if (h < 0 || h >= (int32_t)UDPS_MAX_SOCKETS) return;
    sk[h].abierto = false;
    sk[h].puerto = 0;
    sk[h].cabeza = sk[h].cuantos = 0;
}

uint16_t udps_puerto(int32_t h) { return valido(h) ? sk[h].puerto : 0; }

bool udps_entregar(uint16_t puerto_destino, const uint8_t origen_ip[4],
                   uint16_t origen_puerto, const uint8_t *carga, uint32_t len) {
    for (uint32_t i = 0; i < UDPS_MAX_SOCKETS; i++) {
        if (!sk[i].abierto || sk[i].puerto != puerto_destino) continue;

        if (len > UDPS_MAX_DATAGRAMA) {
            // Truncar seria peor que tirarlo: la aplicacion recibiria algo
            // que parece un mensaje entero y esta cortado por la mitad.
            sk[i].perdidos++;
            return true;
        }
        if (sk[i].cuantos >= UDPS_COLA) {
            // Cola llena: se tira EL QUE LLEGA, no el mas viejo. Tirar el
            // mas viejo dejaria a la aplicacion viendo los mensajes en un
            // orden distinto del que llegaron, y eso no se depura; tirando
            // el nuevo, lo que la aplicacion ve es siempre un prefijo de lo
            // que paso, y la cuenta de perdidos dice el resto.
            sk[i].perdidos++;
            return true;
        }
        uint32_t donde = (sk[i].cabeza + sk[i].cuantos) % UDPS_COLA;
        datagrama_t *d = &sk[i].cola[donde];
        for (uint32_t k = 0; k < len; k++) d->datos[k] = carga[k];
        d->len = len;
        d->puerto = origen_puerto;
        for (int k = 0; k < 4; k++) d->ip[k] = origen_ip[k];
        sk[i].cuantos++;
        return true;
    }
    return false;
}

int32_t udps_recibir(int32_t h, uint8_t *out, uint32_t max) {
    if (!valido(h) || sk[h].cuantos == 0) return -1;

    datagrama_t *d = &sk[h].cola[sk[h].cabeza];

    // El remitente se fija AQUI, al entregarlo, no al recibirlo de la red.
    for (int k = 0; k < 4; k++) sk[h].ultimo_ip[k] = d->ip[k];
    sk[h].ultimo_puerto = d->puerto;

    uint32_t n = d->len < max ? d->len : max;
    for (uint32_t k = 0; k < n; k++) out[k] = d->datos[k];

    sk[h].cabeza = (sk[h].cabeza + 1) % UDPS_COLA;
    sk[h].cuantos--;
    return (int32_t)n;
}

void udps_origen(int32_t h, uint8_t ip[4], uint16_t *puerto) {
    if (!valido(h)) {
        for (int k = 0; k < 4; k++) ip[k] = 0;
        if (puerto) *puerto = 0;
        return;
    }
    for (int k = 0; k < 4; k++) ip[k] = sk[h].ultimo_ip[k];
    if (puerto) *puerto = sk[h].ultimo_puerto;
}

uint32_t udps_perdidos(int32_t h) { return valido(h) ? sk[h].perdidos : 0; }

uint32_t udps_pendientes(int32_t h) { return valido(h) ? sk[h].cuantos : 0; }
