// parser.c — compilador Nemo-Blitz
//
// Analizador descendente recursivo clasico, con "precedence climbing"
// para las expresiones. Cada nivel de precedencia es una funcion que
// llama al siguiente nivel mas fuerte; de menor a mayor precedencia:
//
//   Or  ->  And  ->  comparaciones (= < > <= >= <>)  ->  + -  ->  * / Mod
//   -> unario (- Not) -> primario (numero, cadena, variable, (expr),
//   llamada a funcion, indexado de array)

#include "parser.h"
#include "lexer.h"
#include "nblibc.h"
#include "nb_runtime.h"
#include <stdbool.h>

typedef struct {
    Lexer lx;
    Token cur;
} Parser;

static void advance(Parser *p) { p->cur = lexer_next(&p->lx); }

static void error_at(Parser *p, const char *msg) {
    const char *found = (p->cur.type == T_IDENT || p->cur.type == T_STRING || p->cur.type == T_NUMBER)
                ? p->cur.text : token_type_name(p->cur.type);
    nb_fatal(p->cur.line, msg, found);
}

static bool check(Parser *p, TokenType t) { return p->cur.type == t; }

static bool match(Parser *p, TokenType t) {
    if (check(p, t)) { advance(p); return true; }
    return false;
}

static void expect(Parser *p, TokenType t, const char *what) {
    if (!match(p, t)) error_at(p, what);
}

// Salta separadores de sentencia (fin de linea, ':') -- BlitzPlus
// permite varias sentencias en una linea separadas por ':', y
// tratamos ambos separadores igual dentro de un bloque.
static void skip_separators(Parser *p) {
    while (check(p, T_NEWLINE) || check(p, T_COLON)) advance(p);
}

static Node *parse_expr(Parser *p);
static void split_dot(const char *full, char *name_out, uint32_t name_out_size);
static Node *parse_block(Parser *p);
static bool at_block_end(Parser *p);
static Node *parse_assign_or_expr_stmt_or_command(Parser *p);

// ---- expresiones, de menor a mayor precedencia ----

static Node *parse_primary(Parser *p) {
    int line = p->cur.line;

    if (check(p, T_NUMBER)) {
        Node *n = node_new(N_NUM, line);
        n->num_value = p->cur.num_value;
        // Igual que en el compilador del host: marcamos si el propio
        // literal llevaba punto decimal, para saber si es flotante.
        for (int i = 0; p->cur.text[i] != '\0'; i++) {
            if (p->cur.text[i] == '.') { n->text[0] = '#'; break; }
        }
        advance(p);
        return n;
    }
    if (check(p, T_STRING)) {
        Node *n = node_new(N_STR, line);
        nb_strncpy(n->text, p->cur.text, sizeof(n->text) - 1);
        advance(p);
        return n;
    }
    if (check(p, T_KW_TRUE) || check(p, T_KW_FALSE)) {
        Node *n = node_new(N_NUM, line);
        n->num_value = check(p, T_KW_TRUE) ? 1 : 0;
        advance(p);
        return n;
    }
    if (check(p, T_KW_NULL)) {
        Node *n = node_new(N_NUM, line);
        n->num_value = 0;
        advance(p);
        return n;
    }
    if (check(p, T_KW_NEW)) {
        advance(p);
        if (!check(p, T_IDENT)) error_at(p, "se esperaba el nombre del tipo tras 'New'");
        Node *n = node_new(N_NEW, line);
        nb_strncpy(n->text, p->cur.text, sizeof(n->text) - 1);
        advance(p);
        return n;
    }
    if (check(p, T_KW_FIRST) || check(p, T_KW_LAST)) {
        bool is_first = check(p, T_KW_FIRST);
        advance(p);
        bool has_parens = match(p, T_LPAREN);
        if (!check(p, T_IDENT)) error_at(p, "se esperaba el nombre del tipo");
        Node *n = node_new(N_FIRSTLAST, line);
        n->op = is_first ? T_KW_FIRST : T_KW_LAST;
        nb_strncpy(n->text, p->cur.text, sizeof(n->text) - 1);
        advance(p);
        if (has_parens) expect(p, T_RPAREN, "se esperaba ')'");
        return n;
    }
    if (check(p, T_KW_BEFORE) || check(p, T_KW_AFTER)) {
        // Before/After aceptan una EXPRESION general -- ver la nota
        // equivalente en el compilador del host.
        bool is_before = check(p, T_KW_BEFORE);
        advance(p);
        bool has_parens = match(p, T_LPAREN);
        Node *n = node_new(is_before ? N_BEFORE : N_AFTER, line);
        n->a = parse_expr(p);
        if (has_parens) expect(p, T_RPAREN, "se esperaba ')'");
        return n;
    }
    if (check(p, T_LPAREN)) {
        advance(p);
        Node *n = parse_expr(p);
        expect(p, T_RPAREN, "se esperaba ')'");
        return n;
    }
    if (check(p, T_IDENT)) {
        char raw_name[64];
        nb_strncpy(raw_name, p->cur.text, sizeof(raw_name) - 1);
        raw_name[sizeof(raw_name) - 1] = '\0';
        char name[64];
        split_dot(raw_name, name, sizeof(name));
        advance(p);

        if (check(p, T_LPAREN)) {
            advance(p);
            // Ambiguo entre "llamada a funcion" e "indexado de array"
            // a nivel de sintaxis -- ambos son "nombre(args)". El
            // generador de codigo decide cual es segun si 'name' esta
            // declarado como array o como funcion.
            Node *n = node_new(N_CALL, line);
            nb_strncpy(n->text, name, sizeof(n->text) - 1);
            if (!check(p, T_RPAREN)) {
                node_list_add(n, parse_expr(p));
                while (match(p, T_COMMA)) node_list_add(n, parse_expr(p));
            }
            expect(p, T_RPAREN, "se esperaba ')' tras los argumentos");
            return n;
        }

        Node *n = node_new(N_VAR, line);
        nb_strncpy(n->text, name, sizeof(n->text) - 1);

        while (check(p, T_BACKSLASH)) {
            advance(p);
            if (!check(p, T_IDENT)) error_at(p, "se esperaba un nombre de campo tras '\\'");
            Node *fn = node_new(N_FIELD, line);
            nb_strncpy(fn->text, p->cur.text, sizeof(fn->text) - 1);
            fn->a = n;
            advance(p);
            n = fn;
        }
        return n;
    }

    error_at(p, "se esperaba un numero, cadena, variable o '('");
    return NULL; // inalcanzable -- error_at termina el proceso
}

