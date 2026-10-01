// ramfb_pi4.c -- implementacion de ramfb.h para Raspberry Pi 4
//
// La pieza central de la Fase 3: implementa exactamente la misma
// interfaz que ramfb.c usa en QEMU (documentada en ramfb.h), para
// que wm.c, gadgets.c, y cualquier otro codigo que ya dibuje a
// traves de fb_put_pixel/fb_fill_rect/etc funcione en la Pi 4 sin
// cambiar ni una linea.
//
// Misma resolucion fija (1024x768) que ramfb.c usa en QEMU, para
// que ninguna suposicion de coordenadas en el resto del kernel
// (por ejemplo, wm_maximize_window calculando w->w = fb_width())
// se rompa por un tamaño de pantalla distinto entre plataformas.
//
// Diferencia real frente a QEMU: el framebuffer de la Pi 4, pedido
// por mailbox (Fase 2), usa orden de color BGR, no RGB -- verificado
// en hardware real con la prueba de colores primarios puros. El
// back buffer interno de aqui sigue guardando en RGB, exactamente
// igual que en QEMU; la conversion a BGR ocurre solo una vez, en
// fb_present(), al copiar al framebuffer real -- asi ningun codigo
// por encima de esta capa (wm.c incluido) necesita saber que el
// hardware real usa un orden de color distinto.

#include "ramfb.h"
#include "mailbox_pi4.h"
#include <stdint.h>
#include <stdbool.h>

void uart_puts(const char *s);

// FB_W/FB_H: resolucion SEGURA de siempre (1024x768) -- se usa si no
// hay ninguna preferencia guardada, o como respaldo automatico si el
// VideoCore no puede conceder la que se le pida.
#define FB_W 1024
#define FB_H 768
#define FB_BPP 4

// Tamaño MAXIMO que el back buffer puede llegar a necesitar -- cubre
// todas las resoluciones que ofrece el selector de pantalla (ver
// desktoped.c). Si algun dia se añade una mayor a la lista, hay que
// agrandar esto tambien.
#define MAX_FB_W 1600
#define MAX_FB_H 900

// BUG REAL EVITADO: fb_put_pixel/fb_get_pixel/fb_present calculaban
// la posicion de cada fila con el ancho FIJO de compilacion (FB_W),
// no con el ancho real de la resolucion activa -- funcionaba solo
// porque hasta ahora la resolucion SIEMPRE era exactamente FB_W.
// Ahora que la resolucion puede ser otra (elegida en Ajustes), la
// fila de cada pixel se calcula con 'back_stride_px' (el ancho REAL
// en uso), fijado una vez en ramfb_init.
__attribute__((aligned(4096))) static uint8_t back_buffer[MAX_FB_W * MAX_FB_H * FB_BPP];

static uint32_t width_px, height_px;
static uint32_t back_stride_px; // ancho real usado para direccionar back_buffer (ver nota de arriba)
static bool necesita_bgr = true;  // ¿hay que cambiar rojo por azul al presentar? (ver ramfb_init)
static uint8_t *fb_real = 0;      // direccion ARM real, de fb_init_pi4
static uint32_t fb_real_pitch = 0; // pitch REAL devuelto por el VideoCore,
                                     // no necesariamente ancho*4 exacto

// ---- EL FRAMEBUFFER DE LA PI ESTA EN MEMORIA CACHEADA ----
//
// FALLO REAL, encontrado en la placa. El VideoCore devuelve la direccion del
// framebuffer en 0xFE876000; quitandole el alias del bus queda 0x3E876000,
// dentro del primer GB, y el primer GB esta mapeado BLOCK_KERNEL: Normal
// Write-Back, o sea CACHEADO. Lo que escribimos ahi se queda en la cache del
// nucleo, y el controlador de pantalla lee la RAM, no nuestra cache.
//
// Esto NO se notaba mientras 'fb_present' copiaba los 8 MB de la pantalla
// entera en cada fotograma: 8 MB no caben en 1 MB de L2, asi que cada linea
// se expulsaba —y se escribia a RAM— en uno o dos fotogramas. La pantalla
// siempre veia datos frescos por pura presion de cache.
//
// En cuanto la presentacion paso a copiar solo el rectangulo que cambia, se
// vio de golpe: mover el raton escribe 3,5 KB, que caben de sobra en la cache
// y se quedan ahi para siempre. Sintoma exacto: **el cursor solo se movia al
// pinchar**, porque un clic pide redibujado entero y esos megas de trafico
// expulsaban lo demas.
//
// La cura es limpiar a mano las lineas de cache que se han escrito. Es lo que
// hay que hacer con cualquier framebuffer cacheado, y es barato: una
// instruccion por linea de 64 bytes.
static void limpiar_cache(const void *ini, uint32_t bytes) {
    if (bytes == 0) return;
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    // CTR_EL0.DminLine (bits 19:16): log2 del tamaño de linea en PALABRAS.
    uint64_t linea = 4ULL << ((ctr >> 16) & 0xF);
    uint64_t p = (uint64_t)(uintptr_t)ini & ~(linea - 1);
    uint64_t fin = (uint64_t)(uintptr_t)ini + bytes;
    for (; p < fin; p += linea) {
        __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    }
}

