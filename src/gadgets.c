// gadgets.c — Nemo OS
//
// Ver gadgets.h para el porque de este diseño. Todo vive en una unica
// tabla de gadgets; el tipo determina como se dibuja y que hace un
// clic sobre el.

#include "gadgets.h"
#include "wm.h"
#include "input.h"
#include "timer.h" // timer_get_ticks(), para CreateTimer
#include "syscall.h" // image_get_info(), para CreateToolBar
#include "heap.h" // kmalloc/kfree, para la cola de eventos real
#include <stddef.h> // NULL

#define MAX_GADGETS 64
#define GADGET_TEXT_MAX 48
#define LISTBOX_MAX_ITEMS 24
// Las cajas de texto SE PUEDEN ESCRIBIR, y su contenido vive en un
// almacen aparte: antes cada control cargaba con 40 lineas de 48 caracteres
// (120 KB entre los 64 controles) y aun asi no daba para un editor. Ahora hay
// cuatro cajas grandes (200 lineas de 128), y los demas controles no pagan nada.
#define TEXTAREA_MAX_LINES 200
#define TEXTAREA_LINE_MAX  128
#define TEXTAREA_CAJAS     4
#define MAX_WM_WINDOWS MAX_WINDOWS // la de wm.h: una sola definicion

#define ROW_H 18
#define MENUBAR_H 16
#define MENU_ROW_H 16

typedef struct {
    bool used;
    uint8_t type;
    int32_t window_idx;
    int32_t x, y;
    uint32_t w, h;
    char text[GADGET_TEXT_MAX];
    bool visible;
    bool enabled;
    bool focused; // TextField con el foco de teclado
    // El grupo del control (SetGadgetGroup / GadgetGroup). 0 = ninguno.
    //
    // Para las OPCIONES (style 3) decide quien apaga a quien: solo compiten
    // entre ellas las del mismo grupo, y las que no tienen grupo compiten
    // entre las que tampoco lo tienen. Ver apagar_las_demas_del_grupo.
    //
    // Para los demas controles es informativo: no afecta al dibujo ni al
    // anidado (seguimos con una sola ventana por programa).
    int32_t parent_group;

    // ListBox
    char items[LISTBOX_MAX_ITEMS][GADGET_TEXT_MAX];
    uint32_t item_count;
    int32_t selected;

    // TextArea
    int32_t ta_slot;        // hueco en el almacen de cajas de texto (-1: ninguno)
    uint32_t ta_cur_line, ta_cur_col, ta_scroll;   // donde escribe el usuario
    uint32_t ta_line_count;

    // Menu / MenuRoot
    int32_t parent_menu; // -1 si no aplica
    int32_t tag;
    bool checked;

    // Button: estilo (1=normal, 2=casilla, 3=radio, 4/5=clic
    // automatico con Return/Escape -- estos dos ultimos se aceptan
    // pero no se implementan de verdad, ver nota en gadget_create_button)
    uint8_t style;

    // ProgressBar: valor "por mil" (0-1000 = 0.0-1.0), UpdateProgBar
    // -- entero, no double: el kernel no puede usar coma flotante de
    // verdad (-mgeneral-regs-only -- ver la nota grande junto a
    // sound_volume_permil en syscall.c). La conversion desde el
    // double que escribe el programa BlitzPlus se hace en el
    // COMPILADOR, que si tiene coma flotante real (genera ensamblado
    // de usuario, sin esta restriccion).
    int32_t progress_permil;

    // Slider: 'visible' items vistos a traves de la "ventana"
    // deslizante, 'total' items en total, 'value' posicion actual
    // (0..total-visible). Reutiliza 'style' para la orientacion
    // (1=horizontal por defecto, 2=vertical).
    int32_t slider_visible, slider_total, slider_value;

    // ToolBar / SetGadgetIconStrip: 'icon_strip' es el handle de
    // imagen (LoadIconStrip = LoadImage con otro nombre) que se
    // recorta en botones cuadrados iguales. Para ToolBar, tambien
    // reutilizamos 'item_count' (numero de botones) e 'items_enabled'
    // (estado individual de cada boton -- ghosted si esta a false).
    // 'text' se reutiliza para guardar el texto crudo de
    // SetToolBarTips (solo se guarda, no se renderiza como popup
    // todavia -- ver nota junto a la implementacion).
    int32_t icon_strip;
    bool items_enabled[LISTBOX_MAX_ITEMS];
    // SetPanelImage tambien reutiliza 'icon_strip' (como handle de
    // imagen, en vez de color empaquetado) -- este booleano distingue
    // cual de los dos significados tiene ahora mismo, ya que ambos
    // rangos de valores posibles se solapan.
    bool panel_uses_image;

    // TreeView: cada NODO es su propio gadget (GADGET_TREENODE), no
    // un indice como ListBox. Reutiliza 'parent_menu' como "nodo
    // padre" (-1 = es la raiz de su TreeView) y 'checked' como
    // "expandido". 'tree_owner' es el gadget TreeView contenedor al
    // que pertenece en ultima instancia -- hace falta porque
    // SelectTreeViewNode(node) solo recibe el NODO, no el TreeView,
    // asi que necesitamos saber a cual avisar. En el propio
    // GADGET_TREEVIEW, reutiliza 'selected' para guardar el ID del
    // nodo seleccionado (no un indice, a diferencia de ListBox).
    int32_t tree_owner;
} gadget_t;

static gadget_t gadgets[MAX_GADGETS];
static char ta_pool[TEXTAREA_CAJAS][TEXTAREA_MAX_LINES][TEXTAREA_LINE_MAX];
static bool ta_pool_usado[TEXTAREA_CAJAS];
// Las lineas de una caja de texto (NULL si se quedo sin hueco en el almacen)
static char (*ta_lineas(gadget_t *g))[TEXTAREA_LINE_MAX] {
    return (g->ta_slot >= 0) ? ta_pool[g->ta_slot] : NULL;
}

// -- Cola de eventos, de verdad (antes: un solo hueco por ventana,
// compartido entre PollEvent/WaitEvent/GadgetEvent -- si uno lo
// consumia, los demas lo encontraban vacio; y sin ningun campo x/y,
// H_EVENTXY de la auditoria del compilador viejo). Memoria del propio
// kernel (kmalloc/kfree, ver heap.h) -- esto es estado del gestor de
// eventos que ve el kernel entero, no memoria privada de una tarea,
// asi que el heap del kernel es el sitio correcto (a diferencia de
// nb_alloc.c, que es memoria privada DENTRO de cada tarea para sus
// propias cadenas/instancias de Type).
typedef struct nb_event_node {
    int32_t id, source, data, x, y;
    struct nb_event_node *next;
} nb_event_node_t;
static nb_event_node_t *event_head[MAX_WM_WINDOWS];
static nb_event_node_t *event_tail[MAX_WM_WINDOWS];
// Del ULTIMO evento CONSUMIDO de verdad (para EventID/EventSource/
// EventData/EventX/EventY) -- PeekEvent() no los toca, tal como
// documenta BlitzPlus real.
static int32_t last_event_id[MAX_WM_WINDOWS];
static int32_t last_event_source[MAX_WM_WINDOWS];
static int32_t last_event_data[MAX_WM_WINDOWS];
static int32_t last_event_x[MAX_WM_WINDOWS];
static int32_t last_event_y[MAX_WM_WINDOWS];
static int32_t open_menu[MAX_WM_WINDOWS]; // id de la entrada de menu desplegada, -1 si ninguna
// Submenus: la entrada del desplegable cuyo submenu esta abierto
// (-1 si ninguno), y donde se pinto por ultima vez, para borrarlo
static int32_t open_sub[MAX_WM_WINDOWS];
static int32_t last_sub_x[MAX_WM_WINDOWS], last_sub_y[MAX_WM_WINDOWS], last_sub_w[MAX_WM_WINDOWS], last_sub_h[MAX_WM_WINDOWS];
static int32_t open_combobox[MAX_WM_WINDOWS]; // id del ComboBox con la lista desplegada, -1 si ninguno

// -- CreateTimer: un temporizador activo por ventana --
static bool timer_active[MAX_WM_WINDOWS];
static uint32_t timer_interval[MAX_WM_WINDOWS]; // en ticks (100Hz)
static uint64_t timer_next[MAX_WM_WINDOWS];
// PauseTimer/ResumeTimer/ResetTimer/TimerTicks -- 'paused' congela el
// disparo (sin generar eventos ni avanzar el contador) hasta
// ResumeTimer; 'tick_count' es el contador que TimerTicks() lee y
// ResetTimer() pone a cero (independiente de timer_next, que solo
// controla CUANDO toca el siguiente disparo).
static bool timer_paused[MAX_WM_WINDOWS];
static uint32_t timer_tick_count[MAX_WM_WINDOWS];
static int32_t gadget_count = 0; // cuantos gadgets usados hay en total -- ver gadgets_update_and_draw
static int32_t window_menu_root[MAX_WM_WINDOWS];
static bool window_menu_root_set[MAX_WM_WINDOWS];

// Texto de ayuda de la barra de herramientas: el raton medio
// segundo quieto sobre un boton lo muestra; al moverse, se borra.
static int32_t tip_gadget = -1, tip_boton = -1, tip_win = -1;
static uint64_t tip_desde = 0;
static bool tip_visible = false;
static int32_t tip_x, tip_y; static uint32_t tip_w, tip_h;
#define TIP_ESPERA 50        // latidos de 10 ms

// Recordamos donde se dibujo el ULTIMO desplegable de cada ventana,
// para poder borrarlo la proxima vez que cambie o se cierre -- si no,
// sus pixeles se quedan "pegados" en pantalla porque nadie los vuelve
// a tocar (0 ancho = no habia ninguno la ultima vez).
static int32_t last_dd_x[MAX_WM_WINDOWS], last_dd_y[MAX_WM_WINDOWS];
static int32_t last_dd_w[MAX_WM_WINDOWS], last_dd_h[MAX_WM_WINDOWS];

// Lo que habia DEBAJO del desplegable y del submenu.
//
// Antes, al cerrarlos, su hueco se rellenaba con el gris de fondo. En una
// ventana que no pinta nada propio eso no se nota; en un editor o un juego
// deja un agujero gris hasta que el programa repinta -- y un programa bien
// hecho solo repinta cuando algo cambia, asi que el agujero se queda.
//
// Ahora se guardan los pixeles antes de tapar y se devuelven al destapar.
// La memoria se pide del monton solo cuando hace falta y se reaprovecha
// mientras el tamaño no crezca: abrir un menu no puede fallar por falta de
// memoria, asi que si no hay sitio se vuelve al relleno gris de siempre.
static uint8_t *fondo_dd[MAX_WM_WINDOWS];
static uint32_t fondo_dd_cap[MAX_WM_WINDOWS];
static uint8_t *fondo_sub[MAX_WM_WINDOWS];
static uint32_t fondo_sub_cap[MAX_WM_WINDOWS];

// ---- El menu flotante (CreateContextMenu / ShowContextMenu) ----
//
// Es el mismo desplegable de siempre, con dos diferencias: se abre donde
// diga el programa en vez de debajo de una entrada de la barra, y la
// ventana puede no tener barra ninguna.
//
// Esa segunda diferencia obliga a llevar su propio borrado. El del
// desplegable normal vive dentro de draw_menubar, que solo se ejecuta si la
// ventana tiene barra de menu: un menu flotante en una ventana sin barra se
// quedaria pegado en pantalla al cerrarse, y nadie volveria a tocar esos
// pixeles.
static int32_t open_context[MAX_WM_WINDOWS];  // id de la raiz flotante abierta, -1 si ninguna
static int32_t ctx_x[MAX_WM_WINDOWS], ctx_y[MAX_WM_WINDOWS];
static int32_t last_ctx_x[MAX_WM_WINDOWS], last_ctx_y[MAX_WM_WINDOWS];
static int32_t last_ctx_w[MAX_WM_WINDOWS], last_ctx_h[MAX_WM_WINDOWS];
static uint8_t *fondo_ctx[MAX_WM_WINDOWS];
static uint32_t fondo_ctx_cap[MAX_WM_WINDOWS];

static uint8_t *fondo_reservar(uint8_t **buf, uint32_t *cap, uint32_t bytes) {
    if (*buf && *cap >= bytes) return *buf;
    if (*buf) { kfree(*buf); *buf = 0; *cap = 0; }
    uint8_t *p = (uint8_t *)kmalloc(bytes);
    if (!p) return 0;
    *buf = p; *cap = bytes;
    return p;
}

// Lo mismo para la lista de un ComboBox: se pinta DENTRO del
// dibujo de la ventana, por debajo de su caja, y al cerrarse nadie la
// borraba -- se quedaba pintada como si siguiera abierta. Por ventana: que
// ComboBox la tenia abierta y donde (-1 = ninguno).
static int32_t combo_dd_id[MAX_WM_WINDOWS];
static int32_t combo_dd_x[MAX_WM_WINDOWS], combo_dd_y[MAX_WM_WINDOWS];
static uint32_t combo_dd_w[MAX_WM_WINDOWS], combo_dd_h[MAX_WM_WINDOWS];

// Color de texto legible sobre el fondo que hay en (x, y) del dibujo de la
// ventana: oscuro sobre claro, claro sobre oscuro. Las casillas
// y las opciones escriben su texto directamente sobre el fondo, y con un
// gris muy claro fijo no se leian sobre el gris claro de las ventanas.
static uint32_t texto_legible(int32_t win, int32_t x, int32_t y, bool activo) {
    uint32_t c = (x >= 0 && y >= 0) ? wm_content_get_pixel(win, (uint32_t)x, (uint32_t)y) : 0;
    uint32_t r = (c >> 16) & 0xFF, gr = (c >> 8) & 0xFF, b = c & 0xFF;
    uint32_t luz = (r * 299 + gr * 587 + b * 114) / 1000;
    if (!activo) return 0x00808080;
    return (luz > 128) ? 0x00202028 : 0x00E0E0E0;
}

