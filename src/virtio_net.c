// virtio_net.c -- Nemo OS, QEMU virt
// Driver de la tarjeta virtio-net (virtio-mmio moderno, VERSION_1),
// el equivalente en QEMU del GENET de la Pi 4. Existe para que toda la
// pila de red (net.c, tcp.c, netshell.c) se pueda probar en QEMU sin
// la placa: `make run` arranca con una tarjeta virtio-net y el puerto
// 2323 redirigido al host, y `nc -v localhost 2323` en el Mac llega a
// la shell remota de Nemo OS corriendo dentro de QEMU.
//
// Mismo patron que disk.c (virtio-blk), que ya lleva meses probado en
// esta misma maquina virtual: escaneo de los 32 slots MMIO, negociacion
// de features, colas virtqueue de descriptores/avail/used en RAM. Lo
// que cambia: dos colas en vez de una (0 = recepcion, 1 = transmision),
// y cada trama va precedida de una cabecera virtio-net de 12 bytes que
// para nosotros es todo ceros (sin offload de checksum ni GSO: la pila
// de arriba calcula todos los checksums por software, igual que en la
// Pi). En recepcion, se dejan colgados de antemano varios buferes
// vacios (writable) y el dispositivo los va rellenando; el driver los
// recoge de la cola "used" y los vuelve a colgar.
//
// Sin mantenimiento de cache: en QEMU (TCG) el DMA es coherente con la
// CPU emulada -- disk.c tampoco lo hace. Aqui bastan las barreras.

#include <stdint.h>
#include <stdbool.h>
#include "virtio_net.h"
#include "uart.h"

#define VIRTIO_MMIO_BASE   0x0a000000UL
#define VIRTIO_MMIO_STRIDE 0x200UL
#define VIRTIO_MMIO_SLOTS  32
#define VIRTIO_MAGIC       0x74726976UL
#define VIRTIO_DEVICE_ID_NET 1

#define REG_MAGIC             0x000
#define REG_DEVICE_ID         0x008
#define REG_DEVICE_FEATURES   0x010
#define REG_DEVICE_FEATURES_SEL 0x014
#define REG_DRIVER_FEATURES   0x020
#define REG_DRIVER_FEATURES_SEL 0x024
#define REG_QUEUE_SEL         0x030
#define REG_QUEUE_NUM_MAX     0x034
#define REG_QUEUE_NUM         0x038
#define REG_QUEUE_READY       0x044
#define REG_QUEUE_NOTIFY      0x050
#define REG_STATUS            0x070
#define REG_QUEUE_DESC_LOW    0x080
#define REG_QUEUE_DESC_HIGH   0x084
#define REG_QUEUE_DRIVER_LOW  0x090
#define REG_QUEUE_DRIVER_HIGH 0x094
#define REG_QUEUE_DEVICE_LOW  0x0a0
#define REG_QUEUE_DEVICE_HIGH 0x0a4
#define REG_CONFIG            0x100

#define STATUS_ACKNOWLEDGE 1
#define STATUS_DRIVER      2
#define STATUS_DRIVER_OK   4
#define STATUS_FEATURES_OK 8

#define VIRTIO_NET_F_MAC        5   // la MAC esta en el espacio de configuracion
#define VIRTIO_F_VERSION_1_BIT  0   // en la palabra alta (bit 32)

#define QUEUE_SIZE 16
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#define VNET_HDR_LEN 12
#define BUF_LEN (VNET_HDR_LEN + 1536)

struct virtq_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[QUEUE_SIZE]; uint16_t used_event; } __attribute__((packed));
struct virtq_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));
struct virtq_used { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[QUEUE_SIZE]; uint16_t avail_event; } __attribute__((packed));

#define RXQ 0
#define TXQ 1

__attribute__((aligned(4096))) static struct virtq_desc  desc[2][QUEUE_SIZE];
__attribute__((aligned(4096))) static struct virtq_avail avail[2];
__attribute__((aligned(4096))) static struct virtq_used  used[2];

static uint8_t rx_buf[QUEUE_SIZE][BUF_LEN] __attribute__((aligned(16)));
static uint8_t tx_buf[BUF_LEN] __attribute__((aligned(16)));

