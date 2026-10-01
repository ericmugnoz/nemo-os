// pcie_pi4.c -- Nemo OS, Raspberry Pi 4
// Bring-up del controlador PCIe del BCM2711 y del VL805 (el chip
// xHCI de los cuatro puertos USB-A), en el ORDEN del driver real.
//
// Esta es la secuencia que cerro la Fase B. Las tres cosas que la
// distinguen de los intentos anteriores, y que resolvieron a la vez
// el `0xdeaddead` al leer los registros xHCI y el SError con la MMU
// activa:
//
//  1. PERST# se mantiene ASERTADO durante toda la configuracion del
//     puente y se libera UNA sola vez al final. (Escribir 0 en
//     RGR1_SW_INIT_1 como primer paso soltaba PERST# y el reset del
//     puente a la vez, con el puente vacio: el VL805 entrenaba contra
//     nada, y esa transicion disparaba un SError asincrono.)
//  2. Esperas REALES con el temporizador de ARM: 100ms tras liberar
//     PERST#, 300ms tras pedir a VideoCore el firmware del VL805.
//     Esta placa (Pi 4B 8GB rev1.5) no tiene EEPROM: sin firmware el
//     VL805 responde a configuracion por hardware, pero su bloque de
//     registros xHCI no existe todavia y el bus devuelve relleno.
//  3. Ventana de reenvio de memoria del puente (MEMORY_BASE/LIMIT,
//     offset 0x20 de la cabecera tipo 1). Sin ella el puente ni
//     siquiera acepta su registro de Comando. En Linux la escribe el
//     nucleo generico de PCI, no el driver del SoC.
//
// Referencias (solo lectura, ninguna linea copiada):
//   Linux drivers/pci/controller/pcie-brcmstb.c y el nucleo PCI
//   (pci_setup_bridge_mmio); commit de N. Saenz Julienne que añadio
//   NOTIFY_XHCI_RESET; OpenBSD bcm2711_pcie.c; Circle (rsta2) como
//   implementacion bare-metal confirmada funcionando en esta placa.

#include "pcie_pi4.h"

void uart_puts(const char *s);
void uart_putc(char c);

static void uart_put_hex32(uint32_t val) {
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uint8_t n = (val >> i) & 0xF;
        uart_putc(n < 10 ? (char)('0' + n) : (char)('a' + n - 10));
    }
}

// ---------------------------------------------------------------------
// Registros del controlador (BCM2711, modo low-peri)
// ---------------------------------------------------------------------
#define PCIE_BASE 0xFD500000UL
#define REG32(off) (*(volatile uint32_t *)(PCIE_BASE + (off)))

// Configuracion del puente (bus 0): cabecera PCI directamente en base.
// Otros buses: escribir el tag en EXT_CFG_INDEX y leer/escribir en
// EXT_CFG_DATA + registro.
#define PCIE_EXT_CFG_INDEX               REG32(0x9000)
#define PCIE_EXT_CFG_DATA                (PCIE_BASE + 0x8000)

#define PCIE_RC_CFG_VENDOR_SPECIFIC_REG1 REG32(0x0188)
#define PCIE_RC_CFG_PRIV1_ID_VAL3        REG32(0x043C)
#define PCIE_RC_CFG_PRIV1_LINK_CAP       REG32(0x04DC)
#define PCIE_RC_ROOT_CAP_CONTROL         REG32(0x00C8)   // PCIe cap + RTCTL
#define PCIE_MISC_CTRL                   REG32(0x4008)
#define PCIE_WIN0_LO                     REG32(0x400C)
#define PCIE_WIN0_HI                     REG32(0x4010)
#define PCIE_MISC_RC_BAR1_CONFIG_LO      REG32(0x402C)
#define PCIE_MISC_RC_BAR2_CONFIG_LO      REG32(0x4034)
#define PCIE_MISC_RC_BAR2_CONFIG_HI      REG32(0x4038)
#define PCIE_MISC_RC_BAR3_CONFIG_LO      REG32(0x403C)
#define PCIE_REG_STATUS                  REG32(0x4068)
#define PCIE_WIN0_BASE_LIMIT             REG32(0x4070)
#define PCIE_WIN0_BASE_HI                REG32(0x4080)
#define PCIE_WIN0_LIMIT_HI               REG32(0x4084)
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG   REG32(0x4204)
#define PCIE_INTR2_CPU_CLEAR             REG32(0x4308)
#define PCIE_INTR2_CPU_MASK_SET          REG32(0x4310)
#define PCIE_REG_INIT                    REG32(0x9210)   // RGR1_SW_INIT_1