static uint32_t str_len(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

// ---- Menus desplegados ----
// El ancho del desplegable sale del texto mas largo (antes, 140 fijos: un
// texto largo se salia); una entrada SIN texto es una linea de separacion
// (como en BlitzPlus: antes era una fila vacia que ademas se podia pulsar);
// y la marca de CheckMenu va en su propia columna, a la izquierda (antes,
// un "* " delante que movia el texto). Dibujo y clic usan las mismas
// funciones, para no desfasarse.
#define MENU_MARCA_W 20
static int32_t menu_ancho_desplegable(int32_t open);   // (usa 'gadgets', definido mas abajo)
static bool menu_tiene_hijos(int32_t id);
static void menu_geo(int32_t root, int32_t open, int32_t *x, int32_t *y, int32_t *w, int32_t *h);
static bool menu_geo_sub(int32_t win, int32_t root, int32_t *x, int32_t *y, int32_t *w, int32_t *h);
static int32_t menu_entrada_en(int32_t padre, int32_t fila);
static int32_t menu_contar(int32_t padre);
static int32_t menu_ancho_desplegable(int32_t open);
static void dibujar_filas_menu(int32_t window_idx, int32_t padre, int32_t x, int32_t y, int32_t dw);
static void dibujar_marca(int32_t win, int32_t x, int32_t y, uint32_t color);
static bool menu_es_separador(const char *texto) { return texto[0] == '\0'; }

static void set_text(char *dst, const char *src) {
    uint32_t i = 0;
    while (src[i] && i < GADGET_TEXT_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void gadgets_init(void) {
    for (int i = 0; i < MAX_GADGETS; i++) gadgets[i].used = false;
    for (int i = 0; i < MAX_WM_WINDOWS; i++) {
        event_head[i] = NULL;
        event_tail[i] = NULL;
        last_event_id[i] = 0;
        last_event_source[i] = 0;
        last_event_data[i] = 0;
        last_event_x[i] = 0;
        last_event_y[i] = 0;
        open_menu[i] = -1;
        open_sub[i] = -1; last_sub_w[i] = 0;
        open_combobox[i] = -1;
        combo_dd_id[i] = -1;
        open_context[i] = -1; last_ctx_w[i] = 0;
        window_menu_root_set[i] = false;
        last_dd_w[i] = 0;
        timer_active[i] = false;
    }
    gadget_count = 0;
}

static int32_t alloc_gadget(void) {
    for (int i = 1; i < MAX_GADGETS; i++) { // 0 se reserva como "sin gadget"
        if (!gadgets[i].used) return i;
    }
    return -1;
}

static int32_t create_common(uint8_t type, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx) {
    int32_t id = alloc_gadget();
    if (id < 0) return -1;
    gadget_t *g = &gadgets[id];
    g->used = true;
    g->type = type;
    g->window_idx = window_idx;
    g->x = x; g->y = y; g->w = w; g->h = h;
    g->text[0] = '\0';
    g->visible = true;
    g->enabled = true;
    g->focused = false;
    g->item_count = 0;
    g->selected = -1;
    g->ta_line_count = 0;
    g->parent_menu = -1;
    g->tag = 0;
    g->checked = false;
    g->style = 1; // push normal por defecto
    g->progress_permil = 0;
    g->slider_visible = 1;
    g->slider_total = 10; // valores por defecto razonables hasta que se llame a SetSliderRange
    g->slider_value = 0;
    g->icon_strip = -1;
    g->panel_uses_image = false;
    g->parent_group = 0;
    for (int k = 0; k < LISTBOX_MAX_ITEMS; k++) g->items_enabled[k] = true;
    g->tree_owner = -1;
    gadget_count++;
    return id;
}

int32_t gadget_create_button(const char *text, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx, uint32_t style) {
    int32_t id = create_common(GADGET_BUTTON, x, y, w, h, window_idx);
    if (id >= 0) {
        set_text(gadgets[id].text, text);
        // style 4/5 (clic automatico con Return/Escape) se acepta
        // pero se trata como un boton normal -- no tenemos el
        // concepto de "boton por defecto activado por Enter" en
        // nuestros campos de texto todavia.
        gadgets[id].style = (style >= 1 && style <= 5) ? (uint8_t)style : 1;
    }
    return id;
}
int32_t gadget_create_panel(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx) {
    return create_common(GADGET_PANEL, x, y, w, h, window_idx);
}
int32_t gadget_create_textfield(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx) {
    return create_common(GADGET_TEXTFIELD, x, y, w, h, window_idx);
}
int32_t gadget_create_listbox(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx) {
    return create_common(GADGET_LISTBOX, x, y, w, h, window_idx);
}
int32_t gadget_create_textarea(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx) {
    int32_t id = create_common(GADGET_TEXTAREA, x, y, w, h, window_idx);
    if (id > 0) {
        gadgets[id].ta_slot = -1;
        for (int k = 0; k < TEXTAREA_CAJAS; k++) {
            if (!ta_pool_usado[k]) {
                ta_pool_usado[k] = true; gadgets[id].ta_slot = k;
                for (uint32_t i = 0; i < TEXTAREA_MAX_LINES; i++) ta_pool[k][i][0] = '\0';
                break;
            }
        }
        gadgets[id].ta_cur_line = 0; gadgets[id].ta_cur_col = 0; gadgets[id].ta_scroll = 0;
        gadgets[id].ta_line_count = 1;
    }
    return id;
}
int32_t gadget_create_label(const char *text, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx, uint32_t style) {
    int32_t id = create_common(GADGET_LABEL, x, y, w, h, window_idx);
    if (id >= 0) {
        set_text(gadgets[id].text, text);
        // 0=sin borde (por defecto), 1=borde plano, 2=sin borde
        // (documentado como "?" en la propia referencia oficial --
        // tratado igual que 0), 3=borde 3D hundido.
        gadgets[id].style = (style <= 3) ? (uint8_t)style : 0;
    }
    return id;
}
int32_t gadget_create_progbar(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx) {
    // 'style' de BlitzPlus real se documenta como "Not supported" --
    // ni lo aceptamos como parametro aqui.
    return create_common(GADGET_PROGBAR, x, y, w, h, window_idx);
}

static bool valid(int32_t id) { return id > 0 && id < MAX_GADGETS && gadgets[id].used; }
// ListBox, ComboBox y Tabber comparten el mismo almacenamiento de
// items (items[], item_count, selected) -- confirmado contra la
// documentacion oficial: InsertGadgetItem y companeros funcionan
// igual con los tres ("This command may only be used with combobox,
// listbox and tabber gadgets").
static bool is_item_gadget(uint8_t type) { return type == GADGET_LISTBOX || type == GADGET_COMBOBOX || type == GADGET_TABBER; }

void gadget_update_progbar(int32_t id, int32_t value_permil) {
    if (!valid(id) || gadgets[id].type != GADGET_PROGBAR) return;
    if (value_permil < 0) value_permil = 0;
    if (value_permil > 1000) value_permil = 1000;
    gadgets[id].progress_permil = value_permil;
}

int32_t gadget_create_slider(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx, uint32_t style) {
    int32_t id = create_common(GADGET_SLIDER, x, y, w, h, window_idx);
    if (id >= 0) {
        // 1=horizontal (por defecto), 2=vertical
        gadgets[id].style = (style == 2) ? 2 : 1;
    }
    return id;
}
// La barra de desplazamiento comparte con el deslizador el recorrido, el
// pomo y el valor: son la misma cuenta. Lo que añade son las flechas de los
// extremos y el salto de pagina al pulsar el carril, que es justo lo que
// hace que se pueda recorrer un texto largo sin arrastrar a ojo.
//
// Comparten tambien los comandos: SetSliderRange, SetSliderValue y
// SliderValue valen para las dos. Duplicarlos con otro nombre seria pedirle
// al programador que recuerde cual va con cual, para hacer lo mismo.
int32_t gadget_create_scrollbar(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx, uint32_t style) {
    int32_t id = create_common(GADGET_SCROLLBAR, x, y, w, h, window_idx);
    if (id >= 0) {
        gadgets[id].style = (style == 2) ? 2 : 1;   // 1=horizontal, 2=vertical
        gadgets[id].slider_visible = 1;
        gadgets[id].slider_total = 1;
    }
    return id;
}

// Los dos tipos que tienen recorrido y pomo.
static bool es_deslizante(int32_t id) {
    return valid(id) && (gadgets[id].type == GADGET_SLIDER || gadgets[id].type == GADGET_SCROLLBAR);
}

// El lado de las flechas de una barra: cuadradas, del grueso de la barra, y
// nunca mas de un tercio de su largo -- en una barra corta, dos flechas
// enteras no dejarian carril donde pulsar.
static int32_t flecha_lado(gadget_t *g) {
    if (g->type != GADGET_SCROLLBAR) return 0;
    int32_t grueso = (g->style == 2) ? (int32_t)g->w : (int32_t)g->h;
    int32_t largo  = (g->style == 2) ? (int32_t)g->h : (int32_t)g->w;
    int32_t lado = grueso;
    if (lado > largo / 3) lado = largo / 3;
    if (lado < 0) lado = 0;
    return lado;
}

void gadget_set_slider_range(int32_t id, int32_t visible, int32_t total) {
    if (!es_deslizante(id)) return;
    if (visible < 1) visible = 1;
    if (total < visible) total = visible;
    gadgets[id].slider_visible = visible;
    gadgets[id].slider_total = total;
    int32_t maxval = total - visible;
    if (gadgets[id].slider_value > maxval) gadgets[id].slider_value = maxval;
}
void gadget_set_slider_value(int32_t id, int32_t value) {
    if (!es_deslizante(id)) return;
    int32_t maxval = gadgets[id].slider_total - gadgets[id].slider_visible;
    if (value < 0) value = 0;
    if (value > maxval) value = maxval;
    gadgets[id].slider_value = value;
}
int32_t gadget_slider_value(int32_t id) {
    if (!es_deslizante(id)) return 0;
    return gadgets[id].slider_value;
}
int32_t gadget_create_combobox(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx) {
    return create_common(GADGET_COMBOBOX, x, y, w, h, window_idx);
}
int32_t gadget_create_tabber(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx) {
    // 'style' de BlitzPlus real se documenta como "Not supported" --
    // ni lo aceptamos como parametro aqui.
    return create_common(GADGET_TABBER, x, y, w, h, window_idx);
}
// CreateToolBar -- 'image_handle' ya viene cargado (LoadIconStrip =
// LoadImage con otro nombre, ver la nota junto a image_get_info en
// syscall.c). El numero de botones se calcula dividiendo el ancho
// entre el alto (iconos cuadrados, empaquetados en horizontal, igual
// que documenta BlitzPlus real). w=0,h=0 (como en el ejemplo oficial:
// "CreateToolBar(BMP$,0,0,0,0,WinHandle)") significa "usar el tamaño
// natural de la imagen completa".
int32_t gadget_create_toolbar(int32_t image_handle, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx) {
    uint32_t iw, ih;
    if (!image_get_info(image_handle, &iw, &ih, NULL) || ih == 0) return -1;
    if (w == 0) w = iw;
    if (h == 0) h = ih;
    int32_t id = create_common(GADGET_TOOLBAR, x, y, w, h, window_idx);
    if (id >= 0) {
        gadgets[id].icon_strip = image_handle;
        gadgets[id].item_count = iw / ih;
    }
    return id;
}
void gadget_set_icon_strip(int32_t id, int32_t strip_handle) {
    if (!valid(id)) return;
    gadgets[id].icon_strip = strip_handle;
}
// SetPanelColor -- reutiliza el mismo campo 'icon_strip' que Panel
// nunca usa para nada mas, empaquetado como 0xRRGGBB.
void gadget_set_panel_color(int32_t id, uint32_t rgb) {
    if (!valid(id) || gadgets[id].type != GADGET_PANEL) return;
    gadgets[id].icon_strip = (int32_t)(rgb & 0xFFFFFF);
    gadgets[id].panel_uses_image = false;
}
// SetPanelImage -- reutiliza 'icon_strip' como handle de imagen (en
// vez de color empaquetado), marcado con 'panel_uses_image' para
// distinguirlo. La imagen se dibuja en mosaico (repetida) para llenar
// toda el area del panel, tal como documenta BlitzPlus real.
void gadget_set_panel_image(int32_t id, int32_t image_handle) {
    if (!valid(id) || gadgets[id].type != GADGET_PANEL) return;
    gadgets[id].icon_strip = image_handle;
    gadgets[id].panel_uses_image = true;
}
// SetGadgetGroup / GadgetGroup -- ver la nota junto al campo
// 'parent_group' en la definicion de gadget_t. En una OPCION decide con
// quien compite; en los demas controles es informativo.
void gadget_set_group(int32_t id, int32_t group) {
    if (!valid(id)) return;
    gadgets[id].parent_group = group;
}
int32_t gadget_get_group(int32_t id) {
    if (!valid(id)) return 0;
    return gadgets[id].parent_group;
}
void gadget_enable_toolbar_item(int32_t id, int32_t index, bool enabled) {
    if (!valid(id) || gadgets[id].type != GADGET_TOOLBAR) return;
    if (index < 0 || (uint32_t)index >= gadgets[id].item_count || (uint32_t)index >= LISTBOX_MAX_ITEMS) return;
    gadgets[id].items_enabled[index] = enabled;
}
// SetToolBarTips -- LIMITACION DOCUMENTADA: se guarda el texto crudo
// (separado por comas, tal como lo da el programa) pero no se
// renderiza como popup emergente al pasar el raton -- eso exigiria
// deteccion de "raton quieto encima X tiempo" + una ventana emergente
// nueva, infraestructura que no tenemos todavia.
void gadget_set_toolbar_tips(int32_t id, const char *tips) {
    // Uno por boton, en su propia linea (items[i]): antes iban
    // juntos en el texto del control, de 48 caracteres, y los de una barra
    // de diez botones se perdian a partir del quinto o el sexto.
    if (!valid(id) || gadgets[id].type != GADGET_TOOLBAR) return;
    gadget_t *g = &gadgets[id];
    uint32_t k = 0, col = 0;
    for (uint32_t i = 0; k < LISTBOX_MAX_ITEMS; i++) {
        char c = tips[i];
        if (c == ',' || c == '\0') {
            g->items[k][col] = '\0'; k++; col = 0;
            if (c == '\0') break;
        } else if (col < GADGET_TEXT_MAX - 1) {
            g->items[k][col++] = c;
        }
    }
    for (; k < LISTBOX_MAX_ITEMS; k++) g->items[k][0] = '\0';
}

int32_t gadget_create_treeview(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx) {
    int32_t id = create_common(GADGET_TREEVIEW, x, y, w, h, window_idx);
    if (id >= 0) gadgets[id].selected = -1; // ningun nodo seleccionado al principio
    return id;
}
// TreeViewRoot -- crea (la primera vez) o devuelve (las siguientes)
// el nodo raiz de un TreeView. La raiz misma nunca se dibuja: solo
// sirve como "padre" del primer nivel de nodos visibles.
int32_t gadget_treeview_root(int32_t treeview) {
    if (!valid(treeview) || gadgets[treeview].type != GADGET_TREEVIEW) return -1;
    for (int i = 1; i < MAX_GADGETS; i++) {
        if (gadgets[i].used && gadgets[i].type == GADGET_TREENODE &&
            gadgets[i].tree_owner == treeview && gadgets[i].parent_menu == -1) {
            return i;
        }
    }
    int32_t id = create_common(GADGET_TREENODE, 0, 0, 0, 0, gadgets[treeview].window_idx);
    if (id >= 0) {
        gadgets[id].tree_owner = treeview;
        gadgets[id].parent_menu = -1; // marca de "soy la raiz"
        gadgets[id].checked = true; // "expandida" -- irrelevante para dibujar (la raiz no se dibuja), pero coherente
    }
    return id;
}
int32_t gadget_add_treeview_node(const char *text, int32_t parent) {
    if (!valid(parent) || gadgets[parent].type != GADGET_TREENODE) return -1;
    int32_t id = create_common(GADGET_TREENODE, 0, 0, 0, 0, gadgets[parent].window_idx);
    if (id >= 0) {
        set_text(gadgets[id].text, text);
        gadgets[id].parent_menu = parent;
        gadgets[id].tree_owner = gadgets[parent].tree_owner;
        gadgets[id].checked = false; // colapsado por defecto
    }
    return id;
}
// InsertTreeViewNode -- LIMITACION DOCUMENTADA: se trata igual que
// AddTreeViewNode, ignorando 'index' (siempre se añade al final, en
// el orden de creacion) -- mantener un orden explicito de hermanos
// pediria una lista enlazada aparte por cada nodo padre.
int32_t gadget_insert_treeview_node(int32_t index, const char *text, int32_t parent) {
    (void)index;
    return gadget_add_treeview_node(text, parent);
}
void gadget_modify_treeview_node(int32_t node, const char *text) {
    if (!valid(node) || gadgets[node].type != GADGET_TREENODE) return;
    set_text(gadgets[node].text, text);
}
// FreeTreeViewNode -- libera el nodo Y TODOS sus descendientes
// (recursivo), para no dejar nodos huerfanos sueltos por ahi.
void gadget_free_treeview_node(int32_t node) {
    if (!valid(node) || gadgets[node].type != GADGET_TREENODE) return;
    for (int i = 1; i < MAX_GADGETS; i++) {
        if (gadgets[i].used && gadgets[i].type == GADGET_TREENODE && gadgets[i].parent_menu == node) {
            gadget_free_treeview_node(i);
        }
    }
    gadget_free(node);
}
void gadget_expand_treeview_node(int32_t node, bool expand) {
    if (!valid(node) || gadgets[node].type != GADGET_TREENODE) return;
    gadgets[node].checked = expand;
}
int32_t gadget_count_treeview_nodes(int32_t parent) {
    if (!valid(parent) || gadgets[parent].type != GADGET_TREENODE) return 0;
    int32_t count = 0;
    for (int i = 1; i < MAX_GADGETS; i++) {
        if (gadgets[i].used && gadgets[i].type == GADGET_TREENODE && gadgets[i].parent_menu == parent) count++;
    }
    return count;
}
int32_t gadget_selected_treeview_node(int32_t treeview) {
    if (!valid(treeview) || gadgets[treeview].type != GADGET_TREEVIEW) return -1;
    return gadgets[treeview].selected;
}
void gadget_select_treeview_node(int32_t node) {
    if (!valid(node) || gadgets[node].type != GADGET_TREENODE) return;
    int32_t owner = gadgets[node].tree_owner;
    if (valid(owner)) gadgets[owner].selected = node;
}
// CreateCanvas -- se dibuja como un rectangulo simple (igual que
// Panel); el dibujo DE VERDAD dentro del canvas se consigue
// redirigiendo Origin+Viewport a su rectangulo via CanvasBuffer +
// SetBuffer (ver la nota junto a CANVAS_BUFFER_OFFSET en syscall.c),
// no con un buffer de pixeles propio.
int32_t gadget_create_canvas(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx) {
    return create_common(GADGET_CANVAS, x, y, w, h, window_idx);
}
bool gadget_is_canvas(int32_t id) {
    return valid(id) && gadgets[id].type == GADGET_CANVAS;
}

void gadget_free(int32_t id) {
    if (valid(id) && gadgets[id].type == GADGET_TEXTAREA && gadgets[id].ta_slot >= 0) {
        ta_pool_usado[gadgets[id].ta_slot] = false;      // el hueco del almacen, libre
        gadgets[id].ta_slot = -1;
    }
    if (!valid(id)) return;
    gadgets[id].used = false;
    gadget_count--;
}

// Libera TODOS los gadgets de una ventana de golpe -- hay que
// llamarla cuando la ventana se destruye, o los gadgets del programa
// anterior se quedarian "fantasma": vivos para siempre, y si esa
// misma ventana se reutiliza para un programa nuevo, sus gadgets se
// mezclarian con los viejos (duplicados, eventos que no coinciden).
void gadgets_free_window(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return;

    for (int i = 1; i < MAX_GADGETS; i++) {
        if (gadgets[i].used && gadgets[i].window_idx == window_idx) {
            gadgets[i].used = false;
            gadget_count--;
        }
    }

    window_menu_root_set[window_idx] = false;
    open_menu[window_idx] = -1;
    open_sub[window_idx] = -1; last_sub_w[window_idx] = 0;
    open_combobox[window_idx] = -1;
    combo_dd_id[window_idx] = -1;
    open_context[window_idx] = -1; last_ctx_w[window_idx] = 0;
    gadgets_flush_events(window_idx, 0); // vacia la cola de verdad, liberando cada nodo
    last_event_id[window_idx] = 0;
    last_event_source[window_idx] = 0;
    last_event_data[window_idx] = 0;
    last_event_x[window_idx] = 0;
    last_event_y[window_idx] = 0;
    last_dd_w[window_idx] = 0;
    timer_active[window_idx] = false;
}

void gadget_set_text(int32_t id, const char *text) {
    if (!valid(id)) return;
    set_text(gadgets[id].text, text);
}

uint32_t gadget_get_text(int32_t id, char *out, uint32_t max_len) {
    if (!valid(id) || max_len == 0) { if (max_len) out[0] = '\0'; return 0; }
    // De una caja de texto, TODO su contenido: antes devolvia una
    // cadena vacia y guardar lo escrito en ella no guardaba nada.
    if (gadgets[id].type == GADGET_TEXTAREA) {
        gadget_textarea_get_text(id, 0, -1, out, max_len);
        uint32_t n = 0; while (out[n]) n++;
        return n;
    }
    uint32_t i = 0;
    while (gadgets[id].text[i] && i < max_len - 1) { out[i] = gadgets[id].text[i]; i++; }
    out[i] = '\0';
    return i;
}

bool gadget_get_rect(int32_t id, int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
    if (!valid(id)) return false;
    *x = gadgets[id].x; *y = gadgets[id].y; *w = gadgets[id].w; *h = gadgets[id].h;
    return true;
}
// Al mover o redimensionar un control hay que BORRAR donde
// estaba: los controles se pintan dentro del dibujo de la ventana, y su imagen
// anterior se quedaba como un fantasma. Antes lo tenia que hacer cada programa
// (nemo_gui.mover_gadget lo hacia a mano); ahora lo hace el kernel, y vale
// para Nemo Basic, Lua y Aronnax.
static void borrar_donde_estaba(int32_t id) {
    gadget_t *g = &gadgets[id];
    if (g->window_idx < 0 || g->w == 0 || g->h == 0) return;
    int32_t off = (int32_t)gadgets_menubar_height(g->window_idx);
    wm_content_fill_rect(g->window_idx, (uint32_t)g->x, (uint32_t)(g->y + off), g->w, g->h, 0x00D4D0C8);
}
void gadget_move(int32_t id, int32_t x, int32_t y) {
    if (!valid(id)) return;
    if (gadgets[id].x != x || gadgets[id].y != y) borrar_donde_estaba(id);
    gadgets[id].x = x; gadgets[id].y = y;
}
void gadget_resize(int32_t id, uint32_t w, uint32_t h) {
    if (!valid(id)) return;
    if (gadgets[id].w != w || gadgets[id].h != h) borrar_donde_estaba(id);
    gadgets[id].w = w; gadgets[id].h = h;
}
void gadget_show(int32_t id, bool visible) { if (valid(id)) gadgets[id].visible = visible; }
void gadget_enable(int32_t id, bool enabled) { if (valid(id)) gadgets[id].enabled = enabled; }

void gadget_activate(int32_t id) {
    if (!valid(id) || gadgets[id].type != GADGET_TEXTFIELD) return;
    // Solo un TextField tiene el foco a la vez, por ventana
    for (int i = 1; i < MAX_GADGETS; i++) {
        if (gadgets[i].used && gadgets[i].type == GADGET_TEXTFIELD && gadgets[i].window_idx == gadgets[id].window_idx) {
            gadgets[i].focused = false;
        }
    }
    gadgets[id].focused = true;
}

// -- Eventos "en crudo" (WaitEvent/EventID/EventSource/EventData) --

void gadgets_fire_raw_event(int32_t window_idx, int32_t event_id, int32_t source, int32_t data, int32_t x, int32_t y) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return;
    nb_event_node_t *node = (nb_event_node_t *)kmalloc(sizeof(nb_event_node_t));
    if (!node) return; // memoria del kernel agotada de verdad -- se pierde este evento,
                        // no hay nada mejor que hacer; no debería pasar en la práctica
                        // (cada nodo es pequeño y se libera al consumirse).
    node->id = event_id; node->source = source; node->data = data;
    node->x = x; node->y = y; node->next = NULL;
    if (event_tail[window_idx]) event_tail[window_idx]->next = node;
    else event_head[window_idx] = node;
    event_tail[window_idx] = node;
}

int32_t gadgets_poll_raw_event(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return 0;
    nb_event_node_t *node = event_head[window_idx];
    if (!node) return 0;
    event_head[window_idx] = node->next;
    if (!event_head[window_idx]) event_tail[window_idx] = NULL;
    last_event_id[window_idx] = node->id;
    last_event_source[window_idx] = node->source;
    last_event_data[window_idx] = node->data;
    last_event_x[window_idx] = node->x;
    last_event_y[window_idx] = node->y;
    int32_t id = node->id;
    kfree(node);
    return id;
}

// PeekEvent(): igual que gadgets_poll_raw_event, pero SIN consumirlo
// -- no lo saca de la cola, no actualiza last_event_* (así EventID/
// EventData/etc. siguen dando lo del ÚLTIMO evento consumido de
// verdad, tal como documenta BlitzPlus real: "PeekEvent does not
// update the other event functions").
int32_t gadgets_peek_raw_event(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return 0;
    return event_head[window_idx] ? event_head[window_idx]->id : 0;
}

// FlushEvents([id]): con id=0 (omitido) descarta TODA la cola; con un
// id concreto, solo los eventos que coincidan (a diferencia del
// modelo de un solo hueco de antes, ahora sí hay varios que
// recorrer). Libera cada nodo descartado.
void gadgets_flush_events(int32_t window_idx, int32_t filter_id) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return;
    if (filter_id == 0) {
        nb_event_node_t *node = event_head[window_idx];
        while (node) { nb_event_node_t *next = node->next; kfree(node); node = next; }
        event_head[window_idx] = NULL;
        event_tail[window_idx] = NULL;
        return;
    }
    nb_event_node_t *prev = NULL, *node = event_head[window_idx];
    while (node) {
        nb_event_node_t *next = node->next;
        if (node->id == filter_id) {
            if (prev) prev->next = next; else event_head[window_idx] = next;
            if (node == event_tail[window_idx]) event_tail[window_idx] = prev;
            kfree(node);
        } else {
            prev = node;
        }
        node = next;
    }
}

int32_t gadgets_get_last_event_source(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return 0;
    return last_event_source[window_idx];
}

int32_t gadgets_get_last_event_data(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return 0;
    return last_event_data[window_idx];
}

// Nuevas -- antes no existía ningún sitio donde guardar esto
// (H_EVENTXY de la auditoría). De momento ningún evento real lleva
// coordenadas propias (todas las llamadas a gadgets_fire_raw_event
// pasan x=0,y=0) -- la plomería ya existe; qué eventos deberían
// llevar una posición real (un clic sobre un gadget, por ejemplo) es
// una decisión de diseño aparte, para cuando se necesite de verdad.
int32_t gadgets_get_last_event_x(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return 0;
    return last_event_x[window_idx];
}

int32_t gadgets_get_last_event_y(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return 0;
    return last_event_y[window_idx];
}

// gadget_poll_event() es la version "simplificada" de antes (solo el
// id del gadget, sin distinguir tipo de evento) -- se queda tal cual
// para que GadgetEvent() (nuestra API de gadgets propia, anterior a
// esta ronda) siga funcionando exactamente igual que siempre.
int32_t gadget_poll_event(int32_t window_idx) {
    int32_t id = gadgets_poll_raw_event(window_idx);
    if (id == 0) return 0;
    return last_event_source[window_idx];
}

static void fire_event(int32_t window_idx, int32_t gadget_id) {
    gadgets_fire_raw_event(window_idx, EVENT_GADGETACTION, gadget_id, 0, 0, 0);
}
// Igual que fire_event, pero con un dato explicito para EventData()
// -- lo usa ToolBar, donde EventData() debe dar el INDICE del boton
// pulsado (confirmado en la documentacion oficial de CreateToolBar).
static void fire_event_data(int32_t window_idx, int32_t gadget_id, int32_t data) {
    gadgets_fire_raw_event(window_idx, EVENT_GADGETACTION, gadget_id, data, 0, 0);
}

static void fire_menu_event(int32_t window_idx, int32_t menu_id) {
    int32_t tag = valid(menu_id) ? gadgets[menu_id].tag : 0;
    gadgets_fire_raw_event(window_idx, EVENT_MENUACTION, menu_id, tag, 0, 0);
}

// -- CreateTimer --

int32_t gadget_create_timer(int32_t window_idx, uint32_t hertz) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return -1;
    if (hertz == 0) hertz = 1;
    uint32_t interval = 100 / hertz; // 100Hz -> ticks por disparo
    if (interval == 0) interval = 1;
    timer_active[window_idx] = true;
    timer_interval[window_idx] = interval;
    timer_next[window_idx] = timer_get_ticks() + interval;
    timer_paused[window_idx] = false;
    timer_tick_count[window_idx] = 0;
    return window_idx;
}