static Node *parse_unary(Parser *p) {
    int line = p->cur.line;
    if (check(p, T_MINUS) || check(p, T_KW_NOT)) {
        int op = p->cur.type;
        advance(p);
        Node *n = node_new(N_UNOP, line);
        n->op = op;
        n->a = parse_unary(p);
        return n;
    }
    return parse_primary(p);
}

static Node *parse_mul(Parser *p) {
    Node *left = parse_unary(p);
    while (check(p, T_STAR) || check(p, T_SLASH) || check(p, T_KW_MOD) ||
           check(p, T_KW_SHL) || check(p, T_KW_SHR) || check(p, T_KW_SAR)) {
        int op = p->cur.type;
        int line = p->cur.line;
        advance(p);
        Node *n = node_new(N_BINOP, line);
        n->op = op;
        n->a = left;
        n->b = parse_unary(p);
        left = n;
    }
    return left;
}

static Node *parse_add(Parser *p) {
    Node *left = parse_mul(p);
    while (check(p, T_PLUS) || check(p, T_MINUS)) {
        int op = p->cur.type;
        int line = p->cur.line;
        advance(p);
        Node *n = node_new(N_BINOP, line);
        n->op = op;
        n->a = left;
        n->b = parse_mul(p);
        left = n;
    }
    return left;
}

static Node *parse_comparison(Parser *p) {
    Node *left = parse_add(p);
    while (check(p, T_EQ) || check(p, T_LT) || check(p, T_GT) ||
           check(p, T_LE) || check(p, T_GE) || check(p, T_NE)) {
        int op = p->cur.type;
        int line = p->cur.line;
        advance(p);
        Node *n = node_new(N_BINOP, line);
        n->op = op;
        n->a = left;
        n->b = parse_add(p);
        left = n;
    }
    return left;
}

static Node *parse_and(Parser *p) {
    Node *left = parse_comparison(p);
    while (check(p, T_KW_AND)) {
        int line = p->cur.line;
        advance(p);
        Node *n = node_new(N_BINOP, line);
        n->op = T_KW_AND;
        n->a = left;
        n->b = parse_comparison(p);
        left = n;
    }
    return left;
}

