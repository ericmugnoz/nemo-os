// nbc_driver.c -- version NATIVA (Mac/host, gcc normal) del ensamblador
// final de nbc-selfhost: une lexer -> parser -> generador de codigo ->
// bloque de runtime en un .pro real, con cabecera NEXE.
//
// Es un paso intermedio, deliberado: el codigo que genera el
// compilador ya es ARM64 de verdad sea cual sea el compilador que
// compile AL COMPILADOR -- asi que esta version, aunque corra nativa
// en el Mac, ya produce un .pro utilizable de verdad en QEMU. La
// version AUTOHOSPEDADA (corriendo dentro de Nemo OS, leyendo/
// escribiendo con syscalls en vez de fopen/fwrite) es una pieza
// aparte, para mas adelante -- mismo pipeline, solo cambia la entrada/
// salida de archivos.
//
// Uso: nbc_driver entrada.nb salida.pro

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "nb_ast.h"
#include "nb_lexer.h"
#include "nb_codebuf.h"
#include "nb_encode.h"
#include "nb_symtab.h"

extern void nb_free(void *p);

// El mismo nb_codegen_ctx_t que usan todas las pruebas de host de
// este proyecto -- nb_codegen.c no expone una cabecera propia (todo
// vive en el .c, como el resto de piezas de esta reescritura), asi
// que cualquier programa que use nb_emit_program repite este mismo
// typedef.
typedef struct nb_func_table nb_func_table_t;
typedef struct nb_runtime_table nb_runtime_table_t;
typedef struct nb_array_set nb_array_set_t;
typedef struct nb_label_table nb_label_table_t;
typedef struct nb_restore_table nb_restore_table_t;
typedef struct nb_field_table nb_field_table_t;
typedef struct nb_type_table nb_type_table_t;
// El contexto del generador ya NO se declara aqui: antes era una
// copia de la estructura de nb_codegen.c, y las copias se quedaron cortas --
// el generador escribia mas alla del final, pisando la pila de este programa.
// Ahora se pide su tamaño al generador y se comprueba que cabe.
extern const uint32_t nb_codegen_ctx_bytes;
extern void nb_codegen_ctx_init(void *ctx, nb_codebuf_t *cb, nb_symtab_t *globals);

extern bool nb_tree_is_console_app(nb_node_t *program);
extern bool nb_codegen_get_error(const char **out_msg, int32_t *out_line);
// Avisos: variables compartidas entre una funcion y el resto
// del programa sin declararlas. No impiden compilar.
extern int32_t nb_codegen_num_avisos(void);
extern bool nb_codegen_avisos_desbordados(void);
extern bool nb_codegen_aviso(int32_t i, const char **variable, const char **funcion, int32_t *line);
extern uint32_t nb_codegen_get_bss_size(void);
extern void nb_emit_program(void *ctx, nb_node_t *program);
extern nb_node_t *nb_parse_program(const char *source, bool *ok, const char **err_msg, int32_t *err_line);

// Cabecera NEXE de Nemo OS, VERSION 3 -- 24 bytes, empaquetada sin
// relleno. Ver la descripcion completa en src/loader.c.
//
// Frente a la version 1/2 (16 bytes) añade dos campos:
//   mem_size -- memoria TOTAL que necesita el programa al ejecutarse,
//               incluido el .bss del runtime, que no viaja en el
//               archivo. Con esto el cargador puede decir claramente
//               que un programa no cabe, en vez de un Data Abort.
//   flags    -- bit 0: aplicacion de consola. Antes eso se codificaba
//               en la propia version (1 escritorio, 2 consola).
typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint32_t version;
    uint32_t entry_offset;
    uint32_t code_size;
    uint32_t mem_size;
    uint32_t flags;
} nexe_header_t;
#define NEXE_FLAG_CONSOLE 1u

static char *read_whole_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = (long)got;
    return buf;
}