// FreeTimer(handle) -- el "handle" es el mismo indice de ventana que
// devolvio CreateTimer, asi que solo hace falta desactivarlo.
void gadget_free_timer(int32_t handle) {
    if (handle < 0 || handle >= MAX_WM_WINDOWS) return;
    timer_active[handle] = false;
}

// WaitTimer(handle) -- consulta pura (sin efectos secundarios): true
// si ya toca el siguiente disparo. El bucle de espera vive en el
// runtime del compilador (SYS_PUMP repetido), no aqui -- igual que
// WaitKey/WaitMouse, para no bloquear el kernel de verdad.
bool gadget_timer_ready(int32_t handle) {
    if (handle < 0 || handle >= MAX_WM_WINDOWS || !timer_active[handle]) return true; // si no hay timer activo, no bloqueamos
    return timer_get_ticks() >= timer_next[handle];
}

// Avanza el temporizador al siguiente disparo -- se llama UNA vez,
// justo despues de que gadget_timer_ready() haya dado true, para
// "consumir" este tick (misma logica que gadgets_check_timers, pero
// sin generar un evento de por medio).
void gadget_timer_consume(int32_t handle) {
    if (handle < 0 || handle >= MAX_WM_WINDOWS || !timer_active[handle]) return;
    timer_next[handle] = timer_get_ticks() + timer_interval[handle];
}

// Se llama una vez por vuelta desde task_yield -- independiente de si
// hay gadgets normales o no (gadgets_update_and_draw sale pronto si
// gadget_count es 0, y un programa que SOLO use CreateTimer no
// tendria ningun gadget de verdad).
void gadgets_check_timers(void) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < MAX_WM_WINDOWS; i++) {
        if (!timer_active[i] || timer_paused[i]) continue;
        if (now < timer_next[i]) continue;
        gadgets_fire_raw_event(i, EVENT_TIMERTICK, i, 0, 0, 0);
        timer_next[i] = now + timer_interval[i];
        timer_tick_count[i]++;
    }
}

// PauseTimer/ResumeTimer -- 'paused' congela el disparo sin perder el
// estado (interval/tick_count siguen intactos). ResumeTimer
// recalcula timer_next desde AHORA, para no generar una rafaga de
// disparos "atrasados" acumulados durante la pausa.
void gadget_pause_timer(int32_t handle) {
    if (handle < 0 || handle >= MAX_WM_WINDOWS) return;
    timer_paused[handle] = true;
}
void gadget_resume_timer(int32_t handle) {
    if (handle < 0 || handle >= MAX_WM_WINDOWS) return;
    timer_paused[handle] = false;
    timer_next[handle] = timer_get_ticks() + timer_interval[handle];
}
void gadget_reset_timer(int32_t handle) {
    if (handle < 0 || handle >= MAX_WM_WINDOWS) return;
    timer_tick_count[handle] = 0;
}
uint32_t gadget_timer_ticks(int32_t handle) {
    if (handle < 0 || handle >= MAX_WM_WINDOWS) return 0;
    return timer_tick_count[handle];
}

// ---- HotKeyEvent ----
//
// HotKeyEvent rawkey,modifier,event_id[,event_data,x,y,z,event_source]
// -- registra una tecla rapida global que dispara un evento
// "enlatado" cuando se pulsa. Solo guardamos id/data/source de verdad
// (no tenemos EventX/EventY/EventZ implementados todavia, asi que
// esos tres se aceptan por compatibilidad de firma pero se
// descartan -- limitacion documentada). El evento se dispara sobre
// la ventana ENFOCADA (v1: un programa, una ventana).
#define MAX_HOTKEYS 16
typedef struct {
    bool used;
    uint16_t rawkey;
    uint8_t modifier; // bit0=shift, bit1=ctrl, bit2=alt
    int32_t event_id, event_data, event_source;
    bool was_down; // para detectar el flanco de subida
} hotkey_t;
static hotkey_t hotkeys[MAX_HOTKEYS];

void gadget_hotkey_event(uint16_t rawkey, uint8_t modifier, int32_t event_id,
                          int32_t event_data, int32_t event_source) {
    for (int i = 0; i < MAX_HOTKEYS; i++) {
        if (hotkeys[i].used && hotkeys[i].rawkey == rawkey && hotkeys[i].modifier == modifier) {
            if (event_id == 0) { hotkeys[i].used = false; return; } // event_id=0 quita el hotkey
            hotkeys[i].event_id = event_id;
            hotkeys[i].event_data = event_data;
            hotkeys[i].event_source = event_source;
            return;
        }
    }
    if (event_id == 0) return; // nada que quitar
    for (int i = 0; i < MAX_HOTKEYS; i++) {
        if (!hotkeys[i].used) {
            hotkeys[i].used = true;
            hotkeys[i].rawkey = rawkey;
            hotkeys[i].modifier = modifier;
            hotkeys[i].event_id = event_id;
            hotkeys[i].event_data = event_data;
            hotkeys[i].event_source = event_source;
            hotkeys[i].was_down = false;
            return;
        }
    }
}

// Se llama una vez por vuelta, igual que gadgets_check_timers.
void gadgets_check_hotkeys(void) {
    for (int i = 0; i < MAX_HOTKEYS; i++) {
        if (!hotkeys[i].used) continue;
        bool mod_ok = true;
        if (hotkeys[i].modifier & 1) mod_ok = mod_ok && (key_is_down(42) || key_is_down(54));  // shift
        if (hotkeys[i].modifier & 2) mod_ok = mod_ok && (key_is_down(29) || key_is_down(97));  // ctrl
        if (hotkeys[i].modifier & 4) mod_ok = mod_ok && (key_is_down(56) || key_is_down(100)); // alt
        bool down = mod_ok && key_is_down(hotkeys[i].rawkey);
        if (down && !hotkeys[i].was_down) {
            int32_t win = wm_get_focused_window();
            if (win >= 0) gadgets_fire_raw_event(win, hotkeys[i].event_id, hotkeys[i].event_source, hotkeys[i].event_data, 0, 0);
        }
        hotkeys[i].was_down = down;
    }
}

// ---- ListBox ----

void gadget_listbox_add_item(int32_t id, const char *text) {
    if (!valid(id) || !is_item_gadget(gadgets[id].type)) return;
    gadget_t *g = &gadgets[id];
    if (g->item_count >= LISTBOX_MAX_ITEMS) return;
    set_text(g->items[g->item_count], text);
    g->item_count++;
}
// InsertGadgetItem -- a diferencia de add_item (que SIEMPRE añade al
// final), esta inserta en cualquier POSICION, desplazando los
// siguientes ítems un hueco hacia adelante. El parametro 'icon' de
// BlitzPlus real se acepta en el compilador pero se descarta aqui: no
// tenemos sistema de tiras de iconos (SetGadgetIconStrip y companeros
// son una pieza de trabajo aparte, no implementada).
void gadget_listbox_insert_item(int32_t id, int32_t index, const char *text) {
    if (!valid(id) || !is_item_gadget(gadgets[id].type)) return;
    gadget_t *g = &gadgets[id];
    if (g->item_count >= LISTBOX_MAX_ITEMS) return;
    if (index < 0) index = 0;
    if ((uint32_t)index > g->item_count) index = (int32_t)g->item_count;
    for (uint32_t i = g->item_count; (int32_t)i > index; i--) {
        set_text(g->items[i], g->items[i - 1]);
    }
    set_text(g->items[index], text);
    g->item_count++;
    if (g->selected >= index) g->selected++; // el seleccionado se desplaza si estaba en o tras el hueco nuevo
}
// RemoveGadgetItem -- quita un item concreto, desplazando los
// siguientes un hueco hacia atras.
void gadget_listbox_remove_item(int32_t id, int32_t index) {
    if (!valid(id) || !is_item_gadget(gadgets[id].type)) return;
    gadget_t *g = &gadgets[id];
    if (index < 0 || (uint32_t)index >= g->item_count) return;
    for (uint32_t i = (uint32_t)index; i + 1 < g->item_count; i++) {
        set_text(g->items[i], g->items[i + 1]);
    }
    g->item_count--;
    if (g->selected == index) g->selected = -1;
    else if (g->selected > index) g->selected--;
}
// ModifyGadgetItem -- cambia el texto de un item ya existente, sin
// mover nada.
void gadget_listbox_modify_item(int32_t id, int32_t index, const char *text) {
    if (!valid(id) || !is_item_gadget(gadgets[id].type)) return;
    gadget_t *g = &gadgets[id];
    if (index < 0 || (uint32_t)index >= g->item_count) return;
    set_text(g->items[index], text);
}
void gadget_listbox_clear(int32_t id) {
    if (!valid(id) || !is_item_gadget(gadgets[id].type)) return;
    gadgets[id].item_count = 0;
    gadgets[id].selected = -1;
}
int32_t gadget_listbox_selected(int32_t id) {
    if (!valid(id) || !is_item_gadget(gadgets[id].type)) return -1;
    return gadgets[id].selected;
}
void gadget_listbox_select(int32_t id, int32_t index) {
    if (!valid(id) || !is_item_gadget(gadgets[id].type)) return;
    if (index >= 0 && (uint32_t)index < gadgets[id].item_count) gadgets[id].selected = index;
}
uint32_t gadget_listbox_item_count(int32_t id) {
    if (!valid(id) || !is_item_gadget(gadgets[id].type)) return 0;
    return gadgets[id].item_count;
}
uint32_t gadget_listbox_item_text(int32_t id, int32_t index, char *out, uint32_t max_len) {
    if (!valid(id) || !is_item_gadget(gadgets[id].type) || max_len == 0) { if (max_len) out[0] = '\0'; return 0; }
    gadget_t *g = &gadgets[id];
    if (index < 0 || (uint32_t)index >= g->item_count) { out[0] = '\0'; return 0; }
    uint32_t i = 0;
    while (g->items[index][i] && i < max_len - 1) { out[i] = g->items[index][i]; i++; }
    out[i] = '\0';
    return i;
}

// ---- TextArea ----

