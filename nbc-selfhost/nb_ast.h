// nb_ast.h — árbol de sintaxis abstracta de Nemo-Blitz 2.0.
//
// Misma idea que el AST viejo (una única estructura Node con campos
// genéricos a/b/c/d + una lista, en vez de un union por tipo de
// nodo) -- eso nunca fue el problema. La diferencia real: 'text' es
// nb_string_t*, no un `char text[64]` fijo -- el AST viejo truncaba
// en silencio tanto literales de cadena largos como nombres de
// variable/función largos (64 caracteres es fácil de superar con un
// nombre descriptivo). Los nodos se reservan con nb_alloc (el mismo
// montón privado por tarea que usan las cadenas y los Type) -- el
// propio compilador es una tarea más de Nemo OS, con su propia
// memoria que gestionar igual que cualquier programa que compile.

#ifndef NB_AST_H
#define NB_AST_H

#include <stdint.h>

typedef struct nb_string nb_string_t;

typedef enum {
    // Expresiones
    N_NUM, N_STR, N_VAR, N_BINOP, N_UNOP, N_CALL, N_FIELD, N_INDEX,
    N_NEW, N_FIRSTLAST, N_BEFORE, N_AFTER,

    // Sentencias -- primer bloque (control de flujo central)
    N_BLOCK, N_EXPRSTMT, N_ASSIGN,
    N_IF, N_ELSEIF, N_FOR, N_WHILE, N_REPEAT,
    N_PRINT, N_CLS, N_PLOT, N_LINE, N_RECT, N_DELAY,
    N_COLOR, N_GRAPHICS, N_OVAL, N_TEXT, N_CREATEWINDOW,
    N_CONSOLEAPP, N_DESKTOPAPP,
    N_RETURN, N_EXIT, N_ENDPROGRAM, N_GOTO, N_GOSUB,

    // Sentencias -- segundo bloque (funciones, tipos, datos)
    N_FUNCDEF, N_VARDECL, N_DIM, N_TYPEDEF,
    N_DELETE, N_FOREACH, N_INSERT,
    N_DATA, N_DATALABEL, N_READ, N_RESTORE,
} nb_node_kind_t;

// Marca en el campo 'op' de un parametro de Function declarado "nombre()".
#define NB_PARAM_ARRAY 1

typedef struct nb_node nb_node_t;

struct nb_node {
    nb_node_kind_t kind;
    int32_t line;

    double num_value;   // N_NUM
    nb_string_t *text;  // N_STR (contenido), N_VAR/N_CALL/N_FIELD (nombre) -- NULL si no aplica
    int32_t op;         // N_BINOP/N_UNOP: tipo de token del operador
                        // N_VAR como parametro de Function: NB_PARAM_ARRAY si se
                        // declaro "nombre()", es decir si recibe un array

    nb_node_t *a, *b, *c, *d; // uso generico, documentado donde se construye cada nodo

    nb_node_t **list;   // N_CALL: argumentos
    int32_t list_count;
    int32_t list_cap;
};

nb_node_t *nb_node_new(nb_node_kind_t kind, int32_t line);
void nb_node_list_add(nb_node_t *n, nb_node_t *child);

// Libera un arbol entero: primero sus hijos (a/b/c/d y la lista),
// luego la cadena que posea (si N_STR/N_VAR/N_CALL/N_FIELD), luego el
// propio nodo. El compilador NO tiene por que llamar a esto para todo
// el arbol de un programa que se compila una vez y termina (el
// monton entero de la tarea se recupera igual al salir) -- existe
// sobre todo para poder probar esta pieza sin fugas de verdad, y para
// cualquier caso futuro donde un sub-arbol se descarte a mitad de
// compilacion (una rama de error, por ejemplo).
void nb_node_free_tree(nb_node_t *n);

#endif