static Node *parse_xor(Parser *p) {
    Node *left = parse_and(p);
    while (check(p, T_KW_XOR)) {
        int line = p->cur.line;
        advance(p);
        Node *n = node_new(N_BINOP, line);
        n->op = T_KW_XOR;
        n->a = left;
        n->b = parse_and(p);
        left = n;
    }
    return left;
}

static Node *parse_or(Parser *p) {
    Node *left = parse_xor(p);
    while (check(p, T_KW_OR)) {
        int line = p->cur.line;
        advance(p);
        Node *n = node_new(N_BINOP, line);
        n->op = T_KW_OR;
        n->a = left;
        n->b = parse_xor(p);
        left = n;
    }
    return left;
}

static Node *parse_expr(Parser *p) { return parse_or(p); }

// ---- sentencias ----

// Nombre de variable (o array) seguido de '=' o de '(' indices ')' '='
// -- lo tratamos aqui en vez de en parse_primary porque una
// ASIGNACION es una sentencia, no una expresion.
static Node *parse_assign_or_expr_stmt(Parser *p) {
    int line = p->cur.line;
    char raw_name[64];
    nb_strncpy(raw_name, p->cur.text, sizeof(raw_name) - 1);
    raw_name[sizeof(raw_name) - 1] = '\0';
    char name[64];
    split_dot(raw_name, name, sizeof(name));
    advance(p); // consume el identificador

    if (check(p, T_LPAREN)) {
        Parser saved = *p;
        advance(p);
        Node *idx = node_new(N_INDEX, line);
        nb_strncpy(idx->text, name, sizeof(idx->text) - 1);
        if (!check(p, T_RPAREN)) {
            node_list_add(idx, parse_expr(p));
            while (match(p, T_COMMA)) node_list_add(idx, parse_expr(p));
        }
        expect(p, T_RPAREN, "se esperaba ')' tras los indices");

        if (match(p, T_EQ)) {
            Node *n = node_new(N_ASSIGN, line);
            n->a = idx;
            n->b = parse_expr(p);
            return n;
        }

        if (check(p, T_STAR) || check(p, T_SLASH) || check(p, T_PLUS) || check(p, T_MINUS) ||
            check(p, T_KW_MOD) || check(p, T_KW_AND) || check(p, T_KW_OR) ||
            check(p, T_LT) || check(p, T_GT) || check(p, T_LE) || check(p, T_GE) ||
            check(p, T_NE) || check(p, T_EQ)) {
            *p = saved;
            Node *cmd = node_new(N_CALL, line);
            nb_strncpy(cmd->text, name, sizeof(cmd->text) - 1);
            node_list_add(cmd, parse_expr(p));
            while (match(p, T_COMMA)) node_list_add(cmd, parse_expr(p));
            Node *stmt2 = node_new(N_EXPRSTMT, line);
            stmt2->a = cmd;
            return stmt2;
        }

        // No era una asignacion -- era una llamada a funcion usada
        // como sentencia (ej. "MiProcedimiento(1,2)" sin usar su
        // resultado). Reempaquetamos 'idx' como N_CALL para que el
        // generador de codigo lo trate como tal.
        Node *call = node_new(N_CALL, line);
        nb_strncpy(call->text, name, sizeof(call->text) - 1);
        call->list = idx->list;
        call->list_count = idx->list_count;
        Node *stmt = node_new(N_EXPRSTMT, line);
        stmt->a = call;
        return stmt;
    }

    if (check(p, T_BACKSLASH)) {
        Node *n = node_new(N_VAR, line);
        nb_strncpy(n->text, name, sizeof(n->text) - 1);
        while (check(p, T_BACKSLASH)) {
            advance(p);
            if (!check(p, T_IDENT)) error_at(p, "se esperaba un nombre de campo tras '\\'");
            Node *fn = node_new(N_FIELD, line);
            nb_strncpy(fn->text, p->cur.text, sizeof(fn->text) - 1);
            fn->a = n;
            advance(p);
            n = fn;
        }
        if (match(p, T_EQ)) {
            Node *asn = node_new(N_ASSIGN, line);
            asn->a = n;
            asn->b = parse_expr(p);
            return asn;
        }
        Node *stmt = node_new(N_EXPRSTMT, line);
        stmt->a = n;
        return stmt;
    }

    if (match(p, T_EQ)) {
        Node *var = node_new(N_VAR, line);
        nb_strncpy(var->text, name, sizeof(var->text) - 1);
        Node *n = node_new(N_ASSIGN, line);
        n->a = var;
        n->b = parse_expr(p);
        return n;
    }

    // Ni indexado ni asignacion -- una llamada de "comando" al estilo
    // BASIC clasico, sin parentesis: "Foo" sola, o "Foo arg1, arg2"
    // con cero o mas argumentos separados por comas.
    Node *var = node_new(N_CALL, line);
    nb_strncpy(var->text, name, sizeof(var->text) - 1);
    if (!check(p, T_NEWLINE) && !check(p, T_COLON) && !check(p, T_EOF) && !at_block_end(p)) {
        node_list_add(var, parse_expr(p));
        while (match(p, T_COMMA)) node_list_add(var, parse_expr(p));
    }
    Node *stmt = node_new(N_EXPRSTMT, line);
    stmt->a = var;
    return stmt;
}