// Reemplaza TODO el contenido de golpe, partiendolo en lineas por
// '\n' -- asi es como funciona SetTextAreaText en BlitzPlus real (a
// diferencia de un ListBox, no hay "añadir de uno en uno" en v1).
// ---- La caja de texto ----
// Cada linea se guarda en GADGET_TEXT_MAX (48) bytes, y antes, al llenarse,
// lo que seguia SE DESCARTABA EN SILENCIO: un programa que iba añadiendo
// texto (AddTextAreaText) dejaba de "escribir" sin ningun aviso. Y se
// dibujaba siempre desde la primera linea, asi que lo ultimo quedaba fuera.
// Ahora:
//   - las lineas se PARTEN solas al ancho de la caja (por la ultima palabra
//     si se puede), o al llenarse su espacio: no se pierde nada;
//   - con las TEXTAREA_MAX_LINES llenas, se descarta la linea MAS ANTIGUA;
//   - se dibujan las ultimas lineas (como un registro).
// Las letras de varios bytes (UTF-8: a, n...) nunca se parten por la mitad.
// El texto partido guarda sus saltos: leerlo de vuelta los incluye.
static void ta_nueva_linea(gadget_t *g) {
    char (*lineas)[TEXTAREA_LINE_MAX] = ta_lineas(g);
    if (!lineas) return;
    if (g->ta_line_count >= TEXTAREA_MAX_LINES) {
        for (uint32_t k = 1; k < TEXTAREA_MAX_LINES; k++) {         // descartar la mas antigua
            // BUG REAL CORREGIDO: esto copiaba GADGET_TEXT_MAX bytes (48), pero
            // cada fila mide TEXTAREA_LINE_MAX (128). Al desplazar, de cada
            // linea solo se movian sus primeros 48 bytes y el resto de la fila
            // se quedaba con la cola de la linea ANTERIOR: cualquier linea de
            // mas de 48 letras salia mezclada con otra en cuanto el TextArea
            // se llenaba y empezaba a descartar.
            for (uint32_t b = 0; b < TEXTAREA_LINE_MAX; b++) lineas[k - 1][b] = lineas[k][b];
        }
        g->ta_line_count = TEXTAREA_MAX_LINES - 1;
    }
    lineas[g->ta_line_count][0] = '\0';
    g->ta_line_count++;
}

// Añade un byte al final. 'ancho' = pixeles utiles de la caja.
static void ta_poner(gadget_t *g, char c, uint32_t ancho) {
    char (*lineas)[TEXTAREA_LINE_MAX] = ta_lineas(g);
    if (!lineas) return;
    if (g->ta_line_count == 0) ta_nueva_linea(g);
    if (c == '\n') { ta_nueva_linea(g); return; }
    char *l = lineas[g->ta_line_count - 1];
    uint32_t col = str_len(l);
    bool continuacion = ((uint8_t)c & 0xC0) == 0x80;                // 2o, 3o... byte de una letra
    if (!continuacion) {
        // ¿cabe una letra entera mas (hasta 4 bytes) y no se pasa del ancho?
        bool lleno = col + 4 >= GADGET_TEXT_MAX;
        if (!lleno && col > 0) {
            l[col] = c; l[col + 1] = '\0';
            lleno = wm_ui_text_width(l) > ancho;
            l[col] = '\0';
        }
        if (lleno && col > 0) {
            // partir por el ultimo espacio, si lo hay; si no, aqui mismo
            int32_t esp = -1;
            for (int32_t k = (int32_t)col - 1; k > 0; k--) if (l[k] == ' ') { esp = k; break; }
            char resto[GADGET_TEXT_MAX]; uint32_t nr = 0;
            if (esp > 0 && c != ' ') {
                for (uint32_t k = (uint32_t)esp + 1; k < col; k++) resto[nr++] = l[k];
                l[esp] = '\0';
            }
            resto[nr] = '\0';
            ta_nueva_linea(g);
            l = lineas[g->ta_line_count - 1];
            for (uint32_t k = 0; k <= nr; k++) l[k] = resto[k];
            col = nr;
            if (c == ' ' && col == 0) return;                          // un espacio al partir: sobra
        }
    } else if (col + 1 >= GADGET_TEXT_MAX) {
        return;                                                         // (no pasa: se reservan 4 bytes)
    }
    l[col] = c; l[col + 1] = '\0';
}

static uint32_t ta_ancho(gadget_t *g) { return (g->w > 12) ? g->w - 12 : 1; }

void gadget_textarea_set_text(int32_t id, const char *text) {
    if (!valid(id) || gadgets[id].type != GADGET_TEXTAREA) return;
    gadget_t *g = &gadgets[id];
    g->ta_line_count = 0;
    ta_nueva_linea(g);
    for (uint32_t i = 0; text[i] != '\0'; i++) ta_poner(g, text[i], ta_ancho(g));
}

// Añade texto al FINAL del contenido -- a diferencia de
// SetTextAreaText (que reemplaza todo), esto respeta lo que ya
// hubiera. Si el texto nuevo contiene '\n', genera lineas nuevas.
void gadget_textarea_add_text(int32_t id, const char *text) {
    if (!valid(id) || gadgets[id].type != GADGET_TEXTAREA) return;
    gadget_t *g = &gadgets[id];
    for (uint32_t i = 0; text[i] != '\0'; i++) ta_poner(g, text[i], ta_ancho(g));
}

// TextAreaLen(textarea[,units]) -- 1=caracteres (por defecto), 2=lineas
uint32_t gadget_textarea_len(int32_t id, int32_t units) {
    if (!valid(id) || gadgets[id].type != GADGET_TEXTAREA) return 0;

    char (*lineas)[TEXTAREA_LINE_MAX] = ta_lineas(&gadgets[id]);
    if (!lineas) return 0;
    gadget_t *g = &gadgets[id];
    if (units == 2) return g->ta_line_count;
    uint32_t total = 0;
    for (uint32_t i = 0; i < g->ta_line_count; i++) {
        total += str_len(lineas[i]);
        if (i + 1 < g->ta_line_count) total += 1; // el '\n' entre lineas
    }
    return total;
}

// TextAreaLineLen(textarea,line) -- longitud de una linea concreta
uint32_t gadget_textarea_line_len(int32_t id, int32_t line) {
    if (!valid(id) || gadgets[id].type != GADGET_TEXTAREA) return 0;

    char (*lineas)[TEXTAREA_LINE_MAX] = ta_lineas(&gadgets[id]);
    if (!lineas) return 0;
    gadget_t *g = &gadgets[id];
    if (line < 0 || (uint32_t)line >= g->ta_line_count) return 0;
    return str_len(lineas[line]);
}

// TextAreaLine(textarea,char) -- que linea contiene el caracter dado
// (indice de caracter sobre el texto plano, contando los '\n' entre
// lineas como parte del recuento).
int32_t gadget_textarea_line_of_char(int32_t id, int32_t charpos) {
    char (*lineas)[TEXTAREA_LINE_MAX] = ta_lineas(&gadgets[id]);
    if (!lineas) return 0;
    if (!valid(id) || gadgets[id].type != GADGET_TEXTAREA) return 0;
    gadget_t *g = &gadgets[id];
    if (g->ta_line_count == 0) return 0;
    int32_t pos = 0;
    for (uint32_t i = 0; i < g->ta_line_count; i++) {
        int32_t len = (int32_t)str_len(lineas[i]);
        if (charpos < pos + len || i + 1 == g->ta_line_count) return (int32_t)i;
        pos += len + 1; // +1 por el '\n' que separa de la siguiente linea
    }
    return (int32_t)g->ta_line_count - 1;
}

// TextAreaText$(textarea[,start[,count]]) -- texto plano (lineas
// unidas por '\n'), recortado a [start, start+count). count<0 =
// "hasta el final". Escribe en 'out' (max_len bytes, incluido el
// terminador).
void gadget_textarea_get_text(int32_t id, int32_t start, int32_t count, char *out, uint32_t max_len) {
    char (*lineas)[TEXTAREA_LINE_MAX] = ta_lineas(&gadgets[id]);
    if (!lineas) return;
    if (max_len > 0) out[0] = '\0';
    if (!valid(id) || gadgets[id].type != GADGET_TEXTAREA || max_len == 0) return;
    gadget_t *g = &gadgets[id];
    // Construimos el texto plano completo en un buffer temporal --
    // TEXTAREA_MAX_LINES*GADGET_TEXT_MAX es pequeño (40*48=1920), cabe
    // de sobra en la pila del kernel.
    char full[TEXTAREA_MAX_LINES * GADGET_TEXT_MAX];
    uint32_t flen = 0;
    for (uint32_t i = 0; i < g->ta_line_count; i++) {
        uint32_t ll = str_len(lineas[i]);
        for (uint32_t j = 0; j < ll && flen < sizeof(full) - 1; j++) full[flen++] = lineas[i][j];
        if (i + 1 < g->ta_line_count && flen < sizeof(full) - 1) full[flen++] = '\n';
    }
    full[flen] = '\0';

    if (start < 0) start = 0;
    if ((uint32_t)start >= flen) return; // fuera de rango -- cadena vacia
    uint32_t avail = flen - (uint32_t)start;
    uint32_t take = (count < 0) ? avail : (uint32_t)count;
    if (take > avail) take = avail;
    if (take > max_len - 1) take = max_len - 1;
    for (uint32_t k = 0; k < take; k++) out[k] = full[(uint32_t)start + k];
    out[take] = '\0';
}

// ---- Menus ----

int32_t gadget_window_menu(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return -1;
    if (window_menu_root_set[window_idx]) return window_menu_root[window_idx];

    int32_t id = create_common(GADGET_MENU_ROOT, 0, 0, 0, 0, window_idx);
    window_menu_root[window_idx] = id;
    window_menu_root_set[window_idx] = true;
    return id;
}

// Un menu flotante vacio. Las entradas se le cuelgan con
// gadget_create_menu pasandole este id como 'parent', exactamente igual que
// a una entrada de la barra: asi comparten el dibujo, las marcas, los
// separadores, el activado y desactivado, y el evento que mandan.
int32_t gadget_create_context_menu(int32_t window_idx) {
    return create_common(GADGET_CONTEXT_ROOT, 0, 0, 0, 0, window_idx);
}

// Lo abre en (x, y), en coordenadas del area de contenido de la ventana.
//
// Se coloca donde se pide y se deja: recortarlo contra el borde de la
// ventana necesita saber el ancho de la ventana aqui dentro, y quien llama
// ya sabe donde ha pulsado el raton. Lo que si se hace es no dejarlo salir
// por arriba ni por la izquierda, que es lo unico que lo dibujaria fuera
// del buffer.
void gadget_show_context_menu(int32_t id, int32_t x, int32_t y) {
    if (!valid(id) || gadgets[id].type != GADGET_CONTEXT_ROOT) return;
    int32_t win = gadgets[id].window_idx;
    if (win < 0 || win >= MAX_WM_WINDOWS) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    open_context[win] = id;
    ctx_x[win] = x;
    ctx_y[win] = y;
    // Un flotante y un desplegable de la barra abiertos a la vez se taparian
    // el uno al otro sin que ninguno de los dos supiera del otro.
    open_menu[win] = -1;
    open_sub[win] = -1;
    wm_request_redraw();
}

int32_t gadget_create_menu(const char *text, int32_t tag, int32_t parent) {
    if (!valid(parent)) return -1;
    int32_t window_idx = gadgets[parent].window_idx;
    int32_t id = create_common(GADGET_MENU, 0, 0, 0, 0, window_idx);
    if (id < 0) return -1;
    set_text(gadgets[id].text, text);
    gadgets[id].tag = tag;
    gadgets[id].parent_menu = parent;
    return id;
}
void gadget_menu_check(int32_t id, bool checked) { if (valid(id)) gadgets[id].checked = checked; }

// ButtonState/SetButtonState -- reutilizan el mismo campo 'checked'
// que los menus (un gadget nunca es las dos cosas a la vez, asi que
// no hay conflicto). Solo tiene sentido para botones de estilo
// casilla (2) o radio (3), pero no hace falta comprobarlo aqui --
// leer/escribir 'checked' en un boton normal simplemente no se ve
// reflejado en el dibujo (draw_button solo pinta el indicador para
// esos dos estilos).
bool gadget_button_state(int32_t id) { return valid(id) ? gadgets[id].checked : false; }

// Getter generico de 'enabled' -- funciona igual para MenuEnabled()
// que para cualquier otro gadget, ya que el campo 'enabled' vive en
// TODOS los gadgets, no solo en menus (misma idea que reutilizar
// gadget_button_state para MenuChecked()).
bool gadget_is_enabled(int32_t id) { return valid(id) ? gadgets[id].enabled : false; }
// Apaga las demas opciones que compiten con esta.
//
// COMPITEN LAS DEL MISMO GRUPO. Antes competian todas las de la misma
// VENTANA, y eso hacia imposible tener dos juegos de opciones a la vez: en
// una ventana con "dificultad" y "sonido", elegir una dificultad apagaba
// tambien la eleccion de sonido. No daba ningun error; simplemente no se
// podia hacer, y el sintoma --una opcion que se apaga sola-- parece un fallo
// de dibujo.
//
// Sin grupo (0), se conserva el comportamiento de siempre: compiten todas
// las de la ventana. Asi los programas que ya existen siguen igual sin
// tocarlos, y quien quiera dos juegos de opciones pone los grupos.
//
// Un solo sitio para las dos formas de marcar una opcion, el clic y
// SetButtonState. Estaban duplicadas, y una regla repetida en dos sitios es
// una regla que acaba siendo dos reglas distintas.
static void apagar_las_demas_del_grupo(int32_t id) {
    gadget_t *g = &gadgets[id];
    for (int k = 0; k < MAX_GADGETS; k++) {
        gadget_t *otro = &gadgets[k];
        if (!otro->used || otro->type != GADGET_BUTTON || otro->style != 3) continue;
        if (otro->window_idx != g->window_idx) continue;
        // Con grupo, solo las de ese grupo. Sin grupo, solo las que
        // tampoco lo tienen -- una opcion agrupada no debe apagar a una
        // suelta ni al reves.
        if (otro->parent_group != g->parent_group) continue;
        otro->checked = false;
    }
}

void gadget_set_button_state(int32_t id, bool state) {
    if (!valid(id) || gadgets[id].type != GADGET_BUTTON) return;
    // Marcar una opcion a mano apaga a sus companeras igual que el clic.
    if (gadgets[id].style == 3 && state) apagar_las_demas_del_grupo(id);
    gadgets[id].checked = state;
}
void gadget_menu_enable(int32_t id, bool enabled) { if (valid(id)) gadgets[id].enabled = enabled; }
int32_t gadget_menu_get_tag(int32_t id) { return valid(id) ? gadgets[id].tag : -1; }

uint32_t gadgets_menubar_height(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return 0;
    if (!window_menu_root_set[window_idx]) return 0;
    // Solo reserva espacio si hay al menos una entrada de primer nivel
    int32_t root = window_menu_root[window_idx];
    for (int i = 1; i < MAX_GADGETS; i++) {
        if (gadgets[i].used && gadgets[i].type == GADGET_MENU && gadgets[i].parent_menu == root) return MENUBAR_H;
    }
    return 0;
}

// ---- dibujo y entrada ----

