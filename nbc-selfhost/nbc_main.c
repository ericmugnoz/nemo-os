// nbc_main.c — nbc.pro: el compilador Nemo-Blitz + ensamblador
// Nemo-AS, corriendo DENTRO de Nemo OS como un programa .pro normal.
//
// Uso desde la shell: run nbc.pro   (con el argumento de lanzamiento
// puesto al nombre del archivo .bb a compilar -- por ahora, mientras
// no tengamos una forma comoda de pasar argumentos a 'run' desde la
// shell, se lanza indirectamente; ver notas mas abajo).
//
// Flujo completo, SIN salir nunca de Nemo OS:
//   1. Lee el .bb desde NemoFS
//   2. Lo compila a ensamblador (en un buffer de memoria)
//   3. Ensambla ese buffer a codigo maquina (en otro buffer)
//   4. Envuelve el resultado con la cabecera NEXE
//   5. Escribe el .pro resultante de vuelta en NemoFS
//
// Necesita el hueco de memoria grande (6MB) que le dimos a cada
// tarea -- el arbol de sintaxis, el texto del ensamblador generado y
// el binario final no caben en el limite normal de 64KB.

#include "nb_syscalls.h"
#include "nblibc.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "nb_output.h"
#include "assembler.h"

#define MAX_SRC_SIZE  (64 * 1024)
#define MAX_ASM_SIZE  (256 * 1024)
// NemoFS admite como mucho 268288 bytes por archivo (ver la nota
// junto a nemofs_inode_t en nemofs.c) -- MAX_BIN_SIZE debe coincidir
// CON ESE LIMITE (no ser mayor), o un programa compilado que quepa
// en este buffer pero supere la capacidad real de NemoFS fallaria al
// guardarse EN SILENCIO (la falta de espacio se detecta aqui, en el
// ensamblado, que ya maneja bien el error -- "nbc: fallo al
// ensamblar" -- en vez de en la escritura del archivo, que antes no
// se comprobaba). CONFIRMADO CON COMPILACION REAL: nbc.pro ocupa
// 207854 bytes -- el limite anterior (202752) se quedaba corto de
// verdad, no solo en teoria.
#define MAX_BIN_SIZE  268288

// Todo esto vive en .bss (se limpia solo al cargar la tarea) -- no
// cabria ni de broma en la pila.
static char src_buf[MAX_SRC_SIZE];
static char asm_buf[MAX_ASM_SIZE];
static uint8_t bin_buf[MAX_BIN_SIZE];
static uint8_t pro_buf[MAX_BIN_SIZE + 16]; // +16 = cabecera NEXE

// -- preprocesador de Include --
//
// Igual proposito que en el compilador del host (ver main.c): Include
// "corta y pega" el contenido de otro archivo .bb en ese punto, ANTES
// de lexer/parser, sin incluir el mismo archivo dos veces. Aqui,
// SIN malloc/free disponibles (entorno freestanding dentro de Nemo
// OS), usamos buffers estaticos de tamano fijo en vez de asignacion
// dinamica -- dos buffers en "ping-pong" (src_buf <-> include_buf2),
// expandiendo TODOS los Include que aparezcan en una pasada, y
// repitiendo hasta que una pasada no inserte nada nuevo (para que los
// includes anidados -- un archivo incluido que a su vez incluye
// otro -- tambien se expandan correctamente).
#define MAX_INCLUDE_NAMES 16
#define MAX_INCLUDE_DEPTH 8
static char include_buf2[MAX_SRC_SIZE];
static char include_file_tmp[MAX_SRC_SIZE]; // buffer de lectura reutilizado para cada archivo incluido
static char included_names[MAX_INCLUDE_NAMES][64];
static int included_count = 0;

static bool name_already_included(const char *name) {
    for (int i = 0; i < included_count; i++) {
        if (nb_strcmp(included_names[i], name) == 0) return true;
    }
    return false;
}

