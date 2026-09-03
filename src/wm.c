// wm.c — Nemo OS
//
// Gestor de ventanas v1. Sin aceleracion ni doble buffer real -- cada
// vez que algo cambia (raton movido, ventana arrastrada, boton
// pulsado), redibujamos la pantalla ENTERA en el orden correcto
// (fondo -> ventanas de atras a adelante -> barra de tareas ->
// cursor). Es el enfoque mas simple posible; mas adelante, cuando el
// rendimiento importe, se puede optimizar a "solo redibujar lo que
// cambio" -- pero eso añade bastante complejidad de "regiones sucias"
// que no merece la pena todavia.

#include "wm.h"
#include "ramfb.h"
#include "text.h"
#include "input.h"
#include "tasks.h"
#include "uart.h"
#include "font5x7.h"
#include "power.h"
#include "timer.h"
#include "icons_data.h"
#include "gadgets.h"

#define MAX_WINDOWS 8
#define TITLE_BAR_H 24
#define TASKBAR_H 32
#define START_BTN_W 70

#define COLOR_DESKTOP    0x00204060
#define COLOR_WIN_BODY   0x00D4D0C8
#define COLOR_WIN_TITLE  0x00000080
#define COLOR_WIN_BORDER 0x00000000
#define COLOR_TASKBAR    0x00C0C0C0
#define COLOR_START_BTN  0x00008000
#define COLOR_TASK_BTN   0x00A0A0A0
#define COLOR_TASK_BTN_ACTIVE 0x00808080
#define COLOR_CURSOR     0x00FF0000
#define COLOR_TEXT_LIGHT 0x00FFFFFF
#define COLOR_TEXT_DARK  0x00000000
#define COLOR_BTN_NORMAL 0x00404090
#define COLOR_BTN_CLOSE  0x00CC3333
#define COLOR_MENU_BG    0x00E8E8E8
#define COLOR_MENU_HOVER 0x00C0D0F0

#define WIN_BTN_SIZE 16
#define WIN_BTN_GAP  20 // separacion entre el borde derecho de cada boton

typedef struct {
    bool used;
    int32_t x, y;
    uint32_t w, h;
    char title[24];
    bool owns_content;
    bool minimized;
    bool maximized;
    int32_t restore_x, restore_y;
    uint32_t restore_w, restore_h;
    bool event_mode; // true tras CreateWindow() -- ver wm_set_event_mode
    // SetMinWindowSize -- almacenado pero SIN aplicar todavia: no
    // existe redimensionado interactivo por arrastre en este SO (solo
    // se puede mover la ventana), asi que no hay nada que restringir
    // de verdad por ahora. Guardado para cuando se implemente.
    uint32_t min_w, min_h;
} window_t;

// Buffer de contenido propio de cada ventana -- separado del back
// buffer general de la pantalla. Sin esto, cualquier redibujado
// disparado por mover el raton (needs_redraw) borraria lo que un
// programa hubiera dibujado dentro de su ventana, porque el
// redibujado general vuelve a pintar el fondo del escritorio encima
// de todo. Con este buffer, el contenido del programa persiste, y en
// cada redibujado simplemente lo "pegamos" de nuevo en su sitio.
#define MAX_CONTENT_W 1400
#define MAX_CONTENT_H 900
static uint8_t window_content[MAX_WINDOWS][MAX_CONTENT_H][MAX_CONTENT_W * 4];

// -- Botones nativos (SYS_DEFINE_BUTTON / SYS_GET_BUTTON_ID) --
#define MAX_BUTTONS_PER_WINDOW 8
typedef struct {
    bool used;
    uint32_t id;
    int32_t x, y;
    uint32_t w, h;
} ui_button_t;
static ui_button_t win_buttons[MAX_WINDOWS][MAX_BUTTONS_PER_WINDOW];
static bool btn_last_left[MAX_WINDOWS];

static window_t windows[MAX_WINDOWS];
static int32_t z_order[MAX_WINDOWS]; // indices en windows[], de atras (0) a adelante
static int32_t window_count = 0;

static int32_t dragging_window = -1;
static int32_t drag_offset_x = 0, drag_offset_y = 0;
static int32_t focused_window = -1; // ventana que recibe el teclado
static bool start_menu_open = false;
static bool launch_pending = false;
static char launch_target[32] = "";
static char launch_arg[32] = "";
static int32_t launch_requesting_window = -1; // -1 = lanzado sin "padre" (icono, menu Start...)
static uint32_t launch_search_dir = 0xFFFFFFFF; // 0xFFFFFFFF = sin preferencia (raiz, luego PROGRAMAS)

// -- Iconos de escritorio: extensibles, se añaden con wm_add_desktop_icon --
#define MAX_ICONS 12
#define ICON_W 56
#define ICON_H 44

typedef struct {
    bool used;
    int32_t x, y;
    char label[16];
    char target[32]; // nombre del archivo .pro a lanzar con doble clic
} desktop_icon_t;

static desktop_icon_t icons[MAX_ICONS];
static int32_t icon_count = 0;
static int32_t last_icon_clicked = -1;
static uint64_t last_icon_click_tick = 0;
#define DOUBLE_CLICK_TICKS 50 // medio segundo a 100Hz

static bool needs_redraw = true;
static bool last_left_down = false;

static void set_focus(int32_t idx);

