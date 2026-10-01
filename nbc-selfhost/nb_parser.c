// nb_parser.c — parser de EXPRESIONES de Nemo-Blitz 2.0 (primera
// pieza testable; las sentencias van en un archivo aparte, encima de
// esta). Misma cadena de precedencia que el compilador viejo, de
// menos a más fuerte:
//
//   Or > Xor > And > comparacion(=,<,>,<=,>=,<>) > suma/resta >
//   mul/div/Mod/Shl/Shr/Sar > unario(-, Not) > primario
//
// Disciplina de propiedad de nb_string_t: cada token con texto
// (identificador, cadena) se "reclama" exactamente una vez -- su
// puntero pasa a vivir dentro de un nodo del arbol, y nb_advance() NO
// libera nada por su cuenta. Es responsabilidad de quien consume un
// token con texto que no va a guardar en ningun sitio soltarlo
// explicitamente antes de avanzar (en esta gramatica de expresiones,
// no se da el caso: todo identificador/cadena acaba en un nodo).

#include "nb_ast.h"
#include "nb_lexer.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern bool nb_string_eq(nb_string_t *a, nb_string_t *b);   // nb_string.c

// Nombres declarados con Const, para rechazar cualquier asignacion
// posterior. Se comprueba aqui, en el analizador, porque es el que ve
// el programa en el orden en que esta escrito. Se vacia al empezar cada
// programa (nb_parse_program). Los nombres son los del propio arbol,
// que vive hasta despues de generar el codigo.
#define NB_MAX_CONSTANTES 256
static nb_string_t *nb_constantes[NB_MAX_CONSTANTES];
static int32_t nb_num_constantes;

static bool nb_es_constante(nb_string_t *nombre) {
    for (int32_t i = 0; i < nb_num_constantes; i++) {
        if (nb_string_eq(nb_constantes[i], nombre)) return true;
    }
    return false;
}


extern void nb_string_release(nb_string_t *s);
extern nb_string_t *nb_string_new(const char *data, uint32_t len);

static uint32_t nb_cstrlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

typedef struct {
    nb_lexer_t lx;
    nb_token_t cur;
    bool had_error;
    const char *error_msg; // mensaje fijo (literal C, no nb_string_t) de el ultimo error
    int32_t error_line;
    // ¿Estamos dentro del cuerpo de una Function? Hace falta para
    // rechazar Gosub ahi: el Return de la subrutina y el Return de la
    // funcion son la misma palabra, y el generador no puede
    // distinguirlos. Antes se aceptaba y el Gosub se comportaba mal en
    // silencio.
    bool in_function;
} nb_parser_t;

void nb_parser_init(nb_parser_t *p, const char *source) {
    nb_lexer_init(&p->lx, source);
    p->cur = nb_lexer_next(&p->lx);
    p->had_error = false;
    p->in_function = false;
    p->error_msg = NULL;
    p->error_line = 0;
}

static void nb_advance(nb_parser_t *p) {
    p->cur = nb_lexer_next(&p->lx);
}

static bool nb_check(nb_parser_t *p, nb_token_type_t t) {
    return p->cur.type == t;
}

static bool nb_match(nb_parser_t *p, nb_token_type_t t) {
    if (nb_check(p, t)) { nb_advance(p); return true; }
    return false;
}

// Marca el primer error encontrado (los siguientes se ignoran -- ya
// hay uno que reportar) y NO aborta el proceso: cada funcion de parseo
// devuelve NULL en cuanto ve had_error, para que el fallo se propague
// hacia arriba de forma ordenada en vez de seguir leyendo tokens que
// ya no tienen sentido. El manejo de errores REAL del lenguaje
// (Try/Catch, esteroide #1 del diseño) es una pieza futura; esto es
// solo el error de SINTAXIS del propio compilador, siempre existio
// aparte de eso.
static void nb_error_at(nb_parser_t *p, const char *msg) {
    if (p->had_error) return;
    p->had_error = true;
    p->error_msg = msg;
    p->error_line = p->cur.line;
}

static void nb_expect(nb_parser_t *p, nb_token_type_t t, const char *msg) {
    if (!nb_check(p, t)) { nb_error_at(p, msg); return; }
    nb_advance(p);
}

static nb_node_t *nb_parse_expr(nb_parser_t *p);