static Node *parse_if(Parser *p) {
    int line = p->cur.line;
    advance(p); // If
    Node *n = node_new(N_IF, line);
    n->a = parse_expr(p);
    match(p, T_KW_THEN); // 'Then' es OPCIONAL en BlitzPlus real

    // BlitzPlus admite dos formas: "If x Then stmt" (una linea) o
    // bloque multilinea terminado en EndIf. Si tras 'Then' viene un
    // fin de linea, es la forma de bloque.
    if (check(p, T_NEWLINE)) {
        n->b = parse_block(p);

        while (check(p, T_KW_ELSEIF)) {
            int eline = p->cur.line;
            advance(p);
            Node *ei = node_new(N_ELSEIF, eline);
            ei->a = parse_expr(p);
            match(p, T_KW_THEN); // opcional, igual que en If
            skip_separators(p);
            ei->b = parse_block(p);
            node_list_add(n, ei);
        }

        if (match(p, T_KW_ELSE)) {
            skip_separators(p);
            n->c = parse_block(p);
        }

        expect(p, T_KW_ENDIF, "se esperaba 'EndIf'");
    } else {
        // Forma de una linea: "If x Then stmt1[:stmt2...] [Else stmt3[:stmt4...]]"
        Node *block = node_new(N_BLOCK, line);
        node_list_add(block, parse_assign_or_expr_stmt_or_command(p));
        while (match(p, T_COLON) && !check(p, T_KW_ELSE)) {
            node_list_add(block, parse_assign_or_expr_stmt_or_command(p));
        }
        n->b = block;

        if (match(p, T_KW_ELSE)) {
            Node *eblock = node_new(N_BLOCK, line);
            node_list_add(eblock, parse_assign_or_expr_stmt_or_command(p));
            while (match(p, T_COLON)) {
                node_list_add(eblock, parse_assign_or_expr_stmt_or_command(p));
            }
            n->c = eblock;
        }
    }
    return n;
}

static Node *parse_for(Parser *p) {
    int line = p->cur.line;
    advance(p); // For
    if (!check(p, T_IDENT)) error_at(p, "se esperaba el nombre de la variable del bucle");
    char raw_name[64];
    nb_strncpy(raw_name, p->cur.text, sizeof(raw_name) - 1);
    raw_name[sizeof(raw_name) - 1] = '\0';
    char varname[64];
    split_dot(raw_name, varname, sizeof(varname));
    advance(p);
    expect(p, T_EQ, "se esperaba '=' tras la variable del bucle");

    if (check(p, T_KW_EACH)) {
        advance(p); // Each
        if (!check(p, T_IDENT)) error_at(p, "se esperaba el nombre del tipo tras 'Each'");
        Node *n = node_new(N_FOREACH, line);
        nb_strncpy(n->text, varname, sizeof(n->text) - 1);
        Node *typenode = node_new(N_VAR, line);
        nb_strncpy(typenode->text, p->cur.text, sizeof(typenode->text) - 1);
        n->a = typenode;
        advance(p);
        skip_separators(p);
        n->b = parse_block(p);
        expect(p, T_KW_NEXT, "se esperaba 'Next'");
        if (check(p, T_IDENT)) advance(p); // "Next variable" opcional
        return n;
    }

    Node *n = node_new(N_FOR, line);
    nb_strncpy(n->text, varname, sizeof(n->text) - 1);
    n->a = parse_expr(p);
    expect(p, T_KW_TO, "se esperaba 'To'");
    n->b = parse_expr(p);
    if (match(p, T_KW_STEP)) {
        n->c = parse_expr(p);
    }
    skip_separators(p);
    n->d = parse_block(p);
    expect(p, T_KW_NEXT, "se esperaba 'Next'");
    return n;
}