void wm_init(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) windows[i].used = false;
    window_count = 0;
    needs_redraw = true;
}

void wm_add_desktop_icon(const char *label, const char *target_pro, int32_t x, int32_t y) {
    if (icon_count >= MAX_ICONS) return;
    desktop_icon_t *ic = &icons[icon_count];
    ic->used = true;
    ic->x = x;
    ic->y = y;
    int i = 0;
    while (label[i] != '\0' && i < 15) { ic->label[i] = label[i]; i++; }
    ic->label[i] = '\0';
    i = 0;
    while (target_pro[i] != '\0' && i < 31) { ic->target[i] = target_pro[i]; i++; }
    ic->target[i] = '\0';
    icon_count++;
    needs_redraw = true;
}

static void request_launch(const char *target_pro, const char *arg, int32_t requesting_window, uint32_t search_dir) {
    int i = 0;
    while (target_pro[i] != '\0' && i < 31) { launch_target[i] = target_pro[i]; i++; }
    launch_target[i] = '\0';
    i = 0;
    if (arg) {
        while (arg[i] != '\0' && i < 31) { launch_arg[i] = arg[i]; i++; }
    }
    launch_arg[i] = '\0';
    launch_requesting_window = requesting_window;
    launch_search_dir = search_dir;
    launch_pending = true;
}

int32_t wm_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h, const char *title) {
    if (window_count >= MAX_WINDOWS) return -1;

    int32_t idx = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].used) { idx = i; break; }
    }
    if (idx < 0) return -1;

    windows[idx].used = true;
    windows[idx].x = x;
    windows[idx].y = y;
    windows[idx].w = w;
    windows[idx].h = h;
    int i = 0;
    while (title[i] != '\0' && i < 23) { windows[idx].title[i] = title[i]; i++; }
    windows[idx].title[i] = '\0';
    windows[idx].owns_content = false;
    windows[idx].minimized = false;
    windows[idx].maximized = false;
    windows[idx].event_mode = false;
    for (int b = 0; b < MAX_BUTTONS_PER_WINDOW; b++) win_buttons[idx][b].used = false;
    btn_last_left[idx] = false;

    z_order[window_count] = idx;
    window_count++;

    set_focus(idx);
    needs_redraw = true;
    return idx;
}

// Reconfigura una ventana ya existente -- ver wm.h.
void wm_configure_window(int32_t idx, const char *title, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    windows[idx].x = x;
    windows[idx].y = y;
    windows[idx].w = w;
    windows[idx].h = h;
    int i = 0;
    while (title[i] != '\0' && i < 23) { windows[idx].title[i] = title[i]; i++; }
    windows[idx].title[i] = '\0';
    needs_redraw = true;
}

// Solo cambia el titulo, sin tocar posicion/tamaño -- para AppTitle().
// wm_configure_window SIEMPRE sobreescribe los cuatro, asi que
// reutilizarla aqui habria pedido leerlos primero; mas simple y
// seguro tener una funcion aparte que solo toca lo que hace falta.
void wm_set_title(int32_t idx, const char *title) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    int i = 0;
    while (title[i] != '\0' && i < 23) { windows[idx].title[i] = title[i]; i++; }
    windows[idx].title[i] = '\0';
    needs_redraw = true;
}

void wm_set_event_mode(int32_t idx, bool on) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    windows[idx].event_mode = on;
}

// Mueve el indice dado al final de z_order (lo trae al frente)
static void raise_window(int32_t win_idx) {
    int pos = -1;
    for (int i = 0; i < window_count; i++) {
        if (z_order[i] == win_idx) { pos = i; break; }
    }
    if (pos < 0 || pos == window_count - 1) return; // no encontrada o ya al frente

    for (int i = pos; i < window_count - 1; i++) {
        z_order[i] = z_order[i + 1];
    }
    z_order[window_count - 1] = win_idx;
    needs_redraw = true;
}

static bool point_in_rect(int32_t px, int32_t py, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    return px >= x && px < x + (int32_t)w && py >= y && py < y + (int32_t)h;
}

// Cambia la ventana con foco de teclado, vaciando cualquier tecla
// pendiente si de verdad cambiamos de ventana -- asi lo que escribas
// en una ventana no aparece de golpe en otra al cambiar el foco.
static void set_focus(int32_t idx) {
    if (idx != focused_window) {
        input_flush_chars();
        focused_window = idx;
    }
}

static void toggle_maximize(int32_t idx) {
    window_t *w = &windows[idx];
    if (!w->maximized) {
        w->restore_x = w->x;
        w->restore_y = w->y;
        w->restore_w = w->w;
        w->restore_h = w->h;
        w->x = 0;
        w->y = 0;
        w->w = fb_width();
        w->h = fb_height() - TASKBAR_H - TITLE_BAR_H;
        w->maximized = true;
    } else {
        w->x = w->restore_x;
        w->y = w->restore_y;
        w->w = w->restore_w;
        w->h = w->restore_h;
        w->maximized = false;
    }
    if (w->event_mode) gadgets_fire_raw_event(idx, EVENT_WINDOWSIZE, 0, 0);
    needs_redraw = true;
}

