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
#include "medir.h"
#include "bkl.h"   // componer soltando el candado a trozos
#include "fonts.h"
#include "rtc.h"
#include "ramfb.h"
#include "text.h"
#include "input.h"
#include "tasks.h"
#include "uart.h"
#include "font5x7.h"
#include "power.h"
#include "sound.h"
#include "timer.h"
#include "icons_data.h"
#include "heap.h"        // kmalloc/kfree: el fondo de pantalla vive en el monton
#include "gadgets.h"
#include "nemofs.h"
#include "syscall.h"  // console_queue_reset(): ver el bug de la cola reciclada

// MAX_WINDOWS vive en wm.h (una sola definicion para todo el kernel)
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
    // Botones de la barra de titulo que el programa no quiere:
    // un reloj o una calculadora no tienen nada que hacer maximizados. Ver
    // wm_set_window_buttons y botones_titulo.
    bool sin_maximizar, sin_minimizar;
    // Y el de cerrar: lo quitan juegos a pantalla completa, que
    // ofrecen su propia salida (Esc). Si un programa asi se cuelga, se
    // cierra desde el gestor de tareas.
    bool sin_cerrar;
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
__attribute__((aligned(16)))
static uint8_t window_content[MAX_WINDOWS][MAX_CONTENT_H][MAX_CONTENT_W * 4];

// ---- Doble bufer de las ventanas (SMP, fase 5b) ----
//
// window_content es donde DIBUJA la tarea. window_front es lo que se
// MUESTRA: el compositor (blit_content) solo lee de aqui.
//
// Por que: con un solo nucleo, la pantalla se componia en el turno del
// kernel, es decir, siempre despues de que la tarea hubiera cedido el
// turno -- entre fotogramas. Con varios nucleos, la tarea dibuja en otro
// nucleo llamada a llamada, y el nucleo 0 componia cuando le tocaba:
// podia pillar la ventana recien borrada y sin el texto todavia. La
// shell parpadeaba entera y el explorador se veia a medio redibujar.
//
// Ahora el dibujo solo pasa a window_front al PUBLICARSE, en un punto
// entre fotogramas (wm_publicar_contenido): cuando la tarea cede el
// turno, o cuando el compositor ve que la tarea no esta corriendo en
// otro nucleo. La pantalla vuelve a mostrar siempre fotogramas enteros.
//
// Cuesta un segundo lienzo por ventana (~4.8 MB, 77 MB en total para 16
// ventanas); el kernel queda en unos 405 MB de los 512 de QEMU.
__attribute__((aligned(16)))
static uint8_t window_front[MAX_WINDOWS][MAX_CONTENT_H][MAX_CONTENT_W * 4];

// Cambiado desde la ultima publicacion. Sin esta marca se copiaria el
// lienzo en cada cesion de turno, y las tareas ceden muchisimas veces sin
// dibujar nada (un Delay cede en bucle).
static bool contenido_cambiado[MAX_WINDOWS];

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

// ---------------------------------------------------------------
// MENU DE INICIO POR SECCIONES
//
// Antes eran cuatro programas escritos a mano en el codigo: añadir uno
// obligaba a recompilar el kernel. Ahora sale de MENU.CFG, un archivo de
// TEXTO en la raiz, con secciones y submenus. Se eligio texto y no un
// formato binario a proposito: asi se puede arreglar con el editor del
// propio sistema si algo va mal, y se entiende sin herramientas.
//
//   ; comentario
//   seccion Programacion 14
//     item IDE ide.pro 5
//     item Aronnax aronnax.lua 14
//
// El ultimo numero es el icono del catalogo (ver icons_data.h); si falta,
// se pone uno generico. Apagar y Reiniciar NO se configuran: van siempre
// al final, porque un menu sin forma de apagar deja el sistema sin salida.
#define MENU_CFG "MENU.CFG"
#define MENU_MAX_SEC 10
#define MENU_MAX_ITEM 14
#define MENU_LARGO 28

typedef struct {
    char etiqueta[MENU_LARGO];
    char destino[MENU_LARGO];
    int32_t icono;
} menu_item_t;

typedef struct {
    char etiqueta[MENU_LARGO];
    int32_t icono;
    menu_item_t items[MENU_MAX_ITEM];
    int32_t n_items;
} menu_sec_t;

static menu_sec_t menu_secs[MENU_MAX_SEC];
static int32_t menu_n_secs = 0;
static int32_t menu_sec_abierta = -1;   // submenu desplegado, -1 ninguno

// La geometria del menu, en UN solo sitio: la usan el dibujado y los
// clics. Tenerla escrita dos veces es como se consigue que un menu
// responda un pixel mas abajo de donde se ve.
#define MENU_FILA_H 28
#define MENU_ANCHO 172
#define MENU_SUB_ANCHO 186

static int32_t menu_filas_total(void) { return menu_n_secs + 2; }  // + Apagar + Reiniciar
static int32_t menu_alto(void) { return menu_filas_total() * MENU_FILA_H + 6; }
static int32_t menu_top(void) { return (int32_t)fb_height() - TASKBAR_H - menu_alto(); }

static void menu_icono(int32_t x, int32_t y, int32_t id) {
    const uint8_t *rgba = icon_get_rgba(id);
    if (!rgba) rgba = icon_get_rgba(ICON_FOLDER);
    // OJO: 'size' es a la vez el lado del icono Y EL PASO DE FILA del
    // bufer. Los del catalogo son de 24x24: pedir 18 para que "quepa
    // mejor" hace que cada fila se lea desplazada seis pixeles, y el
    // icono sale convertido en rayas diagonales. Si hay que encajarlo en
    // otro tamaño, se cambia la FILA, no esto.
    fb_blit_icon_scaled((uint32_t)x, (uint32_t)y, ICON_SIZE, 1, rgba);
}
static bool launch_pending = false;
static char launch_target[32] = "";
static char launch_arg[TASK_LAUNCH_ARG_MAX] = "";
static int32_t launch_requesting_window = -1; // -1 = lanzado sin "padre" (icono, menu Start...)
static uint32_t launch_search_dir = 0xFFFFFFFF; // 0xFFFFFFFF = sin preferencia (raiz, luego PROGRAMAS)

// -- Iconos de escritorio: extensibles, se añaden con wm_add_desktop_icon --
#define MAX_ICONS 40   // sitio de sobra para que el usuario añada los suyos, no solo los 4 de fabrica
// Tamaño de los iconos de escritorio: 1 (24x24, el de siempre) o 2
// (48x48). Ajustable en caliente desde el editor de escritorio
// (AJUSTES) y persistido en disco -- por defecto, tamaño normal.
static bool needs_redraw = true;

// ---- EL RECTANGULO SUCIO ----
//
// La composicion volvia a pintarlo TODO aunque hubiera cambiado un trozo de
// una ventana: el fondo de pantalla entero, cada ventana abierta entera, y la
// copia de los cinco megas de pantalla. Con el monitor del sistema (560x620)
// eso son 1,4 MB de esa ventana en cada composicion, y cada programa abierto
// suma el suyo -- de ahi que el escritorio fuera a peor cuantas mas cosas
// hubiera abiertas.
//
// Ahora se lleva la cuenta de QUE trozo ha cambiado, como un solo rectangulo
// que engloba todo lo sucio. Un rectangulo y no una lista a proposito: la
// lista solo gana cuando hay cambios lejos unos de otros, y cuesta bastante
// mas codigo en la parte del sistema donde un fallo se ve raro y se
// diagnostica mal.
//
// Lo que se recorta al rectangulo son las copias GRANDES -- el fondo y el
// contenido de cada ventana --, que es donde estan los bytes. El resto (los
// marcos, los iconos, la barra de tareas) se sigue pintando entero: son
// rellenos pequeños, y pintarlos fuera del rectangulo no rompe nada porque
// esos pixeles se quedan como estaban y no se publican.
//
// Por que eso es correcto: fuera del rectangulo sucio el bufer de pantalla
// conserva el fotograma anterior, que sigue siendo bueno porque ahi no ha
// cambiado nada. Mover o cerrar una ventana ensucia SU SITIO VIEJO tambien
// (wm_request_redraw lo ensucia todo), asi que el hueco que deja se repinta.
static int32_t sucio_x0, sucio_y0, sucio_x1, sucio_y1;   // x1/y1 exclusivos
static bool hay_sucio = false;

static void ensuciar(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return;
    if (!hay_sucio) {
        sucio_x0 = x; sucio_y0 = y; sucio_x1 = x + w; sucio_y1 = y + h;
        hay_sucio = true;
        return;
    }
    if (x < sucio_x0) sucio_x0 = x;
    if (y < sucio_y0) sucio_y0 = y;
    if (x + w > sucio_x1) sucio_x1 = x + w;
    if (y + h > sucio_y1) sucio_y1 = y + h;
}

static void ensuciar_todo(void) {
    ensuciar(0, 0, (int32_t)fb_width(), (int32_t)fb_height());
}

// Recorta el rectangulo sucio a la pantalla y lo deja en x/y/w/h.
static bool sucio_recortado(int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
    if (!hay_sucio) return false;
    int32_t x0 = sucio_x0 < 0 ? 0 : sucio_x0;
    int32_t y0 = sucio_y0 < 0 ? 0 : sucio_y0;
    int32_t x1 = sucio_x1 > (int32_t)fb_width() ? (int32_t)fb_width() : sucio_x1;
    int32_t y1 = sucio_y1 > (int32_t)fb_height() ? (int32_t)fb_height() : sucio_y1;
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}

// Relleno de color recortado al rectangulo sucio. Para los rellenos GRANDES
// -- el escritorio sin fondo, el cuerpo de cada ventana --, que son los que
// crecian con el tamaño de la ventana y con cuantas hubiera abiertas. Los
// pequeños (marcos, botones, barra de tareas) siguen sin recortar: comprobar
// costaria mas que pintarlos.
static void rellenar_recortado(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    int32_t rx, ry, rw, rh;
    if (!sucio_recortado(&rx, &ry, &rw, &rh)) return;
    int32_t x0 = x > rx ? x : rx;
    int32_t y0 = y > ry ? y : ry;
    int32_t x1 = (x + w) < (rx + rw) ? (x + w) : (rx + rw);
    int32_t y1 = (y + h) < (ry + rh) ? (y + h) : (ry + rh);
    if (x1 <= x0 || y1 <= y0) return;
    fb_fill_rect((uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), color);
}