// Una sola barrera al final de cada presentacion, no una por fila.
static inline void cache_a_la_ram(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

// want_w/want_h: resolucion pedida (la guardada en Ajustes, o 0,0
// para la segura de siempre). Si el VideoCore no la concede --
// pantalla que no la admite, cable raro, lo que sea -- se reintenta
// UNA vez con la segura antes de rendirse: mejor arrancar en un
// tamaño que no es el pedido que no arrancar en absoluto.
bool ramfb_init(uint32_t want_w, uint32_t want_h) {
    // --- DIAGNOSTICO TEMPORAL, Fase A del subproyecto USB ---
    // Prueba aislada de PCIe/VL805, sin relacion con el
    // framebuffer -- puesta aqui solo porque este archivo ya se
    // ejecuta pronto en el arranque y ya imprime diagnosticos por
    // UART. Quitar esta llamada en cuanto la prueba de PCIe se
    // confirme (funcione o falle).
    extern bool pcie_init_pi4(void);
    pcie_init_pi4();
    // --- fin del diagnostico temporal ---

    // Desactivamos interrupciones durante la transaccion con el
    // VideoCore -- en la Fase 2 (sin temporizador activo todavia)
    // esta misma llamada funciono sin problema; la diferencia real
    // aqui es que kernel_main ya activo el timer antes de llegar a
    // esta funcion, y una interrupcion a mitad de la espera del
    // mailbox es la sospecha mas concreta que tenemos.
    // want_w=0 (o mayor que el maximo que el back buffer puede
    // guardar) -- usar la resolucion segura de siempre directamente,
    // sin intentar nada raro primero.
    uint32_t pedir_w = want_w, pedir_h = want_h;
    if (pedir_w == 0 || pedir_h == 0 || pedir_w > MAX_FB_W || pedir_h > MAX_FB_H) {
        pedir_w = FB_W;
        pedir_h = FB_H;
    }

    uint64_t daif_previo;
    __asm__ volatile("mrs %0, daif" : "=r"(daif_previo));
    __asm__ volatile("msr daifset, #2");

    framebuffer_t fb = fb_init_pi4(pedir_w, pedir_h);

    // Si el VideoCore no concedio la resolucion pedida (pantalla que
    // no la admite, lo que sea) y no era ya la segura de siempre, se
    // reintenta UNA vez con esa -- mejor un tamaño distinto al
    // elegido que ninguna pantalla en absoluto.
    if (!fb.ok && (pedir_w != FB_W || pedir_h != FB_H)) {
        uart_puts("ramfb_pi4: la resolucion pedida no se concedio, "
                  "reintentando con la segura (1024x768)\n");
        fb = fb_init_pi4(FB_W, FB_H);
    }

    __asm__ volatile("msr daif, %0" :: "r"(daif_previo));

    if (!fb.ok) {
        uart_puts("ramfb_pi4: el VideoCore no concedio el "
                  "framebuffer\n");
        return false;
    }

    width_px = fb.ancho;
    height_px = fb.alto;
    back_stride_px = width_px;
    fb_real = fb.buffer;
    fb_real_pitch = fb.pitch;

    // ---- ¿HACE FALTA CAMBIAR ROJO POR AZUL? SI, SIEMPRE ----
    //
    // COMPROBADO EN LA PLACA, y no salio como se esperaba. El firmware
    // contesta en el mailbox que orden concede, y se probo a hacerle caso:
    // en esta Pi contesta "concedido = 1 (RGB)" y AUN ASI la pantalla sale
    // con los colores invertidos si no se hace el cambio. O sea que el valor
    // que devuelve la etiqueta 0x48006 no significa lo que parece, y **no se
    // puede usar para decidir**. Se deja leido y anunciado en el arranque
    // como dato, nada mas.
    //
    // Por eso FORZAR_BGR se queda a 1: es el comportamiento probado en
    // hardware desde la Fase 2. El camino rapido sin conversion sigue escrito
    // y funciona (lo usa QEMU), pero aqui no se toma.
    #define FORZAR_BGR 1
    #if FORZAR_BGR
    necesita_bgr = true;
    (void)fb.orden_pixel;
    #else
    necesita_bgr = (fb.orden_pixel != 1);
    #endif

    uart_puts("ramfb_pi4: framebuffer real listo, presentando ");
    uart_puts(necesita_bgr ? "con cambio de rojo/azul\n" : "SIN conversion\n");
    return true;
}

uint32_t fb_width(void) { return width_px; }
uint32_t fb_height(void) { return height_px; }

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= width_px || y >= height_px) return;
    uint32_t *p = (uint32_t *)(back_buffer +
                                y * (back_stride_px * FB_BPP) + x * 4);
    *p = color;
}

