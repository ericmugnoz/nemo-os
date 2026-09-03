// lexer.c — compilador Nemo-Blitz

#include "lexer.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>

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

typedef struct { const char *word; TokenType type; } KeywordEntry;

static const KeywordEntry keywords[] = {
    {"IF", T_KW_IF}, {"THEN", T_KW_THEN}, {"ELSE", T_KW_ELSE}, {"ELSEIF", T_KW_ELSEIF}, {"ENDIF", T_KW_ENDIF},
    {"FOR", T_KW_FOR}, {"TO", T_KW_TO}, {"STEP", T_KW_STEP}, {"NEXT", T_KW_NEXT},
    {"WHILE", T_KW_WHILE}, {"WEND", T_KW_WEND},
    {"REPEAT", T_KW_REPEAT}, {"UNTIL", T_KW_UNTIL}, {"FOREVER", T_KW_FOREVER}, {"EXIT", T_KW_EXIT},
    {"GOTO", T_KW_GOTO}, {"GOSUB", T_KW_GOSUB},
    {"FUNCTION", T_KW_FUNCTION}, {"RETURN", T_KW_RETURN},
    {"DIM", T_KW_DIM}, {"TYPE", T_KW_TYPE}, {"NEW", T_KW_NEW}, {"DELETE", T_KW_DELETE},
    {"FIRST", T_KW_FIRST}, {"LAST", T_KW_LAST}, {"FIELD", T_KW_FIELD}, {"EACH", T_KW_EACH}, {"NULL", T_KW_NULL},
    {"BEFORE", T_KW_BEFORE}, {"AFTER", T_KW_AFTER}, {"INSERT", T_KW_INSERT},
    {"GLOBAL", T_KW_GLOBAL}, {"LOCAL", T_KW_LOCAL}, {"CONST", T_KW_CONST},
    {"SELECT", T_KW_SELECT}, {"CASE", T_KW_CASE}, {"DEFAULT", T_KW_DEFAULT},
    {"DATA", T_KW_DATA}, {"READ", T_KW_READ}, {"RESTORE", T_KW_RESTORE},
    {"MOD", T_KW_MOD}, {"AND", T_KW_AND}, {"OR", T_KW_OR}, {"NOT", T_KW_NOT},
    {"XOR", T_KW_XOR}, {"SHL", T_KW_SHL}, {"SHR", T_KW_SHR}, {"SAR", T_KW_SAR},
    {"TRUE", T_KW_TRUE}, {"FALSE", T_KW_FALSE},
    {"PRINT", T_KW_PRINT}, {"CLS", T_KW_CLS}, {"PLOT", T_KW_PLOT}, {"LINE", T_KW_LINE}, {"RECT", T_KW_RECT},
    {"DELAY", T_KW_DELAY},
    {"END", T_KW_END},
    {NULL, T_EOF}
};

// "End Function" y "End Type" son dos palabras en BlitzPlus real, pero
// aqui las tratamos como un unico token para simplificar el parser --
// el lexer las junta cuando ve "END" seguido de "FUNCTION"/"TYPE".
static TokenType lookup_keyword(const char *upper) {
    for (int i = 0; keywords[i].word; i++) {
        if (strcmp(keywords[i].word, upper) == 0) return keywords[i].type;
    }
    return T_IDENT;
}

static void skip_line_comment(Lexer *lx) {
    while (peek(lx) != '\0' && peek(lx) != '\n') advance(lx);
}