#define RGR1_PERST_MASK                  0x1   // bit 0: PERST# del VL805
#define RGR1_INIT_GENERIC_MASK           0x2   // bit 1: reset del puente

#define MISC_CTRL_SCB_ACCESS_EN_MASK     0x1000
#define MISC_CTRL_CFG_READ_UR_MODE_MASK  0x2000
#define MISC_CTRL_MAX_BURST_SIZE_MASK    0x300000
#define MISC_CTRL_BURST_SIZE_128         0x0        // BCM2711
#define MISC_CTRL_SCB0_SIZE_MASK         0xF8000000
#define MISC_CTRL_SCB0_SIZE_SHIFT        27

#define RC_BAR2_SIZE_ENCODED_4GB         0x11       // log2(4GB) - 15
#define RC_BARx_SIZE_MASK                0x1F
#define RC_CRS_EN_MASK                   0x10       // RTCTL.CRSSVE
#define SERDES_IDDQ_MASK                 (1u << 27)
#define HARD_DEBUG_CLKREQ_DEBUG_ENABLE   0x2
#define STATUS_DL_ACTIVE_MASK            0x20
#define STATUS_PHYLINKUP_MASK            0x10
#define STATUS_PORT_RC_MASK              0x80

#define BROADCOM_VID                     0x14E4
#define VL805_VID                        0x1106

// Ventana de salida CPU->PCIe: 64MB en CPU 0x600000000 -> PCI 0xf8000000.
#define VL805_BAR0_CPU_ADDR              0x600000000ULL
#define VL805_BAR0_PCI_ADDR              0xf8000000UL
#define VENTANA_SALIDA_SIZE              0x4000000UL

// Ventana de entrada PCIe->RAM (RC_BAR2): 4GB en PCI 0x1_00000000.
#define PCIE_DMA_OFFSET_PCI              0x100000000ULL

// ---------------------------------------------------------------------
// Mailbox (canal de propiedades) para NOTIFY_XHCI_RESET
// ---------------------------------------------------------------------
#define MAILBOX_BASE   0xFE00B880UL
#define MAILBOX_READ   (*(volatile uint32_t *)(MAILBOX_BASE + 0x00))
#define MAILBOX_STATUS (*(volatile uint32_t *)(MAILBOX_BASE + 0x18))
#define MAILBOX_WRITE  (*(volatile uint32_t *)(MAILBOX_BASE + 0x20))
#define MAILBOX_FULL   0x80000000
#define MAILBOX_EMPTY  0x40000000
#define MAILBOX_CH_PROP 8
#define TAG_NOTIFY_XHCI_RESET 0x00030058

__attribute__((aligned(16)))
static volatile uint32_t msg_xhci_reset[7];

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------
static void esperar_ms(uint32_t ms) {
    uint64_t f, t0, t;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
    uint64_t fin = t0 + (f * ms) / 1000;
    do { __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t)); } while (t < fin);
}

static uint32_t hacer_tag(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t reg) {
    return (bus << 20) | (dev << 15) | (fn << 12) | (reg & ~3U);
}

static uint32_t pcie_conf_read(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t reg) {
    if (bus == 0) return REG32(reg);
    PCIE_EXT_CFG_INDEX = hacer_tag(bus, dev, fn, reg);
    return *(volatile uint32_t *)(PCIE_EXT_CFG_DATA + reg);
}

static void pcie_conf_write(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t reg, uint32_t v) {
    if (bus == 0) { REG32(reg) = v; return; }
    PCIE_EXT_CFG_INDEX = hacer_tag(bus, dev, fn, reg);
    *(volatile uint32_t *)(PCIE_EXT_CFG_DATA + reg) = v;
}

static bool esperar_link_up(uint32_t intentos) {
    while (intentos--) {
        uint32_t s = PCIE_REG_STATUS;
        if ((s & STATUS_DL_ACTIVE_MASK) && (s & STATUS_PHYLINKUP_MASK)) return true;
    }
    return false;
}