static int32_t g_icon_scale = 1;

static int32_t icon_cell_w(void) { return g_icon_scale >= 2 ? 76 : 56; }
static int32_t icon_cell_h(void) { return g_icon_scale >= 2 ? 80 : 44; }

int32_t wm_get_icon_scale(void) { return g_icon_scale; }
void wm_set_icon_scale(int32_t scale) {
    g_icon_scale = (scale >= 2) ? 2 : 1;
    wm_request_redraw();
}

typedef struct {
    bool used;
    int32_t x, y;
    int32_t icon_id;  // que dibujo usar (ICON_FOLDER, ICON_TERMINAL...)
    char label[16];
    char target[32]; // nombre del archivo .pro a lanzar con doble clic
} desktop_icon_t;

static desktop_icon_t icons[MAX_ICONS];
static int32_t icon_count = 0;
static int32_t last_icon_clicked = -1;
static uint64_t last_icon_click_tick = 0;
#define DOUBLE_CLICK_TICKS 50 // medio segundo a 100Hz

static bool last_left_down = false;

static void set_focus(int32_t idx);

void wm_init(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) windows[i].used = false;
    window_count = 0;
    wm_request_redraw();
}

void wm_add_desktop_icon_ex(const char *label, const char *target_pro, int32_t icon_id, int32_t x, int32_t y) {
    if (icon_count >= MAX_ICONS) return;
    desktop_icon_t *ic = &icons[icon_count];
    ic->used = true;
    ic->x = x;
    ic->y = y;
    ic->icon_id = icon_id;
    int i = 0;
    while (label[i] != '\0' && i < 15) { ic->label[i] = label[i]; i++; }
    ic->label[i] = '\0';
    i = 0;
    while (target_pro[i] != '\0' && i < 31) { ic->target[i] = target_pro[i]; i++; }
    ic->target[i] = '\0';
    icon_count++;
    wm_request_redraw();
}

void wm_add_desktop_icon(const char *label, const char *target_pro, int32_t x, int32_t y) {
    wm_add_desktop_icon_ex(label, target_pro, ICON_FOLDER, x, y);
}

// Quita el icono 'index' y compacta el hueco (desplaza los siguientes
// una posicion hacia atras) -- asi icon_count siempre refleja cuantos
// hay de verdad, sin agujeros que el resto del codigo tendria que
// saber saltarse.
bool wm_remove_desktop_icon(int32_t index) {
    if (index < 0 || index >= icon_count) return false;
    for (int32_t i = index; i < icon_count - 1; i++) icons[i] = icons[i + 1];
    icon_count--;
    icons[icon_count].used = false;
    wm_request_redraw();
    return true;
}

bool wm_move_desktop_icon(int32_t index, int32_t x, int32_t y) {
    if (index < 0 || index >= icon_count || !icons[index].used) return false;
    icons[index].x = x;
    icons[index].y = y;
    wm_request_redraw();
    return true;
}

bool wm_set_desktop_icon_graphic(int32_t index, int32_t icon_id) {
    if (index < 0 || index >= icon_count || !icons[index].used) return false;
    icons[index].icon_id = icon_id;
    wm_request_redraw();
    return true;
}

// Cambiar el NOMBRE de un icono ya colocado. Antes solo se
// podia poner al crearlo: para corregir una errata habia que borrarlo y
// volver a colocarlo en el mismo sitio.
bool wm_set_desktop_icon_label(int32_t index, const char *label) {
    if (index < 0 || index >= icon_count || !icons[index].used) return false;
    if (!label || !label[0]) return false;      // una etiqueta vacia deja el icono sin nada escrito
    int j = 0;
    while (j < 15 && label[j]) { icons[index].label[j] = label[j]; j++; }
    icons[index].label[j] = '\0';
    wm_request_redraw();
    return true;
}

int32_t wm_desktop_icon_count(void) { return icon_count; }

bool wm_get_desktop_icon(int32_t index, char *out_label, uint32_t label_max,
                         char *out_target, uint32_t target_max,
                         int32_t *out_icon_id, int32_t *out_x, int32_t *out_y) {
    if (index < 0 || index >= icon_count || !icons[index].used) return false;
    if (out_label) {
        uint32_t i = 0;
        while (icons[index].label[i] != '\0' && i < label_max - 1) { out_label[i] = icons[index].label[i]; i++; }
        out_label[i] = '\0';
    }
    if (out_target) {
        uint32_t i = 0;
        while (icons[index].target[i] != '\0' && i < target_max - 1) { out_target[i] = icons[index].target[i]; i++; }
        out_target[i] = '\0';
    }
    if (out_icon_id) *out_icon_id = icons[index].icon_id;
    if (out_x) *out_x = icons[index].x;
    if (out_y) *out_y = icons[index].y;
    return true;
}

// -- persistencia en disco: sin esto, cualquier icono que el usuario
// coloque desapareceria en el siguiente arranque. Formato sencillo:
// contador (4 bytes) + esa cantidad de registros de tamaño fijo
// (label[16] + target[32] + icon_id(4) + x(4) + y(4) = 60 bytes).
#define DESKTOP_CFG_NAME "DESKTOP.CFG"
// Version del contenido, no del formato: sube cuando el sistema trae
// iconos nuevos que hay que añadir a un escritorio ya existente.
// 2: añade el icono de AYUDA a los escritorios ya
// guardados. Subir este numero es lo que dispara wm_migrar_escritorio.
#define DESKTOP_CFG_VERSION 2
static int32_t g_cfg_version = 0;
#define DESKTOP_RECORD_SIZE (16 + 32 + 4 + 4 + 4)

void wm_save_desktop_icons(void) {
    // Formato: escala (4) + contador (4) + registros. La escala va
    // PRIMERO y se valida al cargar (solo 1 o 2 son validos) -- asi,
    // si algun dia hay que cambiar el formato otra vez, un archivo
    // viejo se detecta como invalido en vez de leerse mal en silencio,
    // y el arranque cae de vuelta a crear-y-guardar los valores por
    // defecto en el formato nuevo.
    static uint8_t buf[8 + MAX_ICONS * DESKTOP_RECORD_SIZE];
    uint32_t pos = 0;
    // El campo de la escala lleva ademas la VERSION en su parte alta.
    // Hace falta para poder añadir iconos nuevos del sistema a un
    // escritorio que YA existe: sin una marca, o no se añaden nunca, o
    // se vuelven a añadir en cada arranque aunque el usuario los haya
    // quitado. Los archivos viejos traen version 0 y se migran una vez.
    uint32_t scale_u = ((uint32_t)g_icon_scale & 0xFFFF) | ((uint32_t)DESKTOP_CFG_VERSION << 16);
    for (int b = 0; b < 4; b++) buf[pos++] = (uint8_t)(scale_u >> (b * 8));
    uint32_t n = (uint32_t)icon_count;
    for (int b = 0; b < 4; b++) buf[pos++] = (uint8_t)(n >> (b * 8));
    for (int32_t i = 0; i < icon_count; i++) {
        int j = 0;
        while (j < 16) { buf[pos + j] = (uint8_t)icons[i].label[j]; j++; }
        pos += 16;
        j = 0;
        while (j < 32) { buf[pos + j] = (uint8_t)icons[i].target[j]; j++; }
        pos += 32;
        uint32_t icon_id_u = (uint32_t)icons[i].icon_id;
        for (int b = 0; b < 4; b++) buf[pos++] = (uint8_t)(icon_id_u >> (b * 8));
        uint32_t x_u = (uint32_t)icons[i].x;
        for (int b = 0; b < 4; b++) buf[pos++] = (uint8_t)(x_u >> (b * 8));
        uint32_t y_u = (uint32_t)icons[i].y;
        for (int b = 0; b < 4; b++) buf[pos++] = (uint8_t)(y_u >> (b * 8));
    }
    int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, DESKTOP_CFG_NAME);
    if (idx < 0) idx = nemofs_create(NEMOFS_ROOT_INODE, DESKTOP_CFG_NAME, NEMOFS_TYPE_FILE);
    if (idx >= 0) nemofs_write_file((uint32_t)idx, buf, pos);
}

// ¿Hay ya un icono que apunte a esto? Se compara el DESTINO, no la
// etiqueta: el usuario puede haberlo renombrado.
static bool hay_icono_para(const char *target) {
    for (int32_t i = 0; i < icon_count; i++) {
        const char *t = icons[i].target;
        int j = 0;
        while (t[j] && target[j] && t[j] == target[j]) j++;
        if (t[j] == '\0' && target[j] == '\0') return true;
    }
    return false;
}

// Añade los iconos que el sistema ha empezado a traer despues de que
// este escritorio se guardara. Se ejecuta UNA vez, por version: si
// luego se quita uno a mano, no vuelve a aparecer.
void wm_migrar_escritorio(void) {
    if (g_cfg_version >= DESKTOP_CFG_VERSION) return;
    struct { const char *label; const char *target; int32_t icono; } nuevos[] = {
        { "ARONNAX",   "aronnax.lua",   ICON_DESIGN },
        { "PINTOR",    "pintor.lua",    ICON_PAINT },
        { "LUA",       "shell.lua",     ICON_LUA },
        { "NAVEGANTE", "navegante.lua", ICON_COMPASS },
        { "AYUDA",     "ayuda.lua",     ICON_HELP },     // ayuda de usuario
    };
    // BUG REAL CORREGIDO: la 'y' solo avanzaba cuando se
    // colocaba un icono, asi que en una migracion posterior -- un
    // escritorio que YA tenia los cuatro de la version 1 y al que solo
    // le falta el nuevo -- los cuatro primeros se saltaban sin mover
    // la 'y' y el nuevo aterrizaba en 150,30: justo encima de ARONNAX.
    // Ahora se arranca por debajo de lo que ya haya en esa columna.
    int32_t y = 30;
    for (int32_t i = 0; i < icon_count; i++) {
        if (icons[i].x != 150) continue;
        if (icons[i].y + 100 > y) y = icons[i].y + 100;
    }
    for (uint32_t i = 0; i < sizeof(nuevos) / sizeof(nuevos[0]); i++) {
        if (hay_icono_para(nuevos[i].target)) continue;
        // segunda columna, para no pisar los que ya estan
        wm_add_desktop_icon_ex(nuevos[i].label, nuevos[i].target, nuevos[i].icono, 150, y);
        y += 100;
    }
    g_cfg_version = DESKTOP_CFG_VERSION;
    wm_save_desktop_icons();
}

