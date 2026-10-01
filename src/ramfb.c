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

// QEMU siempre usa su tamaño fijo (FB_W x FB_H de aqui abajo) -- el
// selector de resolucion es una funcion real solo en hardware (Pi 4);
// aqui los parametros se ignoran para no complicar el entorno de
// pruebas con algo que no aporta nada en un framebuffer virtual.
bool ramfb_init(uint32_t want_w, uint32_t want_h) {
    (void)want_w; (void)want_h;
    uint16_t key;
    uint32_t size;
    if (!fw_cfg_find_file("etc/ramfb", &key, &size)) {
        uart_puts("ramfb: no se encontro 'etc/ramfb' (falta -device ramfb en QEMU?)\n");
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

// Igual, pero solo un rectangulo: lo que hace falta para mover el cursor del
// raton sin volver a copiar los cinco megas de la pantalla entera.
void fb_present_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }               // recortar por arriba y por la izquierda
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((uint32_t)x >= width_px || (uint32_t)y >= height_px) return;
    if ((uint32_t)(x + w) > width_px) w = (int32_t)width_px - x;
    if ((uint32_t)(y + h) > height_px) h = (int32_t)height_px - y;

    for (int32_t f = 0; f < h; f++) {
        const uint32_t *src = (const uint32_t *)(back_buffer + (uint32_t)(y + f) * stride_bytes + (uint32_t)x * 4);
        uint32_t *dst = (uint32_t *)(framebuffer + (uint32_t)(y + f) * stride_bytes + (uint32_t)x * 4);
        for (int32_t i = 0; i < w; i++) dst[i] = src[i];
    }
}

// Rellenar un rectangulo recortando UNA VEZ y escribiendo filas seguidas.
//
// Antes llamaba a fb_put_pixel por cada pixel, con su comprobacion de
// limites y su multiplicacion cada vez. Rellenar el fondo del escritorio
// era una llamada por cada pixel de la pantalla, y la composicion entera
// tardaba 8-17 ms con el candado grande cogido: con varios nucleos, el
// resto pasaba hasta el 90% del tiempo esperandola (medido con medir.c).
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (x >= width_px || y >= height_px) return;
    if (w > width_px - x) w = width_px - x;     // recortar sin desbordar
    if (h > height_px - y) h = height_px - y;
    for (uint32_t j = 0; j < h; j++) {
        uint32_t *p = (uint32_t *)(back_buffer + (y + j) * stride_bytes + x * 4);
        for (uint32_t i = 0; i < w; i++) p[i] = color;
    }
}

// Copiar una fila de pixeles al back buffer, recortada contra la
// pantalla. x puede ser negativa (ventana medio fuera por la izquierda).
// Para blit_content (wm.c), que antes llamaba a fb_put_pixel por pixel.
void fb_blit_row(int32_t x, int32_t y, const uint32_t *src, uint32_t n) {
    if (y < 0 || (uint32_t)y >= height_px) return;
    if (x < 0) {
        uint32_t fuera = (uint32_t)(-x);
        if (fuera >= n) return;
        src += fuera; n -= fuera; x = 0;
    }
    if ((uint32_t)x >= width_px) return;
    if (n > width_px - (uint32_t)x) n = width_px - (uint32_t)x;
    uint32_t *dst = (uint32_t *)(back_buffer + (uint32_t)y * stride_bytes + (uint32_t)x * 4);
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
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
    fb_blit_icon_scaled(x, y, size, 1, rgba);
}

// Igual que fb_blit_icon, pero cada pixel de origen se dibuja como un
// bloque scale x scale -- mismo mecanismo que SetFont con las fuentes,
// para poder mostrar iconos mas grandes sin bitmaps aparte por tamaño.
void fb_blit_icon_scaled(uint32_t x, uint32_t y, uint32_t size, uint32_t scale, const uint8_t *rgba) {
    if (scale < 1) scale = 1;
    for (uint32_t iy = 0; iy < size; iy++) {
        for (uint32_t ix = 0; ix < size; ix++) {
            const uint8_t *px = &rgba[(iy * size + ix) * 4];
            uint8_t a = px[3];
            if (a == 0) continue;

            for (uint32_t sy = 0; sy < scale; sy++) {
                for (uint32_t sx = 0; sx < scale; sx++) {
                    uint32_t dx = x + ix * scale + sx;
                    uint32_t dy = y + iy * scale + sy;
                    uint32_t dst_color = fb_get_pixel(dx, dy);
                    uint8_t dr = (uint8_t)(dst_color >> 16);
                    uint8_t dg = (uint8_t)(dst_color >> 8);
                    uint8_t db = (uint8_t)(dst_color);

                    uint8_t r = (uint8_t)((px[0] * a + dr * (255 - a)) / 255);
                    uint8_t g = (uint8_t)((px[1] * a + dg * (255 - a)) / 255);
                    uint8_t b = (uint8_t)((px[2] * a + db * (255 - a)) / 255);

                    fb_put_pixel(dx, dy, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
                }
            }
        }
    }
}
