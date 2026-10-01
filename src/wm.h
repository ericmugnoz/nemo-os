// wm.h — Nemo OS
#ifndef WM_H
#define WM_H

// Numero maximo de ventanas a la vez. Definido AQUI, una sola vez.
//
// Antes estaba en tres sitios -- MAX_WINDOWS en wm.c, MAX_WM_WINDOWS en
// gadgets.c y MAX_CONSOLE_WINDOWS en syscall.c -- cada uno con un
// comentario de "debe coincidir con wm.c". Si se subia solo uno, la
// ventana nueva existia pero su consola y sus gadgets escribian fuera
// de sus arrays, en silencio.
//
// Era 8, y con los huecos de tarea ya en 16 era lo que impedia abrir la
// novena app: cada app necesita su ventana. Cada ventana cuesta ~4.8MB
// fijos de lienzo en el kernel (MAX_CONTENT_W x MAX_CONTENT_H x 4 en
// wm.c): 16 ventanas son 77MB, que caben de sobra en los 512MB de QEMU.
#define MAX_WINDOWS 16

#include <stdint.h>
#include <stdbool.h>

void wm_init(void);

// Crea una ventana nueva. Devuelve su indice, o -1 si no hay hueco.
int32_t wm_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h, const char *title);

// Reconfigura una ventana YA EXISTENTE (titulo, posicion, tamaño) --
// la usa CreateWindow() estilo BlitzPlus para "personalizar" la
// ventana que cada tarea ya tiene automaticamente, sin crear una
// segunda ventana de verdad.
void wm_configure_window(int32_t idx, const char *title, int32_t x, int32_t y, uint32_t w, uint32_t h);
// Solo cambia el titulo, sin tocar posicion/tamaño -- para AppTitle().
void wm_set_title(int32_t idx, const char *title);

// Activa/desactiva el "modo evento" de una ventana -- una vez activo,
// pulsar la X de cerrar YA NO destruye la ventana directamente: en su
// lugar dispara EVENT_WINDOWCLOSE ($803), para que el programa lo vea
// con WaitEvent() y decida el mismo cuando terminar (tipico patron
// BlitzPlus: "If WaitEvent()=$803 Then Exit"). Las ventanas normales
// (editor, shell, explorador...) no lo activan, y se comportan como
// siempre: la X las cierra sin mas.
void wm_set_event_mode(int32_t idx, bool on);

// Procesa entrada (raton) y decide si hace falta redibujar.
void wm_update(void);

// Redibuja la pantalla completa SOLO si algo cambio desde la ultima vez.
void wm_draw_if_needed(void);

// Devuelve el area de dibujo de una ventana (sin la barra de titulo),
// en coordenadas de pantalla. Lo usan las syscalls de graficos para
// saber donde dibuja cada programa. Devuelve 'false' si el indice no
// es una ventana valida.
bool wm_get_window_client_rect(int32_t idx, int32_t *x, int32_t *y, uint32_t *w, uint32_t *h);

// Marca una ventana como "dueña de su contenido": lo que dibuje ahi un
// programa sobrevive a los redibujados generales del escritorio (sin
// esto, mover el raton borraria el contenido dibujado por un programa).
void wm_set_owns_content(int32_t idx, bool owns);