// ---------------------------------------------------------------
// FONDO DE PANTALLA
//
// NO es una "imagen" del sistema: esas estan limitadas a 1024x1024 y la
// pantalla puede ser mayor. El fondo es un bufer propio, ya del tamaño
// EXACTO de la pantalla, escalado UNA VEZ al cargarlo. Asi componer no
// tiene que escalar nada: es copiar filas, que es lo mas barato que hay.
//
// Cuesta ancho*alto*4 bytes del monton del kernel -- 8,3 MB a 1920x1080.
// Si no hay sitio, se queda el color liso de siempre: un fondo bonito no
// puede impedir que el escritorio funcione.
static uint32_t *g_fondo = 0;
static uint32_t g_fondo_w = 0, g_fondo_h = 0;

// Se queda con el bufer (ya del tamaño de la pantalla) y suelta el
// anterior. Pasar 0 vuelve al color liso.
void wm_fondo_set(uint32_t *pixeles, uint32_t ancho, uint32_t alto) {
    if (g_fondo && g_fondo != pixeles) kfree(g_fondo);
    g_fondo = pixeles;
    g_fondo_w = ancho;
    g_fondo_h = alto;
    wm_request_redraw();
}

bool wm_fondo_hay(void) { return g_fondo != 0; }

static void dibujar_fondo(void) {
    // Si la pantalla ha cambiado de tamaño desde que se cargo el fondo,
    // no cuadra: mejor el color liso que una imagen descolocada.
    if (!g_fondo || g_fondo_w != fb_width() || g_fondo_h != fb_height()) {
        rellenar_recortado(0, 0, (int32_t)fb_width(), (int32_t)fb_height(), COLOR_DESKTOP);
        return;
    }
    // Solo las filas y las columnas que estan sucias. El fondo de una pantalla
    // de 1024x768 son 3 MB; repintar entero para que cambie una ventana de
    // 560x620 era casi todo trabajo tirado.
    int32_t rx, ry, rw, rh;
    if (!sucio_recortado(&rx, &ry, &rw, &rh)) return;
    if ((uint32_t)rx >= g_fondo_w || (uint32_t)ry >= g_fondo_h) return;
    if ((uint32_t)(rx + rw) > g_fondo_w) rw = (int32_t)g_fondo_w - rx;
    if ((uint32_t)(ry + rh) > g_fondo_h) rh = (int32_t)g_fondo_h - ry;
    for (int32_t f = 0; f < rh; f++) {
        fb_blit_row(rx, ry + f,
                    &g_fondo[(uint64_t)(ry + f) * g_fondo_w + (uint32_t)rx], (uint32_t)rw);
    }
}

// ---------------------------------------------------------------
// EL PUNTERO DEL RATON
//
// Era un cuadrado rojo de 8x8: se veia, pero no apuntaba a nada -- con
// un cuadrado no se sabe cual de sus esquinas es el punto que pulsa, y
// al pasar por encima de algo rojo desaparecia.
//
// Ahora es la flecha de toda la vida, 12x19, con BORDE NEGRO y relleno
// BLANCO. El borde no es adorno: sin el, la flecha se pierde sobre un
// fondo claro, y un puntero que no se ve sobre la mitad de las ventanas
// no sirve. Con los dos colores se ve sobre cualquier cosa.
//
// El punto que pulsa es la esquina de arriba a la izquierda (0,0) --
// la punta de la flecha --, igual que antes con el cuadrado, asi que
// nada de lo que ya funcionaba cambia de sitio.
#define CURSOR_W 12
#define CURSOR_H 19
// 'X' borde, '.' relleno, ' ' transparente
static const char *CURSOR_FLECHA[CURSOR_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X..X  ",
    "      X..X  ",
    "       XX   ",
};

// ---- EL CURSOR, COMO CAPA ENCIMA ----
//
// Antes, mover el raton un pixel ponia needs_redraw y eso significaba
// COMPONER LA PANTALLA ENTERA: el fondo, todas las ventanas abiertas, la barra
// de tareas, y la conversion de color y copia de los cinco megas de pantalla.
// Con una sola ventana se aguantaba; con el monitor del sistema (560x620) y un
// par de programas mas, cada movimiento del raton costaba lo mismo que
// redibujar el escritorio completo, y el raton iba a tirones cuantas mas cosas
// hubiera abiertas. Justo lo que se notaba.
//
// Ahora se guardan los pixeles que hay DEBAJO del cursor antes de pintarlo. Si
// lo unico que ha pasado es que el raton se ha movido, no se compone nada: se
// devuelven los pixeles guardados a su sitio, se guardan los del sitio nuevo,
// se pinta el cursor alli, y se publican solo esos dos rectangulos. Son unos
// 3,5 KB en vez de 5 MB.
//
// El tamaño maximo es el del cursor a escala doble, que es la mayor que
// permite dibujar_cursor.
#define CURSOR_MAX_W (CURSOR_W * 2)
#define CURSOR_MAX_H (CURSOR_H * 2)
static uint32_t cursor_debajo[CURSOR_MAX_H][CURSOR_MAX_W];
static int32_t cursor_guardado_x = 0, cursor_guardado_y = 0;
static int32_t cursor_guardado_w = 0, cursor_guardado_h = 0;  // 0 = no hay nada guardado
static bool cursor_movido = false;   // el raton se movio y NADA mas cambio

static int32_t cursor_escala(void) {
    int32_t esc = g_icon_scale;
    if (esc < 1) esc = 1;
    if (esc > 2) esc = 2;
    return esc;
}

// Copia a 'cursor_debajo' lo que hay en pantalla donde va a ir el cursor.
static void cursor_guardar(int32_t x, int32_t y) {
    int32_t esc = cursor_escala();
    int32_t w = CURSOR_W * esc, h = CURSOR_H * esc;
    uint32_t ancho = fb_width(), alto = fb_height();
    for (int32_t f = 0; f < h; f++) {
        for (int32_t c = 0; c < w; c++) {
            int32_t px = x + c, py = y + f;
            if (px < 0 || py < 0 || (uint32_t)px >= ancho || (uint32_t)py >= alto) continue;
            cursor_debajo[f][c] = fb_get_pixel((uint32_t)px, (uint32_t)py);
        }
    }
    cursor_guardado_x = x; cursor_guardado_y = y;
    cursor_guardado_w = w; cursor_guardado_h = h;
}

// Devuelve a la pantalla lo que habia debajo del cursor.
static void cursor_restaurar(void) {
    if (cursor_guardado_w == 0) return;
    uint32_t ancho = fb_width(), alto = fb_height();
    for (int32_t f = 0; f < cursor_guardado_h; f++) {
        for (int32_t c = 0; c < cursor_guardado_w; c++) {
            int32_t px = cursor_guardado_x + c, py = cursor_guardado_y + f;
            if (px < 0 || py < 0 || (uint32_t)px >= ancho || (uint32_t)py >= alto) continue;
            fb_put_pixel((uint32_t)px, (uint32_t)py, cursor_debajo[f][c]);
        }
    }
}

static void dibujar_cursor(int32_t x, int32_t y) {
    // Crece con los iconos del escritorio: en una pantalla grande, una
    // flecha de 12 px se pierde. Es la misma escala que ya eligio el
    // usuario para los iconos, asi que no hace falta otro ajuste.
    int32_t esc = g_icon_scale;
    if (esc < 1) esc = 1;
    if (esc > 2) esc = 2;

    uint32_t ancho = fb_width(), alto = fb_height();
    for (int32_t fila = 0; fila < CURSOR_H; fila++) {
        const char *linea = CURSOR_FLECHA[fila];
        for (int32_t col = 0; col < CURSOR_W; col++) {
            char c = linea[col];
            if (c == ' ') continue;
            uint32_t color = (c == 'X') ? 0x00000000 : 0x00FFFFFF;
            for (int32_t sy = 0; sy < esc; sy++) {
                for (int32_t sx = 0; sx < esc; sx++) {
                    int32_t px = x + col * esc + sx;
                    int32_t py = y + fila * esc + sy;
                    if (px < 0 || py < 0) continue;
                    if ((uint32_t)px >= ancho || (uint32_t)py >= alto) continue;
                    fb_put_pixel((uint32_t)px, (uint32_t)py, color);
                }
            }
        }
    }
}

