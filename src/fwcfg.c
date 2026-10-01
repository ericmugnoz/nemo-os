// fwcfg.c — Nemo OS
//
// fw_cfg es el mecanismo que usa QEMU para pasarle información al
// firmware/kernel invitado: desde el número de CPUs hasta, en nuestro
// caso, la configuración del framebuffer (ramfb). Funciona con un
// registro "selector" (eliges qué archivo quieres) y un registro de
// "datos" (lees su contenido byte a byte) -- muy parecido en espíritu
// a como seleccionamos registros indexados en otros drivers.
//
// DETALLE IMPORTANTE: los registros de control de fw_cfg son BIG
// ENDIAN, aunque nuestra CPU trabaja en little-endian. Por eso vas a
// ver "bswap" (intercambio de bytes) por todas partes en este archivo
// -- sin eso, escribiríamos los valores "al revés" y el dispositivo
// entendería otra cosa completamente distinta.

#include "fwcfg.h"
#include "uart.h"

#define FW_CFG_BASE     0x09020000UL
#define FW_CFG_DATA_OFF 0x00
#define FW_CFG_SEL_OFF  0x08
#define FW_CFG_DMA_OFF  0x10

#define FW_CFG_FILE_DIR 0x19

#define FW_CFG_DMA_CTL_ERROR  0x01
#define FW_CFG_DMA_CTL_READ   0x02
#define FW_CFG_DMA_CTL_SELECT 0x08
#define FW_CFG_DMA_CTL_WRITE  0x10

typedef struct __attribute__((packed)) {
    uint32_t control;
    uint32_t length;
    uint64_t address;
} fw_cfg_dma_t;

static inline uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}
static inline uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline uint64_t bswap64(uint64_t v) {
    return ((uint64_t)bswap32((uint32_t)(v & 0xFFFFFFFFu)) << 32) |
           bswap32((uint32_t)(v >> 32));
}

static inline void fw_cfg_select(uint16_t key) {
    *(volatile uint16_t *)(FW_CFG_BASE + FW_CFG_SEL_OFF) = bswap16(key);
}
static inline uint8_t fw_cfg_read_byte(void) {
    return *(volatile uint8_t *)(FW_CFG_BASE + FW_CFG_DATA_OFF);
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

bool fw_cfg_find_file(const char *name, uint16_t *out_key, uint32_t *out_size) {
    fw_cfg_select(FW_CFG_FILE_DIR);

    // El propio contenido del "archivo" FW_CFG_FILE_DIR es un blob con
    // enteros big-endian dentro (esto es endianness de los DATOS, no
    // del registro -- lo leemos byte a byte y lo recomponemos nosotros).
    uint32_t count = 0;
    for (int i = 0; i < 4; i++) count = (count << 8) | fw_cfg_read_byte();

    int name_len = str_len(name);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t size = 0;
        for (int b = 0; b < 4; b++) size = (size << 8) | fw_cfg_read_byte();

        uint16_t select = 0;
        for (int b = 0; b < 2; b++) select = (uint16_t)((select << 8) | fw_cfg_read_byte());

        fw_cfg_read_byte(); fw_cfg_read_byte(); // reserved, 2 bytes

        char name_buf[56];
        for (int b = 0; b < 56; b++) name_buf[b] = (char)fw_cfg_read_byte();

        bool eq = true;
        for (int b = 0; b < name_len; b++) {
            if (name_buf[b] != name[b]) { eq = false; break; }
        }
        if (eq && name_buf[name_len] != '\0') eq = false;

        if (eq) {
            *out_key = select;
            *out_size = size;
            return true;
        }
    }
    return false;
}

bool fw_cfg_dma_write(uint16_t key, const void *buf, uint32_t length) {
    __attribute__((aligned(16))) static fw_cfg_dma_t dma;

    dma.control = bswap32(((uint32_t)key << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_WRITE);
    dma.length = bswap32(length);
    dma.address = bswap64((uint64_t)buf);

    __asm__ volatile("dsb sy" ::: "memory");

    uint64_t dma_addr_be = bswap64((uint64_t)&dma);
    *(volatile uint64_t *)(FW_CFG_BASE + FW_CFG_DMA_OFF) = dma_addr_be;

    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t attempts = 0;
    const uint32_t MAX_ATTEMPTS = 20000000;
    while (1) {
        uint32_t ctl = bswap32(dma.control);
        if (ctl == 0) return true; // 0 = terminado con exito
        if (ctl & FW_CFG_DMA_CTL_ERROR) {
            uart_puts("fwcfg: DMA devolvio error\n");
            return false;
        }
        attempts++;
        if (attempts >= MAX_ATTEMPTS) {
            uart_puts("fwcfg: TIMEOUT esperando la operacion DMA\n");
            return false;
        }
        __asm__ volatile("" ::: "memory");
    }
}
