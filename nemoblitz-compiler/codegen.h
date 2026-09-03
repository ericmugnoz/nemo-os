// codegen.h — compilador Nemo-Blitz
#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include <stdio.h>

// Genera ensamblador AArch64 (compatible con aarch64-elf-as, el mismo
// toolchain que usa Nemo OS) a partir del AST, y lo escribe en 'out'.
void codegen_generate(Node *program, FILE *out);

#endif