static nb_node_t *nb_parse_primary(nb_parser_t *p) {
    int32_t line = p->cur.line;

    if (p->had_error) return NULL;

    if (nb_check(p, TOK_NUMBER)) {
        nb_node_t *n = nb_node_new(N_NUM, line);
        n->num_value = p->cur.num_value;
        n->op = p->cur.is_float ? 1 : 0; // ver la nota en nb_ast.h: 1 = literal con punto decimal
        nb_advance(p);
        return n;
    }
    if (nb_check(p, TOK_STRING)) {
        nb_node_t *n = nb_node_new(N_STR, line);
        n->text = p->cur.text; // se reclama el texto -- el nodo es ahora su dueño
        nb_advance(p);
        return n;
    }
    if (nb_check(p, TOK_KW_TRUE) || nb_check(p, TOK_KW_FALSE)) {
        nb_node_t *n = nb_node_new(N_NUM, line);
        // True vale -1 (todos los bits), no 1, para ir con el mismo
        // convenio que las comparaciones -- ver la nota en
        // nb_codegen.c. Asi "x And True" conserva x.
        n->num_value = nb_check(p, TOK_KW_TRUE) ? -1 : 0;
        nb_advance(p);
        return n;
    }
    if (nb_check(p, TOK_KW_NULL)) {
        nb_node_t *n = nb_node_new(N_NUM, line);
        n->num_value = 0;
        nb_advance(p);
        return n;
    }
    if (nb_check(p, TOK_KW_NEW)) {
        nb_advance(p);
        if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba el nombre del tipo tras 'New'"); return NULL; }
        nb_node_t *n = nb_node_new(N_NEW, line);
        n->text = p->cur.text;
        nb_advance(p);
        return n;
    }
    if (nb_check(p, TOK_KW_FIRST) || nb_check(p, TOK_KW_LAST)) {
        bool is_first = nb_check(p, TOK_KW_FIRST);
        nb_advance(p);
        bool has_parens = nb_match(p, TOK_LPAREN);
        if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba el nombre del tipo"); return NULL; }
        nb_node_t *n = nb_node_new(N_FIRSTLAST, line);
        n->op = is_first ? TOK_KW_FIRST : TOK_KW_LAST;
        n->text = p->cur.text;
        nb_advance(p);
        if (has_parens) nb_expect(p, TOK_RPAREN, "se esperaba ')'");

        return n;
    }
    if (nb_check(p, TOK_KW_BEFORE) || nb_check(p, TOK_KW_AFTER)) {
        bool is_before = nb_check(p, TOK_KW_BEFORE);
        nb_advance(p);
        bool has_parens = nb_match(p, TOK_LPAREN);
        nb_node_t *n = nb_node_new(is_before ? N_BEFORE : N_AFTER, line);
        n->a = nb_parse_expr(p);
        if (has_parens) nb_expect(p, TOK_RPAREN, "se esperaba ')'");
        return n;
    }
    if (nb_check(p, TOK_LPAREN)) {
        nb_advance(p);
        nb_node_t *n = nb_parse_expr(p);
        nb_expect(p, TOK_RPAREN, "se esperaba ')'");
        return n;
    }
    if (nb_check(p, TOK_IDENT)) {
        nb_string_t *name = p->cur.text; // se reclama
        nb_advance(p);

        if (nb_check(p, TOK_LPAREN)) {
            nb_advance(p);
            // Ambiguo entre "llamada a funcion" e "indexado de array"
            // a nivel de sintaxis, igual que en el compilador viejo --
            // el generador de codigo decide segun lo que 'name' tenga
            // declarado.
            nb_node_t *n = nb_node_new(N_CALL, line);
            n->text = name;
            if (!nb_check(p, TOK_RPAREN)) {
                nb_node_t *arg = nb_parse_expr(p);
                if (p->had_error) return NULL;
                nb_node_list_add(n, arg);
                while (nb_match(p, TOK_COMMA)) {
                    arg = nb_parse_expr(p);
                    if (p->had_error) return NULL;
                    nb_node_list_add(n, arg);
                }
            }
            nb_expect(p, TOK_RPAREN, "se esperaba ')' tras los argumentos");
            return n;
        }

        nb_node_t *n = nb_node_new(N_VAR, line);
        n->text = name;

        while (nb_check(p, TOK_BACKSLASH)) {
            nb_advance(p);
            if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba un nombre de campo tras '\\'"); return NULL; }
            nb_node_t *fn = nb_node_new(N_FIELD, line);
            fn->text = p->cur.text; // se reclama
            fn->a = n;
            nb_advance(p);
            n = fn;
        }
        return n;
    }

    nb_error_at(p, "se esperaba un numero, cadena, variable o '('");
    return NULL;
}

static nb_node_t *nb_parse_unary(nb_parser_t *p) {
    int32_t line = p->cur.line;
    if (nb_check(p, TOK_MINUS) || nb_check(p, TOK_KW_NOT)) {
        int32_t op = p->cur.type;
        nb_advance(p);
        nb_node_t *n = nb_node_new(N_UNOP, line);
        n->op = op;
        n->a = nb_parse_unary(p);
        return n;
    }
    return nb_parse_primary(p);
}

static nb_node_t *nb_parse_mul(nb_parser_t *p) {
    nb_node_t *left = nb_parse_unary(p);
    while (!p->had_error && (nb_check(p, TOK_STAR) || nb_check(p, TOK_SLASH) || nb_check(p, TOK_KW_MOD) ||
           nb_check(p, TOK_KW_SHL) || nb_check(p, TOK_KW_SHR) || nb_check(p, TOK_KW_SAR))) {
        int32_t op = p->cur.type, line = p->cur.line;
        nb_advance(p);
        nb_node_t *n = nb_node_new(N_BINOP, line);
        n->op = op; n->a = left; n->b = nb_parse_unary(p);
        left = n;
    }
    return left;
}

static nb_node_t *nb_parse_add(nb_parser_t *p) {
    nb_node_t *left = nb_parse_mul(p);
    while (!p->had_error && (nb_check(p, TOK_PLUS) || nb_check(p, TOK_MINUS))) {
        int32_t op = p->cur.type, line = p->cur.line;
        nb_advance(p);
        nb_node_t *n = nb_node_new(N_BINOP, line);
        n->op = op; n->a = left; n->b = nb_parse_mul(p);
        left = n;
    }
    return left;
}

static nb_node_t *nb_parse_comparison(nb_parser_t *p) {
    nb_node_t *left = nb_parse_add(p);
    while (!p->had_error && (nb_check(p, TOK_EQ) || nb_check(p, TOK_LT) || nb_check(p, TOK_GT) ||
           nb_check(p, TOK_LE) || nb_check(p, TOK_GE) || nb_check(p, TOK_NE))) {
        int32_t op = p->cur.type, line = p->cur.line;
        nb_advance(p);
        nb_node_t *n = nb_node_new(N_BINOP, line);
        n->op = op; n->a = left; n->b = nb_parse_add(p);
        left = n;
    }
    return left;
}

static nb_node_t *nb_parse_and(nb_parser_t *p) {
    nb_node_t *left = nb_parse_comparison(p);
    while (!p->had_error && nb_check(p, TOK_KW_AND)) {
        int32_t line = p->cur.line;
        nb_advance(p);
        nb_node_t *n = nb_node_new(N_BINOP, line);
        n->op = TOK_KW_AND; n->a = left; n->b = nb_parse_comparison(p);
        left = n;
    }
    return left;
}

static nb_node_t *nb_parse_xor(nb_parser_t *p) {
    nb_node_t *left = nb_parse_and(p);
    while (!p->had_error && nb_check(p, TOK_KW_XOR)) {
        int32_t line = p->cur.line;
        nb_advance(p);
        nb_node_t *n = nb_node_new(N_BINOP, line);
        n->op = TOK_KW_XOR; n->a = left; n->b = nb_parse_and(p);
        left = n;
    }
    return left;
}

