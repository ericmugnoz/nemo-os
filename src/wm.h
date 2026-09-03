// wm.h — Nemo OS
#ifndef WM_H
#define WM_H

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

// Devuelve el indice de la ventana que tiene actualmente el foco de
// teclado (la ultima en la que se hizo clic), o -1 si ninguna.
int32_t wm_get_focused_window(void);
void wm_activate_window(int32_t idx); // ActivateWindow: trae al frente + da foco de teclado
void wm_maximize_window(int32_t idx);
void wm_minimize_window(int32_t idx);
bool wm_window_maximized(int32_t idx);
bool wm_window_minimized(int32_t idx);
void wm_set_min_window_size(int32_t idx, uint32_t w, uint32_t h); // 0,0 = tamaño actual

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

#endif