// Procesa TODO 'src' (longitud 'src_len'), copiando cada linea a
// 'dst' tal cual salvo las que sean "Include "archivo""; esas se
// sustituyen por el contenido de ese archivo (leido de NemoFS, solo
// desde la raiz -- misma limitacion que el .bb principal), o por nada
// si ese archivo ya se habia incluido antes. Escribe como mucho
// dst_max-1 bytes mas el terminador. Devuelve true si insercio
// contenido NUEVO de algun archivo (senal de que hace falta otra
// pasada, por si ese contenido tenia Include anidados).
static bool expand_includes_once(const char *src, uint32_t src_len, char *dst, uint32_t dst_max) {
    uint32_t si = 0, di = 0;
    bool inserted_new = false;
    while (si < src_len && di < dst_max - 1) {
        uint32_t line_start = si;
        while (si < src_len && src[si] != '\n') si++;
        uint32_t line_end = si;
        bool had_newline = (si < src_len);

        uint32_t p = line_start;
        while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;

        bool is_include = false;
        uint32_t name_start = 0, name_len = 0;
        if (p < line_end && src[p] != ';' && (line_end - p) >= 7) {
            const char *kw = "Include";
            bool matches = true;
            for (int k = 0; k < 7; k++) {
                if (nb_tolower(src[p + (uint32_t)k]) != nb_tolower(kw[k])) { matches = false; break; }
            }
            if (matches) {
                uint32_t q = p + 7;
                if (q < line_end && (src[q] == ' ' || src[q] == '\t')) {
                    while (q < line_end && (src[q] == ' ' || src[q] == '\t')) q++;
                    if (q < line_end && src[q] == '"') {
                        q++;
                        uint32_t nstart = q;
                        while (q < line_end && src[q] != '"') q++;
                        if (q < line_end && src[q] == '"') {
                            is_include = true;
                            name_start = nstart;
                            name_len = q - nstart;
                        }
                    }
                }
            }
        }

        if (is_include) {
            char fname[64];
            uint32_t copy_len = (name_len < 63) ? name_len : 63;
            nb_memcpy(fname, &src[name_start], copy_len);
            fname[copy_len] = '\0';

            if (!name_already_included(fname)) {
                if (included_count < MAX_INCLUDE_NAMES) {
                    nb_strncpy(included_names[included_count], fname, 63);
                    included_count++;
                }
                int64_t handle = nb_file_open(fname, NEMOFS_ROOT_INODE, VOLUME_NEMOFS);
                if (handle >= 0) {
                    int64_t inc_len = nb_file_read(handle, include_file_tmp, MAX_SRC_SIZE - 1, VOLUME_NEMOFS);
                    if (inc_len < 0) inc_len = 0;
                    for (int64_t k = 0; k < inc_len && di < dst_max - 1; k++) {
                        dst[di++] = include_file_tmp[(uint32_t)k];
                    }
                    if (di < dst_max - 1) dst[di++] = '\n';
                    inserted_new = true;
                }
                // si no se pudo abrir, no se inserta nada (fallo silencioso)
            }
            // linea Include consumida -- no copiamos su texto original
        } else {
            for (uint32_t k = line_start; k < line_end && di < dst_max - 1; k++) {
                dst[di++] = src[k];
            }
            if (di < dst_max - 1) dst[di++] = '\n';
        }

        if (had_newline) si++;
    }
    dst[di] = '\0';
    return inserted_new;
}

// Cambia la extension de un nombre de archivo (".bb" -> ".pro"). Si
// no termina en ".bb", simplemente le añade ".pro" al final.
static void make_output_name(const char *input, char *output, uint32_t max_len) {
    uint32_t len = nb_strlen(input);
    uint32_t base_len = len;
    if (len > 3 && input[len-3] == '.' && nb_tolower(input[len-2]) == 'b' && nb_tolower(input[len-1]) == 'b') {
        base_len = len - 3;
    }
    nb_strncpy(output, input, (base_len + 1 < max_len) ? base_len + 1 : max_len);
    output[base_len] = '\0';
    const char *suffix = ".pro";
    uint32_t i = 0;
    while (suffix[i] != '\0' && base_len + i + 1 < max_len) {
        output[base_len + i] = suffix[i];
        i++;
    }
    output[base_len + i] = '\0';
}

