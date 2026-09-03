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
#include <stddef.h> // NULL

#define MAX_GADGETS 64
#define GADGET_TEXT_MAX 48
#define LISTBOX_MAX_ITEMS 24
#define TEXTAREA_MAX_LINES 40
#define MAX_WM_WINDOWS 8 // debe coincidir con MAX_WINDOWS de wm.c

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
    // GadgetGroup: registra el handle de "grupo" pasado al crear el
    // gadget (parametro 'group' de CreateButton/etc, documentado
    // oficialmente como "A group gadget handle"). NO afecta al
    // renderizado ni al anidado real (seguimos con el modelo de una
    // sola ventana por programa) -- es puramente informativo, para
    // que GadgetGroup() pueda devolver lo que el programa registro.
    // 0 = sin grupo asignado (por defecto).
    int32_t parent_group;

    // ListBox
    char items[LISTBOX_MAX_ITEMS][GADGET_TEXT_MAX];
    uint32_t item_count;
    int32_t selected;

    // TextArea
    char ta_lines[TEXTAREA_MAX_LINES][GADGET_TEXT_MAX];
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
static int32_t pending_event_id[MAX_WM_WINDOWS];     // 0 = ninguno pendiente
static int32_t pending_event_source[MAX_WM_WINDOWS];
static int32_t pending_event_data[MAX_WM_WINDOWS];
static int32_t last_event_source[MAX_WM_WINDOWS];    // del ULTIMO evento consultado (para EventSource/EventData)
static int32_t last_event_data[MAX_WM_WINDOWS];
static int32_t open_menu[MAX_WM_WINDOWS]; // id de la entrada de menu desplegada, -1 si ninguna
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

// Recordamos donde se dibujo el ULTIMO desplegable de cada ventana,
// para poder borrarlo la proxima vez que cambie o se cierre -- si no,
// sus pixeles se quedan "pegados" en pantalla porque nadie los vuelve
// a tocar (0 ancho = no habia ninguno la ultima vez).
static int32_t last_dd_x[MAX_WM_WINDOWS], last_dd_y[MAX_WM_WINDOWS];
static int32_t last_dd_w[MAX_WM_WINDOWS], last_dd_h[MAX_WM_WINDOWS];