// ---- Include: los archivos, junto al programa principal ----
#include "nb_include.h"
extern void *nb_alloc(uint32_t n);
static char carpeta_principal[1024] = ".";
// Un Include puede traer carpetas: Include "JUEGOS/utiles.nb". Dentro de
// Nemo OS valen '/' y '\', una barra inicial solo significa "desde la raiz"
// -- que para el compilador es la carpeta del programa principal -- y ".."
// no se admite. Aqui se normaliza igual, para que el mismo fuente compile
// byte a byte lo mismo en el Mac y dentro del sistema.
static bool normalizar_incluido(const char *nombre, char *salida, size_t max) {
    while (*nombre == '/' || *nombre == '\\') nombre++;      // barra inicial
    size_t n = 0;
    bool principio = true;                                    // ¿empieza componente?
    for (const char *p = nombre; *p; p++) {
        char c = (*p == '\\') ? '/' : *p;
        if (principio && c == '.') {
            // ".." o "." como componente entero: fuera
            const char *q = p;
            size_t puntos = 0;
            while (*q == '.') { puntos++; q++; }
            if (puntos <= 2 && (*q == '/' || *q == '\0')) return false;
        }
        if (n + 1 >= max) return false;
        salida[n++] = c;
        principio = (c == '/');
    }
    salida[n] = '\0';
    // Acabar en separador nombra una CARPETA, no un archivo. Sin esto,
    // Include "JUEGOS/" abria el directorio, leia cero bytes y compilaba
    // como si el archivo estuviera vacio, sin decir nada -- justo el fallo
    // silencioso que dentro del sistema ya se rechaza.
    if (n == 0 || salida[n - 1] == '/') return false;
    return true;
}

