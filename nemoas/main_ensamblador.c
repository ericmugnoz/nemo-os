// main.c — driver de linea de comandos para probar el ensamblador en
// el host (Mac/Linux). La version que corra DENTRO de Nemo OS sera
// distinta (sin FILE*, usando las syscalls del kernel), pero la
// logica de ensamblado (assembler.c/asm_encode.c/asm_lexer.c) es
// exactamente la misma -- solo cambia como se lee el archivo de
// entrada y se escribe el de salida.
#include <stdio.h>
#include <stdlib.h>
#include "assembler.h"

static char *read_file(const char *path, long *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "no se pudo abrir %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "error leyendo %s\n", path); exit(1); }
    buf[size] = '\0';
    fclose(f);
    if (out_size) *out_size = size;
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s entrada.s salida.bin\n", argv[0]);
        return 1;
    }
    char *source = read_file(argv[1], NULL);

    static uint8_t out_buf[256 * 1024];
    int64_t size = assemble(source, out_buf, sizeof(out_buf));
    if (size < 0) return 1;

    FILE *out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "no se pudo crear %s\n", argv[2]); return 1; }
    fwrite(out_buf, 1, (size_t)size, out);
    fclose(out);

    printf("Ensamblado correctamente: %s -> %s (%lld bytes)\n", argv[1], argv[2], (long long)size);
    return 0;
}
