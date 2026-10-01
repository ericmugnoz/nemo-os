// desktoped.c — Nemo OS
//
// Editor de escritorio: deja colocar programas en el escritorio,
// eligiendo icono y posicion (en una rejilla, para que quede
// ordenado), quitarlos, cambiarles el icono, o moverlos. Los cambios
// se guardan en disco al momento (DESKTOP.CFG), asi que sobreviven al
// reinicio.
//
// Cuadricula fija, sin arrastrar -- todo es "selecciona, luego haz
// clic en una casilla": mas predecible que arrastrar, y nada queda
// nunca a medio pixel entre dos casillas.
//
// Flujo:
//   AÑADIR -> elige el programa (.pro) con el dialogo de abrir de
//   siempre -> aparece con un icono generico, listo para colocar con
//   un clic en una casilla libre de la rejilla.
//   Clic en un icono ya puesto -> lo selecciona: QUITAR lo borra,
//   clic en el catalogo de iconos (abajo) le cambia el dibujo, y un
//   clic en una casilla libre lo mueve ahi.

#include <stdint.h>
#include <stdbool.h>
#include "barra.h"   // reparto de la barra de botones, compartido

#define SYS_READ_CHAR        12
#define SYS_PUMP             14
#define SYS_DRAW_RECT        30
#define SYS_DRAW_TEXT        31
#define SYS_DRAW_ICON        32
#define SYS_GET_WINDOW_SIZE  33
#define SYS_GET_SCREEN_SIZE  35
#define SYS_GET_MOUSE        34
#define SYS_DEFINE_BUTTON    36
#define SYS_GET_BUTTON_ID    37
#define SYS_OPEN_FILE_DIALOG 38
#define SYS_CREATE_WINDOW    40
#define SYS_POLL_EVENT        8
#define EVENT_WINDOWCLOSE     0x803
#define SYS_ICON_CATALOG_COUNT      235
#define SYS_DESKTOP_ICON_COUNT      236
#define SYS_DESKTOP_ICON_GET        237
#define SYS_DESKTOP_ICON_ADD        238
#define SYS_DESKTOP_ICON_REMOVE     239
#define SYS_DESKTOP_ICON_MOVE       240
#define SYS_DESKTOP_ICON_SET_GRAPHIC 241
#define SYS_DESKTOP_SAVE            242
#define SYS_DESKTOP_ICON_SCALE_GET   243
#define SYS_DESKTOP_ICON_SCALE_SET   244

