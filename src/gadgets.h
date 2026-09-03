// gadgets.h — Nemo OS
//
// Capa de "gadgets" (widgets) al estilo BlitzPlus: Button, Panel,
// TextField, ListBox y Menu. La idea no es imitar Win32 por dentro --
// es que estos gadgets respondan a los MISMOS comandos que un
// programa BlitzPlus portado espera (CreateButton, SetGadgetText,
// etc.), construidos encima de lo que ya tenemos (ventanas, dibujo,
// entrada). Asi el codigo portado no nota la diferencia por debajo.
//
// A diferencia de nuestras syscalls de dibujo de mas bajo nivel
// (SYS_DRAW_RECT/TEXT), los gadgets se dibujan y gestionan ELLOS
// SOLOS -- el programa los crea una vez y el kernel se encarga de
// pintarlos y de procesar clics/teclado en cada vuelta del
// planificador, exactamente como hace BlitzPlus de verdad.

#ifndef GADGETS_H
#define GADGETS_H

#include <stdint.h>
#include <stdbool.h>

#define GADGET_BUTTON    1
#define GADGET_PANEL     2
#define GADGET_TEXTFIELD 3
#define GADGET_LISTBOX   4
#define GADGET_MENU      5 // entrada de menu (de la barra o de un desplegable)
#define GADGET_MENU_ROOT 6 // raiz de la barra de menu de una ventana (ver gadget_window_menu)
#define GADGET_TEXTAREA  7 // caja de texto multilinea, de solo lectura en v1
#define GADGET_LABEL     8 // solo texto, sin eventos (CreateLabel)
#define GADGET_PROGBAR   9 // barra de progreso (CreateProgBar)
#define GADGET_SLIDER   10 // deslizador con arrastre (CreateSlider)
#define GADGET_COMBOBOX 11 // lista desplegable (CreateComboBox) -- comparte almacenamiento de items con ListBox
#define GADGET_TABBER   12 // fila de pestañas (CreateTabber) -- comparte almacenamiento de items con ListBox
#define GADGET_TOOLBAR  13 // fila de botones con icono (CreateToolBar)
#define GADGET_TREEVIEW 14 // contenedor jerarquico (CreateTreeView)
#define GADGET_TREENODE 15 // nodo individual de un TreeView -- cada uno es su propio gadget con handle propio
#define GADGET_CANVAS   16 // superficie de dibujo dentro de una ventana (CreateCanvas)

// Codigos de evento -- los mismos valores hexadecimales que BlitzPlus
// real, para que un programa portado los reconozca tal cual (los
// ejemplos suelen compararlos directamente, ej. "WaitEvent()=$803").
#define EVENT_GADGETACTION 0x401 // un gadget disparo algo (boton, seleccion...)
#define EVENT_WINDOWSIZE   0x802 // la ventana cambio de tamaño
#define EVENT_WINDOWCLOSE  0x803 // se pulso la X de cerrar
#define EVENT_MENUACTION   0x1001 // se eligio una entrada de menu
#define EVENT_TIMERTICK    0x4001 // temporizador (CreateTimer)

void gadgets_init(void);

// -- creacion --
int32_t gadget_create_button(const char *text, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx, uint32_t style);
int32_t gadget_create_panel(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx);
int32_t gadget_create_textfield(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx);
int32_t gadget_create_listbox(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx);