// Carga el escritorio guardado. Devuelve false si el archivo no
// existe (primer arranque: quien llama debe crear los iconos por
// defecto y guardarlos).
bool wm_load_desktop_icons(void) {
    int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, DESKTOP_CFG_NAME);
    if (idx < 0) return false;
    static uint8_t buf[8 + MAX_ICONS * DESKTOP_RECORD_SIZE];
    int32_t got = nemofs_read_file((uint32_t)idx, buf, sizeof(buf));
    if (got < 8) return false;

    uint32_t campo = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    uint32_t scale = campo & 0xFFFF;
    g_cfg_version = (int32_t)(campo >> 16);
    if (scale != 1 && scale != 2) return false; // formato viejo o corrupto -- mejor recrear por defecto
    g_icon_scale = (int32_t)scale;

    uint32_t n = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    if (n > (uint32_t)MAX_ICONS) n = MAX_ICONS;
    uint32_t pos = 8;
    icon_count = 0;
    for (uint32_t i = 0; i < n && pos + DESKTOP_RECORD_SIZE <= (uint32_t)got; i++) {
        desktop_icon_t *ic = &icons[icon_count];
        ic->used = true;
        int j = 0;
        while (j < 16) { ic->label[j] = (char)buf[pos + j]; j++; }
        ic->label[15] = '\0';
        pos += 16;
        j = 0;
        while (j < 32) { ic->target[j] = (char)buf[pos + j]; j++; }
        ic->target[31] = '\0';
        pos += 32;
        ic->icon_id = (int32_t)((uint32_t)buf[pos] | ((uint32_t)buf[pos+1] << 8) | ((uint32_t)buf[pos+2] << 16) | ((uint32_t)buf[pos+3] << 24));
        pos += 4;
        ic->x = (int32_t)((uint32_t)buf[pos] | ((uint32_t)buf[pos+1] << 8) | ((uint32_t)buf[pos+2] << 16) | ((uint32_t)buf[pos+3] << 24));
        pos += 4;
        ic->y = (int32_t)((uint32_t)buf[pos] | ((uint32_t)buf[pos+1] << 8) | ((uint32_t)buf[pos+2] << 16) | ((uint32_t)buf[pos+3] << 24));
        pos += 4;
        icon_count++;
    }
    wm_request_redraw();
    return true;
}

