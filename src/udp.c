// udp.c -- Nemo OS
// Ver udp.h para el alcance. Dos funciones y una suma de comprobacion.

#include "udp.h"

static uint16_t leer16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void escribir16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

// La suma de Internet (RFC 1071) acumulada, para poder sumar por
// trozos: primero la pseudo-cabecera, luego el datagrama.
static uint32_t suma_parcial(uint32_t suma, const uint8_t *p, uint32_t len) {
    while (len > 1) { suma += ((uint32_t)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len == 1) suma += (uint32_t)p[0] << 8;   // el byte suelto va en la mitad ALTA
    return suma;
}

static uint16_t suma_cerrar(uint32_t suma) {
    while (suma >> 16) suma = (suma & 0xFFFF) + (suma >> 16);
    return (uint16_t)~suma;
}

// La pseudo-cabecera de IPv4: IP origen, IP destino, un cero, el
// protocolo, y la longitud UDP. Doce bytes que NO se envian; solo
// entran en la cuenta.
static uint32_t suma_pseudo(const uint8_t src_ip[4], const uint8_t dst_ip[4], uint32_t udp_len) {
    uint32_t suma = 0;
    suma = suma_parcial(suma, src_ip, 4);
    suma = suma_parcial(suma, dst_ip, 4);
    suma += UDP_PROTO;
    suma += udp_len;
    return suma;
}

uint32_t udp_empaquetar(uint8_t *out, uint32_t carga_len,
                        const uint8_t src_ip[4], const uint8_t dst_ip[4],
                        uint16_t src_port, uint16_t dst_port) {
    uint32_t total = UDP_HDR_LEN + carga_len;
    escribir16(out + 0, src_port);
    escribir16(out + 2, dst_port);
    escribir16(out + 4, (uint16_t)total);
    escribir16(out + 6, 0);                      // a cero MIENTRAS se calcula

    uint16_t cs = suma_cerrar(suma_parcial(suma_pseudo(src_ip, dst_ip, total), out, total));
    // Una suma que sale 0 se transmite como 0xFFFF: el cero esta
    // reservado para decir "no la calcule", y son valores equivalentes
    // en aritmetica de complemento a uno.
    if (cs == 0) cs = 0xFFFF;
    escribir16(out + 6, cs);
    return total;
}

bool udp_desempaquetar(const uint8_t *seg, uint32_t seg_len,
                       const uint8_t src_ip[4], const uint8_t dst_ip[4],
                       uint16_t *src_port, uint16_t *dst_port,
                       const uint8_t **carga, uint32_t *carga_len) {
    if (seg_len < UDP_HDR_LEN) return false;

    uint32_t declarada = leer16(seg + 4);
    if (declarada < UDP_HDR_LEN || declarada > seg_len) return false;

    uint16_t cs = leer16(seg + 6);
    if (cs != 0) {   // 0 = el remitente decidio no calcularla (legal en IPv4)
        if (suma_cerrar(suma_parcial(suma_pseudo(src_ip, dst_ip, declarada), seg, declarada)) != 0)
            return false;
    }

    *src_port = leer16(seg + 0);
    *dst_port = leer16(seg + 2);
    *carga = seg + UDP_HDR_LEN;
    *carga_len = declarada - UDP_HDR_LEN;
    return true;
}
