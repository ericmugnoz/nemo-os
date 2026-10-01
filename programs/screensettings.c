// screensettings.c — Nemo OS
//
// Configurar pantalla: elegir la resolucion con la que arranca el
// sistema. Programa separado de desktoped.c (el editor de iconos del
// escritorio) a proposito -- cada uno con su responsabilidad propia,
// mas simple y facil de razonar que un solo programa con pestañas
// para dos cosas sin relacion entre si.
//
// El cambio se GUARDA (SCREEN.CFG) pero no se aplica al momento --
// cambiar el framebuffer en caliente con ventanas ya abiertas es
// demasiado arriesgado. Hace falta Menu Start -> Reiniciar.

#include <stdint.h>
#include <stdbool.h>
#include "barra.h"

#define SYS_READ_CHAR        12
#define SYS_PUMP             14
#define SYS_DRAW_RECT        30
#define SYS_DRAW_TEXT        31
#define SYS_GET_WINDOW_SIZE  33
#define SYS_GET_SCREEN_SIZE  35
#define SYS_GET_MOUSE        34
#define SYS_DEFINE_BUTTON    36
#define SYS_GET_BUTTON_ID    37
#define SYS_CREATE_WINDOW    40
#define SYS_POLL_EVENT        8
#define EVENT_WINDOWCLOSE     0x803
#define SYS_SCREEN_RES_GET_PENDING 245
#define SYS_SCREEN_RES_SET_PENDING 246

static inline uint64_t syscall5(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    register uint64_t x4 __asm__("x4") = a4;
    register uint64_t x8 __asm__("x8") = num;
    __asm__ volatile("svc #0"
                      : "+r"(x0)
                      : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
                      : "memory");
    return x0;
}

static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    syscall5(SYS_DRAW_RECT, (uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h, color);
}
// ---- Fuente ----
// La sans de 12 px del sistema, la de la interfaz, en lugar de la 5x7 en
// mayusculas. draw_text compensa la altura para que el texto quede donde
// quedaba: las coordenadas son las de siempre, pensadas para glifos de
// 7 px (lo mismo que hace el kernel con UI_TEXT_DY). Si la fuente no se
// pudiera cargar, la del sistema sin compensar: nunca peor que antes.
#ifndef SYS_LOAD_FONT
#define SYS_LOAD_FONT 189
#endif
#ifndef SYS_SET_FONT
#define SYS_SET_FONT  191
#endif
static int texto_dy = 0;
static void draw_text(int x, int y, const char *s, uint32_t color) {
    syscall5(SYS_DRAW_TEXT, (uint64_t)x, (uint64_t)(y + texto_dy), (uint64_t)s, color, 0);
}
static void cargar_fuente_ui(void) {
    int64_t f = (int64_t)syscall5(SYS_LOAD_FONT, (uint64_t)"sans", 12, 0, 0, 0);
    if (f > 0) { syscall5(SYS_SET_FONT, (uint64_t)f, 0, 0, 0, 0); texto_dy = -3; }
}
static void define_button(uint32_t id, int x, int y, int w, int h, uint32_t color) {
    uint64_t packed_wh = ((uint64_t)(uint16_t)w << 16) | (uint16_t)h;
    syscall5(SYS_DEFINE_BUTTON, id, (uint64_t)x, (uint64_t)y, packed_wh, color);
}
static void draw_bevel(int x, int y, int w, int h, uint32_t face, bool raised) {
    draw_rect(x, y, w, h, face);
    if (w <= 0 || h <= 0) return;
    uint32_t c1 = raised ? 0x00FFFFFFu : 0x00404040u;
    uint32_t c2 = raised ? 0x00404040u : 0x00FFFFFFu;
    draw_rect(x, y, w, 1, c1);
    draw_rect(x, y, 1, h, c1);
    draw_rect(x, y + h - 1, w, 1, c2);
    draw_rect(x + w - 1, y, 1, h, c2);
}
static int append_dec(char *buf, int pos, uint32_t value) {
    if (value == 0) { buf[pos++] = '0'; return pos; }
    char digits[10]; int n = 0;
    while (value > 0) { digits[n++] = (char)('0' + value % 10); value /= 10; }
    while (n > 0) buf[pos++] = digits[--n];
    return pos;
}