static char *leer_incluido(const char *nombre, uint32_t *largo, void *ctx) {
    (void)ctx;
    char limpio[1024];
    if (!normalizar_incluido(nombre, limpio, sizeof limpio)) return NULL;
    char ruta[1200];
    // snprintf corta sin protestar; una ruta cortada abriria otro archivo, o
    // ninguno, sin decir por que. Mejor rechazarla y que salga el error de
    // "no se encuentra", que sí se ve.
    int n_ruta = snprintf(ruta, sizeof ruta, "%s/%s", carpeta_principal, limpio);
    if (n_ruta < 0 || (size_t)n_ruta >= sizeof ruta) return NULL;
    // Una CARPETA no es un archivo. fopen("JUEGOS/NEMO", "rb") funciona en
    // Linux y luego lee cero bytes, asi que sin esta comprobacion el Include
    // de una carpeta compilaba como un archivo vacio, callado -- y dentro de
    // Nemo OS el mismo fuente daba error. El compilador tiene que decidir lo
    // mismo en las dos maquinas.
    { struct stat st; if (stat(ruta, &st) != 0 || !S_ISREG(st.st_mode)) return NULL; }
    FILE *f = fopen(ruta, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *b = (char *)nb_alloc((uint32_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f); fclose(f);
    b[got] = 0; *largo = (uint32_t)got;
    return b;
}
static nb_inc_t incluidos;
extern void nb_codegen_set_includes(const nb_inc_t *mapa);
static void error_en(int32_t linea, const char *msg) {
    const char *archivo; uint32_t lo; bool principal;
    nb_inc_origen(&incluidos, (uint32_t)(linea > 0 ? linea : 0), &archivo, &lo, &principal);
    if (principal) fprintf(stderr, "nbc: error en la linea %u: %s\n", lo, msg);
    else fprintf(stderr, "nbc: error en %s, linea %u: %s\n", archivo, lo, msg);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "uso: %s entrada.nb salida.pro\n", argv[0]);
        return 1;
    }

    long src_len;
    char *source = read_whole_file(argv[1], &src_len);
    if (!source) {
        fprintf(stderr, "nbc: no se pudo leer '%s'\n", argv[1]);
        return 1;
    }

    // Include: la carpeta del programa principal, y los archivos dentro
    {
        const char *barra = strrchr(argv[1], '/');
        if (barra) { size_t n = (size_t)(barra - argv[1]); if (n >= sizeof carpeta_principal) n = sizeof carpeta_principal - 1; memcpy(carpeta_principal, argv[1], n); carpeta_principal[n] = 0; }
    }
    const char *solo_nombre = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
    if (!nb_inc_expandir(&incluidos, solo_nombre, source, (uint32_t)src_len, leer_incluido, NULL)) {
        if (incluidos.error_archivo == 0) fprintf(stderr, "nbc: error en la linea %u: %s\n", incluidos.error_linea, incluidos.error);
        else fprintf(stderr, "nbc: error en %s, linea %u: %s\n", incluidos.nombres[incluidos.error_archivo], incluidos.error_linea, incluidos.error);
        free(source);
        return 1;
    }

    bool ok;
    const char *err_msg;
    int32_t err_line;
    nb_node_t *prog = nb_parse_program(incluidos.texto, &ok, &err_msg, &err_line);
    if (!ok) {
        error_en(err_line, err_msg);
        free(source);
        return 1;
    }

    nb_codebuf_t cb;
    nb_symtab_t globals;
    nb_codebuf_init(&cb);
    nb_symtab_init(&globals);

    static uint8_t ctx_mem[512] __attribute__((aligned(16)));
    if (nb_codegen_ctx_bytes > sizeof ctx_mem) {
        fprintf(stderr, "nbc: el contexto del generador no cabe (%u bytes)\n", nb_codegen_ctx_bytes);
        return 1;
    }
    nb_codegen_ctx_init(ctx_mem, &cb, &globals);
    nb_codegen_set_includes(&incluidos);
    nb_emit_program(ctx_mem, prog);

    // Ahora que TODO el codigo esta generado (incluido el bloque de
    // runtime si hiciera falta), el tamaño final del codigo ya se
    // conoce -- momento en el que se resuelven las direcciones
    // diferidas de las variables globales, y se construye la imagen
    // final de la region de datos (literales + huecos de globales a
    // cero + los parcheos dato-sobre-dato de Data).
    // El generador tambien puede fallar, no solo el analizador: un
    // Goto a una etiqueta que no existe, una llamada a una funcion
    // inexistente... Antes esos casos no generaban nada y el .pro
    // salia "bien" pero incompleto.
    {
        const char *gerr; int32_t gline;
        if (nb_codegen_get_error(&gerr, &gline)) {
            error_en(gline, gerr);
            return 1;
        }
    }

    // Avisos: no impiden compilar, pero conviene verlos.
    {
        int32_t na = nb_codegen_num_avisos();
        for (int32_t i = 0; i < na; i++) {
            const char *var, *fn; int32_t ln;
            if (!nb_codegen_aviso(i, &var, &fn, &ln)) break;
            fprintf(stderr, "nbc: aviso en la linea %d: la funcion %s comparte '%s' con el resto "
                            "del programa sin declararla (Global %s)\n", ln, fn, var, var);
        }
        if (nb_codegen_avisos_desbordados())
            fprintf(stderr, "nbc: ...y mas avisos, no caben todos\n");
    }

    nb_symtab_resolve_globals(&globals, &cb, cb.count);
    uint32_t data_len = 0;
    uint8_t *data_image = nb_symtab_build_data_image(&globals, cb.count, &data_len);

    // La region de datos ya tiene su hueco RESERVADO dentro del
    // propio codigo (ver nb_emit_program): se copia ahi, en vez de
    // añadirse al final. Asi el .pro termina donde termina el bloque
    // de runtime, sin arrastrar detras los megabytes de ceros de su
    // .bss -- que no hacen falta en el archivo, porque el area de
    // cada tarea ya viene a cero.
    if (data_image && data_len > 0 && globals.data_region_pos != 0) {
        uint8_t *dst = (uint8_t *)cb.words + globals.data_region_pos;
        for (uint32_t i = 0; i < data_len; i++) dst[i] = data_image[i];
    }
    uint32_t code_bytes = cb.count * 4;
    uint32_t total_size = code_bytes;

    nexe_header_t hdr;
    hdr.magic[0] = 'N'; hdr.magic[1] = 'E'; hdr.magic[2] = 'X'; hdr.magic[3] = 'E';
    hdr.version = 3;
    hdr.entry_offset = 0; // el programa principal siempre empieza en la primera instruccion generada
    hdr.code_size = total_size;
    // Memoria total: lo que ocupa el archivo mas el .bss del runtime
    // (el monton de nb_alloc), que no se escribe pero se usara igual.
    hdr.mem_size = total_size + nb_codegen_get_bss_size();
    // El programador declara "Console" o "Desktop" con una linea
    // suelta. Sin declaracion es de escritorio.
    hdr.flags = nb_tree_is_console_app(prog) ? NEXE_FLAG_CONSOLE : 0u;

    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "nbc: no se pudo escribir '%s'\n", argv[2]);
        return 1;
    }
    fwrite(&hdr, sizeof(hdr), 1, out);
    fwrite(cb.words, 4, cb.count, out);
    fclose(out);

    fprintf(stderr, "nbc: '%s' -> '%s' (%u bytes en total, con %u de datos ya dentro)\n",
            argv[1], argv[2], total_size, data_len);

    if (data_image) nb_free(data_image);
    nb_node_free_tree(prog);
    nb_symtab_free(&globals);
    nb_codebuf_free(&cb);
    free(source);
    return 0;
}