static uint32_t str_len(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

static void set_text(char *dst, const char *src) {
    uint32_t i = 0;
    while (src[i] && i < GADGET_TEXT_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void gadgets_init(void) {
    for (int i = 0; i < MAX_GADGETS; i++) gadgets[i].used = false;
    for (int i = 0; i < MAX_WM_WINDOWS; i++) {
        pending_event_id[i] = 0;
        pending_event_source[i] = 0;
        pending_event_data[i] = 0;
        last_event_source[i] = 0;
        last_event_data[i] = 0;
        open_menu[i] = -1;
        open_combobox[i] = -1;
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
    return create_common(GADGET_TEXTAREA, x, y, w, h, window_idx);
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
void gadget_set_slider_range(int32_t id, int32_t visible, int32_t total) {
    if (!valid(id) || gadgets[id].type != GADGET_SLIDER) return;
    if (visible < 1) visible = 1;
    if (total < visible) total = visible;
    gadgets[id].slider_visible = visible;
    gadgets[id].slider_total = total;
    int32_t maxval = total - visible;
    if (gadgets[id].slider_value > maxval) gadgets[id].slider_value = maxval;
}
void gadget_set_slider_value(int32_t id, int32_t value) {
    if (!valid(id) || gadgets[id].type != GADGET_SLIDER) return;
    int32_t maxval = gadgets[id].slider_total - gadgets[id].slider_visible;
    if (value < 0) value = 0;
    if (value > maxval) value = maxval;
    gadgets[id].slider_value = value;
}
int32_t gadget_slider_value(int32_t id) {
    if (!valid(id) || gadgets[id].type != GADGET_SLIDER) return 0;
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
// GadgetGroup -- ver la nota junto al campo 'parent_group' en la
// definicion de gadget_t: es puramente informativo (registra el
// handle pasado como 'group' al crear el gadget), sin afectar al
// renderizado.
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
    if (!valid(id) || gadgets[id].type != GADGET_TOOLBAR) return;
    set_text(gadgets[id].text, tips);
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
    open_combobox[window_idx] = -1;
    pending_event_id[window_idx] = 0;
    pending_event_source[window_idx] = 0;
    pending_event_data[window_idx] = 0;
    last_dd_w[window_idx] = 0;
    timer_active[window_idx] = false;
}

void gadget_set_text(int32_t id, const char *text) {
    if (!valid(id)) return;
    set_text(gadgets[id].text, text);
}

uint32_t gadget_get_text(int32_t id, char *out, uint32_t max_len) {
    if (!valid(id) || max_len == 0) { if (max_len) out[0] = '\0'; return 0; }
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
void gadget_move(int32_t id, int32_t x, int32_t y) {
    if (!valid(id)) return;
    gadgets[id].x = x; gadgets[id].y = y;
}
void gadget_resize(int32_t id, uint32_t w, uint32_t h) {
    if (!valid(id)) return;
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

void gadgets_fire_raw_event(int32_t window_idx, int32_t event_id, int32_t source, int32_t data) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return;
    pending_event_id[window_idx] = event_id;
    pending_event_source[window_idx] = source;
    pending_event_data[window_idx] = data;
}

int32_t gadgets_poll_raw_event(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return 0;
    int32_t id = pending_event_id[window_idx];
    if (id == 0) return 0;
    pending_event_id[window_idx] = 0;
    last_event_source[window_idx] = pending_event_source[window_idx];
    last_event_data[window_idx] = pending_event_data[window_idx];
    return id;
}

// PeekEvent(): igual que gadgets_poll_raw_event, pero SIN
// consumirlo -- no lo saca de la cola, no actualiza
// last_event_source/data (asi EventID/EventData siguen dando lo del
// ULTIMO evento consumido de verdad, tal como documenta BlitzPlus
// real: "PeekEvent does not update the other event functions").
int32_t gadgets_peek_raw_event(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return 0;
    return pending_event_id[window_idx];
}

// FlushEvents([id]): descarta el evento pendiente -- si se da un id
// concreto, solo lo descarta si coincide; con id=0 (omitido) lo
// descarta sin mirar cual sea (nuestro modelo solo guarda UN evento
// pendiente a la vez, no una cola de verdad con varios).
void gadgets_flush_events(int32_t window_idx, int32_t filter_id) {
    if (window_idx < 0 || window_idx >= MAX_WM_WINDOWS) return;
    if (filter_id == 0 || pending_event_id[window_idx] == filter_id) {
        pending_event_id[window_idx] = 0;
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
    gadgets_fire_raw_event(window_idx, EVENT_GADGETACTION, gadget_id, 0);
}
// Igual que fire_event, pero con un dato explicito para EventData()
// -- lo usa ToolBar, donde EventData() debe dar el INDICE del boton
// pulsado (confirmado en la documentacion oficial de CreateToolBar).
static void fire_event_data(int32_t window_idx, int32_t gadget_id, int32_t data) {
    gadgets_fire_raw_event(window_idx, EVENT_GADGETACTION, gadget_id, data);
}

static void fire_menu_event(int32_t window_idx, int32_t menu_id) {
    int32_t tag = valid(menu_id) ? gadgets[menu_id].tag : 0;
    gadgets_fire_raw_event(window_idx, EVENT_MENUACTION, menu_id, tag);
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
        gadgets_fire_raw_event(i, EVENT_TIMERTICK, i, 0);
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
            if (win >= 0) gadgets_fire_raw_event(win, hotkeys[i].event_id, hotkeys[i].event_source, hotkeys[i].event_data);
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
void gadget_textarea_set_text(int32_t id, const char *text) {
    if (!valid(id) || gadgets[id].type != GADGET_TEXTAREA) return;
    gadget_t *g = &gadgets[id];
    g->ta_line_count = 0;
    uint32_t col = 0;
    uint32_t i = 0;
    while (text[i] != '\0' && g->ta_line_count < TEXTAREA_MAX_LINES) {
        if (text[i] == '\n') {
            g->ta_lines[g->ta_line_count][col] = '\0';
            g->ta_line_count++;
            col = 0;
        } else if (col < GADGET_TEXT_MAX - 1) {
            g->ta_lines[g->ta_line_count][col] = text[i];
            col++;
        }
        i++;
    }
    if (g->ta_line_count < TEXTAREA_MAX_LINES) {
        g->ta_lines[g->ta_line_count][col] = '\0';
        g->ta_line_count++;
    }
}

// Añade texto al FINAL del contenido -- a diferencia de
// SetTextAreaText (que reemplaza todo), esto respeta lo que ya
// hubiera. Si el texto nuevo contiene '\n', puede generar varias
// lineas nuevas.
void gadget_textarea_add_text(int32_t id, const char *text) {
    if (!valid(id) || gadgets[id].type != GADGET_TEXTAREA) return;
    gadget_t *g = &gadgets[id];
    if (g->ta_line_count == 0) g->ta_line_count = 1; // empezamos con una linea vacia si no habia nada
    uint32_t line = g->ta_line_count - 1;
    uint32_t col = str_len(g->ta_lines[line]);
    uint32_t i = 0;
    while (text[i] != '\0') {
        if (text[i] == '\n') {
            g->ta_lines[line][col] = '\0';
            if (g->ta_line_count >= TEXTAREA_MAX_LINES) break; // sin espacio para mas lineas
            line = g->ta_line_count;
            g->ta_line_count++;
            col = 0;
            g->ta_lines[line][0] = '\0';
        } else if (col < GADGET_TEXT_MAX - 1) {
            g->ta_lines[line][col] = text[i];
            col++;
        }
        i++;
    }
    g->ta_lines[line][col] = '\0';
}

// TextAreaLen(textarea[,units]) -- 1=caracteres (por defecto), 2=lineas
uint32_t gadget_textarea_len(int32_t id, int32_t units) {
    if (!valid(id) || gadgets[id].type != GADGET_TEXTAREA) return 0;
    gadget_t *g = &gadgets[id];
    if (units == 2) return g->ta_line_count;
    uint32_t total = 0;
    for (uint32_t i = 0; i < g->ta_line_count; i++) {
        total += str_len(g->ta_lines[i]);
        if (i + 1 < g->ta_line_count) total += 1; // el '\n' entre lineas
    }
    return total;
}

// TextAreaLineLen(textarea,line) -- longitud de una linea concreta
uint32_t gadget_textarea_line_len(int32_t id, int32_t line) {
    if (!valid(id) || gadgets[id].type != GADGET_TEXTAREA) return 0;
    gadget_t *g = &gadgets[id];
    if (line < 0 || (uint32_t)line >= g->ta_line_count) return 0;
    return str_len(g->ta_lines[line]);
}

// TextAreaLine(textarea,char) -- que linea contiene el caracter dado
// (indice de caracter sobre el texto plano, contando los '\n' entre
// lineas como parte del recuento).
int32_t gadget_textarea_line_of_char(int32_t id, int32_t charpos) {
    if (!valid(id) || gadgets[id].type != GADGET_TEXTAREA) return 0;
    gadget_t *g = &gadgets[id];
    if (g->ta_line_count == 0) return 0;
    int32_t pos = 0;
    for (uint32_t i = 0; i < g->ta_line_count; i++) {
        int32_t len = (int32_t)str_len(g->ta_lines[i]);
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
    if (max_len > 0) out[0] = '\0';
    if (!valid(id) || gadgets[id].type != GADGET_TEXTAREA || max_len == 0) return;
    gadget_t *g = &gadgets[id];
    // Construimos el texto plano completo en un buffer temporal --
    // TEXTAREA_MAX_LINES*GADGET_TEXT_MAX es pequeño (40*48=1920), cabe
    // de sobra en la pila del kernel.
    char full[TEXTAREA_MAX_LINES * GADGET_TEXT_MAX];
    uint32_t flen = 0;
    for (uint32_t i = 0; i < g->ta_line_count; i++) {
        uint32_t ll = str_len(g->ta_lines[i]);
        for (uint32_t j = 0; j < ll && flen < sizeof(full) - 1; j++) full[flen++] = g->ta_lines[i][j];
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
void gadget_set_button_state(int32_t id, bool state) {
    if (!valid(id) || gadgets[id].type != GADGET_BUTTON) return;
    if (gadgets[id].style == 3 && state) {
        // Radio marcado a mano -- misma exclusion mutua que al hacer
        // clic, para no dejar dos radios del mismo grupo marcados.
        for (int k = 0; k < MAX_GADGETS; k++) {
            gadget_t *other = &gadgets[k];
            if (other->used && other->type == GADGET_BUTTON && other->style == 3 &&
                other->window_idx == gadgets[id].window_idx) {
                other->checked = false;
            }
        }
    }
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
        uint32_t tcolor = g->enabled ? 0x00E0E0E0 : 0x00808080;
        wm_content_draw_string(win, (uint32_t)(g->x + ox + 18), (uint32_t)(g->y + oy + (int32_t)g->h / 2 - 3), g->text, tcolor, 1);
        return;
    }
    uint32_t bg = g->enabled ? 0x00505860 : 0x00303840;
    wm_content_fill_rect(win, (uint32_t)(g->x + ox), (uint32_t)(g->y + oy), g->w, g->h, bg);
    wm_content_draw_string(win, (uint32_t)(g->x + ox + 4), (uint32_t)(g->y + oy + (int32_t)g->h / 2 - 3), g->text, 0x00FFFFFF, 1);
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
    uint32_t tcolor = g->enabled ? 0x00E0E0E0 : 0x00808080;
    wm_content_draw_string(win, (uint32_t)(x + 3), (uint32_t)(y + (int32_t)g->h / 2 - 3), g->text, tcolor, 1);
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
static void slider_thumb_geom(gadget_t *g, int32_t *thumb_pos, int32_t *thumb_len) {
    int32_t track_len = (g->style == 2) ? (int32_t)g->h : (int32_t)g->w;
    int32_t total = g->slider_total > 0 ? g->slider_total : 1;
    int32_t visible = g->slider_visible > 0 ? g->slider_visible : 1;
    int32_t len = (track_len * visible) / total;
    if (len < 8) len = 8; // tamaño minimo para poder agarrarlo con el raton
    if (len > track_len) len = track_len;
    int32_t maxval = total - visible;
    int32_t avail = track_len - len; // espacio de movimiento del pomo
    int32_t pos = (maxval > 0 && avail > 0) ? (avail * g->slider_value) / maxval : 0;
    *thumb_pos = pos;
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
// Calcula el nuevo valor de un slider a partir de una coordenada
// LOCAL del raton (relativa al origen del gadget, en el eje
// principal) -- el click se interpreta como "el CENTRO del pomo
// deberia estar aqui", para que agarrar y arrastrar se sienta natural.
static void slider_set_value_from_local(gadget_t *g, int32_t local_pos) {
    int32_t track_len = (g->style == 2) ? (int32_t)g->h : (int32_t)g->w;
    int32_t thumb_pos, thumb_len;
    slider_thumb_geom(g, &thumb_pos, &thumb_len);
    (void)thumb_pos;
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
    wm_content_draw_string(win, (uint32_t)(g->x + ox + 4), (uint32_t)(g->y + oy + (int32_t)g->h / 2 - 3), g->text, 0x00E0E0E0, 1);
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
        wm_content_draw_string(win, (uint32_t)(g->x + ox + 4), (uint32_t)(ry + 5), g->items[i], 0x00E0E0E0, 1);
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
        wm_content_draw_string(win, (uint32_t)(x + 4), (uint32_t)(y + (int32_t)g->h / 2 - 3), g->items[g->selected], tcolor, 1);
    }
    wm_content_draw_string(win, (uint32_t)(x + (int32_t)g->w - 12), (uint32_t)(y + (int32_t)g->h / 2 - 3), "v", 0x00A0A0A0, 1);

    if (open_combobox[win] != id || g->item_count == 0) return;

    int32_t dd_y = y + (int32_t)g->h;
    uint32_t dd_h = g->item_count * ROW_H + 4;
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)dd_y, g->w, dd_h, 0x00181C20);
    wm_content_fill_rect(win, (uint32_t)x, (uint32_t)dd_y, g->w, 1, border);
    for (uint32_t i = 0; i < g->item_count; i++) {
        int32_t ry = dd_y + 2 + (int32_t)(i * ROW_H);
        if ((int32_t)i == g->selected) {
            wm_content_fill_rect(win, (uint32_t)(x + 1), (uint32_t)ry, g->w - 2, ROW_H, 0x00405070);
        }
        wm_content_draw_string(win, (uint32_t)(x + 4), (uint32_t)(ry + 5), g->items[i], 0x00E0E0E0, 1);
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
        int32_t tw = (int32_t)str_len(g->items[i]) * 6 + 14;
        uint32_t bg = ((int32_t)i == g->selected) ? 0x00405070 : 0x00303840;
        wm_content_fill_rect(win, (uint32_t)tx, (uint32_t)y, (uint32_t)tw, TABBER_TAB_H, bg);
        wm_content_draw_string(win, (uint32_t)(tx + 6), (uint32_t)(y + 7), g->items[i], 0x00E0E0E0, 1);
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
        if (enabled) {
            const uint8_t *src = pixels + (uint32_t)(i * ih) * 4; // offset horizontal en la tira
            wm_content_blit_image_rect(win, (uint32_t)bx, (uint32_t)y, ih, ih, iw, src, false, true, mask_color);
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
            wm_content_draw_string(win, (uint32_t)(x + 4 + indent), (uint32_t)(ry + 5), n->checked ? "-" : "+", 0x00A0A0A0, 1);
        }
        wm_content_draw_string(win, (uint32_t)(x + 16 + indent), (uint32_t)(ry + 5), n->text, 0x00E0E0E0, 1);
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
static void draw_textarea(gadget_t *g, int32_t win, int32_t ox, int32_t oy) {
    wm_content_fill_rect(win, (uint32_t)(g->x + ox), (uint32_t)(g->y + oy), g->w, g->h, 0x00181C20);
    wm_content_fill_rect(win, (uint32_t)(g->x + ox), (uint32_t)(g->y + oy), g->w, 1, 0x00707070);

    uint32_t max_rows = g->h / ROW_H;
    for (uint32_t i = 0; i < g->ta_line_count && i < max_rows; i++) {
        int32_t ry = g->y + oy + 2 + (int32_t)(i * ROW_H);
        wm_content_draw_string(win, (uint32_t)(g->x + ox + 4), (uint32_t)(ry + 5), g->ta_lines[i], 0x00E0E0E0, 1);
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
        wm_content_draw_string(window_idx, (uint32_t)x, 4, g->text, color, 1);
        x += (int32_t)str_len(g->text) * 6 + 14;
    }

    // Borramos SIEMPRE donde estaba el desplegable la vez anterior --
    // si no, sus pixeles se quedan pegados en pantalla al cambiar de
    // menu o al cerrarlo (0x00D4D0C8 es el mismo fondo por defecto que
    // usa el resto de la ventana).
    if (last_dd_w[window_idx] > 0) {
        wm_content_fill_rect(window_idx, (uint32_t)last_dd_x[window_idx], (uint32_t)last_dd_y[window_idx],
                              (uint32_t)last_dd_w[window_idx], (uint32_t)last_dd_h[window_idx], 0x00D4D0C8);
        last_dd_w[window_idx] = 0;
    }

    // Desplegable abierto, si lo hay -- solo un nivel de submenu (lo
    // habitual: barra -> lista de opciones, sin sub-sub-menus)
    int32_t open = open_menu[window_idx];
    if (open <= 0 || !gadgets[open].used) return;

    // Localizamos la posicion X de 'open' en la barra para colocar el
    // desplegable justo debajo
    int32_t menu_x = 4;
    for (int i = 1; i < MAX_GADGETS; i++) {
        gadget_t *g = &gadgets[i];
        if (!g->used || g->type != GADGET_MENU || g->parent_menu != root) continue;
        if (i == open) break;
        menu_x += (int32_t)str_len(g->text) * 6 + 14;
    }

    int32_t count = 0;
    for (int i = 1; i < MAX_GADGETS; i++) {
        if (gadgets[i].used && gadgets[i].type == GADGET_MENU && gadgets[i].parent_menu == open) count++;
    }
    if (count == 0) return;

    int32_t dw = 140;
    int32_t dh = count * MENU_ROW_H + 4;
    wm_content_fill_rect(window_idx, (uint32_t)menu_x, MENUBAR_H, (uint32_t)dw, (uint32_t)dh, 0x00F0F0F0);
    wm_content_fill_rect(window_idx, (uint32_t)menu_x, MENUBAR_H, (uint32_t)dw, 1, 0x00000000);

    // Recordamos este rectangulo para poder borrarlo la proxima vez.
    last_dd_x[window_idx] = menu_x;
    last_dd_y[window_idx] = MENUBAR_H;
    last_dd_w[window_idx] = dw;
    last_dd_h[window_idx] = dh;

    int32_t row = 0;
    for (int i = 1; i < MAX_GADGETS; i++) {
        gadget_t *g = &gadgets[i];
        if (!g->used || g->type != GADGET_MENU || g->parent_menu != open) continue;
        uint32_t color = g->enabled ? 0x00000000 : 0x00808080;
        char label[GADGET_TEXT_MAX + 2];
        int p = 0;
        if (g->checked) { label[p++] = '*'; label[p++] = ' '; }
        int j = 0; while (g->text[j] && p < GADGET_TEXT_MAX) label[p++] = g->text[j++];
        label[p] = '\0';
        wm_content_draw_string(window_idx, (uint32_t)menu_x + 6, (uint32_t)(MENUBAR_H + 4 + row * MENU_ROW_H), label, color, 1);
        row++;
    }
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
    if (!left) dragging_slider = -1; // se solto el boton -- termina cualquier arrastre
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
    if (click_edge && focused_win >= 0 && focused_win < MAX_WM_WINDOWS && open_menu[focused_win] > 0) {
        int32_t open = open_menu[focused_win];
        int32_t root = window_menu_root_set[focused_win] ? window_menu_root[focused_win] : -1;

        int32_t wx, wy; uint32_t ww, wh;
        if (wm_get_window_client_rect(focused_win, &wx, &wy, &ww, &wh)) {
            (void)ww; (void)wh;
            int32_t lx = mouse_x() - wx;
            int32_t ly = mouse_y() - wy;

            int32_t menu_x = 4;
            for (int i = 1; i < MAX_GADGETS; i++) {
                gadget_t *g = &gadgets[i];
                if (!g->used || g->type != GADGET_MENU || g->parent_menu != root) continue;
                if (i == open) break;
                menu_x += (int32_t)str_len(g->text) * 6 + 14;
            }
            int32_t count = 0;
            for (int i = 1; i < MAX_GADGETS; i++) {
                if (gadgets[i].used && gadgets[i].type == GADGET_MENU && gadgets[i].parent_menu == open) count++;
            }
            int32_t dw = 140, dh = count * MENU_ROW_H + 4;

            if (lx >= menu_x && lx < menu_x + dw && ly >= MENUBAR_H && ly < MENUBAR_H + dh) {
                int32_t item = (ly - MENUBAR_H - 2) / MENU_ROW_H;
                int32_t row = 0;
                for (int i = 1; i < MAX_GADGETS; i++) {
                    gadget_t *g = &gadgets[i];
                    if (!g->used || g->type != GADGET_MENU || g->parent_menu != open) continue;
                    if (row == item && g->enabled) { fire_menu_event(focused_win, i); break; }
                    row++;
                }
                open_menu[focused_win] = -1;
                click_edge = false; // el clic era PARA el desplegable -- consumido
            } else {
                open_menu[focused_win] = -1; // fuera del desplegable: lo cerramos...
                // ...pero click_edge se queda tal cual, para que este mismo
                // clic se procese normalmente a continuacion (abrir otro
                // menu, pulsar un boton, lo que corresponda).
            }
        } else {
            open_menu[focused_win] = -1;
        }
    }

    for (int i = 1; i < MAX_GADGETS; i++) {
        gadget_t *g = &gadgets[i];
        if (!g->used) continue;
        int32_t win = g->window_idx;
        if (win < 0) continue;

        if (g->type == GADGET_MENU_ROOT) {
            draw_menubar(win, i);

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
                            int32_t mw = (int32_t)str_len(m->text) * 6 + 14;
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
                            // Radio -- se marca a si mismo y desmarca
                            // a los demas radio del MISMO grupo (misma
                            // ventana -- no tenemos jerarquia de
                            // gadgets mas fina que esa todavia).
                            for (int k = 0; k < MAX_GADGETS; k++) {
                                gadget_t *other = &gadgets[k];
                                if (other->used && other->type == GADGET_BUTTON &&
                                    other->style == 3 && other->window_idx == g->window_idx) {
                                    other->checked = false;
                                }
                            }
                            g->checked = true;
                        }
                        fire_event(win, i);
                    } else if (g->type == GADGET_TEXTFIELD) {
                        gadget_activate(i);
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
                                int32_t tw = (int32_t)str_len(g->items[k]) * 6 + 14;
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
                slider_set_value_from_local(g, local);
                if (g->slider_value != old_value) fire_event(win, i);
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
            if (g->used && g->type == GADGET_TEXTFIELD && g->window_idx == focused_win && g->focused) {
                focused_field = i;
                break;
            }
        }
        if (focused_field >= 0) {
            char c;
            if (input_read_char(&c)) {
                textfield_handle_key(&gadgets[focused_field], c);
            }
        }
    }

    wm_request_redraw();
}