// Pantalla de apagado/reinicio -- en vez de dejar al usuario con una
// pantalla negra sin explicacion (que es justo lo que pasaba antes),
// mostramos un mensaje claro, presentamos ese fotograma, y solo
// entonces detenemos la maquina.
static void show_shutdown_screen(const char *message) {
    fb_fill_rect(0, 0, fb_width(), fb_height(), 0x00000000);
    uint32_t tw = text_width(message, 3);
    fb_draw_string((fb_width() - tw) / 2, fb_height() / 2, message, COLOR_TEXT_LIGHT, 3);
    fb_present();
}

void wm_update(void) {
    int32_t mx = mouse_x();
    int32_t my = mouse_y();
    bool left = mouse_left_down();
    bool left_pressed_edge = left && !last_left_down;
    bool left_released_edge = !left && last_left_down;

    // El raton se movio -> hay que redibujar (el cursor esta en otro sitio)
    static int32_t last_mx = -1, last_my = -1;
    if (mx != last_mx || my != last_my) {
        needs_redraw = true;
        last_mx = mx;
        last_my = my;
    }

    if (left_pressed_edge) {
        uint32_t fbh = fb_height();

        // Menu Start abierto -- comprobamos sus filas antes que nada
        if (start_menu_open) {
            int32_t menu_x = 4;
            int32_t menu_w = 140;
            int32_t row_h = 20;
            int32_t total_rows = 7; // titulo + 4 programas + separador + apagar + reiniciar
            int32_t menu_top = (int32_t)fbh - TASKBAR_H - (row_h * total_rows);

            if (point_in_rect(mx, my, menu_x, menu_top, (uint32_t)menu_w, (uint32_t)(row_h * total_rows))) {
                int32_t row = (my - menu_top) / row_h;
                if (row == 1) {
                    request_launch("shell.pro", 0, -1, 0xFFFFFFFF);
                } else if (row == 2) {
                    request_launch("explorer.pro", 0, -1, 0xFFFFFFFF);
                } else if (row == 3) {
                    request_launch("editor.pro", 0, -1, 0xFFFFFFFF);
                } else if (row == 4) {
                    request_launch("ide.pro", 0, -1, 0xFFFFFFFF);
                } else if (row == 5) { // "APAGAR"
                    show_shutdown_screen("APAGANDO NEMO OS...");
                    power_shutdown();
                } else if (row == 6) { // "REINICIAR"
                    show_shutdown_screen("REINICIANDO...");
                    power_reset();
                }
            }
            start_menu_open = false;
            needs_redraw = true;
        }
        // Boton Start
        else if (point_in_rect(mx, my, 4, (int32_t)fbh - TASKBAR_H + 4, START_BTN_W, TASKBAR_H - 8)) {
            start_menu_open = true;
            needs_redraw = true;
        } else {
            // Botones de la barra de tareas (uno por ventana abierta)
            int32_t bx = 4 + START_BTN_W + 8;
            bool hit_taskbar_btn = false;
            for (int i = 0; i < window_count; i++) {
                window_t *w = &windows[z_order[i]];
                if (!w->used) continue;
                if (point_in_rect(mx, my, bx, (int32_t)fbh - TASKBAR_H + 4, 90, TASKBAR_H - 8)) {
                    w->minimized = false;
                    raise_window(z_order[i]);
                    set_focus(z_order[i]);
                    hit_taskbar_btn = true;
                    break;
                }
                bx += 96;
            }

            if (!hit_taskbar_btn && my < (int32_t)fbh - TASKBAR_H) {
                bool hit_window = false;
                // Buscamos la ventana mas al frente (visible) que contenga el punto
                for (int i = window_count - 1; i >= 0; i--) {
                    window_t *w = &windows[z_order[i]];
                    if (!w->used || w->minimized) continue;
                    if (point_in_rect(mx, my, w->x, w->y, w->w, w->h + TITLE_BAR_H)) {
                        hit_window = true;
                        int32_t win_idx = z_order[i];

                        // Botones de la barra de titulo: cerrar, maximizar, minimizar
                        int32_t close_x = w->x + (int32_t)w->w - WIN_BTN_GAP;
                        int32_t max_x   = w->x + (int32_t)w->w - WIN_BTN_GAP * 2;
                        int32_t min_x   = w->x + (int32_t)w->w - WIN_BTN_GAP * 3;
                        int32_t btn_y   = w->y + 4;
                        bool on_title = my < w->y + (int32_t)TITLE_BAR_H;

                        if (on_title && point_in_rect(mx, my, close_x, btn_y, WIN_BTN_SIZE, WIN_BTN_SIZE)) {
                            if (w->event_mode) {
                                // Modo evento (CreateWindow de verdad) --
                                // NO destruimos nada aqui. Disparamos
                                // EVENT_WINDOWCLOSE y dejamos que el
                                // propio programa decida cuando terminar
                                // (patron tipico: "If WaitEvent()=$803
                                // Then Exit"). Si el programa nunca
                                // reacciona, la ventana se queda abierta
                                // -- es el comportamiento real de
                                // BlitzPlus tambien.
                                gadgets_fire_raw_event(win_idx, EVENT_WINDOWCLOSE, 0, 0);
                            } else {
                                // Limpiamos los gadgets EN ESTE MISMO INSTANTE, no
                                // cuando la tarea se de cuenta por su cuenta --
                                // si no, hay una ventana de tiempo en la que un
                                // programa nuevo podria reutilizar este mismo
                                // hueco de ventana y mezclar sus gadgets con los
                                // viejos, que todavia no se habrian borrado.
                                //
                                // Y TAMBIEN hay que terminar la tarea dueña,
                                // no solo la ventana -- si no, un programa como
                                // "Graphics()" (sin modo de eventos, la X cierra
                                // directo) se queda corriendo para siempre en
                                // segundo plano, ocupando su hueco del
                                // planificador sin que nadie mas lo pueda usar.
                                task_kill_by_window(win_idx);
                                gadgets_free_window(win_idx);
                                wm_destroy_window(win_idx);
                            }
                        } else if (on_title && point_in_rect(mx, my, max_x, btn_y, WIN_BTN_SIZE, WIN_BTN_SIZE)) {
                            raise_window(win_idx);
                            set_focus(win_idx);
                            toggle_maximize(win_idx);
                        } else if (on_title && point_in_rect(mx, my, min_x, btn_y, WIN_BTN_SIZE, WIN_BTN_SIZE)) {
                            w->minimized = true;
                        } else {
                            raise_window(win_idx);
                            set_focus(win_idx);
                            if (on_title) {
                                dragging_window = z_order[window_count - 1];
                                drag_offset_x = mx - w->x;
                                drag_offset_y = my - w->y;
                            }
                        }
                        break;
                    }
                }

                if (!hit_window) {
                    // Clic en el escritorio vacio -- comprobamos si cayo
                    // sobre un icono, y si es un doble clic (mismo icono,
                    // dentro de la ventana de tiempo) lo lanzamos.
                    for (int i = 0; i < icon_count; i++) {
                        if (!icons[i].used) continue;
                        if (point_in_rect(mx, my, icons[i].x, icons[i].y, ICON_W, ICON_H)) {
                            uint64_t now = timer_get_ticks();
                            if (last_icon_clicked == i && (now - last_icon_click_tick) < DOUBLE_CLICK_TICKS) {
                                request_launch(icons[i].target, 0, -1, 0xFFFFFFFF);
                                last_icon_clicked = -1;
                            } else {
                                last_icon_clicked = i;
                                last_icon_click_tick = now;
                            }
                            break;
                        }
                    }
                }
            }
        }
        needs_redraw = true;
    }

    if (left_released_edge) {
        dragging_window = -1;
    }

    if (dragging_window >= 0 && left) {
        window_t *w = &windows[dragging_window];
        w->x = mx - drag_offset_x;
        w->y = my - drag_offset_y;
        needs_redraw = true;
    }

    last_left_down = left;
}

