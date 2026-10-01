// nb_ast.c — ver nb_ast.h.

#include "nb_ast.h"
#include <stddef.h>

extern void *nb_alloc(uint32_t n);
extern void nb_free(void *p);
extern void nb_string_release(nb_string_t *s);

nb_node_t *nb_node_new(nb_node_kind_t kind, int32_t line) {
    nb_node_t *n = (nb_node_t *)nb_alloc((uint32_t)sizeof(nb_node_t));
    // nb_alloc no puede devolver NULL en la practica salvo que el
    // monton de 8MB de la tarea este realmente agotado -- un programa
    // fuente tendria que ser gigantesco. Por ahora, sin manejo de
    // errores real todavia (esteroide #1 del diseño), un fallo aqui
    // se trata igual que en nb_string.c: se deja pasar como NULL y el
    // llamador lo notara mas tarde en vez de reventar aqui mismo.
    if (!n) return NULL;
    n->kind = kind;
    n->line = line;
    n->num_value = 0;
    n->text = NULL;
    n->op = 0;
    n->a = n->b = n->c = n->d = NULL;
    n->list = NULL;
    n->list_count = 0;
    n->list_cap = 0;
    return n;
}

void nb_node_list_add(nb_node_t *n, nb_node_t *child) {
    if (n->list_count >= n->list_cap) {
        int32_t new_cap = n->list_cap == 0 ? 4 : n->list_cap * 2;
        nb_node_t **new_list = (nb_node_t **)nb_alloc(sizeof(nb_node_t *) * (uint32_t)new_cap);
        if (!new_list) return; // memoria agotada -- se descarta silenciosamente
                                // por ahora (misma nota que en nb_node_new)
        for (int32_t i = 0; i < n->list_count; i++) new_list[i] = n->list[i];
        // A diferencia del compilador viejo (que dejaba el array
        // anterior sin liberar cada vez que la lista crecia -- una
        // fuga pequeña pero real), aqui SI se libera: nb_alloc es un
        // asignador de verdad, con nb_free real detras, asi que no
        // hay motivo para no hacerlo.
        if (n->list) nb_free(n->list);
        n->list = new_list;
        n->list_cap = new_cap;
    }
    n->list[n->list_count++] = child;
}

void nb_node_free_tree(nb_node_t *n) {
    if (!n) return;
    nb_node_free_tree(n->a);
    nb_node_free_tree(n->b);
    nb_node_free_tree(n->c);
    nb_node_free_tree(n->d);
    for (int32_t i = 0; i < n->list_count; i++) nb_node_free_tree(n->list[i]);
    if (n->list) nb_free(n->list);
    if (n->text) nb_string_release(n->text);
    nb_free(n);
}