// BUG REAL, SEGUNDO INTENTO: la primera correccion (una funcion con
// switch que devuelve el texto segun el indice) SEGUIA fallando en la
// practica. Motivo probable: con pocos casos y valores densos (0,1,2,3),
// el propio COMPILADOR puede decidir traducir el switch en una tabla
// de punteros interna -- exactamente el mismo problema de antes
// (puntero grabado como dato, con la direccion de enlazado, no
// valida si el programa carga en otro sitio), solo que generada por
// el optimizador en vez de escrita por mi. No hay forma de verlo con
// una simple lectura del codigo fuente -- para curarnos en salud, se
// quita CUALQUIER seleccion indexada de texto: cada resolucion se
// escribe en su PROPIA linea de codigo, con su PROPIA llamada a
// draw_text y su cadena literal directamente ahi (nunca en un array,
// nunca en un switch, nunca detras de una funcion que "elige" segun
// un numero) -- asi no le queda ninguna tabla que construir.
typedef struct { uint32_t w, h; } res_choice_t;
static const res_choice_t RES_CHOICES[] = {
    {1024, 768},
    {1280, 720},
    {1400, 900},
    {1600, 900},
};
#define RES_CHOICE_COUNT 4

#define RES_ROW_H 22
#define TOOLBAR_H 26
#define BTN_EXIT 1

static uint32_t pending_w = 0, pending_h = 0;
static uint32_t active_w = 0, active_h = 0;
static char status_msg[64] = "";
static int win_w, win_h;
static bool running = true;

static void set_status(const char *s) {
    int i = 0; while (s[i] && i < 63) { status_msg[i] = s[i]; i++; } status_msg[i] = '\0';
}

static void redraw(void) {
    draw_rect(0, 0, win_w, win_h, 0x00404850);

    // La barra. Aqui solo hay un boton, anclado a la derecha, asi que no
    // puede solaparse con nadie -- pero la x sale de barra.h igual que en
    // el explorador y el editor de escritorio, y no de un "win_w - 62"
    // escrito tres veces. El dia que se añada un segundo boton, el
    // reparto ya esta: barra_iniciar(&b, 4, x_salir, 4) y a pedir hueco.
    // Empezar con el numero a mano es como llegaron los otros dos al
    // fallo que barra.h vino a arreglar.
    const int x_salir = barra_anclar_derecha(win_w, 58, 4);
    draw_bevel(0, 0, win_w, TOOLBAR_H, 0x00C0C0C0, true);
    draw_text(8, 8, "Configurar pantalla", 0x00000000);
    define_button(BTN_EXIT, x_salir, 3, 58, TOOLBAR_H - 6, 0x00A05050);
    draw_bevel(x_salir, 3, 58, TOOLBAR_H - 6, 0x00A05050, true);
    draw_text(x_salir + 8, 8, "Salir", 0x00000000);

    int y = TOOLBAR_H + 14;
    draw_text(10, y, "Resolución de pantalla", 0x00FFFFFF); y += 20;

    char line[48]; int p2 = 0;
    const char *a = "Activa ahora: "; int j = 0;
    while (a[j]) line[p2++] = a[j++];
    p2 = append_dec(line, p2, active_w); line[p2++] = 'x'; p2 = append_dec(line, p2, active_h);
    line[p2] = '\0';
    draw_text(10, y, line, 0x00C0C0C0); y += 24;

    // Las cajas SI pueden ir en bucle (son solo rectangulos, ningun
    // texto ni puntero implicado -- ver la nota de mas arriba).
    bool is_pending[RES_CHOICE_COUNT];
    for (int i = 0; i < RES_CHOICE_COUNT; i++) {
        int ry = y + i * RES_ROW_H;
        is_pending[i] = (RES_CHOICES[i].w == pending_w && RES_CHOICES[i].h == pending_h);
        draw_bevel(10, ry, win_w - 20, RES_ROW_H - 4, is_pending[i] ? 0x00305080 : 0x00C0C0C0, true);
        if (is_pending[i]) draw_text(win_w - 90, ry + 5, "Guardada", 0x00FFFFFF);
    }
    // Las 4 etiquetas: cada una en su propia linea, texto literal
    // directamente en la llamada -- nunca en un array ni un switch.
    draw_text(18, y + 0 * RES_ROW_H + 5, "1024 × 768  (segura de siempre)", is_pending[0] ? 0x00FFFFFF : 0x00000000);
    draw_text(18, y + 1 * RES_ROW_H + 5, "1280 × 720",                     is_pending[1] ? 0x00FFFFFF : 0x00000000);
    draw_text(18, y + 2 * RES_ROW_H + 5, "1400 × 900",                     is_pending[2] ? 0x00FFFFFF : 0x00000000);
    draw_text(18, y + 3 * RES_ROW_H + 5, "1600 × 900",                     is_pending[3] ? 0x00FFFFFF : 0x00000000);
    int end_y = y + RES_CHOICE_COUNT * RES_ROW_H + 8;
    draw_text(10, end_y, "Al elegir una: menú Inicio -> Reiniciar para aplicarla", 0x00FFFF80);
    if (status_msg[0]) draw_text(10, end_y + 20, status_msg, 0x0080FF80);
}