static nb_node_t *nb_parse_or(nb_parser_t *p) {
    nb_node_t *left = nb_parse_xor(p);
    while (!p->had_error && nb_check(p, TOK_KW_OR)) {
        int32_t line = p->cur.line;
        nb_advance(p);
        nb_node_t *n = nb_node_new(N_BINOP, line);
        n->op = TOK_KW_OR; n->a = left; n->b = nb_parse_xor(p);
        left = n;
    }
    return left;
}

static nb_node_t *nb_parse_expr(nb_parser_t *p) { return nb_parse_or(p); }

// ---------------------------------------------------------------
// Sentencias. Cubre esta pieza: bloques, asignacion (variable/
// indexado de array/campo de Type), llamadas usadas como sentencia,
// If/For/While/Repeat, Print/Cls/Plot/Line/Rect/Delay,
// Return/Exit/End/Goto/Gosub. Se deja para un siguiente paso, por
// higiene de tamaño de la pieza (probar esto a fondo antes de seguir
// construyendo encima): Function, Type/Field/New/Delete/Insert/
// Before/After/First/Last/Each, Select/Case, Data/Read/Restore,
// Global/Local/Const/Dim, etiquetas de Goto.
//
// Simplificacion deliberada frente al compilador viejo, documentada
// aqui (no silenciosa): el viejo tenia un mecanismo de "guardar el
// estado del parser, probar una interpretacion, deshacerla si no
// encaja" para el caso ambiguo "Nombre(args) SEGUIDO de un operador"
// usado como sentencia suelta (ni asignacion ni llamada clara) --
// ademas de complicado, ese "deshacer" dejaba sin liberar cualquier
// nodo/cadena creado durante el intento abandonado (una fuga
// pequeña pero real, del mismo tipo que esta reescritura existe para
// eliminar). Aqui, "Nombre(args)" sin '=' detras se trata SIEMPRE
// como una llamada (N_CALL) -- cubre con corrección el caso real
// (llamar a un procedimiento, o leer un array sin usar el resultado,
// que es un no-operacion de todos modos); si esto resulta demasiado
// estricto para algun programa real ya escrito, se revisa entonces,
// con un caso concreto delante en vez de una regla general fragil.

// Palabras clave que CIERRAN un bloque -- parse_block se detiene al
// verlas, sin consumirlas (quien llamo a parse_block decide como
// cerrarlo, para poder dar un error mas preciso si no coincide).
static bool nb_at_block_end(nb_parser_t *p) {
    switch (p->cur.type) {
        case TOK_EOF:
        case TOK_KW_ENDIF: case TOK_KW_ELSE: case TOK_KW_ELSEIF:
        case TOK_KW_NEXT: case TOK_KW_WEND:
        case TOK_KW_UNTIL: case TOK_KW_FOREVER:
        case TOK_KW_ENDFUNCTION:
        case TOK_KW_ENDTYPE:
        case TOK_KW_CASE: case TOK_KW_DEFAULT: case TOK_KW_ENDSELECT:
            return true;
        default:
            return false;
    }
}

static void nb_skip_separators(nb_parser_t *p) {
    while (nb_check(p, TOK_NEWLINE) || nb_check(p, TOK_COLON)) nb_advance(p);
}

static nb_node_t *nb_parse_block(nb_parser_t *p);
static nb_node_t *nb_parse_statement(nb_parser_t *p);

// Sentencia que empieza por un identificador: campo de Type, indexado
// de array, variable, o llamada/comando sin parentesis. Ver la nota
// de simplificacion mas arriba.
static nb_node_t *nb_parse_ident_stmt(nb_parser_t *p) {
    int32_t line = p->cur.line;
    nb_string_t *name = p->cur.text; // se reclama
    nb_advance(p);

    if (nb_check(p, TOK_BACKSLASH)) {
        nb_node_t *n = nb_node_new(N_VAR, line);
        n->text = name;
        while (nb_check(p, TOK_BACKSLASH)) {
            nb_advance(p);
            if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba un nombre de campo tras '\\'"); return NULL; }
            nb_node_t *fn = nb_node_new(N_FIELD, line);
            fn->text = p->cur.text; fn->a = n;
            nb_advance(p);
            n = fn;
        }
        if (nb_match(p, TOK_EQ)) {
            nb_node_t *asn = nb_node_new(N_ASSIGN, line);
            asn->a = n; asn->b = nb_parse_expr(p);
            return asn;
        }
        nb_node_t *stmt = nb_node_new(N_EXPRSTMT, line);
        stmt->a = n;
        return stmt;
    }

    if (nb_check(p, TOK_LPAREN)) {
        nb_advance(p);
        nb_node_t *n = nb_node_new(N_INDEX, line);
        n->text = name;
        if (!nb_check(p, TOK_RPAREN)) {
            nb_node_list_add(n, nb_parse_expr(p));
            while (nb_match(p, TOK_COMMA)) nb_node_list_add(n, nb_parse_expr(p));
        }
        nb_expect(p, TOK_RPAREN, "se esperaba ')' tras los indices");
        if (nb_match(p, TOK_EQ)) {
            nb_node_t *asn = nb_node_new(N_ASSIGN, line);
            asn->a = n; asn->b = nb_parse_expr(p);
            return asn;
        }
        // No era asignacion -- llamada/lectura de array como
        // sentencia (ver la nota de simplificacion). Reempaquetado
        // como N_CALL; el generador de codigo decide mas adelante si
        // 'name' es un array o una funcion, igual que en el viejo.
        nb_node_t *call = nb_node_new(N_CALL, line);
        call->text = name; call->list = n->list;
        call->list_count = n->list_count; call->list_cap = n->list_cap;
        n->list = NULL; n->list_count = 0; n->list_cap = 0; // evita liberar dos veces la misma lista
        // Y el NOMBRE tambien: 'call->text' es el mismo puntero que
        // 'n->text', y nb_node_free_tree lo libera. Antes solo se
        // desenganchaba la lista, asi que la llamada se quedaba con un
        // nombre ya liberado: "Perder()" como sentencia daba "funcion
        // que no existe" (el hueco contenia basura) y "Saluda(\"x\")"
        // estrellaba el compilador (analizar los argumentos reutilizaba
        // esa memoria). "x = Perder()" nunca pasaba por aqui.
        n->text = NULL;
        nb_node_free_tree(n);
        nb_node_t *stmt = nb_node_new(N_EXPRSTMT, line);
        stmt->a = call;
        return stmt;
    }

    if (nb_check(p, TOK_EQ) && nb_es_constante(name)) {
        nb_error_at(p, "no se puede cambiar el valor de una constante");
        return NULL;
    }
    if (nb_match(p, TOK_EQ)) {
        nb_node_t *var = nb_node_new(N_VAR, line);
        var->text = name;
        nb_node_t *n = nb_node_new(N_ASSIGN, line);
        n->a = var; n->b = nb_parse_expr(p);
        return n;
    }

    // Comando al estilo BASIC clasico, sin parentesis: "Foo" sola, o
    // "Foo arg1, arg2" con cero o mas argumentos separados por comas.
    nb_node_t *call = nb_node_new(N_CALL, line);
    call->text = name;
    if (!nb_check(p, TOK_NEWLINE) && !nb_check(p, TOK_COLON) && !nb_check(p, TOK_EOF) && !nb_at_block_end(p)) {
        nb_node_list_add(call, nb_parse_expr(p));
        while (nb_match(p, TOK_COMMA)) nb_node_list_add(call, nb_parse_expr(p));
    }
    nb_node_t *stmt = nb_node_new(N_EXPRSTMT, line);
    stmt->a = call;
    return stmt;
}

