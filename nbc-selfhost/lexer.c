// lexer.c — compilador Nemo-Blitz

#include "lexer.h"
#include "nblibc.h"

void lexer_init(Lexer *lx, const char *source) {
    lx->src = source;
    lx->pos = 0;
    lx->line = 1;
}

static char peek(Lexer *lx) { return lx->src[lx->pos]; }
static char peek2(Lexer *lx) { return lx->src[lx->pos] ? lx->src[lx->pos + 1] : '\0'; }
static char advance(Lexer *lx) {
    char c = lx->src[lx->pos];
    if (c != '\0') lx->pos++;
    return c;
}

// "End Function" y "End Type" son dos palabras en BlitzPlus real, pero
// aqui las tratamos como un unico token para simplificar el parser --
// el lexer las junta cuando ve "END" seguido de "FUNCTION"/"TYPE".
//
// OJO: NO usamos una tabla de punteros a cadenas (algo como
// "static const struct {const char *word; TokenType type;} keywords[]")
// -- esos punteros se calculan en tiempo de ENLACE asumiendo que el
// programa se carga en la direccion 0 (la que usa nuestro linker
// script), pero nuestro cargador copia el programa a una direccion
// de memoria real distinta cada vez, y NO reubica punteros de datos
// (solo el propio codigo, via instrucciones adrp relativas a si
// mismo, es realmente independiente de la posicion). Una tabla asi
// quedaria con punteros absolutos incorrectos en tiempo de ejecucion.
// Es el mismo problema (y la misma solucion) que ya documentamos en
// el propio kernel: cadena de comparaciones directas, nada de tablas
// de punteros inicializadas en tiempo de compilacion.
static TokenType lookup_keyword(const char *upper) {
    if (nb_strcmp(upper, "IF") == 0) return T_KW_IF;
    if (nb_strcmp(upper, "THEN") == 0) return T_KW_THEN;
    if (nb_strcmp(upper, "ELSE") == 0) return T_KW_ELSE;
    if (nb_strcmp(upper, "ELSEIF") == 0) return T_KW_ELSEIF;
    if (nb_strcmp(upper, "ENDIF") == 0) return T_KW_ENDIF;
    if (nb_strcmp(upper, "FOR") == 0) return T_KW_FOR;
    if (nb_strcmp(upper, "TO") == 0) return T_KW_TO;
    if (nb_strcmp(upper, "STEP") == 0) return T_KW_STEP;
    if (nb_strcmp(upper, "NEXT") == 0) return T_KW_NEXT;
    if (nb_strcmp(upper, "WHILE") == 0) return T_KW_WHILE;
    if (nb_strcmp(upper, "WEND") == 0) return T_KW_WEND;
    if (nb_strcmp(upper, "REPEAT") == 0) return T_KW_REPEAT;
    if (nb_strcmp(upper, "UNTIL") == 0) return T_KW_UNTIL;
    if (nb_strcmp(upper, "FOREVER") == 0) return T_KW_FOREVER;
    if (nb_strcmp(upper, "EXIT") == 0) return T_KW_EXIT;
    if (nb_strcmp(upper, "GOTO") == 0) return T_KW_GOTO;
    if (nb_strcmp(upper, "GOSUB") == 0) return T_KW_GOSUB;
    if (nb_strcmp(upper, "FUNCTION") == 0) return T_KW_FUNCTION;
    if (nb_strcmp(upper, "RETURN") == 0) return T_KW_RETURN;
    if (nb_strcmp(upper, "DIM") == 0) return T_KW_DIM;
    if (nb_strcmp(upper, "TYPE") == 0) return T_KW_TYPE;
    if (nb_strcmp(upper, "NEW") == 0) return T_KW_NEW;
    if (nb_strcmp(upper, "DELETE") == 0) return T_KW_DELETE;
    if (nb_strcmp(upper, "FIRST") == 0) return T_KW_FIRST;
    if (nb_strcmp(upper, "LAST") == 0) return T_KW_LAST;
    if (nb_strcmp(upper, "FIELD") == 0) return T_KW_FIELD;
    if (nb_strcmp(upper, "EACH") == 0) return T_KW_EACH;
    if (nb_strcmp(upper, "NULL") == 0) return T_KW_NULL;
    if (nb_strcmp(upper, "BEFORE") == 0) return T_KW_BEFORE;
    if (nb_strcmp(upper, "AFTER") == 0) return T_KW_AFTER;
    if (nb_strcmp(upper, "INSERT") == 0) return T_KW_INSERT;
    if (nb_strcmp(upper, "GLOBAL") == 0) return T_KW_GLOBAL;
    if (nb_strcmp(upper, "LOCAL") == 0) return T_KW_LOCAL;
    if (nb_strcmp(upper, "CONST") == 0) return T_KW_CONST;
    if (nb_strcmp(upper, "SELECT") == 0) return T_KW_SELECT;
    if (nb_strcmp(upper, "CASE") == 0) return T_KW_CASE;
    if (nb_strcmp(upper, "DEFAULT") == 0) return T_KW_DEFAULT;
    if (nb_strcmp(upper, "DATA") == 0) return T_KW_DATA;
    if (nb_strcmp(upper, "READ") == 0) return T_KW_READ;
    if (nb_strcmp(upper, "RESTORE") == 0) return T_KW_RESTORE;
    if (nb_strcmp(upper, "MOD") == 0) return T_KW_MOD;
    if (nb_strcmp(upper, "AND") == 0) return T_KW_AND;
    if (nb_strcmp(upper, "OR") == 0) return T_KW_OR;
    if (nb_strcmp(upper, "NOT") == 0) return T_KW_NOT;
    if (nb_strcmp(upper, "XOR") == 0) return T_KW_XOR;
    if (nb_strcmp(upper, "SHL") == 0) return T_KW_SHL;
    if (nb_strcmp(upper, "SHR") == 0) return T_KW_SHR;
    if (nb_strcmp(upper, "SAR") == 0) return T_KW_SAR;
    if (nb_strcmp(upper, "TRUE") == 0) return T_KW_TRUE;
    if (nb_strcmp(upper, "FALSE") == 0) return T_KW_FALSE;
    if (nb_strcmp(upper, "PRINT") == 0) return T_KW_PRINT;
    if (nb_strcmp(upper, "CLS") == 0) return T_KW_CLS;
    if (nb_strcmp(upper, "PLOT") == 0) return T_KW_PLOT;
    if (nb_strcmp(upper, "LINE") == 0) return T_KW_LINE;
    if (nb_strcmp(upper, "RECT") == 0) return T_KW_RECT;
    if (nb_strcmp(upper, "DELAY") == 0) return T_KW_DELAY;
    if (nb_strcmp(upper, "END") == 0) return T_KW_END;
    return T_IDENT;
}