static void blit_content(window_t *w, int32_t idx);

static void draw_window(int32_t idx) {
    window_t *w = &windows[idx];
    fb_fill_rect((uint32_t)w->x, (uint32_t)w->y, w->w, w->h + TITLE_BAR_H, COLOR_WIN_BODY);
    fb_fill_rect((uint32_t)w->x, (uint32_t)w->y, w->w, TITLE_BAR_H, COLOR_WIN_TITLE);

    if (w->owns_content) {
        blit_content(w, idx);
    }

    fb_draw_rect_border((uint32_t)w->x, (uint32_t)w->y, w->w, w->h + TITLE_BAR_H, COLOR_WIN_BORDER);
    fb_draw_string((uint32_t)w->x + 6, (uint32_t)w->y + 6, w->title, COLOR_TEXT_LIGHT, 1);

    // Botones: minimizar, maximizar, cerrar (de izquierda a derecha)
    int32_t close_x = w->x + (int32_t)w->w - WIN_BTN_GAP;
    int32_t max_x   = w->x + (int32_t)w->w - WIN_BTN_GAP * 2;
    int32_t min_x   = w->x + (int32_t)w->w - WIN_BTN_GAP * 3;
    int32_t btn_y   = w->y + 4;

    fb_fill_rect((uint32_t)min_x, (uint32_t)btn_y, WIN_BTN_SIZE, WIN_BTN_SIZE, COLOR_BTN_NORMAL);
    fb_draw_string((uint32_t)min_x + 5, (uint32_t)btn_y + 5, "-", COLOR_TEXT_LIGHT, 1);

    fb_fill_rect((uint32_t)max_x, (uint32_t)btn_y, WIN_BTN_SIZE, WIN_BTN_SIZE, COLOR_BTN_NORMAL);
    fb_draw_string((uint32_t)max_x + 3, (uint32_t)btn_y + 5, "O", COLOR_TEXT_LIGHT, 1);

    fb_fill_rect((uint32_t)close_x, (uint32_t)btn_y, WIN_BTN_SIZE, WIN_BTN_SIZE, COLOR_BTN_CLOSE);
    fb_draw_string((uint32_t)close_x + 4, (uint32_t)btn_y + 5, "X", COLOR_TEXT_LIGHT, 1);
}

static void draw_desktop_icons(void) {
    for (int i = 0; i < icon_count; i++) {
        if (!icons[i].used) continue;
        fb_blit_icon((uint32_t)icons[i].x + 4, (uint32_t)icons[i].y, ICON_SIZE, icon_get_rgba(ICON_FOLDER));
        fb_draw_string((uint32_t)icons[i].x, (uint32_t)icons[i].y + ICON_H - 14, icons[i].label, COLOR_TEXT_LIGHT, 1);
    }
}

