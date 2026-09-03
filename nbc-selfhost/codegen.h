// codegen.h — compilador Nemo-Blitz
#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include "nb_output.h"

// Genera ensamblador AArch64 (compatible con nuestro propio
// ensamblador Nemo-AS) a partir del AST, y lo escribe en 'out' (un
// buffer en memoria, preparado antes con nb_output_begin).
void codegen_generate(Node *program, NBOut *out);

#endif