static void draw_button(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    if (g->style == 2 || g->style == 3) {
        // Casilla (cuadrado) o radio (redondeado via Oval seria mas
        // fiel, pero un cuadrado mas pequeño ya distingue bien el
        // estado sin complicar el dibujo) -- indicador a la
        // izquierda, texto al lado, SIN fondo de boton.
        int32_t bx = g->x + ox, by = g->y + oy + (int32_t)g->h / 2 - 6;
        uint32_t border = g->enabled ? 0x00A0A0A0 : 0x00505050;
        wm_content_fill_rect(win, (uint32_t)bx, (uint32_t)by, 12, 12, border);
        wm_content_fill_rect(win, (uint32_t)(bx + 1), (uint32_t)(by + 1), 10, 10, 0x00181C20);
        if (g->checked) {
            uint32_t mark = g->enabled ? 0x0080C0FF : 0x00506070;
            wm_content_fill_rect(win, (uint32_t)(bx + 3), (uint32_t)(by + 3), 6, 6, mark);
        }
        // el fondo, en un punto que este control nunca pinta: entre el
        // cuadradito (12 px) y el texto (a 18 px), en su fila de arriba
        uint32_t tcolor = texto_legible(win, g->x + ox + 14, g->y + oy + 1, g->enabled);
        wm_content_ui_text(win, (uint32_t)(g->x + ox + 18), (uint32_t)(g->y + oy + (int32_t)g->h / 2 - 3), g->text, tcolor);
        return;
    }
    uint32_t bg = g->enabled ? 0x00505860 : 0x00303840;
    wm_content_fill_rect(win, (uint32_t)(g->x + ox), (uint32_t)(g->y + oy), g->w, g->h, bg);
    wm_content_ui_text(win, (uint32_t)(g->x + ox + 4), (uint32_t)(g->y + oy + (int32_t)g->h / 2 - 3), g->text, 0x00FFFFFF);
}
static void draw_panel(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    int32_t x = g->x + ox, y = g->y + oy;
    if (g->panel_uses_image) {
        uint32_t iw, ih;
        const uint8_t *pixels;
        if (image_get_info(g->icon_strip, &iw, &ih, &pixels) && iw > 0 && ih > 0) {
            // Mosaico: recorremos el area del panel en pasos del
            // tamano de la imagen, recortando el ultimo azulejo
            // (parcial) en cada borde con blit_w/blit_h mas pequenos
            // que iw/ih, mientras src_stride sigue siendo iw completo
            // (para leer las filas de la imagen correctamente).
            for (uint32_t ty = 0; ty < g->h; ty += ih) {
                uint32_t th = (ty + ih > g->h) ? (g->h - ty) : ih;
                for (uint32_t tx = 0; tx < g->w; tx += iw) {
                    uint32_t tw = (tx + iw > g->w) ? (g->w - tx) : iw;
                    wm_content_blit_image_rect(win, (uint32_t)x + tx, (uint32_t)y + ty, tw, th, iw, pixels, true, false, 0);
                }
            }
            return;
        }
        // Handle de imagen invalido -- caemos al color por defecto
    }
    // 'icon_strip' se reutiliza aqui como color RGB empaquetado
    // (0xRRGGBB) fijado con SetPanelColor -- -1 (el valor por
    // defecto de create_common) significa "sin fijar", usa el gris
    // de siempre.
    uint32_t color = (g->icon_strip >= 0) ? (uint32_t)g->icon_strip : 0x00303840;
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, g->h, color);
}
// CreateLabel -- solo texto, sin fondo ni eventos. style: 0/2=sin
// borde, 1=borde plano, 3=borde 3D hundido (construido a mano con 4
// franjas finas, ya que no tenemos una funcion de borde dedicada).
static void draw_label(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    int32_t x = g->x + ox, y = g->y + oy;
    // SEGUNDO BUG REAL, encontrado justo despues del anterior: la
    // etiqueta nunca limpiaba su propio rectangulo antes de escribir
    // -- como se redibuja en CADA vuelta (gadgets_update_and_draw se
    // llama en cada task_yield), un texto mas corto que el anterior
    // (p.ej. "10" -> "9") dejaba pixeles del texto viejo asomando por
    // los lados, imposible de leer. 0xD4D0C8 es el mismo
    // COLOR_WIN_BODY de wm.c (no exportado en wm.h; si ese color
    // cambia alguna vez, hay que actualizar tambien este valor).
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, g->h, 0x00D4D0C8);
    if (g->style == 1) {
        uint32_t c = 0x00707070;
        wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, 1, c);
        wm_content_fill_rect(win, (uint32_t)x, (uint32_t)(y + (int32_t)g->h - 1), g->w, 1, c);
        wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, 1, g->h, c);
        wm_content_fill_rect(win, (uint32_t)(x + (int32_t)g->w - 1), (uint32_t)y, 1, g->h, c);
    } else if (g->style == 3) {
        uint32_t dark = 0x00404040, light = 0x00909090;
        wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, 1, dark); // arriba: oscuro
        wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, 1, g->h, dark); // izquierda: oscuro
        wm_content_fill_rect(win, (uint32_t)x, (uint32_t)(y + (int32_t)g->h - 1), g->w, 1, light); // abajo: claro
        wm_content_fill_rect(win, (uint32_t)(x + (int32_t)g->w - 1), (uint32_t)y, 1, g->h, light); // derecha: claro
    }
    // BUG REAL CORREGIDO: a diferencia de TODOS los demas gadgets
    // (boton, campo de texto, lista...), la etiqueta no pinta ningun
    // fondo propio -- deja ver el cuerpo claro de la ventana por
    // debajo. Usar el mismo texto CLARO que el resto (pensado para
    // sus fondos oscuros) la dejaba casi invisible: a 5x7 pixeles, un
    // digito gris muy claro sobre un fondo tambien claro se ve como
    // un cuadro borroso, no como un caracter legible.
    uint32_t tcolor = g->enabled ? 0x00000000 : 0x00808080;
    wm_content_ui_text(win, (uint32_t)(x + 3), (uint32_t)(y + (int32_t)g->h / 2 - 3), g->text, tcolor);
}
// CreateProgBar -- fondo oscuro + relleno proporcional al valor
// (0.0-1.0), con borde fino para distinguir el hueco del relleno.
static void draw_progbar(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    int32_t x = g->x + ox, y = g->y + oy;
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, g->h, 0x00181C20);
    uint32_t fill_w = (g->w * (uint32_t)g->progress_permil) / 1000;
    if (fill_w > 0) {
        uint32_t fillcolor = g->enabled ? 0x0060A0E0 : 0x00405060;
        wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, fill_w, g->h, fillcolor);
    }
    uint32_t border = 0x00505860;
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, 1, border);
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)(y + (int32_t)g->h - 1), g->w, 1, border);
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, 1, g->h, border);
    wm_content_fill_rect(win, (uint32_t)(x + (int32_t)g->w - 1), (uint32_t)y, 1, g->h, border);
}
// Geometria del "pomo" (thumb) de un slider: posicion y tamaño a lo
// largo del eje principal (horizontal=ancho, vertical=alto), como
// fraccion visible/total del recorrido. Se reutiliza tanto para
// dibujar como para el calculo de arrastre -- misma formula en
// ambos sitios, sin duplicarla.
// El pomo. 'thumb_pos' sale ya contado desde el borde del control, con las
// flechas incluidas si es una barra de desplazamiento -- asi el dibujo y la
// deteccion del raton usan el mismo numero y no hay que acordarse de sumar
// el hueco de la flecha en cada sitio.
static void slider_thumb_geom(gadget_t *g, int32_t *thumb_pos, int32_t *thumb_len) {
    int32_t flecha = flecha_lado(g);
    int32_t track_len = ((g->style == 2) ? (int32_t)g->h : (int32_t)g->w) - 2 * flecha;
    if (track_len < 1) track_len = 1;
    int32_t total = g->slider_total > 0 ? g->slider_total : 1;
    int32_t visible = g->slider_visible > 0 ? g->slider_visible : 1;
    int32_t len = (track_len * visible) / total;
    if (len < 8) len = 8; // tamaño minimo para poder agarrarlo con el raton
    if (len > track_len) len = track_len;
    int32_t maxval = total - visible;
    int32_t avail = track_len - len; // espacio de movimiento del pomo
    int32_t pos = (maxval > 0 && avail > 0) ? (avail * g->slider_value) / maxval : 0;
    *thumb_pos = pos + flecha;
    *thumb_len = len;
}
static void draw_slider(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    int32_t x = g->x + ox, y = g->y + oy;
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, g->h, 0x00181C20); // track
    int32_t thumb_pos, thumb_len;
    slider_thumb_geom(g, &thumb_pos, &thumb_len);
    uint32_t thumbcolor = g->enabled ? 0x00707880 : 0x00404850;
    if (g->style == 2) {
        wm_content_fill_rect(win, (uint32_t)x, (uint32_t)(y + thumb_pos), g->w, (uint32_t)thumb_len, thumbcolor);
    } else {
        wm_content_fill_rect(win, (uint32_t)(x + thumb_pos), (uint32_t)y, (uint32_t)thumb_len, g->h, thumbcolor);
    }
    uint32_t border = 0x00505860;
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, 1, border);
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)(y + (int32_t)g->h - 1), g->w, 1, border);
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, 1, g->h, border);
    wm_content_fill_rect(win, (uint32_t)(x + (int32_t)g->w - 1), (uint32_t)y, 1, g->h, border);
}

// Un triangulito apuntando a un lado, dibujado por filas. Sin poligonos:
// para cuatro flechas de doce pixeles no hace falta un rasterizador.
// 'dir': 0 arriba, 1 abajo, 2 izquierda, 3 derecha.
static void draw_flecha(int32_t win, int32_t x, int32_t y, int32_t lado, int dir, uint32_t color) {
    int32_t margen = lado / 4;
    int32_t n = lado - 2 * margen;               // lado util del triangulo
    if (n < 3) { margen = 0; n = lado; }
    if (n < 1) return;
    for (int32_t k = 0; k < n; k++) {
        // En cada paso la base crece de dos en dos desde la punta.
        int32_t ancho = 1 + 2 * k;
        if (ancho > n) ancho = n;
        int32_t desde = (n - ancho) / 2;
        switch (dir) {
            case 0: wm_content_fill_rect(win, (uint32_t)(x + margen + desde), (uint32_t)(y + margen + k),
                                         (uint32_t)ancho, 1, color); break;
            case 1: wm_content_fill_rect(win, (uint32_t)(x + margen + desde), (uint32_t)(y + margen + n - 1 - k),
                                         (uint32_t)ancho, 1, color); break;
            case 2: wm_content_fill_rect(win, (uint32_t)(x + margen + k), (uint32_t)(y + margen + desde),
                                         1, (uint32_t)ancho, color); break;
            default: wm_content_fill_rect(win, (uint32_t)(x + margen + n - 1 - k), (uint32_t)(y + margen + desde),
                                          1, (uint32_t)ancho, color); break;
        }
    }
}

// La barra de desplazamiento: el mismo carril y el mismo pomo que el
// deslizador, mas una flecha en cada extremo.
static void draw_scrollbar(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    int32_t x = g->x + ox, y = g->y + oy;
    bool vertical = (g->style == 2);
    int32_t flecha = flecha_lado(g);
    uint32_t fondo = 0x00181C20, marco = 0x00505860;
    uint32_t tinta = g->enabled ? 0x00C0C8D0 : 0x00606870;
    uint32_t pomo  = g->enabled ? 0x00707880 : 0x00404850;

    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, g->h, fondo);

    int32_t thumb_pos, thumb_len;
    slider_thumb_geom(g, &thumb_pos, &thumb_len);
    if (vertical) wm_content_fill_rect(win, (uint32_t)x, (uint32_t)(y + thumb_pos), g->w, (uint32_t)thumb_len, pomo);
    else          wm_content_fill_rect(win, (uint32_t)(x + thumb_pos), (uint32_t)y, (uint32_t)thumb_len, g->h, pomo);

    if (flecha > 0) {
        uint32_t cajas = 0x00303840;
        if (vertical) {
            wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, (uint32_t)flecha, cajas);
            wm_content_fill_rect(win, (uint32_t)x, (uint32_t)(y + (int32_t)g->h - flecha), g->w, (uint32_t)flecha, cajas);
            draw_flecha(win, x, y, flecha, 0, tinta);
            draw_flecha(win, x, y + (int32_t)g->h - flecha, flecha, 1, tinta);
        } else {
            wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, (uint32_t)flecha, g->h, cajas);
            wm_content_fill_rect(win, (uint32_t)(x + (int32_t)g->w - flecha), (uint32_t)y, (uint32_t)flecha, g->h, cajas);
            draw_flecha(win, x, y, flecha, 2, tinta);
            draw_flecha(win, x + (int32_t)g->w - flecha, y, flecha, 3, tinta);
        }
    }

    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, 1, marco);
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)(y + (int32_t)g->h - 1), g->w, 1, marco);
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, 1, g->h, marco);
    wm_content_fill_rect(win, (uint32_t)(x + (int32_t)g->w - 1), (uint32_t)y, 1, g->h, marco);
}

