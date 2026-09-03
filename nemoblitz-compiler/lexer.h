// lexer.h — compilador Nemo-Blitz
//
// Convierte texto fuente en una secuencia de tokens. Primera pieza
// del compilador, autocontenida y facil de probar por separado antes
// de escribir el parser encima.

#ifndef LEXER_H
#define LEXER_H

typedef enum {
    T_EOF, T_NEWLINE,

    T_NUMBER, T_STRING, T_IDENT,

    // Operadores y puntuacion
    T_PLUS, T_MINUS, T_STAR, T_SLASH,
    T_EQ, T_LT, T_GT, T_LE, T_GE, T_NE,
    T_LPAREN, T_RPAREN, T_COMMA, T_COLON,

    // Palabras clave -- control de flujo
    T_KW_IF, T_KW_THEN, T_KW_ELSE, T_KW_ELSEIF, T_KW_ENDIF,
    T_KW_FOR, T_KW_TO, T_KW_STEP, T_KW_NEXT,
    T_KW_WHILE, T_KW_WEND,
    T_KW_REPEAT, T_KW_UNTIL, T_KW_FOREVER, T_KW_EXIT,
    T_KW_GOTO, T_KW_GOSUB,

    // Funciones y datos
    T_KW_FUNCTION, T_KW_ENDFUNCTION, T_KW_RETURN,
    T_KW_DIM, T_KW_TYPE, T_KW_ENDTYPE, T_KW_NEW, T_KW_DELETE,
    T_KW_FIRST, T_KW_LAST, T_KW_FIELD, T_KW_EACH, T_KW_NULL,
    T_KW_BEFORE, T_KW_AFTER, T_KW_INSERT,
    T_BACKSLASH, // var\campo -- acceso a un campo de Type
    T_KW_GLOBAL, T_KW_LOCAL, T_KW_CONST,

    // Select / Case -- se traduce en el parser a una cadena de
    // If/ElseIf ya existente, asi que el generador de codigo no
    // necesita saber que esto existe.
    T_KW_SELECT, T_KW_CASE, T_KW_DEFAULT, T_KW_ENDSELECT,

    // Data / Read / Restore -- bloques de constantes leidos en
    // secuencia. Las etiquetas de datos (".nombre") son un tipo de
    // token aparte, no una palabra clave -- empiezan por punto.
    T_KW_DATA, T_KW_READ, T_KW_RESTORE, T_DATALABEL,

    // Operadores logicos con nombre
    T_KW_MOD, T_KW_AND, T_KW_OR, T_KW_NOT,
    T_KW_XOR, T_KW_SHL, T_KW_SHR, T_KW_SAR,
    T_KW_TRUE, T_KW_FALSE,

    // Comandos incorporados (primer subconjunto)
    T_KW_PRINT, T_KW_CLS, T_KW_PLOT, T_KW_LINE, T_KW_RECT, T_KW_DELAY,
    T_KW_END,
} TokenType;

#define TOKEN_TEXT_MAX 64

typedef struct {
    TokenType type;
    char text[TOKEN_TEXT_MAX]; // identificador, cadena (sin comillas), o el numero como texto
    double num_value;          // valido solo si type == T_NUMBER
    int line;
} Token;

typedef struct {
    const char *src;
    int pos;
    int line;
} Lexer;

void lexer_init(Lexer *lx, const char *source);

// Devuelve el siguiente token y avanza. Al llegar al final del
// archivo, sigue devolviendo T_EOF indefinidamente.
Token lexer_next(Lexer *lx);

// Nombre legible de un tipo de token, para mensajes de error.
const char *token_type_name(TokenType type);

#endif
