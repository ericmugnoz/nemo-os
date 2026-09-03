// input.h — Nemo OS
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stdbool.h>

bool input_init(void);

// Procesa los eventos pendientes de teclado/raton. Hay que llamarla
// periodicamente (todavia no tenemos interrupciones para esto, es
// polling desde el bucle principal).
void input_poll(void);

// Raton -- posicion absoluta ya escalada a los pixeles del framebuffer
int32_t mouse_x(void);
int32_t mouse_y(void);
bool mouse_left_down(void);
bool mouse_right_down(void);
bool mouse_middle_down(void);

// Rueda del raton -- devuelve el acumulado desde la ultima llamada
// (positivo = hacia arriba/alejando, negativo = hacia abajo/acercando,
// como en cualquier raton normal) y lo RESETEA a cero. Llamarla una
// vez por vuelta del bucle principal es suficiente.
int32_t mouse_wheel_delta(void);
int32_t mouse_wheel_total(void); // MouseZ() -- acumulado, no se resetea

// Teclado -- codigos de tecla estilo Linux (ver KEY_* en input.c)
bool key_is_down(uint16_t keycode);

// Cola de caracteres ya traducidos a ASCII (teniendo en cuenta mayus).
// Devuelve 'false' si no hay ninguno pendiente.
bool input_read_char(char *out);

// Descarta cualquier tecla pendiente en la cola -- se usa al cambiar
// el foco de teclado entre ventanas, para que lo que escribas en un
// sitio no aparezca de golpe en otro cuando cambies de ventana.
void input_flush_chars(void);

// Cola de codigos de tecla PULSADOS (flanco de subida), para
// GetKey()/WaitKey() -- a diferencia de input_read_char (que da
// caracteres YA TRADUCIDOS a ASCII), esta da el SCANCODE crudo,
// incluso de teclas sin traduccion ASCII (F1-F12, etc).
bool input_read_scancode(uint16_t *out);

// KeyHit(scancode): CUANTAS veces se pulso esa tecla EN CONCRETO
// desde la ultima vez que se pregunto por ELLA -- consume el contador
// (vuelve a 0 al leerlo). BlitzPlus real devuelve un numero, no un
// simple si/no.
uint32_t key_was_hit(uint16_t keycode);

// MouseHit(boton): igual que key_was_hit pero para los botones del
// raton. boton: 1=izquierdo, 2=derecho.
uint32_t mouse_button_was_hit(int button);

// MouseXSpeed()/MouseYSpeed(): diferencia de posicion desde la
// ULTIMA vez que se llamo a CADA UNA (se resetean por separado).
int32_t mouse_x_speed(void);
int32_t mouse_y_speed(void);

// MoveMouse(x,y): fuerza la posicion del cursor. OJO -- es temporal:
// en cuanto el raton FISICO del Mac se mueva de nuevo, el siguiente
// evento absoluto del dispositivo la vuelve a sobreescribir (nuestro
// raton reporta posicion absoluta real, no hay forma de "desconectarla").
// Sigue siendo util para efectos puntuales (centrar el cursor, etc).
void mouse_move_to(int32_t x, int32_t y);

// Descarta cualquier tecla pendiente en TODAS las colas (caracteres,
// scancodes, y las banderas de KeyHit) -- para FlushKeys.
void input_flush_keys(void);
void input_flush_mouse(void); // FlushMouse -- separado de FlushKeys

#endif