// Calcula el nuevo valor de un slider a partir de una coordenada
// LOCAL del raton (relativa al origen del gadget, en el eje
// principal) -- el click se interpreta como "el CENTRO del pomo
// deberia estar aqui", para que agarrar y arrastrar se sienta natural.
static void slider_set_value_from_local(gadget_t *g, int32_t local_pos) {
    int32_t flecha = flecha_lado(g);
    int32_t track_len = ((g->style == 2) ? (int32_t)g->h : (int32_t)g->w) - 2 * flecha;
    int32_t thumb_pos, thumb_len;
    slider_thumb_geom(g, &thumb_pos, &thumb_len);
    (void)thumb_pos;
    // La coordenada llega contada desde el borde del control; el carril
    // empieza despues de la flecha.
    local_pos -= flecha;
    int32_t avail = track_len - thumb_len;
    int32_t maxval = g->slider_total - g->slider_visible;
    if (avail <= 0 || maxval <= 0) { g->slider_value = 0; return; }
    int32_t target = local_pos - thumb_len / 2;
    if (target < 0) target = 0;
    if (target > avail) target = avail;
    int32_t newval = (target * maxval) / avail;
    if (newval < 0) newval = 0;
    if (newval > maxval) newval = maxval;
    g->slider_value = newval;
}
static void draw_textfield(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    uint32_t border = g->focused ? 0x0080A0FF : 0x00707070;
    wm_content_fill_rect(win, (uint32_t)(g->x + ox), (uint32_t)(g->y + oy), g->w, g->h, border);
    wm_content_fill_rect(win, (uint32_t)(g->x + ox + 1), (uint32_t)(g->y + oy + 1), g->w - 2, g->h - 2, 0x00181C20);
    wm_content_ui_text(win, (uint32_t)(g->x + ox + 4), (uint32_t)(g->y + oy + (int32_t)g->h / 2 - 3), g->text, 0x00E0E0E0);
}
static void draw_listbox(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    wm_content_fill_rect(win, (uint32_t)(g->x + ox), (uint32_t)(g->y + oy), g->w, g->h, 0x00181C20);
    wm_content_fill_rect(win, (uint32_t)(g->x + ox), (uint32_t)(g->y + oy), g->w, 1, 0x00707070);

    uint32_t max_rows = g->h / ROW_H;
    for (uint32_t i = 0; i < g->item_count && i < max_rows; i++) {
        int32_t ry = g->y + oy + 2 + (int32_t)(i * ROW_H);
        if ((int32_t)i == g->selected) {
            wm_content_fill_rect(win, (uint32_t)(g->x + ox + 1), (uint32_t)ry, g->w - 2, ROW_H, 0x00405070);
        }
        wm_content_ui_text(win, (uint32_t)(g->x + ox + 4), (uint32_t)(ry + 5), g->items[i], 0x00E0E0E0);
    }
}
// CreateComboBox -- caja cerrada mostrando el item seleccionado +
// indicador, y (si esta abierta) una lista desplegable debajo,
// dibujada igual que ListBox pero sin limite de altura propio. NOTA:
// igual que el desplegable de menu, se dibuja "en linea" en el orden
// de iteracion normal -- si otro gadget se solapa visualmente por
// debajo y se dibuja DESPUES en la lista, podria taparlo (misma
// limitacion ya existente y aceptada para los menus).
static void draw_combobox(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    int32_t id = (int32_t)(g - gadgets);
    int32_t x = g->x + ox, y = g->y + oy;
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, g->h, 0x00181C20);
    uint32_t border = 0x00505860;
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, 1, border);
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)(y + (int32_t)g->h - 1), g->w, 1, border);
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, 1, g->h, border);
    wm_content_fill_rect(win, (uint32_t)(x + (int32_t)g->w - 1), (uint32_t)y, 1, g->h, border);
    if (g->selected >= 0 && (uint32_t)g->selected < g->item_count) {
        uint32_t tcolor = g->enabled ? 0x00E0E0E0 : 0x00808080;
        wm_content_ui_text(win, (uint32_t)(x + 4), (uint32_t)(y + (int32_t)g->h / 2 - 3), g->items[g->selected], tcolor);
    }
    wm_content_ui_text(win, (uint32_t)(x + (int32_t)g->w - 12), (uint32_t)(y + (int32_t)g->h / 2 - 3), "v", 0x00A0A0A0);

    if (open_combobox[win] != id || g->item_count == 0) {
        // cerrado: si su lista seguia pintada, borrarla (con el fondo de
        // las ventanas, como hacen los menus; lo que hubiera debajo se
        // repinta solo en la vuelta siguiente)
        if (combo_dd_id[win] == id) {
            wm_content_fill_rect(win, (uint32_t)combo_dd_x[win], (uint32_t)combo_dd_y[win], combo_dd_w[win], combo_dd_h[win], 0x00D4D0C8);
            combo_dd_id[win] = -1;
        }
        return;
    }

    int32_t dd_y = y + (int32_t)g->h;
    uint32_t dd_h = g->item_count * ROW_H + 4;
    combo_dd_id[win] = id; combo_dd_x[win] = x; combo_dd_y[win] = dd_y; combo_dd_w[win] = g->w; combo_dd_h[win] = dd_h;
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)dd_y, g->w, dd_h, 0x00181C20);
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)dd_y, g->w, 1, border);
    for (uint32_t i = 0; i < g->item_count; i++) {
        int32_t ry = dd_y + 2 + (int32_t)(i * ROW_H);
        if ((int32_t)i == g->selected) {
            wm_content_fill_rect(win, (uint32_t)(x + 1), (uint32_t)ry, g->w - 2, ROW_H, 0x00405070);
        }
        wm_content_ui_text(win, (uint32_t)(x + 4), (uint32_t)(ry + 5), g->items[i], 0x00E0E0E0);
    }
}
#define TABBER_TAB_H 22
// CreateTabber -- fila de pestañas en la parte superior del gadget.
// BlitzPlus real documenta que el PROPIO PROGRAMA debe encargarse de
// mostrar/ocultar el contenido de cada pestaña (sugiere CreatePanel
// + Hide/ShowGadget, que ya tenemos) -- aqui solo dibujamos y
// gestionamos la fila de pestañas en si, sin logica de contenido.
static void draw_tabber(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    int32_t x = g->x + ox, y = g->y + oy;
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, TABBER_TAB_H, 0x00181C20);
    int32_t tx = x;
    for (uint32_t i = 0; i < g->item_count; i++) {
        int32_t tw = (int32_t)wm_ui_text_width(g->items[i]) + 14;
        uint32_t bg = ((int32_t)i == g->selected) ? 0x00405070 : 0x00303840;
        wm_content_fill_rect(win, (uint32_t)tx, (uint32_t)y, (uint32_t)tw, TABBER_TAB_H, bg);
        wm_content_ui_text(win, (uint32_t)(tx + 6), (uint32_t)(y + 7), g->items[i], 0x00E0E0E0);
        tx += tw;
    }
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)(y + (int32_t)TABBER_TAB_H), g->w, 1, 0x00505860);
}
// CreateToolBar -- recorta cada boton (cuadrado, lado = alto de la
// imagen) de la tira de iconos ya cargada, con el pixel superior
// izquierdo de la imagen como color transparente (confirmado en la
// documentacion oficial). Los botones desactivados se dibujan
// "fantasma" (mas oscuros, sin el icono encima).
static void draw_toolbar(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    uint32_t iw, ih;
    const uint8_t *pixels;
    if (!image_get_info(g->icon_strip, &iw, &ih, &pixels) || ih == 0) return;
    uint32_t mask_color = ((uint32_t)pixels[0] << 16) | ((uint32_t)pixels[1] << 8) | pixels[2];
    int32_t x = g->x + ox, y = g->y + oy;
    for (uint32_t i = 0; i < g->item_count && i < LISTBOX_MAX_ITEMS; i++) {
        int32_t bx = x + (int32_t)(i * ih);
        bool enabled = g->items_enabled[i];
        uint32_t bg = enabled ? 0x00303840 : 0x00202428;
        wm_content_fill_rect(win, (uint32_t)bx, (uint32_t)y, ih, ih, bg);
        const uint8_t *src = pixels + (uint32_t)(i * ih) * 4; // offset horizontal en la tira
        if (enabled) {
            wm_content_blit_image_rect(win, (uint32_t)bx, (uint32_t)y, ih, ih, iw, src, false, true, mask_color);
        } else {
            // desactivado: el icono en gris apagado (antes no se
            // dibujaba nada, y no se sabia que boton era)
            for (uint32_t py = 0; py < ih; py++) {
                for (uint32_t px = 0; px < ih; px++) {
                    const uint8_t *q = src + (py * iw + px) * 4;
                    uint32_t c = ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
                    if (c == mask_color) continue;
                    uint32_t l = ((uint32_t)q[0] * 3 + (uint32_t)q[1] * 6 + q[2]) / 10;
                    uint32_t v = 0x28 + l / 4;                       // gris, cerca del fondo
                    wm_content_fill_rect(win, (uint32_t)bx + px, (uint32_t)y + py, 1, 1, (v << 16) | (v << 8) | v);
                }
            }
        }
    }
}
// Recorre recursivamente los HIJOS de 'parent' (en profundidad,
// preorden), dibujando una fila por nodo visible -- 'row' es un
// contador compartido a traves de la recursion (fila actual dentro
// del area visible del TreeView), 'depth' controla la sangria. Los
// hijos de un nodo COLAPSADO ('checked'=false) no se dibujan ni
// cuentan filas. Devuelve la fila siguiente tras dibujar este
// subarbol completo.
static uint32_t draw_treeview_children(gadget_t *tv, int32_t tv_id, int32_t win, int32_t x, int32_t y, int32_t parent, int32_t depth, uint32_t row, uint32_t max_rows) {
    for (int i = 1; i < MAX_GADGETS && row < max_rows; i++) {
        gadget_t *n = &gadgets[i];
        if (!n->used || n->type != GADGET_TREENODE || n->parent_menu != parent) continue;
        int32_t ry = y + 2 + (int32_t)(row * ROW_H);
        int32_t indent = depth * 14;
        bool has_children = gadget_count_treeview_nodes(i) > 0;
        if (i == tv->selected) {
            wm_content_fill_rect(win, (uint32_t)(x + 1), (uint32_t)ry, tv->w - 2, ROW_H, 0x00405070);
        }
        if (has_children) {
            wm_content_ui_text(win, (uint32_t)(x + 4 + indent), (uint32_t)(ry + 5), n->checked ? "-" : "+", 0x00A0A0A0);
        }
        wm_content_ui_text(win, (uint32_t)(x + 16 + indent), (uint32_t)(ry + 5), n->text, 0x00E0E0E0);
        row++;
        if (n->checked && row < max_rows) {
            row = draw_treeview_children(tv, tv_id, win, x, y, i, depth + 1, row, max_rows);
        }
    }
    return row;
}
// Recorre igual que draw_treeview_children, pero buscando que nodo
// cae en la fila 'target_row' -- 'row' es el contador compartido a
// traves de la recursion. Devuelve el id del nodo, o -1 si esa fila
// no corresponde a ningun nodo visible (fuera de rango, o cayo en un
// hueco tras un subarbol colapsado).
static int32_t treeview_node_at_row(int32_t parent, int32_t target_row, uint32_t *row) {
    for (int i = 1; i < MAX_GADGETS; i++) {
        gadget_t *n = &gadgets[i];
        if (!n->used || n->type != GADGET_TREENODE || n->parent_menu != parent) continue;
        if ((int32_t)*row == target_row) return i;
        (*row)++;
        if (n->checked) {
            int32_t found = treeview_node_at_row(i, target_row, row);
            if (found >= 0) return found;
        }
    }
    return -1;
}
static void draw_treeview(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    int32_t x = g->x + ox, y = g->y + oy;
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, g->h, 0x00181C20);
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)y, g->w, 1, 0x00707070);
    int32_t tv_id = (int32_t)(g - gadgets);
    int32_t root = -1;
    for (int i = 1; i < MAX_GADGETS; i++) {
        if (gadgets[i].used && gadgets[i].type == GADGET_TREENODE &&
            gadgets[i].tree_owner == tv_id && gadgets[i].parent_menu == -1) { root = i; break; }
    }
    if (root < 0) return; // TreeViewRoot() aun no se ha llamado
    uint32_t max_rows = g->h / ROW_H;
    draw_treeview_children(g, tv_id, win, x, y, root, 0, 0, max_rows);
}
// ---- Escribir en una caja de texto ----
// Las mismas teclas que en cualquier editor: letras, Intro, Retroceso, Supr,
// flechas, Inicio y Fin. El kernel traduce las teclas sin caracter a los
// codigos 0x11-0x18 (ver input.c), que es lo que llega aqui.
static void ta_asegurar_visible(gadget_t *g) {
    uint32_t filas = g->h / ROW_H;
    if (filas == 0) filas = 1;
    if (g->ta_cur_line < g->ta_scroll) g->ta_scroll = g->ta_cur_line;
    if (g->ta_cur_line >= g->ta_scroll + filas) g->ta_scroll = g->ta_cur_line - filas + 1;
}
static void textarea_handle_key(gadget_t *g, char c) {
    char (*lineas)[TEXTAREA_LINE_MAX] = ta_lineas(g);
    if (!lineas) return;
    if (g->ta_line_count == 0) g->ta_line_count = 1;
    if (g->ta_cur_line >= g->ta_line_count) g->ta_cur_line = g->ta_line_count - 1;
    char *l = lineas[g->ta_cur_line];
    uint32_t len = str_len(l);
    if (g->ta_cur_col > len) g->ta_cur_col = len;
    uint8_t u = (uint8_t)c;
    if (u == 0x11 || u == 0x12) {                       // flecha izquierda / derecha
        if (u == 0x11) {
            if (g->ta_cur_col > 0) g->ta_cur_col--;
            else if (g->ta_cur_line > 0) { g->ta_cur_line--; g->ta_cur_col = str_len(lineas[g->ta_cur_line]); }
        } else {
            if (g->ta_cur_col < len) g->ta_cur_col++;
            else if (g->ta_cur_line + 1 < g->ta_line_count) { g->ta_cur_line++; g->ta_cur_col = 0; }
        }
    } else if (u == 0x13 || u == 0x14) {                // flecha arriba / abajo
        if (u == 0x13 && g->ta_cur_line > 0) g->ta_cur_line--;
        else if (u == 0x14 && g->ta_cur_line + 1 < g->ta_line_count) g->ta_cur_line++;
        uint32_t nl = str_len(lineas[g->ta_cur_line]);
        if (g->ta_cur_col > nl) g->ta_cur_col = nl;
    } else if (u == 0x15) { g->ta_cur_col = 0;                       // Inicio
    } else if (u == 0x16) { g->ta_cur_col = len;                     // Fin
    } else if (u == 0x17 || u == 0x18) {                             // RePag / AvPag
        uint32_t filas = g->h / ROW_H; if (filas == 0) filas = 1;
        if (u == 0x17) g->ta_cur_line = (g->ta_cur_line > filas) ? g->ta_cur_line - filas : 0;
        else { g->ta_cur_line += filas; if (g->ta_cur_line >= g->ta_line_count) g->ta_cur_line = g->ta_line_count - 1; }
        uint32_t nl = str_len(lineas[g->ta_cur_line]);
        if (g->ta_cur_col > nl) g->ta_cur_col = nl;
    } else if (c == '\n' || c == '\r') {                             // Intro: partir la linea
        if (g->ta_line_count >= TEXTAREA_MAX_LINES) return;
        for (uint32_t i = g->ta_line_count; i > g->ta_cur_line + 1; i--)
            for (uint32_t b = 0; b < TEXTAREA_LINE_MAX; b++) lineas[i][b] = lineas[i - 1][b];
        char *nueva = lineas[g->ta_cur_line + 1];
        uint32_t k = 0;
        for (uint32_t i = g->ta_cur_col; i < len && k < TEXTAREA_LINE_MAX - 1; i++) nueva[k++] = l[i];
        nueva[k] = '\0';
        l[g->ta_cur_col] = '\0';
        g->ta_line_count++;
        g->ta_cur_line++; g->ta_cur_col = 0;
    } else if (c == 8 || c == 127) {                                 // Retroceso
        if (g->ta_cur_col > 0) {
            for (uint32_t i = g->ta_cur_col - 1; i < len; i++) l[i] = l[i + 1];
            g->ta_cur_col--;
        } else if (g->ta_cur_line > 0) {                              // unir con la de arriba
            char *arriba = lineas[g->ta_cur_line - 1];
            uint32_t la = str_len(arriba), k = la;
            for (uint32_t i = 0; i < len && k < TEXTAREA_LINE_MAX - 1; i++) arriba[k++] = l[i];
            arriba[k] = '\0';
            for (uint32_t i = g->ta_cur_line; i + 1 < g->ta_line_count; i++)
                for (uint32_t b = 0; b < TEXTAREA_LINE_MAX; b++) lineas[i][b] = lineas[i + 1][b];
            g->ta_line_count--;
            g->ta_cur_line--; g->ta_cur_col = la;
        }
    } else if (u >= 32 && u != 127) {                                // una letra
        if (len + 1 >= TEXTAREA_LINE_MAX) return;
        for (uint32_t i = len + 1; i > g->ta_cur_col; i--) l[i] = l[i - 1];
        l[g->ta_cur_col] = c;
        g->ta_cur_col++;
    } else return;
    ta_asegurar_visible(g);
}

static void draw_textarea(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    char (*lineas)[TEXTAREA_LINE_MAX] = ta_lineas(g);
    if (!lineas) return;
    wm_content_fill_rect(win, (uint32_t)(g->x + ox), (uint32_t)(g->y + oy), g->w, g->h, 0x00181C20);
    wm_content_fill_rect(win, (uint32_t)(g->x + ox), (uint32_t)(g->y + oy), g->w, 1, 0x00707070);

    // Desde donde mire el usuario (ta_scroll). Sin foco y sin haber tocado
    // nada, se ven las ULTIMAS lineas: lo que un programa acaba de escribir.
    uint32_t max_rows = g->h / ROW_H;
    if (max_rows == 0) max_rows = 1;
    uint32_t primera = g->ta_scroll;
    if (!g->focused && g->ta_cur_line == 0 && g->ta_cur_col == 0 && g->ta_scroll == 0)
        primera = (g->ta_line_count > max_rows) ? g->ta_line_count - max_rows : 0;
    for (uint32_t i = primera; i < g->ta_line_count && i < primera + max_rows; i++) {
        int32_t ry = g->y + oy + 2 + (int32_t)((i - primera) * ROW_H);
        wm_content_ui_text(win, (uint32_t)(g->x + ox + 4), (uint32_t)(ry + 5), lineas[i], 0x00E0E0E0);
    }
    if (g->focused) {                        // el cursor donde se escribe
        if (g->ta_cur_line >= primera && g->ta_cur_line < primera + max_rows) {
            char antes[TEXTAREA_LINE_MAX];
            uint32_t n = g->ta_cur_col;
            if (n >= TEXTAREA_LINE_MAX) n = TEXTAREA_LINE_MAX - 1;
            for (uint32_t i = 0; i < n; i++) antes[i] = lineas[g->ta_cur_line][i];
            antes[n] = '\0';
            int32_t cx = g->x + ox + 4 + (int32_t)wm_ui_text_width(antes);
            int32_t cy = g->y + oy + 2 + (int32_t)((g->ta_cur_line - primera) * ROW_H);
            wm_content_fill_rect(win, (uint32_t)cx, (uint32_t)(cy + 3), 2, ROW_H - 4, 0x00FFFFFF);
        }
        wm_content_fill_rect(win, (uint32_t)(g->x + ox), (uint32_t)(g->y + oy), g->w, 1, 0x00FFD060);
    }
}

static void draw_menubar(int32_t window_idx, int32_t root) {
    int32_t wx, wy; uint32_t ww, wh;
    if (!wm_get_window_client_rect(window_idx, &wx, &wy, &ww, &wh)) return;
    (void)wx; (void)wy; (void)wh;

    wm_content_fill_rect(window_idx, 0, 0, ww, MENUBAR_H, 0x00D4D0C8);

    int32_t x = 4;
    for (int i = 1; i < MAX_GADGETS; i++) {
        gadget_t *g = &gadgets[i];
        if (!g->used || g->type != GADGET_MENU || g->parent_menu != root) continue;
        uint32_t color = g->enabled ? 0x00000000 : 0x00808080;
        wm_content_ui_text(window_idx, (uint32_t)x, 4, g->text, color);
        x += (int32_t)wm_ui_text_width(g->text) + 14;
    }

    // Borramos SIEMPRE donde estaba el desplegable la vez anterior --
    // si no, sus pixeles se quedan pegados en pantalla al cambiar de
    // menu o al cerrarlo (0x00D4D0C8 es el mismo fondo por defecto que
    // usa el resto de la ventana).
    // El submenu se destapa ANTES que el desplegable: esta dibujado
    // encima y en parte sobre el, asi que al reves le devolveria al
    // desplegable unos pixeles que ya incluyen el submenu.
    if (last_sub_w[window_idx] > 0) {
        if (fondo_sub[window_idx]) {
            wm_content_restore_rect(window_idx, (uint32_t)last_sub_x[window_idx], (uint32_t)last_sub_y[window_idx],
                                    (uint32_t)last_sub_w[window_idx], (uint32_t)last_sub_h[window_idx], fondo_sub[window_idx]);
        } else {
            wm_content_fill_rect(window_idx, (uint32_t)last_sub_x[window_idx], (uint32_t)last_sub_y[window_idx],
                                  (uint32_t)last_sub_w[window_idx], (uint32_t)last_sub_h[window_idx], 0x00D4D0C8);
        }
        last_sub_w[window_idx] = 0;
    }
    if (last_dd_w[window_idx] > 0) {
        if (fondo_dd[window_idx]) {
            wm_content_restore_rect(window_idx, (uint32_t)last_dd_x[window_idx], (uint32_t)last_dd_y[window_idx],
                                    (uint32_t)last_dd_w[window_idx], (uint32_t)last_dd_h[window_idx], fondo_dd[window_idx]);
        } else {
            wm_content_fill_rect(window_idx, (uint32_t)last_dd_x[window_idx], (uint32_t)last_dd_y[window_idx],
                                  (uint32_t)last_dd_w[window_idx], (uint32_t)last_dd_h[window_idx], 0x00D4D0C8);
        }
        last_dd_w[window_idx] = 0;
    }

}

// El desplegable abierto, si lo hay -- solo un nivel de submenu (lo
// habitual: barra -> lista de opciones, sin sub-sub-menus).
// Se dibuja al FINAL de la vuelta, despues de todos los controles: antes
// iba con la barra, al principio, y la barra de herramientas o un panel
// que estuvieran debajo lo tapaban.
static void draw_menu_desplegable(int32_t window_idx, int32_t root) {
    int32_t open = open_menu[window_idx];
    if (open <= 0 || !gadgets[open].used) return;
    int32_t menu_x, my, dw, dh;
    menu_geo(root, open, &menu_x, &my, &dw, &dh);
    if (dh <= 4) return;                                   // sin entradas
    // Guardar lo de debajo ANTES de tapar
    {
        uint8_t *b = fondo_reservar(&fondo_dd[window_idx], &fondo_dd_cap[window_idx], (uint32_t)dw * (uint32_t)dh * 4);
        if (!b || !wm_content_save_rect(window_idx, (uint32_t)menu_x, MENUBAR_H, (uint32_t)dw, (uint32_t)dh, b)) {
            if (fondo_dd[window_idx]) { kfree(fondo_dd[window_idx]); fondo_dd[window_idx] = 0; fondo_dd_cap[window_idx] = 0; }
        }
    }
    wm_content_fill_rect(window_idx, (uint32_t)menu_x, MENUBAR_H, (uint32_t)dw, (uint32_t)dh, 0x00F0F0F0);
    wm_content_fill_rect(window_idx, (uint32_t)menu_x, MENUBAR_H, (uint32_t)dw, 1, 0x00000000);

    // Recordamos este rectangulo para poder borrarlo la proxima vez.
    last_dd_x[window_idx] = menu_x;
    last_dd_y[window_idx] = MENUBAR_H;
    last_dd_w[window_idx] = dw;
    last_dd_h[window_idx] = dh;
    dibujar_filas_menu(window_idx, open, menu_x, MENUBAR_H, dw);

    // el submenu abierto, a la derecha
    int32_t sx, sy, sw, sh;
    if (menu_geo_sub(window_idx, root, &sx, &sy, &sw, &sh)) {
        {
            uint8_t *b = fondo_reservar(&fondo_sub[window_idx], &fondo_sub_cap[window_idx], (uint32_t)sw * (uint32_t)sh * 4);
            if (!b || !wm_content_save_rect(window_idx, (uint32_t)sx, (uint32_t)sy, (uint32_t)sw, (uint32_t)sh, b)) {
                if (fondo_sub[window_idx]) { kfree(fondo_sub[window_idx]); fondo_sub[window_idx] = 0; fondo_sub_cap[window_idx] = 0; }
            }
        }
        wm_content_fill_rect(window_idx, (uint32_t)sx, (uint32_t)sy, (uint32_t)sw, (uint32_t)sh, 0x00F0F0F0);
        wm_content_fill_rect(window_idx, (uint32_t)sx, (uint32_t)sy, (uint32_t)sw, 1, 0x00808080);
        wm_content_fill_rect(window_idx, (uint32_t)sx, (uint32_t)sy, 1, (uint32_t)sh, 0x00808080);
        last_sub_x[window_idx] = sx; last_sub_y[window_idx] = sy; last_sub_w[window_idx] = sw; last_sub_h[window_idx] = sh;
        dibujar_filas_menu(window_idx, open_sub[window_idx], sx, sy, sw);
    }
}