__attribute__((section(".text.start")))
void _start(void) {
    nb_write_string("nbc: arrancando...\n");
    nb_pump();

    char arg[64];
    uint32_t arg_len = nb_get_launch_arg(arg, sizeof(arg));

    if (arg_len == 0) {
        nb_write_string("nbc: falta el nombre del archivo .bb (argumento de lanzamiento)\n");
        nb_exit();
        for (;;) { nb_pump(); }
    }

    nb_write_string("nbc: compilando ");
    nb_write_string(arg);
    nb_write_string("...\n");

    // Por ahora buscamos solo en la raiz -- igual que hicimos con
    // 'run' en la shell, se puede ampliar despues para buscar en la
    // carpeta actual primero si hace falta.
    int64_t handle = nb_file_open(arg, NEMOFS_ROOT_INODE, VOLUME_NEMOFS);
    if (handle < 0) {
        nb_write_string("nbc: no se encontro el archivo '");
        nb_write_string(arg);
        nb_write_string("' en la raiz de NemoFS\n");
        nb_exit();
        for (;;) { nb_pump(); }
    }

    int64_t src_len = nb_file_read(handle, src_buf, MAX_SRC_SIZE - 1, VOLUME_NEMOFS);
    if (src_len < 0) src_len = 0;
    src_buf[src_len] = '\0';

    // -- expandimos Include, si los hay, antes de seguir --
    // ping-pong entre src_buf e include_buf2, hasta que una pasada no
    // inserte contenido nuevo (o hasta el limite de profundidad, por
    // seguridad ante includes circulares).
    included_count = 0;
    for (int pass = 0; pass < MAX_INCLUDE_DEPTH; pass++) {
        bool changed = expand_includes_once(src_buf, (uint32_t)src_len, include_buf2, MAX_SRC_SIZE);
        uint32_t new_len = nb_strlen(include_buf2);
        nb_memcpy(src_buf, include_buf2, new_len + 1);
        src_len = (int64_t)new_len;
        if (!changed) break;
    }

    nb_write_string("nbc: archivo leido (");
    { char n[12]; nb_itoa(src_len, n, sizeof(n)); nb_write_string(n); }
    nb_write_string(" bytes), parseando...\n");
    nb_write_string("nbc: primeros caracteres: [");
    {
        char preview[41];
        int32_t plen = (src_len < 40) ? (int32_t)src_len : 40;
        for (int32_t k = 0; k < plen; k++) {
            char c = src_buf[k];
            preview[k] = (c == '\n') ? '|' : (c == '\r' ? '^' : c);
        }
        preview[plen] = '\0';
        nb_write_string(preview);
    }
    nb_write_string("]\n");
    nb_pump(); // dejamos que la shell muestre esto antes de seguir

    // -- Compilador: .bb -> texto de ensamblador --
    nb_alloc_reset();

    // Depuracion: volcamos los primeros tokens tal cual los ve el
    // lexer, antes de que el parser haga nada con ellos.
    {
        Lexer dbg_lx;
        lexer_init(&dbg_lx, src_buf);
        nb_write_string("nbc: tokens: ");
        for (int t = 0; t < 8; t++) {
            Token tok = lexer_next(&dbg_lx);
            char n[6];
            nb_itoa((int64_t)tok.type, n, sizeof(n));
            nb_write_string("[");
            nb_write_string(n);
            if (tok.type == 2 || tok.type == 3 || tok.type == 4) { // NUMBER/STRING/IDENT
                nb_write_string(":");
                nb_write_string(tok.text);
            }
            nb_write_string("]");
            if (tok.type == 0) break; // T_EOF
        }
        nb_write_string("\n");
        nb_pump();
    }

    Node *program = parse_program(src_buf); // termina el programa solo si hay un error (nb_fatal)

    nb_write_string("nbc: parseado con exito, generando codigo...\n");
    nb_pump();

    nb_output_begin(asm_buf, MAX_ASM_SIZE);
    codegen_generate(program, &nb_stdout_placeholder);

    nb_write_string("nbc: generados ");
    {
        char len_str[12];
        nb_itoa((int64_t)nb_output_length(), len_str, sizeof(len_str));
        nb_write_string(len_str);
    }
    nb_write_string(" bytes de ensamblador, ensamblando...\n");
    nb_pump();

    // -- Ensamblador: ese mismo texto -> codigo maquina --
    int64_t bin_size = assemble(asm_buf, bin_buf, MAX_BIN_SIZE);
    if (bin_size < 0) {
        nb_write_string("nbc: fallo al ensamblar\n");
        nb_exit();
        for (;;) { nb_pump(); }
    }

    nb_write_string("nbc: ensamblado con exito, escribiendo el .pro...\n");
    nb_pump();

    // -- Cabecera NEXE: "NEXE" + version(u32) + entry_offset(u32) + code_size(u32) --
    pro_buf[0] = 'N'; pro_buf[1] = 'E'; pro_buf[2] = 'X'; pro_buf[3] = 'E';
    uint32_t version = 1, entry_offset = 0, code_size = (uint32_t)bin_size;
    nb_memcpy(pro_buf + 4, &version, 4);
    nb_memcpy(pro_buf + 8, &entry_offset, 4);
    nb_memcpy(pro_buf + 12, &code_size, 4);
    nb_memcpy(pro_buf + 16, bin_buf, code_size);

    char out_name[64];
    make_output_name(arg, out_name, sizeof(out_name));

    int64_t out_handle = nb_file_open(out_name, NEMOFS_ROOT_INODE, VOLUME_NEMOFS);
    if (out_handle < 0) {
        nb_write_string("nbc: no se pudo crear el archivo de salida\n");
        nb_exit();
        for (;;) { nb_pump(); }
    }
    int64_t write_result = nb_file_write(out_handle, pro_buf, 16 + code_size, VOLUME_NEMOFS);
    if (write_result < 0) {
        nb_write_string("nbc: fallo al escribir el .pro (¿demasiado grande?)\n");
        nb_exit();
        for (;;) { nb_pump(); }
    }

    nb_write_string("nbc: listo -> ");
    nb_write_string(out_name);
    nb_write_string(" (");
    {
        char size_str[12];
        nb_itoa((int64_t)code_size, size_str, sizeof(size_str));
        nb_write_string(size_str);
    }
    nb_write_string(" bytes de codigo)\n");

    nb_exit();
    for (;;) { nb_pump(); }
}
