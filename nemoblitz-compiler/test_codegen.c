#include <stdio.h>
#include "parser.h"
#include "codegen.h"

static const char *sample =
    "Global total = 0\n"
    "For i = 1 To 5\n"
    "    total = total + i\n"
    "Next\n"
    "Print \"Total: \" + Str$(total)\n"
    "\n"
    "Function Doble(x)\n"
    "    Return x * 2\n"
    "End Function\n"
    "\n"
    "Print Doble(21)\n";

int main(void) {
    Node *program = parse_program(sample);
    codegen_generate(program, stdout);
    return 0;
}