// Pide a VideoCore que cargue el firmware del VL805 (necesario tras
// cada reset de PCIe en las placas sin EEPROM). Devuelve true si el
// firmware atendio el tag.
static bool notificar_reset_vl805(void) {
    msg_xhci_reset[0] = 7 * 4;
    msg_xhci_reset[1] = 0;
    msg_xhci_reset[2] = TAG_NOTIFY_XHCI_RESET;
    msg_xhci_reset[3] = 4;
    msg_xhci_reset[4] = 4;
    msg_xhci_reset[5] = hacer_tag(1, 0, 0, 0);   // bus 1, dev 0, fn 0
    msg_xhci_reset[6] = 0;

    uintptr_t base = (uintptr_t)&msg_xhci_reset;
    uintptr_t fin = base + sizeof(msg_xhci_reset);
    for (uintptr_t a = base & ~63UL; a < fin; a += 64)
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t direccion = ((uint32_t)base & ~0xF) | MAILBOX_CH_PROP;
    while (MAILBOX_STATUS & MAILBOX_FULL) {}
    MAILBOX_WRITE = direccion;
    for (;;) {
        while (MAILBOX_STATUS & MAILBOX_EMPTY) {}
        if (MAILBOX_READ == direccion) break;
    }

    for (uintptr_t a = base & ~63UL; a < fin; a += 64)
        __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");

    return msg_xhci_reset[1] == 0x80000000 && (msg_xhci_reset[4] & 0x80000000);
}

// ---------------------------------------------------------------------
// API
// ---------------------------------------------------------------------
uintptr_t pcie_xhci_base(void)  { return (uintptr_t)VL805_BAR0_CPU_ADDR; }
uint64_t  pcie_dma_offset(void) { return PCIE_DMA_OFFSET_PCI; }