static void draw_taskbar(void) {
    uint32_t fbw = fb_width();
    uint32_t fbh = fb_height();

    fb_fill_rect(0, fbh - TASKBAR_H, fbw, TASKBAR_H, COLOR_TASKBAR);
    fb_fill_rect(4, fbh - TASKBAR_H + 4, START_BTN_W, TASKBAR_H - 8, COLOR_START_BTN);
    fb_draw_string(12, fbh - TASKBAR_H + 12, "START", COLOR_TEXT_LIGHT, 1);

    int32_t bx = 4 + START_BTN_W + 8;
    for (int i = 0; i < window_count; i++) {
        window_t *w = &windows[z_order[i]];
        if (!w->used) continue;
        bool is_top = (i == window_count - 1);
        fb_fill_rect((uint32_t)bx, fbh - TASKBAR_H + 4, 90, TASKBAR_H - 8,
                     is_top ? COLOR_TASK_BTN_ACTIVE : COLOR_TASK_BTN);
        fb_draw_string((uint32_t)bx + 4, fbh - TASKBAR_H + 12, w->title, COLOR_TEXT_DARK, 1);
        bx += 96;
    }

    fb_draw_string(fbw - 50, fbh - TASKBAR_H + 12, "12:34", COLOR_TEXT_DARK, 1);

    if (start_menu_open) {
        int32_t menu_x = 4;
        int32_t menu_w = 140;
        int32_t row_h = 20;
        int32_t total_rows = 7;
        int32_t menu_top = (int32_t)fbh - TASKBAR_H - (row_h * total_rows);

        fb_fill_rect((uint32_t)menu_x, (uint32_t)menu_top, (uint32_t)menu_w, (uint32_t)(row_h * total_rows), COLOR_MENU_BG);
        fb_draw_rect_border((uint32_t)menu_x, (uint32_t)menu_top, (uint32_t)menu_w, (uint32_t)(row_h * total_rows), COLOR_WIN_BORDER);

        fb_draw_string((uint32_t)menu_x + 6, (uint32_t)(menu_top + 4), "PROGRAMAS", COLOR_TEXT_DARK, 1);
        fb_draw_string((uint32_t)menu_x + 14, (uint32_t)(menu_top + row_h * 1 + 4), "SHELL", COLOR_TEXT_DARK, 1);
        fb_draw_string((uint32_t)menu_x + 14, (uint32_t)(menu_top + row_h * 2 + 4), "EXPLORADOR", COLOR_TEXT_DARK, 1);
        fb_draw_string((uint32_t)menu_x + 14, (uint32_t)(menu_top + row_h * 3 + 4), "EDITOR", COLOR_TEXT_DARK, 1);
        fb_draw_string((uint32_t)menu_x + 14, (uint32_t)(menu_top + row_h * 4 + 4), "IDE", COLOR_TEXT_DARK, 1);
        fb_draw_hline((uint32_t)menu_x, (uint32_t)(menu_top + row_h * 5), (uint32_t)menu_w, COLOR_WIN_BORDER);
        fb_draw_string((uint32_t)menu_x + 6, (uint32_t)(menu_top + row_h * 5 + 4), "APAGAR", COLOR_TEXT_DARK, 1);
        fb_draw_string((uint32_t)menu_x + 6, (uint32_t)(menu_top + row_h * 6 + 4), "REINICIAR", COLOR_TEXT_DARK, 1);
    }
}

bool wm_get_window_client_rect(int32_t idx, int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return false;
    *x = windows[idx].x;
    *y = windows[idx].y + TITLE_BAR_H;
    *w = windows[idx].w;
    *h = windows[idx].h;
    return true;
}

void wm_set_owns_content(int32_t idx, bool owns) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    windows[idx].owns_content = owns;
    if (owns) {
        // Empezamos con el buffer en el mismo gris que el cuerpo de
        // una ventana normal, para que no se vea "sucio" hasta que el
        // programa dibuje algo.
        for (int y = 0; y < MAX_CONTENT_H; y++) {
            for (int x = 0; x < MAX_CONTENT_W; x++) {
                uint32_t *p = (uint32_t *)&window_content[idx][y][x * 4];
                *p = COLOR_WIN_BODY;
            }
        }
    }
}

static void content_put_pixel(int32_t idx, uint32_t x, uint32_t y, uint32_t color) {
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    if (x >= MAX_CONTENT_W || y >= MAX_CONTENT_H) return;
    uint32_t *p = (uint32_t *)&window_content[idx][y][x * 4];
    *p = color;
}

// Lee un pixel del buffer de contenido de una ventana -- para
// GetColor(). Devuelve 0 si esta fuera de rango (no hay "color
// invalido" que devolver aparte, asi que 0=negro es la opcion mas
// razonable).
uint32_t wm_content_get_pixel(int32_t idx, uint32_t x, uint32_t y) {
    if (idx < 0 || idx >= MAX_WINDOWS) return 0;
    if (x >= MAX_CONTENT_W || y >= MAX_CONTENT_H) return 0;
    uint32_t *p = (uint32_t *)&window_content[idx][y][x * 4];
    return *p;
}

