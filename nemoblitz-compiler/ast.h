// ast.h — compilador Nemo-Blitz
//
// Arbol de sintaxis abstracta. Usamos una unica estructura Node con
// campos genericos (a/b/c/d + una lista) en vez de una union por tipo
// de nodo -- menos "elegante" que un union, pero mucho mas rapido de
// escribir y de recorrer despues, y para un compilador de este
// tamaño el ahorro de memoria de un union no compensa la complejidad.

#ifndef AST_H
#define AST_H

typedef enum {
    // Expresiones
    N_NUM, N_STR, N_VAR, N_BINOP, N_UNOP, N_CALL, N_INDEX,

    // Sentencias
    N_ASSIGN, N_PRINT, N_IF, N_ELSEIF, N_FOR, N_WHILE, N_REPEAT,
    N_FUNCDEF, N_RETURN, N_DIM, N_VARDECL, N_EXPRSTMT,
    N_CLS, N_PLOT, N_LINE, N_RECT, N_DELAY, N_ENDPROGRAM,
    N_BLOCK, N_EXIT,
    N_DATA, N_DATALABEL, N_READ, N_RESTORE,
    N_TYPEDEF, N_NEW, N_FIELD, N_DELETE, N_FOREACH, N_FIRSTLAST,
    N_LABEL, N_GOTO, N_GOSUB,
    N_BEFORE, N_AFTER, N_INSERT,
} NodeKind;

typedef struct Node Node;

struct Node {
    NodeKind kind;
    int line;

    double num_value;   // N_NUM
    char text[64];      // N_STR (contenido), N_VAR/N_CALL/N_INDEX/N_DIM (nombre),
                         // N_FOR (variable del bucle), N_FUNCDEF (nombre de la funcion)
    int op;              // N_BINOP/N_UNOP: TokenType del operador. N_VARDECL: Global/Local/Const

    // Uso generico, varia segun 'kind' -- documentado en cada caso en
    // parser.c justo donde se construye el nodo.
    Node *a, *b, *c, *d;

    Node **list;
    int list_count;
    int list_cap;
};

Node *node_new(NodeKind kind, int line);
void node_list_add(Node *n, Node *child);

#endif
