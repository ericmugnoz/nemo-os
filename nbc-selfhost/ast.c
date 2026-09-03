// ast.c — compilador Nemo-Blitz
//
// No liberamos memoria explicitamente -- el compilador procesa un
// archivo, genera codigo, y termina; dejar que el sistema operativo
// libere todo al salir es mas simple y no tiene ningun coste real
// para un programa de vida tan corta. Por el mismo motivo, cuando
// una lista de hijos necesita crecer, simplemente pedimos un bloque
// nuevo mas grande y copiamos -- el bloque viejo se queda sin usar
// (nuestro asignador de avance no tiene "liberar"), pero con el
// pool de 4MB de sobra que tenemos, no supone ningun problema real.

#include "ast.h"
#include "nblibc.h"

Node *node_new(NodeKind kind, int line) {
    Node *n = (Node *)nb_alloc(sizeof(Node)); // nb_alloc ya pone todo a cero, como calloc
    n->kind = kind;
    n->line = line;
    return n;
}

void node_list_add(Node *n, Node *child) {
    if (n->list_count >= n->list_cap) {
        int new_cap = n->list_cap == 0 ? 4 : n->list_cap * 2;
        Node **new_list = (Node **)nb_alloc(sizeof(Node *) * (uint32_t)new_cap);
        if (n->list_count > 0) {
            nb_memcpy(new_list, n->list, sizeof(Node *) * (uint32_t)n->list_count);
        }
        n->list = new_list;
        n->list_cap = new_cap;
    }
    n->list[n->list_count++] = child;
}