// Copia un rectangulo DENTRO del mismo buffer de contenido de una
// ventana -- para CopyRect(). Si los rectangulos origen/destino se
// solapan, copiamos en el orden correcto (de atras hacia adelante
// cuando el destino esta MAS ABAJO/DERECHA que el origen) para no
// pisarnos a nosotros mismos a medio copiar, igual que memmove.
void wm_content_copy_rect(int32_t idx, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, uint32_t dx, uint32_t dy) {
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    bool reverse_y = dy > sy;
    bool reverse_x = dx > sx;
    if (reverse_y) {
        for (uint32_t j = h; j-- > 0;) {
            if (reverse_x) {
                for (uint32_t i = w; i-- > 0;) {
                    content_put_pixel(idx, dx + i, dy + j, wm_content_get_pixel(idx, sx + i, sy + j));
                }
            } else {
                for (uint32_t i = 0; i < w; i++) {
                    content_put_pixel(idx, dx + i, dy + j, wm_content_get_pixel(idx, sx + i, sy + j));
                }
            }
        }
    } else {
        for (uint32_t j = 0; j < h; j++) {
            if (reverse_x) {
                for (uint32_t i = w; i-- > 0;) {
                    content_put_pixel(idx, dx + i, dy + j, wm_content_get_pixel(idx, sx + i, sy + j));
                }
            } else {
                for (uint32_t i = 0; i < w; i++) {
                    content_put_pixel(idx, dx + i, dy + j, wm_content_get_pixel(idx, sx + i, sy + j));
                }
            }
        }
    }
}

void wm_content_fill_rect(int32_t idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t j = 0; j < h; j++) {
        for (uint32_t i = 0; i < w; i++) {
            content_put_pixel(idx, x + i, y + j, color);
        }
    }
}

void wm_content_draw_string(int32_t idx, uint32_t x, uint32_t y, const char *str, uint32_t color, uint32_t scale) {
    uint32_t cursor_x = x;
    while (*str) {
        char c = *str;
        char uc = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        if (uc >= 0 && uc < 128) {
            const uint8_t *rows = font5x7[(int)uc];
            for (int ry = 0; ry < FONT_HEIGHT; ry++) {
                uint8_t bits = rows[ry];
                for (int rx = 0; rx < FONT_WIDTH; rx++) {
                    if (bits & (1 << (FONT_WIDTH - 1 - rx))) {
                        for (uint32_t sy = 0; sy < scale; sy++) {
                            for (uint32_t sx = 0; sx < scale; sx++) {
                                content_put_pixel(idx, cursor_x + rx * scale + sx, y + ry * scale + sy, color);
                            }
                        }
                    }
                }
            }
        }
        cursor_x += (FONT_WIDTH + 1) * scale;
        str++;
    }
}

