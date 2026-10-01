// barra.h — Nemo OS: reparto de una barra de botones
//
// POR QUE EXISTE
//
// Las barras de herramientas de los programas se escribian con las
// coordenadas a mano: el primer boton en x=4, el siguiente en 78, el
// siguiente en 152... Funciona mientras la ventana sea ancha y nadie
// añada nada. En cuanto pasa una de las dos cosas, el ultimo boton se
// mete DEBAJO del que va anclado a la derecha, y quedan dos botones
// superpuestos: el de arriba tapa al de abajo, el de abajo sigue
// registrado como zona pulsable, y el usuario pulsa una cosa y le pasa
// otra.
//
// Ocurrio de verdad en el editor de escritorio: un boton nuevo empujo
// al del tamaño de iconos debajo del de Salir. Se parcheo alli con una
// condicion a medida ("si 246 + ancho >= win_w - 66, no lo dibujes"),
// con los numeros metidos a mano. Eso arregla un sitio y deja el mismo
// fallo esperando en todos los demas.
//
// Esto es ese calculo, una sola vez y sin numeros magicos.
//
// COMO SE USA
//
//   barra_t b;
//   barra_iniciar(&b, 4, barra_anclar_derecha(win_w, 58, 4), 4);
//
//   int x;
//   if (barra_hueco(&b, 70, &x)) {
//       define_button(BTN_ADD, x, 3, 70, ALTO, COLOR);
//       draw_bevel(x, 3, 70, ALTO, COLOR, true);
//       draw_text(x + 8, 8, "Añadir", NEGRO);
//   }
//
// REGLA IMPORTANTE: si barra_hueco() dice que no, no se dibuja NI se
// llama a define_button. Registrar la zona pulsable de un boton que no
// se ve es exactamente el fallo que esto viene a evitar.
//
// Solo hace cuentas: no dibuja, no llama a ninguna syscall y no
// depende de nada. Son funciones 'static inline' en una cabecera a
// proposito, porque cada .pro se enlaza por su cuenta y no hay ninguna
// biblioteca comun que enlazar.
#ifndef BARRA_H
#define BARRA_H

#include <stdbool.h>

typedef struct {
    int x;        // donde iria el siguiente boton
    int limite;   // primer pixel que ya NO se puede ocupar
    int sep;      // separacion entre botones
} barra_t;

// x0: donde empieza la fila. limite: primer pixel prohibido (tipicamente
// el borde izquierdo del boton anclado a la derecha, o win_w si no hay).
static inline void barra_iniciar(barra_t *b, int x0, int limite, int sep) {
    b->x = x0;
    b->limite = limite;
    b->sep = sep;
}

// ¿Cabe un boton de este ancho? Si cabe, deja su x en *x_out y avanza el
// cursor. Si no, devuelve false y NO avanza: los siguientes tampoco
// caben, y preguntarlo es inofensivo.
static inline bool barra_hueco(barra_t *b, int ancho, int *x_out) {
    if (ancho <= 0) return false;
    if (b->x + ancho > b->limite) return false;
    if (x_out) *x_out = b->x;
    b->x += ancho + b->sep;
    return true;
}

// Cuanto sitio queda hasta el limite, por si un boton quiere encogerse
// en vez de desaparecer.
static inline int barra_queda(const barra_t *b) {
    int q = b->limite - b->x;
    return q > 0 ? q : 0;
}

// La x de un boton pegado al borde derecho de la ventana. El valor que
// devuelve sirve tambien como 'limite' para barra_iniciar: es justo
// donde no deben llegar los de la izquierda.
static inline int barra_anclar_derecha(int win_w, int ancho, int margen) {
    int x = win_w - ancho - margen;
    return x > 0 ? x : 0;
}

#endif