// TextArea: caja multilinea de solo lectura -- SetTextAreaText()
// reemplaza TODO el contenido de golpe, partiendolo por '\n'.
int32_t gadget_create_textarea(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx);
int32_t gadget_create_label(const char *text, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx, uint32_t style);
int32_t gadget_create_progbar(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx);
void gadget_update_progbar(int32_t id, int32_t value_permil);
int32_t gadget_create_slider(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx, uint32_t style);
void gadget_set_slider_range(int32_t id, int32_t visible, int32_t total);
void gadget_set_slider_value(int32_t id, int32_t value);
int32_t gadget_slider_value(int32_t id);
int32_t gadget_create_combobox(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx);
int32_t gadget_create_tabber(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx);
int32_t gadget_create_toolbar(int32_t image_handle, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx);
void gadget_set_icon_strip(int32_t id, int32_t strip_handle);
void gadget_set_panel_color(int32_t id, uint32_t rgb);
void gadget_set_panel_image(int32_t id, int32_t image_handle);
void gadget_set_group(int32_t id, int32_t group);
int32_t gadget_get_group(int32_t id);
void gadget_enable_toolbar_item(int32_t id, int32_t index, bool enabled);
void gadget_set_toolbar_tips(int32_t id, const char *tips);
int32_t gadget_create_treeview(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx);
int32_t gadget_treeview_root(int32_t treeview);
int32_t gadget_add_treeview_node(const char *text, int32_t parent);
int32_t gadget_insert_treeview_node(int32_t index, const char *text, int32_t parent);
void gadget_modify_treeview_node(int32_t node, const char *text);
void gadget_free_treeview_node(int32_t node);
void gadget_expand_treeview_node(int32_t node, bool expand);
int32_t gadget_count_treeview_nodes(int32_t parent);
int32_t gadget_selected_treeview_node(int32_t treeview);
void gadget_select_treeview_node(int32_t node);
int32_t gadget_create_canvas(int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t window_idx);
bool gadget_is_canvas(int32_t id);
void gadget_textarea_set_text(int32_t id, const char *text);
void gadget_textarea_add_text(int32_t id, const char *text);
uint32_t gadget_textarea_len(int32_t id, int32_t units); // 1=caracteres, 2=lineas
uint32_t gadget_textarea_line_len(int32_t id, int32_t line);
int32_t gadget_textarea_line_of_char(int32_t id, int32_t charpos);
void gadget_textarea_get_text(int32_t id, int32_t start, int32_t count, char *out, uint32_t max_len);

// -- modificadores universales --
void gadget_free(int32_t id);

// Libera TODOS los gadgets de una ventana de golpe -- hay que
// llamarla cuando la ventana se destruye (lo hace tasks.c
// automaticamente), para que no queden gadgets "fantasma" si esa
// ventana se reutiliza para un programa nuevo.
void gadgets_free_window(int32_t window_idx);
void gadget_set_text(int32_t id, const char *text);
uint32_t gadget_get_text(int32_t id, char *out, uint32_t max_len);
bool gadget_get_rect(int32_t id, int32_t *x, int32_t *y, uint32_t *w, uint32_t *h);
void gadget_move(int32_t id, int32_t x, int32_t y);
void gadget_resize(int32_t id, uint32_t w, uint32_t h);
void gadget_show(int32_t id, bool visible);
void gadget_enable(int32_t id, bool enabled);
void gadget_activate(int32_t id); // da el foco de teclado (TextField)

// Devuelve el id del gadget que ha disparado un evento en esta
// ventana desde la ultima llamada (0 si ninguno): boton pulsado,
// seleccion de lista cambiada, Enter en un campo de texto, o entrada
// de menu elegida.
int32_t gadget_poll_event(int32_t window_idx);

// -- Eventos "en crudo", estilo BlitzPlus (WaitEvent/EventID/...) --
//
// Generalizacion de lo de arriba: en vez de solo "que gadget disparo
// algo", cualquier suceso de la ventana (un gadget, pero tambien
// cerrarla, redimensionarla, un timer...) se guarda como un evento
// con TRES campos -- id (que tipo de suceso, los mismos codigos
// hexadecimales que BlitzPlus: $401 gadget, $802 redimension, $803
// cerrar...), fuente (quien lo disparo) y datos (extra, segun el
// tipo). Solo se guarda UNO pendiente a la vez por ventana -- si
// llega uno nuevo antes de que se consulte el anterior, lo
// sobreescribe (igual de "ultimo gana" que el resto del sistema).
void gadgets_fire_raw_event(int32_t window_idx, int32_t event_id, int32_t source, int32_t data);

