// asm_lexer.c — ensamblador Nemo-AS

#include "asm_lexer.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>

void asm_lexer_init(AsmLexer *lx, const char *source) {
    lx->src = source;
    lx->pos = 0;
    lx->line = 1;
}

static char peek(AsmLexer *lx) { return lx->src[lx->pos]; }
static char peek2(AsmLexer *lx) { return lx->src[lx->pos] ? lx->src[lx->pos + 1] : '\0'; }
static char advance(AsmLexer *lx) {
    char c = lx->src[lx->pos];
    if (c != '\0') lx->pos++;
    return c;
}

static bool starts_with(AsmLexer *lx, const char *s) {
    int i = 0;
    while (s[i]) {
        if (lx->src[lx->pos + i] != s[i]) return false;
        i++;
    }
    return true;
}

AsmToken asm_lexer_next(AsmLexer *lx) {
    AsmToken tok;
    memset(&tok, 0, sizeof(tok));

    for (;;) {
        char c = peek(lx);
        if (c == ' ' || c == '\t' || c == '\r') { advance(lx); continue; }
        if (c == '/' && peek2(lx) == '/') { while (peek(lx) != '\0' && peek(lx) != '\n') advance(lx); continue; }
        break;
    }

    tok.line = lx->line;
    char c = peek(lx);

    if (c == '\0') { tok.type = AT_EOF; return tok; }
    if (c == '\n') { advance(lx); lx->line++; tok.type = AT_NEWLINE; return tok; }

    // Numeros: "#123", "#-16", "#0xFFFFFF", o desnudos (.space 8, .align 3)
    if (c == '#' || isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)peek2(lx)))) {
        bool had_hash = (c == '#');
        if (had_hash) advance(lx);
        bool neg = false;
        int start_digits = lx->pos;
        if (peek(lx) == '-') { neg = true; advance(lx); }
        long long value = 0;
        if (peek(lx) == '0' && (peek2(lx) == 'x' || peek2(lx) == 'X')) {
            advance(lx); advance(lx);
            while (isxdigit((unsigned char)peek(lx))) {
                char h = advance(lx);
                int d = (h >= '0' && h <= '9') ? h - '0' : (tolower(h) - 'a' + 10);
                value = value * 16 + d;
            }
        } else {
            while (isdigit((unsigned char)peek(lx))) {
                value = value * 10 + (advance(lx) - '0');
            }
        }

        // Etiquetas numericas locales (sintaxis de GNU as: "1:" las
        // define, "1f"/"1b" las referencian -- "hacia adelante" o
        // "hacia atras"). Nuestro propio generador las usa dentro de
        // las rutinas de tiempo de ejecucion. Como cada numero solo
        // aparece UNA vez por archivo en nuestro caso, las tratamos
        // como una etiqueta normal (con el numero como nombre) y
        // simplemente ignoramos la 'f'/'b' -- si algun dia se
        // reutilizara el mismo numero dos veces, la definicion
        // duplicada se detecta y se avisa igualmente.
        if (!had_hash && !neg) {
            if (peek(lx) == ':') {
                tok.type = AT_IDENT;
                int len = lx->pos - start_digits;
                if (len >= ASM_TOKEN_TEXT_MAX) len = ASM_TOKEN_TEXT_MAX - 1;
                memcpy(tok.text, lx->src + start_digits, (size_t)len);
                tok.text[len] = '\0';
                return tok;
            }
            if ((peek(lx) == 'f' || peek(lx) == 'b') && !isalnum((unsigned char)peek2(lx))) {
                advance(lx); // consumimos la 'f'/'b', no la guardamos
                tok.type = AT_IDENT;
                int len = lx->pos - start_digits - 1;
                if (len >= ASM_TOKEN_TEXT_MAX) len = ASM_TOKEN_TEXT_MAX - 1;
                memcpy(tok.text, lx->src + start_digits, (size_t)len);
                tok.text[len] = '\0';
                return tok;
            }
        }

        tok.type = AT_NUMBER;
        tok.num_value = neg ? -value : value;
        return tok;
    }

    // Cadenas (.asciz "...") -- mismo formato de escapes que emitimos
    // nosotros mismos: salto de linea, tabulador, comilla y barra invertida
    if (c == '"') {
        advance(lx);
        int len = 0;
        while (peek(lx) != '\0' && peek(lx) != '"') {
            char ch = advance(lx);
            if (ch == '\\') {
                char esc = advance(lx);
                if (esc == 'n') ch = '\n';
                else if (esc == 't') ch = '\t';
                else ch = esc; // \" y \\ se quedan tal cual
            }
            if (len < ASM_TOKEN_TEXT_MAX - 1) tok.text[len++] = ch;
        }
        if (peek(lx) == '"') advance(lx);
        tok.text[len] = '\0';
        tok.type = AT_STRING;
        return tok;
    }

    // ":lo12:etiqueta" -- prefijo especial de reubicacion
    if (c == ':' && starts_with(lx, ":lo12:")) {
        for (int i = 0; i < 6; i++) advance(lx);
        tok.type = AT_LO12;
        return tok;
    }
    if (c == ':') { advance(lx); tok.type = AT_COLON; return tok; }

    // Identificadores -- incluye etiquetas locales ".Lxxx" (las
    // generamos siempre con ese prefijo exacto, asi que basta con
    // reconocer el patron ".L").
    if (isalpha((unsigned char)c) || c == '_' || (c == '.' && peek2(lx) == 'L')) {
        int start = lx->pos;
        if (c == '.') advance(lx); // el punto inicial de ".Lxxx"
        while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_' || peek(lx) == '$') advance(lx);
        int len = lx->pos - start;
        if (len >= ASM_TOKEN_TEXT_MAX) len = ASM_TOKEN_TEXT_MAX - 1;
        memcpy(tok.text, lx->src + start, (size_t)len);
        tok.text[len] = '\0';
        tok.type = AT_IDENT;
        return tok;
    }

    // Directivas: ".section", ".global", ".asciz", ".space", ".align", ".bss", ".rodata"...
    if (c == '.') {
        advance(lx);
        int start = lx->pos;
        while (isalnum((unsigned char)peek(lx)) || peek(lx) == '.' || peek(lx) == '_') advance(lx);
        int len = lx->pos - start;
        if (len >= ASM_TOKEN_TEXT_MAX) len = ASM_TOKEN_TEXT_MAX - 1;
        memcpy(tok.text, lx->src + start, (size_t)len);
        tok.text[len] = '\0';
        tok.type = AT_DIRECTIVE;
        return tok;
    }

    advance(lx);
    switch (c) {
        case ',': tok.type = AT_COMMA; return tok;
        case '!': tok.type = AT_BANG; return tok;
        case '[': tok.type = AT_LBRACKET; return tok;
        case ']': tok.type = AT_RBRACKET; return tok;
        default:
            // Caracter no reconocido -- lo devolvemos como identificador
            // de un caracter para poder dar un error con contexto en
            // vez de atascarnos.
            tok.text[0] = c;
            tok.text[1] = '\0';
            tok.type = AT_IDENT;
            return tok;
    }
}