// Print/Cls/Plot/Line/Rect/Delay -- todos "comando seguido de cero o
// mas argumentos separados por comas", con o sin parentesis.
static nb_node_t *nb_parse_command_with_args(nb_parser_t *p, nb_node_kind_t kind) {
    int32_t line = p->cur.line;
    nb_advance(p); // el propio comando
    bool has_parens = nb_match(p, TOK_LPAREN);
    nb_node_t *n = nb_node_new(kind, line);
    if (!nb_check(p, TOK_NEWLINE) && !nb_check(p, TOK_COLON) && !nb_check(p, TOK_RPAREN) &&
        !nb_check(p, TOK_EOF) && !nb_at_block_end(p)) {
        nb_node_list_add(n, nb_parse_expr(p));
        while (nb_match(p, TOK_COMMA)) nb_node_list_add(n, nb_parse_expr(p));
    }
    if (has_parens) nb_expect(p, TOK_RPAREN, "se esperaba ')'");
    // Cuantos argumentos necesita CADA comando, como minimo.
    //
    // Sin esto, escribir "Plot 5" (con uno solo) compilaba sin una
    // queja y no dibujaba nada: el generador tiene un "if (faltan
    // argumentos) return;" y se iba en silencio. Decirlo aqui, con el
    // nombre del comando y lo que falta, es infinitamente mejor.
    int32_t minimo = 0;
    const char *aviso = NULL;
    switch (kind) {
        case N_PLOT:         minimo = 2; aviso = "Plot necesita 2 numeros: x, y"; break;
        case N_RECT:         minimo = 4; aviso = "Rect necesita 4 numeros: x, y, ancho, alto"; break;
        case N_OVAL:         minimo = 4; aviso = "Oval necesita 4 numeros: x, y, ancho, alto"; break;
        case N_LINE:         minimo = 4; aviso = "Line necesita 4 numeros: x0, y0, x1, y1"; break;
        case N_TEXT:         minimo = 3; aviso = "Text necesita 3 argumentos: x, y, cadena$"; break;
        case N_CREATEWINDOW: minimo = 5; aviso = "CreateWindow necesita 5 argumentos: titulo$, x, y, ancho, alto"; break;
        case N_GRAPHICS:     minimo = 2; aviso = "Graphics necesita 2 numeros: ancho, alto"; break;
        default: break;
    }
    if (aviso && n->list_count < minimo) {
        nb_error_at(p, aviso);
        return NULL;
    }
    return n;
}

static nb_node_t *nb_parse_if(nb_parser_t *p) {
    int32_t line = p->cur.line;
    nb_advance(p); // If
    nb_node_t *n = nb_node_new(N_IF, line);
    n->a = nb_parse_expr(p);
    nb_match(p, TOK_KW_THEN); // 'Then' opcional, igual que en BlitzPlus real

    if (nb_check(p, TOK_NEWLINE)) {
        n->b = nb_parse_block(p);
        while (nb_check(p, TOK_KW_ELSEIF)) {
            int32_t eline = p->cur.line;
            nb_advance(p);
            nb_node_t *ei = nb_node_new(N_ELSEIF, eline);
            ei->a = nb_parse_expr(p);
            nb_match(p, TOK_KW_THEN);
            nb_skip_separators(p);
            ei->b = nb_parse_block(p);
            nb_node_list_add(n, ei);
        }
        if (nb_match(p, TOK_KW_ELSE)) {
            nb_skip_separators(p);
            n->c = nb_parse_block(p);
        }
        nb_expect(p, TOK_KW_ENDIF, "se esperaba 'EndIf'");
    } else {
        // Forma de una linea: "If x Then stmt1[:stmt2...] [Else stmt3[:stmt4...]]"
        nb_node_t *block = nb_node_new(N_BLOCK, line);
        nb_node_list_add(block, nb_parse_statement(p));
        while (nb_match(p, TOK_COLON) && !nb_check(p, TOK_KW_ELSE)) {
            nb_node_list_add(block, nb_parse_statement(p));
        }
        n->b = block;
        if (nb_match(p, TOK_KW_ELSE)) {
            nb_node_t *eblock = nb_node_new(N_BLOCK, line);
            nb_node_list_add(eblock, nb_parse_statement(p));
            while (nb_match(p, TOK_COLON)) nb_node_list_add(eblock, nb_parse_statement(p));
            n->c = eblock;
        }
    }
    return n;
}