uint32_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (x >= width_px || y >= height_px) return 0;
    uint32_t *p = (uint32_t *)(back_buffer +
                                y * (back_stride_px * FB_BPP) + x * 4);
    return *p;
}

// Recortar UNA VEZ y escribir filas seguidas, en vez de una llamada a
// fb_put_pixel por pixel. Ver la nota en ramfb.c (QEMU): la composicion
// entera tenia el candado grande cogido demasiado tiempo.
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint32_t color) {
    if (x >= width_px || y >= height_px) return;
    if (w > width_px - x) w = width_px - x;
    if (h > height_px - y) h = height_px - y;
    for (uint32_t j = 0; j < h; j++) {
        uint32_t *p = (uint32_t *)(back_buffer + (y + j) * (back_stride_px * FB_BPP) + x * 4);
        for (uint32_t i = 0; i < w; i++) p[i] = color;
    }
}

// Copiar una fila de pixeles al back buffer, recortada contra la
// pantalla (x puede ser negativa). Ver ramfb.c.
void fb_blit_row(int32_t x, int32_t y, const uint32_t *src, uint32_t n) {
    if (y < 0 || (uint32_t)y >= height_px) return;
    if (x < 0) {
        uint32_t fuera = (uint32_t)(-x);
        if (fuera >= n) return;
        src += fuera; n -= fuera; x = 0;
    }
    if ((uint32_t)x >= width_px) return;
    if (n > width_px - (uint32_t)x) n = width_px - (uint32_t)x;
    uint32_t *dst = (uint32_t *)(back_buffer + (uint32_t)y * (back_stride_px * FB_BPP) + (uint32_t)x * 4);
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

void fb_draw_hline(uint32_t x, uint32_t y, uint32_t w,
                    uint32_t color) {
    for (uint32_t i = 0; i < w; i++) fb_put_pixel(x + i, y, color);
}

void fb_draw_vline(uint32_t x, uint32_t y, uint32_t h,
                    uint32_t color) {
    for (uint32_t j = 0; j < h; j++) fb_put_pixel(x, y + j, color);
}

void fb_draw_rect_border(uint32_t x, uint32_t y, uint32_t w,
                          uint32_t h, uint32_t color) {
    fb_draw_hline(x, y, w, color);
    fb_draw_hline(x, y + h - 1, w, color);
    fb_draw_vline(x, y, h, color);
    fb_draw_vline(x + w - 1, y, h, color);
}

void fb_blit_icon(uint32_t x, uint32_t y, uint32_t size,
                   const uint8_t *rgba) {
    fb_blit_icon_scaled(x, y, size, 1, rgba);
}

// Igual que fb_blit_icon, pero cada pixel de origen se dibuja como un
// bloque scale x scale -- mismo mecanismo que SetFont con las fuentes,
// para poder mostrar iconos mas grandes sin bitmaps aparte por tamaño.
void fb_blit_icon_scaled(uint32_t x, uint32_t y, uint32_t size,
                          uint32_t scale, const uint8_t *rgba) {
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

                    uint32_t solid = ((uint32_t)r << 16) |
                                      ((uint32_t)g << 8) | b;
                    fb_put_pixel(dx, dy, solid);
                }
            }
        }
    }
}