// El menu flotante de una ventana. Dos partes: borrar donde estaba el de
// la vuelta anterior, y dibujar el de ahora.
//
// El borrado va SIEMPRE, incluso sin menu abierto: es lo que destapa lo que
// habia debajo al cerrarlo. Y va aqui y no en draw_menubar porque una
// ventana puede no tener barra de menu, y entonces draw_menubar no se
// ejecuta nunca -- el flotante se quedaria pegado en pantalla.
static void draw_menu_contextual(int32_t window_idx) {
    if (last_ctx_w[window_idx] > 0) {
        if (fondo_ctx[window_idx]) {
            wm_content_restore_rect(window_idx, (uint32_t)last_ctx_x[window_idx], (uint32_t)last_ctx_y[window_idx],
                                    (uint32_t)last_ctx_w[window_idx], (uint32_t)last_ctx_h[window_idx], fondo_ctx[window_idx]);
        } else {
            wm_content_fill_rect(window_idx, (uint32_t)last_ctx_x[window_idx], (uint32_t)last_ctx_y[window_idx],
                                 (uint32_t)last_ctx_w[window_idx], (uint32_t)last_ctx_h[window_idx], 0x00D4D0C8);
        }
        last_ctx_w[window_idx] = 0;
    }

    int32_t raiz = open_context[window_idx];
    if (raiz <= 0 || !gadgets[raiz].used) return;
    int32_t dw = menu_ancho_desplegable(raiz);
    int32_t dh = menu_contar(raiz) * MENU_ROW_H + 4;
    if (dh <= 4) return;                                    // sin entradas: no se abre nada
    int32_t x = ctx_x[window_idx], y = ctx_y[window_idx];

    {   // guardar lo de debajo ANTES de tapar
        uint8_t *b = fondo_reservar(&fondo_ctx[window_idx], &fondo_ctx_cap[window_idx], (uint32_t)dw * (uint32_t)dh * 4);
        if (!b || !wm_content_save_rect(window_idx, (uint32_t)x, (uint32_t)y, (uint32_t)dw, (uint32_t)dh, b)) {
            if (fondo_ctx[window_idx]) { kfree(fondo_ctx[window_idx]); fondo_ctx[window_idx] = 0; fondo_ctx_cap[window_idx] = 0; }
        }
    }
    wm_content_fill_rect(window_idx, (uint32_t)x, (uint32_t)y, (uint32_t)dw, (uint32_t)dh, 0x00F0F0F0);
    wm_content_fill_rect(window_idx, (uint32_t)x, (uint32_t)y, (uint32_t)dw, 1, 0x00808080);
    wm_content_fill_rect(window_idx, (uint32_t)x, (uint32_t)y, 1, (uint32_t)dh, 0x00808080);
    last_ctx_x[window_idx] = x; last_ctx_y[window_idx] = y;
    last_ctx_w[window_idx] = dw; last_ctx_h[window_idx] = dh;
    dibujar_filas_menu(window_idx, raiz, x, y, dw);
}

static bool menu_tiene_hijos(int32_t id) {
    for (int i = 1; i < MAX_GADGETS; i++)
        if (gadgets[i].used && gadgets[i].type == GADGET_MENU && gadgets[i].parent_menu == id) return true;
    return false;
}
static int32_t menu_ancho_desplegable(int32_t open) {
    int32_t w = 120;
    for (int i = 1; i < MAX_GADGETS; i++) {
        gadget_t *g = &gadgets[i];
        if (!g->used || g->type != GADGET_MENU || g->parent_menu != open) continue;
        int32_t t = (int32_t)wm_ui_text_width(g->text) + MENU_MARCA_W + 16 + (menu_tiene_hijos(i) ? 14 : 0);   // + la flecha
        if (t > w) w = t;
    }
    return w;
}
// Las filas de un desplegable, en orden: la 'fila'-esima entrada de 'padre'
static int32_t menu_entrada_en(int32_t padre, int32_t fila) {
    int32_t r = 0;
    for (int i = 1; i < MAX_GADGETS; i++) {
        if (!gadgets[i].used || gadgets[i].type != GADGET_MENU || gadgets[i].parent_menu != padre) continue;
        if (r == fila) return i;
        r++;
    }
    return -1;
}
static int32_t menu_contar(int32_t padre) {
    int32_t n = 0;
    for (int i = 1; i < MAX_GADGETS; i++) if (gadgets[i].used && gadgets[i].type == GADGET_MENU && gadgets[i].parent_menu == padre) n++;
    return n;
}
// Donde va el desplegable del menu 'open' de la barra 'root'
static void menu_geo(int32_t root, int32_t open, int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
    int32_t mx = 4;
    for (int i = 1; i < MAX_GADGETS; i++) {
        gadget_t *g = &gadgets[i];
        if (!g->used || g->type != GADGET_MENU || g->parent_menu != root) continue;
        if (i == open) break;
        mx += (int32_t)wm_ui_text_width(g->text) + 14;
    }
    *x = mx; *y = MENUBAR_H; *w = menu_ancho_desplegable(open); *h = menu_contar(open) * MENU_ROW_H + 4;
}
// Donde va el submenu abierto de la ventana (false si no hay): a la derecha
// del desplegable, a la altura de su entrada. Dibujo, raton y clic usan esta
// misma funcion, para no desfasarse.
static bool menu_geo_sub(int32_t win, int32_t root, int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
    int32_t open = open_menu[win], sub = open_sub[win];
    if (open <= 0 || sub <= 0 || !gadgets[sub].used || gadgets[sub].parent_menu != open) return false;
    int32_t dx, dy, dw, dh;
    menu_geo(root, open, &dx, &dy, &dw, &dh);
    int32_t fila = 0;
    while (fila < 64 && menu_entrada_en(open, fila) != sub) fila++;
    *x = dx + dw - 2; *y = MENUBAR_H + fila * MENU_ROW_H;
    *w = menu_ancho_desplegable(sub); *h = menu_contar(sub) * MENU_ROW_H + 4;
    return menu_contar(sub) > 0;
}
// Las filas de un desplegable (el del menu o el de un submenu), en (x, y)
static void dibujar_filas_menu(int32_t window_idx, int32_t padre, int32_t x, int32_t y, int32_t dw) {
    int32_t row = 0;
    for (int i = 1; i < MAX_GADGETS; i++) {
        gadget_t *g = &gadgets[i];
        if (!g->used || g->type != GADGET_MENU || g->parent_menu != padre) continue;
        uint32_t color = g->enabled ? 0x00000000 : 0x00808080;
        int32_t ry = y + 4 + row * MENU_ROW_H;
        if (menu_es_separador(g->text)) {
            wm_content_fill_rect(window_idx, (uint32_t)(x + 6), (uint32_t)(ry + MENU_ROW_H / 2 - 1), (uint32_t)(dw - 12), 1, 0x00A0A0A0);
        } else {
            if (g->checked) dibujar_marca(window_idx, x + 5, ry + 1, color);
            wm_content_ui_text(window_idx, (uint32_t)(x + MENU_MARCA_W + 4), (uint32_t)ry, g->text, color);
            if (menu_tiene_hijos(i)) {                      // la flecha: tiene submenu
                for (int k = 0; k < 4; k++)
                    wm_content_fill_rect(window_idx, (uint32_t)(x + dw - 12 + k), (uint32_t)(ry + 3 + k), 1, (uint32_t)(8 - 2 * k), color);
            }
        }
        row++;
    }
}
// Una marca de verificacion, dibujada con cuadraditos (no depende de que la
// fuente tenga el caracter)
static void dibujar_marca(int32_t win, int32_t x, int32_t y, uint32_t color) {
    for (int k = 0; k < 3; k++) wm_content_fill_rect(win, (uint32_t)(x + k), (uint32_t)(y + 4 + k), 2, 2, color);
    for (int k = 0; k < 6; k++) wm_content_fill_rect(win, (uint32_t)(x + 3 + k), (uint32_t)(y + 6 - k), 2, 2, color);
}

static void textfield_handle_key(gadget_t *g, char c) {
    uint32_t len = str_len(g->text);
    if (c == '\b') {
        if (len > 0) g->text[len - 1] = '\0';
    } else if (c == '\n') {
        fire_event(g->window_idx, (int32_t)(g - gadgets));
    } else if (c >= 32 && c < 127 && len < GADGET_TEXT_MAX - 1) {
        g->text[len] = c;
        g->text[len + 1] = '\0';
    }
}