#define ICON_PROGRAM 3
#define ICON_SIZE 24   // tamaño base de todos los iconos del catalogo

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
static void draw_icon(int x, int y, int icon_id) {
    syscall5(SYS_DRAW_ICON, (uint64_t)x, (uint64_t)y, (uint64_t)icon_id, 1, 0);
}
static void draw_icon_scaled(int x, int y, int icon_id, int scale) {
    syscall5(SYS_DRAW_ICON, (uint64_t)x, (uint64_t)y, (uint64_t)icon_id, (uint64_t)scale, 0);
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

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static char to_upper_ascii(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

// -- estado --
#define MAX_ICONS 40
typedef struct {
    char label[16];
    char target[32];
    int32_t icon_id, x, y;
} icon_view_t;
static icon_view_t view[MAX_ICONS];
static int32_t view_count = 0;

static int32_t screen_w, screen_h;
static int win_w, win_h;
static bool running = true;

// GRID_CELL sigue la MISMA regla que icon_cell_w/h en wm.c -- se
// recalcula al arrancar (y al cambiar el ajuste) segun icon_scale,
// para que la vista previa coincida siempre con el escritorio real.
static int GRID_CELL_v = 56;
#define GRID_CELL GRID_CELL_v
static int32_t icon_scale = 1;   // 1 o 2 -- se consulta al arrancar y se cambia con el boton de la barra

static void recompute_layout(void) {
    GRID_CELL_v = (icon_scale == 2) ? 80 : 56;
}
#define TOOLBAR_H 26
static int32_t catalog_count = 0;   // se usa en el calculo de filas, mas abajo

// El catalogo era UNA fila y se cortaba al llegar al borde:
// con 13 iconos cabia, pero al pasar a 25 los ultimos quedaban fuera de
// la ventana y no habia forma de elegirlos. Ahora se reparte en las filas
// que hagan falta y la franja crece con ellas.
#define CAT_PASO 32            // lo que ocupa un icono del catalogo, con su hueco
#define CAT_FILA_H 32
static int catalog_cols(void) {
    int c = (win_w - 12) / CAT_PASO;
    return c < 1 ? 1 : c;
}
static int catalog_filas(void) {
    int cols = catalog_cols();
    int n = catalog_count < 1 ? 1 : catalog_count;
    return (n + cols - 1) / cols;
}
static int catalog_alto(void) { return catalog_filas() * CAT_FILA_H + 8; }
#define CATALOG_H (catalog_alto())
#define STATUS_H  16   // franja del mensaje de estado, encima del catalogo

// ---- La vista del escritorio ----
// La zona de la ventana donde se ve el escritorio real empieza DEBAJO de
// la barra de botones. Antes se dibujaba y se hacia clic como si el
// (0,0) del escritorio fuera el (0,0) de la ventana: los iconos de arriba
// se pintaban encima de los botones, cada clic caia 26 px desplazado (un
// icono se podia dejar "encima" de la barra) y los de abajo se montaban
// con el mensaje de estado. Ahora: vista = escritorio + (0, VISTA_Y), en
// el dibujo y en los clics, y solo valen casillas que se vean ENTERAS.
#define VISTA_Y TOOLBAR_H
static int vista_h(void) { return win_h - TOOLBAR_H - CATALOG_H - STATUS_H; }

// ¿Se ve ENTERA en la ventana la casilla del escritorio que empieza en (x, y)?
static bool en_vista(int32_t x, int32_t y) {
    return x >= 0 && y >= 0 && x + GRID_CELL <= win_w && y + GRID_CELL <= vista_h();
}
#define BTN_ADD    1
#define BTN_REMOVE 2
#define BTN_EXIT   3
#define BTN_TOGGLE_SIZE 4
#define BTN_RENAME 5
#define SYS_DESKTOP_ICON_SET_LABEL 275

static int32_t selected = -1;    // indice en 'view' del icono seleccionado (-1 = ninguno)
static bool placing = false;     // hay un icono nuevo listo para colocar (esperando clic en la rejilla)
static char pending_target[32] = "";
static char pending_label[16] = "";
static int32_t pending_icon = ICON_PROGRAM;



static char status_msg[64] = "";
static void set_status(const char *s) {
    int i = 0; while (s[i] && i < 63) { status_msg[i] = s[i]; i++; } status_msg[i] = '\0';
}

// -- cuadro de texto para la etiqueta (mismo estilo que "Renombrar" del explorador) --
// El cuadro de texto se usa para DOS cosas: poner el nombre de un icono
// nuevo y cambiar el de uno que ya esta. 'renombrando' distingue cual,
// porque al aceptar hay que hacer cosas distintas.
static bool naming = false;
static bool renombrando = false;
static char name_buf[16] = "";
static int name_len = 0;

static void reload_view(void) {
    view_count = 0;
    int32_t total = (int32_t)syscall5(SYS_DESKTOP_ICON_COUNT, 0, 0, 0, 0, 0);
    for (int32_t i = 0; i < total && view_count < MAX_ICONS; i++) {
        int32_t ints[3];
        int64_t ok = (int64_t)syscall5(SYS_DESKTOP_ICON_GET, (uint64_t)i,
                                       (uint64_t)view[view_count].label,
                                       (uint64_t)view[view_count].target,
                                       (uint64_t)ints, 0);
        if (ok) {
            view[view_count].icon_id = ints[0];
            view[view_count].x = ints[1];
            view[view_count].y = ints[2];
            view_count++;
        }
    }
}

// Snap de una posicion cualquiera a la rejilla, dentro de los limites
// de la pantalla real (para que nunca se coloque un icono fuera de la
// vista, o encima de la barra de tareas).
static void snap(int px, int py, int32_t *out_x, int32_t *out_y) {
    int gx = (px / GRID_CELL) * GRID_CELL + 4;
    int gy = (py / GRID_CELL) * GRID_CELL + 4;
    if (gx < 4) gx = 4;
    if (gy < 4) gy = 4;
    if (gx > screen_w - GRID_CELL) gx = screen_w - GRID_CELL;
    if (gy > screen_h - GRID_CELL - 40) gy = screen_h - GRID_CELL - 40; // 40 ~ barra de tareas
    *out_x = gx; *out_y = gy;
}

static int32_t find_icon_at(int px, int py) {
    for (int32_t i = 0; i < view_count; i++) {
        if (px >= view[i].x && px < view[i].x + GRID_CELL - 4 && py >= view[i].y && py < view[i].y + GRID_CELL - 4) return i;
    }
    return -1;
}

static void begin_add(void) {
    // Elegir el programa con el dialogo de abrir de siempre -- 0 =
    // empezar en la raiz de NemoFS.
    static char name_buf2[28];
    uint64_t r = syscall5(SYS_OPEN_FILE_DIALOG, 0, (uint64_t)name_buf2, sizeof(name_buf2), 0, 0);
    if (r == (uint64_t)-1) { set_status("Cancelado"); return; }
    int i = 0; while (name_buf2[i] && i < 31) { pending_target[i] = name_buf2[i]; i++; }
    pending_target[i] = '\0';

    // Etiqueta por defecto: el nombre sin ".PRO", en mayusculas, hasta
    // 15 caracteres -- el usuario la puede cambiar en el cuadro que se
    // abre a continuacion.
    int len = str_len(pending_target);
    int base = len;
    for (int k = len - 1; k > 0; k--) if (pending_target[k] == '.') { base = k; break; }
    if (base > 15) base = 15;
    int j = 0;
    while (j < base) { pending_label[j] = to_upper_ascii(pending_target[j]); j++; }
    pending_label[j] = '\0';

    pending_icon = ICON_PROGRAM;
    name_len = str_len(pending_label);
    j = 0; while (pending_label[j]) { name_buf[j] = pending_label[j]; j++; } name_buf[j] = '\0';
    naming = true;
}

// Empieza a cambiar el nombre del icono seleccionado. Se parte del que
// ya tiene, para corregir una errata sin reescribirlo entero.
static void begin_rename(void) {
    if (selected < 0) { set_status("Elige antes un icono"); return; }
    int j = 0;
    while (j < 15 && view[selected].label[j]) { name_buf[j] = view[selected].label[j]; j++; }
    name_buf[j] = '\0';
    name_len = j;
    naming = true;
    renombrando = true;
    set_status("Escribe el nombre nuevo y pulsa Intro");
}

static void finish_naming(bool accept) {
    naming = false;
    if (renombrando) {
        renombrando = false;
        if (!accept || name_len == 0) { set_status("Cancelado"); return; }
        if (syscall5(SYS_DESKTOP_ICON_SET_LABEL, (uint64_t)selected, (uint64_t)name_buf, 0, 0, 0) == 0) {
            syscall5(SYS_DESKTOP_SAVE, 0, 0, 0, 0, 0);   // que sobreviva al reinicio
            reload_view();
            set_status("Renombrado");
        } else {
            set_status("No se pudo renombrar");
        }
        return;
    }
    if (!accept || name_len == 0) { set_status("Cancelado"); return; }
    int i = 0; while (name_buf[i]) { pending_label[i] = name_buf[i]; i++; } pending_label[i] = '\0';
    placing = true;
    set_status("Elige el icono abajo si quieres, y haz clic en una casilla libre");
}

static void place_pending(int px, int py) {
    int32_t x, y;
    snap(px, py, &x, &y);
    if (!en_vista(x - 4, y - 4)) { set_status("Esa casilla no se ve entera: elige otra o amplía la ventana"); return; }   // snap da el icono, 4 px dentro de su casilla
    if (find_icon_at(x, y) >= 0) { set_status("Ya hay algo ahí: prueba otra casilla"); return; }
    int64_t idx = (int64_t)syscall5(SYS_DESKTOP_ICON_ADD, (uint64_t)pending_label, (uint64_t)pending_target,
                                    (uint64_t)pending_icon, (uint64_t)x, (uint64_t)y);
    if (idx < 0) { set_status("No se pudo añadir (¿escritorio lleno?)"); return; }
    syscall5(SYS_DESKTOP_SAVE, 0, 0, 0, 0, 0);
    placing = false;
    reload_view();
    set_status("Añadido y guardado");
}

// Mueve el icono ya seleccionado a la casilla bajo (px,py) --
// cuadricula fija, sin arrastrar: se selecciona uno y se hace clic en
// donde se quiere que quede.
static void move_selected(int px, int py) {
    int32_t x, y;
    snap(px, py, &x, &y);
    if (!en_vista(x - 4, y - 4)) { set_status("Esa casilla no se ve entera: elige otra o amplía la ventana"); return; }   // snap da el icono, 4 px dentro de su casilla
    int32_t hit = find_icon_at(x, y);
    if (hit >= 0 && hit != selected) { set_status("Ya hay algo ahí: elige otra casilla"); return; }
    syscall5(SYS_DESKTOP_ICON_MOVE, (uint64_t)selected, (uint64_t)x, (uint64_t)y, 0, 0);
    syscall5(SYS_DESKTOP_SAVE, 0, 0, 0, 0, 0);
    reload_view();
    set_status("Movido y guardado");
}

static void remove_selected(void) {
    if (selected < 0) { set_status("Nada seleccionado"); return; }
    if ((int64_t)syscall5(SYS_DESKTOP_ICON_REMOVE, (uint64_t)selected, 0, 0, 0, 0) == 0) {
        syscall5(SYS_DESKTOP_SAVE, 0, 0, 0, 0, 0);
        selected = -1;
        reload_view();
        set_status("Quitado y guardado");
    } else {
        set_status("No se pudo quitar");
    }
}

static void catalog_click(int catalog_icon) {
    if (placing) {
        pending_icon = catalog_icon;
        set_status("Icono elegido: haz clic en una casilla libre del escritorio");
    } else if (selected >= 0) {
        syscall5(SYS_DESKTOP_ICON_SET_GRAPHIC, (uint64_t)selected, (uint64_t)catalog_icon, 0, 0, 0);
        syscall5(SYS_DESKTOP_SAVE, 0, 0, 0, 0, 0);
        reload_view();
        set_status("Icono cambiado y guardado");
    } else {
        set_status("Selecciona un icono del escritorio primero, o pulsa Añadir");
    }
}

static void redraw(void) {
    draw_rect(0, 0, win_w, win_h, 0x00404850);

    // Barra de herramientas. El reparto lo lleva barra.h: cada boton
    // pide su hueco y, si no cabe antes de "Salir", sencillamente no se
    // dibuja NI se registra su zona pulsable. Antes esto eran
    // coordenadas a mano (4, 78, 152, 246) con una condicion a medida
    // para el ultimo; el boton de tamaño acabo debajo del de Salir.
    draw_bevel(0, 0, win_w, TOOLBAR_H, 0x00C0C0C0, true);
    const int alto_btn = TOOLBAR_H - 6;
    const int x_salir = barra_anclar_derecha(win_w, 58, 4);

    barra_t b;
    barra_iniciar(&b, 4, x_salir, 4);
    int x;

    if (barra_hueco(&b, 70, &x)) {
        define_button(BTN_ADD, x, 3, 70, alto_btn, 0x00C0C0C0);
        draw_bevel(x, 3, 70, alto_btn, 0x00C0C0C0, true);
        draw_text(x + 10, 8, "Añadir", 0x00000000);
    }
    if (barra_hueco(&b, 70, &x)) {
        define_button(BTN_REMOVE, x, 3, 70, alto_btn, 0x00C0C0C0);
        draw_bevel(x, 3, 70, alto_btn, 0x00C0C0C0, true);
        draw_text(x + 6, 8, "Quitar", 0x00000000);
    }
    if (barra_hueco(&b, 90, &x)) {
        define_button(BTN_RENAME, x, 3, 90, alto_btn, 0x00C0C0C0);
        draw_bevel(x, 3, 90, alto_btn, 0x00C0C0C0, true);
        draw_text(x + 6, 8, "Renombrar", 0x00000000);
    }
    // El del tamaño de iconos prefiere su rotulo largo, y se conforma
    // con el corto si no queda sitio para el.
    {
        int w_tam = (barra_queda(&b) >= 120) ? 120 : 76;
        bool largo = (w_tam == 120);
        if (barra_hueco(&b, w_tam, &x)) {
            define_button(BTN_TOGGLE_SIZE, x, 3, w_tam, alto_btn, 0x00C0C0C0);
            draw_bevel(x, 3, w_tam, alto_btn, 0x00C0C0C0, icon_scale != 2);
            if (largo) draw_text(x + 6, 8, icon_scale == 2 ? "Iconos: grandes" : "Iconos: normales", 0x00000000);
            else       draw_text(x + 6, 8, icon_scale == 2 ? "Iconos 2x" : "Iconos 1x", 0x00000000);
        }
    }
    define_button(BTN_EXIT, x_salir, 3, 58, alto_btn, 0x00A05050);
    draw_bevel(x_salir, 3, 58, alto_btn, 0x00A05050, true);
    draw_text(x_salir + 8, 8, "Salir", 0x00000000);

    // Vista en miniatura del escritorio, a escala 1:1 si cabe (si la
    // ventana es mas pequeña que la pantalla real, se ve recortada --
    // suficiente para colocar iconos, que suelen ir en la zona
    // superior izquierda).
    int desk_y = VISTA_Y;
    int desk_h = vista_h();
    draw_bevel(0, desk_y, win_w, desk_h, 0x00203040, false);

    // Rejilla, tenue, para que se vea donde encaja cada icono
    for (int gx = 0; gx < win_w; gx += GRID_CELL) draw_rect(gx, desk_y, 1, desk_h, 0x00304050);
    for (int gy = desk_y; gy < desk_y + desk_h; gy += GRID_CELL) draw_rect(0, gy, win_w, 1, 0x00304050);

    for (int32_t i = 0; i < view_count; i++) {
        // posicion en el escritorio real -> en la vista (debajo de la barra)
        if (!en_vista(view[i].x - 4, view[i].y - 4)) continue;   // solo los que se ven enteros
        int ix = view[i].x, iy = view[i].y + desk_y;
        if (i == selected) draw_rect(ix - 2, iy - 2, GRID_CELL - 6, GRID_CELL - 6, 0x00305080);
        draw_icon_scaled(ix + 4, iy, view[i].icon_id, icon_scale);
        draw_text(ix, iy + ICON_SIZE * icon_scale + 6, view[i].label, 0x00FFFFFF);
    }


    // Catalogo de iconos, al fondo -- clic para elegir (nuevo icono o
    // para cambiarselo al seleccionado)
    int cat_y = win_h - CATALOG_H;
    draw_bevel(0, cat_y, win_w, CATALOG_H, 0x00C0C0C0, true);
    {
        int cols = catalog_cols();
        for (int32_t i = 0; i < catalog_count; i++) {
            int cx = 6 + ((int)i % cols) * CAT_PASO;
            int cy = cat_y + 4 + ((int)i / cols) * CAT_FILA_H;
            bool hi = placing ? (i == pending_icon) : (selected >= 0 && i == view[selected].icon_id);
            if (hi) draw_rect(cx - 2, cy - 2, 28, 28, 0x00305080);
            draw_icon(cx, cy, (int)i);
        }
    }

    // El mensaje de estado, en su franja (antes, encima de la vista)
    draw_rect(0, cat_y - STATUS_H, win_w, STATUS_H, 0x00182430);
    if (status_msg[0]) draw_text(4, cat_y - STATUS_H + 5, status_msg, 0x00FFFF80);
    else if (placing) draw_text(4, cat_y - STATUS_H + 5, "Haz clic para colocar el nuevo icono...", 0x00FFFF80);

    // Cuadro de la etiqueta, si se esta escribiendo -- mismo estilo
    // que "Renombrar" del explorador.
    if (naming) {
        int w = 200, h = 54;
        int x = (win_w - w) / 2, y = (win_h - h) / 2;
        draw_bevel(x, y, w, h, 0x00F0F0F0, true);
        draw_text(x + 8, y + 6, "Nombre del icono:", 0x00000000);
        draw_bevel(x + 8, y + 20, w - 16, 16, 0x00FFFFFF, false);
        draw_text(x + 12, y + 24, name_buf, 0x00000000);
        draw_text(x + 8, y + 40, "Enter acepta, Esc cancela", 0x00606060);
    }
}

__attribute__((section(".text.start")))
void _start(void) {
    cargar_fuente_ui();
    syscall5(SYS_CREATE_WINDOW, (uint64_t)"Editor de escritorio", 60, 60, 560, 420);

    uint64_t sz = syscall5(SYS_GET_SCREEN_SIZE, 0, 0, 0, 0, 0);
    screen_w = (int32_t)(sz >> 32);
    screen_h = (int32_t)(sz & 0xFFFFFFFF);
    if (screen_w <= 0) screen_w = 800;
    if (screen_h <= 0) screen_h = 600;

    uint64_t size = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
    win_w = (int)(size >> 32);
    win_h = (int)(size & 0xFFFFFFFF);
    if (win_w <= 0) win_w = 560;
    if (win_h <= 0) win_h = 420;

    icon_scale = (int32_t)syscall5(SYS_DESKTOP_ICON_SCALE_GET, 0, 0, 0, 0, 0);
    if (icon_scale != 2) icon_scale = 1;
    recompute_layout();
    catalog_count = (int32_t)syscall5(SYS_ICON_CATALOG_COUNT, 0, 0, 0, 0, 0);
    reload_view();
    set_status("Pulsa Añadir para poner un programa en el escritorio");
    redraw();

    bool last_left = false;

    while (running) {
        uint64_t pump = syscall5(SYS_PUMP, 0, 0, 0, 0, 0);
        if ((int64_t)pump < 0) break;
        if ((int64_t)syscall5(SYS_POLL_EVENT, 0, 0, 0, 0, 0) == EVENT_WINDOWCLOSE) break;

        uint64_t sz2 = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
        int nw = (int)(sz2 >> 32), nh = (int)(sz2 & 0xFFFFFFFF);
        bool need_redraw = (nw > 0 && nw != win_w) || (nh > 0 && nh != win_h);
        if (nw > 0) win_w = nw;
        if (nh > 0) win_h = nh;

        uint32_t btn = (uint32_t)syscall5(SYS_GET_BUTTON_ID, 0, 0, 0, 0, 0);
        if (btn == BTN_ADD) { begin_add(); need_redraw = true; }
        else if (btn == BTN_REMOVE) { remove_selected(); need_redraw = true; }
        else if (btn == BTN_RENAME) { begin_rename(); need_redraw = true; }
        else if (btn == BTN_TOGGLE_SIZE) {
            icon_scale = (icon_scale == 2) ? 1 : 2;
            syscall5(SYS_DESKTOP_ICON_SCALE_SET, (uint64_t)icon_scale, 0, 0, 0, 0);
            recompute_layout();
            set_status(icon_scale == 2 ? "Iconos grandes (48x48)" : "Iconos normales (24x24)");
            need_redraw = true;
        }
        else if (btn == BTN_EXIT) { running = false; break; }

        uint64_t m = syscall5(SYS_GET_MOUSE, 0, 0, 0, 0, 0);
        if (m != (uint64_t)-1) {
            int mx = (int)((m >> 32) & 0xFFFF);
            int my = (int)((m >> 16) & 0xFFFF);
            bool left = (m & 1) != 0;

            if (left && !last_left && !naming) {
                int cat_y = win_h - CATALOG_H;
                int desk_y = VISTA_Y;
                if (my >= cat_y) {
                    int cols = catalog_cols();
                    int col = (mx - 6) / CAT_PASO;
                    int fila = (my - cat_y - 4) / CAT_FILA_H;
                    int idx = fila * cols + col;
                    if (col >= 0 && col < cols && fila >= 0 && idx >= 0 && idx < catalog_count) catalog_click(idx);
                } else if (my >= desk_y && my < desk_y + vista_h()) {
                    // de la ventana al escritorio real
                    int dx = mx, dy = my - desk_y;
                    if (placing) {
                        place_pending(dx, dy);
                    } else {
                        int32_t hit = find_icon_at(dx, dy);
                        if (hit >= 0) {
                            // Clic en un icono: lo selecciona (o, si ya
                            // habia otro seleccionado, cambia la
                            // seleccion a este -- nunca mueve nada aqui).
                            selected = hit;
                            set_status("Seleccionado: clic en otra casilla para moverlo, o usa Quitar / el catálogo");
                        } else if (selected >= 0) {
                            // Clic en una casilla vacia con algo ya
                            // seleccionado: lo mueve ahi. Cuadricula
                            // fija, nada de arrastrar.
                            move_selected(dx, dy);
                        } else {
                            selected = -1;
                        }
                    }
                }
                need_redraw = true;
            }
            last_left = left;
        } else {
            last_left = false;
        }

        char c = (char)syscall5(SYS_READ_CHAR, 0, 0, 0, 0, 0);
        if (naming) {
            if (c == 27) { finish_naming(false); need_redraw = true; }
            else if (c == '\n') { finish_naming(true); need_redraw = true; }
            else if (c == '\b') { if (name_len > 0) { name_len--; name_buf[name_len] = '\0'; need_redraw = true; } }
            else if (c >= 32 && c < 127 && name_len < 15) {
                char up = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
                name_buf[name_len++] = up;
                name_buf[name_len] = '\0';
                need_redraw = true;
            }
        }

        if (need_redraw) redraw();
    }
}