bool pcie_init_pi4(void) {
    // 1. Puente en reset Y PERST# asertado.
    PCIE_REG_INIT = RGR1_INIT_GENERIC_MASK | RGR1_PERST_MASK;
    __asm__ volatile("dsb sy" ::: "memory");
    esperar_ms(1);

    // 2. Puente fuera de reset; PERST# sigue asertado.
    PCIE_REG_INIT = RGR1_PERST_MASK;
    __asm__ volatile("dsb sy" ::: "memory");

    // 3. SerDes fuera de IDDQ.
    PCIE_MISC_HARD_PCIE_HARD_DEBUG &= ~SERDES_IDDQ_MASK;
    esperar_ms(1);

    // 4. El puente responde?
    if ((pcie_conf_read(0, 0, 0, 0) & 0xFFFF) != BROADCOM_VID) {
        uart_puts("pcie_pi4: FALLO -- el puente PCIe no responde.\n");
        return false;
    }

    // 5. Interrupciones propias del controlador: limpiar y enmascarar.
    PCIE_INTR2_CPU_CLEAR    = 0xFFFFFFFF; (void)PCIE_INTR2_CPU_CLEAR;
    PCIE_INTR2_CPU_MASK_SET = 0xFFFFFFFF; (void)PCIE_INTR2_CPU_MASK_SET;

    // 6. MISC_CTRL.
    {
        uint32_t m = PCIE_MISC_CTRL;
        m |= MISC_CTRL_SCB_ACCESS_EN_MASK | MISC_CTRL_CFG_READ_UR_MODE_MASK;
        m = (m & ~MISC_CTRL_MAX_BURST_SIZE_MASK) | MISC_CTRL_BURST_SIZE_128;
        PCIE_MISC_CTRL = m;
    }

    // 7. RC_BAR2 (entrada, 4GB en PCI 0x1_00000000), SCB0_SIZE, BAR1/3 off.
    PCIE_MISC_RC_BAR2_CONFIG_LO = RC_BAR2_SIZE_ENCODED_4GB;
    PCIE_MISC_RC_BAR2_CONFIG_HI = (uint32_t)(PCIE_DMA_OFFSET_PCI >> 32);
    PCIE_MISC_RC_BAR1_CONFIG_LO &= ~RC_BARx_SIZE_MASK;
    PCIE_MISC_RC_BAR3_CONFIG_LO &= ~RC_BARx_SIZE_MASK;
    {
        uint32_t m = PCIE_MISC_CTRL;
        m = (m & ~MISC_CTRL_SCB0_SIZE_MASK) |
            ((RC_BAR2_SIZE_ENCODED_4GB << MISC_CTRL_SCB0_SIZE_SHIFT) & MISC_CTRL_SCB0_SIZE_MASK);
        PCIE_MISC_CTRL = m;
    }

    // 8-10. ASPM solo L1; clase RC = puente PCI-PCI; endian BAR2 little.
    PCIE_RC_CFG_PRIV1_LINK_CAP = (PCIE_RC_CFG_PRIV1_LINK_CAP & ~0xC00u) | (0x2u << 10);
    PCIE_RC_CFG_PRIV1_ID_VAL3  = (PCIE_RC_CFG_PRIV1_ID_VAL3 & ~0xFFFFFFu) | 0x060400u;
    PCIE_RC_CFG_VENDOR_SPECIFIC_REG1 &= ~0xCu;

    // 11. Liberar PERST# una sola vez y esperar el enlace.
    PCIE_REG_INIT = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    esperar_ms(100);
    if (!esperar_link_up(2000000)) {
        uart_puts("pcie_pi4: FALLO -- el enlace PCIe no entreno. STATUS=");
        uart_put_hex32(PCIE_REG_STATUS);
        uart_puts("\n");
        return false;
    }
    if (!(PCIE_REG_STATUS & STATUS_PORT_RC_MASK)) {
        uart_puts("pcie_pi4: FALLO -- el controlador no esta en modo RC.\n");
        return false;
    }

    // 12. Ventana de salida CPU->PCIe.
    PCIE_WIN0_LO = (uint32_t)VL805_BAR0_PCI_ADDR;
    PCIE_WIN0_HI = 0;
    PCIE_WIN0_BASE_LIMIT = 0x03f00000;   // base/limite en MB, bits bajos
    PCIE_WIN0_BASE_HI    = 0x06;         // 0x600000000 >> 32
    PCIE_WIN0_LIMIT_HI   = 0x06;

    // 13. Refclk gated con CLKREQ#.
    PCIE_MISC_HARD_PCIE_HARD_DEBUG |= HARD_DEBUG_CLKREQ_DEBUG_ENABLE;

    // 14. Cabecera tipo 1 del puente.
    REG32(0x18) = 0x00 | (0x01 << 8) | (0x01 << 16);            // buses 0/1/1
    {
        uint32_t base_mb  = (uint32_t)(VL805_BAR0_PCI_ADDR >> 16);
        uint32_t limit_mb = (uint32_t)((VL805_BAR0_PCI_ADDR + VENTANA_SALIDA_SIZE - 1) >> 16);
        REG32(0x20) = (limit_mb << 16) | base_mb;                // MEMORY_BASE/LIMIT
    }
    REG32(0x3C) |= (1u << 16);                                   // BRIDGE_CONTROL: paridad
    PCIE_RC_ROOT_CAP_CONTROL |= RC_CRS_EN_MASK;                  // CRS software visible
    REG32(0x04) |= (1u << 1) | (1u << 2) | (1u << 6) | (1u << 8); // Mem|Master|Parity|SERR

    // 15. Firmware del VL805.
    if (!notificar_reset_vl805()) {
        uart_puts("pcie_pi4: AVISO -- VideoCore no atendio NOTIFY_XHCI_RESET.\n");
    }
    esperar_ms(300);

    // 16. El dispositivo.
    {
        uint32_t id = pcie_conf_read(1, 0, 0, 0);
        if ((id & 0xFFFF) != VL805_VID) {
            uart_puts("pcie_pi4: FALLO -- VL805 no detectado (VID=");
            uart_put_hex32(id & 0xFFFF);
            uart_puts(").\n");
            return false;
        }
    }
    pcie_conf_write(1, 0, 0, 0x0C, 16);                          // cache line size
    pcie_conf_write(1, 0, 0, 0x10, (uint32_t)VL805_BAR0_PCI_ADDR); // BAR0 lo
    pcie_conf_write(1, 0, 0, 0x14, 0);                           // BAR0 hi
    pcie_conf_write(1, 0, 0, 0x04,
                    pcie_conf_read(1, 0, 0, 0x04) | (1u << 1) | (1u << 2) | (1u << 6) | (1u << 8));

    // 17. El bloque xHCI responde?
    {
        volatile uint32_t *cap = (volatile uint32_t *)(uintptr_t)VL805_BAR0_CPU_ADDR;
        uint32_t cap0 = cap[0];
        if (cap0 == 0xdeaddead || cap0 == 0xffffffff || cap0 == 0) {
            uart_puts("pcie_pi4: FALLO -- el bloque xHCI no responde (");
            uart_put_hex32(cap0);
            uart_puts(").\n");
            return false;
        }
        uart_puts("pcie_pi4: VL805 listo. xHCI HCIVERSION=");
        uart_put_hex32(cap0 >> 16);
        uart_puts(" HCSPARAMS1=");
        uart_put_hex32(cap[1]);
        uart_puts("\n");
    }
    return true;
}