static nb_node_t *nb_parse_for(nb_parser_t *p) {
    int32_t line = p->cur.line;
    nb_advance(p); // For
    if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba el nombre de la variable del bucle"); return NULL; }
    nb_string_t *varname = p->cur.text; // se reclama
    nb_advance(p);
    nb_expect(p, TOK_EQ, "se esperaba '=' tras la variable del bucle");

    if (nb_check(p, TOK_KW_EACH)) {
        nb_advance(p); // Each
        if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba el nombre del tipo tras 'Each'"); return NULL; }
        nb_node_t *n = nb_node_new(N_FOREACH, line);
        n->text = varname;
        nb_node_t *typenode = nb_node_new(N_VAR, line);
        typenode->text = p->cur.text; // se reclama
        n->a = typenode;
        nb_advance(p);
        nb_skip_separators(p);
        n->b = nb_parse_block(p);
        nb_expect(p, TOK_KW_NEXT, "se esperaba 'Next'");
        if (nb_check(p, TOK_IDENT)) { nb_string_release(p->cur.text); nb_advance(p); }
        return n;
    }

    nb_node_t *n = nb_node_new(N_FOR, line);
    n->text = varname;
    n->a = nb_parse_expr(p);
    nb_expect(p, TOK_KW_TO, "se esperaba 'To'");
    n->b = nb_parse_expr(p);
    if (nb_match(p, TOK_KW_STEP)) n->c = nb_parse_expr(p);
    nb_skip_separators(p);
    n->d = nb_parse_block(p);
    nb_expect(p, TOK_KW_NEXT, "se esperaba 'Next'");
    if (nb_check(p, TOK_IDENT)) { nb_string_release(p->cur.text); nb_advance(p); } // "Next variable" opcional
    return n;
}

static nb_node_t *nb_parse_while(nb_parser_t *p) {
    int32_t line = p->cur.line;
    nb_advance(p); // While
    nb_node_t *n = nb_node_new(N_WHILE, line);
    n->a = nb_parse_expr(p);
    nb_skip_separators(p);
    n->b = nb_parse_block(p);
    nb_expect(p, TOK_KW_WEND, "se esperaba 'Wend'");
    return n;
}

static nb_node_t *nb_parse_repeat(nb_parser_t *p) {
    int32_t line = p->cur.line;
    nb_advance(p); // Repeat
    nb_node_t *n = nb_node_new(N_REPEAT, line);
    nb_skip_separators(p);
    n->a = nb_parse_block(p);
    if (nb_match(p, TOK_KW_UNTIL)) n->b = nb_parse_expr(p);
    else nb_expect(p, TOK_KW_FOREVER, "se esperaba 'Until' o 'Forever'");
    return n;
}

static nb_node_t *nb_parse_funcdef(nb_parser_t *p) {
    int32_t line = p->cur.line;
    // Una Function DENTRO de otra no existe en Nemo Basic, y hasta ahora se
    // aceptaba en silencio: se parseaba, no se generaba, no se podia llamar
    // y no se avisaba. Quien escribiera una funcion auxiliar dentro de otra
    // la veia desaparecer sin ningun mensaje. Se dice.
    if (p->in_function) {
        nb_error_at(p, "no se puede declarar una Function dentro de otra: sacala fuera");
        return NULL;
    }
    nb_advance(p); // Function
    if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba el nombre de la funcion"); return NULL; }
    nb_node_t *n = nb_node_new(N_FUNCDEF, line);
    n->text = p->cur.text;
    nb_advance(p);
    nb_expect(p, TOK_LPAREN, "se esperaba '(' tras el nombre de la funcion");
    if (!nb_check(p, TOK_RPAREN)) {
        for (;;) {
            if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba un nombre de parametro"); return NULL; }
            nb_node_t *param = nb_node_new(N_VAR, p->cur.line);
            param->text = p->cur.text;
            nb_advance(p);
            // "nombre()" -- un ARRAY como parametro.
            //
            // Lo que viaja es el puntero al bloque del array, no una copia:
            // la funcion escribe en el array de quien la llama, que es justo
            // para lo que sirve (Partir(texto$, sep$, trozos$()) deja los
            // trozos donde el programa pueda leerlos). Copiarlo seria caro y
            // ademas inutil, porque entonces no habria forma de devolverlo.
            //
            // Los parentesis van VACIOS: el tamaño lo lleva el propio bloque
            // en su cabecera, asi que escribirlo aqui seria repetir un dato
            // que ya existe y que podria no coincidir.
            if (nb_check(p, TOK_LPAREN)) {
                nb_advance(p);
                if (!nb_check(p, TOK_RPAREN)) {
                    nb_error_at(p, "los parentesis de un array como parametro van vacios: nombre()");
                    return NULL;
                }
                nb_advance(p);
                param->op = NB_PARAM_ARRAY;
            }
            if (nb_match(p, TOK_EQ)) param->b = nb_parse_expr(p); // valor por defecto
            nb_node_list_add(n, param);
            if (!nb_match(p, TOK_COMMA)) break;
        }
    }
    nb_expect(p, TOK_RPAREN, "se esperaba ')'");
    nb_skip_separators(p);
    // El cuerpo se parsea con la bandera puesta; se guarda y se
    // restaura por si algun dia se anidan funciones.
    bool fuera = p->in_function;
    p->in_function = true;
    n->d = nb_parse_block(p);
    p->in_function = fuera;

    nb_expect(p, TOK_KW_ENDFUNCTION, "se esperaba 'End Function'");
    return n;
}

