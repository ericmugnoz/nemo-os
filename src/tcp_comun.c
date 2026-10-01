// tcp_comun.c -- Nemo OS. Ver tcp_comun.h.

#include "tcp_comun.h"

uint16_t tcpc_leer16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t tcpc_leer32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
void tcpc_escribir16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void tcpc_escribir32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t suma_parcial(uint32_t suma, const uint8_t *p, uint32_t len) {
    while (len > 1) { suma += ((uint32_t)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len == 1) suma += (uint32_t)p[0] << 8;   // el byte suelto, en la mitad ALTA
    return suma;
}

uint16_t tcpc_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                       const uint8_t *seg, uint32_t seg_len) {
    uint8_t pseudo[12];
    for (int i = 0; i < 4; i++) { pseudo[i] = src_ip[i]; pseudo[4 + i] = dst_ip[i]; }
    pseudo[8] = 0; pseudo[9] = TCP_PROTO_NUM;
    tcpc_escribir16(pseudo + 10, (uint16_t)seg_len);
    uint32_t suma = suma_parcial(0, pseudo, 12);
    suma = suma_parcial(suma, seg, seg_len);
    while (suma >> 16) suma = (suma & 0xFFFF) + (suma >> 16);
    return (uint16_t)~suma;
}

uint32_t tcpc_construir(uint8_t *out, const uint8_t mi_ip[4], const uint8_t su_ip[4],
                        uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                        uint8_t flags, uint16_t ventana, bool con_mss,
                        const uint8_t *datos, uint32_t datos_len) {
    uint32_t hdr_len = TCP_MIN_HDR + (con_mss ? 4 : 0);
    tcpc_escribir16(out + 0, sport);
    tcpc_escribir16(out + 2, dport);
    tcpc_escribir32(out + 4, seq);
    tcpc_escribir32(out + 8, ack);
    out[12] = (uint8_t)((hdr_len / 4) << 4);
    out[13] = flags;
    tcpc_escribir16(out + 14, ventana);
    tcpc_escribir16(out + 16, 0);   // checksum: se calcula al final
    tcpc_escribir16(out + 18, 0);   // puntero urgente, no se usa
    if (con_mss) { out[20] = 2; out[21] = 4; tcpc_escribir16(out + 22, TCP_MSS); }
    for (uint32_t i = 0; i < datos_len; i++) out[hdr_len + i] = datos[i];
    uint32_t total = hdr_len + datos_len;
    tcpc_escribir16(out + 16, tcpc_checksum(mi_ip, su_ip, out, total));
    return total;
}