static void skip_line_comment(Lexer *lx) {
    while (peek(lx) != '\0' && peek(lx) != '\n') advance(lx);
}

Token lexer_next(Lexer *lx) {
    Token tok;
    nb_memset(&tok, 0, sizeof(tok));

    // Espacios y comentarios (pero NO saltos de linea -- son un token
    // en si mismos, separan sentencias)
    for (;;) {
        char c = peek(lx);
        if (c == ' ' || c == '\t' || c == '\r') { advance(lx); continue; }
        if (c == ';') { skip_line_comment(lx); continue; }
        break;
    }

    tok.line = lx->line;
    char c = peek(lx);

    if (c == '\0') { tok.type = T_EOF; return tok; }

    if (c == '\n') {
        advance(lx);
        lx->line++;
        tok.type = T_NEWLINE;
        return tok;
    }

    // Flotantes que empiezan directamente por el punto, sin cero
    // delante (.5, .098) -- BlitzPlus real los admite. Se distingue
    // de una etiqueta de datos (".etiqueta") porque tras el punto
    // viene un DIGITO, no una letra. Usamos el mismo enfoque de
    // "combinar y dividir una sola vez" que el resto de flotantes de
    // este archivo, para mantener paridad exacta con el host.
    if (c == '.' && nb_isdigit(peek2(lx))) {
        int start = lx->pos;
        advance(lx); // consume el '.'
        int frac_start = lx->pos;
        while (nb_isdigit(peek(lx))) advance(lx);
        int frac_len = lx->pos - frac_start;
        int len = lx->pos - start;
        if (len >= TOKEN_TEXT_MAX) len = TOKEN_TEXT_MAX - 1;
        nb_memcpy(tok.text, lx->src + start, (uint32_t)len);
        tok.text[len] = '\0';
        int64_t combined = 0;
        double divisor = 1.0;
        for (int i = 0; i < frac_len; i++) {
            combined = combined * 10 + (lx->src[frac_start + i] - '0');
            divisor = divisor * 10.0;
        }
        tok.num_value = (double)combined / divisor;
        tok.type = T_NUMBER;
        return tok;
    }

    // Numeros: enteros o con parte fraccionaria (3.14) -- ahora que
    // el lenguaje si soporta coma flotante, calculamos el valor
    // completo, no solo la parte entera.
    if (nb_isdigit(c)) {
        int start = lx->pos;
        while (nb_isdigit(peek(lx))) advance(lx);
        int int_len = lx->pos - start;
        bool has_frac = false;
        int frac_start = 0, frac_len = 0;
        if (peek(lx) == '.' && nb_isdigit(peek2(lx))) {
            advance(lx); // consume el '.'
            frac_start = lx->pos;
            while (nb_isdigit(peek(lx))) advance(lx);
            frac_len = lx->pos - frac_start;
            has_frac = true;
        }
        int len = lx->pos - start;
        if (len >= TOKEN_TEXT_MAX) len = TOKEN_TEXT_MAX - 1;
        nb_memcpy(tok.text, lx->src + start, (uint32_t)len);
        tok.text[len] = '\0';

        if (has_frac) {
            // Combinamos entero+fraccion en UN SOLO entero, y
            // dividimos UNA UNICA VEZ al final -- dividir repetidas
            // veces por 10.0 (una por digito) acumula error de
            // redondeo en cada paso y puede acabar 1 bit distinto del
            // valor "correcto" que dan atof()/strtod() reales. Con un
            // solo entero exacto y una sola division, coincide con
            // ellos en la practica totalidad de los casos razonables.
            int64_t combined = 0;
            double divisor = 1.0;
            for (int i = 0; i < int_len; i++) combined = combined * 10 + (lx->src[start + i] - '0');
            for (int i = 0; i < frac_len; i++) {
                combined = combined * 10 + (lx->src[frac_start + i] - '0');
                divisor = divisor * 10.0;
            }
            tok.num_value = (double)combined / divisor;
        } else {
            char int_part[TOKEN_TEXT_MAX];
            nb_strncpy(int_part, tok.text, (uint32_t)int_len + 1);
            tok.num_value = (double)nb_atoi64(int_part);
        }
        tok.type = T_NUMBER;
        return tok;
    }

    // Literales hexadecimales: $1A2B -- aqui el simbolo de dolar es
    // un PREFIJO de numero, distinto del sufijo "cadena" que va
    // DESPUES de un identificador (Str$). Se distinguen mirando el
    // caracter siguiente: si tras el $ viene un digito hexadecimal,
    // es un numero, no el inicio de un identificador.
    if (c == '$' && nb_isxdigit(peek2(lx))) {
        advance(lx); // consume el '$'
        int64_t hexval = 0;
        while (nb_isxdigit(peek(lx))) {
            char hc = advance(lx);
            int digit;
            if (hc >= '0' && hc <= '9') digit = hc - '0';
            else if (hc >= 'a' && hc <= 'f') digit = hc - 'a' + 10;
            else digit = hc - 'A' + 10;
            hexval = hexval * 16 + digit;
        }
        tok.num_value = hexval;
        tok.text[0] = '\0';
        tok.type = T_NUMBER;
        return tok;
    }

    // Literales binarios: %1001 -- mismo patron que el hexadecimal.
    if (c == '%' && (peek2(lx) == '0' || peek2(lx) == '1')) {
        advance(lx); // consume el '%'
        int64_t binval = 0;
        while (peek(lx) == '0' || peek(lx) == '1') {
            char bc = advance(lx);
            binval = binval * 2 + (bc - '0');
        }
        tok.num_value = binval;
        tok.text[0] = '\0';
        tok.type = T_NUMBER;
        return tok;
    }

    // Cadenas: "..." (sin escapes en v1 -- BlitzPlus tampoco los usa
    // demasiado; una comilla doble dentro de una cadena se duplica: "")
    if (c == '"') {
        advance(lx);
        int start = lx->pos;
        int len = 0;
        while (peek(lx) != '\0' && peek(lx) != '\n') {
            if (peek(lx) == '"') {
                if (peek2(lx) == '"') { // comilla escapada como ""
                    if (len < TOKEN_TEXT_MAX - 1) tok.text[len++] = '"';
                    advance(lx); advance(lx);
                    continue;
                }
                break;
            }
            if (len < TOKEN_TEXT_MAX - 1) tok.text[len++] = peek(lx);
            advance(lx);
        }
        (void)start;
        if (peek(lx) == '"') advance(lx); // cierre
        tok.text[len] = '\0';
        tok.type = T_STRING;
        return tok;
    }

    // Etiquetas de datos: ".nombre" -- marcan una posicion dentro del
    // flujo de Data para que Restore pueda saltar ahi.
    if (c == '.' && nb_isalpha(peek2(lx))) {
        advance(lx);
        int start = lx->pos;
        while (nb_isalnum(peek(lx)) || peek(lx) == '_') advance(lx);
        int len = lx->pos - start;
        if (len >= TOKEN_TEXT_MAX) len = TOKEN_TEXT_MAX - 1;
        nb_memcpy(tok.text, lx->src + start, (uint32_t)len);
        tok.text[len] = '\0';
        tok.type = T_DATALABEL;
        return tok;
    }

    // Identificadores y palabras clave: letra inicial, luego
    // letras/digitos/guion bajo, con sufijo opcional $ o # que forma
    // parte del propio nombre (asi "nombre$" y "nombre" son variables
    // DISTINTAS, como en BlitzPlus de verdad). El sufijo '%' (entero
    // explicito) es distinto: puramente decorativo, se descarta del
    // texto del token (ver la nota equivalente en el host).
    if (nb_isalpha(c) || c == '_') {
        int start = lx->pos;
        while (nb_isalnum(peek(lx)) || peek(lx) == '_') advance(lx);
        int base_end = lx->pos;
        bool has_percent = false;
        if (peek(lx) == '$' || peek(lx) == '#') {
            advance(lx);
            base_end = lx->pos;
        } else if (peek(lx) == '%') {
            advance(lx);
            has_percent = true;
        }
        int after_suffix = lx->pos;

        // Anotacion de tipo de una instancia de Type: "variable.Tipo"
        // -- ver la nota equivalente en el compilador del host.
        if (peek(lx) == '.' && nb_isalpha(peek2(lx))) {
            advance(lx); // el punto
            while (nb_isalnum(peek(lx)) || peek(lx) == '_') advance(lx);
        }
        int end = lx->pos;

        if (!has_percent) {
            int len = end - start;
            if (len >= TOKEN_TEXT_MAX) len = TOKEN_TEXT_MAX - 1;
            nb_memcpy(tok.text, lx->src + start, (uint32_t)len);
            tok.text[len] = '\0';
        } else {
            int len1 = base_end - start;
            int len2 = end - after_suffix;
            int len = len1 + len2;
            if (len >= TOKEN_TEXT_MAX) len = TOKEN_TEXT_MAX - 1;
            int copy1 = len1 > len ? len : len1;
            nb_memcpy(tok.text, lx->src + start, (uint32_t)copy1);
            int remaining = len - copy1;
            if (remaining > 0) nb_memcpy(tok.text + copy1, lx->src + after_suffix, (uint32_t)remaining);
            tok.text[len] = '\0';
        }

        char upper[TOKEN_TEXT_MAX];
        int i = 0;
        for (; tok.text[i]; i++) upper[i] = (char)nb_toupper(tok.text[i]);
        upper[i] = '\0';

        TokenType kw = lookup_keyword(upper);

        // "END FUNCTION" / "END TYPE" -- el lexer las funde en un
        // unico token para que el parser no tenga que lidiar con
        // secuencias de dos palabras clave.
        if (kw == T_KW_END) {
            int save_pos = lx->pos, save_line = lx->line;
            while (peek(lx) == ' ' || peek(lx) == '\t') advance(lx);
            int w_start = lx->pos;
            while (nb_isalpha(peek(lx))) advance(lx);
            int w_len = lx->pos - w_start;
            char next_upper[16];
            if (w_len > 0 && w_len < 16) {
                for (int k = 0; k < w_len; k++) next_upper[k] = (char)nb_toupper(lx->src[w_start + k]);
                next_upper[w_len] = '\0';
                if (nb_strcmp(next_upper, "FUNCTION") == 0) { tok.type = T_KW_ENDFUNCTION; return tok; }
                if (nb_strcmp(next_upper, "TYPE") == 0) { tok.type = T_KW_ENDTYPE; return tok; }
                if (nb_strcmp(next_upper, "SELECT") == 0) { tok.type = T_KW_ENDSELECT; return tok; }
                if (nb_strcmp(next_upper, "IF") == 0) { tok.type = T_KW_ENDIF; return tok; }
            }
            // no era "End Function"/"End Type" -- retrocedemos
            lx->pos = save_pos;
            lx->line = save_line;
        }

        tok.type = kw;
        return tok;
    }

    // Operadores y puntuacion
    advance(lx);
    switch (c) {
        case '+': tok.type = T_PLUS; return tok;
        case '-': tok.type = T_MINUS; return tok;
        case '*': tok.type = T_STAR; return tok;
        case '/': tok.type = T_SLASH; return tok;
        case '\\': tok.type = T_BACKSLASH; return tok;
        case '(': tok.type = T_LPAREN; return tok;
        case ')': tok.type = T_RPAREN; return tok;
        case ',': tok.type = T_COMMA; return tok;
        case ':': tok.type = T_COLON; return tok;
        case '=':
            if (peek(lx) == '>') { advance(lx); tok.type = T_GE; return tok; }
            if (peek(lx) == '<') { advance(lx); tok.type = T_LE; return tok; }
            tok.type = T_EQ; return tok;
        case '<':
            if (peek(lx) == '=') { advance(lx); tok.type = T_LE; return tok; }
            if (peek(lx) == '>') { advance(lx); tok.type = T_NE; return tok; }
            tok.type = T_LT; return tok;
        case '>':
            if (peek(lx) == '=') { advance(lx); tok.type = T_GE; return tok; }
            tok.type = T_GT; return tok;
        default:
            // Caracter no reconocido -- lo devolvemos como un
            // identificador de un solo caracter para que el parser
            // pueda reportar un error de sintaxis con contexto, en vez
            // de que el lexer se quede atascado.
            tok.text[0] = c;
            tok.text[1] = '\0';
            tok.type = T_IDENT;
            return tok;
    }
}