static nb_node_t *nb_parse_vardecl(nb_parser_t *p, nb_token_type_t kind_tok) {
    int32_t line = p->cur.line;
    nb_advance(p); // Global/Local/Const
    nb_node_t *n = nb_node_new(N_VARDECL, line);
    n->op = kind_tok;
    for (;;) {
        if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba un nombre de variable"); return NULL; }
        nb_node_t *item = nb_node_new(N_ASSIGN, p->cur.line);
        nb_node_t *var = nb_node_new(N_VAR, p->cur.line);
        var->text = p->cur.text;
        item->a = var;
        nb_advance(p);
        if (kind_tok == TOK_KW_CONST) {
            // Una constante: necesita su valor en la propia declaracion,
            // se declara una sola vez, y no se puede cambiar despues.
            if (nb_es_constante(var->text)) { nb_error_at(p, "esa constante ya estaba declarada"); return NULL; }
            if (!nb_check(p, TOK_EQ)) { nb_error_at(p, "una constante necesita su valor: Const NOMBRE = valor"); return NULL; }
            if (nb_num_constantes >= NB_MAX_CONSTANTES) { nb_error_at(p, "demasiadas constantes"); return NULL; }
            nb_constantes[nb_num_constantes++] = var->text;
        }
        if (nb_match(p, TOK_EQ)) item->b = nb_parse_expr(p);
        nb_node_list_add(n, item);
        if (!nb_match(p, TOK_COMMA)) break;
    }
    return n;
}

static nb_node_t *nb_parse_one_dim(nb_parser_t *p) {
    if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba un nombre de array"); return NULL; }
    nb_node_t *n = nb_node_new(N_DIM, p->cur.line);
    n->text = p->cur.text;
    nb_advance(p);
    nb_expect(p, TOK_LPAREN, "se esperaba '(' con el tamaño del array");
    nb_node_list_add(n, nb_parse_expr(p));
    while (nb_match(p, TOK_COMMA)) nb_node_list_add(n, nb_parse_expr(p));
    nb_expect(p, TOK_RPAREN, "se esperaba ')'");

    // Solo 1 y 2 dimensiones. Tres o mas quedaron fuera del diseño,
    // pero hasta ahora se ACEPTABAN y el generador no producia nada:
    // "Dim a(10,10,10)" compilaba sin una queja y el array no existia.
    // Un error aqui es mucho mejor que un programa que parece bueno y
    // escribe en memoria que no es suya.
    if (n->list_count > 2) {
        nb_error_at(p, "Dim admite 1 o 2 dimensiones; tres o mas no estan implementadas");
        return NULL;
    }
    return n;
}

static nb_node_t *nb_parse_dim(nb_parser_t *p) {
    int32_t line = p->cur.line;
    nb_advance(p); // Dim
    nb_node_t *first = nb_parse_one_dim(p);
    if (!nb_check(p, TOK_COMMA)) return first;
    nb_node_t *block = nb_node_new(N_BLOCK, line);
    nb_node_list_add(block, first);
    while (nb_match(p, TOK_COMMA)) nb_node_list_add(block, nb_parse_one_dim(p));
    return block;
}

static nb_node_t *nb_parse_typedef(nb_parser_t *p) {
    int32_t line = p->cur.line;
    nb_advance(p); // Type
    if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba el nombre del tipo"); return NULL; }
    nb_node_t *n = nb_node_new(N_TYPEDEF, line);
    n->text = p->cur.text;
    nb_advance(p);
    nb_skip_separators(p);
    while (nb_check(p, TOK_KW_FIELD)) {
        nb_advance(p); // Field
        for (;;) {
            if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba un nombre de campo"); return NULL; }
            nb_node_t *f = nb_node_new(N_VAR, p->cur.line);
            f->text = p->cur.text;
            nb_advance(p);
            nb_node_list_add(n, f);
            if (!nb_match(p, TOK_COMMA)) break;
        }
        nb_skip_separators(p);
    }
    nb_expect(p, TOK_KW_ENDTYPE, "se esperaba 'End Type'");
    return n;
}

// ---- Data / Read / Restore ----

static nb_node_t *nb_parse_data(nb_parser_t *p) {
    int32_t line = p->cur.line;
    nb_advance(p); // Data
    nb_node_t *n = nb_node_new(N_DATA, line);
    nb_node_list_add(n, nb_parse_unary(p));
    while (nb_match(p, TOK_COMMA)) nb_node_list_add(n, nb_parse_unary(p));
    return n;
}

static nb_node_t *nb_parse_read(nb_parser_t *p) {
    int32_t line = p->cur.line;
    nb_advance(p); // Read
    if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba un nombre de variable tras 'Read'"); return NULL; }
    nb_node_t *n = nb_node_new(N_READ, line);
    nb_string_t *name = p->cur.text;
    nb_advance(p);
    if (nb_check(p, TOK_LPAREN)) {
        nb_advance(p);
        nb_node_t *idx = nb_node_new(N_INDEX, line);
        idx->text = name;
        nb_node_list_add(idx, nb_parse_expr(p));
        while (nb_match(p, TOK_COMMA)) nb_node_list_add(idx, nb_parse_expr(p));
        nb_expect(p, TOK_RPAREN, "se esperaba ')' tras los indices");
        n->a = idx;
    } else {
        nb_node_t *var = nb_node_new(N_VAR, line);
        var->text = name;
        n->a = var;
    }
    return n;
}

static nb_node_t *nb_parse_restore(nb_parser_t *p) {
    int32_t line = p->cur.line;
    nb_advance(p); // Restore
    nb_node_t *n = nb_node_new(N_RESTORE, line);
    if (nb_check(p, TOK_IDENT)) {
        n->text = p->cur.text;
        nb_advance(p);
    }
    return n;
}

static nb_node_t *nb_parse_datalabel(nb_parser_t *p) {
    nb_node_t *n = nb_node_new(N_DATALABEL, p->cur.line);
    n->text = p->cur.text;
    nb_advance(p);
    return n;
}

// ---- Select / Case ----
//
// No se añade NADA al generador de codigo para esto -- se traduce
// aqui mismo, en el parser, a la cadena de If/ElseIf/Else que ya sabe
// generar codigo. La unica sutileza es evaluar la expresion del
// Select UNA SOLA VEZ (por si tiene efectos secundarios) guardandola
// en una variable temporal sintetica, y comparar esa variable en cada
// Case -- identico al compilador viejo.