static Node *parse_while(Parser *p) {
    int line = p->cur.line;
    advance(p); // While
    Node *n = node_new(N_WHILE, line);
    n->a = parse_expr(p);
    skip_separators(p);
    n->b = parse_block(p);
    expect(p, T_KW_WEND, "se esperaba 'Wend'");
    return n;
}

static Node *parse_repeat(Parser *p) {
    int line = p->cur.line;
    advance(p); // Repeat
    Node *n = node_new(N_REPEAT, line);
    skip_separators(p);
    n->a = parse_block(p);
    if (match(p, T_KW_UNTIL)) {
        n->b = parse_expr(p);
    } else {
        expect(p, T_KW_FOREVER, "se esperaba 'Until' o 'Forever'");
    }
    return n;
}

static Node *parse_funcdef(Parser *p) {
    int line = p->cur.line;
    advance(p); // Function
    if (!check(p, T_IDENT)) error_at(p, "se esperaba el nombre de la funcion");
    Node *n = node_new(N_FUNCDEF, line);
    nb_strncpy(n->text, p->cur.text, sizeof(n->text) - 1);
    advance(p);
    expect(p, T_LPAREN, "se esperaba '(' tras el nombre de la funcion");
    if (!check(p, T_RPAREN)) {
        Node *param = node_new(N_VAR, p->cur.line);
        if (!check(p, T_IDENT)) error_at(p, "se esperaba un nombre de parametro");
        nb_strncpy(param->text, p->cur.text, sizeof(param->text) - 1);
        advance(p);
        if (match(p, T_EQ)) param->b = parse_expr(p);
        node_list_add(n, param);
        while (match(p, T_COMMA)) {
            Node *pn = node_new(N_VAR, p->cur.line);
            if (!check(p, T_IDENT)) error_at(p, "se esperaba un nombre de parametro");
            nb_strncpy(pn->text, p->cur.text, sizeof(pn->text) - 1);
            advance(p);
            if (match(p, T_EQ)) pn->b = parse_expr(p);
            node_list_add(n, pn);
        }
    }
    expect(p, T_RPAREN, "se esperaba ')'");
    skip_separators(p);
    n->d = parse_block(p);
    expect(p, T_KW_ENDFUNCTION, "se esperaba 'End Function'");
    return n;
}

static Node *parse_vardecl(Parser *p, TokenType kind_tok) {
    int line = p->cur.line;
    advance(p); // Global/Local/Const
    Node *n = node_new(N_VARDECL, line);
    n->op = kind_tok;
    for (;;) {
        if (!check(p, T_IDENT)) error_at(p, "se esperaba un nombre de variable");
        Node *item = node_new(N_ASSIGN, p->cur.line);
        Node *var = node_new(N_VAR, p->cur.line);
        nb_strncpy(var->text, p->cur.text, sizeof(var->text) - 1);
        item->a = var;
        advance(p);
        if (match(p, T_EQ)) {
            item->b = parse_expr(p);
        }
        node_list_add(n, item);
        if (!match(p, T_COMMA)) break;
    }
    return n;
}

static Node *parse_one_dim(Parser *p) {
    if (!check(p, T_IDENT)) error_at(p, "se esperaba un nombre de array");
    Node *n = node_new(N_DIM, p->cur.line);
    nb_strncpy(n->text, p->cur.text, sizeof(n->text) - 1);
    advance(p);
    expect(p, T_LPAREN, "se esperaba '(' con el tamaño del array");
    node_list_add(n, parse_expr(p));
    while (match(p, T_COMMA)) node_list_add(n, parse_expr(p));
    expect(p, T_RPAREN, "se esperaba ')'");
    return n;
}

static Node *parse_dim(Parser *p) {
    int line = p->cur.line;
    advance(p); // Dim
    Node *first = parse_one_dim(p);
    if (!check(p, T_COMMA)) return first;
    Node *block = node_new(N_BLOCK, line);
    node_list_add(block, first);
    while (match(p, T_COMMA)) node_list_add(block, parse_one_dim(p));
    return block;
}

// ---- Type / Field / New / Delete / First / Last / Each ----
static void split_dot(const char *full, char *name_out, uint32_t name_out_size) {
    uint32_t i = 0;
    while (full[i] != '\0' && full[i] != '.' && i + 1 < name_out_size) {
        name_out[i] = full[i];
        i++;
    }
    name_out[i] = '\0';
}