const char *token_type_name(TokenType type) {
    switch (type) {
        case T_EOF: return "fin de archivo";
        case T_NEWLINE: return "fin de linea";
        case T_NUMBER: return "numero";
        case T_STRING: return "cadena";
        case T_IDENT: return "identificador";
        case T_PLUS: return "+";
        case T_MINUS: return "-";
        case T_STAR: return "*";
        case T_SLASH: return "/";
        case T_EQ: return "=";
        case T_LT: return "<";
        case T_GT: return ">";
        case T_LE: return "<=";
        case T_GE: return ">=";
        case T_NE: return "<>";
        case T_LPAREN: return "(";
        case T_RPAREN: return ")";
        case T_COMMA: return ",";
        case T_COLON: return ":";
        case T_KW_IF: return "If";
        case T_KW_THEN: return "Then";
        case T_KW_ELSE: return "Else";
        case T_KW_ELSEIF: return "ElseIf";
        case T_KW_ENDIF: return "EndIf";
        case T_KW_FOR: return "For";
        case T_KW_TO: return "To";
        case T_KW_STEP: return "Step";
        case T_KW_NEXT: return "Next";
        case T_KW_WHILE: return "While";
        case T_KW_WEND: return "Wend";
        case T_KW_REPEAT: return "Repeat";
        case T_KW_UNTIL: return "Until";
        case T_KW_FOREVER: return "Forever";
        case T_KW_EXIT: return "Exit";
        case T_KW_GOTO: return "Goto";
        case T_KW_GOSUB: return "Gosub";
        case T_KW_FUNCTION: return "Function";
        case T_KW_ENDFUNCTION: return "End Function";
        case T_KW_RETURN: return "Return";
        case T_KW_DIM: return "Dim";
        case T_KW_TYPE: return "Type";
        case T_KW_ENDTYPE: return "End Type";
        case T_KW_NEW: return "New";
        case T_KW_DELETE: return "Delete";
        case T_KW_FIRST: return "First";
        case T_KW_LAST: return "Last";
        case T_KW_BEFORE: return "Before";
        case T_KW_AFTER: return "After";
        case T_KW_INSERT: return "Insert";
        case T_KW_FIELD: return "Field";
        case T_KW_EACH: return "Each";
        case T_KW_NULL: return "Null";
        case T_BACKSLASH: return "\\";
        case T_KW_SELECT: return "Select";
        case T_KW_CASE: return "Case";
        case T_KW_DEFAULT: return "Default";
        case T_KW_ENDSELECT: return "End Select";
        case T_KW_DATA: return "Data";
        case T_KW_READ: return "Read";
        case T_KW_RESTORE: return "Restore";
        case T_DATALABEL: return "etiqueta de datos";
        case T_KW_GLOBAL: return "Global";
        case T_KW_LOCAL: return "Local";
        case T_KW_CONST: return "Const";
        case T_KW_MOD: return "Mod";
        case T_KW_AND: return "And";
        case T_KW_OR: return "Or";
        case T_KW_NOT: return "Not";
        case T_KW_XOR: return "Xor";
        case T_KW_SHL: return "Shl";
        case T_KW_SHR: return "Shr";
        case T_KW_SAR: return "Sar";
        case T_KW_TRUE: return "True";
        case T_KW_FALSE: return "False";
        case T_KW_PRINT: return "Print";
        case T_KW_CLS: return "Cls";
        case T_KW_PLOT: return "Plot";
        case T_KW_LINE: return "Line";
        case T_KW_RECT: return "Rect";
        case T_KW_DELAY: return "Delay";
        case T_KW_END: return "End";
        default: return "?";
    }
}
