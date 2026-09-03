// ast.c — compilador Nemo-Blitz
//
// No liberamos memoria explicitamente -- el compilador procesa un
// archivo, genera codigo, y termina; dejar que el sistema operativo
// libere todo al salir es mas simple y no tiene ningun coste real
// para un programa de vida tan corta.

#include "ast.h"
#include <stdlib.h>
#include <string.h>

Node *node_new(NodeKind kind, int line) {
    Node *n = (Node *)calloc(1, sizeof(Node));
    n->kind = kind;
    n->line = line;
    return n;
}

void node_list_add(Node *n, Node *child) {
    if (n->list_count >= n->list_cap) {
        n->list_cap = n->list_cap == 0 ? 4 : n->list_cap * 2;
        n->list = (Node **)realloc(n->list, sizeof(Node *) * (size_t)n->list_cap);
    }
    n->list[n->list_count++] = child;
}