static void request_launch(const char *target_pro, const char *arg, int32_t requesting_window, uint32_t search_dir) {
    int i = 0;
    while (target_pro[i] != '\0' && i < 31) { launch_target[i] = target_pro[i]; i++; }
    launch_target[i] = '\0';
    i = 0;
    if (arg) {
        while (arg[i] != '\0' && i < TASK_LAUNCH_ARG_MAX - 1) { launch_arg[i] = arg[i]; i++; }
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
    windows[idx].sin_maximizar = false;     // un hueco reutilizado no hereda los de otra app
    windows[idx].sin_minimizar = false;
    windows[idx].sin_cerrar = false;
    windows[idx].event_mode = false;
    for (int b = 0; b < MAX_BUTTONS_PER_WINDOW; b++) win_buttons[idx][b].used = false;
    btn_last_left[idx] = false;

    z_order[window_count] = idx;
    window_count++;

    console_queue_reset(idx); // bug real: sin esto, una ventana nueva
                               // hereda bytes sin leer de la tarea
                               // ANTERIOR que uso este mismo indice

    set_focus(idx);
    wm_request_redraw();
    return idx;
}

// La ventana ha cambiado de tamaño. Se marca su lienzo como
// cambiado, para que la siguiente publicacion copie el tamaño NUEVO entero:
// el lienzo publicado (window_front) solo tenia copiado el tamaño de
// antes, y lo demas estaba a cero. Al maximizar, esa zona se veia NEGRA
// en cualquier app que no volviera a dibujar (las de controles, por
// ejemplo). El lienzo de dibujo si esta limpio: empieza entero en el gris
// de las ventanas (wm_set_owns_content). Y se avisa a la app, si escucha
// eventos, para que se recoloque.
static void tamano_cambiado(int32_t idx) {
    contenido_cambiado[idx] = true;
    if (windows[idx].event_mode) gadgets_fire_raw_event(idx, EVENT_WINDOWSIZE, 0, 0, 0, 0);
    wm_request_redraw();
}

// Reconfigura una ventana ya existente -- ver wm.h.
void wm_configure_window(int32_t idx, const char *title, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    windows[idx].x = x;
    windows[idx].y = y;
    bool otro_tamano = windows[idx].w != w || windows[idx].h != h;
    windows[idx].w = w;
    windows[idx].h = h;
    int i = 0;
    while (title[i] != '\0' && i < 23) { windows[idx].title[i] = title[i]; i++; }
    windows[idx].title[i] = '\0';
    if (otro_tamano) tamano_cambiado(idx);
    wm_request_redraw();
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
    wm_request_redraw();
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
    wm_request_redraw();
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

// Posiciones de los botones de la barra de titulo. UNA sola funcion para
// dibujarlos y para los clics: si cada sitio calculara las suyas, ocultar un
// boton movería los demas en uno y no en el otro, y un clic caeria en el
// boton equivocado. De derecha a izquierda: cerrar, maximizar, minimizar;
// los que hay, juntos hacia la derecha, sin huecos. Los que no hay: NO_HAY
// (fuera de cualquier clic). Devuelve la x del mas a la izquierda, o el
// borde derecho si no hay ninguno (para recortar el titulo).
#define NO_HAY (-1000000)
static int32_t botones_titulo(const window_t *w, int32_t *close_x, int32_t *max_x, int32_t *min_x) {
    int32_t x = w->x + (int32_t)w->w;          // el borde derecho: el siguiente boton va a su izquierda
    *close_x = NO_HAY;
    *max_x = NO_HAY;
    *min_x = NO_HAY;
    if (!w->sin_cerrar)    { x -= WIN_BTN_GAP; *close_x = x; }
    if (!w->sin_maximizar) { x -= WIN_BTN_GAP; *max_x = x; }
    if (!w->sin_minimizar) { x -= WIN_BTN_GAP; *min_x = x; }
    return x;
}

// El programa dueño de la ventana no quiere alguno de sus botones.
void wm_set_window_buttons(int32_t idx, bool sin_maximizar, bool sin_minimizar, bool sin_cerrar) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    windows[idx].sin_maximizar = sin_maximizar;
    windows[idx].sin_minimizar = sin_minimizar;
    windows[idx].sin_cerrar = sin_cerrar;
    wm_request_redraw();
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
    tamano_cambiado(idx);
}

// ---------------------------------------------------------------
// Texto de la INTERFAZ (titulos, barra de tareas, menu Inicio, gadgets,
// dialogos): fuente proporcional del sistema, sans 12 (negrita para
// titulos), con antialiasing -- desde la Semana 23. Antes todo esto era
// la 5x7 en mayusculas, porque era lo unico que habia.
//
// La (x, y) que reciben estas funciones es la MISMA que recibia
// fb_draw_string con la 5x7 (y = parte de arriba de un glifo de 7 px),
// para no tener que recolocar los treinta y tantos sitios que las
// llaman: UI_TEXT_DY compensa la diferencia de altura para que las
// mayusculas queden centradas donde estaban.
//
// Lo que dibujan los PROGRAMAS sin pedir fuente (DrawText a secas)
// sigue siendo la 5x7 escala 1 a proposito: editor.pro, ide.pro y
// shell.pro colocan el cursor a columna*6 px contando con ella.
// ---------------------------------------------------------------
#define UI_TEXT_DY (-3)
#define UI_FONT_PX 12

static const nfnt_cara_t *ui_font(bool bold) {
    static const nfnt_cara_t *normal = 0, *negrita = 0;
    static bool buscado = false;
    if (!buscado) { normal = fonts_find("sans", UI_FONT_PX, false, false); negrita = fonts_find("sans", UI_FONT_PX, true, false); buscado = (fonts_num_caras() > 0); }
    return bold ? negrita : normal;
}

typedef struct { int32_t clip_x1; } fb_ctx_t;   // clip_x1: no dibujar a partir de esta x (0 = sin limite)
static uint32_t fb_ctx_get(void *c, int32_t x, int32_t y) { (void)c; if (x < 0 || y < 0) return 0; return fb_get_pixel((uint32_t)x, (uint32_t)y); }
static void fb_ctx_put(void *c, int32_t x, int32_t y, uint32_t color) {
    fb_ctx_t *k = (fb_ctx_t *)c;
    if (x < 0 || y < 0 || x >= (int32_t)fb_width() || y >= (int32_t)fb_height()) return;
    if (k && k->clip_x1 > 0 && x >= k->clip_x1) return;
    fb_put_pixel((uint32_t)x, (uint32_t)y, color);
}

// Texto de interfaz sobre el framebuffer. clip_x1 > 0 recorta por la derecha.
static void ui_text_clip(int32_t x, int32_t y, const char *s, uint32_t color, bool bold, int32_t clip_x1) {
    const nfnt_cara_t *f = ui_font(bold);
    if (!f) { fb_draw_string((uint32_t)x, (uint32_t)y, s, color, 1); return; }
    fb_ctx_t k = { clip_x1 };
    fonts_dibujar(f, x, y + UI_TEXT_DY, s, color, fb_ctx_get, fb_ctx_put, &k);
}
static void ui_text(int32_t x, int32_t y, const char *s, uint32_t color, bool bold) { ui_text_clip(x, y, s, color, bold, 0); }
static uint32_t ui_text_width(const char *s, bool bold) {
    const nfnt_cara_t *f = ui_font(bold);
    return f ? fonts_ancho_texto(f, s) : text_width(s, 1);
}

// Igual, sobre el bufer de contenido de una ventana (gadgets, dialogos).
typedef struct { int32_t win; } content_ctx_t;
static uint32_t content_ctx_get(void *c, int32_t x, int32_t y) { if (x < 0 || y < 0) return 0; return wm_content_get_pixel(((content_ctx_t *)c)->win, (uint32_t)x, (uint32_t)y); }
static void content_ctx_put(void *c, int32_t x, int32_t y, uint32_t color) { if (x < 0 || y < 0) return; wm_content_put_pixel(((content_ctx_t *)c)->win, (uint32_t)x, (uint32_t)y, color); }
void wm_content_ui_text(int32_t idx, uint32_t x, uint32_t y, const char *s, uint32_t color) {
    const nfnt_cara_t *f = ui_font(false);
    if (!f) { wm_content_draw_string(idx, x, y, s, color, 1); return; }
    content_ctx_t k = { idx };
    fonts_dibujar(f, (int32_t)x, (int32_t)y + UI_TEXT_DY, s, color, content_ctx_get, content_ctx_put, &k);
}
uint32_t wm_ui_text_width(const char *s) { return ui_text_width(s, false); }
uint32_t wm_ui_text_height(void) { const nfnt_cara_t *f = ui_font(false); return f ? fonts_alto_linea(f) : FONT_HEIGHT; }

// Reloj de la barra de tareas: hora real del RTC. Se vuelve a pintar
// cuando cambia el minuto (ver wm_update).
static int ultimo_minuto = -1;
static void hora_actual(char out[6]) {
    int y, mo, d, h, mi, s;
    rtc_to_civil(rtc_unix_timestamp(), &y, &mo, &d, &h, &mi, &s);
    out[0] = (char)('0' + h / 10); out[1] = (char)('0' + h % 10); out[2] = ':';
    out[3] = (char)('0' + mi / 10); out[4] = (char)('0' + mi % 10); out[5] = 0;
    ultimo_minuto = mi;
}

// Pantalla de apagado/reinicio -- en vez de dejar al usuario con una
// pantalla negra sin explicacion (que es justo lo que pasaba antes),
// mostramos un mensaje claro, presentamos ese fotograma, y solo
// entonces detenemos la maquina.
static void show_shutdown_screen(const char *message) {
    fb_fill_rect(0, 0, fb_width(), fb_height(), 0x00000000);
    const nfnt_cara_t *grande = fonts_find("sans", 32, true, false);
    if (grande) {
        uint32_t tw = fonts_ancho_texto(grande, message);
        fb_ctx_t k = { 0 };
        fonts_dibujar(grande, (int32_t)((fb_width() - tw) / 2), (int32_t)(fb_height() / 2 - fonts_alto_linea(grande) / 2), message, COLOR_TEXT_LIGHT, fb_ctx_get, fb_ctx_put, &k);
    } else {
        uint32_t tw = text_width(message, 3);
        fb_draw_string((fb_width() - tw) / 2, fb_height() / 2, message, COLOR_TEXT_LIGHT, 3);
    }
    fb_present();
}

void wm_update(void) {
    {
        int y, mo, d, h, mi, s;
        rtc_to_civil(rtc_unix_timestamp(), &y, &mo, &d, &h, &mi, &s);
        if (mi != ultimo_minuto) wm_request_redraw();
    }
    int32_t mx = mouse_x();
    int32_t my = mouse_y();
    bool left = mouse_left_down();
    bool left_pressed_edge = left && !last_left_down;
    bool left_released_edge = !left && last_left_down;

    // El raton se movio. Antes esto ponia needs_redraw y componia la pantalla
    // entera; ahora solo marca que el CURSOR esta en otro sitio, y de eso se
    // encarga la capa de encima (ver cursor_guardar). Si ademas cambia
    // cualquier otra cosa, needs_redraw se pone por su cuenta y manda ella.
    static int32_t last_mx = -1, last_my = -1;
    if (mx != last_mx || my != last_my) {
        cursor_movido = true;
        last_mx = mx;
        last_my = my;
    }

    if (left_pressed_edge) {
        uint32_t fbh = fb_height();

        // Menu Start abierto -- comprobamos sus filas antes que nada
        if (start_menu_open) {
            int32_t m_x = 4, m_top = menu_top();
            bool cerrar = true;

            // ¿el submenu? Se mira ANTES: esta dibujado encima y a la
            // derecha, y parte de el cae fuera del panel principal.
            if (menu_sec_abierta >= 0 && menu_sec_abierta < menu_n_secs) {
                menu_sec_t *sc = &menu_secs[menu_sec_abierta];
                int32_t sx = m_x + MENU_ANCHO - 2;
                int32_t sh = sc->n_items * MENU_FILA_H + 6;
                int32_t sy = m_top + 3 + menu_sec_abierta * MENU_FILA_H;
                if (sy + sh > (int32_t)fbh - TASKBAR_H) sy = (int32_t)fbh - TASKBAR_H - sh;
                if (sy < 0) sy = 0;
                if (point_in_rect(mx, my, sx, sy, MENU_SUB_ANCHO, (uint32_t)sh)) {
                    int32_t k = (my - sy - 3) / MENU_FILA_H;
                    if (k >= 0 && k < sc->n_items) {
                        request_launch(sc->items[k].destino, 0, -1, 0xFFFFFFFF);
                    }
                }
            }

            if (cerrar && point_in_rect(mx, my, m_x, m_top, MENU_ANCHO, (uint32_t)menu_alto())) {
                int32_t fila = (my - m_top - 3) / MENU_FILA_H;
                if (fila >= 0 && fila < menu_n_secs) {
                    // una seccion: se abre o se cierra su submenu, el
                    // menu NO se cierra
                    menu_sec_abierta = (menu_sec_abierta == fila) ? -1 : fila;
                    cerrar = false;
                } else if (fila == menu_n_secs) {
                    show_shutdown_screen("APAGANDO NEMO OS...");
                    sound_shutdown();          // que el jack no se quede zumbando
                    power_shutdown();
                } else if (fila == menu_n_secs + 1) {
                    show_shutdown_screen("REINICIANDO...");
                    sound_shutdown();
                    power_reset();
                }
            }

            if (cerrar) { start_menu_open = false; menu_sec_abierta = -1; }
            wm_request_redraw();
        }
        // Boton Start
        else if (point_in_rect(mx, my, 4, (int32_t)fbh - TASKBAR_H + 4, START_BTN_W, TASKBAR_H - 8)) {
            start_menu_open = true;
            menu_sec_abierta = -1;
            wm_request_redraw();
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
                        int32_t close_x, max_x, min_x;
                        botones_titulo(w, &close_x, &max_x, &min_x);
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
                                gadgets_fire_raw_event(win_idx, EVENT_WINDOWCLOSE, 0, 0, 0, 0);
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
                        if (point_in_rect(mx, my, icons[i].x, icons[i].y, icon_cell_w(), icon_cell_h())) {
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
        wm_request_redraw();
    }

    if (left_released_edge) {
        dragging_window = -1;
    }

    if (dragging_window >= 0 && left) {
        window_t *w = &windows[dragging_window];
        w->x = mx - drag_offset_x;
        w->y = my - drag_offset_y;
        wm_request_redraw();
    }

    last_left_down = left;
}

static void blit_content(window_t *w, int32_t idx);

static void draw_window(int32_t idx) {
    window_t *w = &windows[idx];

    // Lo pesado de pintar una ventana -- rellenar el cuerpo y la barra, y
    // pegar su contenido -- se hace SIN el candado grande, con una copia
    // de la ventana tomada mientras aun se tiene. Ver la nota en
    // wm_draw_if_needed. Solo toca el bufer de pantalla (que unicamente
    // escribe el compositor) y el lienzo publicado de la ventana.
    window_t copia = *w;
    bkl_soltar();
    // El cuerpo se rellenaba ENTERO y sin recortar, en cada composicion, y
    // acto seguido el contenido del programa lo tapaba casi todo: en una
    // ventana de 1000x700 eran 700.000 pixeles pintados para nada, 60 veces
    // por segundo, POR VENTANA ABIERTA. De ahi venia que el raton empeorara
    // con cada programa abierto. Ahora:
    //   - todo se recorta al rectangulo sucio, y
    //   - si la ventana tiene lienzo propio, solo se rellenan los margenes
    //     que el lienzo no cubre (el lienzo esta topado a MAX_CONTENT_*).
    if (copia.owns_content) {
        int32_t cw = (int32_t)(copia.w < MAX_CONTENT_W ? copia.w : MAX_CONTENT_W);
        int32_t ch = (int32_t)(copia.h < MAX_CONTENT_H ? copia.h : MAX_CONTENT_H);
        int32_t cuerpo_y = copia.y + TITLE_BAR_H;
        if (cw < (int32_t)copia.w) {          // franja de la derecha
            rellenar_recortado(copia.x + cw, cuerpo_y,
                               (int32_t)copia.w - cw, (int32_t)copia.h, COLOR_WIN_BODY);
        }
        if (ch < (int32_t)copia.h) {          // franja de abajo
            rellenar_recortado(copia.x, cuerpo_y + ch,
                               (int32_t)copia.w, (int32_t)copia.h - ch, COLOR_WIN_BODY);
        }
    } else {
        rellenar_recortado(copia.x, copia.y + TITLE_BAR_H,
                           (int32_t)copia.w, (int32_t)copia.h, COLOR_WIN_BODY);
    }
    rellenar_recortado(copia.x, copia.y, (int32_t)copia.w, TITLE_BAR_H, COLOR_WIN_TITLE);
    if (copia.owns_content) {
        blit_content(&copia, idx);
    }
    bkl_tomar();

    fb_draw_rect_border((uint32_t)w->x, (uint32_t)w->y, w->w, w->h + TITLE_BAR_H, COLOR_WIN_BORDER);
    // Botones: minimizar, maximizar, cerrar (de izquierda a derecha), solo
    // los que el programa quiere; el titulo aprovecha el sitio que dejan.
    int32_t close_x, max_x, min_x;
    int32_t primero = botones_titulo(w, &close_x, &max_x, &min_x);
    ui_text_clip(w->x + 6, w->y + 6, w->title, COLOR_TEXT_LIGHT, true, primero - 4);
    int32_t btn_y   = w->y + 4;

    if (min_x != NO_HAY) {
        fb_fill_rect((uint32_t)min_x, (uint32_t)btn_y, WIN_BTN_SIZE, WIN_BTN_SIZE, COLOR_BTN_NORMAL);
        ui_text(min_x + 4, btn_y + 5, "\xE2\x80\x93", COLOR_TEXT_LIGHT, true);   /* guion largo */
    }
    if (max_x != NO_HAY) {
        fb_fill_rect((uint32_t)max_x, (uint32_t)btn_y, WIN_BTN_SIZE, WIN_BTN_SIZE, COLOR_BTN_NORMAL);
        ui_text(max_x + 4, btn_y + 5, "o", COLOR_TEXT_LIGHT, true);
    }

    if (close_x != NO_HAY) {
        fb_fill_rect((uint32_t)close_x, (uint32_t)btn_y, WIN_BTN_SIZE, WIN_BTN_SIZE, COLOR_BTN_CLOSE);
        ui_text(close_x + 4, btn_y + 5, "\xC3\x97", COLOR_TEXT_LIGHT, true);      /* aspa (U+00D7) */
    }
}

static void draw_desktop_icons(void) {
    for (int i = 0; i < icon_count; i++) {
        if (!icons[i].used) continue;
        // BUG REAL CORREGIDO: se dibujaba SIEMPRE el icono de carpeta,
        // sin importar cual llevara asignado cada uno -- ahora que hay
        // eleccion de icono (ver el editor de escritorio), hay que usar
        // el de verdad.
        const uint8_t *rgba = icon_get_rgba(icons[i].icon_id);
        if (!rgba) rgba = icon_get_rgba(ICON_FOLDER); // por si acaso, nunca dejar sin dibujar
        fb_blit_icon_scaled((uint32_t)icons[i].x + 4, (uint32_t)icons[i].y, ICON_SIZE, (uint32_t)g_icon_scale, rgba);
        {
            uint32_t tw = ui_text_width(icons[i].label, false);
            int32_t cx = icons[i].x + (int32_t)icon_cell_w() / 2 - (int32_t)tw / 2;
            ui_text(cx, icons[i].y + (int32_t)icon_cell_h() - 14, icons[i].label, COLOR_TEXT_LIGHT, false);
        }
    }
}

// ---- MENU.CFG: leer y escribir ----

static void menu_defecto(void) {
    menu_n_secs = 0;
    // static const, no local: un array AUTOMATICO con inicializadores lo
    // resuelve gcc copiandolo desde .rodata con memcpy, y en un kernel
    // sin biblioteca estandar memcpy no existe. Estatico, ya esta ahi.
    static const struct { const char *sec; int32_t ic; const char *items[6][3]; } d[] = {
        // La ayuda, la primera: si se llega aqui es que
        // MENU.CFG falta o esta mal, y entonces es cuando mas falta
        // hace que el manual este a mano.
        { "Ayuda", ICON_HELP, {
            { "Manual",   "ayuda.lua",   "21" },
            { 0, 0, 0 } } },
        { "Programacion", ICON_DESIGN, {
            { "IDE",      "ide.pro",     "5"  },
            { "Aronnax",  "aronnax.lua", "14" },
            { "Editor",   "editor.pro",  "3"  },
            { "Shell",    "shell.pro",   "0"  },
            { "Lua",      "shell.lua",   "15" },
            { 0, 0, 0 } } },
        { "Accesorios", ICON_PAINT, {
            { "Pintor",     "pintor.lua",     "13" },
            { "Navegante",  "navegante.lua",  "12" },
            { "Calculadora","calculadora.lua","20" },
            { "Notas",      "notas.lua",      "3"  },
            { "Reloj",      "reloj.lua",      "19" },
            { 0, 0, 0 } } },
        { "Sistema", ICON_NAUTILUS, {
            { "Explorador",      "explorer.pro",       "2"  },
            { "Escritorio",      "desktoped.pro",      "1"  },
            { "Pantalla",        "screensettings.pro", "11" },
            { "Gestor de tareas","gestor_tareas.lua",  "21" },
            { "Monitor",         "monitor_sistema.lua","22" },
            { 0, 0, 0 } } },
    };
    for (uint32_t i = 0; i < sizeof(d) / sizeof(d[0]) && menu_n_secs < MENU_MAX_SEC; i++) {
        menu_sec_t *sc = &menu_secs[menu_n_secs++];
        int j = 0; while (j < MENU_LARGO - 1 && d[i].sec[j]) { sc->etiqueta[j] = d[i].sec[j]; j++; }
        sc->etiqueta[j] = 0;
        sc->icono = d[i].ic;
        sc->n_items = 0;
        for (int k = 0; k < 6 && d[i].items[k][0]; k++) {
            menu_item_t *it = &sc->items[sc->n_items++];
            int q = 0; while (q < MENU_LARGO - 1 && d[i].items[k][0][q]) { it->etiqueta[q] = d[i].items[k][0][q]; q++; }
            it->etiqueta[q] = 0;
            q = 0; while (q < MENU_LARGO - 1 && d[i].items[k][1][q]) { it->destino[q] = d[i].items[k][1][q]; q++; }
            it->destino[q] = 0;
            it->icono = 0;
            for (const char *c = d[i].items[k][2]; *c >= '0' && *c <= '9'; c++) it->icono = it->icono * 10 + (*c - '0');
        }
    }
}

// Tokenizador minimo: palabras separadas por espacios, el resto de la
// linea tras ';' es comentario.
static const char *menu_palabra(const char *p, char *dst, uint32_t max) {
    while (*p == ' ' || *p == '\t') p++;
    uint32_t n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        if (n < max - 1) dst[n++] = *p;
        p++;
    }
    dst[n] = 0;
    return p;
}

void wm_menu_cargar(void) {
    menu_n_secs = 0;
    menu_sec_abierta = -1;
    int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, MENU_CFG);
    if (idx < 0) { menu_defecto(); return; }

    static uint8_t buf[4096];
    int32_t n = nemofs_read_file((uint32_t)idx, buf, sizeof(buf) - 1);
    if (n <= 0) { menu_defecto(); return; }
    buf[n] = 0;

    const char *p = (const char *)buf;
    while (*p) {
        // una linea
        const char *fin = p;
        while (*fin && *fin != '\n') fin++;
        char linea[256];
        uint32_t L = 0;
        for (const char *q = p; q < fin && L < sizeof(linea) - 1; q++) {
            if (*q == ';') break;                    // comentario
            linea[L++] = *q;
        }
        linea[L] = 0;
        p = (*fin) ? fin + 1 : fin;

        char clave[16];
        const char *r = menu_palabra(linea, clave, sizeof(clave));
        if (clave[0] == 0) continue;

        if (clave[0] == 's' && menu_n_secs < MENU_MAX_SEC) {          // seccion
            menu_sec_t *sc = &menu_secs[menu_n_secs++];
            r = menu_palabra(r, sc->etiqueta, MENU_LARGO);
            char ic[8]; menu_palabra(r, ic, sizeof(ic));
            sc->icono = 0;
            for (const char *c = ic; *c >= '0' && *c <= '9'; c++) sc->icono = sc->icono * 10 + (*c - '0');
            sc->n_items = 0;
        } else if (clave[0] == 'i' && menu_n_secs > 0) {              // item
            menu_sec_t *sc = &menu_secs[menu_n_secs - 1];
            if (sc->n_items >= MENU_MAX_ITEM) continue;
            menu_item_t *it = &sc->items[sc->n_items];
            r = menu_palabra(r, it->etiqueta, MENU_LARGO);
            r = menu_palabra(r, it->destino, MENU_LARGO);
            char ic[8]; menu_palabra(r, ic, sizeof(ic));
            it->icono = 0;
            for (const char *c = ic; *c >= '0' && *c <= '9'; c++) it->icono = it->icono * 10 + (*c - '0');
            if (it->etiqueta[0] && it->destino[0]) sc->n_items++;
        }
    }
    // Un archivo vacio o ilegible NO deja el sistema sin menu.
    if (menu_n_secs == 0) menu_defecto();
    wm_request_redraw();
}

static void dibujar_menu_inicio(void) {
    int32_t mx = 4, top = menu_top(), alto = menu_alto();

    fb_fill_rect((uint32_t)mx, (uint32_t)top, MENU_ANCHO, (uint32_t)alto, COLOR_MENU_BG);
    fb_draw_rect_border((uint32_t)mx, (uint32_t)top, MENU_ANCHO, (uint32_t)alto, COLOR_WIN_BORDER);

    for (int32_t i = 0; i < menu_n_secs; i++) {
        int32_t y = top + 3 + i * MENU_FILA_H;
        if (i == menu_sec_abierta) fb_fill_rect((uint32_t)mx + 1, (uint32_t)y, MENU_ANCHO - 2, MENU_FILA_H, COLOR_TASK_BTN_ACTIVE);
        menu_icono(mx + 5, y + 2, menu_secs[i].icono);
        ui_text(mx + 34, y + 8, menu_secs[i].etiqueta, COLOR_TEXT_DARK, true);
        // la flechita de "hay mas dentro"
        ui_text(mx + MENU_ANCHO - 14, y + 8, ">", COLOR_TEXT_DARK, false);
    }

    int32_t y_ap = top + 3 + menu_n_secs * MENU_FILA_H;
    fb_draw_hline((uint32_t)mx, (uint32_t)y_ap, MENU_ANCHO, COLOR_WIN_BORDER);
    ui_text(mx + 34, y_ap + 8, "Apagar", COLOR_TEXT_DARK, true);
    ui_text(mx + 34, y_ap + MENU_FILA_H + 8, "Reiniciar", COLOR_TEXT_DARK, true);

    // el submenu de la seccion señalada
    if (menu_sec_abierta >= 0 && menu_sec_abierta < menu_n_secs) {
        menu_sec_t *sc = &menu_secs[menu_sec_abierta];
        int32_t sx = mx + MENU_ANCHO - 2;
        int32_t sh = sc->n_items * MENU_FILA_H + 6;
        int32_t sy = top + 3 + menu_sec_abierta * MENU_FILA_H;
        // que no se salga por abajo
        if (sy + sh > (int32_t)fb_height() - TASKBAR_H) sy = (int32_t)fb_height() - TASKBAR_H - sh;
        if (sy < 0) sy = 0;

        fb_fill_rect((uint32_t)sx, (uint32_t)sy, MENU_SUB_ANCHO, (uint32_t)sh, COLOR_MENU_BG);
        fb_draw_rect_border((uint32_t)sx, (uint32_t)sy, MENU_SUB_ANCHO, (uint32_t)sh, COLOR_WIN_BORDER);
        for (int32_t k = 0; k < sc->n_items; k++) {
            int32_t y = sy + 3 + k * MENU_FILA_H;
            menu_icono(sx + 5, y + 2, sc->items[k].icono);
            ui_text(sx + 34, y + 8, sc->items[k].etiqueta, COLOR_TEXT_DARK, false);
        }
    }
}

static void draw_taskbar(void) {
    uint32_t fbw = fb_width();
    uint32_t fbh = fb_height();

    fb_fill_rect(0, fbh - TASKBAR_H, fbw, TASKBAR_H, COLOR_TASKBAR);
    fb_fill_rect(4, fbh - TASKBAR_H + 4, START_BTN_W, TASKBAR_H - 8, COLOR_START_BTN);
    ui_text(12, (int32_t)(fbh - TASKBAR_H + 12), "Inicio", COLOR_TEXT_LIGHT, true);

    int32_t bx = 4 + START_BTN_W + 8;
    for (int i = 0; i < window_count; i++) {
        window_t *w = &windows[z_order[i]];
        if (!w->used) continue;
        bool is_top = (i == window_count - 1);
        fb_fill_rect((uint32_t)bx, fbh - TASKBAR_H + 4, 90, TASKBAR_H - 8,
                     is_top ? COLOR_TASK_BTN_ACTIVE : COLOR_TASK_BTN);
        ui_text_clip(bx + 4, (int32_t)(fbh - TASKBAR_H + 12), w->title, COLOR_TEXT_DARK, false, bx + 88);
        bx += 96;
    }

    {
        char hora[6]; hora_actual(hora);
        uint32_t tw = ui_text_width(hora, false);
        ui_text((int32_t)(fbw - tw - 10), (int32_t)(fbh - TASKBAR_H + 12), hora, COLOR_TEXT_DARK, false);
    }

    if (start_menu_open) dibujar_menu_inicio();
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
    contenido_cambiado[idx] = true;
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
    contenido_cambiado[idx] = true;
}

// Version publica de content_put_pixel -- para fonts.c (texto con
// antialiasing: necesita leer y escribir pixel a pixel).
void wm_content_put_pixel(int32_t idx, uint32_t x, uint32_t y, uint32_t color) {
    content_put_pixel(idx, x, y, color);
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
    // Como en text.c: la fuente ya tiene minusculas y
    // español, asi que se dibuja lo que viene, y la cadena se decodifica
    // como UTF-8 -- la ñ son dos bytes.
    uint32_t cursor_x = x;
    while (*str) {
        uint32_t uc = utf8_siguiente(&str);
        const uint8_t *rows = font5x7_glifo(uc);
        if (rows) {
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
    }
}

// "Pega" un icono RGBA (con transparencia) en el buffer de contenido
// de una ventana, mezclando segun el canal alfa -- igual que
// fb_blit_icon pero para el buffer propio de un programa en vez del
// framebuffer general.
void wm_content_blit_icon(int32_t idx, uint32_t x, uint32_t y, uint32_t size, const uint8_t *rgba) {
    wm_content_blit_icon_scaled(idx, x, y, size, 1, rgba);
}

// Igual que wm_content_blit_icon, pero cada pixel de origen se dibuja
// como un bloque scale x scale -- lo usa SYS_DRAW_ICON para dejar que
// un programa pida iconos mas grandes sin bitmaps aparte por tamaño
// (mismo mecanismo que SetFont con las fuentes).
void wm_content_blit_icon_scaled(int32_t idx, uint32_t x, uint32_t y, uint32_t size, uint32_t scale, const uint8_t *rgba) {
    if (idx >= 0 && idx < MAX_WINDOWS) contenido_cambiado[idx] = true;   // doble bufer
    if (scale < 1) scale = 1;
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    for (uint32_t iy = 0; iy < size; iy++) {
        for (uint32_t ix = 0; ix < size; ix++) {
            const uint8_t *px = &rgba[(iy * size + ix) * 4];
            uint8_t a = px[3];
            if (a == 0) continue;

            for (uint32_t sy = 0; sy < scale; sy++) {
                for (uint32_t sx = 0; sx < scale; sx++) {
                    uint32_t dx = x + ix * scale + sx;
                    uint32_t dy = y + iy * scale + sy;
                    if (dx >= MAX_CONTENT_W || dy >= MAX_CONTENT_H) continue;
                    uint32_t *dst = (uint32_t *)&window_content[idx][dy][dx * 4];
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
    if (idx >= 0 && idx < MAX_WINDOWS) contenido_cambiado[idx] = true;   // doble bufer
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
bool wm_content_save_rect(int32_t idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *dst) {
    if (idx < 0 || idx >= MAX_WINDOWS || !dst) return false;
    if (x >= MAX_CONTENT_W || y >= MAX_CONTENT_H) return false;
    if (x + w > MAX_CONTENT_W || y + h > MAX_CONTENT_H) return false;
    for (uint32_t iy = 0; iy < h; iy++) {
        const uint8_t *src = &window_content[idx][y + iy][x * 4];
        uint8_t *d = &dst[(uint32_t)iy * w * 4];
        for (uint32_t i = 0; i < w * 4; i++) d[i] = src[i];
    }
    return true;
}

void wm_content_restore_rect(int32_t idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *src) {
    if (idx < 0 || idx >= MAX_WINDOWS || !src) return;
    if (x >= MAX_CONTENT_W || y >= MAX_CONTENT_H) return;
    if (x + w > MAX_CONTENT_W || y + h > MAX_CONTENT_H) return;
    contenido_cambiado[idx] = true;
    for (uint32_t iy = 0; iy < h; iy++) {
        uint8_t *d = &window_content[idx][y + iy][x * 4];
        const uint8_t *s = &src[(uint32_t)iy * w * 4];
        for (uint32_t i = 0; i < w * 4; i++) d[i] = s[i];
    }
}

void wm_content_blit_image_rect(int32_t idx, uint32_t x, uint32_t y, uint32_t blit_w, uint32_t blit_h, uint32_t src_stride, const uint8_t *rgba, bool solid, bool has_mask, uint32_t mask_color) {
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    contenido_cambiado[idx] = true;   // doble bufer

    // CAMINO RAPIDO para la copia opaca sin mascara, que es
    // como se dibuja un fondo (DrawBlock). El bucle general comprueba,
    // POR CADA PIXEL: la mascara, el alfa, que el indice de ventana siga
    // siendo valido y que el punto caiga dentro del lienzo maximo. Nada
    // de eso cambia dentro de una fila, asi que se saca fuera una sola
    // vez y dentro solo queda leer tres bytes y escribir la palabra.
    //
    // Importa porque llenar la pantalla una vez (307.200 pixeles) es lo
    // que hace CADA fotograma un juego con scroll: en la Raspberry Pi 4
    // costaba 7,25 ms de los 10 que dura un fotograma a 100 por segundo.
    if (solid && !has_mask) {
        uint32_t w = blit_w, h = blit_h;
        if (x >= MAX_CONTENT_W || y >= MAX_CONTENT_H) return;
        if (x + w > MAX_CONTENT_W) w = MAX_CONTENT_W - x;     // recorte, una vez
        if (y + h > MAX_CONTENT_H) h = MAX_CONTENT_H - y;
        for (uint32_t iy = 0; iy < h; iy++) {
            const uint8_t *src = &rgba[(uint32_t)iy * src_stride * 4];
            uint32_t *dst = (uint32_t *)&window_content[idx][y + iy][x * 4];
            for (uint32_t ix = 0; ix < w; ix++) {
                dst[ix] = ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];
                src += 4;
            }
        }
        return;
    }

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
// Publica el dibujo de una ventana: copia su lienzo de dibujo al lienzo
// que se muestra, si ha cambiado desde la ultima vez. Se llama en puntos
// ENTRE fotogramas -- ver la nota junto a window_front. Solo copia la
// parte que ocupa la ventana, no el lienzo maximo entero.
void wm_publicar_contenido(int32_t idx) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    if (!contenido_cambiado[idx]) return;
    uint32_t cw = windows[idx].w < MAX_CONTENT_W ? windows[idx].w : MAX_CONTENT_W;
    uint32_t ch = windows[idx].h < MAX_CONTENT_H ? windows[idx].h : MAX_CONTENT_H;
    for (uint32_t y = 0; y < ch; y++) {
        const uint64_t *src = (const uint64_t *)&window_content[idx][y][0];
        uint64_t *dst = (uint64_t *)&window_front[idx][y][0];
        // de 2 en 2 pixeles (8 bytes); el pixel suelto del final, aparte
        uint32_t pares = cw / 2;
        for (uint32_t i = 0; i < pares; i++) dst[i] = src[i];
        if (cw & 1) {
            ((uint32_t *)dst)[cw - 1] = ((const uint32_t *)src)[cw - 1];
        }
    }
    contenido_cambiado[idx] = false;
    // Solo ha cambiado el contenido de ESTA ventana: se ensucia su rectangulo,
    // no la pantalla entera. Es el caso comun -- un programa repintandose -- y
    // justo el que crecia con cada programa abierto.
    wm_request_redraw_rect(windows[idx].x, windows[idx].y + TITLE_BAR_H,
                           (int32_t)cw, (int32_t)ch);
    medir_publicacion();
}

static void blit_content(window_t *w, int32_t idx) {
    uint32_t cw = w->w < MAX_CONTENT_W ? w->w : MAX_CONTENT_W;
    uint32_t ch = w->h < MAX_CONTENT_H ? w->h : MAX_CONTENT_H;
    // Fila a fila, no pixel a pixel: una llamada por fila en vez de una
    // por pixel (ver fb_blit_row). Lee lo PUBLICADO (window_front), no lo
    // que la tarea esta dibujando ahora.
    // Recortado al rectangulo sucio: de una ventana que no ha cambiado no se
    // copia ni un byte, y de la que ha cambiado solo su trozo. Aqui estan los
    // bytes que sumaba cada programa abierto.
    int32_t rx, ry, rw, rh;
    if (!sucio_recortado(&rx, &ry, &rw, &rh)) return;
    int32_t cy0 = w->y + TITLE_BAR_H;
    int32_t y0 = ry > cy0 ? ry : cy0;                       // primera fila a copiar
    int32_t y1 = (ry + rh) < (cy0 + (int32_t)ch) ? (ry + rh) : (cy0 + (int32_t)ch);
    int32_t x0 = rx > w->x ? rx : w->x;                     // primera columna
    int32_t x1 = (rx + rw) < (w->x + (int32_t)cw) ? (rx + rw) : (w->x + (int32_t)cw);
    if (y1 <= y0 || x1 <= x0) return;                       // fuera de lo sucio

    for (int32_t y = y0; y < y1; y++) {
        uint32_t fila = (uint32_t)(y - cy0);                // fila dentro del lienzo
        uint32_t col  = (uint32_t)(x0 - w->x);              // columna dentro del lienzo
        fb_blit_row(x0, y, (const uint32_t *)&window_front[idx][fila][col * 4],
                    (uint32_t)(x1 - x0));
    }
}

// Pedir el redibujado SIN decir de que trozo: se ensucia la pantalla entera.
// Es lo correcto y lo seguro para todo lo que mueve, cierra o reordena
// ventanas, que es cuando cambia de sitio lo que tapa a lo que. Quien sepa
// QUE rectangulo ha cambiado usa wm_request_redraw_rect y se ahorra el resto.
void wm_request_redraw(void) {
    needs_redraw = true;
    ensuciar_todo();
}

void wm_request_redraw_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    needs_redraw = true;
    ensuciar(x, y, w, h);
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
    tamano_cambiado(idx);
}

void wm_minimize_window(int32_t idx) {
    if (idx < 0 || idx >= MAX_WINDOWS || !windows[idx].used) return;
    windows[idx].minimized = true;
    windows[idx].maximized = false; // igual que wm_maximize_window hace con
    // minimized: los dos estados son excluyentes. Sin esto, minimizar una
    // ventana maximizada dejaba "maximized" pegado a true -- WindowMaximized()
    // seguia devolviendo 1, Y la siguiente llamada a MaximizeWindow entraba en
    // su atajo "ya lo esta" (if (w->maximized) return;) sin llegar nunca a
    // poner minimized a false tampoco. Confirmado en QEMU, auditoria de
    // Nemo-Blitz, tanda 2 (Ventanas).
    wm_request_redraw();
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

    wm_request_redraw();
}

void wm_draw_if_needed(void) {
    // Publicar el dibujo de toda ventana cuya tarea NO se este ejecutando
    // ahora mismo en otro nucleo: esa tarea esta parada en un punto de
    // cesion, asi que su dibujo es un fotograma completo. Esto cubre
    // tambien las ventanas que dibuja el propio kernel (dialogos...), que
    // no tienen ninguna tarea que ceda el turno. La unica que se deja como
    // estaba es la de una tarea que corre AHORA en otro nucleo: podria
    // estar a medio fotograma, y ella misma la publicara al ceder.
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used && !tasks_ventana_en_otro_nucleo(i)) {
            wm_publicar_contenido(i);
        }
    }

    // ---- EL CAMINO CORTO: solo se ha movido el raton ----
    //
    // Nada mas ha cambiado, asi que no hay nada que componer: se devuelve a su
    // sitio lo que habia bajo el cursor, se pinta en el sitio nuevo, y se
    // publican solo esos dos rectangulos. Unos 3,5 KB frente a los 5 MB de una
    // composicion entera, y sin tocar ni el fondo ni las ventanas -- que es lo
    // que crecia con cada programa abierto.
    if (!needs_redraw) {
        if (!cursor_movido) return;
        cursor_movido = false;

        int32_t vx = cursor_guardado_x, vy = cursor_guardado_y;
        int32_t vw = cursor_guardado_w, vh = cursor_guardado_h;

        bkl_soltar();
        cursor_restaurar();
        cursor_guardar((int32_t)mouse_x(), (int32_t)mouse_y());
        dibujar_cursor((int32_t)mouse_x(), (int32_t)mouse_y());
        if (vw > 0) fb_present_rect(vx, vy, vw, vh);            // de donde se fue
        fb_present_rect(cursor_guardado_x, cursor_guardado_y,
                        cursor_guardado_w, cursor_guardado_h);  // a donde ha ido
        bkl_tomar();
        return;
    }

    // ---- Componer SIN retener el candado grande ----
    //
    // Medido en la Pi: la composicion tenia el candado cogido el 97% del
    // tiempo, y cada llamada al sistema de un juego en otro nucleo
    // esperaba una composicion entera. Ahora el candado se suelta en las
    // partes que solo PINTAN -- rellenar el fondo, pintar el cuerpo de
    // cada ventana y pegar su contenido, y presentar la imagen -- y solo
    // se tiene en las que LEEN el estado de las ventanas (su posicion,
    // titulo, botones, la barra de tareas). Entre trozo y trozo, gracias
    // al candado por turnos, entran las llamadas de los demas nucleos.
    //
    // Por que es seguro: solo compone el turno del nucleo 0 (es el unico
    // que llama a esta funcion), y todo lo que escribe en el bufer de
    // pantalla esta en wm.c, aqui dentro. Lo que puede pasar es cosmetico:
    // si otro nucleo mueve o cierra una ventana en uno de los huecos, este
    // fotograma puede salir con esa ventana a medio cambiar; quien la
    // cambio pide otro redibujado y el siguiente sale bien.
    //
    // La marca se borra AL PRINCIPIO, no al final: si otro nucleo pide un
    // redibujado durante uno de los huecos, esa peticion tiene que
    // sobrevivir hasta la siguiente composicion. Borrandola al final se
    // perderia.
    needs_redraw = false;

    // El primer fotograma de la vida del sistema llega con needs_redraw ya
    // puesto pero sin rectangulo sucio (no se puede saber el tamaño de la
    // pantalla antes de tenerla). Cinturon y tirantes: si alguien pide
    // redibujado sin ensuciar nada, se compone entero como antes.
    if (!hay_sucio) ensuciar_todo();

    // El cursor es una capa aparte, y hay que quitarlo del bufer ANTES de
    // componer. Si no: fuera del rectangulo sucio el bufer conserva el
    // fotograma anterior, CON el cursor pintado en su sitio viejo, y el
    // cursor_guardar de mas abajo se llevaria esos pixeles como si fueran
    // fondo -- al siguiente movimiento del raton el camino corto los
    // devolveria a su sitio y quedaria un cursor fantasma pegado ahi.
    //
    // Su sitio viejo NO se mete en el rectangulo sucio: se publica aparte,
    // al final. Metiendolo, el rectangulo se estiraria desde la ventana que
    // ha cambiado hasta donde este el raton -- con un solo rectangulo, esa
    // caja englobante puede acabar siendo media pantalla.
    int32_t viejo_x = cursor_guardado_x, viejo_y = cursor_guardado_y;
    int32_t viejo_w = cursor_guardado_w, viejo_h = cursor_guardado_h;
    if (cursor_guardado_w > 0) cursor_restaurar();

    uint64_t t_comp = medir_ahora();   // medir.h: cuanto tarda componer

    // ¿Esta TODO lo sucio dentro del lienzo de la ventana que esta mas al
    // frente? Entonces el fondo, los iconos, la barra de tareas y las demas
    // ventanas estan tapados justo ahi, y pintarlos es trabajo tirado. Es el
    // caso mas comun de todos -- el programa con el foco repintandose -- y el
    // que empeoraba con cada ventana abierta de mas.
    int32_t solo = -1;
    {
        int32_t rx, ry, rw, rh;
        if (window_count > 0 && sucio_recortado(&rx, &ry, &rw, &rh)) {
            int32_t top = z_order[window_count - 1];
            window_t *t = &windows[top];
            if (t->used && !t->minimized && t->owns_content) {
                int32_t cw = (int32_t)(t->w < MAX_CONTENT_W ? t->w : MAX_CONTENT_W);
                int32_t ch = (int32_t)(t->h < MAX_CONTENT_H ? t->h : MAX_CONTENT_H);
                int32_t cy = t->y + TITLE_BAR_H;
                if (rx >= t->x && ry >= cy &&
                    rx + rw <= t->x + cw && ry + rh <= cy + ch) {
                    solo = top;
                }
            }
        }
    }

    if (solo < 0) {
        bkl_soltar();
        dibujar_fondo();
        bkl_tomar();

        draw_desktop_icons();
    }

    for (int i = 0; i < window_count; i++) {
        if (solo >= 0 && z_order[i] != solo) continue;
        if (windows[z_order[i]].used && !windows[z_order[i]].minimized) {
            draw_window(z_order[i]);
        }
    }

    if (solo < 0) draw_taskbar();

    // Los pixeles de debajo se guardan ANTES de pintar el cursor: son los que
    // el camino corto devolvera a su sitio en el proximo movimiento del raton.
    cursor_guardar((int32_t)mouse_x(), (int32_t)mouse_y());
    dibujar_cursor((int32_t)mouse_x(), (int32_t)mouse_y());
    cursor_movido = false;   // esta composicion ya lo ha dejado donde toca

    uint64_t t_pres = medir_ahora();

    // El rectangulo se copia y se pone a cero CON el candado cogido, igual
    // que needs_redraw: asi lo que otro nucleo ensucie mientras se publica
    // se guarda como rectangulo propio para el fotograma siguiente, en vez
    // de perderse o de obligar a repintar la pantalla entera.
    int32_t px, py, pw, ph;
    bool publicar = sucio_recortado(&px, &py, &pw, &ph);
    hay_sucio = false;
    int32_t nuevo_x = cursor_guardado_x, nuevo_y = cursor_guardado_y;
    int32_t nuevo_w = cursor_guardado_w, nuevo_h = cursor_guardado_h;

    bkl_soltar();
    // Solo se copia a la pantalla lo que ha cambiado: el rectangulo sucio y
    // los dos sitios del cursor, cada uno por su cuenta. En una pantalla de
    // 1080p una composicion entera son 8 MB por fotograma; el caso comun --
    // un programa repintando su ventana -- baja a los pixeles de esa ventana
    // mas dos cuadraditos de 12x19. Si se solapan, copiar dos veces unos
    // pocos pixeles no rompe nada. Nadie mas escribe en el bufer de
    // pantalla, asi que esto se hace sin el candado.
    if (publicar) fb_present_rect(px, py, pw, ph);
    if (viejo_w > 0) fb_present_rect(viejo_x, viejo_y, viejo_w, viejo_h);
    fb_present_rect(nuevo_x, nuevo_y, nuevo_w, nuevo_h);
    bkl_tomar();
    medir_presentacion(medir_ahora() - t_pres);

    medir_composicion(medir_ahora() - t_comp);
}
