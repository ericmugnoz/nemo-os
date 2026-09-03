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
#include <string.h>
#include <strings.h>
#include <stdbool.h>
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

// -- preprocesador de Include --
//
// Include NO es una funcion en tiempo de ejecucion -- es una
// directiva de COMPILACION que "corta y pega" el contenido de otro
// archivo .bb justo en ese punto, ANTES de pasar el resultado al
// lexer/parser (documentacion oficial: "effectively cuts and pastes
// the contents... temporarily, before being passed to the compiler").
// Cada archivo solo se incluye una vez (aunque se mencione varias
// veces, en el mismo o distinto archivo) -- llevamos la lista de ya
// incluidos en 'included_paths'.
//
// Las rutas relativas se resuelven respecto al directorio del
// archivo que CONTIENE el Include (no siempre el archivo de entrada
// original), para que los includes anidados funcionen de forma
// natural sin importar desde donde se compile.

#define MAX_INCLUDES 64
static char included_paths[MAX_INCLUDES][1024];
static int included_count = 0;

static void dirname_of(const char *path, char *out, size_t out_size) {
    const char *slash = strrchr(path, '/');
    if (!slash) { out[0] = '\0'; return; }
    size_t len = (size_t)(slash - path);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static bool already_included(const char *full_path) {
    for (int i = 0; i < included_count; i++) {
        if (strcmp(included_paths[i], full_path) == 0) return true;
    }
    return false;
}

static char *preprocess_includes(const char *source, const char *base_dir);

// Busca "Include" (insensible a mayusculas) seguido de una cadena
// entre comillas, en una linea que NO sea un comentario (no empieza
// por ';' tras quitar espacios). Devuelve el nombre de archivo (sin
// comillas, recien reservado con malloc) o NULL si la linea no es un
// Include valido.
static char *match_include_line(const char *line) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == ';' || *p == '\0') return NULL; // comentario o linea vacia
    if ((p[0] != 'I' && p[0] != 'i') || (p[1] != 'n' && p[1] != 'N')) return NULL;
    if (strncasecmp(p, "Include", 7) != 0) return NULL;
    p += 7;
    if (*p != ' ' && *p != '\t') return NULL; // "Include" debe ser una palabra completa
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    const char *start = p;
    while (*p != '"' && *p != '\0') p++;
    if (*p != '"') return NULL;
    size_t len = (size_t)(p - start);
    char *name = (char *)malloc(len + 1);
    memcpy(name, start, len);
    name[len] = '\0';
    return name;
}

static char *preprocess_includes(const char *source, const char *base_dir) {
    size_t cap = strlen(source) * 2 + 4096;
    char *result = (char *)malloc(cap);
    result[0] = '\0';
    size_t result_len = 0;

    const char *line_start = source;
    while (*line_start) {
        const char *line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        char *line = (char *)malloc(line_len + 1);
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';

        char *inc_name = match_include_line(line);
        if (inc_name) {
            char full_path[1024];
            if (base_dir[0]) snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, inc_name);
            else snprintf(full_path, sizeof(full_path), "%s", inc_name);

            if (!already_included(full_path)) {
                if (included_count < MAX_INCLUDES) {
                    strncpy(included_paths[included_count], full_path, sizeof(included_paths[0]) - 1);
                    included_count++;
                }
                char *inc_source = read_file(full_path);
                char inc_dir[1024];
                dirname_of(full_path, inc_dir, sizeof(inc_dir));
                char *expanded = preprocess_includes(inc_source, inc_dir);
                free(inc_source);

                size_t exp_len = strlen(expanded);
                while (result_len + exp_len + 2 >= cap) { cap *= 2; result = (char *)realloc(result, cap); }
                memcpy(result + result_len, expanded, exp_len);
                result_len += exp_len;
                result[result_len++] = '\n';
                result[result_len] = '\0';
                free(expanded);
            }
            // si ya estaba incluido, no anadimos nada (linea descartada)
            free(inc_name);
        } else {
            while (result_len + line_len + 2 >= cap) { cap *= 2; result = (char *)realloc(result, cap); }
            memcpy(result + result_len, line, line_len);
            result_len += line_len;
            result[result_len++] = '\n';
            result[result_len] = '\0';
        }
        free(line);

        if (!line_end) break;
        line_start = line_end + 1;
    }
    return result;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s entrada.bb salida.s\n", argv[0]);
        return 1;
    }

    char *raw_source = read_file(argv[1]);
    char base_dir[1024];
    dirname_of(argv[1], base_dir, sizeof(base_dir));
    included_count = 0;
    char *source = preprocess_includes(raw_source, base_dir);
    free(raw_source);

    Node *program = parse_program(source);

    FILE *out = fopen(argv[2], "w");
    if (!out) { perror("no se pudo crear el archivo de salida"); return 1; }
    codegen_generate(program, out);
    fclose(out);

    fprintf(stderr, "Compilado: %s -> %s\n", argv[1], argv[2]);
    return 0;
}
