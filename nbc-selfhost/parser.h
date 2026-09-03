// parser.h — compilador Nemo-Blitz
#ifndef PARSER_H
#define PARSER_H

#include "ast.h"

// Analiza un programa completo y devuelve la raiz del AST (un
// N_BLOCK con todas las sentencias de nivel superior, incluidas las
// definiciones de funcion). Si encuentra un error de sintaxis,
// imprime un mensaje con la linea y termina el proceso -- para un
// compilador de este tamaño, no merece la pena la complejidad de una
// recuperacion de errores mas fina en esta primera version.
Node *parse_program(const char *source);

#endif
