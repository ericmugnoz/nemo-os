// main.c — compilador Nemo-Blitz (herramienta de linea de comandos)
//
// Uso: nemoblitz entrada.bb salida.s
//
// Lee un archivo fuente, lo compila, y escribe el ensamblador AArch64
// resultante. Este binario corre en el ordenador de desarrollo (no
// dentro de Nemo OS) -- ensambla el resultado con aarch64-elf-as /
// aarch64-elf-ld, igual que cualquier otro programa .pro del sistema.

#include <stdio.h>
#include <stdlib.h>
#include "parser.h"
#include "codegen.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("no se pudo abrir el archivo de entrada"); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)size + 1);
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s entrada.bb salida.s\n", argv[0]);
        return 1;
    }

    char *source = read_file(argv[1]);
    Node *program = parse_program(source);

    FILE *out = fopen(argv[2], "w");
    if (!out) { perror("no se pudo crear el archivo de salida"); return 1; }
    codegen_generate(program, out);
    fclose(out);

    fprintf(stderr, "Compilado: %s -> %s\n", argv[1], argv[2]);
    return 0;
}