static Node *parse_typedef(Parser *p) {
    int line = p->cur.line;
    advance(p); // Type
    if (!check(p, T_IDENT)) error_at(p, "se esperaba el nombre del tipo");
    Node *n = node_new(N_TYPEDEF, line);
    nb_strncpy(n->text, p->cur.text, sizeof(n->text) - 1);
    advance(p);
    skip_separators(p);
    while (check(p, T_KW_FIELD)) {
        advance(p); // Field
        for (;;) {
            if (!check(p, T_IDENT)) error_at(p, "se esperaba un nombre de campo");
            Node *f = node_new(N_VAR, p->cur.line);
            nb_strncpy(f->text, p->cur.text, sizeof(f->text) - 1);
            advance(p);
            node_list_add(n, f);
            if (!match(p, T_COMMA)) break;
        }
        skip_separators(p);
    }
    expect(p, T_KW_ENDTYPE, "se esperaba 'End Type'");
    return n;
}

static Node *parse_command_with_args(Parser *p, NodeKind kind, int min_args) {
    int line = p->cur.line;
    advance(p); // el propio comando (Print/Cls/Plot/Line/Rect)

    Parser saved = *p;
    bool has_parens = match(p, T_LPAREN);
    Node *n = node_new(kind, line);
    if (min_args > 0 || !check(p, T_NEWLINE)) {
        if (!check(p, T_NEWLINE) && !check(p, T_COLON) && !check(p, T_RPAREN)) {
            node_list_add(n, parse_expr(p));
            while (match(p, T_COMMA)) node_list_add(n, parse_expr(p));
        }
    }
    if (has_parens) {
        expect(p, T_RPAREN, "se esperaba ')'");
        if (check(p, T_STAR) || check(p, T_SLASH) || check(p, T_PLUS) || check(p, T_MINUS) ||
            check(p, T_KW_MOD) || check(p, T_KW_AND) || check(p, T_KW_OR) ||
            check(p, T_LT) || check(p, T_GT) || check(p, T_LE) || check(p, T_GE) ||
            check(p, T_NE) || check(p, T_EQ)) {
            *p = saved;
            Node *n2 = node_new(kind, line);
            node_list_add(n2, parse_expr(p));
            while (match(p, T_COMMA)) node_list_add(n2, parse_expr(p));
            return n2;
        }
    }
    return n;
}

// Sentencia dentro de un bloque, o en la forma de una linea de If.
// Separado de parse_statement para poder reutilizarlo desde el 'If'
// de una sola linea sin arrastrar el manejo de bloques enteros.
static Node *parse_assign_or_expr_stmt_or_command(Parser *p) {
    switch (p->cur.type) {
        case T_KW_PRINT: return parse_command_with_args(p, N_PRINT, 1);
        case T_KW_CLS:    return parse_command_with_args(p, N_CLS, 0);
        case T_KW_PLOT:   return parse_command_with_args(p, N_PLOT, 2);
        case T_KW_LINE:   return parse_command_with_args(p, N_LINE, 4);
        case T_KW_RECT:   return parse_command_with_args(p, N_RECT, 4);
        case T_KW_DELAY:  return parse_command_with_args(p, N_DELAY, 1);
        case T_KW_RETURN: {
            int line = p->cur.line;
            advance(p);
            Node *n = node_new(N_RETURN, line);
            if (!check(p, T_NEWLINE) && !check(p, T_COLON) && !check(p, T_EOF)) {
                n->a = parse_expr(p);
            }
            return n;
        }
        case T_KW_EXIT: {
            int line = p->cur.line;
            advance(p);
            return node_new(N_EXIT, line);
        }
        case T_KW_END: {
            int line = p->cur.line;
            advance(p);
            return node_new(N_ENDPROGRAM, line);
        }
        case T_KW_GOTO: {
            int line = p->cur.line;
            advance(p);
            if (!check(p, T_IDENT)) error_at(p, "se esperaba un nombre de etiqueta tras 'Goto'");
            Node *n = node_new(N_GOTO, line);
            nb_strncpy(n->text, p->cur.text, sizeof(n->text) - 1);
            advance(p);
            return n;
        }
        case T_KW_GOSUB: {
            int line = p->cur.line;
            advance(p);
            if (!check(p, T_IDENT)) error_at(p, "se esperaba un nombre de etiqueta tras 'Gosub'");
            Node *n = node_new(N_GOSUB, line);
            nb_strncpy(n->text, p->cur.text, sizeof(n->text) - 1);
            advance(p);
            return n;
        }
        case T_KW_INSERT: {
            int line = p->cur.line;
            advance(p); // Insert
            Node *n = node_new(N_INSERT, line);
            n->a = parse_expr(p);
            if (!check(p, T_KW_BEFORE) && !check(p, T_KW_AFTER)) {
                error_at(p, "se esperaba 'Before' o 'After' tras 'Insert <instancia>'");
            }
            n->op = p->cur.type;
            advance(p);
            n->b = parse_expr(p);
            return n;
        }
        case T_IDENT: return parse_assign_or_expr_stmt(p);
        default:
            error_at(p, "sentencia no reconocida");
            return NULL;
    }
}