// Dibujan en el buffer de contenido PROPIO de una ventana, en
// coordenadas LOCALES (0,0 = esquina superior izquierda del area de
// dibujo de esa ventana, debajo de la barra de titulo).
void wm_content_fill_rect(int32_t idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void wm_content_draw_string(int32_t idx, uint32_t x, uint32_t y, const char *str, uint32_t color, uint32_t scale);
void wm_content_blit_icon(int32_t idx, uint32_t x, uint32_t y, uint32_t size, const uint8_t *rgba);
void wm_content_blit_icon_scaled(int32_t idx, uint32_t x, uint32_t y, uint32_t size, uint32_t scale, const uint8_t *rgba);
// Igual que wm_content_blit_icon, pero con ancho y alto
// independientes -- los iconos embebidos son siempre cuadrados, pero
// una imagen cargada con LoadImage puede ser cualquier tamaño.
// 'solid': false=mezcla segun el canal alfa (DrawImage), true=opaco,
// ignora la transparencia (DrawBlock). 'has_mask'/'mask_color': de
// MaskImage, un color "clave" tratado como transparente EN CADA
// dibujado (no se toca la imagen original).
void wm_content_blit_image(int32_t idx, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t *rgba, bool solid, bool has_mask, uint32_t mask_color);

// Igual que wm_content_blit_image, pero SOLO pega un sub-rectangulo
// de una imagen mas ancha (src_stride = ancho real de esa imagen
// entera). Para DrawImageRect/DrawBlockRect y los fotogramas de
// LoadAnimImage.
void wm_content_blit_image_rect(int32_t idx, uint32_t x, uint32_t y, uint32_t blit_w, uint32_t blit_h, uint32_t src_stride, const uint8_t *rgba, bool solid, bool has_mask, uint32_t mask_color);

// Lee un pixel del buffer de contenido de una ventana -- para GetColor().
uint32_t wm_content_get_pixel(int32_t idx, uint32_t x, uint32_t y);
// Guardar y devolver un trozo del lienzo. Para lo que se
// dibuja ENCIMA y luego se quita -- un desplegable de menu, por ejemplo:
// hasta ahora al cerrarlo se rellenaba su hueco con el gris de fondo, y
// en una ventana que pinta lo suyo (un editor, un juego) eso deja un
// agujero gris hasta que el programa repinta. 'dst' debe tener sitio
// para w*h*4 bytes.
bool wm_content_save_rect(int32_t idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *dst);
void wm_content_restore_rect(int32_t idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *src);
void wm_content_put_pixel(int32_t idx, uint32_t x, uint32_t y, uint32_t color);

// Texto de interfaz (sans 12 con antialiasing; ver la nota en wm.c) sobre
// el bufer de una ventana -- para gadgets y dialogos. (x, y) como en
// wm_content_draw_string con la 5x7.
void wm_content_ui_text(int32_t idx, uint32_t x, uint32_t y, const char *s, uint32_t color);
uint32_t wm_ui_text_width(const char *s);
uint32_t wm_ui_text_height(void);

// Copia un rectangulo DENTRO del mismo buffer de contenido de una
// ventana -- para CopyRect(). Maneja solapamiento correctamente.
void wm_content_copy_rect(int32_t idx, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, uint32_t dx, uint32_t dy);

// Boton nativo: se registra (o actualiza) su region y aspecto por
// defecto para una ventana. wm_get_clicked_button devuelve el id del
// boton pulsado en el ultimo clic (0 si ninguno) -- solo detecta un
// clic por llamada (deteccion de flanco), hay que llamarla cada
// vuelta del bucle del programa.
void wm_define_button(int32_t win_idx, uint32_t id, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
uint32_t wm_get_clicked_button(int32_t win_idx);

// Marca la pantalla como "necesita redibujarse" en el proximo ciclo.
// La usan las syscalls de dibujo -- sin esto, escribir texto en una
// ventana no se ve hasta que el raton se mueve por casualidad (el
// redibujado solo se disparaba antes por actividad del raton).
void wm_request_redraw(void);

// Igual que la anterior, pero diciendo QUE trozo de pantalla ha cambiado.
// Solo ese rectangulo se vuelve a copiar a la pantalla: en 1080p una
// composicion entera son 5 MB por fotograma, y un programa que repinta su
// ventana no tiene por que pagarlos. Quien mueva, cierre o reordene
// ventanas usa wm_request_redraw, que ensucia todo -- ahi cambia de sitio
// lo que tapa a lo que, y no basta con un rectangulo.
void wm_request_redraw_rect(int32_t x, int32_t y, int32_t w, int32_t h);

// Devuelve el indice de la ventana que tiene actualmente el foco de
// teclado (la ultima en la que se hizo clic), o -1 si ninguna.
int32_t wm_get_focused_window(void);
void wm_activate_window(int32_t idx); // ActivateWindow: trae al frente + da foco de teclado
void wm_maximize_window(int32_t idx);
void wm_minimize_window(int32_t idx);
bool wm_window_maximized(int32_t idx);
bool wm_window_minimized(int32_t idx);
void wm_set_min_window_size(int32_t idx, uint32_t w, uint32_t h); // 0,0 = tamaño actual
// Ocultar botones de la barra de titulo: maximizar, minimizar y/o cerrar.
void wm_set_window_buttons(int32_t idx, bool sin_maximizar, bool sin_minimizar, bool sin_cerrar);

// Cierra una ventana: la quita de la pantalla, de la barra de tareas,
// y libera su hueco para que se pueda crear otra en su lugar.
void wm_destroy_window(int32_t idx);

// Devuelve 'true' UNA VEZ si el usuario pidio lanzar un programa
// (desde el menu Start o haciendo doble clic en un icono), copiando
// su nombre de archivo .pro en 'out_name'. El kernel debe comprobar
// esto en su bucle principal y lanzar el programa correspondiente.
bool wm_consume_launch_request(char *out_name, uint32_t max_len, char *out_arg, uint32_t max_arg_len,
                                int32_t *out_requesting_window, uint32_t *out_search_dir);
void wm_request_launch(const char *target_pro, const char *arg, int32_t requesting_window, uint32_t search_dir);

// Añade un icono al escritorio. Doble clic sobre el lanza el programa
// 'target_pro' (debe existir como archivo .pro en la raiz de NemoFS).
void wm_add_desktop_icon(const char *label, const char *target_pro, int32_t x, int32_t y);
void wm_add_desktop_icon_ex(const char *label, const char *target_pro, int32_t icon_id, int32_t x, int32_t y);
bool wm_remove_desktop_icon(int32_t index);
bool wm_move_desktop_icon(int32_t index, int32_t x, int32_t y);
bool wm_set_desktop_icon_graphic(int32_t index, int32_t icon_id);
// Cambia el NOMBRE de un icono ya colocado (hasta 15 caracteres).
bool wm_set_desktop_icon_label(int32_t index, const char *label);
int32_t wm_desktop_icon_count(void);
bool wm_get_desktop_icon(int32_t index, char *out_label, uint32_t label_max,
                         char *out_target, uint32_t target_max,
                         int32_t *out_icon_id, int32_t *out_x, int32_t *out_y);
int32_t wm_get_icon_scale(void);
void wm_set_icon_scale(int32_t scale); // 1 = 24x24, 2 = 48x48; cualquier otro valor se ajusta a 1 o 2
void wm_save_desktop_icons(void);
bool wm_load_desktop_icons(void);
// Añade los iconos que el sistema haya empezado a traer desde que se
// guardo este escritorio. Una vez por version (ver DESKTOP_CFG_VERSION).
void wm_migrar_escritorio(void);
// Lee MENU.CFG (el menu de inicio). Si no existe o esta mal, pone el de
// serie: el sistema nunca se queda sin menu.
void wm_menu_cargar(void);

// Fondo de pantalla. 'pixeles' tiene que venir del monton del kernel y
// medir EXACTAMENTE ancho*alto de la pantalla: el gestor se queda con el
// bufer y libera el anterior. Pasar 0 vuelve al color liso.
void wm_fondo_set(uint32_t *pixeles, uint32_t ancho, uint32_t alto);
bool wm_fondo_hay(void);

// Doble bufer: publica el dibujo de una ventana (lo copia al lienzo que
// se muestra) si ha cambiado. Ver wm.c.
void wm_publicar_contenido(int32_t idx);

#endif
