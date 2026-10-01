// nbc_main.c — nbc.pro: el compilador de Nemo Basic corriendo DENTRO
// de Nemo OS, como un programa .pro más.
//
// Es la misma cadena de piezas que usa nbc_driver.c en el Mac
// (nb_lexer -> nb_parser -> nb_codegen -> cabecera NEXE); lo único
// que cambia es de dónde sale el texto y a dónde va el binario:
// aquí, syscalls de Nemo OS en vez de fopen/fwrite.
//
// Uso desde la shell:   run nbc.pro miprograma.nb
//
// El argumento admite dos formas, igual que el nbc.pro anterior:
//   "archivo.nb"          -> se busca en la raíz de NemoFS
//   "inodo:archivo.nb"    -> se busca en esa carpeta (el explorador y
//                            el IDE usan esta forma para compilar un
//                            archivo allí donde vive, sin copiarlo)
//
// A partir de aquí, compilar un .nb dentro de Nemo OS no depende de
// ningún compilador externo: ni del Mac, ni de gcc, ni de nada. El
// sistema compila sus propios programas.

#include <stdint.h>
#include <stdbool.h>

#include "nb_ast.h"
#include "nb_lexer.h"
#include "nb_codebuf.h"
#include "nb_encode.h"
#include "nb_symtab.h"
#include "nb_syscalls.h"
#include "nb_include.h"

// El mismo contexto que usan nbc_driver.c y todas las pruebas de
// host. nb_codegen.c no expone una cabecera propia (todo vive en el
// .c), así que quien lo use repite este typedef -- y hay que
// mantenerlo al día si el contexto crece.
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
extern void nb_node_free_tree(nb_node_t *n);
extern void nb_free(void *p);

// Tamaño máximo de un .nb de entrada. Sin libc no hay memoria
// dinámica del sistema: este buffer es estático, y el montón privado
// (nb_alloc) es cosa aparte.
#define MAX_SRC_SIZE (256 * 1024)
static char src_buf[MAX_SRC_SIZE];

// Buffer de salida: la cabecera NEXE (16 bytes) más el código. Tiene
// que dar para el bloque de runtime entero (~4 KB de código más la
// región de datos), no para su .bss -- eso no se escribe en el
// archivo, ver la nota en nb_codegen.c.
// Cabecera NEXE version 3: 24 bytes (magic, version, entry_offset,
// code_size, mem_size, flags). Ver la descripcion en src/loader.c.
#define NEXE_HEADER_SIZE 24u
#define NEXE_FLAG_CONSOLE 1u
#define MAX_PRO_SIZE (512 * 1024)
static uint8_t pro_buf[MAX_PRO_SIZE];

// -------- utilidades mínimas (sin libc) --------

static uint32_t nb_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static void nb_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