// ---- Select / Case ----
//
// No añadimos NADA al generador de codigo para esto -- lo traducimos
// aqui mismo, en el parser, a la cadena de If/ElseIf/Else que ya
// sabe generar codigo. La unica sutileza es evaluar la expresion del
// Select UNA SOLA VEZ (por si tiene efectos secundarios, como una
// llamada a funcion) guardandola en una variable temporal sintetica,
// y comparar esa variable en cada Case.

static int select_tmp_counter = 0;

static Node *make_var_ref(int line, const char *name) {
    Node *v = node_new(N_VAR, line);
    nb_strncpy(v->text, name, sizeof(v->text) - 1);
    return v;
}

// "Case v1, v2, ..." -> "tmp = v1 Or tmp = v2 Or ..."
static Node *parse_case_condition(Parser *p, const char *tmp_name) {
    Node *cond = NULL;
    for (;;) {
        int line = p->cur.line;
        Node *cmp = node_new(N_BINOP, line);
        cmp->op = T_EQ;
        cmp->a = make_var_ref(line, tmp_name);
        cmp->b = parse_expr(p);
        if (cond == NULL) {
            cond = cmp;
        } else {
            Node *orn = node_new(N_BINOP, line);
            orn->op = T_KW_OR;
            orn->a = cond;
            orn->b = cmp;
            cond = orn;
        }
        if (!match(p, T_COMMA)) break;
    }
    return cond;
}

static Node *parse_select(Parser *p) {
    int line = p->cur.line;
    advance(p); // Select
    Node *sel_expr = parse_expr(p);
    skip_separators(p);

    char tmp_name[32];
    const char *prefix = "__SELTMP";
    int ti = 0;
    while (prefix[ti]) { tmp_name[ti] = prefix[ti]; ti++; }
    char numbuf[12];
    nb_itoa(select_tmp_counter++, numbuf, sizeof(numbuf));
    int ni = 0;
    while (numbuf[ni] && ti < 30) { tmp_name[ti++] = numbuf[ni++]; }
    tmp_name[ti] = '\0';

    Node *assign = node_new(N_ASSIGN, line);
    assign->a = make_var_ref(line, tmp_name);
    assign->b = sel_expr;

    Node *ifn = NULL;
    bool first = true;

    while (check(p, T_KW_CASE)) {
        int cline = p->cur.line;
        advance(p); // Case
        Node *cond = parse_case_condition(p, tmp_name);
        skip_separators(p);
        Node *block = parse_block(p);

        if (first) {
            ifn = node_new(N_IF, cline);
            ifn->a = cond;
            ifn->b = block;
            first = false;
        } else {
            Node *ei = node_new(N_ELSEIF, cline);
            ei->a = cond;
            ei->b = block;
            node_list_add(ifn, ei);
        }
    }

    if (ifn == NULL) {
        ifn = node_new(N_IF, line);
        Node *falsecond = node_new(N_NUM, line);
        falsecond->num_value = 0;
        ifn->a = falsecond;
        ifn->b = node_new(N_BLOCK, line);
    }

    if (match(p, T_KW_DEFAULT)) {
        skip_separators(p);
        ifn->c = parse_block(p);
    }

    expect(p, T_KW_ENDSELECT, "se esperaba 'End Select'");

    Node *wrapper = node_new(N_BLOCK, line);
    node_list_add(wrapper, assign);
    node_list_add(wrapper, ifn);
    return wrapper;
}

// ---- Data / Read / Restore ----

static Node *parse_data(Parser *p) {
    int line = p->cur.line;
    advance(p); // Data
    Node *n = node_new(N_DATA, line);
    node_list_add(n, parse_unary(p));
    while (match(p, T_COMMA)) node_list_add(n, parse_unary(p));
    return n;
}

