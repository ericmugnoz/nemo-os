// arp.c -- Nemo OS. Ver arp.h para el porque y el alcance.

#include "arp.h"

typedef struct {
    uint8_t  ip[4];
    uint8_t  mac[6];
    uint64_t ms;      // cuando se aprendio
    bool     usada;
} entrada_t;

static entrada_t tabla[ARP_ENTRADAS];

static bool misma_ip(const uint8_t a[4], const uint8_t b[4]) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return false;
    return true;
}

// Una entrada caducada cuenta como inexistente. Se comprueba al
// buscarla y no con un barrido periodico: sin nadie que llame, nadie
// necesita saber que caduco, y un barrido seria trabajo en cada vuelta
// del bucle principal a cambio de nada.
static bool vigente(const entrada_t *e, uint64_t ms) {
    return e->usada && (ms - e->ms) < ARP_CADUCIDAD_MS;
}

void arp_aprender(const uint8_t ip[4], const uint8_t mac[6], uint64_t ms) {
    // Ni la direccion "sin asignar" ni la de difusion son de nadie.
    if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) return;
    if (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255) return;

    // Si ya estaba, se refresca -- incluida la MAC, que puede haber
    // cambiado si alguien sustituyo un aparato por otro con la misma IP.
    for (uint32_t i = 0; i < ARP_ENTRADAS; i++) {
        if (tabla[i].usada && misma_ip(tabla[i].ip, ip)) {
            for (int k = 0; k < 6; k++) tabla[i].mac[k] = mac[k];
            tabla[i].ms = ms;
            return;
        }
    }

    // Sitio libre, o el mas viejo. Lo segundo no es una eleccion fina
    // --lo fino seria el menos usado-- pero con ocho entradas la
    // diferencia no se nota y esto se lee de un vistazo.
    uint32_t elegida = 0;
    bool hay_libre = false;
    for (uint32_t i = 0; i < ARP_ENTRADAS; i++) {
        if (!vigente(&tabla[i], ms)) { elegida = i; hay_libre = true; break; }
    }
    if (!hay_libre) {
        uint64_t mas_vieja = tabla[0].ms;
        for (uint32_t i = 1; i < ARP_ENTRADAS; i++)
            if (tabla[i].ms < mas_vieja) { mas_vieja = tabla[i].ms; elegida = i; }
    }

    for (int k = 0; k < 4; k++) tabla[elegida].ip[k] = ip[k];
    for (int k = 0; k < 6; k++) tabla[elegida].mac[k] = mac[k];
    tabla[elegida].ms = ms;
    tabla[elegida].usada = true;
}

bool arp_buscar(const uint8_t ip[4], uint8_t mac_out[6], uint64_t ms) {
    for (uint32_t i = 0; i < ARP_ENTRADAS; i++) {
        if (vigente(&tabla[i], ms) && misma_ip(tabla[i].ip, ip)) {
            for (int k = 0; k < 6; k++) mac_out[k] = tabla[i].mac[k];
            return true;
        }
    }
    return false;
}

void arp_vaciar(void) {
    for (uint32_t i = 0; i < ARP_ENTRADAS; i++) tabla[i].usada = false;
}

uint32_t arp_cuantas(uint64_t ms) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < ARP_ENTRADAS; i++) if (vigente(&tabla[i], ms)) n++;
    return n;
}

uint32_t arp_construir_peticion(uint8_t *out, const uint8_t mi_mac[6],
                                const uint8_t mi_ip[4], const uint8_t buscada[4]) {
    // Ethernet: a todos, de mi, tipo ARP.
    for (int i = 0; i < 6; i++) out[i] = 0xFF;
    for (int i = 0; i < 6; i++) out[6 + i] = mi_mac[i];
    out[12] = 0x08; out[13] = 0x06;

    uint8_t *a = out + 14;
    a[0] = 0x00; a[1] = 0x01;      // sobre Ethernet
    a[2] = 0x08; a[3] = 0x00;      // buscando una IPv4
    a[4] = 6;                       // longitud de una MAC
    a[5] = 4;                       // longitud de una IP
    a[6] = 0x00; a[7] = 0x01;      // operacion: pregunta
    for (int i = 0; i < 6; i++) a[8 + i]  = mi_mac[i];
    for (int i = 0; i < 4; i++) a[14 + i] = mi_ip[i];
    // La MAC que se busca va a cero: es lo que se pregunta.
    for (int i = 0; i < 6; i++) a[18 + i] = 0;
    for (int i = 0; i < 4; i++) a[24 + i] = buscada[i];

    return 14 + 28;
}
