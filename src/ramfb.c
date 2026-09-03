// ramfb.c — Nemo OS
//
// Framebuffer lineal simple. La idea es la más directa posible: un
// bloque de RAM donde cada 4 bytes son un píxel (formato XRGB8888), y
// le decimos a QEMU (via fw_cfg) "muestra esto en pantalla". A partir
// de aquí, "dibujar" es simplemente escribir en ese array -- no hay
// aceleración, ni comandos, ni nada intermedio.

#include "ramfb.h"
#include "fwcfg.h"
#include "uart.h"

#define FB_W 1400
#define FB_H 900
#define FB_BPP 4
#define FOURCC_XRGB8888 0x34325258u // definido por la spec DRM/fourcc

__attribute__((aligned(4096))) static uint8_t framebuffer[FB_W * FB_H * FB_BPP];

// Buffer "trasero": todo lo que dibujamos va aqui, nunca directamente
// al framebuffer real. Sin esto, cada fb_fill_rect() individual seria
// visible en pantalla al instante -- lo que produce exactamente el
// parpadeo/"glitch" de ver la pantalla a medio redibujar. Con
// fb_present() copiamos el fotograma YA TERMINADO de una sola vez.
__attribute__((aligned(4096))) static uint8_t back_buffer[FB_W * FB_H * FB_BPP];

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} ramfb_cfg_t;

static uint32_t width_px, height_px, stride_bytes;

static inline uint32_t bswap32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF);
}
static inline uint64_t bswap64(uint64_t v) {
    return ((uint64_t)bswap32((uint32_t)v) << 32) | bswap32((uint32_t)(v >> 32));
}

bool ramfb_init(void) {
    uint16_t key;
    uint32_t size;
    if (!fw_cfg_find_file("etc/ramfb", &key, &size)) {
        uart_puts("ramfb: no se encontro 'etc/ramfb' (¿falta -device ramfb en QEMU?)\n");
        return false;
    }

    __attribute__((aligned(16))) static ramfb_cfg_t cfg;
    cfg.addr = bswap64((uint64_t)framebuffer);
    cfg.fourcc = bswap32(FOURCC_XRGB8888);
    cfg.flags = 0;
    cfg.width = bswap32(FB_W);
    cfg.height = bswap32(FB_H);
    cfg.stride = bswap32(FB_W * FB_BPP);

    if (!fw_cfg_dma_write(key, &cfg, sizeof(cfg))) {
        uart_puts("ramfb: fallo configurando el framebuffer\n");
        return false;
    }

    width_px = FB_W;
    height_px = FB_H;
    stride_bytes = FB_W * FB_BPP;

    uart_puts("ramfb: framebuffer activo (1400x900, XRGB8888).\n");
    return true;
}

uint32_t fb_width(void) { return width_px; }
uint32_t fb_height(void) { return height_px; }

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= width_px || y >= height_px) return;
    uint32_t *p = (uint32_t *)(back_buffer + y * stride_bytes + x * 4);
    *p = color;
}

uint32_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (x >= width_px || y >= height_px) return 0;
    uint32_t *p = (uint32_t *)(back_buffer + y * stride_bytes + x * 4);
    return *p;
}

// Copia el fotograma ya terminado del back buffer al framebuffer real
// que QEMU esta mostrando -- de una sola vez, al final de cada
// fotograma, nunca a medias.
void fb_present(void) {
    uint64_t *src = (uint64_t *)back_buffer;
    uint64_t *dst = (uint64_t *)framebuffer;
    uint32_t words = (FB_W * FB_H * FB_BPP) / 8; // copiamos de 8 en 8 bytes, mas rapido
    for (uint32_t i = 0; i < words; i++) {
        dst[i] = src[i];
    }
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t j = 0; j < h; j++) {
        for (uint32_t i = 0; i < w; i++) {
            fb_put_pixel(x + i, y + j, color);
        }
    }
}

void fb_draw_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t color) {
    for (uint32_t i = 0; i < w; i++) fb_put_pixel(x + i, y, color);
}

void fb_draw_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t color) {
    for (uint32_t j = 0; j < h; j++) fb_put_pixel(x, y + j, color);
}

void fb_draw_rect_border(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    fb_draw_hline(x, y, w, color);
    fb_draw_hline(x, y + h - 1, w, color);
    fb_draw_vline(x, y, h, color);
    fb_draw_vline(x + w - 1, y, h, color);
}

// Pega un icono RGBA (con transparencia) sobre el framebuffer,
// mezclando cada pixel segun su canal alfa -- necesario porque los
// iconos tienen bordes suaves/semitransparentes, no solo un recorte
// solido.
void fb_blit_icon(uint32_t x, uint32_t y, uint32_t size, const uint8_t *rgba) {
    for (uint32_t iy = 0; iy < size; iy++) {
        for (uint32_t ix = 0; ix < size; ix++) {
            const uint8_t *px = &rgba[(iy * size + ix) * 4];
            uint8_t a = px[3];
            if (a == 0) continue;

            uint32_t dst_color = fb_get_pixel(x + ix, y + iy);
            uint8_t dr = (uint8_t)(dst_color >> 16);
            uint8_t dg = (uint8_t)(dst_color >> 8);
            uint8_t db = (uint8_t)(dst_color);

            uint8_t r = (uint8_t)((px[0] * a + dr * (255 - a)) / 255);
            uint8_t g = (uint8_t)((px[1] * a + dg * (255 - a)) / 255);
            uint8_t b = (uint8_t)((px[2] * a + db * (255 - a)) / 255);

            fb_put_pixel(x + ix, y + iy, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
        }
    }
}