static Node *parse_read(Parser *p) {
    int line = p->cur.line;
    advance(p); // Read
    if (!check(p, T_IDENT)) error_at(p, "se esperaba un nombre de variable tras 'Read'");
    Node *n = node_new(N_READ, line);
    char name[64];
    nb_strncpy(name, p->cur.text, sizeof(name) - 1);
    advance(p);
    if (check(p, T_LPAREN)) {
        advance(p);
        Node *idx = node_new(N_INDEX, line);
        nb_strncpy(idx->text, name, sizeof(idx->text) - 1);
        node_list_add(idx, parse_expr(p));
        while (match(p, T_COMMA)) node_list_add(idx, parse_expr(p));
        expect(p, T_RPAREN, "se esperaba ')' tras los indices");
        n->a = idx;
    } else {
        Node *var = node_new(N_VAR, line);
        nb_strncpy(var->text, name, sizeof(var->text) - 1);
        n->a = var;
    }
    return n;
}

static Node *parse_restore(Parser *p) {
    int line = p->cur.line;
    advance(p); // Restore
    Node *n = node_new(N_RESTORE, line);
    if (check(p, T_IDENT)) {
        nb_strncpy(n->text, p->cur.text, sizeof(n->text) - 1);
        advance(p);
    }
    return n;
}

static Node *parse_datalabel(Parser *p) {
    Node *n = node_new(N_DATALABEL, p->cur.line);
    nb_strncpy(n->text, p->cur.text, sizeof(n->text) - 1);
    advance(p);
    return n;
}

static Node *parse_statement(Parser *p) {
    switch (p->cur.type) {
        case T_KW_IF:       return parse_if(p);
        case T_KW_SELECT:   return parse_select(p);
        case T_KW_FOR:      return parse_for(p);
        case T_KW_WHILE:    return parse_while(p);
        case T_KW_REPEAT:   return parse_repeat(p);
        case T_KW_FUNCTION: return parse_funcdef(p);
        case T_KW_GLOBAL:
        case T_KW_LOCAL:
        case T_KW_CONST:    return parse_vardecl(p, p->cur.type);
        case T_KW_DIM:      return parse_dim(p);
        case T_KW_TYPE:     return parse_typedef(p);
        case T_KW_DELETE: {
            int line = p->cur.line;
            advance(p);
            Node *n = node_new(N_DELETE, line);
            n->a = parse_expr(p);
            return n;
        }
        case T_KW_DATA:     return parse_data(p);
        case T_KW_READ:     return parse_read(p);
        case T_KW_RESTORE:  return parse_restore(p);
        case T_DATALABEL:   return parse_datalabel(p);
        case T_KW_END: {
            int line = p->cur.line;
            advance(p);
            return node_new(N_ENDPROGRAM, line);
        }
        default:            return parse_assign_or_expr_stmt_or_command(p);
    }
}

// Palabras clave que SIERRAN un bloque -- parse_block se detiene al
// verlas (sin consumirlas; quien llamo a parse_block se encarga de
// consumir el cierre correspondiente, para poder dar un mensaje de
// error mas preciso si no coincide).
static bool at_block_end(Parser *p) {
    switch (p->cur.type) {
        case T_EOF:
        case T_KW_ENDIF: case T_KW_ELSE: case T_KW_ELSEIF:
        case T_KW_NEXT: case T_KW_WEND:
        case T_KW_UNTIL: case T_KW_FOREVER:
        case T_KW_ENDFUNCTION:
        case T_KW_CASE: case T_KW_DEFAULT: case T_KW_ENDSELECT:
            return true;
        default:
            return false;
    }
}

static Node *parse_block(Parser *p) {
    Node *block = node_new(N_BLOCK, p->cur.line);
    skip_separators(p);
    while (!at_block_end(p)) {
        node_list_add(block, parse_statement(p));
        if (!check(p, T_NEWLINE) && !check(p, T_COLON) && !at_block_end(p)) {
            error_at(p, "se esperaba fin de linea o ':' entre sentencias");
        }
        skip_separators(p);
    }
    return block;
}

Node *parse_program(const char *source) {
    Parser p;
    lexer_init(&p.lx, source);
    advance(&p);

    Node *program = parse_block(&p);
    if (p.cur.type != T_EOF) {
        error_at(&p, "se esperaba el fin del archivo");
    }
    return program;
}