static uint64_t base = 0;
static bool listo = false;
static uint8_t mi_mac[6];
static uint16_t rx_last_used = 0, tx_last_used = 0;

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(base + off); }
static inline void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; }
static inline void barrera(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static bool configurar_cola(int q) {
    wr(REG_QUEUE_SEL, (uint32_t)q);
    if (rd(REG_QUEUE_NUM_MAX) == 0) return false;
    wr(REG_QUEUE_NUM, QUEUE_SIZE);
    uint64_t d = (uint64_t)desc[q], a = (uint64_t)&avail[q], u = (uint64_t)&used[q];
    wr(REG_QUEUE_DESC_LOW, (uint32_t)d);   wr(REG_QUEUE_DESC_HIGH, (uint32_t)(d >> 32));
    wr(REG_QUEUE_DRIVER_LOW, (uint32_t)a); wr(REG_QUEUE_DRIVER_HIGH, (uint32_t)(a >> 32));
    wr(REG_QUEUE_DEVICE_LOW, (uint32_t)u); wr(REG_QUEUE_DEVICE_HIGH, (uint32_t)(u >> 32));
    wr(REG_QUEUE_READY, 1);
    return true;
}

// Cuelga el bufer i de recepcion (writable) para que el dispositivo lo
// rellene con la siguiente trama que llegue.
static void colgar_rx(int i) {
    desc[RXQ][i].addr = (uint64_t)rx_buf[i];
    desc[RXQ][i].len = BUF_LEN;
    desc[RXQ][i].flags = VIRTQ_DESC_F_WRITE;
    desc[RXQ][i].next = 0;
    barrera();
    avail[RXQ].ring[avail[RXQ].idx % QUEUE_SIZE] = (uint16_t)i;
    barrera();
    avail[RXQ].idx++;
    barrera();
}

bool virtio_net_init(void) {
    for (int i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
        uint64_t b = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_STRIDE;
        if (*(volatile uint32_t *)(b + REG_MAGIC) != VIRTIO_MAGIC) continue;
        if (*(volatile uint32_t *)(b + REG_DEVICE_ID) == VIRTIO_DEVICE_ID_NET) { base = b; break; }
    }
    if (!base) {
        uart_puts("virtio-net: no hay tarjeta de red en la maquina virtual (falta -device virtio-net-device?)\n");
        return false;
    }

    wr(REG_STATUS, 0);
    wr(REG_STATUS, STATUS_ACKNOWLEDGE);
    wr(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    // Features: solo VERSION_1 (obligatoria en virtio moderno) y MAC (para
    // leer la direccion). Todo lo demas que ofrezca el dispositivo --
    // offload de checksum, GSO, buferes fusionados, control queue -- se
    // rechaza a proposito: la pila de arriba lo hace todo por software.
    wr(REG_DEVICE_FEATURES_SEL, 0);
    uint32_t ofrece = rd(REG_DEVICE_FEATURES);
    uint32_t acepto = ofrece & (1u << VIRTIO_NET_F_MAC);
    wr(REG_DRIVER_FEATURES_SEL, 0); wr(REG_DRIVER_FEATURES, acepto);
    wr(REG_DRIVER_FEATURES_SEL, 1); wr(REG_DRIVER_FEATURES, 1u << VIRTIO_F_VERSION_1_BIT);
    wr(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!(rd(REG_STATUS) & STATUS_FEATURES_OK)) {
        uart_puts("virtio-net: el dispositivo rechazo la negociacion de features\n");
        return false;
    }

    // La MAC: 6 bytes al principio del espacio de configuracion.
    for (int i = 0; i < 6; i++) mi_mac[i] = *(volatile uint8_t *)(base + REG_CONFIG + i);

    if (!configurar_cola(RXQ) || !configurar_cola(TXQ)) {
        uart_puts("virtio-net: colas no disponibles\n");
        return false;
    }
    wr(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);

    // Colgar todos los buferes de recepcion y avisar al dispositivo.
    for (int i = 0; i < QUEUE_SIZE; i++) colgar_rx(i);
    wr(REG_QUEUE_NOTIFY, RXQ);

    listo = true;
    uart_puts("virtio-net: tarjeta lista, MAC = ");
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 6; i++) { uart_putc(hex[mi_mac[i] >> 4]); uart_putc(hex[mi_mac[i] & 0xF]); if (i < 5) uart_putc(':'); }
    uart_puts("\n");
    return true;
}

bool virtio_net_link_up(void) { return listo; }
void virtio_net_get_mac(uint8_t out[6]) { for (int i = 0; i < 6; i++) out[i] = mi_mac[i]; }

bool virtio_net_send(const uint8_t *frame, uint32_t len) {
    if (!listo || len > BUF_LEN - VNET_HDR_LEN) return false;
    for (int i = 0; i < VNET_HDR_LEN; i++) tx_buf[i] = 0;          // cabecera virtio-net: sin offloads
    for (uint32_t i = 0; i < len; i++) tx_buf[VNET_HDR_LEN + i] = frame[i];

    desc[TXQ][0].addr = (uint64_t)tx_buf;
    desc[TXQ][0].len = VNET_HDR_LEN + len;
    desc[TXQ][0].flags = 0;
    desc[TXQ][0].next = 0;
    barrera();
    avail[TXQ].ring[avail[TXQ].idx % QUEUE_SIZE] = 0;
    barrera();
    avail[TXQ].idx++;
    barrera();
    wr(REG_QUEUE_NOTIFY, TXQ);

    for (uint32_t intentos = 0; intentos < 20000000; intentos++) {
        __asm__ volatile("" ::: "memory");
        if (used[TXQ].idx != tx_last_used) { tx_last_used = used[TXQ].idx; return true; }
    }
    uart_puts("virtio-net: TIMEOUT transmitiendo\n");
    return false;
}

uint32_t virtio_net_recv(uint8_t *out, uint32_t max_len) {
    if (!listo) return 0;
    barrera();
    if (used[RXQ].idx == rx_last_used) return 0;                    // nada nuevo

    struct virtq_used_elem *e = &used[RXQ].ring[rx_last_used % QUEUE_SIZE];
    uint32_t i = e->id;
    uint32_t total = e->len;
    rx_last_used++;

    uint32_t copiar = 0;
    if (i < QUEUE_SIZE && total > VNET_HDR_LEN) {
        copiar = total - VNET_HDR_LEN;
        if (copiar > max_len) copiar = max_len;
        const uint8_t *src = rx_buf[i] + VNET_HDR_LEN;
        for (uint32_t k = 0; k < copiar; k++) out[k] = src[k];
    }
    if (i < QUEUE_SIZE) { colgar_rx((int)i); wr(REG_QUEUE_NOTIFY, RXQ); } // devolver el bufer al dispositivo
    return copiar;
}