static int32_t nb_select_tmp_counter = 0;

// La inferencia de tipos del generador (nb_codegen.c). Se usa la suya y no
// una escrita aqui: dos inferencias de tipo acaban siendo dos reglas
// distintas, y la que se quede atras lo hara en silencio.
typedef enum { NB_TYPE_INT, NB_TYPE_FLOAT, NB_TYPE_STRING } nb_var_type_t;
extern nb_var_type_t nb_expr_type(nb_node_t *n);

static nb_node_t *nb_make_var_ref(int32_t line, const char *name) {
    nb_node_t *v = nb_node_new(N_VAR, line);
    v->text = nb_string_new(name, (uint32_t)nb_cstrlen(name));
    return v;
}

// "Case v1, v2, ..." -> "tmp = v1 Or tmp = v2 Or ..."
static nb_node_t *nb_parse_case_condition(nb_parser_t *p, const char *tmp_name) {
    nb_node_t *cond = NULL;
    for (;;) {
        int32_t line = p->cur.line;
        nb_node_t *cmp = nb_node_new(N_BINOP, line);
        cmp->op = TOK_EQ;
        cmp->a = nb_make_var_ref(line, tmp_name);
        cmp->b = nb_parse_expr(p);
        if (cond == NULL) {
            cond = cmp;
        } else {
            nb_node_t *orn = nb_node_new(N_BINOP, line);
            orn->op = TOK_KW_OR;
            orn->a = cond; orn->b = cmp;
            cond = orn;
        }
        if (!nb_match(p, TOK_COMMA)) break;
    }
    return cond;
}

static nb_node_t *nb_parse_select(nb_parser_t *p) {
    int32_t line = p->cur.line;
    nb_advance(p); // Select
    nb_node_t *sel_expr = nb_parse_expr(p);
    nb_skip_separators(p);

    char tmp_name[24];
    int32_t id = nb_select_tmp_counter++;
    char digits[12]; int32_t nd = 0;
    if (id == 0) digits[nd++] = '0';
    else { int32_t v = id; while (v > 0) { digits[nd++] = (char)('0' + v % 10); v /= 10; } }
    const char *prefix = "__SELTMP";
    int32_t ti = 0;
    while (prefix[ti]) { tmp_name[ti] = prefix[ti]; ti++; }
    while (nd > 0) tmp_name[ti++] = digits[--nd];
    // El SUFIJO DEL TIPO, que es lo que hace que "Select nombre$" funcione.
    //
    // Esta variable la fabrica el compilador, y en Nemo Basic el tipo de una
    // variable es su ultima letra. Sin sufijo era siempre entera, asi que un
    // Select sobre una cadena daba "se guarda una cadena en algo numerico" --
    // un error sobre una variable que el programador nunca escribio, y que no
    // aparece en su programa por ningun lado. Con un decimal era peor: no
    // daba error, truncaba.
    {
        nb_var_type_t t = nb_expr_type(sel_expr);
        if (t == NB_TYPE_STRING) tmp_name[ti++] = '$';
        else if (t == NB_TYPE_FLOAT) tmp_name[ti++] = '#';
    }
    tmp_name[ti] = '\0';

    nb_node_t *assign = nb_node_new(N_ASSIGN, line);
    assign->a = nb_make_var_ref(line, tmp_name);
    assign->b = sel_expr;

    nb_node_t *ifn = NULL;
    bool first = true;

    while (nb_check(p, TOK_KW_CASE)) {
        int32_t cline = p->cur.line;
        nb_advance(p); // Case
        nb_node_t *cond = nb_parse_case_condition(p, tmp_name);
        nb_skip_separators(p);
        nb_node_t *block = nb_parse_block(p);
        if (first) {
            ifn = nb_node_new(N_IF, cline);
            ifn->a = cond; ifn->b = block;
            first = false;
        } else {
            nb_node_t *ei = nb_node_new(N_ELSEIF, cline);
            ei->a = cond; ei->b = block;
            nb_node_list_add(ifn, ei);
        }
    }

    if (ifn == NULL) {
        ifn = nb_node_new(N_IF, line);
        nb_node_t *falsecond = nb_node_new(N_NUM, line);
        falsecond->num_value = 0;
        ifn->a = falsecond;
        ifn->b = nb_node_new(N_BLOCK, line);
    }

    if (nb_match(p, TOK_KW_DEFAULT)) {
        nb_skip_separators(p);
        ifn->c = nb_parse_block(p);
    }
    nb_expect(p, TOK_KW_ENDSELECT, "se esperaba 'End Select'");

    nb_node_t *wrapper = nb_node_new(N_BLOCK, line);
    nb_node_list_add(wrapper, assign);
    nb_node_list_add(wrapper, ifn);
    return wrapper;
}

