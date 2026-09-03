#include <stdio.h>
#include "lexer.h"

static const char *sample =
    "; comentario de prueba\n"
    "Global x = 5\n"
    "For i = 1 To 10 Step 2\n"
    "    Print \"Hola \" + Str$(i)\n"
    "Next\n"
    "If x > 3 Then\n"
    "    Print \"grande\"\n"
    "EndIf\n"
    "Function Suma(a, b)\n"
    "    Return a + b\n"
    "End Function\n";

int main(void) {
    Lexer lx;
    lexer_init(&lx, sample);

    int count = 0;
    for (;;) {
        Token t = lexer_next(&lx);
        count++;
        if (t.type == T_NUMBER) {
            printf("linea %2d: NUMBER  %s (%.2f)\n", t.line, t.text, t.num_value);
        } else if (t.type == T_STRING) {
            printf("linea %2d: STRING  \"%s\"\n", t.line, t.text);
        } else if (t.type == T_IDENT) {
            printf("linea %2d: IDENT   %s\n", t.line, t.text);
        } else if (t.type == T_NEWLINE) {
            printf("linea %2d: NEWLINE\n", t.line);
        } else {
            printf("linea %2d: KEYWORD/OP  %s\n", t.line, token_type_name(t.type));
        }
        if (t.type == T_EOF) break;
        if (count > 200) { printf("(demasiados tokens, algo va mal)\n"); break; }
    }
    printf("\nTotal tokens: %d\n", count);
    return 0;
}