void gadgets_update_and_draw(void) {
    if (gadget_count == 0) return; // nadie ha creado gadgets -- nada que hacer

    static bool last_left = false;
    bool left = mouse_left_down();
    bool click_edge = left && !last_left;
    static int32_t dragging_slider = -1; // id del slider que se esta arrastrando, -1 = ninguno
    // En que punto del pomo se agarro, para una barra de desplazamiento. El
    // deslizador no lo usa (-1): ahi el clic SI salta, que es lo que se
    // espera de un control de volumen. En una barra, en cambio, saltar al
    // agarrar el pomo lo desplaza solo con tocarlo, y el texto se va de
    // sitio antes de haber movido el raton.
    static int32_t arrastre_agarre = -1;
    if (!left) { dragging_slider = -1; arrastre_agarre = -1; } // se solto el boton -- termina cualquier arrastre
    last_left = left;

    int32_t focused_win = wm_get_focused_window();

    // -- clic en un desplegable de menu ya abierto: se procesa antes
    // que el resto, porque un clic fuera de el debe cerrarlo. IMPORTANTE:
    // solo "consumimos" el clic (click_edge=false) si cayo DENTRO del
    // desplegable. Si cayo fuera, lo cerramos pero dejamos que ese
    // mismo clic se siga procesando normalmente mas abajo -- si no, un
    // clic en otro menu o en un boton se limitaria a cerrar este
    // desplegable, sin llegar a abrir el otro menu ni pulsar el boton
    // hasta un SEGUNDO clic.
    // Con un menu desplegado: el raton sobre una entrada con
    // submenu lo abre; sobre otra, lo cierra. El clic: en el submenu, elige;
    // en el desplegable, elige (o abre/cierra el submenu de esa entrada);
    // fuera, cierra todo y deja que el mismo clic siga (abrir otro menu, pulsar
    // un boton...). La geometria sale de menu_geo / menu_geo_sub, las mismas
    // que usa el dibujo.
    // ---- El menu flotante abierto se lleva el clic antes que nadie ----
    //
    // Va PRIMERO, antes del desplegable de la barra y antes de los controles:
    // esta dibujado encima de todos ellos, asi que un clic dentro de el es
    // suyo. Si se mirara despues, el boton que hay debajo se lo quedaria y se
    // pulsarian las dos cosas con un solo clic.
    if (focused_win >= 0 && focused_win < MAX_WM_WINDOWS && open_context[focused_win] > 0) {
        int32_t raiz = open_context[focused_win];
        int32_t wx, wy; uint32_t ww, wh;
        if (wm_get_window_client_rect(focused_win, &wx, &wy, &ww, &wh)) {
            (void)ww; (void)wh;
            int32_t lx = mouse_x() - wx;
            int32_t ly = mouse_y() - wy - (int32_t)gadgets_menubar_height(focused_win);
            int32_t x = ctx_x[focused_win], y = ctx_y[focused_win];
            int32_t dw = menu_ancho_desplegable(raiz);
            int32_t dh = menu_contar(raiz) * MENU_ROW_H + 4;
            bool dentro = lx >= x && lx < x + dw && ly >= y && ly < y + dh;
            if (click_edge) {
                if (dentro) {
                    int32_t e = menu_entrada_en(raiz, (ly - y - 2) / MENU_ROW_H);
                    if (e > 0 && gadgets[e].enabled && !menu_es_separador(gadgets[e].text)) {
                        fire_menu_event(focused_win, e);
                        open_context[focused_win] = -1;
                    }
                    click_edge = false;   // el clic era PARA el menu -- consumido
                } else {
                    // Fuera: se cierra, y el clic sigue su camino. Pulsar
                    // "fuera del menu" es pulsar lo que haya ahi, no perder
                    // el clic: eso obliga a pulsar dos veces y se nota.
                    open_context[focused_win] = -1;
                }
                wm_request_redraw();
            }
        }
    }

    if (focused_win >= 0 && focused_win < MAX_WM_WINDOWS && open_menu[focused_win] > 0) {
        int32_t open = open_menu[focused_win];
        int32_t root = window_menu_root_set[focused_win] ? window_menu_root[focused_win] : -1;
        int32_t wx, wy; uint32_t ww, wh;
        if (wm_get_window_client_rect(focused_win, &wx, &wy, &ww, &wh)) {
            (void)ww; (void)wh;
            int32_t lx = mouse_x() - wx;
            int32_t ly = mouse_y() - wy;
            int32_t dx, dy, dw, dh, sx = 0, sy = 0, sw = 0, sh = 0;
            menu_geo(root, open, &dx, &dy, &dw, &dh);
            bool hay_sub = menu_geo_sub(focused_win, root, &sx, &sy, &sw, &sh);
            bool en_sub = hay_sub && lx >= sx && lx < sx + sw && ly >= sy && ly < sy + sh;
            bool en_dd = lx >= dx && lx < dx + dw && ly >= MENUBAR_H && ly < MENUBAR_H + dh;
            int32_t fila_dd = en_dd ? (ly - MENUBAR_H - 2) / MENU_ROW_H : -1;
            int32_t entrada_dd = en_dd ? menu_entrada_en(open, fila_dd) : -1;
            // el raton: abrir o cerrar el submenu (sin clic)
            if (!en_sub && entrada_dd > 0) {
                open_sub[focused_win] = (menu_tiene_hijos(entrada_dd) && gadgets[entrada_dd].enabled) ? entrada_dd : -1;
            }
            if (click_edge) {
                if (en_sub) {
                    int32_t e = menu_entrada_en(open_sub[focused_win], (ly - sy - 2) / MENU_ROW_H);
                    if (e > 0 && gadgets[e].enabled && !menu_es_separador(gadgets[e].text) && !menu_tiene_hijos(e)) {
                        fire_menu_event(focused_win, e);
                        open_menu[focused_win] = -1; open_sub[focused_win] = -1;
                    }
                    click_edge = false;
                } else if (en_dd) {
                    gadget_t *g = (entrada_dd > 0) ? &gadgets[entrada_dd] : NULL;
                    if (g && g->enabled && !menu_es_separador(g->text)) {
                        if (menu_tiene_hijos(entrada_dd)) {
                            open_sub[focused_win] = entrada_dd;              // el clic tambien lo abre
                        } else {
                            fire_menu_event(focused_win, entrada_dd);
                            open_menu[focused_win] = -1; open_sub[focused_win] = -1;
                        }
                    }
                    click_edge = false; // el clic era PARA el desplegable -- consumido
                } else {
                    open_menu[focused_win] = -1; open_sub[focused_win] = -1; // fuera: se cierra...
                    // ...pero click_edge se queda tal cual, para que este mismo
                    // clic se procese normalmente a continuacion (abrir otro
                    // menu, pulsar un boton, lo que corresponda).
                }
            }
        }
    }

    for (int i = 1; i < MAX_GADGETS; i++) {
        gadget_t *g = &gadgets[i];
        if (!g->used) continue;
        int32_t win = g->window_idx;
        if (win < 0) continue;

        if (g->type == GADGET_MENU_ROOT) {
            draw_menubar(win, i);      // la barra (y borra el desplegable anterior); el abierto, al final

            if (click_edge && win == focused_win) {
                int32_t wx, wy; uint32_t ww, wh;
                if (wm_get_window_client_rect(win, &wx, &wy, &ww, &wh)) {
                    (void)ww; (void)wh;
                    int32_t lx = mouse_x() - wx;
                    int32_t ly = mouse_y() - wy;
                    if (ly >= 0 && ly < MENUBAR_H) {
                        int32_t mx = 4;
                        for (int k = 1; k < MAX_GADGETS; k++) {
                            gadget_t *m = &gadgets[k];
                            if (!m->used || m->type != GADGET_MENU || m->parent_menu != i) continue;
                            int32_t mw = (int32_t)wm_ui_text_width(m->text) + 14;
                            if (lx >= mx && lx < mx + mw) {
                                open_menu[win] = (open_menu[win] == k) ? -1 : k;
                                break;
                            }
                            mx += mw;
                        }
                    }
                }
            }
            continue;
        }
        if (g->type == GADGET_MENU) continue; // se dibuja como parte de draw_menubar
        // La raiz de un menu flotante no es un control: no ocupa sitio en la
        // ventana y no se dibuja. Solo existe para colgarle las entradas.
        if (g->type == GADGET_CONTEXT_ROOT) continue;

        if (!g->visible) continue;

        int32_t top_offset = (int32_t)gadgets_menubar_height(win);

        if (g->type == GADGET_BUTTON) draw_button(g, win, 0, top_offset);
        else if (g->type == GADGET_PANEL) draw_panel(g, win, 0, top_offset);
        else if (g->type == GADGET_TEXTFIELD) draw_textfield(g, win, 0, top_offset);
        else if (g->type == GADGET_LISTBOX) draw_listbox(g, win, 0, top_offset);
        else if (g->type == GADGET_TEXTAREA) draw_textarea(g, win, 0, top_offset);
        else if (g->type == GADGET_LABEL) draw_label(g, win, 0, top_offset);
        else if (g->type == GADGET_PROGBAR) draw_progbar(g, win, 0, top_offset);
        else if (g->type == GADGET_SLIDER) draw_slider(g, win, 0, top_offset);
        else if (g->type == GADGET_SCROLLBAR) draw_scrollbar(g, win, 0, top_offset);
        else if (g->type == GADGET_COMBOBOX) draw_combobox(g, win, 0, top_offset);
        else if (g->type == GADGET_TABBER) draw_tabber(g, win, 0, top_offset);
        else if (g->type == GADGET_TOOLBAR) draw_toolbar(g, win, 0, top_offset);
        else if (g->type == GADGET_TREEVIEW) draw_treeview(g, win, 0, top_offset);
        // GADGET_CANVAS: sin dibujo propio a proposito -- si lo
        // redibujaramos como un panel en CADA vuelta, borraria lo que
        // el programa ya haya dibujado encima via CanvasBuffer +
        // SetBuffer (ese dibujo va DIRECTO al contenido de la
        // ventana, y debe quedar intacto entre vueltas).

        if (click_edge && win == focused_win && g->enabled) {
            int32_t wx, wy; uint32_t ww, wh;
            if (wm_get_window_client_rect(win, &wx, &wy, &ww, &wh)) {
                (void)ww; (void)wh;
                int32_t lx = mouse_x() - wx;
                int32_t ly = mouse_y() - wy - top_offset;

                if (lx >= g->x && lx < g->x + (int32_t)g->w && ly >= g->y && ly < g->y + (int32_t)g->h) {
                    if (g->type == GADGET_BUTTON) {
                        if (g->style == 2) {
                            // Casilla -- alterna su propio estado
                            g->checked = !g->checked;
                        } else if (g->style == 3) {
                            // Opcion -- se marca y apaga a sus companeras
                            // (las del mismo grupo: ver la nota de
                            // apagar_las_demas_del_grupo).
                            apagar_las_demas_del_grupo(i);
                            g->checked = true;
                        }
                        fire_event(win, i);
                    } else if (g->type == GADGET_TEXTFIELD) {
                        gadget_activate(i);
                    } else if (g->type == GADGET_TEXTAREA) {
                        // el foco del teclado, y el cursor donde se pulso
                        for (int k = 1; k < MAX_GADGETS; k++)
                            if (gadgets[k].used && gadgets[k].window_idx == win &&
                                (gadgets[k].type == GADGET_TEXTFIELD || gadgets[k].type == GADGET_TEXTAREA))
                                gadgets[k].focused = false;
                        g->focused = true;
                        char (*lineas)[TEXTAREA_LINE_MAX] = ta_lineas(g);
                        if (lineas) {
                            uint32_t max_rows = g->h / ROW_H; if (max_rows == 0) max_rows = 1;
                            uint32_t primera = g->ta_scroll;
                            int32_t fila = (ly - g->y - 2) / (int32_t)ROW_H;
                            if (fila < 0) fila = 0;
                            uint32_t destino = primera + (uint32_t)fila;
                            if (destino >= g->ta_line_count) destino = g->ta_line_count ? g->ta_line_count - 1 : 0;
                            g->ta_cur_line = destino;
                            // la columna: la letra mas cercana al clic
                            char antes[TEXTAREA_LINE_MAX];
                            uint32_t len = str_len(lineas[destino]), col = len;
                            for (uint32_t n = 0; n <= len && n < TEXTAREA_LINE_MAX - 1; n++) {
                                for (uint32_t b = 0; b < n; b++) antes[b] = lineas[destino][b];
                                antes[n] = '\0';
                                if (g->x + 4 + (int32_t)wm_ui_text_width(antes) >= lx) { col = n ? n - 1 : 0; break; }
                            }
                            g->ta_cur_col = col;
                        }
                    } else if (g->type == GADGET_LISTBOX) {
                        int32_t row = (ly - g->y - 2) / ROW_H;
                        if (row >= 0 && (uint32_t)row < g->item_count) {
                            g->selected = row;
                            fire_event(win, i);
                        }
                    } else if (g->type == GADGET_SLIDER) {
                        // Arranca el arrastre -- el clic ya mueve el
                        // pomo (no hace falta agarrarlo con precision).
                        dragging_slider = i;
                        int32_t old_value = g->slider_value;
                        int32_t local = (g->style == 2) ? (ly - g->y) : (lx - g->x);
                        slider_set_value_from_local(g, local);
                        if (g->slider_value != old_value) fire_event(win, i);
                    } else if (g->type == GADGET_SCROLLBAR) {
                        // Tres zonas, y cada una hace una cosa distinta.
                        // Esto es lo que separa una barra de un
                        // deslizador: sin las flechas y el salto de
                        // pagina, recorrer un texto largo obliga a
                        // arrastrar el pomo a ojo, y un renglon exacto
                        // no hay forma de alcanzarlo.
                        int32_t largo = (g->style == 2) ? (int32_t)g->h : (int32_t)g->w;
                        int32_t local = (g->style == 2) ? (ly - g->y) : (lx - g->x);
                        int32_t flecha = flecha_lado(g);
                        int32_t thumb_pos, thumb_len;
                        slider_thumb_geom(g, &thumb_pos, &thumb_len);
                        int32_t pagina = g->slider_visible > 1 ? g->slider_visible - 1 : 1;
                        int32_t antes = g->slider_value;
                        if (flecha > 0 && local < flecha) {
                            gadget_set_slider_value(i, antes - 1);
                        } else if (flecha > 0 && local >= largo - flecha) {
                            gadget_set_slider_value(i, antes + 1);
                        } else if (local < thumb_pos) {
                            gadget_set_slider_value(i, antes - pagina);
                        } else if (local >= thumb_pos + thumb_len) {
                            gadget_set_slider_value(i, antes + pagina);
                        } else {
                            // Sobre el pomo: se agarra por donde se pulso,
                            // y NO se mueve de golpe. Saltar al pulsar
                            // encima haria que agarrarlo lo desplazara solo.
                            dragging_slider = i;
                            arrastre_agarre = local - thumb_pos;
                        }
                        if (g->slider_value != antes) fire_event(win, i);
                    } else if (g->type == GADGET_COMBOBOX) {
                        // Clic en la caja cerrada -- abre o cierra el desplegable
                        open_combobox[win] = (open_combobox[win] == i) ? -1 : i;
                    } else if (g->type == GADGET_TABBER) {
                        // Solo reacciona dentro de la franja de
                        // pestañas -- el resto del area es contenido
                        // que gestiona el propio programa (paneles
                        // que el mismo muestra/oculta).
                        if (ly - g->y < TABBER_TAB_H) {
                            int32_t tx = g->x;
                            for (uint32_t k = 0; k < g->item_count; k++) {
                                int32_t tw = (int32_t)wm_ui_text_width(g->items[k]) + 14;
                                if (lx >= tx && lx < tx + tw) {
                                    if ((int32_t)k != g->selected) {
                                        g->selected = (int32_t)k;
                                        fire_event(win, i);
                                    }
                                    break;
                                }
                                tx += tw;
                            }
                        }
                    } else if (g->type == GADGET_TOOLBAR) {
                        uint32_t iw, ih;
                        if (image_get_info(g->icon_strip, &iw, &ih, NULL) && ih > 0) {
                            (void)iw;
                            int32_t idx = (lx - g->x) / (int32_t)ih;
                            if (idx >= 0 && (uint32_t)idx < g->item_count && g->items_enabled[idx]) {
                                fire_event_data(win, i, idx);
                            }
                        }
                    } else if (g->type == GADGET_TREEVIEW) {
                        int32_t row_clicked = (ly - g->y - 2) / ROW_H;
                        int32_t root = -1;
                        for (int k = 1; k < MAX_GADGETS; k++) {
                            if (gadgets[k].used && gadgets[k].type == GADGET_TREENODE &&
                                gadgets[k].tree_owner == i && gadgets[k].parent_menu == -1) { root = k; break; }
                        }
                        if (root >= 0 && row_clicked >= 0) {
                            uint32_t row_counter = 0;
                            int32_t clicked_node = treeview_node_at_row(root, row_clicked, &row_counter);
                            if (clicked_node >= 0) {
                                // clic en cualquier parte de la fila:
                                // selecciona y dispara evento; si
                                // tiene hijos, tambien alterna
                                // expandido/colapsado (interaccion
                                // combinada, mas simple de un solo
                                // clic que distinguir el glifo +/-
                                // con precision de pixel).
                                g->selected = clicked_node;
                                fire_event(win, i);
                                if (gadget_count_treeview_nodes(clicked_node) > 0) {
                                    gadgets[clicked_node].checked = !gadgets[clicked_node].checked;
                                }
                            }
                        }
                    }
                }

                // ComboBox: el desplegable abierto se extiende POR
                // DEBAJO de la caja, fuera del rectangulo estandar de
                // arriba -- se comprueba aparte, con las mismas lx/ly.
                if (g->type == GADGET_COMBOBOX && open_combobox[win] == i) {
                    bool hit_box = (lx >= g->x && lx < g->x + (int32_t)g->w && ly >= g->y && ly < g->y + (int32_t)g->h);
                    int32_t dd_y = g->y + (int32_t)g->h;
                    int32_t dd_h = (int32_t)(g->item_count * ROW_H + 4);
                    bool hit_dropdown = (lx >= g->x && lx < g->x + (int32_t)g->w && ly >= dd_y && ly < dd_y + dd_h);
                    if (hit_dropdown) {
                        int32_t row = (ly - dd_y - 2) / ROW_H;
                        if (row >= 0 && (uint32_t)row < g->item_count) {
                            g->selected = row;
                            fire_event(win, i);
                        }
                        open_combobox[win] = -1;
                    } else if (!hit_box) {
                        // clic fuera de la caja Y del desplegable -- cierra sin elegir
                        open_combobox[win] = -1;
                    }
                }
            }
        }

        // Continuacion del arrastre de un slider -- se ejecuta en
        // CADA vuelta mientras el boton siga pulsado, no solo en el
        // flanco de bajada (a diferencia del resto de gadgets, que
        // solo reaccionan al clic).
        if (dragging_slider == i && left && win == focused_win) {
            int32_t wx, wy; uint32_t ww, wh;
            if (wm_get_window_client_rect(win, &wx, &wy, &ww, &wh)) {
                (void)ww; (void)wh;
                int32_t lx = mouse_x() - wx;
                int32_t ly = mouse_y() - wy - top_offset;
                int32_t old_value = g->slider_value;
                int32_t local = (g->style == 2) ? (ly - g->y) : (lx - g->x);
                if (arrastre_agarre >= 0) {
                    // Se agarro por un punto concreto del pomo, y ese punto
                    // tiene que seguir bajo el raton. slider_set_value_from_local
                    // coloca el CENTRO del pomo donde se le diga, asi que se le
                    // pide el centro que corresponde a este agarre.
                    int32_t thumb_pos, thumb_len;
                    slider_thumb_geom(g, &thumb_pos, &thumb_len);
                    local = local - arrastre_agarre + thumb_len / 2;
                }
                slider_set_value_from_local(g, local);
                if (g->slider_value != old_value) fire_event(win, i);
            }
        }
    }

    // Lo que va ENCIMA de todos los controles, una vez por vuelta
    // y ya fuera del bucle de controles (antes estaba dentro, por error, y se
    // repintaba una vez por cada control).
    // ---- Los desplegables de menu abiertos, encima de todos los controles ----
    for (int i = 1; i < MAX_GADGETS; i++) {
        if (gadgets[i].used && gadgets[i].type == GADGET_MENU_ROOT && gadgets[i].window_idx >= 0)
            draw_menu_desplegable(gadgets[i].window_idx, i);
    }

    // ---- Y los menus flotantes, encima de todo, incluido el desplegable ----
    //
    // Una vez por VENTANA, no una por gadget: un programa puede tener varios
    // menus flotantes creados (uno para la lista, otro para el lienzo) y solo
    // uno abierto. Recorrer los gadgets dibujaria --y peor, BORRARIA-- una vez
    // por cada uno, y el borrado del segundo destaparia lo que acaba de pintar
    // el primero.
    for (int w = 0; w < MAX_WM_WINDOWS; w++) {
        if (open_context[w] > 0 || last_ctx_w[w] > 0) draw_menu_contextual(w);
    }

    // ---- Texto de ayuda de la barra de herramientas ----
    {
        int32_t hg = -1, hb = -1, hx = 0, hy = 0;
        if (focused_win >= 0 && focused_win < MAX_WM_WINDOWS && open_menu[focused_win] <= 0 && !left) {
            int32_t wx, wy; uint32_t ww, wh;
            if (wm_get_window_client_rect(focused_win, &wx, &wy, &ww, &wh)) {
                int32_t lx = mouse_x() - wx, ly = mouse_y() - wy - (int32_t)gadgets_menubar_height(focused_win);
                for (int i = 1; i < MAX_GADGETS && hg < 0; i++) {
                    gadget_t *g = &gadgets[i];
                    if (!g->used || !g->visible || g->type != GADGET_TOOLBAR || g->window_idx != focused_win) continue;
                    uint32_t iw, ih;
                    if (!image_get_info(g->icon_strip, &iw, &ih, NULL) || ih == 0) continue;
                    if (ly >= g->y && ly < g->y + (int32_t)ih && lx >= g->x && lx < g->x + (int32_t)(g->item_count * ih)) {
                        int32_t b = (lx - g->x) / (int32_t)ih;
                        // anclado BAJO SU BOTON, no donde este el raton:
                        // si seguia al raton, cada pequeño movimiento lo volvia a
                        // pintar un poco mas alla y al borrarse quedaba el rastro
                        if (b >= 0 && b < LISTBOX_MAX_ITEMS && g->items[b][0]) {
                            hg = i; hb = b;
                            hx = g->x + b * (int32_t)ih;
                            hy = g->y + (int32_t)ih + 4 + (int32_t)gadgets_menubar_height(focused_win);
                        }
                    }
                }
            }
        }
        uint64_t ahora = timer_get_ticks();
        if (hg != tip_gadget || hb != tip_boton || click_edge) {
            if (tip_visible && tip_win >= 0) wm_content_fill_rect(tip_win, (uint32_t)tip_x, (uint32_t)tip_y, tip_w, tip_h, 0x00D4D0C8);
            tip_visible = false;
            tip_gadget = click_edge ? -1 : hg; tip_boton = click_edge ? -1 : hb; tip_win = focused_win; tip_desde = ahora;
        } else if (hg >= 0 && ahora - tip_desde >= TIP_ESPERA) {
            const char *t = gadgets[hg].items[hb];
            int32_t wx, wy; uint32_t ww, wh;
            if (wm_get_window_client_rect(focused_win, &wx, &wy, &ww, &wh)) {
                tip_w = wm_ui_text_width(t) + 10; tip_h = 20;
                tip_x = hx; tip_y = hy;
                if (tip_x + (int32_t)tip_w > (int32_t)ww) tip_x = (int32_t)ww - (int32_t)tip_w - 2;
                if (tip_x < 0) tip_x = 0;
                wm_content_fill_rect(focused_win, (uint32_t)tip_x, (uint32_t)tip_y, tip_w, tip_h, 0x00505050);
                wm_content_fill_rect(focused_win, (uint32_t)tip_x + 1, (uint32_t)tip_y + 1, tip_w - 2, tip_h - 2, 0x00FFFFE0);
                wm_content_ui_text(focused_win, (uint32_t)tip_x + 5, (uint32_t)tip_y + 3, t, 0x00202020);
                tip_visible = true; tip_win = focused_win;
            }
        }
    }

    // Teclado -- SOLO si hay un TextField con el foco en la ventana
    // activa. Si no lo hay, dejamos el caracter en la cola tal cual,
    // para que el programa (shell, editor...) lo lea el mismo con
    // SYS_READ_CHAR -- de lo contrario, esta funcion robaria teclas a
    // CUALQUIER programa que corra a la vez, use gadgets o no.
    if (focused_win >= 0) {
        int32_t focused_field = -1;
        for (int i = 1; i < MAX_GADGETS; i++) {
            gadget_t *g = &gadgets[i];
            if (g->used && g->window_idx == focused_win && g->focused &&
                (g->type == GADGET_TEXTFIELD || g->type == GADGET_TEXTAREA)) {
                focused_field = i;
                break;
            }
        }
        if (focused_field >= 0) {
            char c;
            if (input_read_char(&c)) {
                // la caja de texto tambien se escribe
                if (gadgets[focused_field].type == GADGET_TEXTAREA) textarea_handle_key(&gadgets[focused_field], c);
                else textfield_handle_key(&gadgets[focused_field], c);
            }
        }
    }

    // ---- Pedir el redibujado SOLO si algo ha cambiado ----
    //
    // BUG REAL CORREGIDO (el raton iba peor cuantos mas programas hubiera
    // abiertos). Esto terminaba con un wm_request_redraw() INCONDICIONAL, asi
    // que cualquier programa con gadgets -- y un Canvas es un gadget, con lo
    // que tambien el monitor del sistema -- forzaba una composicion ENTERA de
    // la pantalla en cada vuelta, hubiera cambiado algo o no. Componer es el
    // fondo, todas las ventanas abiertas, la barra de tareas y la copia de la
    // pantalla completa: con dos o tres ventanas, redibujar el escritorio
    // entero decenas de veces por segundo para nada.
    //
    // Lo que se dibuja en el CONTENIDO de una ventana ya pide el redibujado por
    // su cuenta: content_put_pixel marca la ventana como cambiada y
    // wm_publicar_contenido levanta needs_redraw. Aqui solo hay que vigilar lo
    // que NO pasa por ahi, que es el estado que pinta la composicion: los menus
    // desplegados, los submenus y la lista de un ComboBox.
    //
    // Con algo de eso abierto entra tambien la posicion del raton, porque al
    // pasar por encima se resalta la entrada y se abren y cierran submenus. Es
    // el caso raro y dura lo que dura un menu abierto.
    {
        static uint64_t huella_previa = 0;
        uint64_t huella = 0;
        bool algo_desplegado = false;
        for (int i = 0; i < MAX_WM_WINDOWS; i++) {
            huella = huella * 31u + (uint64_t)(uint32_t)(open_menu[i] + 2);
            huella = huella * 31u + (uint64_t)(uint32_t)(open_sub[i] + 2);
            huella = huella * 31u + (uint64_t)(uint32_t)(open_combobox[i] + 2);
            if (open_menu[i] >= 0 || open_sub[i] >= 0 || open_combobox[i] >= 0) algo_desplegado = true;
        }
        if (algo_desplegado) {
            huella = huella * 31u + (uint64_t)(uint32_t)mouse_x();
            huella = huella * 31u + (uint64_t)(uint32_t)mouse_y();
        }
        if (huella != huella_previa) {
            huella_previa = huella;
            wm_request_redraw();
        }
    }
}
