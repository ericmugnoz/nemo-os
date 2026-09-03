#include <stdio.h>
#include "asm_lexer.h"

static const char *sample =
    "_start:\n"
    "    stp x29, x30, [sp, #-16]!\n"
    "    mov x29, sp\n"
    ".Lfor_0:\n"
    "    adrp x0, var_total\n"
    "    add x0, x0, :lo12:var_total\n"
    ".section .rodata\n"
    "str_0: .asciz \"Total: \"\n"
    ".section .bss\n"
    "var_total: .space 8\n";

static const char *name(AsmTokenType t) {
    switch (t) {
        case AT_EOF: return "EOF"; case AT_NEWLINE: return "NEWLINE";
        case AT_IDENT: return "IDENT"; case AT_NUMBER: return "NUMBER";
        case AT_STRING: return "STRING"; case AT_COMMA: return "COMMA";
        case AT_COLON: return "COLON"; case AT_HASH: return "HASH";
        case AT_BANG: return "BANG"; case AT_LBRACKET: return "LBRACKET";
        case AT_RBRACKET: return "RBRACKET"; case AT_DIRECTIVE: return "DIRECTIVE";
        case AT_LO12: return "LO12"; default: return "?";
    }
}

int main(void) {
    AsmLexer lx;
    asm_lexer_init(&lx, sample);
    int count = 0;
    for (;;) {
        AsmToken t = asm_lexer_next(&lx);
        count++;
        if (t.type == AT_IDENT || t.type == AT_STRING || t.type == AT_DIRECTIVE) {
            printf("%-10s '%s'\n", name(t.type), t.text);
        } else if (t.type == AT_NUMBER) {
            printf("%-10s %lld\n", name(t.type), t.num_value);
        } else {
            printf("%-10s\n", name(t.type));
        }
        if (t.type == AT_EOF) break;
        if (count > 300) { printf("(demasiados tokens)\n"); break; }
    }
    return 0;
}
