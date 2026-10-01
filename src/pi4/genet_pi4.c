// genet_pi4.c -- Nemo OS, Raspberry Pi 4
// Driver del controlador Ethernet integrado (GENET v5, BCM2711).
//
// Registros y secuencia de arranque transcritos del driver real de
// U-Boot para esta misma placa (drivers/net/bcmgenet.c, "Driver for
// Broadcom GENETv5 Ethernet controller (as found on the RPi4)"), que
// a su vez esta basado en el driver de Linux -- es la referencia
// minima PROBADA que existe para este chip en modo sondeo (sin
// interrupciones): un unico anillo (el 16, "cola por defecto"), 256
// descriptores para RX y otros 256 para TX, sin bloques de estado de
// 64 bytes. Nemo OS ya usa exactamente este mismo estilo de "sondeo
// en cada vuelta" para teclado/raton por USB, asi que encaja bien con
// el resto del sistema sin anadir interrupciones nuevas.
//
// Los DESCRIPTORES DE DMA no viven en RAM -- viven DENTRO del propio
// espacio de registros del chip (GENET_RX_OFF/GENET_TX_OFF), que ya
// esta mapeado como memoria de dispositivo por la MMU (dentro del
// bloque de perifericos 0xC0000000-0xFFFFFFFF, ver mmu_pi4.c) -- sin
// cache, sin necesidad de limpiarla. Solo los BUFERES DE PAQUETES de
// verdad (en RAM normal, cacheable) necesitan la limpieza/invalidacion
// manual de siempre: el mismo patron ya usado en xhci_pi4.c para el
// DMA de USB, reutilizado tal cual.
//
// Lo que NO esta aqui todavia: interrupciones (sondeo puro, como el
// resto de la entrada del sistema), multiples colas de prioridad (todo
// va a la cola 16, la mas simple), y Wake-on-LAN/EEE (no hacen falta
// para tener red basica). La negociacion de velocidad del PHY usa los
// registros MII estandar (Clause 22, iguales en cualquier fabricante)
// para deteccion de enlace; la velocidad exacta negociada se lee del
// registro de resumen especifico de Broadcom -- si algun dia se prueba
// con un PHY de otro fabricante, esa unica lectura habria que revisarla.

#include <stdint.h>
#include <stdbool.h>
#include "genet_pi4.h"
#include "mailbox_pi4.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);

// Decimal sencillo, para los tiempos de la negociacion. genet solo
// tenia hexadecimal, y "enlace arriba tras 0x00000514 ms" no se lee.
static void uart_put_dec(uint32_t v) {
    char t[12];
    int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0 && n < 11) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) uart_putc(t[--n]);
}

static void uart_put_hex32(uint32_t val) {
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uint8_t nibble = (val >> i) & 0xF;
        uart_putc((char)((nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10)));
    }
}

// ---------------------------------------------------------------------
// Espera en microsegundos -- no existia en el proyecto todavia (el
// unico temporizador de milisegundos es timer.c, del planificador).
// El contador generico de ARM (CNTPCT_EL0) ya esta en marcha desde el
// arranque -- timer.c lee su frecuencia (CNTFRQ_EL0) para lo mismo.
// ---------------------------------------------------------------------
static void udelay_generico(uint32_t us) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t ciclos = (freq / 1000000ULL) * us;
    uint64_t inicio;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(inicio));
    uint64_t ahora;
    do {
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ahora));
    } while (ahora - inicio < ciclos);
}