__attribute__((section(".text.start")))
void _start(void) {
    cargar_fuente_ui();
    syscall5(SYS_CREATE_WINDOW, (uint64_t)"Configurar pantalla", 80, 80, 480, 320);

    uint64_t size = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
    win_w = (int)(size >> 32);
    win_h = (int)(size & 0xFFFFFFFF);
    if (win_w <= 0) win_w = 480;
    if (win_h <= 0) win_h = 320;

    uint64_t r = syscall5(SYS_SCREEN_RES_GET_PENDING, 0, 0, 0, 0, 0);
    pending_w = (uint32_t)(r >> 32);
    pending_h = (uint32_t)(r & 0xFFFFFFFF);
    uint64_t act = syscall5(SYS_GET_SCREEN_SIZE, 0, 0, 0, 0, 0);
    active_w = (uint32_t)(act >> 32);
    active_h = (uint32_t)(act & 0xFFFFFFFF);
    if (pending_w == 0 || pending_h == 0) { pending_w = active_w; pending_h = active_h; }

    set_status("Elige una resolución de la lista");
    redraw();

    bool last_left = false;

    while (running) {
        uint64_t pump = syscall5(SYS_PUMP, 0, 0, 0, 0, 0);
        if ((int64_t)pump < 0) break;
        if ((int64_t)syscall5(SYS_POLL_EVENT, 0, 0, 0, 0, 0) == EVENT_WINDOWCLOSE) break;

        uint64_t sz = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
        int nw = (int)(sz >> 32), nh = (int)(sz & 0xFFFFFFFF);
        bool need_redraw = (nw > 0 && nw != win_w) || (nh > 0 && nh != win_h);
        if (nw > 0) win_w = nw;
        if (nh > 0) win_h = nh;

        uint32_t btn = (uint32_t)syscall5(SYS_GET_BUTTON_ID, 0, 0, 0, 0, 0);
        if (btn == BTN_EXIT) { running = false; break; }

        uint64_t m = syscall5(SYS_GET_MOUSE, 0, 0, 0, 0, 0);
        if (m != (uint64_t)-1) {
            int mx = (int)((m >> 32) & 0xFFFF);
            int my = (int)((m >> 16) & 0xFFFF);
            bool left = (m & 1) != 0;

            if (left && !last_left) {
                int y0 = TOOLBAR_H + 14 + 20 + 24;
                if (my >= y0 && my < y0 + RES_CHOICE_COUNT * RES_ROW_H && mx >= 10 && mx < win_w - 10) {
                    int i = (my - y0) / RES_ROW_H;
                    if (i >= 0 && i < RES_CHOICE_COUNT) {
                        pending_w = RES_CHOICES[i].w;
                        pending_h = RES_CHOICES[i].h;
                        syscall5(SYS_SCREEN_RES_SET_PENDING, (uint64_t)pending_w, (uint64_t)pending_h, 0, 0, 0);
                        set_status("Guardado: reinicia para aplicarla");
                    }
                }
                need_redraw = true;
            }
            last_left = left;
        } else {
            last_left = false;
        }

        char c = (char)syscall5(SYS_READ_CHAR, 0, 0, 0, 0, 0);
        if (c == 'q' || c == 'Q') running = false;

        if (need_redraw) redraw();
    }
}