static nb_node_t *nb_parse_statement(nb_parser_t *p) {
    switch (p->cur.type) {
        case TOK_KW_IF:     return nb_parse_if(p);
        case TOK_KW_SELECT: return nb_parse_select(p);
        case TOK_KW_FOR:    return nb_parse_for(p);
        case TOK_KW_WHILE:  return nb_parse_while(p);
        case TOK_KW_REPEAT: return nb_parse_repeat(p);
        case TOK_KW_FUNCTION: return nb_parse_funcdef(p);
        case TOK_KW_GLOBAL:
        case TOK_KW_LOCAL:
        case TOK_KW_CONST:  return nb_parse_vardecl(p, p->cur.type);
        case TOK_KW_DIM:    return nb_parse_dim(p);
        case TOK_KW_TYPE:   return nb_parse_typedef(p);
        case TOK_KW_DELETE: {
            int32_t line = p->cur.line;
            nb_advance(p);
            nb_node_t *n = nb_node_new(N_DELETE, line);
            n->a = nb_parse_expr(p);
            return n;
        }
        case TOK_KW_INSERT: {
            // Insert/Before/After (reordenar la lista de instancias de
            // un Type) se dejo FUERA del diseno. Se parsea igualmente
            // para dar un error claro aqui: antes se aceptaba y el
            // generador lo ignoraba, asi que un programa que lo usara
            // compilaba sin una sola queja y luego no reordenaba nada.
            // Un error de compilacion es mucho mejor que eso.
            nb_error_at(p, "'Insert ... Before/After' no esta implementado en Nemo Basic");
            return NULL;
        }
        case TOK_KW_DATA:    return nb_parse_data(p);
        case TOK_KW_READ:    return nb_parse_read(p);
        case TOK_KW_RESTORE: return nb_parse_restore(p);
        case TOK_DATALABEL:  return nb_parse_datalabel(p);
        case TOK_KW_PRINT:  return nb_parse_command_with_args(p, N_PRINT);
        case TOK_KW_CLS:    return nb_parse_command_with_args(p, N_CLS);
        case TOK_KW_PLOT:   return nb_parse_command_with_args(p, N_PLOT);
        case TOK_KW_LINE:   return nb_parse_command_with_args(p, N_LINE);
        case TOK_KW_RECT:   return nb_parse_command_with_args(p, N_RECT);
        case TOK_KW_DELAY:  return nb_parse_command_with_args(p, N_DELAY);
        case TOK_KW_COLOR:    return nb_parse_command_with_args(p, N_COLOR);
        case TOK_KW_GRAPHICS: return nb_parse_command_with_args(p, N_GRAPHICS);
        case TOK_KW_OVAL:     return nb_parse_command_with_args(p, N_OVAL);
        case TOK_KW_TEXT:     return nb_parse_command_with_args(p, N_TEXT);
        case TOK_KW_CREATEWINDOW: return nb_parse_command_with_args(p, N_CREATEWINDOW);
        // Console / Desktop -- declaran COMO quiere abrirse el
        // programa. No generan ni una instruccion: son una marca que
        // viaja en la cabecera del .pro, para que el explorador sepa
        // si tiene que abrirle una shell o lanzarlo directo.
        case TOK_KW_CONSOLE: { int32_t l = p->cur.line; nb_advance(p); return nb_node_new(N_CONSOLEAPP, l); }
        case TOK_KW_DESKTOP: { int32_t l = p->cur.line; nb_advance(p); return nb_node_new(N_DESKTOPAPP, l); }
        case TOK_KW_RETURN: {
            int32_t line = p->cur.line;
            nb_advance(p);
            nb_node_t *n = nb_node_new(N_RETURN, line);
            if (!nb_check(p, TOK_NEWLINE) && !nb_check(p, TOK_COLON) && !nb_check(p, TOK_EOF) && !nb_at_block_end(p)) {
                n->a = nb_parse_expr(p);
            }
            return n;
        }
        case TOK_KW_EXIT: { int32_t line = p->cur.line; nb_advance(p); return nb_node_new(N_EXIT, line); }
        case TOK_KW_END:  { int32_t line = p->cur.line; nb_advance(p); return nb_node_new(N_ENDPROGRAM, line); }
        case TOK_KW_GOTO: {
            int32_t line = p->cur.line;
            nb_advance(p);
            if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba un nombre de etiqueta tras 'Goto'"); return NULL; }
            nb_node_t *n = nb_node_new(N_GOTO, line);
            n->text = p->cur.text;
            nb_advance(p);
            return n;
        }
        case TOK_KW_GOSUB: {
            if (p->in_function) {
                // Documentado como fuera de alcance desde el diseño,
                // pero hasta ahora se ACEPTABA: el Return de la
                // subrutina se tomaba por el Return de la funcion y el
                // programa se comportaba mal sin decir nada.
                nb_error_at(p, "Gosub no se puede usar dentro de una Function (Return ya significa 'volver de la funcion')");
                return NULL;
            }
            int32_t line = p->cur.line;
            nb_advance(p);
            if (!nb_check(p, TOK_IDENT)) { nb_error_at(p, "se esperaba un nombre de etiqueta tras 'Gosub'"); return NULL; }
            nb_node_t *n = nb_node_new(N_GOSUB, line);
            n->text = p->cur.text;
            nb_advance(p);
            return n;
        }
        case TOK_IDENT: return nb_parse_ident_stmt(p);
        default:
            nb_error_at(p, "sentencia no reconocida");
            return NULL;
    }
}

static nb_node_t *nb_parse_block(nb_parser_t *p) {
    nb_node_t *block = nb_node_new(N_BLOCK, p->cur.line);
    nb_skip_separators(p);
    while (!nb_at_block_end(p) && !p->had_error) {
        nb_node_list_add(block, nb_parse_statement(p));
        if (p->had_error) break;
        if (!nb_check(p, TOK_NEWLINE) && !nb_check(p, TOK_COLON) && !nb_at_block_end(p)) {
            nb_error_at(p, "se esperaba fin de linea o ':' entre sentencias");
            break;
        }
        nb_skip_separators(p);
    }
    return block;
}

// Punto de entrada del compilador: parsea el PROGRAMA ENTERO (por
// ahora, solo con las sentencias de esta pieza -- Function/Type/
// Select/Data se añaden en el siguiente paso).
nb_node_t *nb_parse_program(const char *source, bool *ok, const char **err_msg, int32_t *err_line) {
    nb_parser_t p;
    nb_num_constantes = 0;   // las constantes son de cada programa
    nb_parser_init(&p, source);
    nb_node_t *program = nb_parse_block(&p);
    if (!p.had_error && p.cur.type != TOK_EOF) {
        nb_error_at(&p, "se esperaba el fin del archivo");
    }
    *ok = !p.had_error;
    *err_msg = p.error_msg;
    *err_line = p.error_line;
    return program;
}

// Se conserva para poder seguir probando expresiones sueltas por
// separado (lo que ya se probo en la pieza anterior).
nb_node_t *nb_parse_expression_toplevel(const char *source, bool *ok, const char **err_msg, int32_t *err_line) {
    nb_parser_t p;
    nb_parser_init(&p, source);
    nb_node_t *n = nb_parse_expr(&p);
    *ok = !p.had_error;
    *err_msg = p.error_msg;
    *err_line = p.error_line;
    return n;
}