// ---------------------------------------------------------------------
// Coherencia de cache para los buferes de paquetes (RAM normal) --
// identico al patron ya usado en xhci_pi4.c para el DMA de USB.
// ---------------------------------------------------------------------
static void cache_limpiar(const void *p, uintptr_t len) {
    uintptr_t a = (uintptr_t)p & ~63UL, fin = (uintptr_t)p + len;
    for (; a < fin; a += 64) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void cache_invalidar(const void *p, uintptr_t len) {
    uintptr_t a = (uintptr_t)p & ~63UL, fin = (uintptr_t)p + len;
    for (; a < fin; a += 64) __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// ---------------------------------------------------------------------
// Registros. Nombres y desplazamientos identicos a los del driver de
// U-Boot -- ver la cabecera de este archivo para la referencia.
// ---------------------------------------------------------------------
#define GENET_BASE 0xFD580000UL

#define SYS_REV_CTRL          0x00
#define SYS_PORT_CTRL         0x04
#define PORT_MODE_EXT_GPHY    3

#define SYS_RBUF_FLUSH_CTRL   0x08
#define SYS_TBUF_FLUSH_CTRL   0x0c

#define EXT_RGMII_OOB_CTRL    0x8c
#define RGMII_LINK            (1u << 4)
#define OOB_DISABLE           (1u << 5)
#define RGMII_MODE_EN         (1u << 6)
#define ID_MODE_DIS           (1u << 16)

#define RBUF_CTRL             0x300
#define RBUF_ALIGN_2B         (1u << 1)
#define RBUF_TBUF_SIZE_CTRL   0x3b4

#define UMAC_MAC0             0x80c
#define UMAC_MAC1             0x810
#define UMAC_CMD              0x808
#define UMAC_MAX_FRAME_LEN    0x814
#define UMAC_MIB_CTRL         0xd80
#define UMAC_TX_FLUSH         0xb34
#define MDIO_CMD              0xe14

#define MDIO_START_BUSY       (1u << 29)
#define MDIO_READ_FAIL        (1u << 28)
#define MDIO_RD               (2u << 26)
#define MDIO_WR               (1u << 26)
#define MDIO_PMD_SHIFT        21
#define MDIO_REG_SHIFT        16

#define CMD_TX_EN             (1u << 0)
#define CMD_RX_EN             (1u << 1)
#define UMAC_SPEED_10         0
#define UMAC_SPEED_100        1
#define UMAC_SPEED_1000       2
#define CMD_SPEED_SHIFT       2
#define CMD_SW_RESET          (1u << 13)
#define CMD_LCL_LOOP_EN       (1u << 15)

#define MIB_RESET_RX          (1u << 0)
#define MIB_RESET_RUNT        (1u << 1)
#define MIB_RESET_TX          (1u << 2)

#define TOTAL_DESCS 256
#define DEFAULT_Q   0x10   // 16: la cola "por defecto", la mas simple

#define ENET_MAX_MTU_SIZE 1536   // 1500 datos + 14 cabecera + 4 vlan + 6 tag brcm + 4 fcs + 8, redondeado

#define DMA_BUFLENGTH_MASK    0x0fffu
#define DMA_BUFLENGTH_SHIFT   16
#define DMA_RING_SIZE_SHIFT   16
#define DMA_OWN               0x8000u
#define DMA_EOP               0x4000u
#define DMA_SOP               0x2000u
#define DMA_MAX_BURST_LENGTH  0x8u

#define DMA_TX_APPEND_CRC     0x0040u
#define DMA_TX_QTAG_SHIFT     7

#define DMA_RING_SIZE 0x40
#define DMA_RINGS_SIZE (DMA_RING_SIZE * (DEFAULT_Q + 1))

#define DMA_DESC_LENGTH_STATUS 0x00
#define DMA_DESC_ADDRESS_LO    0x04
#define DMA_DESC_ADDRESS_HI    0x08
#define DMA_DESC_SIZE          12

#define GENET_RX_OFF       0x2000
#define GENET_RDMA_REG_OFF (GENET_RX_OFF + TOTAL_DESCS * DMA_DESC_SIZE)
#define GENET_TX_OFF       0x4000
#define GENET_TDMA_REG_OFF (GENET_TX_OFF + TOTAL_DESCS * DMA_DESC_SIZE)

#define DMA_FC_THRESH_HI (TOTAL_DESCS >> 4)
#define DMA_FC_THRESH_LO 5
#define DMA_FC_THRESH_VALUE ((DMA_FC_THRESH_LO << 16) | DMA_FC_THRESH_HI)

#define TDMA_RING_REG_BASE (GENET_TDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)
#define TDMA_READ_PTR        (TDMA_RING_REG_BASE + 0x00)
#define TDMA_CONS_INDEX      (TDMA_RING_REG_BASE + 0x08)
#define TDMA_PROD_INDEX      (TDMA_RING_REG_BASE + 0x0c)
#define DMA_RING_BUF_SIZE    0x10
#define DMA_START_ADDR       0x14
#define DMA_END_ADDR         0x1c
#define DMA_MBUF_DONE_THRESH 0x24
#define TDMA_FLOW_PERIOD     (TDMA_RING_REG_BASE + 0x28)
#define TDMA_WRITE_PTR       (TDMA_RING_REG_BASE + 0x2c)

#define RDMA_RING_REG_BASE (GENET_RDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)
#define RDMA_WRITE_PTR       (RDMA_RING_REG_BASE + 0x00)
#define RDMA_PROD_INDEX      (RDMA_RING_REG_BASE + 0x08)
#define RDMA_CONS_INDEX      (RDMA_RING_REG_BASE + 0x0c)
#define RDMA_XON_XOFF_THRESH (RDMA_RING_REG_BASE + 0x28)
#define RDMA_READ_PTR        (RDMA_RING_REG_BASE + 0x2c)

#define TDMA_REG_BASE (GENET_TDMA_REG_OFF + DMA_RINGS_SIZE)
#define RDMA_REG_BASE (GENET_RDMA_REG_OFF + DMA_RINGS_SIZE)
#define DMA_RING_CFG       0x00
#define DMA_CTRL           0x04
#define DMA_SCB_BURST_SIZE 0x0c
#define DMA_EN             (1u << 0)

#define RX_BUF_LENGTH  2048
#define RX_BUF_OFFSET  2   // alineacion de 2 bytes que pide RBUF_ALIGN_2B

// ---------------------------------------------------------------------
// MDIO -- registros estandar MII (Clause 22, iguales en cualquier PHY)
// ---------------------------------------------------------------------
#define MII_BMCR   0x00   // Control
#define MII_BMSR   0x01   // Estado
#define MII_ADVERTISE 0x04 // Anuncio de autonegociacion
#define BMCR_RESET     (1u << 15)
#define BMCR_ANRESTART (1u << 9)
#define BMCR_ANENABLE  (1u << 12)
#define BMSR_LSTATUS   (1u << 2)   // enlace arriba
// Anuncio estandar IEEE 802.3: 10/100 half y full duplex, mas el bit
// de seleccion de protocolo IEEE 802.3 (los 5 bits bajos = 00001).
// El Gigabit (1000baseT) se anuncia aparte, en el registro 0x09
// (1000BASE-T Control) -- sin ese anuncio, el PHY nunca ofrece
// Gigabit en la negociacion, por mucho que el cable y el otro
// extremo lo soporten.
#define ADVERTISE_10HALF   (1u << 5)
#define ADVERTISE_10FULL   (1u << 6)
#define ADVERTISE_100HALF  (1u << 7)
#define ADVERTISE_100FULL  (1u << 8)
#define ADVERTISE_CSMA     0x0001
#define MII_CTRL1000 0x09
#define ADVERTISE_1000FULL (1u << 9)

// Registro de resumen especifico de Broadcom (BCM54213PE): bits 11-12
// codifican la velocidad resuelta, bit 10 el modo full/half-duplex.
// Si algun dia se usa un PHY de otro fabricante, este registro
// concreto es el que habria que revisar primero.
#define MII_BRCM_AUX_STATUS 0x19
#define AUX_SPEED_SHIFT 8
#define AUX_SPEED_MASK  0x07

static volatile uint8_t *mac_reg;
static void *rx_desc_base, *tx_desc_base;
static int phyaddr = 1;   // confirmado en el device tree oficial (genet-phy@1)

static uint32_t genet_read(uint32_t off) { return *(volatile uint32_t *)(mac_reg + off); }
static void genet_write(uint32_t off, uint32_t val) { *(volatile uint32_t *)(mac_reg + off) = val; }

// ---- MDIO: leer/escribir un registro del PHY ----
static bool mdio_esperar(void) {
    for (int i = 0; i < 2000; i++) {
        if (!(genet_read(MDIO_CMD) & MDIO_START_BUSY)) return true;
        udelay_generico(10);
    }
    return false;
}
static uint16_t mdio_leer(int reg) {
    uint32_t val = MDIO_RD | ((uint32_t)phyaddr << MDIO_PMD_SHIFT) | ((uint32_t)reg << MDIO_REG_SHIFT);
    genet_write(MDIO_CMD, val);
    genet_write(MDIO_CMD, genet_read(MDIO_CMD) | MDIO_START_BUSY);
    if (!mdio_esperar()) return 0xFFFF;
    return (uint16_t)(genet_read(MDIO_CMD) & 0xFFFF);
}
static void mdio_escribir(int reg, uint16_t valor) {
    uint32_t val = MDIO_WR | ((uint32_t)phyaddr << MDIO_PMD_SHIFT) | ((uint32_t)reg << MDIO_REG_SHIFT) | valor;
    genet_write(MDIO_CMD, val);
    genet_write(MDIO_CMD, genet_read(MDIO_CMD) | MDIO_START_BUSY);
    mdio_esperar();
}

static void umac_reset(void) {
    uint32_t reg = genet_read(SYS_RBUF_FLUSH_CTRL);
    genet_write(SYS_RBUF_FLUSH_CTRL, reg | (1u << 1));
    udelay_generico(10);
    genet_write(SYS_RBUF_FLUSH_CTRL, reg & ~(uint32_t)(1u << 1));
    udelay_generico(10);
    genet_write(SYS_RBUF_FLUSH_CTRL, 0);
    udelay_generico(10);

    genet_write(UMAC_CMD, 0);
    genet_write(UMAC_CMD, CMD_SW_RESET | CMD_LCL_LOOP_EN);
    udelay_generico(2);
    genet_write(UMAC_CMD, 0);

    genet_write(UMAC_MIB_CTRL, MIB_RESET_RX | MIB_RESET_TX | MIB_RESET_RUNT);
    genet_write(UMAC_MIB_CTRL, 0);
    genet_write(UMAC_MAX_FRAME_LEN, ENET_MAX_MTU_SIZE);

    reg = genet_read(RBUF_CTRL);
    genet_write(RBUF_CTRL, reg | RBUF_ALIGN_2B);
    genet_write(RBUF_TBUF_SIZE_CTRL, 1);
}

static void genet_write_hwaddr(const uint8_t mac[6]) {
    uint32_t r0 = ((uint32_t)mac[0] << 24) | ((uint32_t)mac[1] << 16) | ((uint32_t)mac[2] << 8) | mac[3];
    uint32_t r1 = ((uint32_t)mac[4] << 8) | mac[5];
    genet_write(UMAC_MAC0, r0);
    genet_write(UMAC_MAC1, r1);
}

static void disable_dma(void) {
    genet_write(TDMA_REG_BASE + DMA_CTRL, genet_read(TDMA_REG_BASE + DMA_CTRL) & ~DMA_EN);
    genet_write(RDMA_REG_BASE + DMA_CTRL, genet_read(RDMA_REG_BASE + DMA_CTRL) & ~DMA_EN);
    genet_write(UMAC_TX_FLUSH, 1);
    udelay_generico(10);
    genet_write(UMAC_TX_FLUSH, 0);
}
static void enable_dma(void) {
    uint32_t dma_ctrl = (1u << (DEFAULT_Q + 1)) | DMA_EN;
    genet_write(TDMA_REG_BASE + DMA_CTRL, dma_ctrl);
    genet_write(RDMA_REG_BASE + DMA_CTRL, genet_read(RDMA_REG_BASE + DMA_CTRL) | dma_ctrl);
}

// ---- buferes de recepcion: RAM normal, alineados a linea de cache ----
#define RX_DESCS TOTAL_DESCS
static uint8_t rxbuffer[RX_DESCS * RX_BUF_LENGTH] __attribute__((aligned(64)));

static int rx_index = 0, c_index = 0;
static int tx_index = 0;

static void rx_descs_init(void) {
    uint32_t len_stat = ((uint32_t)RX_BUF_LENGTH << DMA_BUFLENGTH_SHIFT) | DMA_OWN;
    for (int i = 0; i < RX_DESCS; i++) {
        uintptr_t addr = (uintptr_t)&rxbuffer[i * RX_BUF_LENGTH];
        void *d = (uint8_t *)rx_desc_base + i * DMA_DESC_SIZE;
        *(volatile uint32_t *)((uint8_t *)d + DMA_DESC_ADDRESS_LO) = (uint32_t)addr;
        *(volatile uint32_t *)((uint8_t *)d + DMA_DESC_ADDRESS_HI) = (uint32_t)(addr >> 32);
        *(volatile uint32_t *)((uint8_t *)d + DMA_DESC_LENGTH_STATUS) = len_stat;
    }
}
static void rx_ring_init(void) {
    genet_write(RDMA_REG_BASE + DMA_SCB_BURST_SIZE, DMA_MAX_BURST_LENGTH);
    genet_write(RDMA_RING_REG_BASE + DMA_START_ADDR, 0);
    genet_write(RDMA_READ_PTR, 0);
    genet_write(RDMA_WRITE_PTR, 0);
    genet_write(RDMA_RING_REG_BASE + DMA_END_ADDR, RX_DESCS * DMA_DESC_SIZE / 4 - 1);
    // RDMA_PROD_INDEX no se puede poner a 0 -- se alinea CONS_INDEX
    // con lo que el hardware ya tenga, como hace U-Boot.
    // Enmascarar a 16 bits, igual que hace genet_recv: los 16 altos de
    // este registro NO son el indice, son el contador de descartes del
    // hardware. Cogiendolos aqui, c_index arrancaria con un valor que
    // el indice de productor no puede alcanzar nunca, y la primera
    // comparacion de genet_recv daria "hay algo" leyendo un descriptor
    // vacio. Con el chip recien reseteado ese contador esta a cero y
    // por eso no ha dado la cara todavia.
    c_index = (int)(genet_read(RDMA_PROD_INDEX) & 0xFFFF);
    genet_write(RDMA_CONS_INDEX, (uint32_t)c_index);
    rx_index = c_index & 0xFF;
    genet_write(RDMA_RING_REG_BASE + DMA_RING_BUF_SIZE, ((uint32_t)RX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH);
    genet_write(RDMA_XON_XOFF_THRESH, DMA_FC_THRESH_VALUE);
    genet_write(RDMA_REG_BASE + DMA_RING_CFG, 1u << DEFAULT_Q);
}
static void tx_ring_init(void) {
    genet_write(TDMA_REG_BASE + DMA_SCB_BURST_SIZE, DMA_MAX_BURST_LENGTH);
    genet_write(TDMA_RING_REG_BASE + DMA_START_ADDR, 0);
    genet_write(TDMA_READ_PTR, 0);
    genet_write(TDMA_WRITE_PTR, 0);
    genet_write(TDMA_RING_REG_BASE + DMA_END_ADDR, TOTAL_DESCS * DMA_DESC_SIZE / 4 - 1);
    tx_index = (int)genet_read(TDMA_CONS_INDEX);
    genet_write(TDMA_PROD_INDEX, (uint32_t)tx_index);
    tx_index &= 0xFF;
    genet_write(TDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH, 1);
    genet_write(TDMA_FLOW_PERIOD, 0);
    genet_write(TDMA_RING_REG_BASE + DMA_RING_BUF_SIZE, ((uint32_t)TOTAL_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH);
    genet_write(TDMA_REG_BASE + DMA_RING_CFG, 1u << DEFAULT_Q);
}

static uint32_t velocidad_actual_mbps = 0;

static bool phy_esperar_enlace(void) {
    // Primero, ¿el PHY responde AL MENOS algo por MDIO? Si el primer
    // registro leido es 0xFFFF, no es "sin cable" -- es que el PHY no
    // esta contestando en absoluto (direccion MDIO equivocada, PHY sin
    // alimentacion, o un fallo real del driver). Distinguir esto del
    // caso normal "cable desconectado" es la diferencia entre un bug
    // que hay que arreglar y nada que arreglar en absoluto.
    uint16_t bmsr_inicial = mdio_leer(MII_BMSR);
    if (bmsr_inicial == 0xFFFF) {
        uart_puts("genet: el PHY no responde por MDIO en absoluto (direccion equivocada o fallo del driver, no falta de cable)\n");
        return false;
    }

    // Reset de verdad del PHY (IEEE 802.3, bit 15 de BMCR) -- el
    // reset de CMD_SW_RESET de mas arriba es del lado MAC del GENET,
    // NUNCA toca el chip PHY externo (el BCM54213PE, en su propia
    // direccion MDIO). Sin este paso, el PHY sigue con lo que tuviera
    // configurado de antes (por ejemplo, de un arranque anterior con
    // otro sistema operativo) -- puede bastar para hablar con un
    // switch/router indulgente, pero falla justo en el caso mas
    // exigente: conectado directo a otro ordenador. El reset se
    // autolimpia solo (el propio PHY pone el bit a 0 cuando termina).
    mdio_escribir(MII_BMCR, BMCR_RESET);
    for (int i = 0; i < 500; i++) {
        if (!(mdio_leer(MII_BMCR) & BMCR_RESET)) break;
        udelay_generico(1000);
    }

    // Anunciar explicitamente todo lo que el PHY sabe hacer -- 10/100
    // en el registro estandar, Gigabit en el registro aparte (sin
    // esto, el PHY jamas ofrece Gigabit en la negociacion, y algunos
    // extremos son estrictos con lo que aceptan de un anuncio
    // incompleto).
    mdio_escribir(MII_ADVERTISE, ADVERTISE_CSMA | ADVERTISE_10HALF | ADVERTISE_10FULL |
                                  ADVERTISE_100HALF | ADVERTISE_100FULL);
    mdio_escribir(MII_CTRL1000, ADVERTISE_1000FULL);

    // Reiniciar autonegociacion, y sondear el estado hasta 5 segundos
    // -- un cable real tarda entre unos cientos de ms y un par de
    // segundos en negociar, pero un reset de PHY justo antes puede
    // sumar su propio retardo inicial.
    mdio_escribir(MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
    // Cinco segundos, y se deja constancia de cuanto tardo
    // en subir.
    //
    // Se probo a subirlo a veinte por si la caja del RasPad, que
    // extiende la Ethernet con su propio cable y su propio conector,
    // hacia mas lenta la negociacion. NO era eso: sin cable puesto,
    // veinte segundos dan exactamente el mismo resultado que cinco y
    // cuestan quince segundos de arranque a CADA usuario que no use
    // red. Medido antes de decidir, y por eso se vuelve atras.
    //
    // La marca de tiempo se queda: el dia que haya una negociacion
    // lenta de verdad, el numero estara en NEMO.LOG y no habra que
    // volver a elegir este tope a ojo.
    for (int intento = 0; intento < 500; intento++) {
        uint16_t bmsr = mdio_leer(MII_BMSR);
        if (bmsr == 0xFFFF) return false; // el PHY dejo de responder a media negociacion -- tambien un fallo real
        if (bmsr & BMSR_LSTATUS) {
            uart_puts("genet: enlace arriba tras "); uart_put_dec(intento * 10); uart_puts(" ms\n");
            uint16_t aux = mdio_leer(MII_BRCM_AUX_STATUS);
            uint32_t codigo = (aux >> AUX_SPEED_SHIFT) & AUX_SPEED_MASK;
            // Codificacion del resumen Broadcom: 0/1=10M, 2/3=100M, 4/5/6=1000M
            velocidad_actual_mbps = (codigo >= 4) ? 1000 : (codigo >= 2) ? 100 : 10;
            return true;
        }
        udelay_generico(10000); // 10ms
    }
    uart_puts("genet: el PHY responde por MDIO pero nunca reporto enlace en 5 s -- lo mas probable es que falte el cable\n");
    return false;
}

bool genet_link_up(void) { return velocidad_actual_mbps != 0; }
uint32_t genet_link_speed_mbps(void) { return velocidad_actual_mbps; }

static uint8_t mi_mac[6];
void genet_get_mac(uint8_t out[6]) { for (int i = 0; i < 6; i++) out[i] = mi_mac[i]; }

bool genet_init_pi4(void) {
    mac_reg = (volatile uint8_t *)GENET_BASE;
    rx_desc_base = (void *)(mac_reg + GENET_RX_OFF);
    tx_desc_base = (void *)(mac_reg + GENET_TX_OFF);

    uint32_t rev = genet_read(SYS_REV_CTRL);
    uint32_t major = (rev >> 24) & 0x0F;
    uart_puts("genet: SYS_REV_CTRL="); uart_put_hex32(rev); uart_puts("\n");
    if (major != 6) {
        // El driver de U-Boot documenta este mismo desplazamiento
        // "raro" (6 == v5) -- no es un error de transcripcion.
        uart_puts("genet: version de GENET no reconocida, absteniendose\n");
        return false;
    }

    if (!mailbox_get_mac_pi4(mi_mac)) {
        uart_puts("genet: no se pudo leer la MAC de fabrica por mailbox\n");
        return false;
    }
    uart_puts("genet: MAC de fabrica = ");
    for (int i = 0; i < 6; i++) {
        uart_put_hex32(mi_mac[i]); if (i < 5) uart_putc(':');
    }
    uart_puts("\n");

    // Solo RGMII (lo unico que lleva la Pi 4, PHY BCM54213PE por RGMII)
    genet_write(SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);

    genet_write(SYS_RBUF_FLUSH_CTRL, 0);
    udelay_generico(10);
    genet_write(UMAC_CMD, 0);
    genet_write(UMAC_CMD, CMD_SW_RESET | CMD_LCL_LOOP_EN);

    umac_reset();
    genet_write_hwaddr(mi_mac);
    disable_dma();
    rx_ring_init();
    rx_descs_init();
    tx_ring_init();
    enable_dma();

    if (!phy_esperar_enlace()) {
        uart_puts("genet: el enlace no subio (cable desconectado?)\n");
        return false;
    }
    // En decimal: una velocidad de red se lee en Mbps, no en hexadecimal.
    // Salia "0x000003e8 Mbps", que es 1000 pero hay que traducirlo a mano.
    uart_puts("genet: enlace arriba, "); uart_put_dec(velocidad_actual_mbps); uart_puts(" Mbps\n");

    uint32_t speed_code = (velocidad_actual_mbps >= 1000) ? UMAC_SPEED_1000
                         : (velocidad_actual_mbps >= 100) ? UMAC_SPEED_100 : UMAC_SPEED_10;
    uint32_t oob = genet_read(EXT_RGMII_OOB_CTRL);
    oob &= ~OOB_DISABLE;
    oob |= RGMII_LINK | RGMII_MODE_EN | ID_MODE_DIS;
    genet_write(EXT_RGMII_OOB_CTRL, oob);
    genet_write(UMAC_CMD, speed_code << CMD_SPEED_SHIFT);

    genet_write(UMAC_CMD, genet_read(UMAC_CMD) | CMD_TX_EN | CMD_RX_EN);
    return true;
}

bool genet_send(const uint8_t *frame, uint32_t len) {
    if (len > RX_BUF_LENGTH) return false; // no fragmentamos -- de sobra para una trama normal

    void *desc = (uint8_t *)tx_desc_base + tx_index * DMA_DESC_SIZE;
    uint32_t prod = genet_read(TDMA_PROD_INDEX);

    // El propio buffer del llamador se usa como origen de DMA -- hay
    // que dejarlo visible en RAM fisica antes de que el hardware lo
    // lea. Redondeado a linea de cache completa, igual que hace
    // U-Boot con su rounddown/roundup (aqui, cache_limpiar ya cubre
    // la linea entera sola).
    cache_limpiar(frame, len);

    uint32_t len_stat = (len << DMA_BUFLENGTH_SHIFT)
                       | (0x3Fu << DMA_TX_QTAG_SHIFT)   // obligatorio: sin esto el arbitro descarta la trama
                       | DMA_TX_APPEND_CRC | DMA_SOP | DMA_EOP;

    uintptr_t addr = (uintptr_t)frame;
    *(volatile uint32_t *)((uint8_t *)desc + DMA_DESC_ADDRESS_LO) = (uint32_t)addr;
    *(volatile uint32_t *)((uint8_t *)desc + DMA_DESC_ADDRESS_HI) = (uint32_t)(addr >> 32);
    *(volatile uint32_t *)((uint8_t *)desc + DMA_DESC_LENGTH_STATUS) = len_stat;

    if (++tx_index >= TOTAL_DESCS) tx_index = 0;
    prod++;
    genet_write(TDMA_PROD_INDEX, prod);

    for (int intentos = 0; intentos < 2000; intentos++) {
        if ((genet_read(TDMA_CONS_INDEX) & 0xFFFF) >= (prod & 0xFFFF)) return true;
        udelay_generico(10);
    }
    return false; // el hardware nunca confirmo -- el llamador decide si reintentar
}

void genet_diagnostico(void) {
    uint32_t cmd  = genet_read(UMAC_CMD);
    uint32_t rp   = genet_read(RDMA_PROD_INDEX);
    uint32_t rc   = genet_read(RDMA_CONS_INDEX);
    uint32_t tp   = genet_read(TDMA_PROD_INDEX);
    uint32_t tc   = genet_read(TDMA_CONS_INDEX);

    uart_puts("genet: RX llegadas="); uart_put_dec(rp & 0xFFFF);
    uart_puts(" leidas=");            uart_put_dec(rc & 0xFFFF);
    uart_puts(" descartadas=");       uart_put_dec(rp >> 16);
    uart_puts(" | TX pedidas=");      uart_put_dec(tp & 0xFFFF);
    uart_puts(" salidas=");           uart_put_dec(tc & 0xFFFF);
    uart_puts(" | UMAC_CMD=");        uart_put_hex32(cmd);
    // Los tres bits que tienen que estar como toca para que circule
    // algo. El de bucle local es el mas traicionero: con el puesto,
    // el MAC se habla a si mismo, el enlace se ve perfectamente arriba
    // y no entra ni sale una sola trama por el cable.
    uart_puts(cmd & CMD_RX_EN     ? " RX:si" : " RX:NO");
    uart_puts(cmd & CMD_TX_EN     ? " TX:si" : " TX:NO");
    uart_puts(cmd & CMD_LCL_LOOP_EN ? " BUCLE-LOCAL:PUESTO(mal)" : "");
    uart_puts("\n");
}

uint32_t genet_recv(uint8_t *out, uint32_t max_len) {
    // Solo los 16 bits bajos son el indice de productor; los 16 altos
    // son un contador de descartes del hardware (tramas que no cupieron
    // en el anillo). Sin la mascara, el primer descarte haria que
    // "prod" nunca volviera a coincidir con c_index y se releyeran
    // descriptores viejos para siempre. Mismo detalle que ya se
    // aplica a c_index (& 0xFFFF).
    uint32_t prod = genet_read(RDMA_PROD_INDEX) & 0xFFFF;
    if (prod == (uint32_t)c_index) return 0; // nada nuevo

    void *desc = (uint8_t *)rx_desc_base + rx_index * DMA_DESC_SIZE;
    uint32_t len_stat = *(volatile uint32_t *)((uint8_t *)desc + DMA_DESC_LENGTH_STATUS);
    uint32_t len = (len_stat >> DMA_BUFLENGTH_SHIFT) & DMA_BUFLENGTH_MASK;
    uint32_t addr_lo = *(volatile uint32_t *)((uint8_t *)desc + DMA_DESC_ADDRESS_LO);

    cache_invalidar((const void *)(uintptr_t)addr_lo, RX_BUF_LENGTH);

    uint32_t copiar = len - RX_BUF_OFFSET; // los 2 bytes de alineacion no son parte de la trama
    if (copiar > max_len) copiar = max_len;
    const uint8_t *src = (const uint8_t *)(uintptr_t)addr_lo + RX_BUF_OFFSET;
    for (uint32_t i = 0; i < copiar; i++) out[i] = src[i];

    // Devolver el buffer al hardware: dejarlo listo para la siguiente
    // vez que le toque a este mismo descriptor.
    cache_limpiar((const void *)(uintptr_t)addr_lo, RX_BUF_LENGTH);
    c_index = (c_index + 1) & 0xFFFF;
    genet_write(RDMA_CONS_INDEX, (uint32_t)c_index);
    if (++rx_index >= RX_DESCS) rx_index = 0;

    return copiar;
}