Token lexer_next(Lexer *lx) {
    Token tok;
    memset(&tok, 0, sizeof(tok));

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

    // Numeros: 123, 123.45
    if (isdigit((unsigned char)c)) {
        int start = lx->pos;
        while (isdigit((unsigned char)peek(lx))) advance(lx);
        if (peek(lx) == '.' && isdigit((unsigned char)peek2(lx))) {
            advance(lx);
            while (isdigit((unsigned char)peek(lx))) advance(lx);
        }
        int len = lx->pos - start;
        if (len >= TOKEN_TEXT_MAX) len = TOKEN_TEXT_MAX - 1;
        memcpy(tok.text, lx->src + start, (size_t)len);
        tok.text[len] = '\0';
        tok.num_value = atof(tok.text);
        tok.type = T_NUMBER;
        return tok;
    }

    // Flotantes que empiezan directamente por el punto, sin cero
    // delante (.5, .098) -- BlitzPlus real los admite (confirmado con
    // ejemplos reales: "Const grav#=.098"). Se distingue de una
    // etiqueta de datos (".etiqueta") porque tras el punto viene un
    // DIGITO, no una letra.
    if (c == '.' && isdigit((unsigned char)peek2(lx))) {
        int start = lx->pos;
        advance(lx); // consume el '.'
        while (isdigit((unsigned char)peek(lx))) advance(lx);
        int len = lx->pos - start;
        if (len >= TOKEN_TEXT_MAX) len = TOKEN_TEXT_MAX - 1;
        memcpy(tok.text, lx->src + start, (size_t)len);
        tok.text[len] = '\0';
        tok.num_value = atof(tok.text);
        tok.type = T_NUMBER;
        return tok;
    }

    // Literales hexadecimales: $1A2B -- aqui el simbolo de dolar es
    // un PREFIJO de numero, distinto del sufijo "cadena" que va
    // DESPUES de un identificador (Str$). Se distinguen mirando el
    // caracter siguiente: si tras el $ viene un digito hexadecimal,
    // es un numero, no el inicio de un identificador.
    if (c == '$' && isxdigit((unsigned char)peek2(lx))) {
        advance(lx); // consume el '$'
        long hexval = 0;
        while (isxdigit((unsigned char)peek(lx))) {
            char hc = advance(lx);
            int digit;
            if (hc >= '0' && hc <= '9') digit = hc - '0';
            else if (hc >= 'a' && hc <= 'f') digit = hc - 'a' + 10;
            else digit = hc - 'A' + 10;
            hexval = hexval * 16 + digit;
        }
        tok.num_value = (double)hexval;
        tok.text[0] = '\0';
        tok.type = T_NUMBER;
        return tok;
    }

    // Literales binarios: %1001 -- mismo patron que el hexadecimal de
    // arriba, con '0'/'1' en vez de digitos hex. Confirmado con
    // ejemplos reales (CreateWindow con flags "%1001").
    if (c == '%' && (peek2(lx) == '0' || peek2(lx) == '1')) {
        advance(lx); // consume el '%'
        long binval = 0;
        while (peek(lx) == '0' || peek(lx) == '1') {
            char bc = advance(lx);
            binval = binval * 2 + (bc - '0');
        }
        tok.num_value = (double)binval;
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

    // Identificadores y palabras clave: letra inicial, luego
    // letras/digitos/guion bajo, con sufijo opcional $ o # que forma
    // parte del propio nombre (asi "nombre$" y "nombre" son variables
    // DISTINTAS, como en BlitzPlus de verdad).
    // Identificadores y palabras clave: letra inicial, luego
    // letras/digitos/guion bajo, con sufijo opcional $ o # que forma
    // parte del propio nombre (asi "nombre$" y "nombre" son variables
    // DISTINTAS, como en BlitzPlus de verdad). El sufijo '%' (entero
    // explicito) es distinto: es puramente decorativo (el entero YA
    // es el tipo por defecto sin sufijo), asi que "x" y "x%" son la
    // MISMA variable -- se consume para no romper el analisis, pero
    // se DESCARTA del texto del token (confirmado con un ejemplo
    // real: "Function f(Convert%)" declarado con sufijo, pero usado
    // como "Convert" a secas en el cuerpo de la funcion).
    if (isalpha((unsigned char)c) || c == '_') {
        int start = lx->pos;
        while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
        int base_end = lx->pos; // fin del nombre base, antes de $/#/%
        bool has_percent = false;
        if (peek(lx) == '$' || peek(lx) == '#') {
            advance(lx);
            base_end = lx->pos; // $/# SI forman parte del nombre
        } else if (peek(lx) == '%') {
            advance(lx);
            has_percent = true; // '%' NO forma parte del nombre -- se salta
        }
        int after_suffix = lx->pos; // aqui podria seguir un ".Tipo"

        // Anotacion de tipo de una instancia de Type: "variable.Tipo"
        // -- BlitzPlus las escribe pegadas, sin espacio. Si tras el
        // nombre viene un punto seguido de una letra, forma parte del
        // MISMO identificador (distinto de una etiqueta de datos,
        // ".nombre", que solo se reconoce cuando el punto es el
        // PRIMER caracter del token, ver mas abajo).
        if (peek(lx) == '.' && isalpha((unsigned char)peek2(lx))) {
            advance(lx); // el punto
            while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
        }
        int end = lx->pos;

        if (!has_percent) {
            int len = end - start;
            if (len >= TOKEN_TEXT_MAX) len = TOKEN_TEXT_MAX - 1;
            memcpy(tok.text, lx->src + start, (size_t)len);
            tok.text[len] = '\0';
        } else {
            // Copiamos el nombre base + lo que venga tras el '%'
            // (un posible ".Tipo"), saltandonos el propio caracter '%'.
            int len1 = base_end - start;
            int len2 = end - after_suffix;
            int len = len1 + len2;
            if (len >= TOKEN_TEXT_MAX) len = TOKEN_TEXT_MAX - 1;
            int copy1 = len1 > len ? len : len1;
            memcpy(tok.text, lx->src + start, (size_t)copy1);
            int remaining = len - copy1;
            if (remaining > 0) memcpy(tok.text + copy1, lx->src + after_suffix, (size_t)remaining);
            tok.text[len] = '\0';
        }

        char upper[TOKEN_TEXT_MAX];
        int i = 0;
        for (; tok.text[i]; i++) upper[i] = (char)toupper((unsigned char)tok.text[i]);
        upper[i] = '\0';

        TokenType kw = lookup_keyword(upper);

        // "End Function", "End Type", "End Select" y "End If" son dos
        // palabras en BlitzPlus real, pero aqui las tratamos como un
        // unico token para simplificar el parser -- el lexer las
        // junta cuando ve "END" seguido de la palabra correspondiente.
        if (kw == T_KW_END) {
            int save_pos = lx->pos, save_line = lx->line;
            while (peek(lx) == ' ' || peek(lx) == '\t') advance(lx);
            int w_start = lx->pos;
            while (isalpha((unsigned char)peek(lx))) advance(lx);
            int w_len = lx->pos - w_start;
            char next_upper[16];
            if (w_len > 0 && w_len < 16) {
                for (int k = 0; k < w_len; k++) next_upper[k] = (char)toupper((unsigned char)lx->src[w_start + k]);
                next_upper[w_len] = '\0';
                if (strcmp(next_upper, "FUNCTION") == 0) { tok.type = T_KW_ENDFUNCTION; return tok; }
                if (strcmp(next_upper, "TYPE") == 0) { tok.type = T_KW_ENDTYPE; return tok; }
                if (strcmp(next_upper, "SELECT") == 0) { tok.type = T_KW_ENDSELECT; return tok; }
                if (strcmp(next_upper, "IF") == 0) { tok.type = T_KW_ENDIF; return tok; } // "End If" -- alternativa valida a "EndIf" en BlitzPlus real
            }
            // no era "End Function"/"End Type" -- retrocedemos
            lx->pos = save_pos;
            lx->line = save_line;
        }

        tok.type = kw;
        return tok;
    }

    // Etiquetas de datos: ".nombre" -- marcan una posicion dentro del
    // flujo de Data para que Restore pueda saltar ahi. Solo tienen
    // sentido al principio de una linea, pero el lexer no necesita
    // saberlo -- el parser decide que hacer con el token.
    if (c == '.' && isalpha((unsigned char)peek2(lx))) {
        advance(lx); // consume el '.'
        int start = lx->pos;
        while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
        int len = lx->pos - start;
        if (len >= TOKEN_TEXT_MAX) len = TOKEN_TEXT_MAX - 1;
        memcpy(tok.text, lx->src + start, (size_t)len);
        tok.text[len] = '\0';
        tok.type = T_DATALABEL;
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
            // BlitzPlus real (como muchos BASIC clasicos) tambien
            // acepta el operador con el orden invertido: "=>" como
            // ">=", "=<" como "<=".
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