// "Pega" un icono RGBA (con transparencia) en el buffer de contenido
// de una ventana, mezclando segun el canal alfa -- igual que
// fb_blit_icon pero para el buffer propio de un programa en vez del
// framebuffer general.
void wm_content_blit_icon(int32_t idx, uint32_t x, uint32_t y, uint32_t size, const uint8_t *rgba) {
    for (uint32_t iy = 0; iy < size; iy++) {
        for (uint32_t ix = 0; ix < size; ix++) {
            const uint8_t *px = &rgba[(iy * size + ix) * 4];
            uint8_t a = px[3];
            if (a == 0) continue;

            if (idx < 0 || idx >= MAX_WINDOWS) return;
            if (x + ix >= MAX_CONTENT_W || y + iy >= MAX_CONTENT_H) continue;
            uint32_t *dst = (uint32_t *)&window_content[idx][y + iy][(x + ix) * 4];
            uint32_t dst_color = *dst;
            uint8_t dr = (uint8_t)(dst_color >> 16);
            uint8_t dg = (uint8_t)(dst_color >> 8);
            uint8_t db = (uint8_t)(dst_color);

            uint8_t r = (uint8_t)((px[0] * a + dr * (255 - a)) / 255);
            uint8_t g = (uint8_t)((px[1] * a + dg * (255 - a)) / 255);
            uint8_t b = (uint8_t)((px[2] * a + db * (255 - a)) / 255);

            *dst = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

// Igual que wm_content_blit_icon, pero con ancho y alto
// independientes -- los iconos embebidos son siempre cuadrados, pero
// una imagen cargada con LoadImage puede ser cualquier tamaño.
// 'solid' distingue DrawImage (false, mezcla segun el canal alfa) de
// DrawBlock (true, opaco -- copia el color tal cual, ignorando la
// transparencia por completo) -- la misma distincion que hace
// BlitzPlus real entre estos dos comandos. 'has_mask'/'mask_color'
// vienen de MaskImage: cualquier pixel cuyo color (sin contar alfa)
// coincida se trata como transparente EN ESTE dibujado -- sin tocar
// la imagen original, se comprueba cada vez (igual que BlitzPlus real).
void wm_content_blit_image(int32_t idx, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t *rgba, bool solid, bool has_mask, uint32_t mask_color) {
    for (uint32_t iy = 0; iy < height; iy++) {
        for (uint32_t ix = 0; ix < width; ix++) {
            const uint8_t *px = &rgba[(iy * width + ix) * 4];
            uint32_t px_rgb = ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
            if (has_mask && px_rgb == mask_color) continue;
            uint8_t a = solid ? 255 : px[3];
            if (a == 0) continue;

            if (idx < 0 || idx >= MAX_WINDOWS) return;
            if (x + ix >= MAX_CONTENT_W || y + iy >= MAX_CONTENT_H) continue;
            uint32_t *dst = (uint32_t *)&window_content[idx][y + iy][(x + ix) * 4];

            if (solid) {
                *dst = px_rgb;
                continue;
            }

            uint32_t dst_color = *dst;
            uint8_t dr = (uint8_t)(dst_color >> 16);
            uint8_t dg = (uint8_t)(dst_color >> 8);
            uint8_t db = (uint8_t)(dst_color);

            uint8_t r = (uint8_t)((px[0] * a + dr * (255 - a)) / 255);
            uint8_t g = (uint8_t)((px[1] * a + dg * (255 - a)) / 255);
            uint8_t b = (uint8_t)((px[2] * a + db * (255 - a)) / 255);

            *dst = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

// Igual que wm_content_blit_image, pero SOLO pega un sub-rectangulo
// (blit_w x blit_h) de un origen mas ancho -- 'src_stride' es el
// ancho REAL de la imagen entera de la que 'rgba' es un puntero a
// mitad de camino, para poder saltar de una fila a la siguiente
// correctamente. La usan DrawImageRect/DrawBlockRect y los fotogramas
// de LoadAnimImage.
void wm_content_blit_image_rect(int32_t idx, uint32_t x, uint32_t y, uint32_t blit_w, uint32_t blit_h, uint32_t src_stride, const uint8_t *rgba, bool solid, bool has_mask, uint32_t mask_color) {
    for (uint32_t iy = 0; iy < blit_h; iy++) {
        for (uint32_t ix = 0; ix < blit_w; ix++) {
            const uint8_t *px = &rgba[(iy * src_stride + ix) * 4];
            uint32_t px_rgb = ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
            if (has_mask && px_rgb == mask_color) continue;
            uint8_t a = solid ? 255 : px[3];
            if (a == 0) continue;

            if (idx < 0 || idx >= MAX_WINDOWS) return;
            if (x + ix >= MAX_CONTENT_W || y + iy >= MAX_CONTENT_H) continue;
            uint32_t *dst = (uint32_t *)&window_content[idx][y + iy][(x + ix) * 4];

            if (solid) {
                *dst = px_rgb;
                continue;
            }

            uint32_t dst_color = *dst;
            uint8_t dr = (uint8_t)(dst_color >> 16);
            uint8_t dg = (uint8_t)(dst_color >> 8);
            uint8_t db = (uint8_t)(dst_color);

            uint8_t r = (uint8_t)((px[0] * a + dr * (255 - a)) / 255);
            uint8_t g = (uint8_t)((px[1] * a + dg * (255 - a)) / 255);
            uint8_t b = (uint8_t)((px[2] * a + db * (255 - a)) / 255);

            *dst = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

// "Pega" el buffer de contenido de una ventana en su sitio dentro del
// back buffer general -- se llama tras dibujar el cuerpo/marco normal
// de la ventana, asi que el contenido del programa queda encima.
static void blit_content(window_t *w, int32_t idx) {
    uint32_t cw = w->w < MAX_CONTENT_W ? w->w : MAX_CONTENT_W;
    uint32_t ch = w->h < MAX_CONTENT_H ? w->h : MAX_CONTENT_H;
    for (uint32_t y = 0; y < ch; y++) {
        for (uint32_t x = 0; x < cw; x++) {
            uint32_t *src = (uint32_t *)&window_content[idx][y][x * 4];
            fb_put_pixel((uint32_t)(w->x + (int32_t)x), (uint32_t)(w->y + TITLE_BAR_H + (int32_t)y), *src);
        }
    }
}

void wm_request_redraw(void) {
    needs_redraw = true;
}

void wm_define_button(int32_t win_idx, uint32_t id, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (win_idx < 0 || win_idx >= MAX_WINDOWS) return;

    int slot = -1;
    for (int i = 0; i < MAX_BUTTONS_PER_WINDOW; i++) {
        if (win_buttons[win_idx][i].used && win_buttons[win_idx][i].id == id) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_BUTTONS_PER_WINDOW; i++) {
            if (!win_buttons[win_idx][i].used) { slot = i; break; }
        }
    }
    if (slot < 0) return; // sin hueco -- se ignora silenciosamente

    win_buttons[win_idx][slot].used = true;
    win_buttons[win_idx][slot].id = id;
    win_buttons[win_idx][slot].x = x;
    win_buttons[win_idx][slot].y = y;
    win_buttons[win_idx][slot].w = w;
    win_buttons[win_idx][slot].h = h;

    // Aspecto por defecto -- el programa dibuja su etiqueta encima
    // con SYS_DRAW_TEXT si quiere.
    wm_content_fill_rect(win_idx, (uint32_t)x, (uint32_t)y, w, h, color);
}

uint32_t wm_get_clicked_button(int32_t win_idx) {
    if (win_idx < 0 || win_idx >= MAX_WINDOWS) return 0;

    if (win_idx != focused_window) {
        btn_last_left[win_idx] = false;
        return 0;
    }

    bool left = mouse_left_down();
    bool edge = left && !btn_last_left[win_idx];
    btn_last_left[win_idx] = left;
    if (!edge) return 0;

    int32_t wx, wy;
    uint32_t ww, wh;
    if (!wm_get_window_client_rect(win_idx, &wx, &wy, &ww, &wh)) return 0;
    (void)ww; (void)wh;

    int32_t lx = mouse_x() - wx;
    int32_t ly = mouse_y() - wy;

    for (int i = 0; i < MAX_BUTTONS_PER_WINDOW; i++) {
        ui_button_t *b = &win_buttons[win_idx][i];
        if (!b->used) continue;
        if (lx >= b->x && lx < b->x + (int32_t)b->w && ly >= b->y && ly < b->y + (int32_t)b->h) {
            return b->id;
        }
    }
    return 0;
}

int32_t wm_get_focused_window(void) {
    return focused_window;
}

// ActivateWindow -- trae la ventana al frente y le da el foco de
// teclado, igual que hace un clic del usuario sobre ella (mismo
// patron ya usado en el manejo de clics de la barra de tareas).
void wm_activate_window(int32_t idx) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    windows[idx].minimized = false;
    raise_window(idx);
    set_focus(idx);
}

// MaximizeWindow/MinimizeWindow -- version EXPLICITA (fija el
// estado, no lo alterna) de la misma logica que ya usa
// toggle_maximize para el doble-clic en la barra de titulo.
void wm_maximize_window(int32_t idx) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    window_t *w = &windows[idx];
    if (w->maximized) return; // ya lo esta -- idempotente
    w->restore_x = w->x;
    w->restore_y = w->y;
    w->restore_w = w->w;
    w->restore_h = w->h;
    w->x = 0;
    w->y = 0;
    w->w = fb_width();
    w->h = fb_height() - TASKBAR_H - TITLE_BAR_H;
    w->maximized = true;
    w->minimized = false;
    if (w->event_mode) gadgets_fire_raw_event(idx, EVENT_WINDOWSIZE, 0, 0);
    needs_redraw = true;
}

void wm_minimize_window(int32_t idx) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    windows[idx].minimized = true;
    needs_redraw = true;
}

bool wm_window_maximized(int32_t idx) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return false;
    return windows[idx].maximized;
}

bool wm_window_minimized(int32_t idx) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return false;
    return windows[idx].minimized;
}

// SetMinWindowSize(window[,width,height]) -- w=0,h=0 (omitidos)
// significa "el tamaño actual". Ver la nota junto al campo
// min_w/min_h en window_t: se guarda pero no se aplica todavia.
void wm_set_min_window_size(int32_t idx, uint32_t w, uint32_t h) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    window_t *win = &windows[idx];
    win->min_w = (w == 0) ? win->w : w;
    win->min_h = (h == 0) ? win->h : h;
}

// Version publica de request_launch -- la usa la syscall
// SYS_LAUNCH_PROGRAM para que cualquier programa pueda pedir que se
// lance otro (por ejemplo, el explorador pidiendo abrir un .txt con
// el editor).
void wm_request_launch(const char *target_pro, const char *arg, int32_t requesting_window, uint32_t search_dir) {
    request_launch(target_pro, arg, requesting_window, search_dir);
}

bool wm_consume_launch_request(char *out_name, uint32_t max_len, char *out_arg, uint32_t max_arg_len,
                                int32_t *out_requesting_window, uint32_t *out_search_dir) {
    if (!launch_pending) return false;
    launch_pending = false;
    uint32_t i = 0;
    while (launch_target[i] != '\0' && i < max_len - 1) {
        out_name[i] = launch_target[i];
        i++;
    }
    out_name[i] = '\0';

    if (out_arg && max_arg_len > 0) {
        uint32_t j = 0;
        while (launch_arg[j] != '\0' && j < max_arg_len - 1) {
            out_arg[j] = launch_arg[j];
            j++;
        }
        out_arg[j] = '\0';
    }

    if (out_requesting_window) *out_requesting_window = launch_requesting_window;
    if (out_search_dir) *out_search_dir = launch_search_dir;
    return true;
}

void wm_destroy_window(int32_t idx) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;

    // La quitamos de z_order (desplazamos lo que hay detras un hueco)
    int pos = -1;
    for (int i = 0; i < window_count; i++) {
        if (z_order[i] == idx) { pos = i; break; }
    }
    if (pos >= 0) {
        for (int i = pos; i < window_count - 1; i++) {
            z_order[i] = z_order[i + 1];
        }
        window_count--;
    }

    windows[idx].used = false;
    windows[idx].owns_content = false;
    for (int b = 0; b < MAX_BUTTONS_PER_WINDOW; b++) win_buttons[idx][b].used = false;

    if (dragging_window == idx) dragging_window = -1;
    if (focused_window == idx) {
        // Le pasamos el foco a la ventana que haya quedado mas al
        // frente, si hay alguna
        focused_window = (window_count > 0) ? z_order[window_count - 1] : -1;
    }

    needs_redraw = true;
}

void wm_draw_if_needed(void) {
    if (!needs_redraw) return;

    fb_fill_rect(0, 0, fb_width(), fb_height(), COLOR_DESKTOP);

    draw_desktop_icons();

    for (int i = 0; i < window_count; i++) {
        if (windows[z_order[i]].used && !windows[z_order[i]].minimized) {
            draw_window(z_order[i]);
        }
    }

    draw_taskbar();

    fb_fill_rect((uint32_t)mouse_x(), (uint32_t)mouse_y(), 8, 8, COLOR_CURSOR);

    fb_present();

    needs_redraw = false;
}
