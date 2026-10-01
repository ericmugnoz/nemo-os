// nb_type.c — instancias de `Type` (New/Delete) del runtime de
// Nemo-Blitz 2.0, sobre el mismo montón privado que nb_alloc.c.
//
// Arregla el hallazgo más grave de los tres encontrados al revisar
// los pools compartidos del compilador viejo: `type_X_pool` tenía
// sitio para 64 instancias, y el contador `next_idx` que decidía
// dónde iba la siguiente NUNCA comprobaba ese límite -- ni se
// reseteaba cuando `Delete` borraba una instancia. La instancia 65 de
// cualquier Type, en la vida ENTERA del programa (no "vivas a la
// vez" -- total, aunque se hubieran borrado y recreado muchas veces),
// escribía fuera del pool reservado, encima de lo que viniera después
// en memoria.
//
// A diferencia de las cadenas (nb_string.c), las instancias de Type
// NO llevan conteo de referencias: su ciclo de vida ya lo decide el
// propio programa de forma explícita con New/Delete, igual que en
// BlitzPlus real -- no hay ninguna operación que cree o suelte una
// instancia por sorpresa (a diferencia de una cadena, que se crea
// constantemente sin que el programa lo pida a propósito: Left$,
// concatenar, Str$...). Por eso aquí basta con envolver nb_alloc/
// nb_free directamente: la parte de mantener la lista enlazada
// (First/Last/recorrido) sigue siendo responsabilidad del código
// generado por el compilador, exactamente igual que antes -- lo único
// que cambia es DE DÓNDE sale la memoria de cada instancia.
//
// Sin libc, mismo entorno que nb_alloc.c / nb_string.c.

#include <stdint.h>
#include <stddef.h>

extern void *nb_alloc(uint32_t n);
extern void nb_free(void *p);

// inst_size ya incluye el hueco para el puntero de lista enlazada
// (el primer campo de toda instancia, tal y como ya hacía el
// compilador viejo: inst_size = (1 + numero_de_campos) * 8) -- este
// archivo no necesita saber nada de esa convención, solo reserva y
// pone a cero los bytes exactos que se le piden.
void *nb_type_new(uint32_t inst_size) {
    void *p = nb_alloc(inst_size);
    if (!p) return NULL; // memoria agotada -- misma nota que en nb_string.c
    uint64_t *w = (uint64_t *)p;
    uint32_t n = inst_size / 8;
    for (uint32_t i = 0; i < n; i++) w[i] = 0;
    return p;
}

// Libera la memoria de verdad -- a diferencia del next_idx de antes,
// que nunca bajaba, este hueco queda disponible para la SIGUIENTE
// New (de este Type o de cualquier otro: el montón es compartido por
// todos, no un array separado por cada Type como antes).
void nb_type_delete(void *p) {
    nb_free(p);
}