// Consume el evento pendiente de esa ventana (0 si no hay ninguno) --
// y cachea su fuente/datos para que gadgets_get_last_event_source/data
// los puedan devolver despues, aunque ya no este "pendiente".
int32_t gadgets_poll_raw_event(int32_t window_idx);
int32_t gadgets_peek_raw_event(int32_t window_idx);
void gadgets_flush_events(int32_t window_idx, int32_t filter_id);

int32_t gadgets_get_last_event_source(int32_t window_idx);
int32_t gadgets_get_last_event_data(int32_t window_idx);

// -- CreateTimer: un temporizador por ventana, dispara EVENT_TIMERTICK
// ($4001) a intervalos regulares mientras el programa siga llamando a
// WaitEvent()/PollEvent() (o simplemente cediendo el control). El
// "handle" que devuelve es el propio indice de ventana -- v1 solo
// soporta un temporizador activo por ventana.
int32_t gadget_create_timer(int32_t window_idx, uint32_t hertz);
void gadget_free_timer(int32_t handle);
bool gadget_timer_ready(int32_t handle);
void gadget_timer_consume(int32_t handle);
void gadget_pause_timer(int32_t handle);
void gadget_resume_timer(int32_t handle);
void gadget_reset_timer(int32_t handle);
uint32_t gadget_timer_ticks(int32_t handle);
void gadgets_check_timers(void); // llamar una vez por vuelta desde task_yield

// HotKeyEvent rawkey,modifier,event_id[,event_data,...,event_source]
// -- registra una tecla rapida global. event_id=0 quita el hotkey
// para esa tecla/modificador.
void gadget_hotkey_event(uint16_t rawkey, uint8_t modifier, int32_t event_id, int32_t event_data, int32_t event_source);
void gadgets_check_hotkeys(void); // llamar una vez por vuelta, igual que gadgets_check_timers

// -- ListBox --
void gadget_listbox_add_item(int32_t id, const char *text);
void gadget_listbox_insert_item(int32_t id, int32_t index, const char *text);
void gadget_listbox_remove_item(int32_t id, int32_t index);
void gadget_listbox_modify_item(int32_t id, int32_t index, const char *text);
void gadget_listbox_clear(int32_t id);
int32_t gadget_listbox_selected(int32_t id);
void gadget_listbox_select(int32_t id, int32_t index);
uint32_t gadget_listbox_item_count(int32_t id);
uint32_t gadget_listbox_item_text(int32_t id, int32_t index, char *out, uint32_t max_len);

// -- Menus --
// Devuelve (creando la primera vez que se llama) la raiz de la barra
// de menu de esa ventana -- se usa como 'parent' al crear las
// entradas de primer nivel con gadget_create_menu.
int32_t gadget_window_menu(int32_t window_idx);

// 'parent' es la raiz (gadget_window_menu) para una entrada de primer
// nivel ("&Archivo"), o el id de otra entrada de menu para crear un
// submenu dentro de ella ("Nuevo" dentro de "Archivo").
int32_t gadget_create_menu(const char *text, int32_t tag, int32_t parent);
void gadget_menu_check(int32_t id, bool checked);
bool gadget_button_state(int32_t id);
bool gadget_is_enabled(int32_t id); // generico: MenuEnabled() y similares
void gadget_set_button_state(int32_t id, bool state);
void gadget_menu_enable(int32_t id, bool enabled);
int32_t gadget_menu_get_tag(int32_t id);

// Se llama una vez por vuelta del planificador (desde task_yield) --
// procesa clics/teclado y redibuja los gadgets de todas las ventanas.
void gadgets_update_and_draw(void);

// Cuanto espacio reserva la barra de menu en la parte superior del
// area de contenido de una ventana que tiene menu (0 si no tiene).
uint32_t gadgets_menubar_height(int32_t window_idx);

#endif
