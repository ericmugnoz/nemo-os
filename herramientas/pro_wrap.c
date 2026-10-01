// pro_wrap.c -- utilidad de HOST: le pone a un binario crudo la cabecera
// NEXE de 16 bytes que el cargador de Nemo OS espera, convirtiendolo en un
// .pro ejecutable.
//
// PARA QUE. Los programas que viajan dentro del kernel (shell.pro, nbc.pro,
// explorer.pro...) reciben esa cabecera en el arranque, cuando el cargador
// los instala desde su blob. Un programa que NO va embebido no pasa por ahi,
// asi que hay que ponersela a mano antes de copiarlo al sistema de archivos
// y lanzarlo con el 'run' de la shell. El caso real es lua.pro: el interprete
// de Lua se construye aparte (ver lua/Makefile) y se instala como archivo.
//
// La cabecera que escribe es la de 16 bytes, que el cargador sigue aceptando
// (ver src/loader.c: reconoce las tres versiones). Los programas compilados
// por nbc llevan la de 24, con el tamaño de memoria y las banderas.
//
// Uso: pro_wrap entrada.bin salida.pro [entry_offset]

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "uso: pro_wrap entrada.bin salida.pro [entry_offset]\n");
        return 1;
    }
    uint32_t entry_offset = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;

    FILE *in = fopen(argv[1], "rb");
    if (!in) { fprintf(stderr, "pro_wrap: no se pudo abrir '%s'\n", argv[1]); return 1; }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);

    unsigned char *buf = malloc((size_t)size);
    if (fread(buf, 1, (size_t)size, in) != (size_t)size) {
        fprintf(stderr, "pro_wrap: lectura incompleta de '%s'\n", argv[1]);
        fclose(in); free(buf); return 1;
    }
    fclose(in);

    FILE *out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "pro_wrap: no se pudo crear '%s'\n", argv[2]); free(buf); return 1; }

    uint32_t version = 1, code_size = (uint32_t)size;
    fwrite("NEXE", 1, 4, out);
    fwrite(&version, 4, 1, out);
    fwrite(&entry_offset, 4, 1, out);
    fwrite(&code_size, 4, 1, out);
    fwrite(buf, 1, (size_t)size, out);
    fclose(out);
    free(buf);

    printf("pro_wrap: %s (%ld bytes de codigo) -> %s\n", argv[1], size, argv[2]);
    return 0;
}
