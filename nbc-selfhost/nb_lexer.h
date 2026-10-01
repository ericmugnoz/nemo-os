// nb_lexer.h — lexer del compilador Nemo-Blitz 2.0.
//
// Convierte texto fuente en una secuencia de tokens. Gramática FIEL a
// la ya auditada (357 funciones, ~60 palabras clave, todos los casos
// límite ya encontrados en la tanda 1 de la auditoría) -- esta pieza
// se reescribe por higiene de memoria (ver más abajo), no porque el
// lexer viejo tuviera bugs de sintaxis.
//
// Diferencia real frente al lexer viejo: el texto de cada token
// (identificador, cadena, etiqueta de datos) usa nb_string_t
// (nb_string.c), no un `char text[64]` de tamaño fijo. Una cadena
// literal de más de 63 caracteres en el código fuente se truncaba en
// silencio con el diseño viejo -- exactamente la clase de bug que
// esta reescritura existe para eliminar (ver el principio 0.1 del
// documento de diseño).

#ifndef NB_LEXER_H
#define NB_LEXER_H

#include <stdint.h>
#include <stdbool.h>

typedef struct nb_string nb_string_t; // ver nb_string.c -- tipo opaco aqui

typedef enum {
    TOK_EOF, TOK_NEWLINE,

    TOK_NUMBER, TOK_STRING, TOK_IDENT,

    // Operadores y puntuacion
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH,
    TOK_EQ, TOK_LT, TOK_GT, TOK_LE, TOK_GE, TOK_NE,
    TOK_LPAREN, TOK_RPAREN, TOK_COMMA, TOK_COLON, TOK_BACKSLASH,

    // Control de flujo
    TOK_KW_IF, TOK_KW_THEN, TOK_KW_ELSE, TOK_KW_ELSEIF, TOK_KW_ENDIF,
    TOK_KW_FOR, TOK_KW_TO, TOK_KW_STEP, TOK_KW_NEXT,
    TOK_KW_WHILE, TOK_KW_WEND,
    TOK_KW_REPEAT, TOK_KW_UNTIL, TOK_KW_FOREVER, TOK_KW_EXIT,
    TOK_KW_GOTO, TOK_KW_GOSUB,

    // Funciones y datos
    TOK_KW_FUNCTION, TOK_KW_ENDFUNCTION, TOK_KW_RETURN,
    TOK_KW_DIM, TOK_KW_TYPE, TOK_KW_ENDTYPE, TOK_KW_NEW, TOK_KW_DELETE,
    TOK_KW_FIRST, TOK_KW_LAST, TOK_KW_FIELD, TOK_KW_EACH, TOK_KW_NULL,
    TOK_KW_BEFORE, TOK_KW_AFTER, TOK_KW_INSERT,
    TOK_KW_GLOBAL, TOK_KW_LOCAL, TOK_KW_CONST,

    // Select / Case
    TOK_KW_SELECT, TOK_KW_CASE, TOK_KW_DEFAULT, TOK_KW_ENDSELECT,

    // Data / Read / Restore
    TOK_KW_DATA, TOK_KW_READ, TOK_KW_RESTORE, TOK_DATALABEL,

    // Operadores logicos con nombre
    TOK_KW_MOD, TOK_KW_AND, TOK_KW_OR, TOK_KW_NOT,
    TOK_KW_XOR, TOK_KW_SHL, TOK_KW_SHR, TOK_KW_SAR,
    TOK_KW_TRUE, TOK_KW_FALSE,

    // Comandos incorporados con sintaxis propia (el resto de
    // funciones -- las ~350 restantes del inventario -- son
    // identificadores normales, reconocidos por nombre en el parser,
    // no palabras clave del lexer)
    TOK_KW_PRINT, TOK_KW_CLS, TOK_KW_PLOT, TOK_KW_LINE, TOK_KW_RECT,
    TOK_KW_DELAY, TOK_KW_END,
    TOK_KW_COLOR, TOK_KW_GRAPHICS, TOK_KW_OVAL, TOK_KW_TEXT,
    TOK_KW_CREATEWINDOW, TOK_KW_CONSOLE, TOK_KW_DESKTOP,

    // Manejo de errores real (esteroide #1 del diseño) -- palabras
    // clave nuevas, no existian en el lenguaje viejo.
    TOK_KW_TRY, TOK_KW_CATCH, TOK_KW_ENDTRY, TOK_KW_THROW,
} nb_token_type_t;

typedef struct {
    nb_token_type_t type;
    nb_string_t *text; // NULL salvo en TOK_STRING/TOK_IDENT/TOK_DATALABEL --
                        // el llamador es dueño de esta referencia (nb_string_release
                        // cuando ya no la necesite), igual que cualquier otra
                        // nb_string_t que se recibe de una funcion que la crea.
    double num_value;  // valido solo si type == TOK_NUMBER
    bool is_float;      // TOK_NUMBER: true si el literal llevaba punto decimal
                         // (5 es entero, 5.0 es flotante, aunque valgan lo mismo) --
                         // false para hex/binario, que siempre son enteros.
    int32_t line;
} nb_token_t;

typedef struct {
    const char *src;
    uint32_t pos;
    int32_t line;
} nb_lexer_t;

void nb_lexer_init(nb_lexer_t *lx, const char *source);

// Devuelve el siguiente token y avanza. Al llegar al final del
// archivo, sigue devolviendo TOK_EOF indefinidamente.
nb_token_t nb_lexer_next(nb_lexer_t *lx);

const char *nb_token_type_name(nb_token_type_t type);

#endif
