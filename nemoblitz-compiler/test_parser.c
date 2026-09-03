#include <stdio.h>
#include "parser.h"

static const char *node_kind_name(NodeKind k) {
    switch (k) {
        case N_NUM: return "NUM"; case N_STR: return "STR"; case N_VAR: return "VAR";
        case N_BINOP: return "BINOP"; case N_UNOP: return "UNOP"; case N_CALL: return "CALL";
        case N_INDEX: return "INDEX"; case N_ASSIGN: return "ASSIGN"; case N_PRINT: return "PRINT";
        case N_IF: return "IF"; case N_ELSEIF: return "ELSEIF"; case N_FOR: return "FOR";
        case N_WHILE: return "WHILE"; case N_REPEAT: return "REPEAT"; case N_FUNCDEF: return "FUNCDEF";
        case N_RETURN: return "RETURN"; case N_DIM: return "DIM"; case N_VARDECL: return "VARDECL";
        case N_EXPRSTMT: return "EXPRSTMT"; case N_CLS: return "CLS"; case N_PLOT: return "PLOT";
        case N_LINE: return "LINE"; case N_RECT: return "RECT"; case N_BLOCK: return "BLOCK";
        default: return "?";
    }
}

static void dump(Node *n, int depth) {
    if (!n) return;
    for (int i = 0; i < depth; i++) printf("  ");
    printf("%s", node_kind_name(n->kind));
    if (n->kind == N_NUM) printf(" %.2f", n->num_value);
    if (n->kind == N_STR || n->kind == N_VAR || n->kind == N_CALL || n->kind == N_INDEX ||
        n->kind == N_FOR || n->kind == N_FUNCDEF || n->kind == N_DIM) {
        printf(" '%s'", n->text);
    }
    if (n->kind == N_BINOP || n->kind == N_UNOP) printf(" op=%d", n->op);
    printf("\n");

    dump(n->a, depth + 1);
    dump(n->b, depth + 1);
    dump(n->c, depth + 1);
    dump(n->d, depth + 1);
    for (int i = 0; i < n->list_count; i++) dump(n->list[i], depth + 1);
}

static const char *sample =
    "Global total = 0\n"
    "For i = 1 To 5\n"
    "    total = total + i\n"
    "    If i = 3 Then Print \"tres!\"\n"
    "Next\n"
    "Print \"Total: \" + Str$(total)\n"
    "\n"
    "Function Doble(x)\n"
    "    Return x * 2\n"
    "End Function\n"
    "\n"
    "Dim puntos(10)\n"
    "puntos(0) = 5\n";

int main(void) {
    Node *program = parse_program(sample);
    printf("=== AST ===\n");
    dump(program, 0);
    printf("\nParseado sin errores: %d sentencias de nivel superior.\n", program->list_count);
    return 0;
}