// La unica funcion realmente distinta de QEMU: copia el back
// buffer (en RGB) al framebuffer real (en BGR), pixel a pixel,
// respetando el pitch real del hardware -- que puede no coincidir
// exactamente con FB_W*4 si el VideoCore añadio relleno.
// Igual que fb_present pero SOLO un rectangulo, con el mismo cambio de rojo y
// azul. Lo que hace falta para mover el cursor del raton sin volver a convertir
// y copiar la pantalla entera: el cursor mide 12x19 (24x38 a escala doble), o
// sea unos 3,5 KB frente a los 5 MB de una pantalla de 1280x1024.
//
// Aqui se va pixel a pixel a proposito, no de dos en dos: un rectangulo
// estrecho casi nunca empieza en una direccion alineada a 8 bytes, y el
// pixel suelto de cada borde costaria mas cuidado del que ahorraria la
// pareja. Con 900 pixeles da igual; con la pantalla entera no daba.
void fb_present_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!fb_real || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((uint32_t)x >= width_px || (uint32_t)y >= height_px) return;
    if ((uint32_t)(x + w) > width_px) w = (int32_t)width_px - x;
    if ((uint32_t)(y + h) > height_px) h = (int32_t)height_px - y;

    for (int32_t f = 0; f < h; f++) {
        const uint32_t *orig = (const uint32_t *)
            (back_buffer + (uint32_t)(y + f) * (back_stride_px * FB_BPP)) + x;
        uint32_t *dest = (uint32_t *)(fb_real + (uint32_t)(y + f) * fb_real_pitch) + x;
        if (!necesita_bgr) {
            for (int32_t i = 0; i < w; i++) dest[i] = orig[i];
        } else {
            for (int32_t i = 0; i < w; i++) {
                uint32_t c = orig[i];
                dest[i] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
            }
        }
        // Sin esto, un rectangulo pequeño se queda en la cache y no llega
        // nunca a la pantalla. Ver la nota de limpiar_cache.
        limpiar_cache(dest, (uint32_t)w * FB_BPP);
    }
    cache_a_la_ram();
}

void fb_present(void) {
    if (!fb_real) return;

    // Si el firmware concedio RGB, los dos bufer tienen ya el mismo formato y
    // presentar es copiar: de 8 en 8 bytes y sin tocar ningun pixel. Se ahorra
    // la mitad del coste de la composicion. Ver la nota de ramfb_init.
    if (!necesita_bgr) {
        for (uint32_t y = 0; y < height_px; y++) {
            const uint64_t *orig = (const uint64_t *)
                (back_buffer + y * (back_stride_px * FB_BPP));
            uint64_t *dest = (uint64_t *)(fb_real + y * fb_real_pitch);
            uint32_t pares = width_px / 2;
            for (uint32_t i = 0; i < pares; i++) dest[i] = orig[i];
            if (width_px & 1) {
                ((uint32_t *)dest)[width_px - 1] = ((const uint32_t *)orig)[width_px - 1];
            }
            limpiar_cache(dest, width_px * FB_BPP);
        }
        cache_a_la_ram();
        return;
    }

    // El framebuffer de la Pi es BGR (confirmado en la Fase 2) y el back
    // buffer RGB: hay que intercambiar rojo y azul en cada pixel.
    //
    // Antes se hacia pixel a pixel, sacando los tres bytes y recolocandolos.
    // Era la mitad de cada composicion (6.75 ms de 13.4 medidos en la
    // Pi). Ahora se hace de DOS EN DOS PIXELES con palabras de 64 bits: el
    // azul (byte 0 de cada pixel) sube 16 bits, el rojo (byte 2) baja 16,
    // y el verde se queda. Las mascaras hacen que nada pase de un pixel al
    // otro, y el byte alto de cada pixel queda a 0, igual que antes.
    //
    // El pixel suelto -- el primero si la fila no empieza alineada a 8
    // bytes, y el ultimo si quedan impares -- se hace por separado, con la
    // misma formula en 32 bits.
    const uint64_t AZUL  = 0x000000FF000000FFULL;
    const uint64_t VERDE = 0x0000FF000000FF00ULL;
    for (uint32_t y = 0; y < height_px; y++) {
        const uint32_t *orig = (const uint32_t *)
            (back_buffer + y * (back_stride_px * FB_BPP));
        uint32_t *dest = (uint32_t *)(fb_real + y * fb_real_pitch);
        uint32_t x = 0;

        if (((uintptr_t)dest & 7) != 0 && x < width_px) {   // alinear a 8
            uint32_t c = orig[x];
            dest[x] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
            x++;
        }
        for (; x + 1 < width_px; x += 2) {
            uint64_t v;
            __builtin_memcpy(&v, &orig[x], 8);   // el origen puede no estar alineado
            uint64_t bgr = ((v & AZUL) << 16) | (v & VERDE) | ((v >> 16) & AZUL);
            *(uint64_t *)&dest[x] = bgr;
        }
        if (x < width_px) {                                  // pixel final impar
            uint32_t c = orig[x];
            dest[x] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
        }
        limpiar_cache(dest, width_px * FB_BPP);
    }
    cache_a_la_ram();
}
