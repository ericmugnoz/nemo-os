// asm_lexer.h — ensamblador Nemo-AS
//
// Tokenizador de la sintaxis de ensamblador que usa nuestro propio
// generador de codigo (compatible con lo que ya escribiamos para
// aarch64-elf-as, pero solo el subconjunto que de verdad usamos).

#ifndef ASM_LEXER_H
#define ASM_LEXER_H

typedef enum {
    AT_EOF, AT_NEWLINE,
    AT_IDENT,       // mnemonico, registro, o nombre de simbolo/etiqueta
    AT_NUMBER,      // literal numerico (tras '#', o desnudo en un desplazamiento)
    AT_STRING,      // cadena entre comillas (.asciz "...")
    AT_COMMA, AT_COLON, AT_HASH, AT_BANG,
    AT_LBRACKET, AT_RBRACKET,
    AT_DIRECTIVE,   // .section, .global, .asciz, .space, .align, .word...
    AT_LO12,        // ":lo12:" -- prefijo especial antes de un simbolo
} AsmTokenType;

#define ASM_TOKEN_TEXT_MAX 64

typedef struct {
    AsmTokenType type;
    char text[ASM_TOKEN_TEXT_MAX]; // identificador, cadena, o directiva (sin el punto)
    long long num_value;            // valido solo si type == AT_NUMBER
    int line;
} AsmToken;

typedef struct {
    const char *src;
    int pos;
    int line;
} AsmLexer;

void asm_lexer_init(AsmLexer *lx, const char *source);
AsmToken asm_lexer_next(AsmLexer *lx);

#endif