// Número a texto, para los mensajes de progreso.
static void nb_itoa(int64_t v, char *out, uint32_t max) {
    if (max == 0) return;
    if (v == 0) { if (max > 1) { out[0] = '0'; out[1] = '\0'; } else out[0] = '\0'; return; }
    bool neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
    char tmp[24];
    uint32_t n = 0;
    while (u > 0 && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    uint32_t i = 0;
    if (neg && i + 1 < max) out[i++] = '-';
    while (n > 0 && i + 1 < max) out[i++] = tmp[--n];
    out[i] = '\0';
}

// Cambia la extensión del nombre: "juego.nb" -> "juego.pro".
//
// Corta por el ÚLTIMO punto, no por el primero, y si no hay punto
// simplemente añade ".pro" al final. (El nbc.pro anterior tuvo aquí
// un bug real: añadía la extensión al nombre entero, así que
// "menu.nbst" acababa siendo "menu.nbst.pro" en vez de "menu.pro".)
static void make_output_name(const char *input, char *output, uint32_t max_len) {
    uint32_t len = nb_strlen(input);
    uint32_t corte = len;
    for (uint32_t i = 0; i < len; i++) if (input[i] == '.') corte = i;

    uint32_t i = 0;
    while (i < corte && i + 1 < max_len) { output[i] = input[i]; i++; }
    const char *suffix = ".pro";
    uint32_t j = 0;
    while (suffix[j] != '\0' && i + 1 < max_len) output[i++] = suffix[j++];
    output[i] = '\0';
}

// ---- Buscar un archivo SIN crearlo ----
// SYS_FILE_OPEN crea el archivo si no existe: compilar un nombre mal escrito
// creaba un archivo vacio y compilaba un programa vacio, sin ningun error. Se
// busca en la LISTA de la carpeta. (Mayusculas y minusculas dan igual.)
static uint8_t lista_buf[256 * 40];
static char nbm_minus(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
// Cada entrada del listado son 40 bytes: inodo(4) | tipo(4) | tamaño(4) | nombre(28)
static bool nb_buscar_en(uint32_t carpeta, const char *nombre, uint32_t *inodo, uint32_t *tam) {
    int64_t n = nb_file_list(carpeta, lista_buf, 256, VOLUME_NEMOFS);
    for (int64_t i = 0; i < n && i < 256; i++) {
        const uint8_t *e = lista_buf + i * 40;
        const char *nm = (const char *)(e + 12);
        uint32_t k = 0;
        while (nm[k] && nombre[k] && nbm_minus(nm[k]) == nbm_minus(nombre[k])) k++;
        if (nm[k] == 0 && nombre[k] == 0) {
            *inodo = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
            *tam   = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
            return true;
        }
    }
    return false;
}

// ---- Rutas con carpetas ----
// Include "JUEGOS/utiles.nb": la ruta la camina EL KERNEL, con las syscalls
// 83/82/81, que aceptan rutas y -- lo importante -- no crean nada si no
// existe. Aqui hubo un caminante propio durante un tiempo, componente a
// componente con SYS_FILE_LIST, porque ninguna syscall resolvia una ruta sin
// crear el archivo. Eran dos copias de la misma regla, y dos copias acaban
// separandose: si el kernel y el compilador entienden "JUEGOS/NEMO" de forma
// distinta, el mismo programa compila aqui y falla alli. Ahora hay una sola.
static bool nb_es_separador(char c) { return c == '/' || c == '\\'; }
static bool nb_ruta_tiene_carpeta(const char *ruta) {
    for (const char *p = ruta; *p; p++) if (nb_es_separador(*p)) return true;
    return false;
}

// Parte "JUEGOS/NEMO/juego.nb" en "JUEGOS/NEMO" y "juego.nb". Es un corte de
// texto por el ultimo separador, no un recorrido: no toca el disco.
static bool nb_partir_ruta(const char *ruta, char *carpeta, uint32_t carpeta_max,
                           char *hoja, uint32_t hoja_max) {
    uint32_t largo = 0;
    while (ruta[largo]) largo++;
    if (largo == 0 || nb_es_separador(ruta[largo - 1])) return false;  // acaba en separador
    uint32_t corte = largo;
    while (corte > 0 && !nb_es_separador(ruta[corte - 1])) corte--;    // corte = tras el ultimo separador
    uint32_t n_hoja = largo - corte;
    if (n_hoja == 0 || n_hoja + 1 > hoja_max) return false;
    for (uint32_t i = 0; i < n_hoja; i++) hoja[i] = ruta[corte + i];
    hoja[n_hoja] = '\0';
    uint32_t n_car = corte > 0 ? corte - 1 : 0;                        // sin el separador final
    if (n_car + 1 > carpeta_max) return false;
    for (uint32_t i = 0; i < n_car; i++) carpeta[i] = ruta[i];
    carpeta[n_car] = '\0';
    return true;
}

// El inodo y el tamaño de un archivo dado por su ruta. Falla si no existe o
// si lo que hay ahi es una carpeta.
static bool nb_buscar_ruta(const char *ruta, uint32_t *inodo, uint32_t *tam) {
    if (nb_file_type(ruta) != 1) return false;        // 0 = no existe, 2 = carpeta
    int64_t ino = nb_find_child(ruta, NEMOFS_ROOT_INODE);
    if (ino < 0) return false;
    int64_t bytes = nb_file_size(ruta);
    if (bytes < 0) return false;
    *inodo = (uint32_t)ino;
    *tam = (uint32_t)bytes;
    return true;
}

// La CARPETA que contiene a la hoja de una ruta. Hace falta para dejar el
// .pro al lado del .nb y para que los Include con nombre a secas se busquen
// en la carpeta del programa.
static bool nb_ruta_padre(const char *ruta, uint32_t *padre, char *hoja, uint32_t hoja_max) {
    char carpeta[256];
    if (!nb_partir_ruta(ruta, carpeta, sizeof(carpeta), hoja, hoja_max)) return false;
    if (carpeta[0] == '\0') { *padre = NEMOFS_ROOT_INODE; return true; }  // "/x.nb"
    if (nb_file_type(carpeta) != 2) return false;     // no existe, o es un archivo
    int64_t ino = nb_find_child(carpeta, NEMOFS_ROOT_INODE);
    if (ino < 0) return false;
    *padre = (uint32_t)ino;
    return true;
}
// Los archivos de Include: en la carpeta del programa, en la raiz, en
// DOCUMENTOS y en SISTEMA, por ese orden
extern void *nb_alloc(uint32_t n);
static char *leer_incluido(const char *nombre, uint32_t *largo, void *ctx) {
    uint32_t carpeta = *(uint32_t *)ctx, ino, tam, dir;
    bool hay;
    if (nb_ruta_tiene_carpeta(nombre)) {
        // La ruta manda: si no esta ahi, no esta. Buscar ademas en las
        // cuatro carpetas de siempre haria que "JUEGOS/utiles.nb" pudiera
        // acabar cargando otro archivo distinto sin decirlo.
        hay = nb_buscar_ruta(nombre, &ino, &tam);
    } else {
        hay = nb_buscar_en(carpeta, nombre, &ino, &tam) || nb_buscar_en(NEMOFS_ROOT_INODE, nombre, &ino, &tam);
        if (!hay && nb_buscar_en(NEMOFS_ROOT_INODE, "DOCUMENTOS", &dir, &tam)) hay = nb_buscar_en(dir, nombre, &ino, &tam);
        if (!hay && nb_buscar_en(NEMOFS_ROOT_INODE, "SISTEMA", &dir, &tam)) hay = nb_buscar_en(dir, nombre, &ino, &tam);
    }
    if (!hay) return 0;
    char *buf = (char *)nb_alloc(tam + 1);
    if (!buf) return 0;
    int64_t got = nb_file_read((int64_t)ino, buf, tam + 1, VOLUME_NEMOFS);
    if (got < 0) got = 0;
    buf[got] = 0;
    *largo = (uint32_t)got;
    return buf;
}
// Un error, en su archivo y su linea. En el programa principal, el formato de
// siempre ("error en la linea N: ..."), que es el que leen el IDE y Aronnax.
static nb_inc_t incluidos;
extern void nb_codegen_set_includes(const nb_inc_t *mapa);
static void escribir_error(int32_t linea, const char *msg) {
    const char *archivo; uint32_t lo; bool principal;
    nb_inc_origen(&incluidos, (uint32_t)(linea > 0 ? linea : 0), &archivo, &lo, &principal);
    if (principal) nb_write_string("nbc: error en la linea ");
    else { nb_write_string("nbc: error en "); nb_write_string(archivo); nb_write_string(", linea "); }
    { char n[16]; nb_itoa((int64_t)lo, n, sizeof(n)); nb_write_string(n); }
    nb_write_string(": ");
    nb_write_string(msg);
    nb_write_string("\n");
}

static void morir(const char *msg) {
    nb_write_string(msg);
    nb_exit();
    for (;;) { nb_pump(); }
}

__attribute__((section(".text.start")))
void _start(void) {
    nb_write_string("nbc: Nemo Basic, compilando dentro de Nemo OS\n");
    nb_pump();

    char arg[64];
    uint32_t arg_len = nb_get_launch_arg(arg, sizeof(arg));
    if (arg_len == 0) {
        morir("nbc: falta el nombre del archivo .nb\n"
              "     uso: run nbc.pro miprograma.nb\n");
    }

    // "inodo:archivo.nb" -> compilar el archivo de esa carpeta.
    uint32_t carpeta = NEMOFS_ROOT_INODE;
    const char *nombre = arg;
    {
        uint32_t i = 0, val = 0;
        while (arg[i] >= '0' && arg[i] <= '9') { val = val * 10 + (uint32_t)(arg[i] - '0'); i++; }
        if (i > 0 && arg[i] == ':') { carpeta = val; nombre = &arg[i + 1]; }
    }

    // El programa principal tambien puede venir con carpetas:
    // "run nbc.pro JUEGOS/juego.nb". Se resuelve a (carpeta, nombre) aqui,
    // asi que todo lo de abajo sigue igual: el .pro sale al lado del .nb, y
    // los Include con nombre a secas se buscan en la carpeta del programa.
    static char hoja_principal[32];
    if (nb_ruta_tiene_carpeta(nombre)) {
        uint32_t padre;
        if (!nb_ruta_padre(nombre, &padre, hoja_principal, sizeof(hoja_principal)))
            morir("nbc: esa ruta no existe\n");
        carpeta = padre;
        nombre = hoja_principal;
    }

    nb_write_string("nbc: leyendo ");
    nb_write_string(nombre);
    nb_write_string("\n");
    nb_pump();

    uint32_t ino_fuente, tam_fuente;
    if (!nb_buscar_en(carpeta, nombre, &ino_fuente, &tam_fuente)) morir("nbc: no se encontro ese archivo\n");
    if (tam_fuente >= MAX_SRC_SIZE) morir("nbc: el archivo es demasiado grande (mas de 256 KB)\n");

    int64_t src_len = nb_file_read((int64_t)ino_fuente, src_buf, MAX_SRC_SIZE - 1, VOLUME_NEMOFS);
    if (src_len < 0) src_len = 0;
    src_buf[src_len] = '\0';

    nb_write_string("nbc: ");
    { char n[16]; nb_itoa(src_len, n, sizeof(n)); nb_write_string(n); }
    nb_write_string(" bytes leidos, compilando...\n");

    // Vistazo a lo que de verdad hay en el buffer. No es adorno: si el
    // archivo se lee "bien" (el tamaño cuadra) pero el contenido no
    // llega, el analizador ve un archivo vacio, no da error -- un
    // archivo vacio es valido -- y produce un .pro de 16 bytes con
    // solo el prologo y el epilogo. Sin esta linea, ese caso parece
    // que todo fue bien.
    nb_write_string("nbc: empieza por [");
    {
        char preview[41];
        int32_t n = (src_len < 40) ? (int32_t)src_len : 40;
        for (int32_t k = 0; k < n; k++) {
            char c = src_buf[k];
            preview[k] = (c == '\n') ? '|' : (c == '\r' ? '^' : c);
        }
        preview[n] = '\0';
        nb_write_string(preview);
    }
    nb_write_string("]\n");
    nb_pump();

    // -------- lexer + parser --------
    bool ok = false;
    const char *err_msg = "";
    int32_t err_line = 0;
    // Include: los archivos incluidos, dentro, antes de analizar
    if (!nb_inc_expandir(&incluidos, nombre, src_buf, (uint32_t)src_len, leer_incluido, &carpeta)) {
        if (incluidos.error_archivo == 0) nb_write_string("nbc: error en la linea ");
        else { nb_write_string("nbc: error en "); nb_write_string(incluidos.nombres[incluidos.error_archivo]); nb_write_string(", linea "); }
        { char n[16]; nb_itoa((int64_t)incluidos.error_linea, n, sizeof(n)); nb_write_string(n); }
        nb_write_string(": "); nb_write_string(incluidos.error); nb_write_string("\n");
        morir("");
    }
    if (incluidos.n_nombres > 1) {
        nb_write_string("nbc: con Include: ");
        { char n[16]; nb_itoa((int64_t)(incluidos.n_nombres - 1), n, sizeof(n)); nb_write_string(n); }
        nb_write_string(" archivo(s) mas\n");
    }
    nb_node_t *prog = nb_parse_program(incluidos.texto, &ok, &err_msg, &err_line);
    if (!ok) {
        escribir_error(err_line, err_msg);
        morir("");
    }

    // -------- generacion de codigo --------
    nb_codebuf_t cb;
    nb_symtab_t globals;
    nb_codebuf_init(&cb);
    nb_symtab_init(&globals);

    // El contexto lo prepara el generador, y se comprueba que cabe: antes
    // habia aqui una COPIA de su estructura que se quedo corta, y el
    // generador escribia mas alla del final, pisando esta misma pila.
    static uint8_t ctx_mem[512] __attribute__((aligned(16)));
    if (nb_codegen_ctx_bytes > sizeof ctx_mem) morir("nbc: el contexto del generador no cabe\n");
    nb_codegen_ctx_init(ctx_mem, &cb, &globals);

    nb_codegen_set_includes(&incluidos);   // los errores en tiempo de ejecucion dicen tambien el archivo
    nb_emit_program(ctx_mem, prog);

    // Con todo el codigo ya generado se conoce su tamaño final, que es
    // lo que hacia falta para resolver las direcciones de las
    // variables globales y para rellenar la region de datos.
    // El generador tambien puede fallar, no solo el analizador (un
    // Goto a una etiqueta que no existe, una llamada a una funcion
    // inexistente...). Antes esos casos no generaban nada y el .pro
    // salia "bien" pero incompleto.
    {
        const char *gerr; int32_t gline;
        if (nb_codegen_get_error(&gerr, &gline)) {
            escribir_error(gline, gerr);
            morir("");
        }
    }

    // Avisos: no impiden compilar, pero conviene verlos.
    {
        int32_t na = nb_codegen_num_avisos();
        for (int32_t i = 0; i < na; i++) {
            const char *var, *fn; int32_t ln;
            if (!nb_codegen_aviso(i, &var, &fn, &ln)) break;
            const char *archivo; uint32_t lo; bool principal;
            nb_inc_origen(&incluidos, (uint32_t)(ln > 0 ? ln : 0), &archivo, &lo, &principal);
            nb_write_string("nbc: aviso en ");
            if (!principal) { nb_write_string(archivo); nb_write_string(", "); }
            nb_write_string("la linea ");
            { char b[16]; nb_itoa((int64_t)lo, b, sizeof(b)); nb_write_string(b); }
            nb_write_string(": la funcion "); nb_write_string(fn);
            nb_write_string(" comparte '"); nb_write_string(var);
            nb_write_string("' con el resto del programa sin declararla (Global "); nb_write_string(var);
            nb_write_string(")\n");
        }
        if (nb_codegen_avisos_desbordados()) nb_write_string("nbc: ...y mas avisos, no caben todos\n");
    }

    nb_symtab_resolve_globals(&globals, &cb, cb.count);
    uint32_t data_len = 0;
    uint8_t *data_image = nb_symtab_build_data_image(&globals, cb.count, &data_len);
    if (data_image && data_len > 0 && globals.data_region_pos != 0) {
        uint8_t *dst = (uint8_t *)cb.words + globals.data_region_pos;
        for (uint32_t i = 0; i < data_len; i++) dst[i] = data_image[i];
    }

    uint32_t code_bytes = cb.count * 4;
    if (NEXE_HEADER_SIZE + code_bytes > MAX_PRO_SIZE) morir("nbc: el programa compilado no cabe en el buffer de salida\n");

    // -------- cabecera NEXE + escritura --------
    pro_buf[0] = 'N'; pro_buf[1] = 'E'; pro_buf[2] = 'X'; pro_buf[3] = 'E';
    // Version 3: la cabecera lleva la memoria TOTAL que necesita el
    // programa (incluido el .bss del runtime, que no viaja en el
    // archivo) y las banderas, entre ellas si es de consola. Con eso
    // el cargador puede decir claramente que un programa no cabe.
    uint32_t version = 3, entry_offset = 0, code_size = code_bytes;
    uint32_t mem_size = code_bytes + nb_codegen_get_bss_size();
    uint32_t flags = nb_tree_is_console_app(prog) ? NEXE_FLAG_CONSOLE : 0u;
    nb_memcpy(pro_buf + 4, &version, 4);
    nb_memcpy(pro_buf + 8, &entry_offset, 4);
    nb_memcpy(pro_buf + 12, &code_size, 4);
    nb_memcpy(pro_buf + 16, &mem_size, 4);
    nb_memcpy(pro_buf + 20, &flags, 4);
    nb_memcpy(pro_buf + NEXE_HEADER_SIZE, cb.words, code_bytes);

    char out_name[64];
    make_output_name(nombre, out_name, sizeof(out_name));

    // El .pro se escribe en la MISMA carpeta donde estaba el .nb, no
    // siempre en la raiz -- asi compilar algo de DOCUMENTOS deja el
    // resultado al lado del fuente.
    int64_t out_handle = nb_file_open(out_name, carpeta, VOLUME_NEMOFS);
    if (out_handle < 0) morir("nbc: no se pudo crear el archivo de salida\n");

    int64_t escrito = nb_file_write(out_handle, pro_buf, NEXE_HEADER_SIZE + code_size, VOLUME_NEMOFS);
    if (escrito < 0) morir("nbc: fallo al escribir el .pro\n");

    nb_write_string("nbc: listo -> ");
    nb_write_string(out_name);
    nb_write_string(" (");
    { char n[16]; nb_itoa((int64_t)(NEXE_HEADER_SIZE + code_size), n, sizeof(n)); nb_write_string(n); }
    nb_write_string(" bytes)\n");

    if (data_image) nb_free(data_image);
    nb_node_free_tree(prog);
    nb_symtab_free(&globals);
    nb_codebuf_free(&cb);

    nb_exit();
    for (;;) { nb_pump(); }
}
