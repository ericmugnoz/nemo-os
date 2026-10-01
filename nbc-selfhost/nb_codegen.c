// nb_codegen.c — generador de código de Nemo-Blitz 2.0.
//
// PRIMERA PIEZA, deliberadamente pequeña: aritmética entera sobre
// LITERALES (N_NUM, N_BINOP, N_UNOP) -- todavía sin variables (eso
// necesita antes una tabla de símbolos: dónde vive cada una, global o
// local, que es la siguiente pieza natural).
//
// Modelo de evaluación: el mismo que el compilador viejo, que nunca
// fue el problema -- cada subexpresión deja su resultado en x0; para
// un binario, se calcula el lado izquierdo, se empuja a la pila real
// del procesador, se calcula el lado derecho (que queda en x0), se
// recupera el izquierdo en x1, y entonces izquierda=x1, derecha=x0.
//
// Verificación: sin QEMU ni un emulador ARM64 en este entorno, cada
// prueba compara la secuencia EXACTA de palabras emitidas contra lo
// que nb_encode.c calcularía a mano para la misma operación -- no es
// "se ejecutó y dio el resultado esperado" (eso hace falta probarlo
// de verdad en QEMU cuando haya un programa completo), pero sí
// confirma que el generador llama al codificador correcto, con los
// registros y el orden de operandos correctos, en cada caso.

#include "nb_ast.h"
#include "nb_codebuf.h"
#include "nb_encode.h"
#include "nb_lexer.h" // TOK_PLUS, TOK_MINUS, etc. -- los mismos tipos de token que ya trae cada N_BINOP/N_UNOP
#include "nb_symtab.h"
#include "nb_runtime_blob.h"
#include "nb_include.h"   // la tabla de lineas de los errores dice tambien el archivo
#include <stddef.h>

extern bool nb_string_eq(nb_string_t *a, nb_string_t *b);
extern void *nb_alloc(uint32_t n);
extern void nb_free(void *p);

// Contexto de generacion: el buffer de codigo, la tabla de globales
// (una por programa), y el ambito local ACTUAL -- NULL si estamos en
// el nivel superior del programa (fuera de toda Function), donde
// cualquier variable que aparezca es global por definicion, igual que
// en BlitzPlus real.
typedef struct {
    nb_codebuf_t *cb;
    nb_symtab_t *globals;
    nb_local_scope_t *locals;
    struct nb_func_table *functions;
    struct nb_runtime_table *runtime;
    struct nb_array_set *arrays;
    struct nb_label_table *labels;
    struct nb_restore_table *restore;
    nb_string_t *data_cursor; // nombre de la variable global oculta que lleva la posicion de lectura de Data
    uint32_t data_start_offset; // desplazamiento del PRIMER valor de Data de todo el programa -- a donde vuelve un "Restore" sin argumento
    struct nb_field_table *fields;
    struct nb_type_table *types;
    nb_string_t *color_var; // nombre de la variable global oculta con el color activo (Color/Plot/Rect/Line/Oval/Text)
    nb_string_t *cls_color_var; // idem para el color de borrado (ClsColor/Cls)
    nb_label_t *return_label; // NULL en el nivel superior del programa; dentro de una funcion, a donde salta Return
    // Etiqueta de salida del bucle MAS INTERNO, para que Exit sepa a
    // donde saltar. Se guarda y se restaura al entrar y salir de cada
    // bucle, asi que en bucles anidados Exit sale del de dentro --
    // igual que en BlitzPlus.
    //
    // Exit se parseaba desde el principio pero NO se generaba: un
    // programa con Exit compilaba sin una sola queja y el Exit
    // simplemente no hacia nada. Peor que un error de compilacion.
    nb_label_t *exit_label;
    // Etiqueta de la rutina de "indice fuera de rango". Es UNA sola
    // para todo el programa: cada acceso a array que se sale salta
    // aqui, en vez de llevar su propio mensaje y su propia salida.
    // Asi el coste por acceso son 3 instrucciones, no 10.
    nb_label_t *bounds_error_label;
} nb_codegen_ctx_t;

// El contexto lo crea el GENERADOR, no quien le llama. Antes cada
// compilador (nbc.pro y el del Mac) declaraba su propia copia de esta
// estructura, y las copias se quedaron cortas: el generador escribia campos
// que en la copia del que llamaba no existian, pisando su pila. Nadie se dio
// cuenta hasta que un campo mas hizo que el desbordamiento cayera en algo
// importante. Ahora se pide el tamaño aqui y se comprueba.
const uint32_t nb_codegen_ctx_bytes = (uint32_t)sizeof(nb_codegen_ctx_t);
void nb_codegen_ctx_init(void *p, nb_codebuf_t *cb, nb_symtab_t *globals) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < sizeof(nb_codegen_ctx_t); i++) b[i] = 0;
    nb_codegen_ctx_t *c = (nb_codegen_ctx_t *)p;
    c->cb = cb;
    c->globals = globals;
}

// ---- Errores del GENERADOR ----
//
// Hasta ahora solo el parser podia dar errores. El generador, cuando
// encontraba algo que no sabia emitir (un Goto a una etiqueta que no
// existe, un ForEach sobre un Type no declarado), simplemente no
// generaba nada y seguia: el programa compilaba limpio y no hacia lo
// que decia.
//
// El estado va en variables de archivo y no en el contexto a
// proposito: el contexto se replica a mano en cada prueba de host y
// en los dos drivers, y añadirle campos obliga a tocar cuarenta
// archivos. Aqui solo hay una compilacion en marcha a la vez, asi que
// no hay nada que compartir.
static bool g_codegen_error = false;
static const char *g_codegen_error_msg = NULL;
static int32_t g_codegen_error_line = 0;

static void nb_codegen_error(const char *msg, int32_t line) {
    if (g_codegen_error) return;   // se queda el PRIMERO, que suele ser la causa
    g_codegen_error = true;
    g_codegen_error_msg = msg;
    g_codegen_error_line = line;
}

// Para los drivers: ¿fallo la generacion? Devuelve el mensaje y la
// linea por los punteros de salida.
// Memoria EXTRA que el programa generado necesita al ejecutarse, por
// encima de lo que ocupa en el archivo: el .bss del bloque de runtime
// (el monton de nb_alloc), que no se escribe en el .pro pero que el
// programa usara igual. 0 si el programa no incrusta el runtime.
//
// Lo necesita el cargador para saber si el programa CABE de verdad en
// el area de la tarea: antes solo podia comprobar el codigo, y un
// programa que se pasara solo daba un Data Abort generico.
static uint32_t g_codegen_bss_size = 0;

uint32_t nb_codegen_get_bss_size(void) {
    return g_codegen_bss_size;
}

bool nb_codegen_get_error(const char **out_msg, int32_t *out_line) {
    if (!g_codegen_error) return false;
    if (out_msg) *out_msg = g_codegen_error_msg;
    if (out_line) *out_line = g_codegen_error_line;
    return true;
}

// Registro 9 reservado como temporal para direcciones de variables
// GLOBALES (adrp+add) -- nunca lo toca la evaluacion de una expresion
// (que solo usa x0/x1/x2), asi que es seguro calcular una direccion
// en x9 y luego evaluar una expresion sin que se pise.
#define NB_ADDR_REG 9

// Busca 'name' primero en el ambito local (si existe), luego en las
// globales -- si no aparece en ningun sitio, se declara como GLOBAL
// nueva alli mismo (variable usada sin declarar = global implicita,
// igual que en BlitzPlus real). 'is_local' indica donde se encontro.
static void nb_resolve_var(nb_codegen_ctx_t *ctx, nb_string_t *name, bool *is_local, int32_t *offset) {
    if (ctx->locals) {
        nb_local_entry_t *le = nb_local_scope_find(ctx->locals, name);
        if (le) { *is_local = true; *offset = le->offset; return; }
    }
    nb_global_entry_t *ge = nb_symtab_find_global(ctx->globals, name);
    if (!ge) {
        extern void nb_string_retain(nb_string_t *s);
        nb_string_retain(name); // la tabla de globales se queda con su propia referencia
        ge = nb_symtab_add_global(ctx->globals, name);
    }
    *is_local = false; *offset = (int32_t)ge->offset;
}

// Push/pop de un valor de 64 bits sobre la pila REAL del procesador
// (str/ldr con pre/post-indexado, igual que ya hacía el compilador
// viejo) -- no hay ningún "buffer de temporales" aparte que gestionar,
// es literalmente la pila de la CPU.
static void nb_push_x0(nb_codebuf_t *cb) {
    // str x0, [sp, #-16]! -- STR con pre-indexado. imm9 SIN escalar
    // (bytes crudos, confirmado en el propio nb_encode.c) -- se pasa
    // -16 directamente, NO -16/8.
    nb_codebuf_emit(cb, nb_enc_str_pre(0, NB_REG_SP, -16));
}

static void nb_pop_x0_marca(nb_codebuf_t *cb) {
    nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));
}
static void nb_pop_x1(nb_codebuf_t *cb) {
    // ldr x1, [sp], #16 -- LDR con post-indexado, imm9 sin escalar.
    nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));
}

// Las dos variantes que faltaban, para cuando lo que hay que apilar o
// recuperar no esta en el registro de siempre.
static void nb_push_x1(nb_codebuf_t *cb) {
    nb_codebuf_emit(cb, nb_enc_str_pre(1, NB_REG_SP, -16));
}

static void nb_pop_x0(nb_codebuf_t *cb) {
    nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));
}

// Carga un entero de 64 bits en x0 con movz/movk -- hasta 4
// instrucciones (una por cada bloque de 16 bits), omitiendo los
// bloques que ya quedarían a cero tras el primer movz (igual que hace
// cualquier ensamblador razonable: no emitir "movk x0,#0,lsl#32" si
// ese bloque ya es cero).
static void nb_emit_load_imm64(nb_codebuf_t *cb, int32_t rd, uint64_t value) {
    uint32_t parts[4];
    for (int i = 0; i < 4; i++) parts[i] = (uint32_t)((value >> (i * 16)) & 0xFFFF);

    if (value == 0) {
        nb_codebuf_emit(cb, nb_enc_movz(rd, 0, 0));
        return;
    }
    bool first = true;
    for (int i = 0; i < 4; i++) {
        if (parts[i] == 0 && !first) continue; // los bloques intermedios en cero se omiten (via movk que no se emite)
        if (first) {
            nb_codebuf_emit(cb, nb_enc_movz(rd, parts[i], i));
            first = false;
        } else if (parts[i] != 0) {
            nb_codebuf_emit(cb, nb_enc_movk(rd, parts[i], i));
        }
    }
}

// Emite el código de una expresión ENTERA -- el resultado queda en
// x0 al terminar. 'ctx' resuelve las variables (local o global).
// Carga el valor de una variable (local o global) en x0.
static bool nb_text_eq_cstr(nb_string_t *s, const char *lit); // adelantada

// Pi (o Pi#): una CONSTANTE, no una variable. Antes "x# = Pi"
// compilaba como una variable sin asignar y valia 0, en silencio.
static bool nb_es_pi(nb_string_t *name) {
    return name && (nb_text_eq_cstr(name, "Pi") || nb_text_eq_cstr(name, "Pi#") || nb_text_eq_cstr(name, "PI"));
}

// Las constantes de eventos: los mismos nombres y valores que en
// el kernel y en nemo_gui. Antes, escribir EVENT_MENUACTION compilaba como una
// variable vacia y valia 0, EN SILENCIO: la comparacion no se cumplia nunca.
// Sin tabla de punteros en datos (nbc.pro): una cadena de comparaciones.
static bool nb_constante_sistema(nb_string_t *name, int64_t *valor) {
    if (!name) return false;
    if (nb_text_eq_cstr(name, "EVENT_GADGETACTION")) { *valor = 0x401; return true; }
    if (nb_text_eq_cstr(name, "EVENT_WINDOWSIZE"))   { *valor = 0x802; return true; }
    if (nb_text_eq_cstr(name, "EVENT_WINDOWCLOSE"))  { *valor = 0x803; return true; }
    if (nb_text_eq_cstr(name, "EVENT_MENUACTION"))   { *valor = 0x1001; return true; }
    if (nb_text_eq_cstr(name, "EVENT_TIMERTICK"))    { *valor = 0x4001; return true; }
    return false;
}

static void nb_emit_load_var_x0(nb_codegen_ctx_t *ctx, nb_string_t *name) {
    nb_codebuf_t *cb = ctx->cb;
    {
        int64_t v;
        if (nb_constante_sistema(name, &v)) { nb_emit_load_imm64(cb, 0, (uint64_t)v); return; }
    }
    if (nb_es_pi(name)) {
        union { double d; uint64_t u; } pi = { 3.14159265358979323846 };
        nb_emit_load_imm64(cb, 0, pi.u);   // los bits del decimal, como cualquier decimal en x0
        return;
    }
    bool is_local; int32_t offset;
    nb_resolve_var(ctx, name, &is_local, &offset);
    if (is_local) {
        nb_codebuf_emit(cb, nb_enc_ldur(0, 29, offset));
    } else {
        nb_emit_global_addr(ctx->globals, cb, NB_ADDR_REG, (uint32_t)offset);
        nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_ADDR_REG, 0));
    }
}

// Guarda x0 en una variable (local o global). A diferencia de
// nb_emit_assign_int, aqui el VALOR ya esta calculado de antemano en
// x0 -- no evalua ninguna expresion, solo resuelve el destino y
// guarda. adrp+add solo escriben en el registro de direccion (x9),
// nunca en x0, asi que calcular la direccion DESPUES de tener el
// valor listo es igual de seguro que al reves.
static void nb_emit_store_var_from_x0(nb_codegen_ctx_t *ctx, nb_string_t *name) {
    nb_codebuf_t *cb = ctx->cb;
    bool is_local; int32_t offset;
    nb_resolve_var(ctx, name, &is_local, &offset);
    if (is_local) {
        nb_codebuf_emit(cb, nb_enc_stur(0, 29, offset));
    } else {
        nb_emit_global_addr(ctx->globals, cb, NB_ADDR_REG, (uint32_t)offset);
        nb_codebuf_emit(cb, nb_enc_str_imm(0, NB_ADDR_REG, 0));
    }
}

// Calcula la direccion de UN elemento de un array (a(i) o a(i,j)) --
// deja la direccion final en x0. La comparten la lectura y la
// escritura, para no duplicar esta logica. Solo 1D/2D -- ver la nota
// extensa mas abajo, junto a Dim.
extern void nb_emit_expr_int(nb_codegen_ctx_t *ctx, nb_node_t *n); // adelantada, definicion completa mas abajo
// Comprueba que 'reg_indice' este dentro de [0, tamano), y si no salta
// a la rutina de error. x0 tiene el puntero al array; 'off_tamano' es
// el desplazamiento (en unidades de 8) donde vive el tamaño de esa
// dimension dentro de la cabecera.
//
// Una sola instruccion de comparacion basta para las DOS mitades del
// rango: comparando SIN SIGNO, un indice negativo se ve como un numero
// enorme, asi que "indice >= tamaño" tambien atrapa los negativos.
// ---- Errores en tiempo de ejecucion, con su linea ----
//
// Cada comprobacion que falla LLAMA (bl) a su rutina de error. La rutina sabe
// desde donde se la llamo (x30), y busca esa posicion en una TABLA DE LINEAS
// que va en los datos del programa: para cada sentencia, en que instruccion
// empieza y que linea es (con su archivo, si viene de un Include). Asi dice
// "Error en tiempo de ejecucion, linea 23: indice de array fuera de rango
// (indice 11, el array tiene 10 elementos)". Mientras nada falla, cada
// comprobacion cuesta una instruccion.
//
// Errores: indice fuera de rango, division entre cero (en ARM64 no falla: da
// 0 y el programa seguia con un resultado falso), objeto Null (falta New) y
// array sin Dim. La rutina es autosuficiente -- no usa el runtime, que no
// siempre esta dentro del programa.
//
// La tabla son desplazamientos (sin punteros): nbc.pro carga sin reubicar.
static uint32_t *g_lin_pos, *g_lin_num;
static uint32_t g_lin_n, g_lin_cap;
static const nb_inc_t *g_inc_mapa;             // de nbc_main / nbc_driver (NULL: sin Include)
static nb_label_t *g_err_div0, *g_err_null, *g_err_dim;
// Ayudantes de las funciones de cadenas, emitidos una sola vez:
//   copia_n: copia x1 bytes de x0 a x9 (que avanza)
//   digitos: escribe x0 en base x1 en x9 (que avanza), sin ceros delante
static nb_label_t *g_h_copia, *g_h_digitos;
static bool g_usa_copia, g_usa_digitos;
// Cadenas temporales: lo que crea una expresion ("a" + b$, Mid$(...),
// Str$(n)...) es texto nuevo que nadie libera. En un programa corto no se nota;
// en un bucle de juego, la memoria se agota en un minuto y a partir de ahi
// Mid$ devuelve vacio y el programa hace cosas absurdas. Ahora cada temporal se
// apunta en una lista y al terminar CADA SENTENCIA se libera lo apuntado en
// ella (con una marca, para que una funcion llamada en medio no libere lo de
// quien la llamo).
static nb_label_t *g_t_apuntar, *g_t_liberar, *g_t_marca;
static bool g_usa_temporales;
static uint32_t g_temp_area;      // en los datos: [cuantos][64 punteros]
#define NB_TEMP_MAX 64
static bool g_usa_indice, g_usa_div0, g_usa_null, g_usa_dim;
void nb_codegen_set_includes(const nb_inc_t *mapa) { g_inc_mapa = mapa; }
static void nb_lineas_anotar(uint32_t pos, int32_t linea) {
    if (linea <= 0) return;
    if (g_lin_n > 0 && g_lin_pos[g_lin_n - 1] == pos) { g_lin_num[g_lin_n - 1] = (uint32_t)linea; return; }
    if (g_lin_n == g_lin_cap) {
        uint32_t nc = g_lin_cap ? g_lin_cap * 2 : 256;
        uint32_t *np = (uint32_t *)nb_alloc(nc * 4), *nn = (uint32_t *)nb_alloc(nc * 4);
        for (uint32_t i = 0; i < g_lin_n; i++) { np[i] = g_lin_pos[i]; nn[i] = g_lin_num[i]; }
        if (g_lin_pos) nb_free(g_lin_pos);
        if (g_lin_num) nb_free(g_lin_num);
        g_lin_pos = np; g_lin_num = nn; g_lin_cap = nc;
    }
    g_lin_pos[g_lin_n] = pos; g_lin_num[g_lin_n] = (uint32_t)linea; g_lin_n++;
}
// Si 'reg' es cero, a la rutina de error (bl: asi sabe desde donde)
static void nb_emit_si_cero_error(nb_codebuf_t *cb, int reg, nb_label_t *err) {
    nb_label_t *sigue = nb_label_new();
    nb_emit_cbnz(cb, reg, sigue);
    nb_emit_bl(cb, err);
    nb_label_define(cb, sigue);
    nb_label_free(sigue);
}

static void nb_emit_bounds_check(nb_codegen_ctx_t *ctx, int reg_indice, uint32_t off_tamano) {
    nb_codebuf_t *cb = ctx->cb;
    if (!ctx->bounds_error_label) return;   // sin rutina de error no se comprueba
    nb_codebuf_emit(cb, nb_enc_ldr_imm(4, 0, off_tamano));           // x4 = tamaño
    nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, reg_indice, 4));  // cmp indice, tamaño
    // Comparando SIN SIGNO, un indice negativo se ve enorme: "indice < tamaño"
    // (CC) atrapa las dos mitades del rango. Si no, a la rutina de error con
    // el indice (x1) y el tamaño (x2), para que el mensaje los diga.
    nb_label_t *sigue = nb_label_new();
    nb_emit_bcond(cb, NB_COND_CC, sigue);
    nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, reg_indice));   // x1 = indice
    nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 4));            // x2 = tamaño
    nb_emit_bl(cb, ctx->bounds_error_label);
    nb_label_define(cb, sigue);
    nb_label_free(sigue);
    g_usa_indice = true;
}

static void nb_emit_array_addr(nb_codegen_ctx_t *ctx, nb_node_t *n) {
    nb_codebuf_t *cb = ctx->cb;
    if (n->list_count == 1) {
        nb_emit_expr_int(ctx, n->list[0]);  // x0 = i
        nb_push_x0(cb);
        nb_emit_load_var_x0(ctx, n->text);   // x0 = puntero al array
        if (ctx->bounds_error_label) { nb_emit_si_cero_error(cb, 0, g_err_dim); g_usa_dim = true; }   // sin Dim todavia
        nb_pop_x1(cb);                        // x1 = i
        nb_emit_bounds_check(ctx, 1, 1);      // dim0_size vive en [puntero+8]
        nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 16)); // + cabecera 1D (dim_count+dim0_size)
        nb_codebuf_emit(cb, nb_enc_add_reg(0, 0, 1, 3)); // + i*8 (lsl #3)
    } else if (n->list_count == 2) {
        nb_emit_expr_int(ctx, n->list[0]);  // x0 = i
        nb_push_x0(cb);
        nb_emit_expr_int(ctx, n->list[1]);  // x0 = j
        nb_push_x0(cb);
        nb_emit_load_var_x0(ctx, n->text);   // x0 = puntero
        if (ctx->bounds_error_label) { nb_emit_si_cero_error(cb, 0, g_err_dim); g_usa_dim = true; }   // sin Dim todavia
        nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16)); // x2 = j (tope de la pila)
        nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16)); // x1 = i
        nb_emit_bounds_check(ctx, 1, 1);                         // i contra dim0_size
        nb_emit_bounds_check(ctx, 2, 2);                         // j contra dim1_size
        nb_codebuf_emit(cb, nb_enc_ldr_imm(3, 0, 2));           // x3 = dim1_size ([puntero+16])
        nb_codebuf_emit(cb, nb_enc_mul(1, 1, 3));                // x1 = i*dim1_size
        nb_codebuf_emit(cb, nb_enc_add_reg(1, 1, 2, 0));         // x1 = i*dim1_size + j
        nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 24));           // + cabecera 2D
        nb_codebuf_emit(cb, nb_enc_add_reg(0, 0, 1, 3));         // + indice_plano*8
    }
    // 3+ dimensiones: fuera de alcance a proposito, no se genera nada (documentado)
}

// ---- Tabla de funciones: nombre -> etiqueta ----
//
// Se rellena ANTES de generar ni una sola instruccion del programa
// (un primer barrido que solo mira los N_FUNCDEF de nivel superior),
// para que cualquier llamada pueda resolver su destino sin importar
// si la funcion llamada aparece antes o despues en el codigo fuente
// -- exactamente el mismo mecanismo de "etiqueta pendiente" de
// siempre (nb_emit_bl ya sabe emitir hacia una etiqueta sin definir).

typedef struct {
    nb_string_t *name; // NO es dueña -- el nodo N_FUNCDEF sigue siendo su dueño
    nb_label_t *label;
    int32_t param_count;
    nb_node_t *def;    // su N_FUNCDEF (NO es dueña): nombres -- y por tanto tipos -- de los parametros
} nb_func_entry_t;

typedef struct nb_func_table {
    nb_func_entry_t *entries;
    uint32_t count, cap;
} nb_func_table_t;

// Tabla de funciones del BLOQUE DE RUNTIME (nb_alloc/nb_string_new/...)
// -- mismo diseño que nb_func_table_t, pero mas simple: no hace falta
// contar parametros (el que la usa ya sabe cuantos tiene cada una), y
// se rellena de una vez a partir de NB_RUNTIME_SYMS, no incrementalmente.
typedef struct {
    // Indice dentro de NB_RUNTIME_SYMS, NO un puntero a su nombre.
    // Guardar aqui un puntero copiado de la tabla funcionaba en el
    // Mac pero no dentro de Nemo OS: alli el programa se carga en una
    // direccion distinta de la que supuso el enlazador, asi que
    // cualquier direccion absoluta guardada en datos estaticos queda
    // apuntando a la nada. Un indice es solo un numero: vale igual en
    // los dos sitios.
    uint32_t sym_index;
    nb_label_t *label;
} nb_runtime_entry_t;

typedef struct nb_runtime_table {
    nb_runtime_entry_t entries[NB_RUNTIME_SYMS_COUNT];
} nb_runtime_table_t;

extern uint32_t nb_string_len(nb_string_t *s);
extern const char *nb_string_cstr(nb_string_t *s);
extern nb_string_t *nb_string_new(const char *data, uint32_t len);
extern void nb_string_release(nb_string_t *s);

static nb_label_t *nb_runtime_find(nb_runtime_table_t *rt, const char *name) {
    for (uint32_t i = 0; i < NB_RUNTIME_SYMS_COUNT; i++) {
        const char *a = NB_RUNTIME_SYMS[rt->entries[i].sym_index].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return rt->entries[i].label; // los dos llegaron a '\0' iguales
    }
    return NULL;
}

// Si una funcion del runtime NO esta en el bloque, antes la llamada
// simplemente no se emitia y el programa seguia con un valor equivocado, en
// silencio (le pasaba a Chr$: el numero quedaba como si fuera una cadena, y el
// programa moria al usarlo). Ahora es un error de compilacion, con su nombre.
static nb_label_t *nb_rt_obligatoria(nb_codegen_ctx_t *ctx, const char *nombre, int32_t linea) {
    nb_label_t *f = ctx->runtime ? nb_runtime_find(ctx->runtime, nombre) : NULL;
    if (!f) {
        static char msg[96];
        uint32_t k = 0;
        const char *pre = "falta una funcion del bloque de runtime: ";
        for (uint32_t i2 = 0; pre[i2] && k < sizeof msg - 1; i2++) msg[k++] = pre[i2];
        for (uint32_t i2 = 0; nombre[i2] && k < sizeof msg - 1; i2++) msg[k++] = nombre[i2];
        msg[k] = 0;
        nb_codegen_error(msg, linea);
    }
    return f;
}

// Compara el CONTENIDO de una nb_string_t (el nombre de un
// identificador, aqui) contra un literal C -- sin asumir que el
// almacenamiento de nb_string_t termina en NUL (nb_string_len es la
// fuente de verdad de su longitud, no un strlen).
static bool nb_text_eq_cstr(nb_string_t *s, const char *lit) {
    uint32_t len = nb_string_len(s);
    uint32_t litlen = 0;
    while (lit[litlen]) litlen++;
    if (len != litlen) return false;
    const char *data = nb_string_cstr(s);
    for (uint32_t i = 0; i < len; i++) if (data[i] != lit[i]) return false;
    return true;
}

// Conjunto de nombres que son ARRAYS (declarados con Dim) -- hace
// falta porque el parser produce N_CALL para "a(3)" dentro de una
// expresion (a nivel de sintaxis, es indistinguible de una llamada a
// funcion; solo N_INDEX como DESTINO de una asignacion, "a(3) = 5",
// se distingue en el propio parser). Sin este conjunto, "x = a(3)"
// con 'a' declarado por Dim se generaria como si fuera una llamada a
// una funcion inexistente -- silenciosamente, sin generar nada (mismo
// patron que cualquier otra funcion no reconocida). No es dueño de
// los nombres que guarda (los nodos Dim siguen siendolo).
typedef struct nb_array_set {
    nb_string_t **names;
    uint32_t count, cap;
} nb_array_set_t;

static void nb_array_set_init(nb_array_set_t *s) { s->names = NULL; s->count = 0; s->cap = 0; }
static void nb_array_set_free(nb_array_set_t *s) { if (s->names) nb_free(s->names); s->names = NULL; s->count = 0; s->cap = 0; }

static bool nb_array_set_contains(nb_array_set_t *s, nb_string_t *name) {
    for (uint32_t i = 0; i < s->count; i++) if (nb_string_eq(s->names[i], name)) return true;
    return false;
}

static void nb_array_set_add(nb_array_set_t *s, nb_string_t *name) {
    if (nb_array_set_contains(s, name)) return;
    if (s->count >= s->cap) {
        uint32_t new_cap = s->cap == 0 ? 8 : s->cap * 2;
        nb_string_t **ne = (nb_string_t **)nb_alloc(new_cap * (uint32_t)sizeof(nb_string_t *));
        if (!ne) return;
        for (uint32_t i = 0; i < s->count; i++) ne[i] = s->names[i];
        if (s->names) nb_free(s->names);
        s->names = ne; s->cap = new_cap;
    }
    s->names[s->count++] = name;
}

// Tabla de etiquetas (Goto/Gosub) -- una por ambito (nivel superior, o
// cada funcion por separado; una etiqueta declarada dentro de una
// funcion no es visible desde otra, ni desde el nivel superior, y al
// reves). Se rellena con un barrido previo del bloque correspondiente
// ANTES de generar su codigo, igual en espiritu que la tabla de
// funciones -- para que un Goto pueda saltar a una etiqueta que
// aparece MAS ADELANTE en el mismo bloque.
typedef struct { nb_string_t *name; nb_label_t *label; } nb_label_entry_t;
typedef struct nb_label_table {
    nb_label_entry_t *entries;
    uint32_t count, cap;
} nb_label_table_t;

static void nb_label_table_init(nb_label_table_t *t) { t->entries = NULL; t->count = 0; t->cap = 0; }
static void nb_label_table_free(nb_label_table_t *t) {
    for (uint32_t i = 0; i < t->count; i++) nb_label_free(t->entries[i].label);
    if (t->entries) nb_free(t->entries);
    t->entries = NULL; t->count = 0; t->cap = 0;
}
static nb_label_t *nb_label_table_find(nb_label_table_t *t, nb_string_t *name) {
    for (uint32_t i = 0; i < t->count; i++) if (nb_string_eq(t->entries[i].name, name)) return t->entries[i].label;
    return NULL;
}
static void nb_label_table_add(nb_label_table_t *t, nb_string_t *name) {
    if (nb_label_table_find(t, name)) return; // ya existe (una etiqueta repetida en el fuente -- se queda con la primera)
    if (t->count >= t->cap) {
        uint32_t new_cap = t->cap == 0 ? 8 : t->cap * 2;
        nb_label_entry_t *ne = (nb_label_entry_t *)nb_alloc(new_cap * (uint32_t)sizeof(nb_label_entry_t));
        if (!ne) return;
        for (uint32_t i = 0; i < t->count; i++) ne[i] = t->entries[i];
        if (t->entries) nb_free(t->entries);
        t->entries = ne; t->cap = new_cap;
    }
    t->entries[t->count].name = name;
    t->entries[t->count].label = nb_label_new();
    t->count++;
}

// Barrido recursivo: recoge cada N_DATALABEL (".etiqueta") de este
// bloque, entrando en If/While/Repeat/For pero SIN entrar en ningun
// N_FUNCDEF (las etiquetas de una funcion son su propio ambito
// aparte, recogidas cuando se genere esa funcion, no desde aqui).
static void nb_scan_labels(nb_node_t *block, nb_label_table_t *table) {
    for (int32_t i = 0; i < block->list_count; i++) {
        nb_node_t *s = block->list[i];
        switch (s->kind) {
            case N_DATALABEL:
                nb_label_table_add(table, s->text);
                break;
            case N_IF:
                nb_scan_labels(s->b, table);
                for (int32_t j = 0; j < s->list_count; j++) nb_scan_labels(s->list[j]->b, table);
                if (s->c) nb_scan_labels(s->c, table);
                break;
            case N_WHILE: nb_scan_labels(s->b, table); break;
            case N_REPEAT: nb_scan_labels(s->a, table); break;
            case N_FOR: nb_scan_labels(s->d, table); break;
            default: break; // N_FUNCDEF a proposito NO se recorre aqui
        }
    }
}

// ---- Data/Read/Restore ----
//
// La MISMA etiqueta (".algo") sirve tanto de destino de Goto/Gosub
// (una posicion de CODIGO, tabla de arriba) como de destino de
// Restore (una posicion dentro de la SECUENCIA DE VALORES de Data) --
// dos contabilidades distintas que comparten nombre, sin conflicto
// real. Esta tabla guarda la segunda: nombre -> desplazamiento en
// BYTES dentro de la region de datos compartida (nb_symtab), que es
// exactamente el mismo numero que el cursor de lectura usa
// directamente como direccion (nunca hace falta multiplicar por 8 --
// el cursor ES un desplazamiento en bytes, no un contador de "valores
// leidos").
typedef struct { nb_string_t *name; uint32_t offset; } nb_restore_entry_t;
typedef struct nb_restore_table {
    nb_restore_entry_t *entries;
    uint32_t count, cap;
} nb_restore_table_t;

static void nb_restore_table_init(nb_restore_table_t *t) { t->entries = NULL; t->count = 0; t->cap = 0; }
static void nb_restore_table_free(nb_restore_table_t *t) { if (t->entries) nb_free(t->entries); t->entries = NULL; t->count = 0; t->cap = 0; }
static bool nb_restore_table_find(nb_restore_table_t *t, nb_string_t *name, uint32_t *out_offset) {
    for (uint32_t i = 0; i < t->count; i++) {
        if (nb_string_eq(t->entries[i].name, name)) { *out_offset = t->entries[i].offset; return true; }
    }
    return false;
}
static void nb_restore_table_add(nb_restore_table_t *t, nb_string_t *name, uint32_t offset) {
    uint32_t dummy;
    if (nb_restore_table_find(t, name, &dummy)) return; // repetida -- se queda con la primera
    if (t->count >= t->cap) {
        uint32_t new_cap = t->cap == 0 ? 8 : t->cap * 2;
        nb_restore_entry_t *ne = (nb_restore_entry_t *)nb_alloc(new_cap * (uint32_t)sizeof(nb_restore_entry_t));
        if (!ne) return;
        for (uint32_t i = 0; i < t->count; i++) ne[i] = t->entries[i];
        if (t->entries) nb_free(t->entries);
        t->entries = ne; t->cap = new_cap;
    }
    t->entries[t->count].name = name;
    t->entries[t->count].offset = offset;
    t->count++;
}

// Bits crudos de un valor NUMERICO de Data (parse_unary admite un
// signo menos delante de un literal, N_UNOP envolviendo un N_NUM) --
// mismo convenio de "8 bytes tal cual" que cualquier otro entero o
// flotante de este generador: el TIPO se decide al leerlo (segun el
// sufijo de la variable destino), no aqui.
static uint64_t nb_data_number_bits(nb_node_t *val) {
    bool neg = false;
    nb_node_t *n = val;
    if (n->kind == N_UNOP && n->op == TOK_MINUS) { neg = true; n = n->a; }
    double v = neg ? -n->num_value : n->num_value;
    if (n->op) { // llevaba punto decimal -- flotante
        union { double d; uint64_t bits; } u; u.d = v;
        return u.bits;
    }
    return (uint64_t)(int64_t)v;
}

// Barrido recursivo que SI entra en las funciones (a diferencia de
// nb_scan_labels) -- Data es compartido por todo el programa, no por
// ambito. Va añadiendo cada valor a la region de datos compartida
// (nb_symtab) segun aparece, y registra en 'restore' el
// desplazamiento ACTUAL (antes de añadir nada mas) en cada
// N_DATALABEL -- ese es el punto al que "Restore etiqueta" debe
// volver.
// ---- La tabla de Data ----
// Read salta de entrada en entrada (8 bytes un numero, 16 una cadena), asi
// que las entradas tienen que ir SEGUIDAS. Antes, cada cadena escribia sus
// letras y justo detras su entrada: [letras "nuevo"][entrada][letras
// "abrir"][entrada]..., y tanto el principio de los datos como una etiqueta
// de Restore caian EN LAS LETRAS. Con mas de una cadena, Read leia letras
// como si fueran una direccion (en Nemo OS: Data Abort con "rba" -- "abr" de
// "abrir" -- dentro de la direccion). Ahora el barrido escribe solo las
// entradas y apunta las cadenas pendientes; sus letras van todas detras.
typedef struct { nb_node_t **nodos; uint32_t *slots; uint32_t n, cap; } nb_data_cadenas_t;
static void nb_data_cadenas_add(nb_data_cadenas_t *c, nb_node_t *nodo, uint32_t slot) {
    if (c->n == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 32;
        nb_node_t **nn = (nb_node_t **)nb_alloc(nc * (uint32_t)sizeof(nb_node_t *));
        uint32_t *ns = (uint32_t *)nb_alloc(nc * (uint32_t)sizeof(uint32_t));
        for (uint32_t i = 0; i < c->n; i++) { nn[i] = c->nodos[i]; ns[i] = c->slots[i]; }
        if (c->nodos) nb_free(c->nodos);
        if (c->slots) nb_free(c->slots);
        c->nodos = nn; c->slots = ns; c->cap = nc;
    }
    c->nodos[c->n] = nodo; c->slots[c->n] = slot; c->n++;
}

static void nb_scan_data(nb_symtab_t *globals, nb_node_t *block, nb_restore_table_t *restore, nb_data_cadenas_t *cad) {
    for (int32_t i = 0; i < block->list_count; i++) {
        nb_node_t *s = block->list[i];
        switch (s->kind) {
            case N_DATALABEL:
                nb_restore_table_add(restore, s->text, globals->next_offset);
                break;
            case N_DATA:
                for (int32_t j = 0; j < s->list_count; j++) {
                    nb_node_t *val = s->list[j];
                    if (val->kind == N_STR) {
                        // hueco de 16 bytes: [direccion (se parchea
                        // despues, de momento a cero)] [longitud, ya
                        // conocida ahora mismo]. Las letras, al final.
                        uint8_t slot[16] = {0};
                        uint32_t len = nb_string_len(val->text);
                        // (uint64_t): desplazar un valor de 32 bits 32 o mas posiciones es
                        // comportamiento indefinido en C, y en la practica x86 y ARM64 lo
                        // hacen "modulo 32": los bytes 4-7 repetian los 0-3 y la longitud
                        // de "hola" quedaba en 0x400000004. Otra causa de H13.
                        for (int b = 0; b < 8; b++) slot[8 + b] = (uint8_t)((uint64_t)len >> (b * 8));
                        uint32_t slot_off = nb_symtab_add_literal_bytes(globals, slot, 16);
                        nb_data_cadenas_add(cad, val, slot_off);
                    } else {
                        uint64_t bits = nb_data_number_bits(val);
                        uint8_t slot[8];
                        for (int b = 0; b < 8; b++) slot[b] = (uint8_t)(bits >> (b * 8));
                        nb_symtab_add_literal_bytes(globals, slot, 8);
                    }
                }
                break;
            case N_IF:
                nb_scan_data(globals, s->b, restore, cad);
                for (int32_t j = 0; j < s->list_count; j++) nb_scan_data(globals, s->list[j]->b, restore, cad);
                if (s->c) nb_scan_data(globals, s->c, restore, cad);
                break;
            case N_WHILE: nb_scan_data(globals, s->b, restore, cad); break;
            case N_REPEAT: nb_scan_data(globals, s->a, restore, cad); break;
            case N_FOR: nb_scan_data(globals, s->d, restore, cad); break;
            case N_FUNCDEF: nb_scan_data(globals, s->d, restore, cad); break; // SI entra, a diferencia de nb_scan_labels
            default: break;
        }
    }
}


// ---- Las rutinas de error (ver "Errores en tiempo de ejecucion") ----
// Una entrada por error, que prepara su mensaje y salta a la parte comun.
// La parte comun: de donde la llamaron -> linea (tabla) -> mensaje en la pila
// -> registro del sistema (terminal de QEMU) y consola -> fin del programa.
static uint32_t nb_lit(nb_codegen_ctx_t *ctx, const char *t) {
    uint32_t n = 0; while (t[n]) n++;
    return nb_symtab_add_literal_bytes(ctx->globals, (const uint8_t *)t, n + 1);   // con su '\0'
}
// copia_n y digitos: los ayudantes de String$, Hex$, Bin$, LSet$ y RSet$
static void nb_emit_ayudantes_cadenas(nb_codegen_ctx_t *ctx) {
    nb_codebuf_t *cb = ctx->cb;
    if (g_usa_copia) {                       // x0 = origen, x1 = cuantos, x25 = destino (avanza)
        nb_label_t *bucle = nb_label_new(), *fin = nb_label_new();
        nb_label_define(cb, g_h_copia);
        nb_label_define(cb, bucle);
        nb_emit_cbz(cb, 1, fin);
        nb_codebuf_emit(cb, nb_enc_ldrb_post(2, 0, 1));
        nb_codebuf_emit(cb, nb_enc_strb_post(2, 25, 1));
        nb_codebuf_emit(cb, nb_enc_sub_imm(1, 1, 1));
        nb_emit_b(cb, bucle);
        nb_label_define(cb, fin);
        nb_codebuf_emit(cb, nb_enc_ret());
        nb_label_free(bucle); nb_label_free(fin);
    }
    if (g_usa_digitos) {                     // x0 = numero (sin signo), x1 = base, x25 = destino (avanza)
        nb_label_t *dig = nb_label_new(), *rev = nb_label_new(), *fin = nb_label_new();
        nb_label_define(cb, g_h_digitos);
        nb_codebuf_emit(cb, nb_enc_orr_reg(3, NB_REG_ZR, 25));      // donde empiezan
        nb_label_define(cb, dig);
        nb_codebuf_emit(cb, nb_enc_udiv(4, 0, 1));
        nb_codebuf_emit(cb, nb_enc_msub(5, 4, 1, 0));              // x5 = cifra
        nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 5, 10));
        nb_label_t *letra = nb_label_new(), *sigue = nb_label_new();
        nb_emit_bcond(cb, NB_COND_CS, letra);
        nb_codebuf_emit(cb, nb_enc_add_imm(5, 5, '0'));
        nb_emit_b(cb, sigue);
        nb_label_define(cb, letra);
        nb_codebuf_emit(cb, nb_enc_add_imm(5, 5, 'A' - 10));
        nb_label_define(cb, sigue);
        nb_codebuf_emit(cb, nb_enc_strb_post(5, 25, 1));
        nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 4));
        nb_emit_cbnz(cb, 0, dig);
        nb_codebuf_emit(cb, nb_enc_sub_imm(6, 25, 1));              // al reves: darles la vuelta
        nb_label_define(cb, rev);
        nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 3, 6));
        nb_emit_bcond(cb, NB_COND_CS, fin);
        nb_codebuf_emit(cb, nb_enc_ldrb_imm(4, 3, 0));
        nb_codebuf_emit(cb, nb_enc_ldrb_imm(5, 6, 0));
        nb_codebuf_emit(cb, nb_enc_strb_imm(5, 3, 0));
        nb_codebuf_emit(cb, nb_enc_strb_imm(4, 6, 0));
        nb_codebuf_emit(cb, nb_enc_add_imm(3, 3, 1));
        nb_codebuf_emit(cb, nb_enc_sub_imm(6, 6, 1));
        nb_emit_b(cb, rev);
        nb_label_define(cb, fin);
        nb_codebuf_emit(cb, nb_enc_ret());
        nb_label_free(dig); nb_label_free(rev); nb_label_free(fin); nb_label_free(letra); nb_label_free(sigue);
    }
}

// apuntar / marcar / liberar los temporales de una sentencia
static void nb_emit_rutinas_temporales(nb_codegen_ctx_t *ctx) {
    if (!g_usa_temporales) return;
    nb_codebuf_t *cb = ctx->cb;
    nb_symtab_t *gl = ctx->globals;
    {   // ahora si: el hueco en los datos para la lista (a cero)
        uint8_t ceros[(NB_TEMP_MAX + 1) * 8];
        for (uint32_t i = 0; i < sizeof ceros; i++) ceros[i] = 0;
        g_temp_area = nb_symtab_add_literal_bytes(gl, ceros, sizeof ceros);
    }
    {   // apuntar: x0 = la cadena (se conserva); si la lista esta llena, no se apunta
        nb_label_t *lleno = nb_label_new();
        nb_label_define(cb, g_t_apuntar);
        // guarda los registros que toca: las funciones de cadenas usan x9 y
        // vecinos como punteros de trabajo
        nb_codebuf_emit(cb, nb_enc_stp_pre(9, 10, NB_REG_SP, -2));
        nb_codebuf_emit(cb, nb_enc_stp_pre(11, 12, NB_REG_SP, -2));
        nb_emit_global_addr(gl, cb, 9, g_temp_area);
        nb_codebuf_emit(cb, nb_enc_ldr_imm(10, 9, 0));                 // cuantos
        nb_emit_load_imm64(cb, 11, NB_TEMP_MAX);
        nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 10, 11));
        nb_emit_bcond(cb, NB_COND_CS, lleno);
        nb_codebuf_emit(cb, nb_enc_add_imm(11, 10, 1));
        nb_codebuf_emit(cb, nb_enc_add_reg(12, 9, 11, 3));             // + (cuantos+1)*8
        nb_codebuf_emit(cb, nb_enc_str_imm(0, 12, 0));
        nb_codebuf_emit(cb, nb_enc_str_imm(11, 9, 0));                 // cuantos = cuantos + 1
        nb_label_define(cb, lleno);
        nb_codebuf_emit(cb, nb_enc_ldp_post(11, 12, NB_REG_SP, 2));
        nb_codebuf_emit(cb, nb_enc_ldp_post(9, 10, NB_REG_SP, 2));
        nb_codebuf_emit(cb, nb_enc_ret());
        nb_label_free(lleno);
    }
    {   // marca: x0 = cuantos hay ahora
        nb_label_define(cb, g_t_marca);
        nb_codebuf_emit(cb, nb_enc_stp_pre(9, 10, NB_REG_SP, -2));
        nb_emit_global_addr(gl, cb, 9, g_temp_area);
        nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 9, 0));
        nb_codebuf_emit(cb, nb_enc_ldp_post(9, 10, NB_REG_SP, 2));
        nb_codebuf_emit(cb, nb_enc_ret());
    }
    {   // liberar hasta la marca: x0 = marca
        nb_label_t *bucle = nb_label_new(), *fin = nb_label_new();
        nb_label_t *rel = nb_rt_obligatoria(ctx, "nb_string_release", 0);
        nb_label_define(cb, g_t_liberar);
        nb_codebuf_emit(cb, nb_enc_stp_pre(29, 30, NB_REG_SP, -2));    // se llama a release: hay que guardar la vuelta
        nb_codebuf_emit(cb, nb_enc_stp_pre(19, 20, NB_REG_SP, -2));    // y los registros de trabajo de las funciones de cadenas
        nb_codebuf_emit(cb, nb_enc_stp_pre(21, 22, NB_REG_SP, -2));
        nb_codebuf_emit(cb, nb_enc_stp_pre(9, 10, NB_REG_SP, -2));
        nb_codebuf_emit(cb, nb_enc_orr_reg(19, NB_REG_ZR, 0));         // x19 = marca
        nb_emit_global_addr(gl, cb, 20, g_temp_area);                  // x20 = la lista
        nb_label_define(cb, bucle);
        nb_codebuf_emit(cb, nb_enc_ldr_imm(21, 20, 0));                // cuantos
        nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 21, 19));
        nb_emit_bcond(cb, NB_COND_LS, fin);                            // cuantos <= marca: ya esta
        nb_codebuf_emit(cb, nb_enc_add_reg(22, 20, 21, 3));            // + cuantos*8 (el ultimo)
        nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 22, 0));
        nb_codebuf_emit(cb, nb_enc_sub_imm(21, 21, 1));
        nb_codebuf_emit(cb, nb_enc_str_imm(21, 20, 0));                // cuantos = cuantos - 1
        if (rel) nb_emit_bl(cb, rel);
        nb_emit_b(cb, bucle);
        nb_label_define(cb, fin);
        nb_codebuf_emit(cb, nb_enc_ldp_post(9, 10, NB_REG_SP, 2));
        nb_codebuf_emit(cb, nb_enc_ldp_post(21, 22, NB_REG_SP, 2));
        nb_codebuf_emit(cb, nb_enc_ldp_post(19, 20, NB_REG_SP, 2));
        nb_codebuf_emit(cb, nb_enc_ldp_post(29, 30, NB_REG_SP, 2));
        nb_codebuf_emit(cb, nb_enc_ret());
        nb_label_free(bucle); nb_label_free(fin);
    }
}

static void nb_emit_rutinas_error(nb_codegen_ctx_t *ctx) {
    nb_emit_ayudantes_cadenas(ctx);
    nb_emit_rutinas_temporales(ctx);
    if (!(g_usa_indice || g_usa_div0 || g_usa_null || g_usa_dim)) return;
    nb_codebuf_t *cb = ctx->cb;
    nb_symtab_t *gl = ctx->globals;
    // la tabla: [posicion de la sentencia (en instrucciones)][ (archivo << 24) | linea ]
    uint32_t n = g_lin_n;
    uint8_t *tab = (uint8_t *)nb_alloc((n ? n : 1) * 16);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t pos = g_lin_pos[i], lin = g_lin_num[i];
        if (g_inc_mapa) {
            const char *arch; uint32_t lo; bool princ;
            nb_inc_origen(g_inc_mapa, g_lin_num[i], &arch, &lo, &princ);
            uint32_t idx = 0;
            for (uint32_t k = 0; k < g_inc_mapa->n_nombres; k++) if (g_inc_mapa->nombres[k] == arch) idx = k;
            lin = ((uint64_t)idx << 24) | (lo & 0xFFFFFFu);
        }
        for (int b = 0; b < 8; b++) { tab[i * 16 + b] = (uint8_t)(pos >> (8 * b)); tab[i * 16 + 8 + b] = (uint8_t)(lin >> (8 * b)); }
    }
    uint32_t tab_off = nb_symtab_add_literal_bytes(gl, tab, (n ? n : 1) * 16);
    nb_free(tab);
    // los nombres de los archivos de Include: huecos de 32 bytes
    uint32_t nombres_off = 0;
    bool con_nombres = g_inc_mapa && g_inc_mapa->n_nombres > 1;
    if (con_nombres) {
        uint32_t nn = g_inc_mapa->n_nombres;
        uint8_t *b = (uint8_t *)nb_alloc(nn * 32);
        for (uint32_t k = 0; k < nn * 32; k++) b[k] = 0;
        for (uint32_t k = 0; k < nn; k++) for (uint32_t c = 0; c < 31 && g_inc_mapa->nombres[k][c]; c++) b[k * 32 + c] = (uint8_t)g_inc_mapa->nombres[k][c];
        nombres_off = nb_symtab_add_literal_bytes(gl, b, nn * 32);
        nb_free(b);
    }
    uint32_t t_pre = nb_lit(ctx, "Error en tiempo de ejecucion, linea "), t_de = nb_lit(ctx, " de "), t_dos = nb_lit(ctx, ": ");
    uint32_t t_i1 = nb_lit(ctx, " (indice "), t_i2 = nb_lit(ctx, ", el array tiene "), t_i3 = nb_lit(ctx, " elementos)"), t_nl = nb_lit(ctx, "\n");
    nb_label_t *comun = nb_label_new(), *copiar = nb_label_new(), *decimal = nb_label_new();

    // ---- las entradas: x20 = mensaje; x23 = 1 si lleva indice (x21) y tamaño (x22) ----
    if (g_usa_indice && ctx->bounds_error_label) {
        nb_label_define(cb, ctx->bounds_error_label);
        nb_codebuf_emit(cb, nb_enc_orr_reg(21, NB_REG_ZR, 1));
        nb_codebuf_emit(cb, nb_enc_orr_reg(22, NB_REG_ZR, 2));
        nb_codebuf_emit(cb, nb_enc_movz(23, 1, 0));
        nb_emit_global_addr(gl, cb, 20, nb_lit(ctx, "indice de array fuera de rango"));
        nb_emit_b(cb, comun);
    }
    struct { bool usa; nb_label_t *et; const char *msg; } simples[3];
    simples[0].usa = g_usa_div0; simples[0].et = g_err_div0; simples[0].msg = "division entre cero";
    simples[1].usa = g_usa_null; simples[1].et = g_err_null; simples[1].msg = "objeto Null (falta New, o ya se hizo Delete)";
    simples[2].usa = g_usa_dim;  simples[2].et = g_err_dim;  simples[2].msg = "array sin Dim (se usa antes de crearlo)";
    for (int k = 0; k < 3; k++) {
        if (!simples[k].usa) continue;
        nb_label_define(cb, simples[k].et);
        nb_codebuf_emit(cb, nb_enc_movz(23, 0, 0));
        nb_emit_global_addr(gl, cb, 20, nb_lit(ctx, simples[k].msg));
        nb_emit_b(cb, comun);
    }

    // ---- la parte comun ----
    nb_label_define(cb, comun);
    nb_codebuf_emit(cb, nb_enc_orr_reg(10, NB_REG_ZR, 30));        // x10 = vuelta (la instruccion tras el bl que fallo)
    nb_label_t *aqui = nb_label_new();
    nb_emit_bl(cb, aqui);                                           // x30 = la direccion de 'aqui'
    nb_label_define(cb, aqui);
    uint32_t pos_aqui = cb->count;
    nb_label_free(aqui);
    nb_codebuf_emit(cb, nb_enc_orr_reg(11, NB_REG_ZR, 30));
    nb_codebuf_emit(cb, nb_enc_sub_reg(10, 10, 11));                // bytes de 'aqui' a la vuelta
    nb_codebuf_emit(cb, nb_enc_asr_imm(10, 10, 2));                 // en instrucciones
    nb_emit_load_imm64(cb, 12, (uint64_t)pos_aqui - 1);
    nb_codebuf_emit(cb, nb_enc_add_reg(10, 10, 12, 0));             // x10 = posicion del bl que fallo
    // la linea: la ultima sentencia que empieza antes (la tabla va en orden)
    nb_emit_global_addr(gl, cb, 12, tab_off);
    nb_emit_load_imm64(cb, 13, n);
    nb_codebuf_emit(cb, nb_enc_movz(14, 0, 0));
    nb_label_t *bucle = nb_label_new(), *fin_b = nb_label_new();
    nb_label_define(cb, bucle);
    nb_emit_cbz(cb, 13, fin_b);
    nb_codebuf_emit(cb, nb_enc_ldr_imm(15, 12, 0));
    nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 15, 10));
    nb_emit_bcond(cb, NB_COND_HI, fin_b);                           // empieza despues: ya no
    nb_codebuf_emit(cb, nb_enc_ldr_imm(14, 12, 1));
    nb_codebuf_emit(cb, nb_enc_add_imm(12, 12, 16));
    nb_codebuf_emit(cb, nb_enc_sub_imm(13, 13, 1));
    nb_emit_b(cb, bucle);
    nb_label_define(cb, fin_b);
    nb_label_free(bucle); nb_label_free(fin_b);
    // el mensaje, en la pila (x25 = donde se escribe)
    nb_codebuf_emit(cb, nb_enc_sub_imm(NB_REG_SP, NB_REG_SP, 512));
    nb_codebuf_emit(cb, nb_enc_add_imm(25, NB_REG_SP, 0));
    nb_emit_global_addr(gl, cb, 0, t_pre); nb_emit_bl(cb, copiar);
    nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 14, 40));
    nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, 40));                  // la linea (24 bits bajos)
    nb_emit_bl(cb, decimal);
    if (con_nombres) {
        nb_label_t *sin = nb_label_new();
        nb_codebuf_emit(cb, nb_enc_lsr_imm(15, 14, 24));            // el archivo (0: el principal)
        nb_emit_cbz(cb, 15, sin);
        nb_emit_global_addr(gl, cb, 0, t_de); nb_emit_bl(cb, copiar);
        nb_emit_global_addr(gl, cb, 0, nombres_off);
        nb_codebuf_emit(cb, nb_enc_add_reg(0, 0, 15, 5));           // + archivo * 32
        nb_emit_bl(cb, copiar);
        nb_label_define(cb, sin);
        nb_label_free(sin);
    }
    nb_emit_global_addr(gl, cb, 0, t_dos); nb_emit_bl(cb, copiar);
    nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 20)); nb_emit_bl(cb, copiar);
    {
        nb_label_t *sin = nb_label_new();
        nb_emit_cbz(cb, 23, sin);
        nb_emit_global_addr(gl, cb, 0, t_i1); nb_emit_bl(cb, copiar);
        nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 21)); nb_emit_bl(cb, decimal);
        nb_emit_global_addr(gl, cb, 0, t_i2); nb_emit_bl(cb, copiar);
        nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 22)); nb_emit_bl(cb, decimal);
        nb_emit_global_addr(gl, cb, 0, t_i3); nb_emit_bl(cb, copiar);
        nb_label_define(cb, sin);
        nb_label_free(sin);
    }
    nb_codebuf_emit(cb, nb_enc_strb_imm(NB_REG_ZR, 25, 0));
    nb_codebuf_emit(cb, nb_enc_add_imm(0, NB_REG_SP, 0));
    nb_emit_load_imm64(cb, 8, 28);                                  // SYS_DEBUG_LOG: la terminal de QEMU
    nb_codebuf_emit(cb, nb_enc_svc(0));
    nb_emit_global_addr(gl, cb, 0, t_nl); nb_emit_bl(cb, copiar);
    nb_codebuf_emit(cb, nb_enc_strb_imm(NB_REG_ZR, 25, 0));
    nb_codebuf_emit(cb, nb_enc_add_imm(0, NB_REG_SP, 0));
    nb_emit_load_imm64(cb, 8, 11);                                  // SYS_WRITE_STRING: la consola (IDE, Aronnax)
    nb_codebuf_emit(cb, nb_enc_svc(0));
    nb_emit_load_imm64(cb, 8, 0);                                   // SYS_EXIT
    nb_codebuf_emit(cb, nb_enc_svc(0));

    // copiar: la cadena de x0 (hasta su '\0') en x25, que avanza
    {
        nb_label_t *fin = nb_label_new();
        nb_label_define(cb, copiar);
        nb_codebuf_emit(cb, nb_enc_ldrb_post(1, 0, 1));
        nb_emit_cbz(cb, 1, fin);
        nb_codebuf_emit(cb, nb_enc_strb_post(1, 25, 1));
        nb_emit_b(cb, copiar);
        nb_label_define(cb, fin);
        nb_codebuf_emit(cb, nb_enc_ret());
        nb_label_free(fin);
    }
    // decimal: el numero de x0 (con signo), en x25, que avanza
    {
        nb_label_t *pos = nb_label_new(), *dig = nb_label_new(), *rev = nb_label_new(), *fin = nb_label_new();
        nb_label_define(cb, decimal);
        nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0));
        nb_emit_bcond(cb, NB_COND_GE, pos);
        nb_codebuf_emit(cb, nb_enc_movz(1, '-', 0));
        nb_codebuf_emit(cb, nb_enc_strb_post(1, 25, 1));
        nb_codebuf_emit(cb, nb_enc_sub_reg(0, NB_REG_ZR, 0));
        nb_label_define(cb, pos);
        nb_codebuf_emit(cb, nb_enc_movz(2, 10, 0));
        nb_codebuf_emit(cb, nb_enc_orr_reg(3, NB_REG_ZR, 25));       // donde empiezan las cifras
        nb_label_define(cb, dig);
        nb_codebuf_emit(cb, nb_enc_udiv(4, 0, 2));
        nb_codebuf_emit(cb, nb_enc_msub(5, 4, 2, 0));               // x5 = x0 - x4*10: la cifra
        nb_codebuf_emit(cb, nb_enc_add_imm(5, 5, '0'));
        nb_codebuf_emit(cb, nb_enc_strb_post(5, 25, 1));
        nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 4));
        nb_emit_cbnz(cb, 0, dig);
        nb_codebuf_emit(cb, nb_enc_sub_imm(6, 25, 1));               // salieron al reves: darles la vuelta
        nb_label_define(cb, rev);
        nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 3, 6));
        nb_emit_bcond(cb, NB_COND_CS, fin);
        nb_codebuf_emit(cb, nb_enc_ldrb_imm(4, 3, 0));
        nb_codebuf_emit(cb, nb_enc_ldrb_imm(5, 6, 0));
        nb_codebuf_emit(cb, nb_enc_strb_imm(5, 3, 0));
        nb_codebuf_emit(cb, nb_enc_strb_imm(4, 6, 0));
        nb_codebuf_emit(cb, nb_enc_add_imm(3, 3, 1));
        nb_codebuf_emit(cb, nb_enc_sub_imm(6, 6, 1));
        nb_emit_b(cb, rev);
        nb_label_define(cb, fin);
        nb_codebuf_emit(cb, nb_enc_ret());
        nb_label_free(pos); nb_label_free(dig); nb_label_free(rev); nb_label_free(fin);
    }
    nb_label_free(comun); nb_label_free(copiar); nb_label_free(decimal);
}

// ---- Type/New/Delete/campos/ForEach ----
//
// Sin sufijo de tipo en las variables (a diferencia del BlitzBasic
// real, "e.Enemy = New Enemy") -- "obj\campo" no sabe en tiempo de
// compilacion de que Type es 'obj'. Se resuelve con dos piezas:
//
// 1) Tabla de CAMPOS global, compartida entre TODOS los tipos: cada
//    nombre de campo distinto (en todo el programa) recibe un
//    desplazamiento fijo, decidido por orden de aparicion. Un campo
//    con el mismo nombre en dos Types distintos comparte el mismo
//    desplazamiento -- simple y funciona, a costa de que una
//    instancia reserve hueco hasta el desplazamiento mas alto que
//    CUALQUIERA de sus propios campos use (aceptado, documentado,
//    no es un problema para programas con un puñado de tipos).
// 2) Cada instancia lleva su propio type_id oculto en la cabecera
//    (ademas de prev/next) -- necesario para Delete/Insert, que solo
//    reciben un puntero opaco y necesitan saber a que lista
//    pertenece para desenganchar la instancia bien.
//
// Cabecera de instancia: [type_id @0][prev @8][next @16][campo0 @24]...
// Tabla de cabeza/cola: un bloque de datos de num_tipos*16 bytes,
// [head_0][tail_0][head_1][tail_1]... indexado por type_id*16 --
// direccion base fija (adrp+add), con el 16*type_id sumado aparte
// (constante en tiempo de compilacion para New/First/Last, en un
// registro para Delete/Insert que parten de un puntero opaco).

typedef struct { nb_string_t *name; uint32_t offset; } nb_field_entry_t;
typedef struct nb_field_table {
    nb_field_entry_t *entries;
    uint32_t count, cap;
} nb_field_table_t;

static void nb_field_table_init(nb_field_table_t *t) { t->entries = NULL; t->count = 0; t->cap = 0; }
static void nb_field_table_free(nb_field_table_t *t) { if (t->entries) nb_free(t->entries); t->entries = NULL; t->count = 0; t->cap = 0; }
static bool nb_field_table_find(nb_field_table_t *t, nb_string_t *name, uint32_t *out) {
    for (uint32_t i = 0; i < t->count; i++) if (nb_string_eq(t->entries[i].name, name)) { *out = t->entries[i].offset; return true; }
    return false;
}
// Devuelve el desplazamiento del campo, creandolo si es la primera
// vez que se ve ese nombre en todo el programa.
static uint32_t nb_field_table_get_or_add(nb_field_table_t *t, nb_string_t *name) {
    uint32_t off;
    if (nb_field_table_find(t, name, &off)) return off;
    if (t->count >= t->cap) {
        uint32_t new_cap = t->cap == 0 ? 8 : t->cap * 2;
        nb_field_entry_t *ne = (nb_field_entry_t *)nb_alloc(new_cap * (uint32_t)sizeof(nb_field_entry_t));
        if (!ne) return 0;
        for (uint32_t i = 0; i < t->count; i++) ne[i] = t->entries[i];
        if (t->entries) nb_free(t->entries);
        t->entries = ne; t->cap = new_cap;
    }
    t->entries[t->count].name = name;
    t->entries[t->count].offset = t->count;
    return t->entries[t->count++].offset;
}

typedef struct {
    nb_string_t *name;
    uint32_t type_id;
    uint32_t instance_size; // en bytes, cabecera (24) + huecos de campos
    uint32_t *field_offsets; // desplazamientos GLOBALES (de nb_field_table_t) de cada campo de ESTE tipo, en el orden declarado -- no se usa hoy dia por el codegen (que resuelve cada "obj\campo" directamente contra la tabla global), pero queda de documentacion/futuro
    uint32_t field_count;
} nb_type_entry_t;

typedef struct nb_type_table {
    nb_type_entry_t *entries;
    uint32_t count, cap;
    uint32_t headtail_base_offset; // desplazamiento (bytes) del bloque de cabeza/cola en la region de datos, valido cuando count>0
} nb_type_table_t;

static void nb_type_table_init(nb_type_table_t *t) { t->entries = NULL; t->count = 0; t->cap = 0; t->headtail_base_offset = 0; }
static void nb_type_table_free(nb_type_table_t *t) {
    for (uint32_t i = 0; i < t->count; i++) if (t->entries[i].field_offsets) nb_free(t->entries[i].field_offsets);
    if (t->entries) nb_free(t->entries);
    t->entries = NULL; t->count = 0; t->cap = 0;
}
static nb_type_entry_t *nb_type_table_find(nb_type_table_t *t, nb_string_t *name) {
    for (uint32_t i = 0; i < t->count; i++) if (nb_string_eq(t->entries[i].name, name)) return &t->entries[i];
    return NULL;
}

// Barrido de TODO el programa (Type, como Function, se declara a
// nivel superior -- no hace falta entrar en bloques de control ni en
// funciones) recogiendo cada N_TYPEDEF: registra sus campos en la
// tabla global de campos, calcula el tamaño de instancia, y reserva
// el bloque de cabeza/cola al final (una vez se sabe cuantos tipos
// hay en total).
static void nb_scan_types(nb_node_t *program, nb_symtab_t *globals, nb_field_table_t *fields, nb_type_table_t *types) {
    for (int32_t i = 0; i < program->list_count; i++) {
        nb_node_t *s = program->list[i];
        if (s->kind != N_TYPEDEF) continue;
        if (types->count >= types->cap) {
            uint32_t new_cap = types->cap == 0 ? 8 : types->cap * 2;
            nb_type_entry_t *ne = (nb_type_entry_t *)nb_alloc(new_cap * (uint32_t)sizeof(nb_type_entry_t));
            if (!ne) continue;
            for (uint32_t j = 0; j < types->count; j++) ne[j] = types->entries[j];
            if (types->entries) nb_free(types->entries);
            types->entries = ne; types->cap = new_cap;
        }
        nb_type_entry_t *te = &types->entries[types->count];
        te->name = s->text;
        te->type_id = types->count;
        te->field_count = (uint32_t)s->list_count;
        te->field_offsets = te->field_count > 0 ? (uint32_t *)nb_alloc(te->field_count * (uint32_t)sizeof(uint32_t)) : NULL;
        uint32_t max_field_offset = 0;
        for (int32_t j = 0; j < s->list_count; j++) {
            uint32_t off = nb_field_table_get_or_add(fields, s->list[j]->text);
            if (te->field_offsets) te->field_offsets[j] = off;
            if (off > max_field_offset) max_field_offset = off;
        }
        te->instance_size = 24 + (s->list_count > 0 ? (max_field_offset + 1) * 8 : 0);
        types->count++;
    }
    if (types->count > 0) {
        uint8_t *zero = (uint8_t *)nb_alloc(types->count * 16);
        if (zero) {
            for (uint32_t i = 0; i < types->count * 16; i++) zero[i] = 0;
            types->headtail_base_offset = nb_symtab_add_literal_bytes(globals, zero, types->count * 16);
            nb_free(zero);
        }
    }
}

static void nb_func_table_init(nb_func_table_t *ft) { ft->entries = NULL; ft->count = 0; ft->cap = 0; }

static void nb_func_table_free(nb_func_table_t *ft) {
    for (uint32_t i = 0; i < ft->count; i++) nb_label_free(ft->entries[i].label);
    if (ft->entries) nb_free(ft->entries);
    ft->entries = NULL; ft->count = 0; ft->cap = 0;
}

static nb_func_entry_t *nb_func_table_find(nb_func_table_t *ft, nb_string_t *name) {
    for (uint32_t i = 0; i < ft->count; i++) if (nb_string_eq(ft->entries[i].name, name)) return &ft->entries[i];
    return NULL;
}

static nb_func_entry_t *nb_func_table_add(nb_func_table_t *ft, nb_string_t *name, int32_t param_count) {
    if (ft->count >= ft->cap) {
        uint32_t new_cap = ft->cap == 0 ? 8 : ft->cap * 2;
        nb_func_entry_t *ne = (nb_func_entry_t *)nb_alloc(new_cap * (uint32_t)sizeof(nb_func_entry_t));
        if (!ne) return NULL;
        for (uint32_t i = 0; i < ft->count; i++) ne[i] = ft->entries[i];
        if (ft->entries) nb_free(ft->entries);
        ft->entries = ne; ft->cap = new_cap;
    }
    nb_func_entry_t *e = &ft->entries[ft->count++];
    e->name = name; e->label = nb_label_new(); e->param_count = param_count; e->def = NULL;
    return e;
}

void nb_emit_expr_int(nb_codegen_ctx_t *ctx, nb_node_t *n); // adelantada -- N_CALL llama a nb_emit_expr para cada argumento
void nb_emit_expr(nb_codegen_ctx_t *ctx, nb_node_t *n);
typedef enum { NB_TYPE_INT, NB_TYPE_FLOAT, NB_TYPE_STRING } nb_var_type_t; // adelantada -- ver la definicion completa mas abajo
static nb_var_type_t nb_infer_type(nb_string_t *name); // adelantada -- la usan Abs/Min/Max y las llamadas
static void nb_emit_expr_para(nb_codegen_ctx_t *ctx, nb_node_t *value, nb_var_type_t destino); // adelantada
// Adelantada -- N_BINOP de nb_emit_expr_int distingue comparaciones de
// cadena. NO es static: la usa tambien el parser, para Select (ver abajo).
nb_var_type_t nb_expr_type(nb_node_t *n);
static void nb_emit_expr_string(nb_codegen_ctx_t *ctx, nb_node_t *n); // adelantada -- idem
static bool nb_es_cadena_guardada(nb_codegen_ctx_t *ctx, nb_node_t *v); // adelantada (la usan los temporales)
static void nb_emit_expr_as_float(nb_codegen_ctx_t *ctx, nb_node_t *n); // adelantada -- la usan Sqr/Abs/Int desde la ruta entera
static void nb_emit_array_addr(nb_codegen_ctx_t *ctx, nb_node_t *n); // adelantada -- N_INDEX en las tres rutas de tipo

// Direccion de un campo ("obj\campo") en el registro 'reg' --
// evalua el objeto primero (puntero a instancia, en x0), luego suma
// el desplazamiento fijo del campo (24 de cabecera + su hueco en la
// tabla GLOBAL de campos, compartida entre todos los Types). Si el
// nombre de campo no esta en la tabla (no deberia pasar en un
// programa bien formado -- todo campo usado viene de un Type
// declarado), se trata como desplazamiento 0, sin fallar.
static void nb_emit_field_addr(nb_codegen_ctx_t *ctx, nb_node_t *n, int32_t reg) {
    nb_emit_expr_int(ctx, n->a); // x0 = puntero a la instancia
    if (g_err_null) { nb_emit_si_cero_error(ctx->cb, 0, g_err_null); g_usa_null = true; }   // Null: falta New
    uint32_t field_off = 0;
    if (ctx->fields) nb_field_table_find(ctx->fields, n->text, &field_off);
    nb_codebuf_emit(ctx->cb, nb_enc_add_imm(reg, 0, 24 + field_off * 8));
}

static void nb_emit_expr_float(nb_codegen_ctx_t *ctx, nb_node_t *n);  // adelantada
const char *nb_firma_de(nb_string_t *nombre);   // adelantada (la firma de una orden incorporada)
// Quien evalua por el camino DECIMAL una llamada quiere los bits del double
// en x0, no el numero truncado: esta bandera lo distingue.
static bool g_quiere_bits;

void nb_emit_expr_int(nb_codegen_ctx_t *ctx, nb_node_t *n) {
    nb_codebuf_t *cb = ctx->cb;
    // Un valor DECIMAL donde se espera un entero se convierte
    // (truncando, como Int). Antes se pasaban sus BITS en crudo: "DrawImage
    // bola, x# - 8, y# - 8" dibujaba en la coordenada 4636772475726725112 --
    // es decir, en ninguna parte. Le pasaba a cualquier orden con argumentos
    // enteros (Rect, Text, DrawImage...) y a los indices de array.
    // Se va directo a nb_emit_expr_float (no a as_float): as_float, ante un
    // nodo que no sabe tratar, vuelve a llamar aqui, y las dos funciones se
    // llamarian sin fin.
    if ((n->kind == N_VAR || n->kind == N_UNOP || n->kind == N_BINOP || n->kind == N_INDEX) &&
        nb_expr_type(n) == NB_TYPE_FLOAT) {
        nb_emit_expr_float(ctx, n);                  // -> d0
        nb_codebuf_emit(cb, nb_enc_fcvtzs(0, 0));    // truncado, como Int
        return;
    }
    // Una llamada que devuelve decimal (Sqr, Sin, una funcion F#...) deja sus
    // BITS en x0. Se vuelve a entrar con la bandera puesta para obtenerlos y
    // luego se convierten al numero que esperaba quien pregunta.
    if (!g_quiere_bits && n->kind == N_CALL && nb_expr_type(n) == NB_TYPE_FLOAT) {
        g_quiere_bits = true;
        nb_emit_expr_int(ctx, n);
        g_quiere_bits = false;
        nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(0, 0));
        nb_codebuf_emit(cb, nb_enc_fcvtzs(0, 0));
        return;
    }
    switch (n->kind) {
        case N_NUM: {
            int64_t v = (int64_t)n->num_value;
            nb_emit_load_imm64(cb, 0, (uint64_t)v);
            return;
        }
        case N_VAR: {
            nb_emit_load_var_x0(ctx, n->text);
            return;
        }
        case N_FIELD: {
            nb_emit_field_addr(ctx, n, 9);
            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 9, 0));
            return;
        }
        case N_NEW: {
            nb_type_entry_t *te = ctx->types ? nb_type_table_find(ctx->types, n->text) : NULL;
            if (!te) { nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0)); return; }
            nb_emit_load_imm64(cb, 0, te->instance_size);
            nb_label_t *fn_alloc = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_alloc", n->line) : NULL;
            if (fn_alloc) nb_emit_bl(cb, fn_alloc);
            nb_push_x0(cb); // la nueva instancia, a salvo en la pila durante todo lo que sigue

            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0));
            nb_codebuf_emit(cb, nb_enc_movz(1, (uint16_t)te->type_id, 0));
            nb_codebuf_emit(cb, nb_enc_str_imm(1, 0, 0));         // [inst+0] = type_id
            nb_codebuf_emit(cb, nb_enc_str_imm(NB_REG_ZR, 0, 1)); // [inst+8] = 0 (prev)
            // [inst+16] = 0 (next). Faltaba: la instancia nueva conservaba
            // en 'next' lo que hubiera en esa memoria. Mientras toda la
            // memoria salia nueva del monton (a cero) no se notaba; en
            // cuanto un Delete liberaba un bloque y el siguiente New lo
            // reutilizaba, 'next' traia un puntero viejo, la lista quedaba
            // cerrada en circulo y el For Each no terminaba nunca. Visto
            // con fuente.nb: se colgaba justo en el fotograma 81, el primero
            // tras morir las primeras particulas (viven 80).
            nb_codebuf_emit(cb, nb_enc_str_imm(NB_REG_ZR, 0, 2)); // [inst+16] = 0 (next)
            uint32_t nfields_slots = (te->instance_size - 24) / 8;
            for (uint32_t i = 0; i < nfields_slots; i++) {
                nb_codebuf_emit(cb, nb_enc_str_imm(NB_REG_ZR, 0, 3 + i)); // [inst+24+8i] = 0
            }

            // Enganchar al final de la lista de instancias de su tipo.
            nb_emit_global_addr(ctx->globals, cb, 9, ctx->types->headtail_base_offset + te->type_id * 16);
            nb_codebuf_emit(cb, nb_enc_ldr_imm(2, 9, 1)); // x2 = tail actual del tipo
            nb_label_t *l_empty = nb_label_new();
            nb_label_t *l_done = nb_label_new();
            nb_emit_cbz(cb, 2, l_empty);
            // Lista no vacia: inst->prev = tail; tail->next = inst.
            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0));
            nb_codebuf_emit(cb, nb_enc_str_imm(2, 0, 1));
            nb_codebuf_emit(cb, nb_enc_str_imm(0, 2, 2));
            nb_emit_b(cb, l_done);
            nb_label_define(cb, l_empty);
            // Lista vacia: head del tipo = inst.
            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0));
            nb_codebuf_emit(cb, nb_enc_str_imm(0, 9, 0));
            nb_label_define(cb, l_done);
            // En cualquier caso: tail del tipo = inst.
            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0));
            nb_codebuf_emit(cb, nb_enc_str_imm(0, 9, 1));
            nb_label_free(l_empty);
            nb_label_free(l_done);

            nb_pop_x1(cb);
            nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1)); // resultado final: la instancia, recuperada de la pila
            return;
        }
        case N_FIRSTLAST: {
            nb_type_entry_t *te = ctx->types ? nb_type_table_find(ctx->types, n->text) : NULL;
            if (!te) { nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0)); return; }
            nb_emit_global_addr(ctx->globals, cb, 9, ctx->types->headtail_base_offset + te->type_id * 16);
            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 9, n->op == TOK_KW_FIRST ? 0 : 1));
            return;
        }
        case N_UNOP: {
            nb_emit_expr_int(ctx, n->a);
            if (n->op == TOK_MINUS) {
                // neg x0 = sub x0, xzr, x0
                nb_codebuf_emit(cb, nb_enc_sub_reg(0, NB_REG_ZR, 0));
            } else if (n->op == TOK_KW_NOT) {
                // Not LOGICO: 0 si el valor era distinto de cero, -1
                // (todos los bits) si era cero. Mismo convenio que las
                // comparaciones -- ver la nota de TOK_EQ mas abajo.
                //
                // ESTABA MAL: se generaba la inversion de BITS
                // (-x - 1). Con eso, Not 0 = -1 (verdadero, bien) pero
                // Not 1 = -2, que tambien es distinto de cero y por
                // tanto TAMBIEN verdadero. Un "While Not KeyDown(1)"
                // no salia nunca, pulsaras lo que pulsaras -- parecia
                // que la tecla no funcionaba, y la tecla estaba bien.
                //
                // cmp x0,#0 + cset x0,eq: una comparacion y una
                // instruccion condicional, sin ramas.
                nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0)); // cmp x0, #0
                nb_codebuf_emit(cb, nb_enc_csetm(0, NB_COND_EQ));      // x0 = (x0 == 0) ? -1 : 0
            }
            return;
        }
        case N_BINOP: {
            // Comparacion entre CADENAS: ruta totalmente distinta a
            // la generica de abajo (que asume enteros) -- los
            // operandos se evaluan como cadenas (nb_emit_expr_string),
            // no como enteros, y se llama a nb_string_eq/nb_string_cmp
            // en vez de subs+cset directo. El resultado sigue siendo
            // un entero de 0/1 al final, por eso vive aqui y no en
            // nb_emit_expr_string.
            bool cmp_is_string = (n->op == TOK_EQ || n->op == TOK_NE || n->op == TOK_LT ||
                                   n->op == TOK_GT || n->op == TOK_LE || n->op == TOK_GE) &&
                                  nb_expr_type(n->a) == NB_TYPE_STRING;
            if (cmp_is_string) {
                nb_emit_expr_string(ctx, n->a); // -> x0 (izquierda)
                nb_push_x0(cb);
                nb_emit_expr_string(ctx, n->b); // -> x0 (derecha)
                nb_pop_x1(cb);                   // izquierda -> x1
                // nb_string_eq/cmp(a,b) quieren a en x0, b en x1 --
                // igual que en la concatenacion, se intercambia con x2.
                nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 0));
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 2));
                // Las comparaciones de cadenas tienen que devolver lo
                // MISMO que las de numeros: -1 verdadero, 0 falso. Si
                // no, "a$ = b$ And flag" se comportaria distinto que
                // "a = b And flag", que es justo el tipo de
                // incoherencia que cuesta horas de encontrar.
                if (n->op == TOK_EQ || n->op == TOK_NE) {
                    nb_label_t *fn = nb_rt_obligatoria(ctx, "nb_string_eq", n->line);
                    if (fn) nb_emit_bl(cb, fn);   // x0 = 0 o 1
                    // Comparar con cero y volver a generar el valor con
                    // csetm: para TOK_EQ queremos -1 cuando x0 era 1,
                    // para TOK_NE cuando era 0.
                    nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0)); // cmp x0, #0
                    nb_codebuf_emit(cb, nb_enc_csetm(0, n->op == TOK_EQ ? NB_COND_NE : NB_COND_EQ));
                } else {
                    nb_label_t *fn = nb_rt_obligatoria(ctx, "nb_string_cmp", n->line);
                    if (fn) nb_emit_bl(cb, fn); // x0 = <0, 0, >0 (orden strcmp)
                    nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0)); // cmp x0, #0
                    int32_t cond = n->op == TOK_LT ? NB_COND_LT : n->op == TOK_GT ? NB_COND_GT
                                 : n->op == TOK_LE ? NB_COND_LE : NB_COND_GE;
                    nb_codebuf_emit(cb, nb_enc_csetm(0, cond));
                }
                return;
            }

            // COMPARAR DECIMALES. Antes las comparaciones pasaban
            // SIEMPRE por enteros: "d# < 0.01" acababa siendo "0 < 0" (falso),
            // y comparar dos variables decimales comparaba sus bits en crudo
            // (bien por casualidad con positivos, mal con negativos). Si
            // cualquiera de los dos lados es decimal, se comparan como tales.
            {
                bool es_cmp = (n->op == TOK_EQ || n->op == TOK_NE || n->op == TOK_LT ||
                               n->op == TOK_GT || n->op == TOK_LE || n->op == TOK_GE);
                if (es_cmp && (nb_expr_type(n->a) == NB_TYPE_FLOAT || nb_expr_type(n->b) == NB_TYPE_FLOAT)) {
                    nb_emit_expr_as_float(ctx, n->a);                       // d0 = izquierda
                    nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0));
                    nb_push_x0(cb);
                    nb_emit_expr_as_float(ctx, n->b);                       // d0 = derecha
                    nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0));
                    nb_pop_x1(cb);
                    nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(1, 1));      // d1 = izquierda
                    nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(0, 0));      // d0 = derecha
                    nb_codebuf_emit(cb, nb_enc_fcmp(1, 0));
                    // Tras fcmp hacen falta las condiciones SIN signo: MI para
                    // "menor" y LS para "menor o igual" (LT y LE miran el bit V,
                    // que en coma flotante no significa lo mismo).
                    int32_t cond = n->op == TOK_EQ ? NB_COND_EQ : n->op == TOK_NE ? NB_COND_NE
                                 : n->op == TOK_LT ? NB_COND_MI : n->op == TOK_GT ? NB_COND_GT
                                 : n->op == TOK_LE ? NB_COND_LS : NB_COND_GE;
                    nb_codebuf_emit(cb, nb_enc_csetm(0, cond));
                    return;
                }
            }

            nb_emit_expr_int(ctx, n->a); // izquierda -> x0
            nb_push_x0(cb);
            nb_emit_expr_int(ctx, n->b); // derecha -> x0
            nb_pop_x1(cb);              // izquierda -> x1
            // Ahora: izquierda=x1, derecha=x0. Todas las operaciones
            // de dos registros de nb_encode.c reciben (rd,rn,rm) con
            // resultado = rn OP rm -- para las no conmutativas
            // (resta, division, modulo), rn=x1 (izquierda) y rm=x0
            // (derecha), en ese orden, para que "a - b" reste b de a
            // y no al reves.
            switch (n->op) {
                case TOK_PLUS:  nb_codebuf_emit(cb, nb_enc_add_reg(0, 1, 0, 0)); break;
                case TOK_MINUS: nb_codebuf_emit(cb, nb_enc_sub_reg(0, 1, 0)); break;
                case TOK_STAR:  nb_codebuf_emit(cb, nb_enc_mul(0, 1, 0)); break;
                case TOK_SLASH:
                    if (g_err_div0) { nb_emit_si_cero_error(cb, 0, g_err_div0); g_usa_div0 = true; }   // x0 = divisor
                    nb_codebuf_emit(cb, nb_enc_sdiv(0, 1, 0)); break;
                case TOK_KW_MOD: {
                    if (g_err_div0) { nb_emit_si_cero_error(cb, 0, g_err_div0); g_usa_div0 = true; }
                    // No hay instruccion de resto directa en ARM64 --
                    // Mod a,b = a - (a/b)*b, con division truncada
                    // hacia cero (igual que sdiv). x2 de temporal para
                    // no pisar x0/x1 antes de terminar el calculo.
                    nb_codebuf_emit(cb, nb_enc_sdiv(2, 1, 0));   // x2 = a/b
                    nb_codebuf_emit(cb, nb_enc_msub(0, 2, 0, 1)); // x0 = a - x2*b = x1 - x2*x0
                    break;
                }
                case TOK_KW_AND: nb_codebuf_emit(cb, nb_enc_and_reg(0, 1, 0)); break;
                case TOK_KW_OR:  nb_codebuf_emit(cb, nb_enc_orr_reg(0, 1, 0)); break;
                case TOK_KW_XOR: nb_codebuf_emit(cb, nb_enc_eor_reg(0, 1, 0)); break;
                case TOK_KW_SHL: nb_codebuf_emit(cb, nb_enc_lslv(0, 1, 0)); break;
                case TOK_KW_SHR: nb_codebuf_emit(cb, nb_enc_lsrv(0, 1, 0)); break;
                case TOK_KW_SAR: nb_codebuf_emit(cb, nb_enc_asrv(0, 1, 0)); break;
                // Comparaciones: subs descarta el resultado (solo le
                // interesan los flags) y cset vuelca el resultado de
                // la condicion como 0 o 1 -- coincide con el convenio
                // de este lenguaje (True=1, False=0, visto en el
                // parser con las palabras clave True/False), asi que
                // no hace falta ninguna inversion de signo.
                // Las comparaciones devuelven -1 para verdadero (TODOS
                // los bits a uno) y 0 para falso, como en BlitzBasic.
                //
                // No es un capricho: And/Or/Xor de este lenguaje son
                // operadores de BITS -- pertenecen a la misma familia
                // que Shl/Shr/Sar, que tienen palabra propia. Con
                // verdadero = 1 funcionaban solo por casualidad, y
                // fallaban en cuanto un lado era un numero cualquiera:
                //   flag = 2
                //   If flag And (x > 5)   ->  2 And 1 = 0  (falso!)
                // Con verdadero = -1, "2 And -1" da 2, que es
                // verdadero -- y "c And 0xFF" para enmascarar sigue
                // funcionando igual. Un solo operador, los dos usos.
                case TOK_EQ: nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 1, 0)); nb_codebuf_emit(cb, nb_enc_csetm(0, NB_COND_EQ)); break;
                case TOK_NE: nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 1, 0)); nb_codebuf_emit(cb, nb_enc_csetm(0, NB_COND_NE)); break;
                case TOK_LT: nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 1, 0)); nb_codebuf_emit(cb, nb_enc_csetm(0, NB_COND_LT)); break;
                case TOK_GT: nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 1, 0)); nb_codebuf_emit(cb, nb_enc_csetm(0, NB_COND_GT)); break;
                case TOK_LE: nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 1, 0)); nb_codebuf_emit(cb, nb_enc_csetm(0, NB_COND_LE)); break;
                case TOK_GE: nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 1, 0)); nb_codebuf_emit(cb, nb_enc_csetm(0, NB_COND_GE)); break;
                default: break;
            }
            return;
        }
        case N_CALL: {
            // "a(3)" dentro de una expresion es AMBIGUO a nivel de
            // sintaxis entre "llamada a funcion" e "indexado de
            // array" -- el parser siempre produce N_CALL aqui (solo
            // el DESTINO de una asignacion, "a(3) = 5", se distingue
            // como N_INDEX en el propio parser). Por eso el primer
            // paso es comprobar si 'n->text' es un nombre ya
            // declarado con Dim -- si lo es, esto es una LECTURA de
            // array, no una llamada, y se trata exactamente igual que
            // N_INDEX (misma direccion, mismo valor cargado).
            if (ctx->arrays && nb_array_set_contains(ctx->arrays, n->text) &&
                (n->list_count == 1 || n->list_count == 2)) {
                nb_emit_array_addr(ctx, n);
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 0, 0));
                return;
            }
            // Los cientos de funciones incorporadas (Str, Sin...) son
            // una pieza aparte, todavia no soportada aqui -- salvo
            // Left$/Right$/Mid$, que ya viven en el bloque de runtime
            // y cuya firma coincide exactamente con este mecanismo de
            // llamada generico (argumentos en orden, en x0/x1/x2...),
            // asi que no hace falta ningun camino especial para ellas,
            // solo encontrar su etiqueta en la tabla de runtime en vez
            // de en la de funciones de usuario.
            //
            // NOTA sobre mayusculas: la comparacion es EXACTA contra
            // "Left$"/"Right$"/"Mid$", tal como se escriben aqui -- si
            // el lexer llegara a normalizar identificadores a otra
            // capitalizacion, o si el lenguaje quisiera tratarlos sin
            // distinguir mayusculas de minusculas, esto habria que
            // revisarlo entonces; por ahora no hay constancia de que
            // haga falta.
            // Funciones matematicas ENTERAS que se resuelven sin
            // llamar a nada -- unas pocas instrucciones del propio
            // procesador, sin ramas:
            //   Abs(x)     -> cmp + cneg     (valor absoluto)
            //   Sgn(x)     -> dos cset       (-1, 0 o 1)
            //   Min(a,b)   -> cmp + csel     (el menor)
            //   Max(a,b)   -> cmp + csel     (el mayor)
            //   Int(x#)    -> fcvtzs         (flotante -> entero, truncando)
            // Sin/Cos/Tan/Log/Exp NO estan aqui: necesitan algoritmo
            // real y van al bloque de runtime, pieza aparte.
            // Abs y Min/Max SIN sufijo con argumentos decimales.
            // Antes evaluaban SIEMPRE como enteros: con decimales comparaban
            // o negaban los bits en crudo -- Min(x#, y#) acertaba por
            // casualidad con positivos y fallaba con negativos, y Abs(x#)
            // devolvia basura. Ahora, si algun argumento es decimal, se
            // calcula en decimal y se dejan los BITS en x0 (como Abs#), y
            // nb_expr_type los da por decimales.
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "Abs") && nb_expr_type(n->list[0]) == NB_TYPE_FLOAT) {
                nb_emit_expr_as_float(ctx, n->list[0]);
                nb_codebuf_emit(cb, nb_enc_fabs(0, 0));
                nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0));
                return;
            }
            if (n->list_count == 2 && (nb_text_eq_cstr(n->text, "Min") || nb_text_eq_cstr(n->text, "Max")) &&
                (nb_expr_type(n->list[0]) == NB_TYPE_FLOAT || nb_expr_type(n->list[1]) == NB_TYPE_FLOAT)) {
                bool es_min = nb_text_eq_cstr(n->text, "Min");
                nb_emit_expr_as_float(ctx, n->list[0]);                  // d0 = a
                nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0));
                nb_push_x0(cb);                                           // bits de a, a salvo
                nb_emit_expr_as_float(ctx, n->list[1]);                  // d0 = b
                nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0));        // x0 = bits de b
                nb_pop_x1(cb);                                            // x1 = bits de a
                nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(1, 1));        // d1 = a
                nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(0, 0));        // d0 = b
                nb_codebuf_emit(cb, nb_enc_fcmp(1, 0));                   // fcmp a, b
                // MI = "menor" (ordenado) tras fcmp; GT = "mayor". Si no, queda b.
                nb_codebuf_emit(cb, nb_enc_csel(0, 1, 0, es_min ? NB_COND_MI : NB_COND_GT));
                return;
            }
            if (n->list_count == 1 && (nb_text_eq_cstr(n->text, "Abs") || nb_text_eq_cstr(n->text, "Abs%"))) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0)); // cmp x0, #0
                nb_codebuf_emit(cb, nb_enc_cneg(0, 0, NB_COND_LT));    // si es negativo, cambiarle el signo
                return;
            }
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "Sgn")) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0)); // cmp x0, #0
                nb_codebuf_emit(cb, nb_enc_cset(1, NB_COND_GT));       // x1 = (x > 0)
                nb_codebuf_emit(cb, nb_enc_cset(2, NB_COND_LT));       // x2 = (x < 0)
                nb_codebuf_emit(cb, nb_enc_sub_reg(0, 1, 2));          // x0 = (x>0) - (x<0)  ->  1, 0 o -1
                return;
            }
            if (n->list_count == 2 && (nb_text_eq_cstr(n->text, "Min") || nb_text_eq_cstr(n->text, "Max"))) {
                bool is_min = nb_text_eq_cstr(n->text, "Min");
                nb_emit_expr_int(ctx, n->list[0]);
                nb_push_x0(cb);
                nb_emit_expr_int(ctx, n->list[1]); // x0 = b
                nb_pop_x1(cb);                     // x1 = a
                nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 1, 0)); // cmp a, b
                // csel x0, x1(a), x0(b), cond -- con LT se queda 'a'
                // si a<b (el menor); con GT se queda 'a' si a>b (el
                // mayor). En los dos casos, si no se cumple, queda 'b'.
                nb_codebuf_emit(cb, nb_enc_csel(0, 1, 0, is_min ? NB_COND_LT : NB_COND_GT));
                return;
            }
            if (n->list_count == 1 && (nb_text_eq_cstr(n->text, "Int") || nb_text_eq_cstr(n->text, "Int%"))) {
                nb_emit_expr_as_float(ctx, n->list[0]);
                nb_codebuf_emit(cb, nb_enc_fcvtzs(0, 0)); // trunca hacia cero, como el Int de BlitzBasic
                return;
            }
            // Trigonometricas: estas SI necesitan algoritmo real
            // (serie polinomica), asi que viven en el bloque de
            // runtime (nb_math.c), no se generan aqui. El argumento
            // va en d0 y el resultado vuelve en d0 -- el convenio
            // REAL de ARM64 para dobles, igual que nb_float_to_str;
            // el resultado se pasa a x0 como bits, que es lo que esta
            // ruta (la entera) debe dejar.
            //
            // ANGULOS EN GRADOS, no radianes: misma convencion que
            // BlitzPlus/Blitz3D y que el compilador anterior.
            if (n->list_count == 1) {
                const char *trig = NULL;
                if (nb_text_eq_cstr(n->text, "Sin") || nb_text_eq_cstr(n->text, "Sin#")) trig = "nb_sin";
                else if (nb_text_eq_cstr(n->text, "Cos") || nb_text_eq_cstr(n->text, "Cos#")) trig = "nb_cos";
                else if (nb_text_eq_cstr(n->text, "Tan") || nb_text_eq_cstr(n->text, "Tan#")) trig = "nb_tan";
                else if (nb_text_eq_cstr(n->text, "ATan") || nb_text_eq_cstr(n->text, "ATan#")) trig = "nb_atan";
                if (trig) {
                    nb_emit_expr_as_float(ctx, n->list[0]); // d0 = argumento
                    nb_label_t *fn = ctx->runtime ? nb_rt_obligatoria(ctx, trig, n->line) : NULL;
                    if (fn) nb_emit_bl(cb, fn);
                    nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0)); // bits del resultado a x0
                    return;
                }
            }
            // Las de RESULTADO FLOTANTE se resuelven tambien aqui, en
            // la ruta entera, y no en la flotante. Motivo: un nombre
            // sin sufijo ("Sqr(2.0)") se infiere como entero, asi que
            // una asignacion a variable flotante llega por este
            // camino igualmente. Dejando los BITS del resultado en x0
            // (fmov d0 -> x0 al final), las dos rutas funcionan: la
            // entera ya los tiene donde espera, y la flotante hace su
            // propio fmov x0 -> d0 justo despues, que los devuelve a
            // su sitio sin perder nada.
            //   Sqr(x)   -> fsqrt   (raiz cuadrada)
            //   Abs#(x)  -> fabs    (valor absoluto de un flotante)
            //   Float(n) -> scvtf   (entero -> flotante, explicito)
            // Sin/Cos/Tan/Log/Exp/ATan NO estan aqui: necesitan un
            // algoritmo real (serie polinomica), que va al bloque de
            // runtime como pieza aparte -- igual que Str$/Val.
            if (n->list_count == 1) {
                int which = 0; // 1=fsqrt  2=fabs  3=scvtf
                if (nb_text_eq_cstr(n->text, "Sqr") || nb_text_eq_cstr(n->text, "Sqr#")) which = 1;
                else if (nb_text_eq_cstr(n->text, "Abs#")) which = 2;
                else if (nb_text_eq_cstr(n->text, "Float") || nb_text_eq_cstr(n->text, "Float#")) which = 3;
                if (which) {
                    if (which == 3) {
                        nb_emit_expr_int(ctx, n->list[0]);
                        nb_codebuf_emit(cb, nb_enc_scvtf(0, 0));
                    } else {
                        nb_emit_expr_as_float(ctx, n->list[0]); // promociona un entero si hace falta
                        nb_codebuf_emit(cb, which == 1 ? nb_enc_fsqrt(0, 0) : nb_enc_fabs(0, 0));
                    }
                    nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0)); // bits del resultado a x0
                    return;
                }
            }
            // ---- Gadgets: crear controles ----
            //
            // Devuelven un identificador, asi que son FUNCIONES y van
            // por esta ruta. La ventana NO se pasa: el kernel la
            // deduce de quien llama, igual que las syscalls de dibujo.
            //
            // OJO con el empaquetado: estas syscalls reciben el ancho
            // y el alto JUNTOS en un solo argumento (ancho<<16|alto).
            // El programador escribe los cuatro numeros por separado y
            // es el compilador quien los arma -- que es justo el tipo
            // de detalle que no deberia asomar en el lenguaje.
            //
            //   CreateButton(texto$, x, y, ancho, alto)  -> syscall 100
            //   CreatePanel(x, y, ancho, alto)           -> 101
            //   CreateTextField(x, y, ancho, alto)       -> 102
            //   CreateListBox(x, y, ancho, alto)         -> 103
            {
                uint32_t gsys = 0;
                bool con_texto = false;
                // El ESTILO de un boton va en x4, y antes no se
                // cargaba nunca: el kernel recibia lo que quedara en el registro
                // (con un 2 o un 3, un boton normal habria salido como casilla o
                // como opcion). Ahora siempre se carga: 0 boton, 2 casilla, 3
                // opcion. El deslizador lleva el suyo en x3: 1 horizontal, 2 vertical.
                uint32_t estilo_x4 = 0, estilo_x3 = 0;
                int32_t arg_vertical = -1;          // CreateSlider con 5 argumentos: el quinto dice si es vertical
                if (nb_text_eq_cstr(n->text, "CreateButton") && n->list_count == 5) { gsys = 100; con_texto = true; }
                else if (nb_text_eq_cstr(n->text, "CreateCheckBox") && n->list_count == 5) { gsys = 100; con_texto = true; estilo_x4 = 2; }
                else if (nb_text_eq_cstr(n->text, "CreateRadio") && n->list_count == 5) { gsys = 100; con_texto = true; estilo_x4 = 3; }
                // CreateToolBar(archivo$, x, y, ancho, alto) -- 172: la tira de iconos
                // (NIMG) la carga el kernel; ancho y alto 0 = los de la imagen
                else if (nb_text_eq_cstr(n->text, "CreateToolBar") && n->list_count == 5) { gsys = 172; con_texto = true; }
                else if (nb_text_eq_cstr(n->text, "CreatePanel") && n->list_count == 4) gsys = 101;
                else if (nb_text_eq_cstr(n->text, "CreateTextField") && n->list_count == 4) gsys = 102;
                else if (nb_text_eq_cstr(n->text, "CreateListBox") && n->list_count == 4) gsys = 103;
                else if (nb_text_eq_cstr(n->text, "CreateTextArea") && n->list_count == 4) gsys = 125;
                else if (nb_text_eq_cstr(n->text, "CreateProgBar") && n->list_count == 4) gsys = 161;
                else if (nb_text_eq_cstr(n->text, "CreateComboBox") && n->list_count == 4) gsys = 167;
                else if (nb_text_eq_cstr(n->text, "CreateTabber") && n->list_count == 4) gsys = 168;
                else if (nb_text_eq_cstr(n->text, "CreateTreeView") && n->list_count == 4) gsys = 175;   // arbol
                else if (nb_text_eq_cstr(n->text, "CreateCanvas") && n->list_count == 4) gsys = 188;     // lienzo
                else if (nb_text_eq_cstr(n->text, "CreateSlider") && (n->list_count == 4 || n->list_count == 5)) {
                    gsys = 163; estilo_x3 = 1; if (n->list_count == 5) arg_vertical = 4;
                }
                // CreateScrollBar(x, y, ancho, alto [, vertical]) -- la misma
                // forma que el deslizador, y comparte con el SetSliderRange,
                // SetSliderValue y SliderValue. Lo que añade son las flechas
                // de los extremos y el salto de pagina.
                else if (nb_text_eq_cstr(n->text, "CreateScrollBar") && (n->list_count == 4 || n->list_count == 5)) {
                    gsys = 314; estilo_x3 = 1; if (n->list_count == 5) arg_vertical = 4;
                }
                if (gsys != 0) {
                    int32_t base = con_texto ? 1 : 0; // desplazamiento de x,y,ancho,alto
                    if (con_texto) {
                        // el texto va en x0 como puntero crudo
                        nb_emit_expr_string(ctx, n->list[0]);
                        nb_label_t *cstr_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                        if (cstr_fn) nb_emit_bl(cb, cstr_fn);
                        nb_push_x0(cb);
                    }
                    nb_emit_expr_int(ctx, n->list[base + 0]); nb_push_x0(cb);   // x
                    nb_emit_expr_int(ctx, n->list[base + 1]); nb_push_x0(cb);   // y
                    // ancho<<16 | alto, armado aqui
                    nb_emit_expr_int(ctx, n->list[base + 2]);
                    nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 0, 16));
                    nb_push_x0(cb);
                    nb_emit_expr_int(ctx, n->list[base + 3]);
                    nb_pop_x1(cb);
                    nb_codebuf_emit(cb, nb_enc_orr_reg(0, 1, 0));
                    // recuperar en orden inverso, cada uno a su registro
                    if (con_texto) {
                        nb_codebuf_emit(cb, nb_enc_orr_reg(3, NB_REG_ZR, 0)); // x3 = tamaño
                        nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16)); // x2 = y
                        nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16)); // x1 = x
                        nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16)); // x0 = texto
                    } else {
                        nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 0)); // x2 = tamaño
                        nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16)); // x1 = y
                        nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16)); // x0 = x
                    }
                    if (arg_vertical >= 0) {
                        // x3 = 1 + (vertical <> 0): 1 horizontal, 2 vertical. Se
                        // evalua con x0-x2 a salvo en la pila.
                        nb_push_x0(cb); nb_push_x1(cb);
                        nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 2)); nb_push_x1(cb);   // x2 a la pila
                        nb_emit_expr_int(ctx, n->list[arg_vertical]);
                        nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 0, NB_REG_ZR));        // cmp x0, 0
                        nb_codebuf_emit(cb, nb_enc_movz(3, 1, 0));
                        nb_codebuf_emit(cb, nb_enc_movz(9, 2, 0));
                        nb_codebuf_emit(cb, nb_enc_csel(3, 3, 9, NB_COND_EQ));                // x3 = (v == 0) ? 1 : 2
                        nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16));
                        nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));
                        nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));
                    } else if (!con_texto) {
                        nb_codebuf_emit(cb, nb_enc_movz(3, (uint16_t)estilo_x3, 0));          // x3 = estilo (deslizador) o 0
                    }
                    if (con_texto) nb_codebuf_emit(cb, nb_enc_movz(4, (uint16_t)estilo_x4, 0)); // x4 = estilo del boton
                    nb_emit_load_imm64(cb, 8, gsys);
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    return;
                }
            }

            // SetGadgetText(id, texto$) -- syscall 105. Devuelve 0;
            // se usa como sentencia suelta, igual que Pump().
            // La caja de texto de varias lineas usa sus propias
            // syscalls: SetTextAreaText (126) y AddTextAreaText (145). Mismo
            // patron que SetGadgetText: (id, texto$).
            if (n->list_count == 2 && (nb_text_eq_cstr(n->text, "SetTextAreaText") || nb_text_eq_cstr(n->text, "AddTextAreaText") ||
                                       nb_text_eq_cstr(n->text, "SetToolBarTips") || nb_text_eq_cstr(n->text, "SetPanelImage") ||
                                       nb_text_eq_cstr(n->text, "ModifyTreeViewNode"))) {
                // y SetToolBarTips (174: textos de ayuda separados
                // por comas) y SetPanelImage (222: una imagen NIMG de fondo)
                uint32_t num = nb_text_eq_cstr(n->text, "SetTextAreaText") ? 126 : nb_text_eq_cstr(n->text, "AddTextAreaText") ? 145
                             : nb_text_eq_cstr(n->text, "SetToolBarTips") ? 174 : nb_text_eq_cstr(n->text, "SetPanelImage") ? 222 : 179;
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);   // id
                nb_emit_expr_string(ctx, n->list[1]);
                nb_label_t *cstr_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr_fn) nb_emit_bl(cb, cstr_fn);
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0)); // x1 = texto
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16)); // x0 = id
                nb_emit_load_imm64(cb, 8, num);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // ---- Menus ----
            // m = WindowMenu()  -- 120: la barra de menus de la ventana (se crea
            // la primera vez); es el 'padre' de las entradas de primer nivel.
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "WindowMenu")) {
                nb_emit_load_imm64(cb, 8, 120);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // UpdateWindowMenu [ventana] -- en BlitzPlus hace falta tras crear los
            // menus; aqui el kernel los redibuja solo. Se acepta y no hace nada,
            // para que los programas de BlitzPlus compilen tal cual.
            if (n->list_count <= 1 && nb_text_eq_cstr(n->text, "UpdateWindowMenu")) {
                nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
                return;
            }
            // e = CreateMenu(texto$, etiqueta, padre) -- 121. La etiqueta es el
            // numero que da EventData() al elegirla (evento $1001 = 4097).
            // Texto "" = una linea de separacion.
            if (n->list_count == 3 && nb_text_eq_cstr(n->text, "CreateMenu")) {
                nb_emit_expr_string(ctx, n->list[0]);
                nb_label_t *cstr_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr_fn) nb_emit_bl(cb, cstr_fn);
                nb_push_x0(cb);                                          // texto
                nb_emit_expr_int(ctx, n->list[1]); nb_push_x0(cb);       // etiqueta
                nb_emit_expr_int(ctx, n->list[2]);                       // x0 = padre
                nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 0));    // x2 = padre
                nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));  // x1 = etiqueta
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));  // x0 = texto
                nb_emit_load_imm64(cb, 8, 121);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // m = CreateContextMenu() -- 315. Un menu flotante vacio. Las
            // entradas se le cuelgan con el mismo CreateMenu de arriba,
            // pasandole este id como padre.
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "CreateContextMenu")) {
                nb_emit_load_imm64(cb, 8, 315);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // ShowContextMenu m, x, y -- 316. Lo abre en ese punto del area
            // de contenido; lo normal es pasarle EventX() y EventY() del
            // clic derecho que lo pidio.
            if (n->list_count == 3 && nb_text_eq_cstr(n->text, "ShowContextMenu")) {
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);       // menu
                nb_emit_expr_int(ctx, n->list[1]); nb_push_x0(cb);       // x
                nb_emit_expr_int(ctx, n->list[2]);                       // x0 = y
                nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 0));    // x2 = y
                nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));  // x1 = x
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));  // x0 = menu
                nb_emit_load_imm64(cb, 8, 316);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // ---- Arbol ----
            // nodo = AddTreeViewNode(texto$, padre) -- 177
            if (n->list_count == 2 && nb_text_eq_cstr(n->text, "AddTreeViewNode")) {
                nb_emit_expr_string(ctx, n->list[0]);
                nb_label_t *cstr_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr_fn) nb_emit_bl(cb, cstr_fn);
                nb_push_x0(cb);                                          // texto
                nb_emit_expr_int(ctx, n->list[1]);                       // x0 = padre
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));    // x1 = padre
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));  // x0 = texto
                nb_emit_load_imm64(cb, 8, 177);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // nodo = InsertTreeViewNode(indice, texto$, padre) -- 178
            if (n->list_count == 3 && nb_text_eq_cstr(n->text, "InsertTreeViewNode")) {
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);       // indice
                nb_emit_expr_string(ctx, n->list[1]);
                nb_label_t *cstr_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr_fn) nb_emit_bl(cb, cstr_fn);
                nb_push_x0(cb);                                          // texto
                nb_emit_expr_int(ctx, n->list[2]);                       // x0 = padre
                nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 0));    // x2 = padre
                nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));  // x1 = texto
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));  // x0 = indice
                nb_emit_load_imm64(cb, 8, 178);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // ---- Lienzo ----
            // SetBuffer CanvasBuffer(lienzo): todo el dibujo (Rect, Oval, Line, Text,
            // Plot, DrawImage) va al lienzo, con sus coordenadas y recortado a el.
            // El kernel distingue un lienzo de una imagen porque suma 100000.
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "CanvasBuffer")) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_emit_load_imm64(cb, 1, 100000);
                nb_codebuf_emit(cb, nb_enc_add_reg(0, 0, 1, 0));
                return;
            }
            // ImageBuffer(imagen): SetBuffer lo recibe tal cual (el handle)
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "ImageBuffer")) {
                nb_emit_expr_int(ctx, n->list[0]);
                return;
            }
            // BackBuffer() / FrontBuffer(): volver a dibujar en la ventana (-1)
            if (n->list_count == 0 && (nb_text_eq_cstr(n->text, "BackBuffer") || nb_text_eq_cstr(n->text, "FrontBuffer"))) {
                nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
                nb_codebuf_emit(cb, nb_enc_sub_imm(0, 0, 1));            // x0 = -1
                return;
            }
            // FlipCanvas lienzo: en BlitzPlus muestra lo dibujado; aqui el dibujo va
            // directo al lienzo, asi que no hace nada (se acepta por compatibilidad)
            if (n->list_count <= 2 && nb_text_eq_cstr(n->text, "FlipCanvas")) {
                nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
                return;
            }

            // EnableToolBarItem / DisableToolBarItem id, n -- 173 (id, boton, 1/0)
            if (n->list_count == 2 && (nb_text_eq_cstr(n->text, "EnableToolBarItem") || nb_text_eq_cstr(n->text, "DisableToolBarItem"))) {
                uint32_t on = nb_text_eq_cstr(n->text, "EnableToolBarItem") ? 1 : 0;
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);
                nb_emit_expr_int(ctx, n->list[1]);
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));
                nb_codebuf_emit(cb, nb_enc_movz(2, (uint16_t)on, 0));
                nb_emit_load_imm64(cb, 8, 173);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // InsertGadgetItem / ModifyGadgetItem id, indice, texto$ (157 / 159)
            if (n->list_count == 3 && (nb_text_eq_cstr(n->text, "InsertGadgetItem") || nb_text_eq_cstr(n->text, "ModifyGadgetItem"))) {
                uint32_t num = nb_text_eq_cstr(n->text, "InsertGadgetItem") ? 157 : 159;
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);   // id
                nb_emit_expr_int(ctx, n->list[1]); nb_push_x0(cb);   // indice
                nb_emit_expr_string(ctx, n->list[2]);
                nb_label_t *cstr_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr_fn) nb_emit_bl(cb, cstr_fn);
                nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 0)); // x2 = texto
                nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16)); // x1 = indice
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16)); // x0 = id
                nb_emit_load_imm64(cb, 8, num);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // HideGadget / ShowGadget / DisableGadget / EnableGadget id: una
            // syscall con un valor fijo (mostrar 110, activar 111).
            {
                uint32_t num = 0, fijo = 0;
                if (n->list_count == 1) {
                    if (nb_text_eq_cstr(n->text, "HideGadget"))        { num = 110; fijo = 0; }
                    else if (nb_text_eq_cstr(n->text, "ShowGadget"))    { num = 110; fijo = 1; }
                    else if (nb_text_eq_cstr(n->text, "DisableGadget")) { num = 111; fijo = 0; }
                    else if (nb_text_eq_cstr(n->text, "EnableGadget"))  { num = 111; fijo = 1; }
                    else if (nb_text_eq_cstr(n->text, "CheckMenu"))     { num = 122; fijo = 1; }
                    else if (nb_text_eq_cstr(n->text, "UncheckMenu"))   { num = 122; fijo = 0; }
                    else if (nb_text_eq_cstr(n->text, "EnableMenu"))    { num = 123; fijo = 1; }
                    else if (nb_text_eq_cstr(n->text, "DisableMenu"))   { num = 123; fijo = 0; }
                    else if (nb_text_eq_cstr(n->text, "ExpandTreeViewNode"))   { num = 181; fijo = 1; }
                    else if (nb_text_eq_cstr(n->text, "CollapseTreeViewNode")) { num = 181; fijo = 0; }
                }
                if (num) {
                    nb_emit_expr_int(ctx, n->list[0]);
                    nb_codebuf_emit(cb, nb_enc_movz(1, (uint16_t)fijo, 0));
                    nb_emit_load_imm64(cb, 8, num);
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    return;
                }
            }
            // SetPanelColor panel, rojo, verde, azul -- syscall 207,
            // que recibe el color empaquetado: (r<<16) | (g<<8) | b. Aronnax
            // dibuja con esto los LED de sus controles GPIO.
            if (n->list_count == 4 && nb_text_eq_cstr(n->text, "SetPanelColor")) {
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);            // panel
                nb_emit_expr_int(ctx, n->list[1]); nb_push_x0(cb);            // rojo
                nb_emit_expr_int(ctx, n->list[2]); nb_push_x0(cb);            // verde
                nb_emit_expr_int(ctx, n->list[3]);                           // x0 = azul
                nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16));      // x2 = verde
                nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));      // x1 = rojo
                nb_codebuf_emit(cb, nb_enc_lsl_imm(1, 1, 16));
                nb_codebuf_emit(cb, nb_enc_lsl_imm(2, 2, 8));
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, 1, 2));
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, 1, 0));                // x1 = (r<<16)|(g<<8)|b
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));      // x0 = panel
                nb_emit_load_imm64(cb, 8, 207);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // UpdateProgBar id, valor# (0.0 a 1.0, como en BlitzPlus). El
            // kernel cuenta POR MIL (0-1000): x1 = Int(valor# * 1000).
            if (n->list_count == 2 && nb_text_eq_cstr(n->text, "UpdateProgBar")) {
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);            // id
                nb_emit_expr_as_float(ctx, n->list[1]);                      // d0 = valor (tambien si es entero)
                union { double d; uint64_t u; } mil = { 1000.0 };
                nb_emit_load_imm64(cb, 9, mil.u);
                nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(1, 9));           // d1 = 1000.0
                nb_codebuf_emit(cb, nb_enc_fmul(0, 0, 1));                   // d0 = valor * 1000
                nb_codebuf_emit(cb, nb_enc_fcvtzs(1, 0));                    // x1 = entero
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));      // x0 = id
                nb_emit_load_imm64(cb, 8, 162);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            if (n->list_count == 2 && nb_text_eq_cstr(n->text, "SetGadgetText")) {
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);   // id
                nb_emit_expr_string(ctx, n->list[1]);
                nb_label_t *cstr_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr_fn) nb_emit_bl(cb, cstr_fn);
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0)); // x1 = texto
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16)); // x0 = id
                nb_emit_load_imm64(cb, 8, 105);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "FreeGadget")) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_emit_load_imm64(cb, 8, 104);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }

            // ---- Gadgets: bucle de eventos ----
            //
            //   WaitEvent()    -> espera a que pase algo (8, cediendo turno)
            //   EventSource()  -> que gadget lo disparo (9, mitad alta)
            //   EventData()    -> dato asociado           (9, mitad baja)
            //
            // Codigos de evento utiles (de gadgets.h):
            //   $401  un gadget se activo      $802  la ventana cambio de tamaño
            //   $803  se pulso la X de cerrar  $1001 se eligio un menu
            // PollEvent() -- version NO BLOQUEANTE de WaitEvent:
            // llama a SYS_POLL_EVENT una sola vez y devuelve 0 si no
            // habia ningun evento pendiente. Imprescindible en bucles
            // de juego donde el programa no puede pararse a esperar.
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "PollEvent")) {
                nb_emit_load_imm64(cb, 8, 8);   // SYS_POLL_EVENT
                nb_codebuf_emit(cb, nb_enc_svc(0)); // x0 = evento o 0
                return;
            }
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "WaitEvent")) {
                // Bucle: preguntar por un evento y, si no hay ninguno,
                // ceder el turno y volver a preguntar. Sin el Pump el
                // planificador cooperativo se quedaria aqui clavado y
                // colgaria el sistema entero.
                nb_label_t *l_otra = nb_label_new();
                nb_label_define(cb, l_otra);
                nb_emit_load_imm64(cb, 8, 8);            // SYS_POLL_EVENT
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0));
                nb_label_t *l_hay = nb_label_new();
                nb_emit_bcond(cb, NB_COND_NE, l_hay);
                nb_push_x0(cb);
                nb_emit_load_imm64(cb, 8, 14);           // SYS_PUMP
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_pop_x1(cb);
                nb_emit_b(cb, l_otra);
                nb_label_define(cb, l_hay);
                nb_label_free(l_otra); nb_label_free(l_hay);
                return;
            }
            if (n->list_count == 0 && (nb_text_eq_cstr(n->text, "EventSource") ||
                                       nb_text_eq_cstr(n->text, "EventData"))) {
                nb_emit_load_imm64(cb, 8, 9);            // SYS_GET_EVENT_INFO
                nb_codebuf_emit(cb, nb_enc_svc(0));
                if (nb_text_eq_cstr(n->text, "EventSource")) {
                    nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, 32));  // mitad alta
                } else {
                    nb_codebuf_emit(cb, nb_enc_and_imm(0, 0, 0xFFFFFFFFu)); // mitad baja
                }
                return;
            }

            // ---- Escribir archivos ----
            //
            //   WriteFile(nombre$, contenido$) -> 0 si ok, -1 si no
            //
            // Escribe el archivo ENTERO de una vez, no linea a linea:
            // asi funciona la capa de archivos por debajo (en FAT, la
            // escritura borra la entrada vieja y la recrea). Para ir
            // anadiendo texto, el programa construye la cadena entera
            // y la escribe al final.
            //
            // El par es: OpenFile/ReadLine$/Eof/CloseFile para LEER
            // (linea a linea) y WriteFile para ESCRIBIR (de golpe).
            if (nb_text_eq_cstr(n->text, "WriteFile") && n->list_count == 2) {
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                nb_label_t *slen = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_len", n->line) : NULL;

                // abrir (o crear) el archivo: SYS_FILE_OPEN(nombre, raiz, NemoFS)
                nb_emit_expr_string(ctx, n->list[0]);
                if (cstr) nb_emit_bl(cb, cstr);
                nb_codebuf_emit(cb, nb_enc_movz(1, 0, 0));   // carpeta raiz
                nb_codebuf_emit(cb, nb_enc_movz(2, 0, 0));   // VOLUME_NEMOFS
                nb_emit_load_imm64(cb, 8, 20);               // SYS_FILE_OPEN
                nb_codebuf_emit(cb, nb_enc_svc(0));          // x0 = identificador
                nb_push_x0(cb);

                // El contenido: hacen falta el PUNTERO al texto y su
                // LONGITUD, y para cada uno hay que llamar a una
                // funcion del runtime. La longitud NO puede quedarse en
                // un registro mientras se llama a nb_string_cstr:
                // x1-x7 los puede usar cualquier funcion. Va a la pila,
                // como todo lo que deba sobrevivir a una llamada.
                //
                // Pila mientras tanto (de arriba a abajo):
                //   [sp]      longitud
                //   [sp+16]   la cadena
                //   [sp+32]   el identificador del archivo
                nb_emit_expr_string(ctx, n->list[1]);
                nb_push_x0(cb);                                        // la cadena
                if (slen) nb_emit_bl(cb, slen);                        // x0 = longitud
                nb_push_x0(cb);                                        // la longitud
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 2));  // x0 = la cadena
                if (cstr) nb_emit_bl(cb, cstr);                        // x0 = texto crudo
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));  // x1 = texto
                nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16)); // x2 = longitud
                // soltar la cadena con un ldr normal (a xzr, que
                // descarta el valor) en vez de mover sp a mano: asi la
                // cuenta de push/pop cuadra y la prueba de equilibrio
                // de pila sigue sirviendo para detectar errores de
                // verdad.
                nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16));
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16)); // x0 = identificador
                nb_codebuf_emit(cb, nb_enc_movz(3, 0, 0));             // VOLUME_NEMOFS
                nb_emit_load_imm64(cb, 8, 22);                         // SYS_FILE_WRITE
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }

            // ---- Imagenes ----
            //
            //   LoadImage(archivo$)        -> handle, o -1 (49)
            //   CreateImage(ancho, alto)   -> lienzo vacio (52)
            //   DrawImage(handle, x, y)    -> pintar (50)
            //   ImageWidth/ImageHeight(h)  -> tamano, del mismo syscall 51
            //   MaskImage(handle, color)   -> ese color pasa a transparente (92)
            //   HandleImage(handle, x, y)  -> punto de agarre (89)
            //   FreeImage(handle)          -> liberar (88)
            //   CopyImage(handle)          -> copia independiente (93)
            //   SaveImage(handle, archivo$) -> guardar (94)
            //
            // El punto de agarre es el pixel de la imagen que cae
            // sobre las coordenadas de DrawImage. Por defecto es la
            // esquina superior izquierda; ponerlo en el centro es lo
            // normal para un sprite que gira o que se centra en una
            // posicion.
            if (nb_text_eq_cstr(n->text, "LoadImage") && n->list_count == 1) {
                nb_emit_expr_string(ctx, n->list[0]);
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr) nb_emit_bl(cb, cstr);
                nb_emit_load_imm64(cb, 8, 49);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            if (nb_text_eq_cstr(n->text, "SaveImage") && n->list_count == 2) {
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);
                nb_emit_expr_string(ctx, n->list[1]);
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr) nb_emit_bl(cb, cstr);
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));
                nb_pop_x0(cb);
                nb_emit_load_imm64(cb, 8, 94);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }

            // ---- Sonido ----
            //
            //   LoadSound(archivo$)          -> handle, o -1 (226)
            //   FreeSound handle             -> liberar (227)      [bloque de 1 entero]
            //   PlaySound handle             -> suena (228)        [bloque de 1 entero]
            //   SoundVolume handle, v#       -> 0.0 a 1.0 (229)
            //   SoundPan handle, p#          -> -1.0 a 1.0 (230)
            //   SoundPitch handle, hercios   -> (231)              [bloque de 2 enteros]
            //
            // El archivo es un WAV. Los nombres son los de Blitz, que es de
            // donde viene este lenguaje.
            //
            // LO QUE HAY QUE SABER ANTES DE USARLO: PlaySound BLOQUEA hasta
            // que el sonido termina de sonar -- el driver de la Pi 4 llena la
            // FIFO del PWM a mano, sin DMA todavia. Mientras suena, ese
            // programa no avanza. Asi que un efecto corto va bien y una
            // musica de fondo no: para eso hace falta el DMA, que es el paso
            // siguiente del driver.
            //
            // Y por lo mismo, SoundVolume, SoundPan y SoundPitch se guardan en
            // el handle pero NO cambian nada de un sonido que ya esta
            // sonando: cuando PlaySound vuelve, ya no hay nada que ajustar.
            // Se ponen ANTES de PlaySound (ver la nota de sound.c sobre lo que
            // haria falta para polifonia de verdad).
            if (nb_text_eq_cstr(n->text, "LoadSound") && n->list_count == 1) {
                nb_emit_expr_string(ctx, n->list[0]);
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr) nb_emit_bl(cb, cstr);
                nb_emit_load_imm64(cb, 8, 226);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // SoundVolume y SoundPan llevan un DECIMAL y el kernel cuenta POR
            // MIL, igual que UpdateProgBar: x1 = Int(valor# * 1000). La
            // conversion se hace aqui, con coma flotante de verdad, para que
            // el kernel no tenga que tocar decimales.
            if (n->list_count == 2 && (nb_text_eq_cstr(n->text, "SoundVolume") ||
                                       nb_text_eq_cstr(n->text, "SoundPan"))) {
                uint32_t num = nb_text_eq_cstr(n->text, "SoundVolume") ? 229 : 230;
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);            // handle
                nb_emit_expr_as_float(ctx, n->list[1]);                       // d0 = valor (tambien si es entero)
                union { double d; uint64_t u; } mil = { 1000.0 };
                nb_emit_load_imm64(cb, 9, mil.u);
                nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(1, 9));            // d1 = 1000.0
                nb_codebuf_emit(cb, nb_enc_fmul(0, 0, 1));                    // d0 = valor * 1000
                nb_codebuf_emit(cb, nb_enc_fcvtzs(1, 0));                     // x1 = entero
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));       // x0 = handle
                nb_emit_load_imm64(cb, 8, num);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // ---- Datos del sistema: un comando por dato ----
            //
            // Varias de estas syscalls devuelven DOS datos metidos en un solo
            // numero de 64 bits. En vez de dar el numero crudo y que cada
            // programa lo parta con Shr y And, cada dato tiene su comando y el
            // desempaquetado lo emite el compilador: un lsr, una mascara, y ya.
            //
            //   desp  bits a desplazar a la derecha
            //   bits  cuantos se quedan despues (0 = todos, sin mascara)
            //   kb    el kernel lo da en KILOBYTES y hay que pasarlo a bytes
            //   arg   lleva un indice (particion)
            //
            // DOS TRAMPAS, comprobadas contra nemo_sistema.lua, que es el
            // envoltorio ya probado en la maquina:
            //   - la 250 empaqueta AL REVES que las demas: total arriba, usado
            //     abajo. Las otras son (usado << 32) | total.
            //   - la 260 da KILOBYTES, no bytes: la reserva de tareas puede
            //     pasar de 4 GB y en bytes no le cabria en 32 bits.
            //
            // NOMBRES COMO ARRAYS, NO PUNTEROS: la misma regla que nb_firmas --
            // nbc.pro se enlaza en la direccion 0 y el cargador de Nemo OS no
            // reubica los punteros de datos.
            {
                typedef struct {
                    char nombre[20];
                    uint16_t sys; uint8_t desp; uint8_t bits; uint8_t kb; uint8_t arg;
                } nb_dato_sis_t;
                static const nb_dato_sis_t datos[] = {
                    // el equipo
                    { "CpuCores",         286, 32,  0, 0, 0 },
                    { "CpuMHz",           286,  0, 32, 0, 0 },   // 0 = no se pudo saber (QEMU)
                    { "TotalRam",         287,  0,  0, 0, 0 },   // bytes de RAM fisica
                    { "ScreenWidth",       35, 32,  0, 0, 0 },   // la PANTALLA entera, no la ventana
                    { "ScreenHeight",      35,  0, 32, 0, 0 },
                    // la memoria
                    { "KernelRam",        288,  0,  0, 0, 0 },
                    { "TaskRamUsed",      260, 32,  0, 1, 0 },
                    { "TaskRamTotal",     260,  0, 32, 1, 0 },
                    { "KernelHeapUsed",   252, 32,  0, 0, 0 },
                    { "KernelHeapTotal",  252,  0, 32, 0, 0 },
                    // el disco: bloques de 512 bytes
                    { "DiskTotalBlocks",  250, 32,  0, 0, 0 },
                    { "DiskUsedBlocks",   250,  0, 32, 0, 0 },
                    { "CardSectors",      279,  0,  0, 0, 0 },
                    { "PartitionType",    280, 56,  0, 0, 1 },
                    { "PartitionStart",   280, 28, 28, 0, 1 },
                    { "PartitionSectors", 280,  0, 28, 0, 1 },
                    // las tareas: cuantas vivas y cuantos huecos hay
                    { "TaskCount",        251, 32,  0, 0, 0 },
                    { "TaskSlots",        251,  0, 32, 0, 0 },
                    // medidas del texto, para colocar cursores y alinear
                    { "FontHeight",       196,  0,  0, 0, 0 },
                    { "FontCharWidth",    206,  0,  0, 0, 0 },
                    // el reloj, todo de la misma syscall
                    { "Year",             134, 48,  0, 0, 0 },
                    { "Month",            134, 40,  8, 0, 0 },
                    { "Day",              134, 32,  8, 0, 0 },
                    { "Hour",             134, 24,  8, 0, 0 },
                    { "Minute",           134, 16,  8, 0, 0 },
                    { "Second",           134,  8,  8, 0, 0 },
                };
                for (uint32_t i = 0; i < (uint32_t)(sizeof(datos) / sizeof(datos[0])); i++) {
                    if (!nb_text_eq_cstr(n->text, datos[i].nombre)) continue;
                    if (n->list_count != (int32_t)datos[i].arg) continue;
                    if (datos[i].arg == 1) nb_emit_expr_int(ctx, n->list[0]);
                    nb_emit_load_imm64(cb, 8, datos[i].sys);
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    if (datos[i].desp) nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, datos[i].desp));
                    if (datos[i].bits) nb_codebuf_emit(cb, nb_enc_and_imm(0, 0, (1ULL << datos[i].bits) - 1ULL));
                    if (datos[i].kb) nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 0, 10));  // KB -> bytes
                    return;
                }
            }
            // TextWidth(texto$) -> lo que ocupa en pixeles con la fuente
            // activa. Sin esto no hay forma de centrar nada ni de saber donde
            // cae el cursor dentro de una linea.
            if (nb_text_eq_cstr(n->text, "TextWidth") && n->list_count == 1) {
                nb_emit_expr_string(ctx, n->list[0]);
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr) nb_emit_bl(cb, cstr);
                nb_emit_load_imm64(cb, 8, 254);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }

            // ---- TextAreaText$(id): todo el contenido, como cadena ----
            //
            // Es la pieza que faltaba para poder GUARDAR lo que el usuario ha
            // escrito. La syscall 149 escribe en un buffer que da el programa,
            // asi que hay tres pasos y todo vive en la pila, porque cada
            // llamada al runtime puede pisar cualquier registro:
            //
            //   1. la longitud, con la 146 (unidades = 1, caracteres)
            //   2. un buffer de longitud+1, con nb_alloc
            //   3. la 149 lo rellena, y nb_string_new lo convierte en cadena
            //
            // El buffer se pide del tamaño EXACTO que hace falta: un tamaño
            // fijo cortaria los archivos grandes sin avisar, que es la clase
            // de fallo que aparece el dia que de verdad importa.
            //
            // La pila, de arriba abajo, segun se va llenando: buffer, longitud,
            // id. nb_push_x0 mueve el puntero de 16 en 16, asi que el hueco
            // numero k se lee con nb_enc_ldr_imm(..., 2*k) -- ese inmediato va
            // escalado de 8 en 8.
            if (nb_text_eq_cstr(n->text, "TextAreaText$") && n->list_count == 1) {
                nb_emit_expr_int(ctx, n->list[0]);                      // x0 = id
                nb_push_x0(cb);                                         // pila: id
                nb_codebuf_emit(cb, nb_enc_movz(1, 1, 0));              // x1 = 1 (caracteres)
                nb_emit_load_imm64(cb, 8, 146);
                nb_codebuf_emit(cb, nb_enc_svc(0));                     // x0 = longitud
                nb_push_x0(cb);                                         // pila: longitud, id

                nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 1));           // x0 = longitud + 1
                nb_label_t *alloc_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_alloc", n->line) : NULL;
                if (alloc_fn) nb_emit_bl(cb, alloc_fn);                 // x0 = buffer
                nb_push_x0(cb);                                         // pila: buffer, longitud, id

                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 4));   // x0 = id
                nb_codebuf_emit(cb, nb_enc_movz(1, 0, 0));              // x1 = desde el principio
                nb_codebuf_emit(cb, nb_enc_ldr_imm(2, NB_REG_SP, 2));   // x2 = cuantos = longitud
                nb_codebuf_emit(cb, nb_enc_ldr_imm(3, NB_REG_SP, 0));   // x3 = buffer
                nb_codebuf_emit(cb, nb_enc_add_imm(4, 2, 1));           // x4 = tamaño del buffer
                nb_emit_load_imm64(cb, 8, 149);
                nb_codebuf_emit(cb, nb_enc_svc(0));

                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0));   // x0 = buffer
                nb_codebuf_emit(cb, nb_enc_ldr_imm(1, NB_REG_SP, 2));   // x1 = longitud
                nb_label_t *new_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                if (new_fn) nb_emit_bl(cb, new_fn);                     // x0 = la cadena
                nb_push_x0(cb);                                         // pila: cadena, buffer, longitud, id

                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 2));   // x0 = buffer
                nb_label_t *free_fn3 = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_free", n->line) : NULL;
                if (free_fn3) nb_emit_bl(cb, free_fn3);

                nb_pop_x0(cb);                                          // x0 = la cadena
                nb_pop_x1(cb);                                          // y a tirar: buffer,
                nb_pop_x1(cb);                                          // longitud
                nb_pop_x1(cb);                                          // e id
                return;
            }

            // ---- Los dos datos del sistema que son TEXTO ----
            //
            // CpuName$() y TaskName$(indice). Las dos syscalls escriben en un
            // buffer que da el programa, asi que hay que reservarlo, llamar, y
            // convertir lo escrito en una cadena de Nemo Basic -- el mismo baile
            // que I2cRead$, con sus tres llamadas al runtime.
            //
            // 64 bytes de buffer: el nombre del procesador no llega a eso, y el
            // de una tarea son 32 como maximo (el campo de SYS_TASK_LIST).
            // Si la syscall falla devuelve negativo, y eso se convierte en cero
            // letras -- o sea la cadena vacia, no una cadena de basura.
            if ((nb_text_eq_cstr(n->text, "CpuName$") && n->list_count == 0) ||
                (nb_text_eq_cstr(n->text, "TaskName$") && n->list_count == 1)) {
                bool de_tarea = nb_text_eq_cstr(n->text, "TaskName$");
                nb_codebuf_emit(cb, nb_enc_movz(0, 64, 0));
                nb_label_t *alloc_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_alloc", n->line) : NULL;
                if (alloc_fn) nb_emit_bl(cb, alloc_fn);
                nb_push_x0(cb);                                         // pila: buffer
                if (de_tarea) {
                    nb_emit_expr_int(ctx, n->list[0]);                  // x0 = indice
                    nb_codebuf_emit(cb, nb_enc_ldr_imm(1, NB_REG_SP, 0)); // x1 = buffer (sigue en la pila)
                    nb_codebuf_emit(cb, nb_enc_movz(2, 64, 0));         // x2 = tamaño maximo
                    nb_emit_load_imm64(cb, 8, 295);
                } else {
                    nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0)); // x0 = buffer
                    nb_codebuf_emit(cb, nb_enc_movz(1, 64, 0));         // x1 = tamaño maximo
                    nb_emit_load_imm64(cb, 8, 285);
                }
                nb_codebuf_emit(cb, nb_enc_svc(0));                     // x0 = letras escritas, o negativo
                nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 0, NB_REG_ZR));   // cmp x0, 0
                nb_codebuf_emit(cb, nb_enc_csel(0, NB_REG_ZR, 0, NB_COND_LT));   // si fallo -> 0 letras
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));   // x1 = longitud
                nb_pop_x0(cb);                                          // x0 = buffer
                nb_push_x0(cb);                                         // pila: buffer (para liberarlo)
                nb_label_t *new_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                if (new_fn) nb_emit_bl(cb, new_fn);                     // x0 = la cadena
                nb_pop_x1(cb);                                          // x1 = buffer
                nb_push_x0(cb);                                         // pila: la cadena
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));   // x0 = buffer
                nb_label_t *free_fn2 = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_free", n->line) : NULL;
                if (free_fn2) nb_emit_bl(cb, free_fn2);
                nb_pop_x0(cb);                                          // x0 = la cadena
                return;
            }
            // Un dato de UNA tarea, por indice: el campo va en x1 (0=slot,
            // 1=ventana, 2=turnos; ver SYS_TASK_FIELD en syscall.h). La ventana
            // puede ser -1 de verdad -- una tarea sin ventana --, asi que esto
            // NO se puede tratar como "sin signo".
            {
                int campo = -1;
                if (nb_text_eq_cstr(n->text, "TaskSlot")) campo = 0;
                else if (nb_text_eq_cstr(n->text, "TaskWindow")) campo = 1;
                else if (nb_text_eq_cstr(n->text, "TaskTurns")) campo = 2;
                if (campo >= 0 && n->list_count == 1) {
                    nb_emit_expr_int(ctx, n->list[0]);                       // x0 = indice
                    nb_codebuf_emit(cb, nb_enc_movz(1, (uint32_t)campo, 0)); // x1 = campo
                    nb_emit_load_imm64(cb, 8, 294);
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    return;
                }
            }
            // LoadAnimImage(archivo$, ancho, alto, primera, cuantas) -> 99
            // Carga una HOJA de celdas iguales: un tileset, o los fotogramas
            // de un personaje. Devuelve un solo handle -- imprescindible,
            // porque el kernel solo tiene 16 huecos de imagen.
            if (nb_text_eq_cstr(n->text, "LoadAnimImage") && n->list_count == 5) {
                nb_emit_expr_string(ctx, n->list[0]);
                nb_label_t *cstr_a = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr_a) nb_emit_bl(cb, cstr_a);
                nb_push_x0(cb);                                   // el nombre, A LA PILA:
                                                                  // tiene que sobrevivir a las
                                                                  // evaluaciones que vienen
                nb_emit_expr_int(ctx, n->list[1]);
                nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 0, 16));
                nb_push_x0(cb);
                nb_emit_expr_int(ctx, n->list[2]);
                nb_pop_x1(cb);
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, 1, 0));     // x0 = ancho<<16 | alto
                nb_push_x0(cb);
                nb_emit_expr_int(ctx, n->list[3]); nb_push_x0(cb);
                nb_emit_expr_int(ctx, n->list[4]);
                nb_codebuf_emit(cb, nb_enc_orr_reg(3, NB_REG_ZR, 0));    // x3 = cuantas
                nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16));  // x2 = primera
                nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));  // x1 = tamaño de celda
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));  // x0 = nombre
                nb_emit_load_imm64(cb, 8, 99);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // DrawImage imagen, x, y, fotograma  (la forma de 3 sigue su camino
            // de siempre, mas abajo, para no cambiar ni un byte de lo ya escrito)
            // DrawBlock imagen, x, y [, fotograma] -- copia OPACA: ignora alfa y
            // mascara, que es lo que quieres para un fondo (mas rapido y sin
            // mezclar con lo que hubiera debajo). El bit 31 de x3 lo indica.
            {
                bool es_block = nb_text_eq_cstr(n->text, "DrawBlock");
                bool es_image = nb_text_eq_cstr(n->text, "DrawImage");
                if ((es_block && (n->list_count == 3 || n->list_count == 4)) ||
                    (es_image && n->list_count == 4)) {
                    for (int32_t i = 0; i < 3; i++) { nb_emit_expr_int(ctx, n->list[i]); nb_push_x0(cb); }
                    if (n->list_count == 4) nb_emit_expr_int(ctx, n->list[3]);
                    else nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
                    if (es_block) {
                        nb_codebuf_emit(cb, nb_enc_movz(9, 0x8000, 1));   // x9 = 0x80000000
                        nb_codebuf_emit(cb, nb_enc_orr_reg(0, 0, 9));
                    }
                    nb_codebuf_emit(cb, nb_enc_orr_reg(3, NB_REG_ZR, 0));    // x3 = fotograma (+ bit opaco)
                    nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16));  // x2 = y
                    nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));  // x1 = x
                    nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));  // x0 = imagen
                    nb_emit_load_imm64(cb, 8, 50);
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    return;
                }
            }
            {
                // Las de dos argumentos enteros
                uint32_t sys2 = 0;
                if (nb_text_eq_cstr(n->text, "CreateImage") && n->list_count == 2) sys2 = 52;
                else if (nb_text_eq_cstr(n->text, "MaskImage") && n->list_count == 2) sys2 = 92;
                // GPIO: pines 2..27 del conector. GpioMode pin, modo
                // (0 entrada, 1 salida, 2 entrada a positivo, 3 a masa) y
                // GpioWrite pin, valor. Devuelven 0, o un error negativo.
                else if (nb_text_eq_cstr(n->text, "GpioMode") && n->list_count == 2) sys2 = 264;
                else if (nb_text_eq_cstr(n->text, "GpioWrite") && n->list_count == 2) sys2 = 265;
                // controles
                else if (nb_text_eq_cstr(n->text, "SetButtonState") && n->list_count == 2) sys2 = 142;
                else if (nb_text_eq_cstr(n->text, "SetSliderValue") && n->list_count == 2) sys2 = 165;
                else if (nb_text_eq_cstr(n->text, "RemoveGadgetItem") && n->list_count == 2) sys2 = 158;
                // sonido: SoundPitch handle, hercios (ver el bloque de sonido,
                // mas arriba). Los hercios son enteros, asi que entra aqui;
                // SoundVolume y SoundPan llevan decimal y van por su camino.
                else if (nb_text_eq_cstr(n->text, "SoundPitch") && n->list_count == 2) sys2 = 231;
                // leer un TextArea: hasta ahora Nemo Basic sabia ESCRIBIR en
                // uno pero no leerlo, asi que un programa podia abrir un
                // archivo y no podia guardarlo.
                else if (nb_text_eq_cstr(n->text, "TextAreaLen") && n->list_count == 2) sys2 = 146;
                else if (nb_text_eq_cstr(n->text, "TextAreaLineLen") && n->list_count == 2) sys2 = 147;
                else if (nb_text_eq_cstr(n->text, "TextAreaLineOfChar") && n->list_count == 2) sys2 = 148;
                // Grupos de opciones: las casillas de un mismo grupo se
                // apagan entre ellas. Sin esto, dos CreateRadio en la misma
                // ventana se quedan las dos encendidas y no hay forma de
                // decir cual manda.
                else if (nb_text_eq_cstr(n->text, "SetGadgetGroup") && n->list_count == 2) sys2 = 223;
                if (sys2 != 0) {
                    nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);
                    nb_emit_expr_int(ctx, n->list[1]);
                    nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));
                    nb_pop_x0(cb);
                    nb_emit_load_imm64(cb, 8, sys2);
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    return;
                }
                // Las de tres argumentos enteros
                uint32_t sys3 = 0;
                if (nb_text_eq_cstr(n->text, "DrawImage") && n->list_count == 3) sys3 = 50;
                else if (nb_text_eq_cstr(n->text, "HandleImage") && n->list_count == 3) sys3 = 89;
                // GpioPwm pin, hz, diezmilesimas: PWM por hardware, pines 12, 13, 18, 19
                else if (nb_text_eq_cstr(n->text, "GpioPwm") && n->list_count == 3) sys3 = 267;
                // WindowButtons maximizar, minimizar, cerrar: 1 = se ve,
                // 0 = oculto. Un juego a pantalla completa puede quitarlos todos
                // (y ofrecer su propia salida, con Esc).
                else if (nb_text_eq_cstr(n->text, "WindowButtons") && n->list_count == 3) sys3 = 271;
                else if (nb_text_eq_cstr(n->text, "SetSliderRange") && n->list_count == 3) sys3 = 164;   // id, visible, total
                if (sys3 != 0) {
                    for (int32_t i = 0; i < 3; i++) { nb_emit_expr_int(ctx, n->list[i]); nb_push_x0(cb); }
                    for (int32_t i = 2; i >= 0; i--) nb_codebuf_emit(cb, nb_enc_ldr_post(i, NB_REG_SP, 16));
                    nb_emit_load_imm64(cb, 8, sys3);
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    return;
                }
                // Las de CINCO argumentos enteros cuyo primero es una
                // IMAGEN.
                //
                //   FillRow imagen, x, y, cuantos, color
                //
                // Pinta una fila de un solo color DENTRO de una imagen, en
                // una sola llamada. Una llamada al sistema cuesta 3,5 us
                // medidos en la Pi 4 y un pixel 4 ns, asi que pintar una
                // fila de 256 pixeles con Plot son 256 peajes y con esto
                // uno. Ver la nota de SYS_FILA_LEER en syscall.h.
                //
                // El +1 del primer argumento NO es un adorno: la syscall
                // sigue la convencion de ImageBuffer, donde el 0 esta
                // reservado, y el hueco de imagen 0 es una imagen
                // PERFECTAMENTE VALIDA. Sin el +1, la primera imagen que
                // carga un programa seria justo la que no se puede pintar.
                uint32_t sys5img = 0;
                if (nb_text_eq_cstr(n->text, "FillRow") && n->list_count == 5) sys5img = 291;
                if (sys5img != 0) {
                    for (int32_t i = 0; i < 5; i++) {
                        nb_emit_expr_int(ctx, n->list[i]);
                        if (i == 0) nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 1));   // imagen -> imagen+1
                        nb_push_x0(cb);
                    }
                    for (int32_t i = 4; i >= 0; i--) nb_codebuf_emit(cb, nb_enc_ldr_post(i, NB_REG_SP, 16));
                    nb_emit_load_imm64(cb, 8, sys5img);
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    return;
                }
                // ReadRow / WriteRow imagen, x, y, cuantos, array
                //
                // Traen una fila de pixeles a un array de enteros, o la
                // llevan del array a la imagen, en UNA llamada. Aqui si
                // merece la pena traerse los pixeles al programa: Nemo Basic
                // compila a ARM64 nativo, asi que recorrer el array cuesta
                // nanosegundos por elemento. (En Lua no: alli cada lectura
                // del buffer cuesta ~210 ns, del mismo orden que la propia
                // syscall, y por eso alli se usa RowRun y no ReadRow.)
                //
                // El quinto argumento es un ARRAY declarado con Dim, no una
                // expresion. La firma de la tabla es "NNNN" (cuatro letras)
                // a proposito: las posiciones que pasan de la firma no se
                // comprueban, asi que el quinto se valida aqui, donde se
                // sabe que nombres son arrays.
                //
                // DOS COSAS QUE NO SON ADORNO:
                //
                // 1. x5 = 8. Un elemento de un array de Nemo Basic ocupa 8
                //    BYTES (el generador indexa con lsl #3), no 4. Sin
                //    decirselo, el kernel dejaria los pixeles apretados de 4
                //    en 4 y el programa leeria dos pixeles en un elemento y
                //    basura en el siguiente, sin ningun aviso.
                //
                // 2. 'cuantos' se ACOTA al tamaño del array. El kernel
                //    comprueba que el buffer sea de la tarea, no que quepa en
                //    el array: un ReadRow de 500 sobre un Dim a(10) escribiria
                //    490 enteros por encima del array, encima de las demas
                //    variables del programa, y pasaria la validacion. Se
                //    compara CON SIGNO para que un 'cuantos' negativo siga
                //    siendo negativo y el kernel devuelva 0, en vez de
                //    convertirse en un numero enorme y leer todo el array.
                {
                    bool es_read = nb_text_eq_cstr(n->text, "ReadRow");
                    bool es_write = nb_text_eq_cstr(n->text, "WriteRow");
                    if ((es_read || es_write) && n->list_count == 5) {
                        // Un nombre suelto llega como N_VAR; si el parser no
                        // pudo distinguirlo de una llamada, como N_CALL sin
                        // argumentos. Valen los dos: lo que decide es que el
                        // nombre este en la lista de arrays declarados.
                        nb_node_t *arr = n->list[4];
                        if (!((arr->kind == N_VAR || (arr->kind == N_CALL && arr->list_count == 0)) &&
                              arr->text && ctx->arrays &&
                              nb_array_set_contains(ctx->arrays, arr->text))) {
                            nb_codegen_error(es_read
                                ? "ReadRow necesita como quinto argumento un array declarado con Dim"
                                : "WriteRow necesita como quinto argumento un array declarado con Dim", n->line);
                            return;
                        }
                        for (int32_t i = 0; i < 4; i++) {
                            nb_emit_expr_int(ctx, n->list[i]);
                            if (i == 0) nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 1));   // imagen -> imagen+1
                            nb_push_x0(cb);
                        }
                        nb_emit_load_var_x0(ctx, arr->text);                   // x0 = bloque del array
                        if (ctx->bounds_error_label) { nb_emit_si_cero_error(cb, 0, g_err_dim); g_usa_dim = true; }
                        nb_codebuf_emit(cb, nb_enc_ldr_imm(9, 0, 1));          // x9 = dim0_size  ([x0+8])
                        nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 16));         // x0 = primer elemento (cabecera 1D)
                        nb_codebuf_emit(cb, nb_enc_orr_reg(4, NB_REG_ZR, 0));  // x4 = puntero al array
                        nb_codebuf_emit(cb, nb_enc_ldr_post(3, NB_REG_SP, 16));// x3 = cuantos
                        nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16));// x2 = y
                        nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));// x1 = x
                        nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));// x0 = imagen+1
                        nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 3, 9)); // cmp cuantos, dim0_size
                        nb_codebuf_emit(cb, nb_enc_csel(3, 3, 9, NB_COND_LT)); // x3 = menor de los dos (con signo)
                        nb_codebuf_emit(cb, nb_enc_movz(5, 8, 0));             // x5 = 8 bytes por elemento
                        nb_emit_load_imm64(cb, 8, es_read ? 289 : 290);
                        nb_codebuf_emit(cb, nb_enc_svc(0));
                        return;
                    }
                }
                // Las de un solo argumento entero
                uint32_t sys1 = 0;
                if (nb_text_eq_cstr(n->text, "FreeImage") && n->list_count == 1) sys1 = 88;
                else if (nb_text_eq_cstr(n->text, "CopyImage") && n->list_count == 1) sys1 = 93;
                // sonido (ver el bloque de sonido, mas arriba). PlaySound
                // devuelve el "canal", que hoy es el mismo handle.
                else if (nb_text_eq_cstr(n->text, "FreeSound") && n->list_count == 1) sys1 = 227;
                else if (nb_text_eq_cstr(n->text, "PlaySound") && n->list_count == 1) sys1 = 228;
                else if (nb_text_eq_cstr(n->text, "GpioRead") && n->list_count == 1) sys1 = 266;   // 0/1, o error negativo
                // controles
                else if (nb_text_eq_cstr(n->text, "ButtonState") && n->list_count == 1) sys1 = 141;     // casilla u opcion: 1/0
                else if (nb_text_eq_cstr(n->text, "SliderValue") && n->list_count == 1) sys1 = 166;
                else if (nb_text_eq_cstr(n->text, "ActivateGadget") && n->list_count == 1) sys1 = 112; // da el foco (campo de texto)
                // temporizador de la ventana: evento $4001 (16385) tantas veces por segundo
                else if (nb_text_eq_cstr(n->text, "CreateTimer") && n->list_count == 1) sys1 = 127;
                else if (nb_text_eq_cstr(n->text, "FreeTimer") && n->list_count == 1) sys1 = 131;
                else if (nb_text_eq_cstr(n->text, "PauseTimer") && n->list_count == 1) sys1 = 216;
                else if (nb_text_eq_cstr(n->text, "ResumeTimer") && n->list_count == 1) sys1 = 217;
                else if (nb_text_eq_cstr(n->text, "ResetTimer") && n->list_count == 1) sys1 = 218;
                else if (nb_text_eq_cstr(n->text, "TimerTicks") && n->list_count == 1) sys1 = 219;
                // arbol
                else if (nb_text_eq_cstr(n->text, "MouseHit") && n->list_count == 1) sys1 = 56;   // un clic, una vez
                else if (nb_text_eq_cstr(n->text, "TreeViewRoot") && n->list_count == 1) sys1 = 176;
                else if (nb_text_eq_cstr(n->text, "FreeTreeViewNode") && n->list_count == 1) sys1 = 180;
                else if (nb_text_eq_cstr(n->text, "CountTreeViewNodes") && n->list_count == 1) sys1 = 182;
                else if (nb_text_eq_cstr(n->text, "SelectedTreeViewNode") && n->list_count == 1) sys1 = 183;
                else if (nb_text_eq_cstr(n->text, "SelectTreeViewNode") && n->list_count == 1) sys1 = 184;
                // lienzo e imagenes: a donde va el dibujo
                else if (nb_text_eq_cstr(n->text, "SetBuffer") && n->list_count == 1) sys1 = 128;
                // KeyBank(n): 64 teclas de golpe en un entero
                else if (nb_text_eq_cstr(n->text, "KeyBank") && n->list_count == 1) sys1 = 273;
                // El grupo al que pertenece un control, o 0 si no tiene
                else if (nb_text_eq_cstr(n->text, "GadgetGroup") && n->list_count == 1) sys1 = 224;
                // Sockets UDP. UdpOpen devuelve el
                // identificador o un negativo; UdpLost, los datagramas
                // tirados por no vaciar a tiempo.
                else if (nb_text_eq_cstr(n->text, "UdpOpen") && n->list_count == 1) sys1 = 301;
                else if (nb_text_eq_cstr(n->text, "UdpClose") && n->list_count == 1) sys1 = 302;
                else if (nb_text_eq_cstr(n->text, "UdpLost") && n->list_count == 1) sys1 = 306;
                // UdpPort: el puerto que de verdad tiene el socket. Hace
                // falta cuando se abre con UdpOpen(0), que elige uno libre:
                // sin esto no hay forma de decirle al otro extremo a donde
                // contestar, y abrir con 0 quedaba a medias.
                else if (nb_text_eq_cstr(n->text, "UdpPort") && n->list_count == 1) sys1 = 317;
                // UdpPending: cuantos datagramas esperan. Es la que hace
                // posible el bucle que vacia la cola -- ver la nota de
                // SYS_UDP_PENDIENTES en syscall.h.
                else if (nb_text_eq_cstr(n->text, "UdpPending") && n->list_count == 1) sys1 = 311;
                if (sys1 != 0) {
                    nb_emit_expr_int(ctx, n->list[0]);
                    nb_emit_load_imm64(cb, 8, sys1);
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    return;
                }
            }
            // ImageWidth / ImageHeight: los dos del MISMO syscall 51,
            // que devuelve (ancho<<32 | alto) -- mismo patron que
            // MouseX/MouseY.
            if (n->list_count == 1 && (nb_text_eq_cstr(n->text, "ImageWidth") ||
                                       nb_text_eq_cstr(n->text, "ImageHeight"))) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_emit_load_imm64(cb, 8, 51);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                if (nb_text_eq_cstr(n->text, "ImageWidth")) nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, 32));
                else nb_codebuf_emit(cb, nb_enc_and_imm(0, 0, 0xFFFFFFFFu));
                return;
            }

            // ---- Input$: leer una linea del teclado ----
            //
            // Una syscall (258) y ya esta. La primera version generaba
            // el bucle entero aqui -- leer tecla, hacer eco, gestionar
            // el retroceso, comparar con Enter -- y colgo el sistema:
            // SYS_READ_CHAR_WAIT devuelve -1 cuando la tarea no tiene
            // ventana de consola, el bucle generado tomaba ese -1 por
            // un caracter, lo hacia eco y no terminaba nunca.
            //
            // El bucle vive ahora en el kernel, en C: mas corto, cede
            // el turno correctamente, y sirve tambien para Lua.
            if (n->list_count == 0 && (nb_text_eq_cstr(n->text, "Input$") ||
                                       nb_text_eq_cstr(n->text, "Input"))) {
                nb_codebuf_emit(cb, nb_enc_movz(0, 256, 0));
                nb_label_t *alloc_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_alloc", n->line) : NULL;
                if (alloc_fn) nb_emit_bl(cb, alloc_fn);
                nb_push_x0(cb);                                        // [sp] = buffer
                nb_codebuf_emit(cb, nb_enc_movz(1, 256, 0));           // x1 = tamano
                nb_emit_load_imm64(cb, 8, 258);                        // SYS_READ_LINE
                nb_codebuf_emit(cb, nb_enc_svc(0));                    // x0 = longitud (o -1)
                // Si no hay consola devuelve -1: se trata como cadena
                // vacia, que es lo menos sorprendente para el programa.
                nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0));
                nb_codebuf_emit(cb, nb_enc_csel(0, 0, NB_REG_ZR, NB_COND_GE));
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));  // x1 = longitud
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0));  // x0 = buffer
                nb_label_t *new_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                if (new_fn) nb_emit_bl(cb, new_fn);                    // x0 = cadena
                nb_pop_x1(cb);                                         // x1 = buffer
                nb_push_x0(cb);                                        // guardar la cadena
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));
                nb_label_t *free_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_free", n->line) : NULL;
                if (free_fn) nb_emit_bl(cb, free_fn);
                nb_pop_x0(cb);                                         // x0 = la cadena
                return;
            }

            // ---- Cadenas: busqueda y transformacion ----
            //
            // Instr(s$, buscar$ [, desde]) -> posicion (1-indexada), 0 si no esta
            // Upper$ / Lower$ / Trim$      -> cadena nueva
            // Replace$(s$, de$, a$)        -> cadena nueva, TODAS las apariciones
            // Chr$(codigo) / Asc(s$)       -> caracter <-> codigo
            {
                const char *fn = NULL;
                int32_t nargs = 0;
                if (nb_text_eq_cstr(n->text, "Upper$"))       { fn = "nb_string_upper"; nargs = 1; }
                else if (nb_text_eq_cstr(n->text, "Lower$"))  { fn = "nb_string_lower"; nargs = 1; }
                else if (nb_text_eq_cstr(n->text, "Trim$"))   { fn = "nb_string_trim";  nargs = 1; }
                if (fn && n->list_count == nargs) {
                    nb_emit_expr_string(ctx, n->list[0]);
                    nb_label_t *f = ctx->runtime ? nb_runtime_find(ctx->runtime, fn) : NULL;
                    if (f) nb_emit_bl(cb, f);
                    return;
                }
            }
            if (nb_text_eq_cstr(n->text, "Instr") && (n->list_count == 2 || n->list_count == 3)) {
                nb_emit_expr_string(ctx, n->list[0]); nb_push_x0(cb);
                nb_emit_expr_string(ctx, n->list[1]); nb_push_x0(cb);
                if (n->list_count == 3) nb_emit_expr_int(ctx, n->list[2]);
                else nb_codebuf_emit(cb, nb_enc_movz(0, 1, 0));   // desde = 1 por defecto
                nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 0)); // x2 = desde
                nb_pop_x1(cb);                                        // x1 = buscar$
                nb_pop_x0(cb);                                        // x0 = cadena$
                nb_label_t *f = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_instr", n->line) : NULL;
                if (f) nb_emit_bl(cb, f);
                return;
            }
            if (nb_text_eq_cstr(n->text, "Replace$") && n->list_count == 3) {
                nb_emit_expr_string(ctx, n->list[0]); nb_push_x0(cb);
                nb_emit_expr_string(ctx, n->list[1]); nb_push_x0(cb);
                nb_emit_expr_string(ctx, n->list[2]);
                nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 0)); // x2 = a$
                nb_pop_x1(cb);                                        // x1 = de$
                nb_pop_x0(cb);                                        // x0 = s$
                nb_label_t *f = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_replace", n->line) : NULL;
                if (f) nb_emit_bl(cb, f);
                return;
            }
            // Chr$ se genera aqui, sin depender del runtime: un byte
            // en la pila y nb_string_new. Antes llamaba a nb_string_chr y, si esa
            // funcion no estaba en el bloque de runtime, la llamada NO se emitia y
            // el numero se usaba como si fuera una cadena (fallo en silencio).
            if (nb_text_eq_cstr(n->text, "Chr$") && n->list_count == 1) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_codebuf_emit(cb, nb_enc_sub_imm(NB_REG_SP, NB_REG_SP, 16));
                nb_codebuf_emit(cb, nb_enc_strb_imm(0, NB_REG_SP, 0));
                nb_codebuf_emit(cb, nb_enc_add_imm(0, NB_REG_SP, 0));
                nb_codebuf_emit(cb, nb_enc_movz(1, 1, 0));
                nb_label_t *f = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                if (f) nb_emit_bl(cb, f);
                nb_codebuf_emit(cb, nb_enc_add_imm(NB_REG_SP, NB_REG_SP, 16));
                return;
            }
            // Asc: el primer byte de la cadena (0 si esta vacia), tambien sin runtime
            if (nb_text_eq_cstr(n->text, "Asc") && n->list_count == 1) {
                nb_emit_expr_string(ctx, n->list[0]);
                nb_codebuf_emit(cb, nb_enc_orr_reg(19, NB_REG_ZR, 0));
                nb_label_t *flen = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_len", n->line) : NULL;
                if (flen) nb_emit_bl(cb, flen);
                nb_label_t *vacia = nb_label_new(), *fin = nb_label_new();
                nb_emit_cbz(cb, 0, vacia);
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 19));
                nb_label_t *fcstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (fcstr) nb_emit_bl(cb, fcstr);
                nb_codebuf_emit(cb, nb_enc_ldrb_imm(0, 0, 0));
                nb_emit_b(cb, fin);
                nb_label_define(cb, vacia);
                nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
                nb_label_define(cb, fin);
                nb_label_free(vacia); nb_label_free(fin);
                return;
            }

            // ---- Exp y Log (serie, al runtime) ----
            if (n->list_count == 1 && (nb_text_eq_cstr(n->text, "Exp") || nb_text_eq_cstr(n->text, "Exp#") ||
                                       nb_text_eq_cstr(n->text, "Log") || nb_text_eq_cstr(n->text, "Log#"))) {
                nb_emit_expr_as_float(ctx, n->list[0]);
                bool es_exp = nb_text_eq_cstr(n->text, "Exp") || nb_text_eq_cstr(n->text, "Exp#");
                nb_label_t *f = ctx->runtime ? nb_runtime_find(ctx->runtime, es_exp ? "nb_exp" : "nb_log") : NULL;
                if (f) nb_emit_bl(cb, f);
                nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0));
                return;
            }

            // ---- Floor y Ceil: UNA instruccion, sin runtime ----
            //
            // El procesador redondea a entero sin salir de coma
            // flotante: frintm hacia -infinito (floor) y frintp hacia
            // +infinito (ceil). Ojo con la diferencia respecto a Int(),
            // que trunca HACIA CERO: Int(-2.5) = -2, Floor(-2.5) = -3.
            if (n->list_count == 1 && (nb_text_eq_cstr(n->text, "Floor") || nb_text_eq_cstr(n->text, "Floor#") ||
                                       nb_text_eq_cstr(n->text, "Ceil") || nb_text_eq_cstr(n->text, "Ceil#"))) {
                nb_emit_expr_as_float(ctx, n->list[0]);
                bool es_floor = nb_text_eq_cstr(n->text, "Floor") || nb_text_eq_cstr(n->text, "Floor#");
                nb_codebuf_emit(cb, es_floor ? nb_enc_frintm(0, 0) : nb_enc_frintp(0, 0));
                nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0));
                return;
            }

            // ---- EventX / EventY: coordenadas del ultimo evento ----
            // syscall 256 devuelve (x<<32)|y
            if (n->list_count == 0 && (nb_text_eq_cstr(n->text, "EventX") ||
                                       nb_text_eq_cstr(n->text, "EventY"))) {
                nb_emit_load_imm64(cb, 8, 256);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                if (nb_text_eq_cstr(n->text, "EventX")) nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, 32));
                else nb_codebuf_emit(cb, nb_enc_and_imm(0, 0, 0xFFFFFFFFu));
                return;
            }

            // ---- CreateLabel: etiqueta de texto estatica (160) ----
            if (nb_text_eq_cstr(n->text, "CreateLabel") && n->list_count == 5) {
                nb_emit_expr_string(ctx, n->list[0]);
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr) nb_emit_bl(cb, cstr);
                nb_push_x0(cb);
                nb_emit_expr_int(ctx, n->list[1]); nb_push_x0(cb);   // x
                nb_emit_expr_int(ctx, n->list[2]); nb_push_x0(cb);   // y
                nb_emit_expr_int(ctx, n->list[3]);
                nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 0, 16));
                nb_push_x0(cb);
                nb_emit_expr_int(ctx, n->list[4]);
                nb_pop_x1(cb);
                nb_codebuf_emit(cb, nb_enc_orr_reg(3, 1, 0));       // x3 = ancho<<16|alto
                nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16)); // x2 = y
                nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16)); // x1 = x
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16)); // x0 = texto
                nb_codebuf_emit(cb, nb_enc_movz(4, 0, 0));            // x4 = estilo (0)
                nb_emit_load_imm64(cb, 8, 160);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }

            // ---- Archivos de texto, linea a linea ----
            //
            //   OpenFile(nombre$)  -> handle, o -1 si no existe (41)
            //   ReadLine$(handle)  -> siguiente linea, sin el salto (42)
            //   Eof(handle)        -> 1 si no queda nada por leer (43)
            //   CloseFile(handle)  -> cerrar (44)
            //
            // El kernel busca primero en la raiz y luego en DOCUMENTOS.
            // Solo LECTURA: escribir archivos necesitaria syscalls que
            // todavia no existen.
            // WriteLine manejador, texto$ -- escribe el texto y un salto de linea
            if (n->list_count == 2 && nb_text_eq_cstr(n->text, "WriteLine")) {
                g_usa_copia = true;
                nb_label_t *fn_cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                nb_label_t *fn_len = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_len", n->line) : NULL;
                nb_emit_expr_int(ctx, n->list[0]);
                nb_emit_load_imm64(cb, 1, 1000);
                nb_codebuf_emit(cb, nb_enc_sub_reg(19, 0, 1));            // x19 = manejador de verdad
                nb_emit_expr_string(ctx, n->list[1]);
                nb_codebuf_emit(cb, nb_enc_orr_reg(20, NB_REG_ZR, 0));
                if (fn_len) nb_emit_bl(cb, fn_len);
                nb_codebuf_emit(cb, nb_enc_orr_reg(21, NB_REG_ZR, 0));    // x21 = longitud
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 20));
                if (fn_cstr) nb_emit_bl(cb, fn_cstr);
                nb_codebuf_emit(cb, nb_enc_sub_imm(NB_REG_SP, NB_REG_SP, 1088));
                nb_codebuf_emit(cb, nb_enc_add_imm(24, NB_REG_SP, 0));
                nb_codebuf_emit(cb, nb_enc_orr_reg(25, NB_REG_ZR, 24));
                nb_emit_load_imm64(cb, 2, 1024);                          // se corta en 1024
                nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 21, 2));
                nb_label_t *cabe = nb_label_new();
                nb_emit_bcond(cb, NB_COND_LS, cabe);
                nb_codebuf_emit(cb, nb_enc_orr_reg(21, NB_REG_ZR, 2));
                nb_label_define(cb, cabe);
                nb_label_free(cabe);
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 21));
                nb_emit_bl(cb, g_h_copia);
                nb_codebuf_emit(cb, nb_enc_movz(2, 10, 0));               // el salto de linea
                nb_codebuf_emit(cb, nb_enc_strb_post(2, 25, 1));
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 19));
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 24));
                nb_codebuf_emit(cb, nb_enc_add_imm(2, 21, 1));            // los bytes + el salto
                nb_emit_load_imm64(cb, 8, 72);                            // SYS_GENFILE_WRITE_BYTES
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_codebuf_emit(cb, nb_enc_add_imm(NB_REG_SP, NB_REG_SP, 1088));
                return;
            }
            // SeekFile manejador, posicion
            if (n->list_count == 2 && nb_text_eq_cstr(n->text, "SeekFile")) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_emit_load_imm64(cb, 1, 1000);
                nb_codebuf_emit(cb, nb_enc_sub_reg(0, 0, 1));
                nb_push_x0(cb);
                nb_emit_expr_int(ctx, n->list[1]);
                nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 0));   // x2 = posicion
                nb_pop_x1(cb);                                           // x1 = manejador
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 2));
                nb_emit_load_imm64(cb, 8, 74);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // ClsColor r, g, b: el color con el que borra Cls.
            // Antes Cls pintaba SIEMPRE de negro, sin decirlo, y un programa de
            // fondo claro se quedaba con su texto oscuro invisible.
            if (nb_text_eq_cstr(n->text, "ClsColor") && n->list_count >= 3) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 0, 16));
                nb_push_x0(cb);
                nb_emit_expr_int(ctx, n->list[1]);
                nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 0, 8));
                nb_pop_x1(cb);
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, 1, 0));
                nb_push_x0(cb);
                nb_emit_expr_int(ctx, n->list[2]);
                nb_pop_x1(cb);
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, 1, 0));
                if (ctx->cls_color_var) nb_emit_store_var_from_x0(ctx, ctx->cls_color_var);
                return;
            }
            // ---- Mover y medir controles, y la ventana ----
            // SetGadgetShape id, x, y, ancho, alto
            if (n->list_count == 5 && nb_text_eq_cstr(n->text, "SetGadgetShape")) {
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);          // id
                nb_emit_expr_int(ctx, n->list[1]); nb_push_x0(cb);          // x
                nb_emit_expr_int(ctx, n->list[2]); nb_push_x0(cb);          // y
                nb_emit_expr_int(ctx, n->list[3]); nb_push_x0(cb);          // ancho
                nb_emit_expr_int(ctx, n->list[4]);
                nb_codebuf_emit(cb, nb_enc_orr_reg(23, NB_REG_ZR, 0));      // x23 = alto
                nb_pop_x1(cb); nb_codebuf_emit(cb, nb_enc_orr_reg(22, NB_REG_ZR, 1));   // x22 = ancho
                nb_pop_x1(cb); nb_codebuf_emit(cb, nb_enc_orr_reg(21, NB_REG_ZR, 1));   // x21 = y
                nb_pop_x1(cb); nb_codebuf_emit(cb, nb_enc_orr_reg(20, NB_REG_ZR, 1));   // x20 = x
                nb_pop_x1(cb); nb_codebuf_emit(cb, nb_enc_orr_reg(19, NB_REG_ZR, 1));   // x19 = id
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 19));
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 20));
                nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 21));
                nb_emit_load_imm64(cb, 8, 108);                             // mover
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 19));
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 22));
                nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 23));
                nb_emit_load_imm64(cb, 8, 109);                             // redimensionar
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // GadgetX / GadgetY / GadgetWidth / GadgetHeight: del rectangulo (107),
            // que viene empaquetado como (x<<48 | y<<32 | ancho<<16 | alto), con x
            // e y de 16 bits CON signo.
            if (n->list_count == 1 && (nb_text_eq_cstr(n->text, "GadgetX") || nb_text_eq_cstr(n->text, "GadgetY") ||
                                       nb_text_eq_cstr(n->text, "GadgetWidth") || nb_text_eq_cstr(n->text, "GadgetHeight"))) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_emit_load_imm64(cb, 8, 107);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                int desplaz = nb_text_eq_cstr(n->text, "GadgetX") ? 48 : nb_text_eq_cstr(n->text, "GadgetY") ? 32 :
                              nb_text_eq_cstr(n->text, "GadgetWidth") ? 16 : 0;
                if (desplaz) nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, desplaz));
                bool con_signo = desplaz >= 32;                             // x e y pueden ser negativos
                if (con_signo) {
                    nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 0, 48));
                    nb_codebuf_emit(cb, nb_enc_asr_imm(0, 0, 48));
                } else {
                    nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 0, 48));
                    nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, 48));
                }
                return;
            }
            // ClientWidth() / ClientHeight(): la zona de dibujo de la ventana
            if (n->list_count == 0 && (nb_text_eq_cstr(n->text, "ClientWidth") || nb_text_eq_cstr(n->text, "ClientHeight"))) {
                nb_emit_load_imm64(cb, 8, 33);                              // SYS_GET_WINDOW_SIZE
                nb_codebuf_emit(cb, nb_enc_svc(0));
                if (nb_text_eq_cstr(n->text, "ClientWidth")) nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, 32));
                else { nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 0, 32)); nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, 32)); }
                return;
            }
            // ---- Archivos ----
            // Por NOMBRE: tamaño, tipo, existe, borrar, crear carpeta, abrir
            // para escribir y recorrer una carpeta.
            if (n->list_count == 1 && (nb_text_eq_cstr(n->text, "FileSize") || nb_text_eq_cstr(n->text, "FileType") ||
                                       nb_text_eq_cstr(n->text, "FileExists") || nb_text_eq_cstr(n->text, "DeleteFile") ||
                                       nb_text_eq_cstr(n->text, "CreateDir") || nb_text_eq_cstr(n->text, "WriteFile") ||
                                       nb_text_eq_cstr(n->text, "ReadDir"))) {
                nb_emit_expr_string(ctx, n->list[0]);
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr) nb_emit_bl(cb, cstr);
                if (nb_text_eq_cstr(n->text, "CreateDir")) {
                    nb_codebuf_emit(cb, nb_enc_movz(1, 0, 0));      // en la raiz
                    nb_codebuf_emit(cb, nb_enc_movz(2, 0, 0));      // NemoFS
                    nb_emit_load_imm64(cb, 8, 24);
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    return;
                }
                if (nb_text_eq_cstr(n->text, "WriteFile")) {        // abrir para escribir: crea vacio
                    nb_codebuf_emit(cb, nb_enc_movz(1, 1, 0));      // modo 1
                    nb_emit_load_imm64(cb, 8, 70);
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    // los manejadores de escritura van +1000, para que CloseFile
                    // sepa con que llamada cerrarlos
                    nb_label_t *malo = nb_label_new();
                    nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0));
                    nb_emit_bcond(cb, NB_COND_LT, malo);
                    nb_emit_load_imm64(cb, 1, 1000);
                    nb_codebuf_emit(cb, nb_enc_add_reg(0, 0, 1, 0));
                    nb_label_define(cb, malo);
                    nb_label_free(malo);
                    return;
                }
                if (nb_text_eq_cstr(n->text, "ReadDir")) {          // recorrer una carpeta
                    nb_label_t *raiz = nb_label_new(), *abrir = nb_label_new(), *no_hay = nb_label_new(), *fin = nb_label_new();
                    nb_codebuf_emit(cb, nb_enc_ldrb_imm(1, 0, 0));  // ¿el nombre esta vacio? -> la raiz
                    nb_emit_cbz(cb, 1, raiz);
                    nb_codebuf_emit(cb, nb_enc_movz(1, 0, 0));      // buscar en la raiz
                    nb_emit_load_imm64(cb, 8, 83);                  // SYS_FIND_CHILD
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0));
                    nb_emit_bcond(cb, NB_COND_LT, no_hay);
                    nb_emit_b(cb, abrir);
                    nb_label_define(cb, raiz);
                    nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
                    nb_label_define(cb, abrir);
                    nb_emit_load_imm64(cb, 8, 78);                  // SYS_DIR_OPEN
                    nb_codebuf_emit(cb, nb_enc_svc(0));
                    nb_emit_b(cb, fin);
                    nb_label_define(cb, no_hay);
                    nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
                    nb_codebuf_emit(cb, nb_enc_sub_imm(0, 0, 1));   // -1: no existe
                    nb_label_define(cb, fin);
                    nb_label_free(raiz); nb_label_free(abrir); nb_label_free(no_hay); nb_label_free(fin);
                    return;
                }
                nb_emit_load_imm64(cb, 8, nb_text_eq_cstr(n->text, "FileSize") ? 81 :
                                          nb_text_eq_cstr(n->text, "DeleteFile") ? 84 : 82);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                if (nb_text_eq_cstr(n->text, "FileExists")) {       // 0/1 a partir del tipo
                    nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0));
                    nb_codebuf_emit(cb, nb_enc_cset(0, NB_COND_NE));
                }
                return;
            }
            // Con un MANEJADOR: posicion, tamaño, cerrar una carpeta
            if (n->list_count == 1 && (nb_text_eq_cstr(n->text, "FilePos") || nb_text_eq_cstr(n->text, "FileLength") ||
                                       nb_text_eq_cstr(n->text, "CloseDir"))) {
                nb_emit_expr_int(ctx, n->list[0]);
                if (!nb_text_eq_cstr(n->text, "CloseDir")) {        // los de escritura van +1000
                    nb_emit_load_imm64(cb, 1, 1000);
                    nb_codebuf_emit(cb, nb_enc_sub_reg(0, 0, 1));
                }
                nb_emit_load_imm64(cb, 8, nb_text_eq_cstr(n->text, "FilePos") ? 73 :
                                          nb_text_eq_cstr(n->text, "FileLength") ? 75 : 80);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "OpenFile")) {
                nb_emit_expr_string(ctx, n->list[0]);
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr) nb_emit_bl(cb, cstr);
                nb_emit_load_imm64(cb, 8, 41);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            if (n->list_count == 1 && (nb_text_eq_cstr(n->text, "Eof") ||
                                       nb_text_eq_cstr(n->text, "CloseFile"))) {
                nb_emit_expr_int(ctx, n->list[0]);
                // Un manejador de ESCRITURA viene de WriteFile y vale
                // +1000: se cierra (o se pregunta el fin) con la otra familia de
                // llamadas. Asi el usuario solo aprende CloseFile y Eof.
                nb_label_t *lectura = nb_label_new(), *fin = nb_label_new();
                nb_emit_load_imm64(cb, 1, 1000);
                nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 0, 1));
                nb_emit_bcond(cb, NB_COND_LT, lectura);
                nb_codebuf_emit(cb, nb_enc_sub_reg(0, 0, 1));
                nb_emit_load_imm64(cb, 8, nb_text_eq_cstr(n->text, "Eof") ? 76 : 77);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_emit_b(cb, fin);
                nb_label_define(cb, lectura);
                nb_emit_load_imm64(cb, 8, nb_text_eq_cstr(n->text, "Eof") ? 43 : 44);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_label_define(cb, fin);
                nb_label_free(lectura); nb_label_free(fin);
                return;
            }
            // ---- I2C: bus 1, GPIO 2 = SDA, GPIO 3 = SCL ----
            // I2cWrite dir, datos$ -> bytes enviados, o error negativo. Los
            // bytes van en una cadena (Chr$ para construirla).
            // Pila mientras se prepara (de arriba a abajo), como en WriteFile:
            //   [sp] longitud, [sp+16] la cadena, [sp+32] la direccion
            if (n->list_count == 2 && nb_text_eq_cstr(n->text, "I2cWrite")) {
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                nb_label_t *slen = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_len", n->line) : NULL;
                nb_emit_expr_int(ctx, n->list[0]);
                nb_push_x0(cb);                                        // la direccion
                nb_emit_expr_string(ctx, n->list[1]);
                nb_push_x0(cb);                                        // la cadena
                if (slen) nb_emit_bl(cb, slen);
                nb_push_x0(cb);                                        // la longitud
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 2));  // x0 = la cadena
                if (cstr) nb_emit_bl(cb, cstr);                        // x0 = bytes crudos
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));  // x1 = bytes
                nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16)); // x2 = longitud
                nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16)); // soltar la cadena
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16)); // x0 = direccion
                nb_emit_load_imm64(cb, 8, 268);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // I2cRead$(dir, n) -> cadena con los n bytes leidos (Asc(Mid$(...))
            // para sacarlos), o "" si algo falla. Patron de ReadLine$: buffer
            // del monton, el kernel lo rellena, nb_string_new, liberar.
            if (n->list_count == 2 && nb_text_eq_cstr(n->text, "I2cRead$")) {
                nb_codebuf_emit(cb, nb_enc_movz(0, 256, 0));
                nb_label_t *alloc_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_alloc", n->line) : NULL;
                if (alloc_fn) nb_emit_bl(cb, alloc_fn);
                nb_push_x0(cb);                                        // pila: buffer
                nb_emit_expr_int(ctx, n->list[1]);
                nb_push_x0(cb);                                        // pila: n
                nb_emit_expr_int(ctx, n->list[0]);                     // x0 = direccion
                nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16)); // x2 = n
                nb_codebuf_emit(cb, nb_enc_ldr_imm(1, NB_REG_SP, 0));  // x1 = buffer (sigue en la pila)
                nb_emit_load_imm64(cb, 8, 269);
                nb_codebuf_emit(cb, nb_enc_svc(0));                    // x0 = bytes leidos, o error
                nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 0, NB_REG_ZR));        // cmp x0, 0
                nb_codebuf_emit(cb, nb_enc_csel(0, NB_REG_ZR, 0, NB_COND_LT));        // error -> 0 bytes
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));  // x1 = longitud
                nb_pop_x0(cb);                                          // x0 = buffer
                nb_push_x0(cb);                                        // pila: buffer (para liberarlo)
                nb_label_t *new_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                if (new_fn) nb_emit_bl(cb, new_fn);                    // x0 = cadena
                nb_pop_x1(cb);                                          // x1 = buffer
                nb_push_x0(cb);                                        // pila: la cadena
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));  // x0 = buffer
                nb_label_t *free_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_free", n->line) : NULL;
                if (free_fn) nb_emit_bl(cb, free_fn);
                nb_pop_x0(cb);                                          // x0 = la cadena
                return;
            }
            // ---- Sockets UDP --------------------------------
            //
            // UdpSend h, ip$, puerto, datos$ -> bytes mandados, o negativo.
            // Syscall 303 con cinco registros: x0 socket, x1 la IP EN TEXTO,
            // x2 puerto, x3 los bytes, x4 cuantos.
            //
            // La IP va en texto y no empaquetada a proposito: partir
            // "192.168.1.40" aqui seria generar un analizador de numeros en
            // ARM64 a mano, y el kernel ya lo tiene hecho y probado
            // (ip_texto.c, 33 comprobaciones). Y ademas una IP en texto se
            // puede RECHAZAR cuando esta mal; un entero empaquetado no tiene
            // forma de parecer equivocado.
            //
            // Por el camino hay tres llamadas al runtime (nb_string_cstr dos
            // veces y nb_string_len) y cada una puede pisar cualquier
            // registro: todo a la pila y se carga al final. Con las seis
            // cosas dentro, de arriba a abajo:
            //   [sp] longitud, [sp+16] la cadena de datos, [sp+32] puerto,
            //   [sp+48] la IP cruda, [sp+64] la cadena de la IP,
            //   [sp+80] el socket
            if (n->list_count == 4 && nb_text_eq_cstr(n->text, "UdpSend")) {
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                nb_label_t *slen = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_len", n->line) : NULL;
                nb_emit_expr_int(ctx, n->list[0]);
                nb_push_x0(cb);                                          // el socket
                nb_emit_expr_string(ctx, n->list[1]);
                nb_push_x0(cb);                                          // la cadena de la IP
                if (cstr) nb_emit_bl(cb, cstr);
                nb_push_x0(cb);                                          // la IP cruda
                nb_emit_expr_int(ctx, n->list[2]);
                nb_push_x0(cb);                                          // el puerto
                nb_emit_expr_string(ctx, n->list[3]);
                nb_push_x0(cb);                                          // la cadena de datos
                if (slen) nb_emit_bl(cb, slen);
                nb_push_x0(cb);                                          // la longitud
                // Ojo con el orden: nb_string_cstr necesita la cadena, y
                // nb_string_len ya se la ha comido de x0. Se recupera de la
                // pila ([sp+16] = la cadena de datos).
                //
                // Y ojo con el indice: nb_enc_ldr_imm cuenta en unidades de
                // OCHO bytes (lo escala el propio ARM), mientras que los
                // huecos de nb_push_x0 son de DIECISEIS. Asi que [sp+16] es
                // el indice 2, no el 1. Escribi el 1 la primera vez: habria
                // leido el relleno del hueco --que nadie escribe nunca-- y
                // le habria pasado eso a nb_string_cstr como si fuera una
                // cadena. No lo caza ninguna prueba de numero de syscall ni
                // de cuadre de pila; se ve leyendo, o se ve en la placa
                // como un Data Abort sin explicacion.
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 2));    // x0 = la cadena de datos ([sp+16])
                if (cstr) nb_emit_bl(cb, cstr);                          // x0 = bytes crudos
                nb_codebuf_emit(cb, nb_enc_orr_reg(3, NB_REG_ZR, 0));    // x3 = los bytes
                nb_codebuf_emit(cb, nb_enc_ldr_post(4, NB_REG_SP, 16));  // x4 = cuantos
                nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16)); // soltar la cadena de datos
                nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16));  // x2 = puerto
                nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));  // x1 = la IP cruda
                nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16)); // soltar la cadena de la IP
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));  // x0 = el socket
                nb_emit_load_imm64(cb, 8, 303);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }

            // UdpRecv$(h) -> el siguiente datagrama como cadena, o "" si no
            // hay ninguno. Mismo patron que I2cRead$: buffer del monton, el
            // kernel lo rellena, nb_string_new, liberar.
            //
            // OJO CON EL "": un datagrama VACIO es legal en UDP --es como se
            // manda un "estoy aqui" sin datos-- y aqui tambien sale "". O
            // sea que cadena vacia NO distingue "no habia nada" de "llego
            // uno sin datos". Quien necesite la diferencia mira UdpFromPort:
            // viene a 0 si no se leyo nada.
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "UdpRecv$")) {
                nb_label_t *alloc_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_alloc", n->line) : NULL;
                nb_label_t *new_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                nb_label_t *free_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_free", n->line) : NULL;
                // 1024 = UDPS_MAX_DATAGRAMA. El tope del kernel y el de aqui
                // tienen que ser el mismo: con menos, un datagrama grande
                // llegaria cortado y parecerian datos.
                nb_emit_load_imm64(cb, 0, 1024);
                if (alloc_fn) nb_emit_bl(cb, alloc_fn);
                nb_push_x0(cb);                                          // el buffer
                nb_emit_expr_int(ctx, n->list[0]);                       // x0 = socket
                nb_codebuf_emit(cb, nb_enc_ldr_imm(1, NB_REG_SP, 0));    // x1 = buffer (sigue en la pila)
                nb_emit_load_imm64(cb, 2, 1024);                         // x2 = tamano
                nb_emit_load_imm64(cb, 8, 304);
                nb_codebuf_emit(cb, nb_enc_svc(0));                      // x0 = bytes, o -1
                nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 0, NB_REG_ZR));  // cmp x0, 0
                nb_codebuf_emit(cb, nb_enc_csel(0, NB_REG_ZR, 0, NB_COND_LT));  // -1 -> 0 bytes
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));    // x1 = longitud
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0));    // x0 = buffer
                if (new_fn) nb_emit_bl(cb, new_fn);                      // x0 = cadena
                nb_pop_x1(cb);                                            // x1 = buffer
                nb_push_x0(cb);                                          // pila: la cadena
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));    // x0 = buffer
                if (free_fn) nb_emit_bl(cb, free_fn);
                nb_pop_x0(cb);                                            // x0 = la cadena
                return;
            }

            // UdpFrom$(h) -> la IP de quien mando EL DATAGRAMA QUE ACABA DE
            // LEER UdpRecv$, en texto. No el ultimo que llego: ver la nota
            // larga de src/udp_sock.h, que es el fallo silencioso que hace
            // que en una partida de cuatro le contestes al que no era.
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "UdpFrom$")) {
                nb_label_t *alloc_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_alloc", n->line) : NULL;
                nb_label_t *new_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                nb_label_t *free_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_free", n->line) : NULL;
                nb_emit_load_imm64(cb, 0, 32);                           // 16 bastan (IP_TEXTO_MAX); 32 sobra
                if (alloc_fn) nb_emit_bl(cb, alloc_fn);
                nb_push_x0(cb);                                          // el buffer
                nb_emit_expr_int(ctx, n->list[0]);                       // x0 = socket
                nb_codebuf_emit(cb, nb_enc_ldr_imm(1, NB_REG_SP, 0));    // x1 = buffer
                nb_emit_load_imm64(cb, 2, 32);                           // x2 = tamano
                nb_emit_load_imm64(cb, 8, 305);
                nb_codebuf_emit(cb, nb_enc_svc(0));                      // x0 = letras | puerto<<32
                nb_codebuf_emit(cb, nb_enc_and_imm(0, 0, 0xFFFFFFFFu));  // solo las letras
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));    // x1 = longitud
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0));    // x0 = buffer
                if (new_fn) nb_emit_bl(cb, new_fn);
                nb_pop_x1(cb);
                nb_push_x0(cb);
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));
                if (free_fn) nb_emit_bl(cb, free_fn);
                nb_pop_x0(cb);
                return;
            }

            // UdpFromPort(h) -> su puerto. Misma syscall que UdpFrom$, con
            // el buffer a cero para no reservar uno y tirarlo: el puerto
            // viene en la mitad alta del resultado. Mismo truco que
            // ImageWidth/ImageHeight.
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "UdpFromPort")) {
                nb_emit_expr_int(ctx, n->list[0]);                       // x0 = socket
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, NB_REG_ZR)); // x1 = 0, sin buffer
                nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, NB_REG_ZR)); // x2 = 0
                nb_emit_load_imm64(cb, 8, 305);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, 32));
                return;
            }

            // ---- HTTP ---------------------------------------
            //
            // HttpGet(ip$, puerto, ruta$)            -> 1 si arranco, 0 si no
            // HttpPost(ip$, puerto, ruta$, cuerpo$)  -> 1 si arranco, 0 si no
            //
            // Las dos son la syscall 307, con x0 la ruta, x1 la IP en texto,
            // x2 el puerto, x3 el cuerpo y x4 su longitud. Un GET manda x3 a
            // cero, que es lo que distingue los dos metodos.
            //
            // NINGUNA DE LAS DOS ESPERA. Arrancan la peticion y vuelven en el
            // acto; despues se consulta HttpState() hasta que termine. El
            // planificador de Nemo OS es cooperativo, asi que un syscall que
            // se quedara esperando con el candado grande cogido pararia la
            // MAQUINA ENTERA y no solo el programa: el raton, el reloj y las
            // demas ventanas. Quien quiera una version que espere, la monta con un
            // bucle y Delay en Nemo Basic (ver MANUALES/RED.NB), donde esperar
            // no le cuesta nada a nadie.
            //
            // Devuelven 1/0 y no el 0/-1 del syscall: "If HttpGet(...) Then"
            // tiene que leerse como se escribe, y con el valor crudo seria
            // falso justo cuando todo ha ido bien.
            if ((n->list_count == 3 && nb_text_eq_cstr(n->text, "HttpGet")) ||
                (n->list_count == 4 && nb_text_eq_cstr(n->text, "HttpPost"))) {
                bool con_cuerpo = nb_text_eq_cstr(n->text, "HttpPost");
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                nb_label_t *slen = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_len", n->line) : NULL;

                nb_emit_expr_string(ctx, n->list[0]);
                nb_push_x0(cb);                                          // la cadena de la IP
                if (cstr) nb_emit_bl(cb, cstr);
                nb_push_x0(cb);                                          // la IP cruda
                nb_emit_expr_int(ctx, n->list[1]);
                nb_push_x0(cb);                                          // el puerto
                if (con_cuerpo) {
                    nb_emit_expr_string(ctx, n->list[3]);
                    nb_push_x0(cb);                                      // la cadena del cuerpo
                    if (slen) nb_emit_bl(cb, slen);
                    nb_push_x0(cb);                                      // la longitud
                    // [sp+16] es el indice 2: nb_enc_ldr_imm cuenta de ocho
                    // en ocho y los huecos son de dieciseis.
                    nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 2)); // x0 = la cadena del cuerpo
                    if (cstr) nb_emit_bl(cb, cstr);
                    nb_push_x0(cb);                                      // el cuerpo crudo
                }
                nb_emit_expr_string(ctx, n->list[2]);
                nb_push_x0(cb);                                          // la cadena de la ruta
                if (cstr) nb_emit_bl(cb, cstr);                          // x0 = la ruta cruda, ya en su sitio
                nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16)); // soltar la cadena de la ruta
                if (con_cuerpo) {
                    nb_codebuf_emit(cb, nb_enc_ldr_post(3, NB_REG_SP, 16));     // x3 = el cuerpo
                    nb_codebuf_emit(cb, nb_enc_ldr_post(4, NB_REG_SP, 16));     // x4 = su longitud
                    nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16)); // soltar la cadena del cuerpo
                } else {
                    nb_codebuf_emit(cb, nb_enc_orr_reg(3, NB_REG_ZR, NB_REG_ZR)); // x3 = 0: es un GET
                    nb_codebuf_emit(cb, nb_enc_orr_reg(4, NB_REG_ZR, NB_REG_ZR)); // x4 = 0
                }
                nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16));  // x2 = el puerto
                nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));  // x1 = la IP cruda
                nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16)); // soltar la cadena de la IP
                nb_emit_load_imm64(cb, 8, 307);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 0, NB_REG_ZR)); // cmp x0, 0
                nb_codebuf_emit(cb, nb_enc_cset(0, NB_COND_EQ));               // 0 -> 1, lo demas -> 0
                return;
            }

            // Val#(texto$) -> el numero CON sus decimales.
            //
            // Val() devuelve entero, y eso convertia "21.5" en 21 sin decir
            // nada. Un programa que lee una temperatura de la red, de un
            // archivo o de un campo de texto perdia la mitad del dato y
            // seguia tan tranquilo. Str$() si escribe los decimales, asi que
            // el viaje de ida y vuelta estaba roto por un lado solo.
            //
            // La funcion del bloque de runtime ya existia (nb_str_to_float):
            // lo unico que faltaba era el nombre para llamarla.
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "Val#")) {
                nb_emit_expr_string(ctx, n->list[0]);
                nb_label_t *fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_str_to_float", n->line) : NULL;
                if (fn) nb_emit_bl(cb, fn);
                nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0)); // los bits del decimal, a x0
                return;
            }

            // CloseWindow() -> cierra la ventana del programa y sigue.
            //
            // Cada programa tiene UNA ventana. Hasta ahora solo desaparecia
            // al terminar el programa entero, asi que un programa que
            // atendia el aviso de cerrar (EVENT_WINDOWCLOSE) no tenia forma
            // de hacerle caso sin morirse. Con esto puede cerrar, guardar lo
            // que estuviera haciendo y terminar cuando le convenga.
            //
            // Un CreateWindow o cualquier comando grafico posterior crea una
            // nueva: la ventana no es un recurso que se agote.
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "CloseWindow")) {
                nb_emit_load_imm64(cb, 8, 313);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }

            // RenameFile(viejo$, nuevo$) -> 1 si se renombro, 0 si no.
            //
            // El kernel busca el archivo donde este (ruta, raiz o DOCUMENTOS),
            // igual que DeleteFile, porque en Nemo Basic los nombres van a
            // secas. Devuelve 1/0 y no el 0/-1 del syscall, para que
            // "If RenameFile(a$, b$) Then" se lea como se escribe.
            //
            // Da 0, y no se lleva nada por delante, si: el archivo no existe,
            // ya hay otro con el nombre nuevo, el nombre nuevo lleva carpetas
            // (renombrar no mueve de sitio), o el archivo esta en FAT.
            if (n->list_count == 2 && nb_text_eq_cstr(n->text, "RenameFile")) {
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                nb_emit_expr_string(ctx, n->list[0]);
                nb_push_x0(cb);                                          // la cadena vieja
                if (cstr) nb_emit_bl(cb, cstr);
                nb_push_x0(cb);                                          // el nombre viejo crudo
                nb_emit_expr_string(ctx, n->list[1]);
                nb_push_x0(cb);                                          // la cadena nueva
                if (cstr) nb_emit_bl(cb, cstr);                          // x0 = el nombre nuevo crudo
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));    // x1 = nombre nuevo
                nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16)); // soltar la cadena nueva
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));  // x0 = nombre viejo
                nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16)); // soltar la cadena vieja
                nb_emit_load_imm64(cb, 8, 312);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 0, NB_REG_ZR)); // cmp x0, 0
                nb_codebuf_emit(cb, nb_enc_cset(0, NB_COND_EQ));               // 0 -> 1, lo demas -> 0
                return;
            }

            // HttpState() -> 0 parada, 1 en marcha, 2 lista, 3 fallo.
            // HttpCode()  -> 200, 404...  0 si aun no se sabe.
            // Las dos son la syscall 297, que devuelve estado | codigo<<8.
            if (n->list_count == 0 && (nb_text_eq_cstr(n->text, "HttpState") ||
                                       nb_text_eq_cstr(n->text, "HttpCode"))) {
                nb_emit_load_imm64(cb, 8, 297);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                if (nb_text_eq_cstr(n->text, "HttpCode"))
                    nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, 8));
                else
                    nb_codebuf_emit(cb, nb_enc_and_imm(0, 0, 0xFFu));
                return;
            }

            // HttpBody$() -> la respuesta recibida (hasta 2048 bytes).
            // HttpFail$() -> por que fallo, en una linea; "" si no fallo.
            // Mismo patron que I2cRead$: buffer del monton, el kernel lo
            // rellena, nb_string_new, liberar.
            if (n->list_count == 0 && (nb_text_eq_cstr(n->text, "HttpBody$") ||
                                       nb_text_eq_cstr(n->text, "HttpFail$"))) {
                bool es_cuerpo = nb_text_eq_cstr(n->text, "HttpBody$");
                // 2048 = DESCARGA_VISTA para el cuerpo; el motivo es una
                // linea y con 128 va sobrado. Los dos numeros tienen que
                // coincidir con los del kernel: con menos, la respuesta
                // llegaria cortada y pareceria la respuesta entera.
                uint32_t tam = es_cuerpo ? 2048 : 128;
                nb_label_t *alloc_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_alloc", n->line) : NULL;
                nb_label_t *new_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                nb_label_t *free_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_free", n->line) : NULL;
                nb_emit_load_imm64(cb, 0, tam);
                if (alloc_fn) nb_emit_bl(cb, alloc_fn);
                nb_push_x0(cb);                                          // el buffer (x0 sigue siendo el)
                nb_emit_load_imm64(cb, 1, tam);                          // x1 = tamano
                nb_emit_load_imm64(cb, 8, es_cuerpo ? 308 : 299);
                nb_codebuf_emit(cb, nb_enc_svc(0));                      // x0 = bytes escritos
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));    // x1 = longitud
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0));    // x0 = buffer
                if (new_fn) nb_emit_bl(cb, new_fn);                      // x0 = cadena
                nb_pop_x1(cb);                                            // x1 = buffer
                nb_push_x0(cb);                                          // pila: la cadena
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));    // x0 = buffer
                if (free_fn) nb_emit_bl(cb, free_fn);
                nb_pop_x0(cb);                                            // x0 = la cadena
                return;
            }

            // NetIp$() / NetMask$() / NetGateway$() / NetDns$() -- la
            // configuracion propia, en texto. Los cuatro son la
            // syscall 309 con un selector en x2, y hay cuatro comandos en vez
            // de uno con argumento por la misma decision que se tomo con los
            // datos del sistema: un comando por dato, que se lee mejor y no
            // hay que recordar si la pasarela era el 2 o el 3.
            if (n->list_count == 0 && (nb_text_eq_cstr(n->text, "NetIp$") ||
                                       nb_text_eq_cstr(n->text, "NetMask$") ||
                                       nb_text_eq_cstr(n->text, "NetGateway$") ||
                                       nb_text_eq_cstr(n->text, "NetDns$"))) {
                uint32_t cual = 0;
                if (nb_text_eq_cstr(n->text, "NetMask$"))    cual = 1;
                if (nb_text_eq_cstr(n->text, "NetGateway$")) cual = 2;
                if (nb_text_eq_cstr(n->text, "NetDns$"))     cual = 3;
                nb_label_t *alloc_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_alloc", n->line) : NULL;
                nb_label_t *new_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                nb_label_t *free_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_free", n->line) : NULL;
                nb_emit_load_imm64(cb, 0, 32);                           // 16 bastan (IP_TEXTO_MAX)
                if (alloc_fn) nb_emit_bl(cb, alloc_fn);
                nb_push_x0(cb);                                          // el buffer (x0 sigue siendo el)
                nb_emit_load_imm64(cb, 1, 32);                           // x1 = tamano
                nb_emit_load_imm64(cb, 2, cual);                         // x2 = cual de los cuatro
                nb_emit_load_imm64(cb, 8, 309);
                nb_codebuf_emit(cb, nb_enc_svc(0));                      // x0 = letras escritas
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));    // x1 = longitud
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0));    // x0 = buffer
                if (new_fn) nb_emit_bl(cb, new_fn);
                nb_pop_x1(cb);
                nb_push_x0(cb);
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));
                if (free_fn) nb_emit_bl(cb, free_fn);
                nb_pop_x0(cb);
                return;
            }

            // NetReady() -> 1 si la red esta lista, 0 si no. Es la pregunta
            // que hay que hacer ANTES de todas las demas: pedir un socket
            // mientras el DHCP negocia no da error, simplemente no funciona.
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "NetReady")) {
                nb_emit_load_imm64(cb, 8, 310);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }

            // SpiTransfer$(datos$, chip, hz, modo): envia los bytes de
            // la cadena por SPI0 y devuelve los recibidos (misma longitud), o ""
            // si algo falla. Syscall 270 con SEIS registros: x0 chip, x1 bytes a
            // enviar, x2 cuantos, x3 hz, x4 buffer de recepcion, x5 modo. Por el
            // camino hay tres llamadas al runtime, que pueden pisar cualquier
            // registro: todo va a la pila y se carga al final. Pila (de arriba
            // a abajo) justo antes de la llamada:
            //   [sp] modo, [sp+16] hz, [sp+32] texto crudo, [sp+48] buffer,
            //   [sp+64] longitud, [sp+80] la cadena
            if (n->list_count == 4 && nb_text_eq_cstr(n->text, "SpiTransfer$")) {
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                nb_label_t *slen = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_len", n->line) : NULL;
                nb_label_t *alloc_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_alloc", n->line) : NULL;
                nb_label_t *new_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                nb_label_t *free_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_free", n->line) : NULL;
                nb_emit_expr_string(ctx, n->list[0]);
                nb_push_x0(cb);                                        // la cadena
                if (slen) nb_emit_bl(cb, slen);
                nb_push_x0(cb);                                        // la longitud
                if (alloc_fn) nb_emit_bl(cb, alloc_fn);                // x0 = buffer de recepcion (longitud bytes)
                nb_push_x0(cb);                                        // el buffer
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 4));  // x0 = la cadena ([sp+32])
                if (cstr) nb_emit_bl(cb, cstr);
                nb_push_x0(cb);                                        // el texto crudo
                nb_emit_expr_int(ctx, n->list[2]);
                nb_push_x0(cb);                                        // hz
                nb_emit_expr_int(ctx, n->list[3]);
                nb_push_x0(cb);                                        // modo
                nb_emit_expr_int(ctx, n->list[1]);                     // x0 = chip
                nb_codebuf_emit(cb, nb_enc_ldr_post(5, NB_REG_SP, 16)); // x5 = modo
                nb_codebuf_emit(cb, nb_enc_ldr_post(3, NB_REG_SP, 16)); // x3 = hz
                nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16)); // x1 = texto crudo
                nb_codebuf_emit(cb, nb_enc_ldr_imm(4, NB_REG_SP, 0));  // x4 = buffer ([sp])
                nb_codebuf_emit(cb, nb_enc_ldr_imm(2, NB_REG_SP, 2));  // x2 = longitud ([sp+16])
                nb_emit_load_imm64(cb, 8, 270);
                nb_codebuf_emit(cb, nb_enc_svc(0));                    // x0 = bytes, o error
                nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 0, NB_REG_ZR));        // cmp x0, 0
                nb_codebuf_emit(cb, nb_enc_csel(0, NB_REG_ZR, 0, NB_COND_LT));        // error -> ""
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));  // x1 = longitud recibida
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0));  // x0 = buffer
                if (new_fn) nb_emit_bl(cb, new_fn);                    // x0 = cadena resultado
                nb_pop_x1(cb);                                          // x1 = buffer
                nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16)); // soltar la longitud
                nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16)); // soltar la cadena
                nb_push_x0(cb);                                        // pila: el resultado
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));  // x0 = buffer
                if (free_fn) nb_emit_bl(cb, free_fn);
                nb_pop_x0(cb);                                          // x0 = el resultado
                return;
            }
            // ---- String$, Hex$, Bin$, LSet$, RSet$ ----
            // Se componen en la pila y se convierten en cadena con
            // nb_string_new. Tope de 1024 caracteres: lo que pase de ahi se
            // corta (y se documenta), en vez de escribir fuera de la pila.
            if ((n->list_count == 2 && (nb_text_eq_cstr(n->text, "String$") || nb_text_eq_cstr(n->text, "LSet$") || nb_text_eq_cstr(n->text, "RSet$"))) ||
                (n->list_count == 1 && (nb_text_eq_cstr(n->text, "Hex$") || nb_text_eq_cstr(n->text, "Bin$")))) {
                nb_label_t *fn_new = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                nb_label_t *fn_cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                nb_label_t *fn_len = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_len", n->line) : NULL;
                bool hexbin = n->list_count == 1;
                nb_codebuf_emit(cb, nb_enc_sub_imm(NB_REG_SP, NB_REG_SP, 1088));   // el hueco de trabajo
                nb_codebuf_emit(cb, nb_enc_add_imm(24, NB_REG_SP, 0));             // x24 = principio
                nb_codebuf_emit(cb, nb_enc_orr_reg(25, NB_REG_ZR, 24));            // x25 = donde se escribe (el 9 lo usa el compilador para las direcciones)
                if (hexbin) {
                    g_usa_digitos = true;
                    nb_emit_expr_int(ctx, n->list[0]);
                    nb_emit_load_imm64(cb, 1, nb_text_eq_cstr(n->text, "Hex$") ? 16 : 2);
                    nb_emit_bl(cb, g_h_digitos);
                } else {
                    g_usa_copia = true;
                    nb_emit_expr_string(ctx, n->list[0]);
                    nb_codebuf_emit(cb, nb_enc_orr_reg(19, NB_REG_ZR, 0));         // x19 = la cadena
                    if (fn_len) nb_emit_bl(cb, fn_len);
                    nb_codebuf_emit(cb, nb_enc_orr_reg(21, NB_REG_ZR, 0));         // x21 = su longitud
                    nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 19));
                    if (fn_cstr) nb_emit_bl(cb, fn_cstr);
                    nb_codebuf_emit(cb, nb_enc_orr_reg(20, NB_REG_ZR, 0));         // x20 = sus bytes
                    nb_emit_expr_int(ctx, n->list[1]);
                    nb_codebuf_emit(cb, nb_enc_orr_reg(22, NB_REG_ZR, 0));         // x22 = veces o ancho
                    if (nb_text_eq_cstr(n->text, "String$")) {
                        // repetir x22 veces, sin pasar de 1024
                        nb_label_t *bucle = nb_label_new(), *fin = nb_label_new();
                        nb_label_define(cb, bucle);
                        nb_emit_cbz(cb, 22, fin);
                        nb_codebuf_emit(cb, nb_enc_sub_reg(23, 25, 24));           // lo escrito
                        nb_codebuf_emit(cb, nb_enc_add_reg(23, 23, 21, 0));
                        nb_emit_load_imm64(cb, 2, 1024);
                        nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 23, 2));
                        nb_emit_bcond(cb, NB_COND_HI, fin);                        // no cabe: se corta
                        nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 20));
                        nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 21));
                        nb_emit_bl(cb, g_h_copia);
                        nb_codebuf_emit(cb, nb_enc_sub_imm(22, 22, 1));
                        nb_emit_b(cb, bucle);
                        nb_label_define(cb, fin);
                        nb_label_free(bucle); nb_label_free(fin);
                    } else {
                        // LSet$/RSet$: al ancho x22, cortando o rellenando con espacios
                        bool izquierda = nb_text_eq_cstr(n->text, "LSet$");
                        nb_emit_load_imm64(cb, 2, 1024);
                        nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 22, 2));
                        nb_label_t *ok_ancho = nb_label_new();
                        nb_emit_bcond(cb, NB_COND_LS, ok_ancho);
                        nb_codebuf_emit(cb, nb_enc_orr_reg(22, NB_REG_ZR, 2));
                        nb_label_define(cb, ok_ancho);
                        nb_label_free(ok_ancho);
                        nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 21, 22));   // ¿mas larga que el ancho?
                        nb_label_t *cabe = nb_label_new();
                        nb_emit_bcond(cb, NB_COND_LS, cabe);
                        nb_codebuf_emit(cb, nb_enc_orr_reg(21, NB_REG_ZR, 22));    // se corta
                        nb_label_define(cb, cabe);
                        nb_label_free(cabe);
                        nb_codebuf_emit(cb, nb_enc_sub_reg(23, 22, 21));           // x23 = espacios
                        nb_label_t *huecos = nb_label_new(), *fin_h = nb_label_new();
                        if (!izquierda) {                                          // RSet$: primero los espacios
                            nb_label_define(cb, huecos);
                            nb_emit_cbz(cb, 23, fin_h);
                            nb_codebuf_emit(cb, nb_enc_movz(2, ' ', 0));
                            nb_codebuf_emit(cb, nb_enc_strb_post(2, 25, 1));
                            nb_codebuf_emit(cb, nb_enc_sub_imm(23, 23, 1));
                            nb_emit_b(cb, huecos);
                            nb_label_define(cb, fin_h);
                        }
                        nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 20));
                        nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 21));
                        nb_emit_bl(cb, g_h_copia);
                        if (izquierda) {                                           // LSet$: los espacios detras
                            nb_label_define(cb, huecos);
                            nb_emit_cbz(cb, 23, fin_h);
                            nb_codebuf_emit(cb, nb_enc_movz(2, ' ', 0));
                            nb_codebuf_emit(cb, nb_enc_strb_post(2, 25, 1));
                            nb_codebuf_emit(cb, nb_enc_sub_imm(23, 23, 1));
                            nb_emit_b(cb, huecos);
                            nb_label_define(cb, fin_h);
                        }
                        nb_label_free(huecos); nb_label_free(fin_h);
                    }
                }
                nb_codebuf_emit(cb, nb_enc_sub_reg(1, 25, 24));                    // x1 = cuantos bytes
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 24));
                if (fn_new) nb_emit_bl(cb, fn_new);
                nb_codebuf_emit(cb, nb_enc_add_imm(NB_REG_SP, NB_REG_SP, 1088));
                return;
            }
            // NextFile$(carpeta): el siguiente nombre, o "" cuando no quedan
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "NextFile$")) {
                nb_label_t *fn_new = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                nb_emit_expr_int(ctx, n->list[0]);
                nb_codebuf_emit(cb, nb_enc_sub_imm(NB_REG_SP, NB_REG_SP, 64));
                nb_codebuf_emit(cb, nb_enc_orr_reg(19, NB_REG_ZR, 0));
                nb_codebuf_emit(cb, nb_enc_add_imm(20, NB_REG_SP, 0));
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 20));
                nb_emit_load_imm64(cb, 2, 48);
                nb_emit_load_imm64(cb, 8, 79);                            // SYS_DIR_NEXT
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));     // x1 = longitud (0 = no quedan)
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 20));
                if (fn_new) nb_emit_bl(cb, fn_new);
                nb_codebuf_emit(cb, nb_enc_add_imm(NB_REG_SP, NB_REG_SP, 64));
                return;
            }
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "ReadLine$")) {
                // Mismo patron que GadgetText$: buffer temporal del
                // monton, el kernel lo rellena, y se envuelve con
                // nb_string_new para devolver una cadena con conteo de
                // referencias como cualquier otra.
                // El buffer va a la PILA, no a x9: x9 es NB_ADDR_REG,
                // el registro que el generador usa para las
                // direcciones de variables globales. Evaluar el
                // handle (que normalmente ES una variable global) lo
                // pisaria, y nb_string_new recibiria basura.
                // Mismo error que ya se cometio una vez con la
                // asignacion a globales -- cualquier cosa que deba
                // sobrevivir a una evaluacion va a la pila.
                nb_codebuf_emit(cb, nb_enc_movz(0, 1024, 0));
                nb_label_t *alloc_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_alloc", n->line) : NULL;
                if (alloc_fn) nb_emit_bl(cb, alloc_fn);
                nb_push_x0(cb);                                        // pila: buffer
                nb_emit_expr_int(ctx, n->list[0]);                     // x0 = handle
                nb_pop_x1(cb);                                         // x1 = buffer
                nb_push_x1(cb);                                        // pila: buffer (lo necesitamos luego)
                nb_codebuf_emit(cb, nb_enc_movz(2, 1024, 0));          // x2 = tamano
                nb_emit_load_imm64(cb, 8, 42);
                nb_codebuf_emit(cb, nb_enc_svc(0));                    // x0 = longitud
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));  // x1 = longitud
                nb_pop_x0(cb);                                          // x0 = buffer
                nb_push_x0(cb);                                        // pila: buffer (para liberarlo)
                nb_label_t *new_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                if (new_fn) nb_emit_bl(cb, new_fn);                    // x0 = cadena
                nb_pop_x1(cb);                                          // x1 = buffer
                nb_push_x0(cb);                                        // pila: la cadena resultado
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));  // x0 = buffer
                nb_label_t *free_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_free", n->line) : NULL;
                if (free_fn) nb_emit_bl(cb, free_fn);
                nb_pop_x0(cb);                                          // x0 = la cadena
                return;
            }

            // ---- Rnd, Seed, MilliSecs ----
            //
            // Rnd(n)      -> entero en [0, n-1]   (nb_rnd en el runtime)
            // Rnd#()      -> flotante en [0.0,1.0) (nb_rnd_float)
            // Seed(n)     -> fijar el generador    (nb_rnd_seed)
            // MilliSecs() -> milisegundos desde que arranco el sistema
            //                (SYS_GET_TICKS * 10, el timer va a 100 Hz)
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "Rnd")) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_label_t *fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_rnd", n->line) : NULL;
                if (fn) nb_emit_bl(cb, fn);
                return;
            }
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "Rnd")) {
                nb_label_t *fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_rnd_float", n->line) : NULL;
                if (fn) nb_emit_bl(cb, fn);
                nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0)); // bits del flotante a x0
                return;
            }
            // Rand(a, b) -> un entero de a a b, los dos incluidos.
            //
            // No es un alias de Rnd: Rnd(n) da de 0 a n-1, y el rango que
            // se quiere casi siempre es "de 1 a 6", no "de 0 a 5". Ponerlo
            // como alias haria que Rand(1, 6) diera un error de arriba
            // abajo, o peor, que alguien lo escribiera pensando en otra
            // cosa.
            if (n->list_count == 2 && nb_text_eq_cstr(n->text, "Rand")) {
                nb_emit_expr_int(ctx, n->list[0]);                      // x0 = a
                nb_push_x0(cb);
                nb_emit_expr_int(ctx, n->list[1]);                      // x0 = b
                // 'a' se MIRA sin sacarlo: la llamada de abajo se lleva
                // por delante x1, asi que tiene que seguir en la pila.
                nb_codebuf_emit(cb, nb_enc_ldr_imm(1, NB_REG_SP, 0));   // x1 = a
                nb_codebuf_emit(cb, nb_enc_sub_reg(0, 0, 1));           // x0 = b - a
                nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 1));           // x0 = b - a + 1
                // Con b < a el rango sale cero o negativo, y nb_rnd no
                // sabe que hacer con eso. Se toma como 1: Rand(6, 1)
                // devuelve 6. Un numero raro es mejor que una division
                // entre cero en mitad de un juego.
                nb_codebuf_emit(cb, nb_enc_movz(2, 1, 0));              // x2 = 1
                nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 1));  // cmp x0, #1
                nb_codebuf_emit(cb, nb_enc_csel(0, 0, 2, NB_COND_GE));  // x0 = max(rango, 1)
                nb_label_t *fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_rnd", n->line) : NULL;
                if (fn) nb_emit_bl(cb, fn);                             // x0 = 0 .. rango-1
                nb_pop_x1(cb);                                          // x1 = a
                nb_codebuf_emit(cb, nb_enc_add_reg(0, 0, 1, 0));        // x0 = a + eso
                return;
            }
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "Seed")) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_label_t *fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_rnd_seed", n->line) : NULL;
                if (fn) nb_emit_bl(cb, fn);
                return;
            }
            // MicroSecs: microsegundos desde el arranque,
            // del contador generico de ARM. MilliSecs() sale del reloj del
            // planificador y solo da saltos de 10 ms -- con el no se puede
            // medir un fotograma ni hacer fisica por tiempo transcurrido.
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "MicroSecs")) {
                nb_emit_load_imm64(cb, 8, 272);         // SYS_MICROS
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // ReadChar: la siguiente tecla pendiente, o 0 si
            // no hay ninguna. NO espera -- por eso devuelve 0 en vez de
            // quedarse parada, y por eso se puede llamar desde el bucle
            // principal de un programa con ventana sin congelar nada.
            //
            // Existe porque el TextArea del kernel no vale para un editor de
            // verdad: guarda 200 lineas y al llenarse tira la mas antigua, y
            // parte solas las lineas largas guardando el salto de verdad. Las
            // dos cosas son perdida de datos en algo que despues guarda. Con
            // esto, un programa en Nemo Basic puede llevar su propio buffer
            // de texto y no depender de ese gadget.
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "ReadChar")) {
                nb_emit_load_imm64(cb, 8, 12);          // SYS_READ_CHAR
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "MilliSecs")) {
                nb_emit_load_imm64(cb, 8, 2);           // SYS_GET_TICKS
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_codebuf_emit(cb, nb_enc_movz(1, 10, 0)); // x1 = 10
                nb_codebuf_emit(cb, nb_enc_mul(0, 0, 1));   // x0 = ticks * 10 = ms
                return;
            }

            // ---- Gadgets: leer y manipular ----
            //
            // GadgetText$(id)              -> texto del gadget (TextField o Button)
            // AddGadgetItem(id, texto$)    -> añadir elemento a un ListBox
            // ClearGadgetItems(id)         -> vaciar un ListBox
            // SelectedGadgetItem(id)       -> indice seleccionado en ListBox (-1 si ninguno)
            // SelectGadgetItem(id, indice) -> seleccionar por indice
            // CountGadgetItems(id)         -> numero de elementos
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "GadgetText$")) {
                // Pedir el texto al kernel (syscall 106) en un buffer temporal
                // del montón, luego envolver con nb_string_new para que el
                // resultado sea un nb_string_t con conteo de referencias.
                // Buffer de 256 bytes: suficiente para cualquier campo de texto.
                // El buffer va a la PILA, no a x9 -- ver la nota en
                // ReadLine$: x9 es NB_ADDR_REG y evaluar el id, que
                // normalmente es una variable global, lo pisa.
                nb_codebuf_emit(cb, nb_enc_movz(0, 256, 0));
                nb_label_t *alloc_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_alloc", n->line) : NULL;
                if (alloc_fn) nb_emit_bl(cb, alloc_fn);    // x0 = buffer
                nb_push_x0(cb);                             // pila: buffer
                nb_emit_expr_int(ctx, n->list[0]);          // x0 = id
                nb_pop_x1(cb);                              // x1 = buffer
                nb_push_x1(cb);                             // pila: buffer
                nb_codebuf_emit(cb, nb_enc_movz(2, 256, 0)); // x2 = tamaño
                nb_emit_load_imm64(cb, 8, 106);
                nb_codebuf_emit(cb, nb_enc_svc(0));          // x0 = longitud
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0)); // x1 = len
                nb_pop_x0(cb);                               // x0 = buffer
                nb_push_x0(cb);                             // pila: buffer
                nb_label_t *new_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_new", n->line) : NULL;
                if (new_fn) nb_emit_bl(cb, new_fn);          // x0 = cadena
                nb_pop_x1(cb);                               // x1 = buffer
                nb_push_x0(cb);                             // pila: la cadena
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1)); // x0 = buffer
                nb_label_t *free_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_free", n->line) : NULL;
                if (free_fn) nb_emit_bl(cb, free_fn);
                nb_pop_x0(cb);                               // x0 = la cadena
                return;
            }
            if (n->list_count == 2 && nb_text_eq_cstr(n->text, "AddGadgetItem")) {
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);  // id
                nb_emit_expr_string(ctx, n->list[1]);
                nb_label_t *cstr = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", n->line) : NULL;
                if (cstr) nb_emit_bl(cb, cstr);
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0)); // x1 = texto
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16)); // x0 = id
                nb_emit_load_imm64(cb, 8, 114);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            if (n->list_count == 1 && nb_text_eq_cstr(n->text, "ClearGadgetItems")) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_emit_load_imm64(cb, 8, 115);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            if (n->list_count == 1 && (nb_text_eq_cstr(n->text, "SelectedGadgetItem") ||
                                        nb_text_eq_cstr(n->text, "CountGadgetItems"))) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_emit_load_imm64(cb, 8, nb_text_eq_cstr(n->text, "SelectedGadgetItem") ? 116 : 118);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            if (n->list_count == 2 && nb_text_eq_cstr(n->text, "SelectGadgetItem")) {
                nb_emit_expr_int(ctx, n->list[0]); nb_push_x0(cb);
                nb_emit_expr_int(ctx, n->list[1]);
                nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 0));
                nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));
                nb_emit_load_imm64(cb, 8, 117);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }

            // ---- Entrada: teclado y raton ----
            //
            // Son FUNCIONES (devuelven un valor), no comandos, asi que
            // van por la ruta de expresiones y no por nb_emit_stmt.
            //   KeyDown(codigo) -> 1 si esa tecla esta pulsada AHORA (syscall 48)
            //   KeyHit(codigo)  -> 1 si se pulso desde la ultima vez (49... ver abajo: 53)
            //   GetKey()        -> siguiente codigo de la cola, o 0 (54)
            //   ReadChar()      -> siguiente LETRA de la cola, o 0 (12)
            //   MouseX()/MouseY()/MouseDown() -> del mismo SYS_GET_MOUSE (34)
            //   Pump()          -> cede el turno al resto del sistema (14)
            //
            // Los codigos de tecla son los del driver (estilo evdev),
            // no los de BlitzPlus: 1=Esc, 28=Enter, 57=Espacio,
            // 103=Arriba, 105=Izquierda, 106=Derecha, 108=Abajo.
            if (n->list_count == 1 && (nb_text_eq_cstr(n->text, "KeyDown") || nb_text_eq_cstr(n->text, "KeyHit"))) {
                nb_emit_expr_int(ctx, n->list[0]);
                nb_emit_load_imm64(cb, 8, nb_text_eq_cstr(n->text, "KeyDown") ? 48 : 53);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // ---- Tiras de pixeles de una fila ----
            //
            //   RowRun(imagen, x, y, maximo, color)
            //   RowSkip(imagen, x, y, maximo, color)
            //
            // Cuantos pixeles seguidos, empezando en (x, y) y avanzando por
            // la fila, SON de ese color (RowRun) o NO lo son (RowSkip). Con
            // 'maximo' negativo se avanza hacia la IZQUIERDA. El pixel de
            // partida cuenta si cumple. Se para en el borde de la imagen.
            //
            // Son la pieza para mirar pixeles en cantidad sin pagar un peaje
            // por cada uno: el recorrido pasa dentro del kernel, a 4 ns el
            // pixel en vez de 3,5 us la llamada. Con ellas, medir hasta
            // donde llega una pared en una mascara de colisiones es UNA
            // llamada, y un relleno por inundacion pasa de costar por pixel
            // a costar por tramo.
            //
            // Son dos comandos y no uno con un argumento de modo por dos
            // razones: en Basic se lee mucho mejor que un 0/1 magico, y la
            // tabla de firmas guarda 5 letras como maximo (char firma[6]),
            // asi que un sexto argumento no se podria comprobar.
            //
            // El modo va en x5, que la syscall 292 usa para eso. El +1 de la
            // imagen, como en FillRow: el hueco 0 es una imagen valida.
            if (n->list_count == 5 && (nb_text_eq_cstr(n->text, "RowRun") || nb_text_eq_cstr(n->text, "RowSkip"))) {
                bool es_skip = nb_text_eq_cstr(n->text, "RowSkip");
                for (int32_t i = 0; i < 5; i++) {
                    nb_emit_expr_int(ctx, n->list[i]);
                    if (i == 0) nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 1));
                    nb_push_x0(cb);
                }
                for (int32_t i = 4; i >= 0; i--) nb_codebuf_emit(cb, nb_enc_ldr_post(i, NB_REG_SP, 16));
                nb_codebuf_emit(cb, nb_enc_movz(5, es_skip ? 1 : 0, 0));   // x5 = modo
                nb_emit_load_imm64(cb, 8, 292);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "GetKey")) {
                nb_emit_load_imm64(cb, 8, 54);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            // ReadChar() -- la pareja de GetKey, y la que hace falta para
            // escribir un editor de texto. GetKey da el CODIGO de la tecla
            // (103 = flecha arriba); ReadChar da la LETRA ya traducida, con
            // las mayusculas y los acentos puestos por el driver.
            //
            // Las teclas que no tienen letra llegan igual, como codigos de
            // control sin uso, asi que un solo bucle de ReadChar sirve para
            // todo: 8 borrar, 10 salto de linea, 9 tabulador, 0x11 arriba,
            // 0x12 abajo, 0x13 izquierda, 0x14 derecha, 0x15 inicio, 0x16
            // fin, 0x17 pagina arriba, 0x18 pagina abajo, 0x7F suprimir.
            // Devuelve 0 cuando no queda ninguna pendiente.
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "ReadChar")) {
                nb_emit_load_imm64(cb, 8, 12);   // SYS_READ_CHAR
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            if (n->list_count == 0 && nb_text_eq_cstr(n->text, "Pump")) {
                nb_emit_load_imm64(cb, 8, 14); // SYS_PUMP -- imprescindible en un bucle: el planificador es cooperativo
                nb_codebuf_emit(cb, nb_enc_svc(0));
                return;
            }
            if (n->list_count == 0 && (nb_text_eq_cstr(n->text, "MouseX") ||
                                       nb_text_eq_cstr(n->text, "MouseY") ||
                                       nb_text_eq_cstr(n->text, "MouseDown"))) {
                // SYS_GET_MOUSE devuelve TODO junto:
                //   bits[47:32] = x, bits[31:16] = y, bits[2:0] = botones
                // ...o -1 entero si el raton esta fuera de la ventana o
                // no tiene el foco. En ese caso devolvemos 0, que es lo
                // menos sorprendente para un programa que solo pregunta
                // "¿donde esta el raton?".
                nb_emit_load_imm64(cb, 8, 34);
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0));    // cmp x0, #0
                nb_codebuf_emit(cb, nb_enc_csel(0, 0, NB_REG_ZR, NB_COND_GE)); // si es negativo (-1), quedarse con 0
                if (nb_text_eq_cstr(n->text, "MouseX")) {
                    nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, 32));
                    nb_codebuf_emit(cb, nb_enc_and_imm(0, 0, 0xFFFFu));
                } else if (nb_text_eq_cstr(n->text, "MouseY")) {
                    nb_codebuf_emit(cb, nb_enc_lsr_imm(0, 0, 16));
                    nb_codebuf_emit(cb, nb_enc_and_imm(0, 0, 0xFFFFu));
                } else {
                    nb_codebuf_emit(cb, nb_enc_and_imm(0, 0, 1u)); // solo el boton izquierdo
                }
                return;
            }
            nb_label_t *target = NULL;
            nb_func_entry_t *fe = nb_func_table_find(ctx->functions, n->text);
            if (fe) {
                target = fe->label;
            } else if (ctx->runtime) {
                const char *runtime_name = NULL;
                if (nb_text_eq_cstr(n->text, "Left$")) runtime_name = "nb_string_left";
                else if (nb_text_eq_cstr(n->text, "Right$")) runtime_name = "nb_string_right";
                else if (nb_text_eq_cstr(n->text, "Mid$")) runtime_name = "nb_string_mid";
                else if (nb_text_eq_cstr(n->text, "Len")) runtime_name = "nb_string_len";
                else if (nb_text_eq_cstr(n->text, "Val")) runtime_name = "nb_str_to_int";
                if (runtime_name) target = nb_rt_obligatoria(ctx, runtime_name, n->line);
            }
            if (!target) {
                // Llamada a algo que no existe: ni funcion de usuario,
                // ni array declarado, ni incorporada reconocida. Antes
                // se iba en silencio y la llamada desaparecia, dejando
                // en x0 lo que hubiera -- un programa que "funciona"
                // devolviendo basura.
                nb_codegen_error("llamada a una funcion que no existe", n->line);
                return;
            }

            // Limite deliberado por ahora: hasta 8 argumentos, los
            // que caben en x0-x7 segun el convenio de llamada de
            // ARM64 -- mas alla de eso haria falta pasar el resto por
            // la pila, que es una ampliacion futura si algun programa
            // real la necesita.
            int32_t nargs = n->list_count;
            // Pasarse de ocho NO se recorta en silencio. Antes si: el noveno
            // argumento se tiraba y llegaba como cero, asi que la funcion
            // trabajaba con un dato que nadie habia escrito y el programa
            // compilaba limpio. Un error aqui cuesta una linea de arreglo;
            // el cero de mas cuesta una tarde.
            if (nargs > 8) {
                nb_codegen_error("como mucho ocho argumentos en una llamada", n->line);
                return;
            }
            for (int32_t i = 0; i < nargs; i++) {
                // A una funcion PROPIA, cada argumento con el tipo de su
                // parametro (F(5) con "Function F(x#)" convierte el 5).
                if (fe && fe->def && i < fe->def->list_count &&
                    fe->def->list[i]->op == NB_PARAM_ARRAY) {
                    // Un parametro "nombre()": lo que se pasa es el PUNTERO al
                    // bloque del array, sin copiar nada y sin tocar el contador
                    // de referencias -- no es una cadena aunque lleve $.
                    nb_node_t *arg = n->list[i];
                    // Un nombre suelto llega como N_VAR, o como N_CALL sin
                    // argumentos si el parser no pudo distinguirlo. Igual que
                    // en ReadRow: lo que decide es que este declarado con Dim.
                    if (!((arg->kind == N_VAR || (arg->kind == N_CALL && arg->list_count == 0)) &&
                          arg->text && ctx->arrays && nb_array_set_contains(ctx->arrays, arg->text))) {
                        nb_codegen_error("ese parametro necesita un array declarado con Dim", n->line);
                        return;
                    }
                    // Un array de numeros donde se espera uno de cadenas (o al
                    // reves) no da ningun error al ejecutarse: la funcion lee
                    // numeros como si fueran direcciones de texto. Se para aqui.
                    if (nb_infer_type(arg->text) != nb_infer_type(fe->def->list[i]->text)) {
                        nb_codegen_error("el array que se pasa no es del mismo tipo que el parametro", n->line);
                        return;
                    }
                    nb_emit_load_var_x0(ctx, arg->text);
                    if (ctx->bounds_error_label) { nb_emit_si_cero_error(cb, 0, g_err_dim); g_usa_dim = true; }
                } else if (fe && fe->def && i < fe->def->list_count)
                    nb_emit_expr_para(ctx, n->list[i], nb_infer_type(fe->def->list[i]->text));
                else {
                    // A una orden incorporada, cada argumento con el tipo que
                    // dice su firma: un decimal donde pide entero se convierte
                    const char *firma = nb_firma_de(n->text);
                    bool entero = false;
                    if (firma) {
                        int32_t k = 0;
                        while (firma[k] && k < i) k++;
                        entero = (k == i && firma[k] == 'N');
                    }
                    if (entero)
                        nb_emit_expr_int(ctx, n->list[i]);
                    else
                        nb_emit_expr(ctx, n->list[i]);
                }
                nb_push_x0(cb);
            }
            // Se recuperan en ORDEN INVERSO al de empuje: lo ultimo
            // empujado (el argumento N-1) es lo primero que sale de
            // la pila, y va al registro N-1; asi cada argumento acaba
            // en el registro que le corresponde.
            for (int32_t i = nargs - 1; i >= 0; i--) {
                nb_codebuf_emit(cb, nb_enc_ldr_post(i, NB_REG_SP, 16));
            }
            nb_emit_bl(cb, target);
            // El resultado ya esta en x0. Si la funcion devuelve
            // DECIMAL, en x0 estan sus BITS: quien esperaba un entero (una
            // coordenada, un indice) necesita el numero, no los bits. Solo el
            // camino decimal pide los bits tal cual.
            if (!g_quiere_bits && nb_expr_type(n) == NB_TYPE_FLOAT) {
                nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(0, 0));
                nb_codebuf_emit(cb, nb_enc_fcvtzs(0, 0));
            }
            return;
        }
        case N_INDEX: {
            nb_emit_array_addr(ctx, n);
            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 0, 0)); // valor entero en esa direccion
            return;
        }
        default:
            return; // el resto de tipos de nodo llegan en piezas siguientes
    }
}

// ---------------------------------------------------------------
// Inferencia de tipos: entero, flotante o cadena, segun el sufijo del
// nombre ($=cadena, #=flotante, nada o %=entero) y la regla de que si
// cualquiera de los dos lados de una suma/resta/multiplicacion/
// division es flotante, el resultado tambien lo es. Las comparaciones
// y las operaciones a nivel de bit (And/Or/Xor/Mod/Shl/Shr/Sar) son
// SIEMPRE enteras en este diseño -- mezclar un operando flotante con
// una de estas ultimas (ej. "5.0 And 3") no esta cubierto todavia: el
// patron de bits del flotante se reinterpretaria tal cual como un
// entero, sin ninguna conversion, dando un resultado sin sentido. No
// es un caso que aparezca en programas BASIC normales, y arreglarlo
// bien (convertir primero) es una ampliacion pequeña pero aparte.

extern uint32_t nb_string_len(nb_string_t *s);
extern const char *nb_string_cstr(nb_string_t *s);

static nb_var_type_t nb_infer_type(nb_string_t *name) {
    uint32_t len = nb_string_len(name);
    if (len == 0) return NB_TYPE_INT;
    char last = nb_string_cstr(name)[len - 1];
    if (last == '$') return NB_TYPE_STRING;
    if (last == '#') return NB_TYPE_FLOAT;
    return NB_TYPE_INT; // sin sufijo, o '%' -- entero
}

// No es static: el PARSER la necesita para Select (ver nb_parse_select en
// nb_parser.c). La variable temporal que Select fabrica tiene que llevar el
// sufijo del tipo de la expresion, y para saberlo hay que mirar el arbol.
// Se comparte esta en vez de escribir una segunda en el parser: dos
// inferencias de tipo acaban siendo dos reglas distintas.
nb_var_type_t nb_expr_type(nb_node_t *n) {
    switch (n->kind) {
        case N_NUM: return n->op ? NB_TYPE_FLOAT : NB_TYPE_INT; // n->op=1 marca "llevaba punto decimal" (ver nb_ast.h)
        case N_STR: return NB_TYPE_STRING;
        case N_VAR: if (nb_es_pi(n->text)) return NB_TYPE_FLOAT; return nb_infer_type(n->text);
        case N_FIELD: case N_INDEX: return nb_infer_type(n->text);
        case N_CALL:
            // Las incorporadas de RESULTADO FLOTANTE se reconocen por
            // nombre, no por sufijo: "Sqr(x)" y "Float(n)" devuelven
            // un flotante aunque se escriban sin '#'. Sin esto, la
            // inferencia las daria por enteras y una expresion como
            // "Sqr(Float(16))" convertiria los BITS del flotante a
            // flotante OTRA VEZ -- fallo real observado: Sqr(16)
            // devolvia 2150627075, que es exactamente la raiz de los
            // bits de 16.0 leidos como entero.
            if (nb_text_eq_cstr(n->text, "Sqr") || nb_text_eq_cstr(n->text, "Sqr#") ||
                nb_text_eq_cstr(n->text, "Float") || nb_text_eq_cstr(n->text, "Float#") ||
                nb_text_eq_cstr(n->text, "Abs#") ||
                nb_text_eq_cstr(n->text, "Sin") || nb_text_eq_cstr(n->text, "Sin#") ||
                nb_text_eq_cstr(n->text, "Cos") || nb_text_eq_cstr(n->text, "Cos#") ||
                nb_text_eq_cstr(n->text, "Tan") || nb_text_eq_cstr(n->text, "Tan#") ||
                nb_text_eq_cstr(n->text, "ATan") || nb_text_eq_cstr(n->text, "ATan#") ||
                nb_text_eq_cstr(n->text, "Exp") || nb_text_eq_cstr(n->text, "Exp#") ||
                nb_text_eq_cstr(n->text, "Log") || nb_text_eq_cstr(n->text, "Log#") ||
                nb_text_eq_cstr(n->text, "Floor") || nb_text_eq_cstr(n->text, "Floor#") ||
                nb_text_eq_cstr(n->text, "Ceil") || nb_text_eq_cstr(n->text, "Ceil#") ||
                (nb_text_eq_cstr(n->text, "Rnd") && n->list_count == 0)) return NB_TYPE_FLOAT;
            // Abs/Min/Max sin sufijo: decimales si algun argumento lo es.
            if ((nb_text_eq_cstr(n->text, "Abs") && n->list_count == 1 && nb_expr_type(n->list[0]) == NB_TYPE_FLOAT) ||
                ((nb_text_eq_cstr(n->text, "Min") || nb_text_eq_cstr(n->text, "Max")) && n->list_count == 2 &&
                 (nb_expr_type(n->list[0]) == NB_TYPE_FLOAT || nb_expr_type(n->list[1]) == NB_TYPE_FLOAT))) return NB_TYPE_FLOAT;
            return nb_infer_type(n->text);
        case N_UNOP: return nb_expr_type(n->a);
        case N_BINOP:
            if (n->op == TOK_PLUS || n->op == TOK_MINUS || n->op == TOK_STAR || n->op == TOK_SLASH) {
                nb_var_type_t ta = nb_expr_type(n->a), tb = nb_expr_type(n->b);
                if (ta == NB_TYPE_STRING || tb == NB_TYPE_STRING) return NB_TYPE_STRING; // concatenacion -- pieza aparte, cadenas
                if (ta == NB_TYPE_FLOAT || tb == NB_TYPE_FLOAT) return NB_TYPE_FLOAT;
                return NB_TYPE_INT;
            }
            return NB_TYPE_INT; // comparaciones y operaciones a nivel de bit -- siempre enteras
        default: return NB_TYPE_INT;
    }
}

static void nb_emit_expr_float(nb_codegen_ctx_t *ctx, nb_node_t *n);

// Evalua 'n' como flotante EN d0, promoviendo desde entero (scvtf) si
// hiciera falta -- lo usan los operandos de un N_BINOP flotante, para
// que "3 + 2.5" evalue el 3 como entero de verdad (nb_emit_expr_int)
// y lo convierta, en vez de reinterpretar sus bits como si ya fueran
// un double.
static void nb_emit_expr_as_float(nb_codegen_ctx_t *ctx, nb_node_t *n) {
    if (nb_expr_type(n) == NB_TYPE_FLOAT) { nb_emit_expr_float(ctx, n); return; }
    nb_emit_expr_int(ctx, n); // -> x0, un entero de verdad
    nb_codebuf_emit(ctx->cb, nb_enc_scvtf(0, 0)); // d0 = (double)x0
}

// Evalua una expresion FLOTANTE -- el resultado queda en d0 al
// terminar (a diferencia de nb_emit_expr_int, que deja el suyo en
// x0 -- el despachador nb_emit_expr, mas abajo, es quien unifica los
// dos convenios de cara a quien generó la llamada).
static void nb_emit_expr_float(nb_codegen_ctx_t *ctx, nb_node_t *n) {
    nb_codebuf_t *cb = ctx->cb;
    switch (n->kind) {
        case N_NUM: {
            // Se reinterpretan los mismos 8 bytes del double como un
            // entero de 64 bits (union -- la forma bien definida en C
            // de hacer esto, a diferencia de un cast de punteros entre
            // tipos incompatibles), se cargan con movz/movk como
            // cualquier entero, y fmov los traslada a d0 SIN
            // convertir el valor -- son los mismos bits, solo cambia
            // el registro que los interpreta.
            union { double d; uint64_t bits; } u;
            u.d = n->num_value;
            nb_emit_load_imm64(cb, 0, u.bits);
            nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(0, 0));
            return;
        }
        case N_VAR: {
            nb_emit_load_var_x0(ctx, n->text); // x0 = los bits ya guardados
            nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(0, 0));
            return;
        }
        case N_UNOP: {
            if (n->op == TOK_MINUS) {
                nb_emit_expr_float(ctx, n->a);
                nb_codebuf_emit(cb, nb_enc_fneg(0, 0));
            }
            // Not sobre un flotante: sin significado claro en este
            // lenguaje -- no soportado (documentado, no un fallo oculto).
            return;
        }
        case N_BINOP: {
            nb_emit_expr_as_float(ctx, n->a); // izquierda -> d0 (con promocion si hacia falta)
            nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0)); // x0 = bits de d0
            nb_push_x0(cb);
            nb_emit_expr_as_float(ctx, n->b); // derecha -> d0
            nb_pop_x1(cb);
            nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(1, 1)); // d1 = bits de x1 (izquierda, recuperada)
            // Ahora: d1=izquierda, d0=derecha -- mismo orden de
            // operandos que en la version entera.
            switch (n->op) {
                case TOK_PLUS:  nb_codebuf_emit(cb, nb_enc_fadd(0, 1, 0)); break;
                case TOK_MINUS: nb_codebuf_emit(cb, nb_enc_fsub(0, 1, 0)); break;
                case TOK_STAR:  nb_codebuf_emit(cb, nb_enc_fmul(0, 1, 0)); break;
                case TOK_SLASH: nb_codebuf_emit(cb, nb_enc_fdiv(0, 1, 0)); break;
                default: break; // las comparaciones nunca llegan aqui -- nb_expr_type las declara enteras
            }
            return;
        }
        case N_CALL: {
            // Una llamada a una funcion que devuelve flotante (por su
            // nombre, ej. "Media#(...)") reutiliza TAL CUAL la logica
            // entera de argumentos+bl (que ya evalua cada argumento
            // con el despachador, asi que un argumento flotante
            // tambien se pasa bien) -- solo cambia la conversion
            // final del resultado.
            {
                bool antes = g_quiere_bits;
                g_quiere_bits = true;                 // quiero los bits, sin convertir
                nb_emit_expr_int(ctx, n);
                g_quiere_bits = antes;
            }
            nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(0, 0));
            return;
        }
        case N_INDEX: {
            nb_emit_array_addr(ctx, n);
            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 0, 0)); // bits crudos en esa direccion
            nb_codebuf_emit(cb, nb_enc_fmov_gpr_to_fpr(0, 0)); // -> d0
            return;
        }
        default:
            return;
    }
}

// Evalua una expresion de CADENA -- el resultado es un nb_string_t*
// (una referencia que el llamador pasa a poseer) en x0, igual que
// cualquier otro valor: un puntero cabe tal cual en 64 bits, sin
// necesitar ningun truco como el fmov de los flotantes.
//
// LIMITACION DELIBERADA de esta pieza (documentada, no un fallo
// oculto): asignar una cadena nueva a una variable que ya tenia una
// referencia viva no libera la anterior todavia -- eso necesita saber
// si la variable ya tenia algo que soltar antes de sobreescribirla,
// que es una pieza de seguimiento de vida aparte. Por ahora, cada
// asignacion de cadena es una fuga de la referencia previa. Tambien
// quedan fuera: concatenacion (+) y comparacion (=, <>) de cadenas --
// las funciones que hacen falta (nb_string_concat, nb_string_eq) ya
// estan en el bloque de runtime, conectarlas es la siguiente pieza.
static void nb_emit_expr_string_bruto(nb_codegen_ctx_t *ctx, nb_node_t *n);

// Una expresion de cadena que NO es una variable guardada crea texto nuevo:
// se apunta para liberarlo al terminar la sentencia.
static void nb_emit_expr_string(nb_codegen_ctx_t *ctx, nb_node_t *n) {
    nb_emit_expr_string_bruto(ctx, n);
    if (nb_es_cadena_guardada(ctx, n)) return;
    if (!g_t_apuntar) return;
    g_usa_temporales = true;
    nb_push_x0(ctx->cb);
    nb_emit_bl(ctx->cb, g_t_apuntar);
    nb_pop_x1(ctx->cb);
    nb_codebuf_emit(ctx->cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));
}

static void nb_emit_expr_string_bruto(nb_codegen_ctx_t *ctx, nb_node_t *n) {
    nb_codebuf_t *cb = ctx->cb;
    switch (n->kind) {
        case N_STR: {
            uint32_t len = nb_string_len(n->text);
            uint32_t offset = nb_symtab_add_literal_bytes(ctx->globals, (const uint8_t *)nb_string_cstr(n->text), len);
            nb_emit_global_addr(ctx->globals, cb, 0, offset); // x0 = direccion de los bytes del literal
            nb_emit_load_imm64(cb, 1, len);                    // x1 = longitud
            nb_label_t *fn = nb_rt_obligatoria(ctx, "nb_string_new", n->line);
            if (fn) nb_emit_bl(cb, fn);
            return;
        }
        case N_VAR: {
            nb_emit_load_var_x0(ctx, n->text); // el puntero ya guardado, tal cual
            return;
        }
        case N_FIELD: {
            nb_emit_field_addr(ctx, n, 9);
            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 9, 0)); // el puntero guardado en ese campo, tal cual
            return;
        }
        case N_INDEX: {
            nb_emit_array_addr(ctx, n);
            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 0, 0)); // el puntero guardado en esa direccion, tal cual
            return;
        }
        case N_CALL: {
            // Str$ es un caso especial: no tiene una firma fija como
            // Left$/Right$/Mid$ -- hay que elegir nb_int_to_str o
            // nb_float_to_str segun el TIPO del argumento, no segun
            // el propio nombre. Y nb_float_to_str recibe un double DE
            // VERDAD (no un puntero) -- por el convenio real de
            // ARM64 (el de gcc, no el nuestro propio para funciones
            // .nb), eso viaja en d0, no en x0. Por eso aqui NO se usa
            // el mecanismo generico de argumentos (que solo mueve
            // enteros/punteros por x0-x7).
            if (nb_text_eq_cstr(n->text, "Str$") && n->list_count == 1) {
                nb_node_t *arg = n->list[0];
                nb_label_t *fn;
                if (nb_expr_type(arg) == NB_TYPE_FLOAT) {
                    nb_emit_expr_as_float(ctx, arg); // -> d0, justo donde nb_float_to_str lo espera
                    fn = nb_rt_obligatoria(ctx, "nb_float_to_str", n->line);
                } else {
                    nb_emit_expr_int(ctx, arg); // -> x0, donde nb_int_to_str lo espera (int64_t normal)
                    fn = nb_rt_obligatoria(ctx, "nb_int_to_str", n->line);
                }
                if (fn) nb_emit_bl(cb, fn);
                return;
            }
            // Una funcion de usuario que devuelve cadena (por su
            // nombre, ej. "Nombre$(...)"), o Left$/Right$/Mid$ --
            // reutiliza tal cual la logica de argumentos+bl; el
            // resultado (un puntero) ya queda en x0 sin necesitar
            // ninguna conversion.
            nb_emit_expr_int(ctx, n);
            return;
        }
        case N_BINOP: {
            if (n->op != TOK_PLUS) {
            // Entre cadenas solo esta definido el '+' (concatenar).
            // Antes, "a$ - b$" compilaba sin una queja y la operacion
            // desaparecia, dejando en x0 lo que hubiera.
            nb_codegen_error("entre cadenas solo se puede usar '+' (concatenar)", n->line);
            return;
        }
            if (nb_expr_type(n->a) != NB_TYPE_STRING || nb_expr_type(n->b) != NB_TYPE_STRING) {
                return; // mezclar con algo que no es cadena (ej. cad$+5) necesitaria
                        // convertir el numero a texto primero (Str$) -- pieza aparte
            }
            // Orden de evaluacion IZQUIERDA-DERECHA, igual que en
            // cualquier otro binario de este generador (importa si
            // alguno de los dos lados tiene efectos observables, como
            // una llamada a funcion) -- pero nb_string_concat(a,b)
            // quiere a en x0 y b en x1, y tras evaluar en orden
            // izquierda-derecha lo que queda es AL REVES (b en x0,
            // a en x1, recien recuperado de la pila) -- se
            // intercambian con x2 de por medio.
            nb_emit_expr_string(ctx, n->a); // -> x0 (izquierda)
            nb_push_x0(cb);
            nb_emit_expr_string(ctx, n->b); // -> x0 (derecha)
            nb_pop_x1(cb);                   // izquierda -> x1
            nb_codebuf_emit(cb, nb_enc_orr_reg(2, NB_REG_ZR, 0)); // x2 = derecha
            nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1)); // x0 = izquierda
            nb_codebuf_emit(cb, nb_enc_orr_reg(1, NB_REG_ZR, 2)); // x1 = derecha
            nb_label_t *fn = nb_rt_obligatoria(ctx, "nb_string_concat", n->line);
            if (fn) nb_emit_bl(cb, fn);
            return;
        }
        default:
            return; // comparacion: se maneja en nb_emit_expr_int, ver la nota alli
    }
}

// Punto de entrada UNICO y recomendado para evaluar cualquier
// expresion, de cualquier tipo -- decide sola si hace falta la ruta
// entera o la flotante, y en cualquier caso deja el resultado en x0
// (para flotantes, son los bits del double, no un entero convertido
// -- exactamente lo que una asignacion, un Return, o un argumento de
// llamada necesitan para guardarlo o pasarlo tal cual).
void nb_emit_expr(nb_codegen_ctx_t *ctx, nb_node_t *n) {
    nb_var_type_t t = nb_expr_type(n);
    if (t == NB_TYPE_FLOAT) {
        nb_emit_expr_float(ctx, n);
        nb_codebuf_emit(ctx->cb, nb_enc_fmov_fpr_to_gpr(0, 0));
        return;
    }
    if (t == NB_TYPE_STRING) {
        nb_emit_expr_string(ctx, n);
        return;
    }
    nb_emit_expr_int(ctx, n);
}


// ---- Evaluar CON EL TIPO DEL DESTINO ----
//
// Antes, al guardar un valor en algo con tipo -- variable, elemento de
// array, campo de un Type, parametro de una funcion, resultado de un
// Return -- se evaluaba la expresion y se guardaba x0 TAL CUAL. Con
// "x# = 5" se guardaban los bits del ENTERO 5 en una variable decimal
// (al leerla, 2.5e-323), y con "n = x#" los bits del decimal en un
// entero. No se notaba porque casi siempre se escribe "x# = 5.0", el
// 0 entero y el 0.0 decimal tienen los mismos bits, y las OPERACIONES
// si convertian ("x# + 1" funcionaba). Fallo silencioso de manual.
//
// Ahora todo valor que acaba en algo con tipo pasa por aqui: de entero a
// decimal, scvtf; de decimal a entero, fcvtzs (trunca hacia cero, como
// Int()); mezclar cadena y numero es error de compilacion (antes
// "x$ = 5" guardaba un 5 donde debia ir un puntero a cadena, y el
// programa caia al usarla). Para convertir a mano: Str$() y Val().
static nb_var_type_t g_tipo_retorno = NB_TYPE_INT;   // el de la funcion que se esta generando

static void nb_emit_expr_para(nb_codegen_ctx_t *ctx, nb_node_t *value, nb_var_type_t destino) {
    nb_var_type_t origen = nb_expr_type(value);
    if ((destino == NB_TYPE_STRING) != (origen == NB_TYPE_STRING)) {
        nb_codegen_error(destino == NB_TYPE_STRING
            ? "se guarda un numero en algo de cadena ($): conviertelo con Str$()"
            : "se guarda una cadena en algo numerico: conviertela con Val()", value->line);
        return;
    }
    if (destino == NB_TYPE_FLOAT && origen == NB_TYPE_INT) {
        nb_emit_expr_int(ctx, value);                              // x0 = entero
        nb_codebuf_emit(ctx->cb, nb_enc_scvtf(0, 0));              // d0 = (double)x0
        nb_codebuf_emit(ctx->cb, nb_enc_fmov_fpr_to_gpr(0, 0));    // bits del decimal a x0
        return;
    }
    if (destino == NB_TYPE_INT && origen == NB_TYPE_FLOAT) {
        nb_emit_expr_float(ctx, value);                            // d0 = decimal
        nb_codebuf_emit(ctx->cb, nb_enc_fcvtzs(0, 0));             // x0 = entero, hacia cero
        return;
    }
    nb_emit_expr(ctx, value);
}

// Copiar una cadena de otra variable, array o campo crea una
// SEGUNDA referencia al mismo texto: hay que contarla (nb_string_retain) o, al
// liberar una de las dos, la otra se queda apuntando a memoria libre. Lo que
// viene de una funcion o de una operacion (Mid$, +) ya es nuestro: no se
// cuenta dos veces.
// OJO: el analizador produce SIEMPRE un nodo de llamada para "v$(i)" -- es el
// generador quien decide si es un array mirando su lista (nb_array_set). Por
// eso aqui hay que preguntarselo igual, o un elemento de array no se reconoce
// como cadena guardada.
static bool nb_es_cadena_guardada(nb_codegen_ctx_t *ctx, nb_node_t *v) {
    if (!v) return false;
    if (v->kind == N_VAR || v->kind == N_INDEX || v->kind == N_FIELD) return true;
    if (v->kind == N_CALL && ctx->arrays && nb_array_set_contains(ctx->arrays, v->text)) return true;
    return false;
}
static void nb_emit_retener_si_copia(nb_codegen_ctx_t *ctx, nb_node_t *value) {
    (void)value;   // SIEMPRE: la variable se queda con su propia
                   // referencia, y el temporal se libera al final de la sentencia
    nb_label_t *ret = nb_rt_obligatoria(ctx, "nb_string_retain", value->line);
    if (!ret) return;
    // nb_string_retain no devuelve nada, asi que x0 queda indefinido despues de
    // llamarla: el puntero se guarda y se recupera alrededor de la llamada.
    nb_push_x0(ctx->cb);
    nb_emit_bl(ctx->cb, ret);
    nb_pop_x1(ctx->cb);
    nb_codebuf_emit(ctx->cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));
}

void nb_emit_assign_int(nb_codegen_ctx_t *ctx, nb_node_t *target, nb_node_t *value) {
    nb_codebuf_t *cb = ctx->cb;
    if (nb_es_pi(target->text)) { nb_codegen_error("Pi es una constante: no se le puede dar otro valor", target->line); return; }
    { int64_t v; if (nb_constante_sistema(target->text, &v)) { nb_codegen_error("es una constante del sistema (un evento): no se le puede dar otro valor", target->line); return; } }
    // Si la variable es de CADENA, hay que liberar la referencia
    // anterior (si tenia alguna) antes de perder su puntero para
    // siempre -- pero el ORDEN importa: el valor nuevo se evalua
    // SIEMPRE primero (puede referenciar la propia variable, como en
    // "x$ = x$ + \"mas\"", y en ese momento la variable todavia tiene
    // que valer lo de antes), y solo despues de tener el valor nuevo
    // a salvo se libera lo viejo y se sobreescribe. Un valor de
    // 0/NULL (variable nunca asignada, o global recien llegada del
    // cargador a cero) se libera sin problema:
    // nb_string_release(NULL) ya es seguro, no hace falta comprobarlo
    // aparte. Enteros y flotantes no tienen nada que liberar, y
    // siguen el orden de siempre (mas simple, sin este cuidado).
    bool is_string = nb_infer_type(target->text) == NB_TYPE_STRING;
    bool is_local; int32_t offset;
    nb_resolve_var(ctx, target->text, &is_local, &offset);

    nb_var_type_t tipo_destino = nb_infer_type(target->text);
    if (is_string && nb_expr_type(value) != NB_TYPE_STRING) { nb_emit_expr_para(ctx, value, tipo_destino); return; }
    if (!is_string) {
        if (is_local) {
            nb_emit_expr_para(ctx, value, tipo_destino); // -> x0, ya con el tipo de la variable
            nb_emit_store_var_from_x0(ctx, target->text);
        } else {
            // CORREGIDO: el comentario que habia aqui ("la evaluacion
            // entera/flotante nunca toca x9") era FALSO en cuanto el
            // valor lee OTRA variable global (que tambien necesita
            // x9 para su propia direccion), lee un elemento de un
            // array, o llama a cualquier funcion (de usuario o del
            // bloque de runtime) -- x9-x15 son de uso libre para
            // cualquier llamada segun el convenio de ARM64, asi que
            // NADA que haga una llamada puede darse por sobrevivido.
            // "x = y" con las dos globales llegaba a escribir el
            // valor en 'y' en vez de en 'x' -- se detecto generando
            // el codigo de arrays, pero el fallo era anterior y mas
            // amplio. Mismo orden seguro que ya usaba la rama de
            // cadenas: valor primero, direccion despues.
            nb_emit_expr_para(ctx, value, tipo_destino); // -> x0, ya con el tipo de la variable
            nb_push_x0(cb);
            nb_emit_global_addr(ctx->globals, cb, NB_ADDR_REG, (uint32_t)offset);
            nb_pop_x1(cb);
            nb_codebuf_emit(cb, nb_enc_str_imm(1, NB_ADDR_REG, 0));
        }
        return;
    }

    nb_label_t *rel = nb_rt_obligatoria(ctx, "nb_string_release", 0);
    if (is_local) {
        nb_emit_expr(ctx, value);                  // valor nuevo -> x0
        nb_emit_retener_si_copia(ctx, value);      // si es copia de otra, cuenta la referencia
        nb_push_x0(cb);                            // a salvo en la pila
        nb_codebuf_emit(cb, nb_enc_ldur(0, 29, offset)); // valor anterior -> x0
        if (rel) nb_emit_bl(cb, rel);              // liberarlo
        nb_pop_x1(cb);                             // valor nuevo -> x1 (recuperado)
        nb_codebuf_emit(cb, nb_enc_stur(1, 29, offset)); // guardarlo de verdad
    } else {
        // La evaluacion del valor nuevo puede necesitar x9 para OTRA
        // variable global (ej. "x$ = y$", con 'y' tambien global) --
        // por eso aqui NO se calcula la direccion de 'x' hasta
        // DESPUES de evaluar el valor, al reves que en el caso
        // entero/flotante de arriba.
        nb_emit_expr(ctx, value);                  // valor nuevo -> x0
        nb_emit_retener_si_copia(ctx, value);      // si es copia de otra, cuenta la referencia
        nb_push_x0(cb);                            // a salvo en la pila
        nb_emit_global_addr(ctx->globals, cb, NB_ADDR_REG, (uint32_t)offset); // AHORA se calcula
        nb_codebuf_emit(cb, nb_enc_str_pre(NB_ADDR_REG, NB_REG_SP, -16)); // la direccion, tambien a salvo
        nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_ADDR_REG, 0));  // valor anterior -> x0
        if (rel) nb_emit_bl(cb, rel);
        nb_codebuf_emit(cb, nb_enc_ldr_post(NB_ADDR_REG, NB_REG_SP, 16)); // direccion recuperada
        nb_pop_x1(cb);                             // valor nuevo -> x1 (recuperado, en orden inverso al empuje)
        nb_codebuf_emit(cb, nb_enc_str_imm(1, NB_ADDR_REG, 0));  // guardarlo de verdad
    }
}

// Asignacion a un elemento de array: a(i) = valor, o a(i,j) = valor.
// Mismo cuidado de orden que en una variable escalar de cadena si el
// array es de cadenas -- el valor nuevo se evalua primero (puede
// referenciar el propio elemento), y solo despues se calcula la
// direccion, se libera lo que hubiera alli, y se guarda lo nuevo.
void nb_emit_assign_array(nb_codegen_ctx_t *ctx, nb_node_t *target, nb_node_t *value) {
    nb_codebuf_t *cb = ctx->cb;
    bool is_string = nb_infer_type(target->text) == NB_TYPE_STRING;
    if (is_string && nb_expr_type(value) != NB_TYPE_STRING) { nb_emit_expr_para(ctx, value, NB_TYPE_STRING); return; }

    if (!is_string) {
        nb_emit_expr_para(ctx, value, nb_infer_type(target->text)); // valor -> x0, con el tipo del array
        nb_push_x0(cb);
        nb_emit_array_addr(ctx, target);  // direccion -> x0 (balanceado por dentro, no toca la pila de arriba)
        nb_pop_x1(cb);                     // valor -> x1
        nb_codebuf_emit(cb, nb_enc_str_imm(1, 0, 0)); // [direccion] = valor
        return;
    }

    nb_emit_expr(ctx, value);            // valor nuevo -> x0
        nb_emit_retener_si_copia(ctx, value);   // copia: cuenta la referencia
    nb_push_x0(cb);                       // pila: [valor]
    nb_emit_array_addr(ctx, target);      // direccion -> x0
    nb_push_x0(cb);                        // pila: [direccion, valor] (direccion en la cima)
    nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0)); // x0 = direccion (mirar sin sacar)
    nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 0, 0));          // x0 = valor anterior en esa direccion
    nb_label_t *rel = nb_rt_obligatoria(ctx, "nb_string_release", target ? target->line : 0);
    if (rel) nb_emit_bl(cb, rel);
    nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16)); // x1 = direccion (ahora si, sacada de la pila)
    nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16)); // x2 = valor nuevo (sacado)
    nb_codebuf_emit(cb, nb_enc_str_imm(2, 1, 0));            // [direccion] = valor nuevo
}

// ---------------------------------------------------------------
// Arrays (Dim). Cada array es un bloque reservado con nb_alloc EN
// TIEMPO DE EJECUCION (no un hueco de tamaño fijo como una variable
// escalar) -- la variable del array solo guarda un PUNTERO a ese
// bloque, igual que una cadena. El bloque lleva una cabecera con el
// numero de dimensiones y el tamaño de cada una, seguida de los
// datos (8 bytes por elemento). Alcance de esta pieza: 1 y 2
// dimensiones, completas y probadas -- 3 o mas se deja fuera a
// proposito (el mecanismo se generaliza de forma obvia si algun dia
// hace falta, pero sin un caso real delante no tiene sentido
// ampliarlo).
//
// Cabecera 1D: [dim_count=1 @0] [dim0_size @8] -- datos desde @16
// Cabecera 2D: [dim_count=2 @0] [dim0_size @8] [dim1_size @16] -- datos desde @24
// (dimN_size es SIEMPRE n+1 -- Dim a(n) admite indices 0..n, n+1
// elementos, igual que BlitzPlus real).
// Pone a CERO los elementos de un array recien reservado.
//   x0 = puntero al bloque, x1 = cuantos elementos, 'cabecera' = 16 o 24.
// x0 y x1 se conservan.
//
// POR QUE HACE FALTA. nb_alloc NO devuelve memoria limpia: reutiliza
// bloques liberados tal cual estaban. Sin esta limpieza, un "Dim t$(10)"
// hecho despues de cualquier cosa que haya usado memoria --una llamada a
// una funcion propia con un argumento de texto basta-- se lleva dentro
// restos de lo anterior. Y la PRIMERA asignacion a un elemento libera el
// valor que habia antes: libera basura, y el programa se cae con un
// acceso a memoria que no apunta a ninguna linea del programa.
//
// En un array de numeros no se cae, que es peor: "Dim marcador(10)"
// empieza con valores cualesquiera en vez de ceros, y un contador que
// arranca en un numero enorme no se parece a nada.
static void nb_emit_limpiar_array(nb_codegen_ctx_t *ctx, uint32_t cabecera) {
    nb_codebuf_t *cb = ctx->cb;
    nb_label_t *bucle = nb_label_new(), *fin = nb_label_new();
    nb_codebuf_emit(cb, nb_enc_add_imm(3, 0, cabecera));   // x3 = primer elemento
    nb_codebuf_emit(cb, nb_enc_orr_reg(4, NB_REG_ZR, 1));  // x4 = cuantos quedan
    nb_label_define(cb, bucle);
    nb_emit_cbz(cb, 4, fin);
    nb_codebuf_emit(cb, nb_enc_str_post(NB_REG_ZR, 3, 8)); // *x3 = 0; x3 += 8
    nb_codebuf_emit(cb, nb_enc_sub_imm(4, 4, 1));
    nb_emit_b(cb, bucle);
    nb_label_define(cb, fin);
    nb_label_free(bucle); nb_label_free(fin);
}

void nb_emit_dim(nb_codegen_ctx_t *ctx, nb_node_t *stmt) {
    nb_codebuf_t *cb = ctx->cb;
    nb_label_t *alloc_fn = nb_rt_obligatoria(ctx, "nb_alloc", stmt->line);
    if (!alloc_fn) return; // sin bloque de runtime -- no deberia pasar (Dim ya activa su incrustacion)

    if (stmt->list_count == 1) {
        nb_emit_expr_int(ctx, stmt->list[0]);            // x0 = limite superior (n)
        nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 1));     // x0 = dim0_size = n+1
        nb_push_x0(cb);
        nb_codebuf_emit(cb, nb_enc_lsl_imm(1, 0, 3));     // x1 = dim0_size*8
        nb_codebuf_emit(cb, nb_enc_add_imm(0, 1, 16));    // x0 = total de bytes (+cabecera)
        nb_emit_bl(cb, alloc_fn);                          // x0 = puntero al bloque
        nb_pop_x1(cb);                                     // x1 = dim0_size (recuperado)
        nb_codebuf_emit(cb, nb_enc_str_imm(1, 0, 1));     // [x0+8] = dim0_size
        nb_codebuf_emit(cb, nb_enc_movz(2, 1, 0));
        nb_codebuf_emit(cb, nb_enc_str_imm(2, 0, 0));     // [x0+0] = dim_count(1)
        nb_emit_limpiar_array(ctx, 16);                    // x1 ya es dim0_size
        nb_emit_store_var_from_x0(ctx, stmt->text);
        if (ctx->arrays) nb_array_set_add(ctx->arrays, stmt->text);
    } else if (stmt->list_count == 2) {
        nb_emit_expr_int(ctx, stmt->list[0]);
        nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 1));     // dim0_size
        nb_push_x0(cb);
        nb_emit_expr_int(ctx, stmt->list[1]);
        nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 1));     // dim1_size
        nb_push_x0(cb);
        // pila (tope primero): [dim1_size, dim0_size] -- se miran SIN
        // sacarlas (offset 0 y 1 desde sp) porque hacen falta
        // DESPUES de la llamada a nb_alloc tambien, y esa llamada
        // puede pisar cualquier registro.
        nb_codebuf_emit(cb, nb_enc_ldr_imm(1, NB_REG_SP, 0)); // x1 = dim1_size
        // x2 = dim0_size: cada valor guardado en la pila ocupa 16
        // BYTES (str x0, [sp, #-16]!), no 8. Leyendo a 8 salia basura: el
        // tamaño del array se calculaba mal y se pedia una barbaridad de
        // memoria (o, si la basura era cero, se pedia de menos y el array se
        // salia de su bloque en silencio).
        nb_codebuf_emit(cb, nb_enc_ldr_imm(2, NB_REG_SP, 2)); // x2 = dim0_size ([sp+16])
        nb_codebuf_emit(cb, nb_enc_mul(0, 2, 1));              // x0 = dim0_size*dim1_size
        nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 0, 3));          // *8
        nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 24));         // + cabecera
        nb_emit_bl(cb, alloc_fn);                               // x0 = puntero
        nb_codebuf_emit(cb, nb_enc_ldr_post(4, NB_REG_SP, 16)); // x4 = dim1_size (ahora si, sacada)
        nb_codebuf_emit(cb, nb_enc_ldr_post(5, NB_REG_SP, 16)); // x5 = dim0_size
        nb_codebuf_emit(cb, nb_enc_str_imm(4, 0, 2));  // [x0+16] = dim1_size
        nb_codebuf_emit(cb, nb_enc_str_imm(5, 0, 1));  // [x0+8] = dim0_size
        nb_codebuf_emit(cb, nb_enc_movz(6, 2, 0));
        nb_codebuf_emit(cb, nb_enc_str_imm(6, 0, 0));  // [x0+0] = dim_count(2)
        nb_codebuf_emit(cb, nb_enc_mul(1, 5, 4));      // x1 = dim0_size*dim1_size
        nb_emit_limpiar_array(ctx, 24);
        nb_emit_store_var_from_x0(ctx, stmt->text);
        if (ctx->arrays) nb_array_set_add(ctx->arrays, stmt->text);
    }
    // 3+ dimensiones: fuera de alcance a proposito, no se genera nada (documentado)
}

// ---------------------------------------------------------------
// Control de flujo: If/While/Repeat. Las tres comparten la misma idea
// de fondo -- la condicion es una expresion entera CUALQUIERA (no
// solo una comparacion suelta: "x > 0 And y < 10" es tan valida como
// "x > 0"), se evalua a x0 con nb_emit_expr_int, y se salta con
// cbz/cbnz segun si el resultado es cero (falso) o no (verdadero).
// For/Next queda para la siguiente pieza -- necesita ademas decidir
// en tiempo de EJECUCION si el paso es positivo o negativo, que es un
// problema propio, distinto de este.

static void nb_emit_block(nb_codegen_ctx_t *ctx, nb_node_t *block);

// Emite una llamada al sistema con sus argumentos: evalua cada uno
// como ENTERO, los deja en x0..x4 (el convenio de syscalls de Nemo OS,
// ver nb_syscalls.h), pone el numero de syscall en x8 y hace 'svc #0'.
// Los argumentos que el programa no escribio se rellenan con el valor
// de 'defaults' correspondiente -- asi "Rect x,y,w,h" (sin el quinto)
// puede usar el color activo por defecto sin caso especial.
//
// Mismo mecanismo de "empujar todos y sacarlos en orden inverso" que
// las llamadas a funcion: hace falta porque evaluar el argumento 2
// puede llamar a otra funcion, que pisaria x0/x1 si ya estuvieran
// puestos.
static void nb_emit_syscall_args(nb_codegen_ctx_t *ctx, nb_node_t *stmt,
                                 uint32_t sysnum, int32_t nargs,
                                 const int64_t *defaults) {
    nb_codebuf_t *cb = ctx->cb;
    if (nargs > 5) nargs = 5; // x0..x4, como el syscall5 de nb_syscalls.h
    for (int32_t i = 0; i < nargs; i++) {
        if (i < stmt->list_count) nb_emit_expr_int(ctx, stmt->list[i]);
        else nb_emit_load_imm64(cb, 0, (uint64_t)(defaults ? defaults[i] : 0));
        nb_push_x0(cb);
    }
    for (int32_t i = nargs - 1; i >= 0; i--) {
        nb_codebuf_emit(cb, nb_enc_ldr_post(i, NB_REG_SP, 16));
    }
    nb_emit_load_imm64(cb, 8, sysnum);
    nb_codebuf_emit(cb, nb_enc_svc(0));
}

static void nb_emit_stmt_bruto(nb_codegen_ctx_t *ctx, nb_node_t *stmt);

// ¿Esta sentencia CREA alguna cadena? (un literal, una concatenacion, Mid$,
// Str$...). Si no, no hace falta envolverla para liberar temporales, y el
// programa no paga ni una instruccion de mas.
static void nb_emit_cond_temporales(nb_codegen_ctx_t *ctx, nb_node_t *cond);
static bool nb_crea_cadenas(nb_codegen_ctx_t *ctx, nb_node_t *n) {
    if (!n) return false;
    if (n->kind == N_STR) return true;
    if (n->kind != N_VAR && n->kind != N_INDEX && n->kind != N_FIELD) {
        if ((n->kind == N_CALL || n->kind == N_BINOP) && nb_expr_type(n) == NB_TYPE_STRING &&
            !nb_es_cadena_guardada(ctx, n)) return true;
    }
    if (nb_crea_cadenas(ctx, n->a) || nb_crea_cadenas(ctx, n->b) ||
        nb_crea_cadenas(ctx, n->c) || nb_crea_cadenas(ctx, n->d)) return true;
    for (int32_t i = 0; i < n->list_count; i++) if (nb_crea_cadenas(ctx, n->list[i])) return true;
    return false;
}

// Cada sentencia deja la lista de temporales como la encontro: marca al
// empezar, y al acabar libera lo que haya creado ella.
static void nb_emit_stmt(nb_codegen_ctx_t *ctx, nb_node_t *stmt) {
    // Solo en programas con cadenas (los que llevan el bloque de runtime); en
    // los demas no hay nada que liberar y no se paga nada.
    //
    // Y solo en sentencias SIMPLES: la marca se guarda en la pila, asi que una
    // sentencia de la que se pueda saltar fuera (Return, Gosub, Goto, Exit) o
    // que contenga otras (If, bucles) dejaria la marca ahi y descuadraria la
    // pila. Las simples son justamente las que crean cadenas temporales.
    bool simple = stmt && (stmt->kind == N_ASSIGN || stmt->kind == N_PRINT || stmt->kind == N_EXPRSTMT ||
                           stmt->kind == N_CALL || stmt->kind == N_TEXT || stmt->kind == N_VARDECL ||
                           stmt->kind == N_DIM || stmt->kind == N_READ || stmt->kind == N_CREATEWINDOW ||
                           stmt->kind == N_DELETE || stmt->kind == N_INSERT);
    if (!g_t_marca || !stmt || !ctx->runtime || !simple || !nb_crea_cadenas(ctx, stmt)) {
        nb_emit_stmt_bruto(ctx, stmt); return;
    }
    nb_codebuf_t *cb = ctx->cb;
    g_usa_temporales = true;
    nb_emit_bl(cb, g_t_marca);
    nb_push_x0(cb);
    nb_emit_stmt_bruto(ctx, stmt);
    nb_pop_x0_marca(cb);
    nb_emit_bl(cb, g_t_liberar);
}

// La condicion de un If, un While o un Until tambien crea cadenas
// ("If Mid$(t$, i, 1) = \"X\""), y esas sentencias NO se pueden envolver enteras
// (se salta fuera de ellas). Se liberan aqui, en cuanto la condicion esta
// evaluada: el resultado ya es un numero.
static void nb_emit_cond_temporales(nb_codegen_ctx_t *ctx, nb_node_t *cond) {
    nb_codebuf_t *cb = ctx->cb;
    if (!g_t_marca || !ctx->runtime || !nb_crea_cadenas(ctx, cond)) { nb_emit_expr(ctx, cond); return; }
    g_usa_temporales = true;
    nb_emit_bl(cb, g_t_marca);
    nb_push_x0(cb);                       // la marca
    nb_emit_expr(ctx, cond);              // la condicion -> x0
    nb_push_x0(cb);                       // el resultado, a salvo
    nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 2));   // la marca: [sp+16], y el indice va en unidades de 8
    nb_emit_bl(cb, g_t_liberar);
    nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16)); // el resultado, de vuelta
    nb_codebuf_emit(cb, nb_enc_add_imm(NB_REG_SP, NB_REG_SP, 16)); // y fuera la marca
}

static void nb_emit_stmt_bruto(nb_codegen_ctx_t *ctx, nb_node_t *stmt) {
    nb_codebuf_t *cb = ctx->cb;
    nb_lineas_anotar(cb->count, stmt->line);      // para los errores en tiempo de ejecucion
    switch (stmt->kind) {
        case N_ASSIGN:
            if (stmt->a->kind == N_VAR) nb_emit_assign_int(ctx, stmt->a, stmt->b);
            else if (stmt->a->kind == N_INDEX) nb_emit_assign_array(ctx, stmt->a, stmt->b);
            else if (stmt->a->kind == N_FIELD) {
                // Sin liberar una referencia de cadena anterior del
                // campo (a diferencia de una variable normal) --
                // limitacion deliberada de esta pieza, documentada:
                // los campos de Type se tratan como almacenamiento
                // crudo, sin conteo de referencias automatico al
                // reasignarlos.
                nb_emit_expr_para(ctx, stmt->b, nb_infer_type(stmt->a->text)); // x0 = valor nuevo, con el tipo del campo
                nb_push_x0(cb);
                nb_emit_field_addr(ctx, stmt->a, 9); // direccion del campo, evaluada DESPUES -- el valor ya esta a salvo en la pila
                nb_pop_x1(cb);
                nb_codebuf_emit(cb, nb_enc_str_imm(1, 9, 0));
            }
            return;
        case N_DIM:
            nb_emit_dim(ctx, stmt);
            return;
        case N_BLOCK:
            // "Dim a(3), b(4)" el parser lo envuelve en un bloque de
            // varios N_DIM -- se recorre como cualquier otro bloque.
            nb_emit_block(ctx, stmt);
            return;
        case N_DATALABEL: {
            nb_label_t *l = ctx->labels ? nb_label_table_find(ctx->labels, stmt->text) : NULL;
            if (l) nb_label_define(cb, l);
            return;
        }
        case N_DATA:
            // Sin nada que generar aqui -- sus valores ya se
            // incorporaron a la region de datos compartida durante el
            // barrido previo (nb_scan_data), antes de generar ni una
            // instruccion del programa.
            return;
        case N_CONSOLEAPP:
        case N_DESKTOPAPP:
            // No generan codigo: son una declaracion, no una orden.
            // El driver las consulta con nb_tree_is_console_app() para
            // rellenar la cabecera del .pro.
            return;

        case N_CREATEWINDOW: {
            // CreateWindow titulo$, x, y, ancho, alto -- syscall 40.
            //
            // A diferencia de Graphics, esta pone la ventana en "modo
            // evento": la X de cerrar deja de destruirla por su cuenta
            // y dispara un evento ($803) que el programa ve con
            // WaitEvent(). Es decir, el programa decide cuando cerrar.
            if (stmt->list_count < 5) return;
            nb_emit_expr_string(ctx, stmt->list[0]);
            nb_label_t *cstr_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", stmt->line) : NULL;
            if (cstr_fn) nb_emit_bl(cb, cstr_fn);
            nb_push_x0(cb);
            for (int32_t i = 1; i < 5; i++) { nb_emit_expr_int(ctx, stmt->list[i]); nb_push_x0(cb); }
            for (int32_t i = 4; i >= 1; i--) nb_codebuf_emit(cb, nb_enc_ldr_post(i, NB_REG_SP, 16));
            nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16)); // x0 = titulo
            nb_emit_load_imm64(cb, 8, 40);
            nb_codebuf_emit(cb, nb_enc_svc(0));
            return;
        }

        // ---- Graficos ----
        //
        // Todos usan el mismo mecanismo: evaluar argumentos, syscall.
        // El COLOR ACTIVO (que fija "Color r,g,b" y usan Plot/Rect/
        // Line/Oval/Text cuando no se les da uno) vive en una variable
        // global oculta, igual que el cursor de Data -- asi se
        // comporta como el Color de BlitzPlus, que es un estado del
        // programa, no un argumento que haya que repetir siempre.
        case N_GRAPHICS:
            // Graphics ancho, alto -- syscall 46
            nb_emit_syscall_args(ctx, stmt, 46, 2, NULL);
            return;
        case N_COLOR: {
            // Color r, g, b -> un solo entero 0xRRGGBB, guardado en la
            // variable oculta del color activo.
            if (stmt->list_count >= 3) {
                nb_emit_expr_int(ctx, stmt->list[0]); // r
                nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 0, 16));
                nb_push_x0(cb);
                nb_emit_expr_int(ctx, stmt->list[1]); // g
                nb_codebuf_emit(cb, nb_enc_lsl_imm(0, 0, 8));
                nb_pop_x1(cb);
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, 1, 0));
                nb_push_x0(cb);
                nb_emit_expr_int(ctx, stmt->list[2]); // b
                nb_pop_x1(cb);
                nb_codebuf_emit(cb, nb_enc_orr_reg(0, 1, 0));
            } else if (stmt->list_count == 1) {
                nb_emit_expr_int(ctx, stmt->list[0]); // Color 0xRRGGBB directo
            } else {
                return;
            }
            if (ctx->color_var) nb_emit_store_var_from_x0(ctx, ctx->color_var);
            return;
        }
        case N_CLS: {
            // Cls -- rectangulo negro del tamaño de la ventana entera.
            // El tamaño se pide en tiempo de ejecucion (syscall 33,
            // que devuelve ancho<<32|alto), no se supone ninguno.
            nb_emit_load_imm64(cb, 8, 33); // SYS_GET_WINDOW_SIZE
            nb_codebuf_emit(cb, nb_enc_svc(0));
            nb_codebuf_emit(cb, nb_enc_lsr_imm(2, 0, 32));           // x2 = ancho
            nb_codebuf_emit(cb, nb_enc_and_imm(3, 0, 0xFFFFFFFFu));   // x3 = alto (los 32 bits bajos)
            nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));                // x0 = 0
            nb_codebuf_emit(cb, nb_enc_movz(1, 0, 0));                // x1 = 0
            if (ctx->cls_color_var) {                                 // el color de ClsColor (negro si no se puso)
                nb_emit_load_var_x0(ctx, ctx->cls_color_var);
                nb_codebuf_emit(cb, nb_enc_orr_reg(4, NB_REG_ZR, 0));
                nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
            } else {
                nb_codebuf_emit(cb, nb_enc_movz(4, 0, 0));            // x4 = negro
            }
            nb_emit_load_imm64(cb, 8, 30);                            // SYS_DRAW_RECT
            nb_codebuf_emit(cb, nb_enc_svc(0));
            return;
        }
        case N_PLOT: {
            // Plot x, y -- un rectangulo de 1x1 con el color activo.
            if (stmt->list_count < 2) return;
            nb_emit_expr_int(ctx, stmt->list[0]); nb_push_x0(cb);
            nb_emit_expr_int(ctx, stmt->list[1]); nb_push_x0(cb);
            if (ctx->color_var) nb_emit_load_var_x0(ctx, ctx->color_var); else nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
            nb_codebuf_emit(cb, nb_enc_orr_reg(4, NB_REG_ZR, 0));     // x4 = color
            nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));   // x1 = y
            nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));   // x0 = x
            nb_codebuf_emit(cb, nb_enc_movz(2, 1, 0));                // ancho 1
            nb_codebuf_emit(cb, nb_enc_movz(3, 1, 0));                // alto 1
            nb_emit_load_imm64(cb, 8, 30);
            nb_codebuf_emit(cb, nb_enc_svc(0));
            return;
        }
        case N_RECT: {
            // Rect x, y, ancho, alto -- con el color activo.
            if (stmt->list_count < 4) return;
            for (int32_t i = 0; i < 4; i++) { nb_emit_expr_int(ctx, stmt->list[i]); nb_push_x0(cb); }
            if (ctx->color_var) nb_emit_load_var_x0(ctx, ctx->color_var); else nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
            nb_codebuf_emit(cb, nb_enc_orr_reg(4, NB_REG_ZR, 0));
            for (int32_t i = 3; i >= 0; i--) nb_codebuf_emit(cb, nb_enc_ldr_post(i, NB_REG_SP, 16));
            nb_emit_load_imm64(cb, 8, 30);
            nb_codebuf_emit(cb, nb_enc_svc(0));
            return;
        }
        case N_OVAL: {
            // Oval x, y, ancho, alto -- syscall 47, con el color
            // activo. OJO: el comentario de syscall.h dice que el
            // quinto argumento es "solido", pero la implementacion
            // real (src/syscall.c, case SYS_DRAW_OVAL) lo usa como
            // COLOR -- siempre dibuja relleno. Pasarle 1 ahi pintaba
            // un ovalo de color 0x000001, negro sobre negro:
            // invisible. Manda el codigo, no el comentario.
            if (stmt->list_count < 4) return;
            for (int32_t i = 0; i < 4; i++) { nb_emit_expr_int(ctx, stmt->list[i]); nb_push_x0(cb); }
            if (ctx->color_var) nb_emit_load_var_x0(ctx, ctx->color_var); else nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
            nb_codebuf_emit(cb, nb_enc_orr_reg(4, NB_REG_ZR, 0)); // x4 = color
            for (int32_t i = 3; i >= 0; i--) nb_codebuf_emit(cb, nb_enc_ldr_post(i, NB_REG_SP, 16));
            nb_emit_load_imm64(cb, 8, 47);
            nb_codebuf_emit(cb, nb_enc_svc(0));
            return;
        }
        case N_TEXT: {
            // Text x, y, cadena$ -- syscall 31, con el color activo.
            if (stmt->list_count < 3) return;
            nb_emit_expr_int(ctx, stmt->list[0]); nb_push_x0(cb);
            nb_emit_expr_int(ctx, stmt->list[1]); nb_push_x0(cb);
            // El tercer argumento es una CADENA: hay que pasarle al
            // sistema el puntero al texto en crudo, no el nb_string_t.
            nb_emit_expr_string(ctx, stmt->list[2]);
            nb_label_t *cstr_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", stmt->line) : NULL;
            if (cstr_fn) nb_emit_bl(cb, cstr_fn);
            nb_push_x0(cb);
            if (ctx->color_var) nb_emit_load_var_x0(ctx, ctx->color_var); else nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
            nb_codebuf_emit(cb, nb_enc_orr_reg(3, NB_REG_ZR, 0));     // x3 = color
            nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16));   // x2 = puntero al texto
            nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16));   // x1 = y
            nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));   // x0 = x
            nb_emit_load_imm64(cb, 8, 31);
            nb_codebuf_emit(cb, nb_enc_svc(0));
            return;
        }
        case N_DELAY:
            // Delay milisegundos -- SYS_SLEEP (1). El kernel cuenta en
            // LATIDOS de 10 ms. Antes se le pasaban los milisegundos tal
            // cual, asi que "Delay 1000" esperaba 1000 latidos: DIEZ
            // segundos, no uno. Ahora se convierte, redondeando hacia
            // arriba (Delay 1 espera un latido, no cero), y un valor
            // negativo cuenta como 0 -- el kernel lee el numero sin signo,
            // y "Delay -5" habria esperado practicamente para siempre.
            // La misma conversion que esperar() en nemo_sistema.lua.
            if (stmt->list_count < 1) { nb_codegen_error("Delay necesita los milisegundos a esperar", stmt->line); return; }
            nb_emit_expr_int(ctx, stmt->list[0]);                             // x0 = milisegundos
            nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 0, 0));            // cmp x0, #0
            nb_codebuf_emit(cb, nb_enc_csel(0, 0, NB_REG_ZR, NB_COND_GT));    // x0 = (x0 > 0) ? x0 : 0
            nb_codebuf_emit(cb, nb_enc_add_imm(0, 0, 9));                     // x0 += 9 (redondear hacia arriba)
            nb_codebuf_emit(cb, nb_enc_movz(1, 10, 0));
            nb_codebuf_emit(cb, nb_enc_udiv(0, 0, 1));                        // x0 = latidos
            nb_emit_load_imm64(cb, 8, 1);                                     // SYS_SLEEP
            nb_codebuf_emit(cb, nb_enc_svc(0));
            return;
        case N_LINE: {
            // Line x0, y0, x1, y1 -- syscall 257 (SYS_DRAW_LINE), con
            // el color activo. Antes esto generaba el bucle de
            // Bresenham ENTERO aqui mismo (unas 50 instrucciones y un
            // marco de pila propio), porque el kernel no tenia rutina
            // de lineas: una llamada al sistema POR PIXEL. Ahora el
            // kernel la trae (misma tecnica, pero en C y de una sola
            // llamada), y ademas queda disponible para Lua y para
            // cualquier otro programa, no solo para este BASIC.
            if (stmt->list_count < 4) return;
            for (int32_t i = 0; i < 4; i++) { nb_emit_expr_int(ctx, stmt->list[i]); nb_push_x0(cb); }
            if (ctx->color_var) nb_emit_load_var_x0(ctx, ctx->color_var);
            else nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
            nb_codebuf_emit(cb, nb_enc_orr_reg(4, NB_REG_ZR, 0)); // x4 = color
            for (int32_t i = 3; i >= 0; i--) nb_codebuf_emit(cb, nb_enc_ldr_post(i, NB_REG_SP, 16));
            nb_emit_load_imm64(cb, 8, 257);
            nb_codebuf_emit(cb, nb_enc_svc(0));
            return;
        }
        case N_PRINT: {
            // numeros, tal cual si ya es cadena) y se envia con la
            // syscall SYS_WRITE_STRING (11) -- convenio DISTINTO al
            // de las llamadas al bloque de runtime: x8=numero de
            // syscall, argumento en x0, 'svc #0' (ver nb_syscalls.h).
            // Al final de la lista, un salto de linea.
            for (int32_t i = 0; i < stmt->list_count; i++) {
                nb_node_t *arg = stmt->list[i];
                // Atajo para un LITERAL de cadena: no hace falta
                // reservar nada ni pasar por el bloque de runtime --
                // se guarda el texto ya terminado en nulo en la
                // region de datos y se pasa su direccion directa a la
                // syscall, EXACTAMENTE igual que syscall_test.s (que
                // esta probado y funciona en Nemo OS). Menos piezas de
                // por medio, menos sitios donde algo pueda fallar.
                if (arg->kind == N_STR) {
                    uint32_t len = nb_string_len(arg->text);
                    const char *src = nb_string_cstr(arg->text);
                    uint8_t *tmp = (uint8_t *)nb_alloc(len + 1);
                    if (tmp) {
                        for (uint32_t k = 0; k < len; k++) tmp[k] = (uint8_t)src[k];
                        tmp[len] = 0;
                        uint32_t off = nb_symtab_add_literal_bytes(ctx->globals, tmp, len + 1);
                        nb_free(tmp);
                        nb_emit_global_addr(ctx->globals, cb, 0, off);
                        nb_codebuf_emit(cb, nb_enc_movz(8, 11, 0));
                        nb_codebuf_emit(cb, nb_enc_svc(0));
                    }
                    continue;
                }
                nb_var_type_t t = nb_expr_type(arg);
                bool owns_temp = (t != NB_TYPE_STRING); // solo si a la fuerza CREAMOS una cadena nueva para imprimir el valor
                if (t == NB_TYPE_STRING) {
                    nb_emit_expr_string(ctx, arg);
                } else if (t == NB_TYPE_FLOAT) {
                    nb_emit_expr_float(ctx, arg);
                    nb_codebuf_emit(cb, nb_enc_fmov_fpr_to_gpr(0, 0));
                    nb_label_t *fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_float_to_str", stmt->line) : NULL;
                    if (fn) nb_emit_bl(cb, fn);
                } else {
                    nb_emit_expr_int(ctx, arg);
                    nb_label_t *fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_int_to_str", stmt->line) : NULL;
                    if (fn) nb_emit_bl(cb, fn);
                }
                // x0 = nb_string_t* (de la variable, o recien creado) -- guardarlo, hace falta despues del syscall para poder liberarlo si era temporal
                nb_push_x0(cb);
                nb_label_t *cstr_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_cstr", stmt->line) : NULL;
                if (cstr_fn) nb_emit_bl(cb, cstr_fn); // x0 = char* a partir del nb_string_t* que ya estaba en x0
                nb_codebuf_emit(cb, nb_enc_movz(8, 11, 0)); // x8 = SYS_WRITE_STRING
                nb_codebuf_emit(cb, nb_enc_svc(0));
                nb_pop_x1(cb); // recuperar el nb_string_t* original
                if (owns_temp) {
                    nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1));
                    nb_label_t *rel_fn = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_string_release", stmt->line) : NULL;
                    if (rel_fn) nb_emit_bl(cb, rel_fn);
                }
            }
            // Salto de linea final -- literal directo, no hace falta
            // pasar por nb_string_t (SYS_WRITE_STRING solo quiere un
            // puntero a texto terminado en nulo).
            {
                uint8_t nl[2] = {'\n', 0};
                uint32_t nl_off = nb_symtab_add_literal_bytes(ctx->globals, nl, 2);
                nb_emit_global_addr(ctx->globals, cb, 0, nl_off);
                nb_codebuf_emit(cb, nb_enc_movz(8, 11, 0));
                nb_codebuf_emit(cb, nb_enc_svc(0));
            }
            return;
        }
        case N_TYPEDEF:
            // Igual que N_DATA: ya se proceso entero en el barrido
            // previo (nb_scan_types) -- aqui no hay nada que generar.
            return;
        case N_DELETE: {
            nb_emit_expr_int(ctx, stmt->a); // x0 = instancia a borrar
            nb_push_x0(cb);
            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 0)); // x0 = instancia
            nb_codebuf_emit(cb, nb_enc_ldr_imm(2, 0, 0)); // x2 = type_id
            nb_codebuf_emit(cb, nb_enc_ldr_imm(3, 0, 1)); // x3 = prev
            nb_codebuf_emit(cb, nb_enc_ldr_imm(4, 0, 2)); // x4 = next

            nb_label_t *l_no_prev = nb_label_new();
            nb_label_t *l_after_prev = nb_label_new();
            nb_emit_cbz(cb, 3, l_no_prev);
            nb_codebuf_emit(cb, nb_enc_str_imm(4, 3, 2)); // prev->next = next
            nb_emit_b(cb, l_after_prev);
            nb_label_define(cb, l_no_prev);
            // No habia anterior: era la cabeza de la lista de su tipo
            // -- el desplazamiento del tipo (type_id) solo se conoce
            // en TIEMPO DE EJECUCION aqui (a diferencia de New/First/
            // Last, Delete solo recibe un puntero opaco), asi que la
            // posicion en la tabla de cabeza/cola se calcula con un
            // registro (type_id*16), no con una constante.
            if (ctx->types) nb_emit_global_addr(ctx->globals, cb, 9, ctx->types->headtail_base_offset);
            nb_codebuf_emit(cb, nb_enc_add_reg(9, 9, 2, 4)); // x9 = base + type_id*16
            nb_codebuf_emit(cb, nb_enc_str_imm(4, 9, 0)); // head del tipo = next
            nb_label_define(cb, l_after_prev);
            nb_label_free(l_no_prev);
            nb_label_free(l_after_prev);

            nb_label_t *l_no_next = nb_label_new();
            nb_label_t *l_after_next = nb_label_new();
            nb_emit_cbz(cb, 4, l_no_next);
            nb_codebuf_emit(cb, nb_enc_str_imm(3, 4, 1)); // next->prev = prev
            nb_emit_b(cb, l_after_next);
            nb_label_define(cb, l_no_next);
            if (ctx->types) nb_emit_global_addr(ctx->globals, cb, 9, ctx->types->headtail_base_offset);
            nb_codebuf_emit(cb, nb_enc_add_reg(9, 9, 2, 4));
            nb_codebuf_emit(cb, nb_enc_str_imm(3, 9, 1)); // tail del tipo = prev
            nb_label_define(cb, l_after_next);
            nb_label_free(l_no_next);
            nb_label_free(l_after_next);

            nb_pop_x1(cb);
            nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1)); // x0 = instancia (bl nb_free la espera ahi)
            nb_label_t *fn_free = ctx->runtime ? nb_rt_obligatoria(ctx, "nb_free", stmt->line) : NULL;
            if (fn_free) nb_emit_bl(cb, fn_free);
            return;
        }
        case N_FOREACH: {
            // "For loopvar = Each TypeName ... Next": equivale a
            // recorrer la lista enlazada del tipo empezando por
            // First(TypeName), avanzando por el propio campo 'next'
            // de cada instancia (offset 16) hasta llegar a 0.
            nb_type_entry_t *te = ctx->types ? nb_type_table_find(ctx->types, stmt->a->text) : NULL;
            if (!te) {
                nb_codegen_error("ForEach sobre un Type que no se ha declarado", stmt->line);
                return;
            }
            nb_emit_global_addr(ctx->globals, cb, 9, ctx->types->headtail_base_offset + te->type_id * 16);
            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 9, 0)); // x0 = First(TypeName)
            nb_emit_store_var_from_x0(ctx, stmt->text); // loopvar = ese valor

            // El SIGUIENTE se lee y se guarda en la pila ANTES del cuerpo.
            // Antes se leia despues, de la propia instancia: si el cuerpo
            // hacia "Delete e", se leia memoria ya liberada. Asi, borrar la
            // instancia actual dentro del bucle es seguro, como en
            // BlitzBasic. (Borrar OTRA -- en concreto la siguiente -- sigue
            // sin serlo.)
            //
            // Y Exit tiene ahora su propia salida (l_exit), que retira ese
            // valor guardado -- igual que el For normal retira su paso --,
            // para que salir a mitad deje la pila equilibrada. Antes el
            // For Each no fijaba exit_label, y un Exit dentro saltaba a la
            // salida del bucle de FUERA, o a ninguna parte.
            nb_label_t *l_check = nb_label_new();
            nb_label_t *l_exit = nb_label_new();
            nb_label_t *l_end = nb_label_new();
            nb_label_t *exit_anterior = ctx->exit_label;   // por si hay bucles anidados
            ctx->exit_label = l_exit;

            nb_label_define(cb, l_check);
            nb_emit_load_var_x0(ctx, stmt->text);
            nb_emit_cbz(cb, 0, l_end);
            nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 0, 2)); // x0 = loopvar->next (offset 16), ANTES del cuerpo
            nb_push_x0(cb);                                 // guardado en la pila
            nb_emit_block(ctx, stmt->b);
            nb_pop_x0(cb);                                  // el siguiente que se guardo
            nb_emit_store_var_from_x0(ctx, stmt->text);
            nb_emit_b(cb, l_check);

            nb_label_define(cb, l_exit);                    // salida por Exit: retirar el siguiente guardado
            nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16));
            nb_label_define(cb, l_end);                     // fin normal: no hay nada guardado
            ctx->exit_label = exit_anterior;
            nb_label_free(l_check);
            nb_label_free(l_exit);
            nb_label_free(l_end);
            return;
        }
        case N_READ: {
            nb_node_t *target = stmt->a;
            // Tambien hacia un elemento de array (N_INDEX). Antes
            // eso "quedaba fuera, se ignoraba silenciosamente": Read boton$(i)
            // no hacia nada, sin ningun aviso, y el array se quedaba vacio.
            // Ahora, cualquier otro destino es un error de compilacion.
            if (!ctx->data_cursor) return;
            if (target->kind != N_VAR && target->kind != N_INDEX) {
                nb_codegen_error("Read solo puede leer en una variable o en un elemento de array", stmt->line);
                return;
            }
            bool is_string = nb_infer_type(target->text) == NB_TYPE_STRING;

            nb_emit_load_var_x0(ctx, ctx->data_cursor); // x0 = cursor (desplazamiento en BYTES, no en "valores")
            nb_push_x0(cb);
            nb_emit_global_addr(ctx->globals, cb, 9, 0); // x9 = direccion absoluta del INICIO de la region de datos compartida
            nb_codebuf_emit(cb, nb_enc_orr_reg(10, NB_REG_ZR, 9)); // x10 = esa misma direccion (la necesita una cadena, abajo)
            nb_codebuf_emit(cb, nb_enc_ldr_imm(1, NB_REG_SP, 0)); // x1 = cursor (mirar sin sacar -- hace falta otra vez para avanzarlo)
            nb_codebuf_emit(cb, nb_enc_add_reg(9, 9, 1, 0)); // x9 = base + cursor (cursor YA es un desplazamiento en bytes)

            // 1) el valor, en x0
            if (is_string) {
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 9, 0));     // desplazamiento del contenido, RELATIVO a la region de datos
                nb_codebuf_emit(cb, nb_enc_add_reg(0, 0, 10, 0)); // + direccion real de la region = direccion del contenido
                nb_codebuf_emit(cb, nb_enc_ldr_imm(1, 9, 1)); // longitud ([x9+8], escalado 1)
                nb_label_t *fn = nb_rt_obligatoria(ctx, "nb_string_new", stmt->line);
                if (fn) nb_emit_bl(cb, fn);
            } else {
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 9, 0)); // valor (entero o flotante -- los mismos bits, sin distincion aqui)
            }
            // 2) guardarlo en su destino
            if (target->kind == N_VAR) {
                nb_emit_store_var_from_x0(ctx, target->text);
            } else if (!is_string) {
                nb_push_x0(cb);                                   // pila: [valor, cursor]
                nb_emit_array_addr(ctx, target);                  // direccion -> x0
                nb_pop_x1(cb);                                    // x1 = valor
                nb_codebuf_emit(cb, nb_enc_str_imm(1, 0, 0));     // [direccion] = valor
            } else {
                // como una asignacion a un elemento de cadena: liberar la anterior
                nb_push_x0(cb);                                   // pila: [valor, cursor]
                nb_emit_array_addr(ctx, target);                  // direccion -> x0
                nb_push_x0(cb);                                   // pila: [direccion, valor, cursor]
                nb_codebuf_emit(cb, nb_enc_ldr_imm(0, 0, 0));     // x0 = cadena anterior
                nb_label_t *rel = nb_rt_obligatoria(ctx, "nb_string_release", stmt->line);
                if (rel) nb_emit_bl(cb, rel);
                nb_codebuf_emit(cb, nb_enc_ldr_post(1, NB_REG_SP, 16)); // x1 = direccion
                nb_codebuf_emit(cb, nb_enc_ldr_post(2, NB_REG_SP, 16)); // x2 = valor nuevo
                nb_codebuf_emit(cb, nb_enc_str_imm(2, 1, 0));            // [direccion] = valor nuevo
            }
            // 3) avanzar el cursor
            nb_pop_x1(cb); // cursor recuperado
            nb_codebuf_emit(cb, nb_enc_add_imm(1, 1, is_string ? 16 : 8)); // una cadena ocupa 16 bytes (direccion+longitud); un numero, 8
            nb_codebuf_emit(cb, nb_enc_orr_reg(0, NB_REG_ZR, 1)); // x0 = x1 (para poder usar store_var_from_x0)
            nb_emit_store_var_from_x0(ctx, ctx->data_cursor);
            return;
        }
        case N_RESTORE: {
            if (!ctx->data_cursor) return;
            uint32_t target_offset;
            if (stmt->text) {
                if (!ctx->restore || !nb_restore_table_find(ctx->restore, stmt->text, &target_offset)) {
                    nb_codegen_error("Restore a una etiqueta que no existe", stmt->line);
                    return;
                }
            } else {
                target_offset = ctx->data_start_offset; // Restore sin argumento -- vuelve al principio de TODO
            }
            nb_emit_load_imm64(cb, 0, target_offset);
            nb_emit_store_var_from_x0(ctx, ctx->data_cursor);
            return;
        }
        case N_GOTO: {
            nb_label_t *l = ctx->labels ? nb_label_table_find(ctx->labels, stmt->text) : NULL;
            if (!l) {
                // Antes: el salto no se generaba y el programa seguia
                // de largo, como si el Goto no estuviera escrito.
                nb_codegen_error("Goto a una etiqueta que no existe", stmt->line);
                return;
            }
            nb_emit_b(cb, l);
            return;
        }
        case N_GOSUB: {
            // Alcance de esta pieza, documentado: Gosub/Return
            // funciona correctamente en el NIVEL SUPERIOR (el uso mas
            // comun) -- dentro de una funcion, "Return" ya tiene un
            // significado fijo (terminar la funcion), y distinguirlo
            // en tiempo de ejecucion de "volver de un Gosub" cuando
            // los dos se mezclan necesitaria una pila de Gosub con
            // comprobacion de "vacio" aparte. Gosub dentro de una
            // funcion no esta soportado por esta pieza.
            //
            // Mecanismo: NO se usa bl/ret (x30 no sobrevive si el
            // codigo de la etiqueta llama a cualquier funcion antes
            // del Return) -- se calcula la direccion de retorno con
            // nb_emit_code_addr y se guarda en NUESTRA PROPIA pila
            // (la misma que usa cualquier evaluacion de expresion),
            // y se salta con un 'b' normal, sin tocar x30 en absoluto.
            nb_label_t *target = ctx->labels ? nb_label_table_find(ctx->labels, stmt->text) : NULL;
            if (!target) {
                nb_codegen_error("Gosub a una etiqueta que no existe", stmt->line);
                return;
            }
            nb_label_t *resume = nb_label_new();
            nb_emit_code_addr(cb, 0, resume); // x0 = direccion a la que Return debe volver
            nb_push_x0(cb);
            nb_emit_b(cb, target);
            nb_label_define(cb, resume); // aqui cae la ejecucion cuando el Return correspondiente salte de vuelta
            nb_label_free(resume);
            return;
        }
        case N_EXPRSTMT:
            // Sentencia suelta que es una llamada (a\"MiProcedimiento(1,2)\"
            // sin usar su resultado) -- se evalua igual que cualquier
            // expresion, y el resultado en x0 simplemente se descarta.
            nb_emit_expr(ctx, stmt->a);
            return;
        case N_VARDECL: {
            // Global y Const: la variable es global de todas formas (ver
            // nb_resolve_var: todo lo que no es parametro ni Local lo es),
            // pero la declaracion tiene que ASIGNAR su valor. Antes se
            // ignoraba la linea entera: "Const MAXIMO = 10" compilaba y
            // MAXIMO valia 0 -- un fallo silencioso del mismo tipo que los
            // de la auditoria. Que una constante no se reasigne lo
            // comprueba el analizador (nb_es_constante).
            if (stmt->op == TOK_KW_GLOBAL || stmt->op == TOK_KW_CONST) {
                for (int32_t i = 0; i < stmt->list_count; i++) {
                    nb_node_t *item = stmt->list[i]; // N_ASSIGN: a=N_VAR, b=valor opcional
                    if (item->b) nb_emit_assign_int(ctx, item->a, item->b);
                }
                return;
            }
            if (stmt->op != TOK_KW_LOCAL) return;
            for (int32_t i = 0; i < stmt->list_count; i++) {
                nb_node_t *item = stmt->list[i]; // N_ASSIGN: a=N_VAR, b=valor opcional
                // La variable entra en el ambito de la funcion ANTES
                // de asignarle nada. Sin esto, "Local x" reservaba su hueco en la
                // pila pero NADIE lo usaba: todas las referencias seguian yendo a
                // la variable GLOBAL del mismo nombre, asi que Local no aislaba y
                // dos funciones que usaran "i" se pisaban entre ellas.
                if (ctx->locals && item->a && item->a->kind == N_VAR &&
                    !nb_local_scope_find(ctx->locals, item->a->text)) {
                    nb_local_scope_add(ctx->locals, item->a->text);
                }
                if (item->b) {
                    nb_emit_assign_int(ctx, item->a, item->b);
                } else {
                    // Sin valor inicial -- a diferencia de una
                    // GLOBAL (cuya region de datos asume el cargador
                    // que llega a cero, igual que un .bss de toda la
                    // vida), una LOCAL vive en la pila, que NO se
                    // limpia sola -- sin este cero explicito, tendria
                    // basura de lo que hubiera antes en esa memoria.
                    nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
                    nb_emit_store_var_from_x0(ctx, item->a->text);
                }
            }
            return;
        }
        case N_RETURN: {
            if (!ctx->return_label) {
                // Nivel superior: aqui SIEMPRE significa "volver del
                // Gosub correspondiente" (dentro de una funcion,
                // ctx->return_label esta puesto y se usa la rama de
                // abajo en su lugar). Se recupera la direccion que el
                // Gosub guardo en nuestra propia pila, y se salta ahi
                // con 'br' -- sin usar x30/ret en ningun momento.
                nb_pop_x1(cb);
                nb_codebuf_emit(cb, nb_enc_br(1));
                return;
            }
            if (stmt->a) {
                // Un Return no se puede envolver (salta fuera), asi
                // que libera aqui sus propios temporales: marca, evalua, se queda
                // con lo que devuelve y suelta el resto. Sin esto, una funcion
                // como Celda() -- Return Asc(Mid$(f$, i, 1)) -- perdia una cadena
                // por llamada: en el Tetris, unas cien por vuelta.
                bool con_temporales = g_t_marca && ctx->runtime && nb_crea_cadenas(ctx, stmt->a);
                if (con_temporales) { g_usa_temporales = true; nb_emit_bl(cb, g_t_marca); nb_push_x0(cb); }
                nb_emit_expr_para(ctx, stmt->a, g_tipo_retorno); // con el tipo de la funcion (F# devuelve decimal)
                // Devolver una cadena GUARDADA (una variable, un
                // elemento de array, un campo) entrega el mismo objeto a quien
                // llama: hay que contar esa referencia. Sin esto, al reasignar
                // el resultado se liberaba un texto que el array seguia usando,
                // y el programa se corrompia en la segunda llamada.
                if (g_tipo_retorno == NB_TYPE_STRING) nb_emit_retener_si_copia(ctx, stmt->a);
                if (con_temporales) {
                    nb_push_x0(cb);                                          // el valor de vuelta, a salvo
                    nb_codebuf_emit(cb, nb_enc_ldr_imm(0, NB_REG_SP, 2));    // la marca ([sp+16])
                    nb_emit_bl(cb, g_t_liberar);
                    nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));  // el valor, de vuelta
                    nb_codebuf_emit(cb, nb_enc_add_imm(NB_REG_SP, NB_REG_SP, 16)); // y fuera la marca
                }
            }
            else nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0)); // Return sin valor -- 0 por defecto, igual que BlitzPlus real
            nb_emit_b(cb, ctx->return_label);
            return;
        }
        case N_IF: {
            // Cadena completa: If -> ElseIf* -> Else?. Cada
            // condicion que falla salta a la siguiente comprobacion
            // (la del proximo ElseIf, o al Else/fin si no quedan
            // mas); cada bloque que se ejecuta salta al final del
            // If entero, saltandose todo lo que venga despues.
            nb_label_t *l_end = nb_label_new();
            nb_label_t *l_next = nb_label_new(); // siguiente comprobacion (proximo ElseIf, o Else/fin)

            nb_emit_cond_temporales(ctx, stmt->a);
            nb_emit_cbz(cb, 0, l_next);
            nb_emit_block(ctx, stmt->b);
            nb_emit_b(cb, l_end);

            for (int32_t i = 0; i < stmt->list_count; i++) {
                nb_node_t *ei = stmt->list[i];
                nb_label_t *this_check = l_next;
                nb_label_define(cb, this_check);
                l_next = nb_label_new(); // uno nuevo para la comprobacion SIGUIENTE a esta
                nb_label_free(this_check); // el anterior ya esta resuelto, no hace falta mas
                nb_emit_cond_temporales(ctx, ei->a);
                nb_emit_cbz(cb, 0, l_next);
                nb_emit_block(ctx, ei->b);
                nb_emit_b(cb, l_end);
            }

            nb_label_define(cb, l_next); // aqui cae si NINGUNA condicion (If ni ElseIf) fue verdadera
            if (stmt->c) nb_emit_block(ctx, stmt->c);
            nb_label_define(cb, l_end);
            nb_label_free(l_next);
            nb_label_free(l_end);
            return;
        }
        case N_WHILE: {
            nb_label_t *l_start = nb_label_new();
            nb_label_t *l_end = nb_label_new();
            nb_label_t *exit_anterior = ctx->exit_label;  // por si hay bucles anidados
            ctx->exit_label = l_end;
            nb_label_define(cb, l_start);
            nb_emit_cond_temporales(ctx, stmt->a); // condicion -> x0
            nb_emit_cbz(cb, 0, l_end);       // si es falsa, sal del bucle
            nb_emit_block(ctx, stmt->b);
            nb_emit_b(cb, l_start);
            nb_label_define(cb, l_end);
            ctx->exit_label = exit_anterior;
            nb_label_free(l_start);
            nb_label_free(l_end);
            return;
        }
        case N_ENDPROGRAM: {
            // End -- terminar el programa aqui mismo.
            //
            // Se llama a SYS_EXIT (0) en vez de saltar al epilogo:
            // desde dentro de una funcion o de un bucle anidado, el
            // epilogo del programa principal no es alcanzable con un
            // simple salto, y ademas habria que deshacer a mano todo
            // lo que esos niveles dejaron en la pila. El sistema
            // retira la tarea entera y ese problema desaparece.
            //
            // Esto tambien se parseaba desde el principio sin
            // generarse: un End no hacia nada y el programa seguia.
            nb_emit_load_imm64(cb, 8, 0);   // SYS_EXIT
            nb_codebuf_emit(cb, nb_enc_svc(0));
            return;
        }
        case N_EXIT: {
            // Salir del bucle mas interno. Fuera de un bucle no hay
            // nada de lo que salir: se ignora en silencio, como en
            // BlitzPlus, en vez de generar un salto a ninguna parte.
            if (ctx->exit_label) nb_emit_b(cb, ctx->exit_label);
            return;
        }
        case N_REPEAT: {
            nb_label_t *l_start = nb_label_new();
            // Repeat no necesitaba etiqueta de fin (sale por la
            // condicion, no por un salto), pero Exit si: hay que
            // darle un sitio al que saltar.
            nb_label_t *l_end = nb_label_new();
            nb_label_t *exit_anterior = ctx->exit_label;
            ctx->exit_label = l_end;
            nb_label_define(cb, l_start);
            nb_emit_block(ctx, stmt->a);
            if (stmt->b) {
                nb_emit_cond_temporales(ctx, stmt->b); // condicion Until -> x0
                nb_emit_cbz(cb, 0, l_start);     // si es FALSA, repite (esa es la semantica de "Until")
            } else {
                nb_emit_b(cb, l_start); // Forever -- bucle infinito, sin condicion
            }
            nb_label_define(cb, l_end);
            ctx->exit_label = exit_anterior;
            nb_label_free(l_start);
            nb_label_free(l_end);
            return;
        }
        case N_FOR: {
            // var = inicio
            nb_emit_expr_int(ctx, stmt->a);
            nb_emit_store_var_from_x0(ctx, stmt->text);

            // paso (por defecto 1) -- se guarda en la pila real
            // durante TODO el bucle: el cuerpo puede contener
            // cualquier cosa, y esta es la unica forma de que
            // sobreviva sin pisarse, sin necesitar una variable local
            // propia (que ademas complicaria el codigo generado a
            // nivel de programa, fuera de una funcion).
            if (stmt->c) nb_emit_expr_int(ctx, stmt->c);
            else nb_codebuf_emit(cb, nb_enc_movz(0, 1, 0)); // paso por defecto = 1
            nb_push_x0(cb);

            nb_label_t *l_check = nb_label_new();
            nb_label_t *l_neg = nb_label_new();
            nb_label_t *l_body = nb_label_new();
            nb_label_t *l_end = nb_label_new();

            // Exit sale por l_end, que es justo donde se retira el
            // paso de la pila -- asi que salir a mitad de un For deja
            // la pila igual de equilibrada que terminarlo del todo.
            nb_label_t *exit_anterior = ctx->exit_label;
            ctx->exit_label = l_end;

            nb_label_define(cb, l_check);
            nb_emit_load_var_x0(ctx, stmt->text);
            nb_push_x0(cb);
            nb_emit_expr_int(ctx, stmt->b); // limite -- evaluado UNA sola vez por vuelta
            nb_pop_x1(cb);                   // x1 = var, x0 = limite
            nb_codebuf_emit(cb, nb_enc_ldr_imm(2, NB_REG_SP, 0)); // x2 = paso (mirar la cima sin sacarlo)
            nb_codebuf_emit(cb, nb_enc_subs_imm(NB_REG_ZR, 2, 0)); // cmp paso, #0
            nb_emit_bcond(cb, NB_COND_LT, l_neg);
            // paso >= 0: seguir mientras var <= limite -- terminar si var > limite
            nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 1, 0)); // cmp var, limite
            nb_emit_bcond(cb, NB_COND_GT, l_end);
            nb_emit_b(cb, l_body);
            nb_label_define(cb, l_neg);
            // paso < 0: seguir mientras var >= limite -- terminar si var < limite
            nb_codebuf_emit(cb, nb_enc_subs_reg(NB_REG_ZR, 1, 0)); // cmp var, limite
            nb_emit_bcond(cb, NB_COND_LT, l_end);

            nb_label_define(cb, l_body);
            nb_emit_block(ctx, stmt->d);
            // var = var + paso
            nb_emit_load_var_x0(ctx, stmt->text);
            nb_codebuf_emit(cb, nb_enc_ldr_imm(1, NB_REG_SP, 0)); // x1 = paso (mirar la cima)
            nb_codebuf_emit(cb, nb_enc_add_reg(0, 0, 1, 0));
            nb_emit_store_var_from_x0(ctx, stmt->text);
            nb_emit_b(cb, l_check);

            nb_label_define(cb, l_end);
            nb_codebuf_emit(cb, nb_enc_ldr_post(NB_REG_ZR, NB_REG_SP, 16)); // retira el paso de la pila (valor descartado)
            ctx->exit_label = exit_anterior;
            nb_label_free(l_check); nb_label_free(l_neg); nb_label_free(l_body); nb_label_free(l_end);
            return;
        }
        default:
            return; // el resto de sentencias llegan en piezas siguientes
    }
}

static void nb_emit_block(nb_codegen_ctx_t *ctx, nb_node_t *block) {
    for (int32_t i = 0; i < block->list_count; i++) {
        nb_emit_stmt(ctx, block->list[i]);
    }
}

// Cuenta cuantas locales (Local a,b,c cuenta 3) hay en TODO el cuerpo
// de una funcion, incluyendo dentro de If/While/Repeat/For anidados
// -- hace falta saberlo ANTES de emitir el prologo (que reserva el
// tamaño del marco de una vez), y el propio codigo del cuerpo se
// genera despues, con exactamente los mismos Local que aqui se
// contaron (nb_local_scope_add les asignara los mismos huecos, en el
// mismo orden).
static int32_t nb_count_locals(nb_node_t *block) {
    int32_t total = 0;
    for (int32_t i = 0; i < block->list_count; i++) {
        nb_node_t *s = block->list[i];
        switch (s->kind) {
            case N_VARDECL:
                if (s->op == TOK_KW_LOCAL) total += s->list_count;
                break;
            case N_IF:
                total += nb_count_locals(s->b);
                for (int32_t j = 0; j < s->list_count; j++) total += nb_count_locals(s->list[j]->b);
                if (s->c) total += nb_count_locals(s->c);
                break;
            case N_WHILE: total += nb_count_locals(s->b); break;
            case N_REPEAT: total += nb_count_locals(s->a); break;
            case N_FOR: total += nb_count_locals(s->d); break;
            default: break;
        }
    }
    return total;
}

static uint32_t nb_align16(uint32_t n) { return (n + 15u) & ~15u; }

// Genera el cuerpo completo de UNA funcion: prologo, parametros
// copiados a sus huecos locales, cuerpo, epilogo compartido (a donde
// salta cualquier Return, y tambien el final natural del cuerpo si no
// hay ningun Return explicito -- con 0 como valor por defecto, igual
// que BlitzPlus real).
static void nb_emit_funcdef(nb_codegen_ctx_t *outer, nb_func_entry_t *fe, nb_node_t *funcdef) {
    nb_codebuf_t *cb = outer->cb;
    nb_local_scope_t locals; nb_local_scope_init(&locals);
    nb_var_type_t tipo_retorno_fuera = g_tipo_retorno;
    g_tipo_retorno = nb_infer_type(funcdef->text);   // "F#" devuelve decimal, "F$" cadena, "F" entero

    int32_t param_count = funcdef->list_count;
    // Mismo limite que en la llamada, y con el mismo motivo para decirlo en
    // vez de recortar: una funcion con nueve parametros se quedaba con ocho
    // y el noveno valia siempre cero dentro del cuerpo.
    if (param_count > 8) {
        nb_codegen_error("como mucho ocho parametros en una Function", funcdef->line);
        param_count = 8;
    }
    for (int32_t i = 0; i < param_count; i++) {
        nb_local_scope_add(&locals, funcdef->list[i]->text);
    }
    int32_t total_locals = param_count + nb_count_locals(funcdef->d);
    uint32_t frame_size = nb_align16((uint32_t)(16 + total_locals * 8));

    nb_label_define(cb, fe->label);
    nb_codebuf_emit(cb, nb_enc_stp_pre(29, 30, NB_REG_SP, -(int32_t)(frame_size / 8)));
    nb_codebuf_emit(cb, nb_enc_add_imm(29, NB_REG_SP, 0)); // mov x29, sp

    for (int32_t i = 0; i < param_count; i++) {
        nb_codebuf_emit(cb, nb_enc_stur(i, 29, locals.entries[i].offset));
    }
    // Las locales que NO son parametros, a cero. Antes traian lo
    // que hubiera quedado en la pila de otra llamada, y al asignarles una
    // cadena el compilador "liberaba el valor anterior", que era basura: la
    // memoria se rompia y los textos se estropeaban unas llamadas despues.
    for (int32_t i = param_count; i < total_locals; i++) {
        int32_t off = 16 + i * 8;
        nb_codebuf_emit(cb, nb_enc_stur(NB_REG_ZR, 29, off));
    }

    nb_label_t *l_return = nb_label_new();
    nb_label_table_t flbl; nb_label_table_init(&flbl);
    nb_scan_labels(funcdef->d, &flbl); // etiquetas propias de ESTA funcion -- ambito aparte, no comparte con el nivel superior
    // OJO: este contexto se construye campo a campo, asi que CADA
    // campo nuevo que se añada a nb_codegen_ctx_t hay que añadirlo
    // tambien AQUI. Si se olvida, dentro de las funciones ese campo
    // vale NULL y lo que dependa de el deja de generarse en silencio
    // -- paso con exit_label y bounds_error_label: dentro de una
    // funcion, ni Exit salia del bucle ni se comprobaban los indices
    // de array, y el programa compilaba igual de limpio.
    //
    // exit_label empieza en NULL a proposito (un Exit suelto al
    // principio de una funcion no debe saltar a un bucle de FUERA);
    // bounds_error_label SI se hereda, porque la rutina de error es
    // una sola para todo el programa.
    nb_codegen_ctx_t ctx = { cb, outer->globals, &locals, outer->functions, outer->runtime,
                             outer->arrays, &flbl, outer->restore, outer->data_cursor,
                             outer->data_start_offset, outer->fields, outer->types,
                             outer->color_var, outer->cls_color_var, l_return,
                             NULL,                          /* exit_label */
                             outer->bounds_error_label };
    nb_emit_block(&ctx, funcdef->d);

    // Final natural del cuerpo (sin Return explicito): 0 por defecto,
    // cayendo directo en el mismo epilogo que usa cualquier Return.
    nb_codebuf_emit(cb, nb_enc_movz(0, 0, 0));
    nb_label_define(cb, l_return);
    // Las cadenas locales de la funcion se sueltan al salir: sin
    // esto, cada llamada dejaba una referencia de mas a la cadena copiada (por
    // ejemplo la de un array), y nadie la liberaba jamas.
    {
        nb_label_t *rel = outer->runtime ? nb_rt_obligatoria(outer, "nb_string_release", funcdef->line) : NULL;
        bool alguna = false;
        // OJO: los PARAMETROS no (los presta quien llama; soltarlos aqui le
        // destruye su cadena). Solo las locales propias de la funcion.
        for (uint32_t i = (uint32_t)param_count; rel && i < locals.count; i++)
            if (nb_infer_type(locals.entries[i].name) == NB_TYPE_STRING) alguna = true;
        if (alguna) {
            nb_codebuf_emit(cb, nb_enc_add_imm(NB_REG_SP, 29, 0));   // la pila, al marco
            nb_push_x0(cb);                                           // el valor de vuelta, a salvo
            for (uint32_t i = (uint32_t)param_count; i < locals.count; i++) {
                if (nb_infer_type(locals.entries[i].name) != NB_TYPE_STRING) continue;
                nb_codebuf_emit(cb, nb_enc_ldur(0, 29, locals.entries[i].offset));
                nb_emit_bl(cb, rel);
            }
            nb_codebuf_emit(cb, nb_enc_ldr_post(0, NB_REG_SP, 16));
        }
    }
    // La pila vuelve al marco ANTES de recuperar x29/x30: un
    // Return dentro de un For (o de un While, o con temporales a medias)
    // dejaba en la pila lo que el bucle habia guardado, y la funcion volvia a
    // una direccion equivocada. Exit ya lo cuidaba; Return no.
    nb_codebuf_emit(cb, nb_enc_add_imm(NB_REG_SP, 29, 0));   // mov sp, x29
    nb_codebuf_emit(cb, nb_enc_ldp_post(29, 30, NB_REG_SP, (int32_t)(frame_size / 8)));
    nb_codebuf_emit(cb, nb_enc_ret());

    nb_label_free(l_return);
    nb_label_table_free(&flbl);
    nb_local_scope_free(&locals);
    g_tipo_retorno = tipo_retorno_fuera;
}

// Barrido recursivo: ¿hace falta el bloque de runtime en algun sitio
// de este arbol? -- una cadena (literal N_STR, o una variable/
// funcion/campo/indice cuyo nombre termina en $), o un array (N_DIM,
// que usa nb_alloc igual que las cadenas). Si no aparece ninguna de
// las dos cosas, no hace falta incrustar el bloque en absoluto.
// Recorre a/b/c/d y la lista de forma generica, sin distinguir por
// tipo de nodo -- los campos que un nodo concreto no usa son NULL o
// listas vacias, recorrerlos de mas no hace daño, y evita tener que
// mantener un caso por cada tipo de nodo (con el riesgo de olvidar
// uno y no detectar un uso real).
// ¿El programa dibuja algo? Solo entonces hace falta la variable
// global oculta del color activo -- un programa que no usa graficos
// no tiene por que cargar con ella ni con su inicializacion.
static bool nb_tree_uses_graphics(nb_node_t *n) {
    if (!n) return false;
    if (n->kind == N_COLOR || n->kind == N_PLOT || n->kind == N_RECT ||
        n->kind == N_OVAL || n->kind == N_TEXT || n->kind == N_CLS ||
        n->kind == N_LINE) return true;
    if (nb_tree_uses_graphics(n->a) || nb_tree_uses_graphics(n->b) ||
        nb_tree_uses_graphics(n->c) || nb_tree_uses_graphics(n->d)) return true;
    for (int32_t i = 0; i < n->list_count; i++) {
        if (nb_tree_uses_graphics(n->list[i])) return true;
    }
    return false;
}

// ¿Hay algun Dim en cualquier parte del arbol (funciones, If, bucles)?
static bool nb_arbol_tiene_dim(nb_node_t *n) {
    if (!n) return false;
    if (n->kind == N_DIM) return true;
    if (nb_arbol_tiene_dim(n->a) || nb_arbol_tiene_dim(n->b) || nb_arbol_tiene_dim(n->c) || nb_arbol_tiene_dim(n->d)) return true;
    for (int32_t i = 0; i < n->list_count; i++) if (nb_arbol_tiene_dim(n->list[i])) return true;
    return false;
}

static bool nb_tree_needs_runtime(nb_node_t *n) {
    if (!n) return false;
    if (n->kind == N_STR || n->kind == N_DIM) return true;
    if (n->kind == N_NEW || n->kind == N_DELETE || n->kind == N_PRINT ||
        n->kind == N_CREATEWINDOW || n->kind == N_TEXT) return true; // su titulo/texto pasa por nb_string_cstr // Print SIEMPRE pasa por nb_string_cstr, incluso para una cadena que ya existia
    // Las trigonometricas viven en el bloque de runtime (nb_math.c),
    // y una llamada como "Sin(45)" no contiene ninguna cadena ni nada
    // mas que delate que el bloque hace falta -- hay que reconocerlas
    // por nombre, o el bloque no se incrustaria y la llamada quedaria
    // sin destino, en silencio.
    if (n->kind == N_CALL && n->text) {
        if (nb_text_eq_cstr(n->text, "Sin") || nb_text_eq_cstr(n->text, "Sin#") ||
            nb_text_eq_cstr(n->text, "Cos") || nb_text_eq_cstr(n->text, "Cos#") ||
            nb_text_eq_cstr(n->text, "Tan") || nb_text_eq_cstr(n->text, "Tan#") ||
            nb_text_eq_cstr(n->text, "ATan") || nb_text_eq_cstr(n->text, "ATan#") ||
            nb_text_eq_cstr(n->text, "Len") || nb_text_eq_cstr(n->text, "Val") ||
            nb_text_eq_cstr(n->text, "CreateButton") ||
            nb_text_eq_cstr(n->text, "SetGadgetText") ||
            nb_text_eq_cstr(n->text, "AddGadgetItem") ||
            nb_text_eq_cstr(n->text, "GadgetText$") ||
            nb_text_eq_cstr(n->text, "Rnd") ||
            nb_text_eq_cstr(n->text, "Seed") ||
            nb_text_eq_cstr(n->text, "OpenFile") ||
            nb_text_eq_cstr(n->text, "ReadLine$") ||
            nb_text_eq_cstr(n->text, "Instr") ||
            nb_text_eq_cstr(n->text, "Upper$") || nb_text_eq_cstr(n->text, "Lower$") ||
            nb_text_eq_cstr(n->text, "Trim$") || nb_text_eq_cstr(n->text, "Replace$") ||
            nb_text_eq_cstr(n->text, "String$") || nb_text_eq_cstr(n->text, "LSet$") || nb_text_eq_cstr(n->text, "RSet$") ||
            nb_text_eq_cstr(n->text, "Hex$") || nb_text_eq_cstr(n->text, "Bin$") ||
            nb_text_eq_cstr(n->text, "WriteLine") || nb_text_eq_cstr(n->text, "NextFile$") ||
            nb_text_eq_cstr(n->text, "FileSize") || nb_text_eq_cstr(n->text, "FileType") ||
            nb_text_eq_cstr(n->text, "FileExists") || nb_text_eq_cstr(n->text, "DeleteFile") ||
            nb_text_eq_cstr(n->text, "CreateDir") || nb_text_eq_cstr(n->text, "ReadDir") ||
            nb_text_eq_cstr(n->text, "Chr$") || nb_text_eq_cstr(n->text, "Asc") ||
            nb_text_eq_cstr(n->text, "Exp") || nb_text_eq_cstr(n->text, "Log") ||
            nb_text_eq_cstr(n->text, "CreateLabel") ||
            nb_text_eq_cstr(n->text, "Input$") || nb_text_eq_cstr(n->text, "Input") ||
            nb_text_eq_cstr(n->text, "LoadImage") ||
            nb_text_eq_cstr(n->text, "LoadSound") ||   // su nombre de archivo pasa por nb_string_cstr
            // Estas dos construyen una cadena a partir de un buffer:
            // nb_alloc + nb_string_new + nb_free, todo del runtime.
            nb_text_eq_cstr(n->text, "CpuName$") ||
            nb_text_eq_cstr(n->text, "TaskName$") ||
            nb_text_eq_cstr(n->text, "TextAreaText$") ||
            nb_text_eq_cstr(n->text, "TextWidth") ||   // su argumento pasa por nb_string_cstr
            nb_text_eq_cstr(n->text, "LoadAnimImage") ||
            nb_text_eq_cstr(n->text, "SaveImage") ||
            nb_text_eq_cstr(n->text, "WriteFile") ||
            nb_text_eq_cstr(n->text, "I2cWrite") || nb_text_eq_cstr(n->text, "I2cRead$") ||
            nb_text_eq_cstr(n->text, "SpiTransfer$") ||
            // Red. UdpSend pasa dos cadenas por
            // nb_string_cstr; UdpRecv$ y UdpFrom$ construyen una con
            // nb_alloc + nb_string_new + nb_free. El nombre acabado en $
            // ya bastaria para que se reconozcan, pero se ponen a mano
            // igual que CpuName$ y TaskName$: si el bloque de runtime no
            // se incrusta, la llamada se queda sin destino Y SIN AVISO.
            nb_text_eq_cstr(n->text, "UdpSend") || nb_text_eq_cstr(n->text, "UdpRecv$") ||
            nb_text_eq_cstr(n->text, "UdpFrom$") ||
            nb_text_eq_cstr(n->text, "HttpGet") || nb_text_eq_cstr(n->text, "HttpPost") ||
            nb_text_eq_cstr(n->text, "HttpBody$") || nb_text_eq_cstr(n->text, "HttpFail$") ||
            nb_text_eq_cstr(n->text, "NetIp$") || nb_text_eq_cstr(n->text, "NetMask$") ||
            nb_text_eq_cstr(n->text, "NetGateway$") || nb_text_eq_cstr(n->text, "NetDns$") ||
            nb_text_eq_cstr(n->text, "CreateCheckBox") || nb_text_eq_cstr(n->text, "CreateRadio") ||
            nb_text_eq_cstr(n->text, "SetTextAreaText") || nb_text_eq_cstr(n->text, "AddTextAreaText") ||
            nb_text_eq_cstr(n->text, "InsertGadgetItem") || nb_text_eq_cstr(n->text, "ModifyGadgetItem") ||
            nb_text_eq_cstr(n->text, "CreateMenu") || nb_text_eq_cstr(n->text, "CreateToolBar") ||
            nb_text_eq_cstr(n->text, "SetToolBarTips") || nb_text_eq_cstr(n->text, "SetPanelImage") ||
            nb_text_eq_cstr(n->text, "AddTreeViewNode") || nb_text_eq_cstr(n->text, "InsertTreeViewNode") ||
            nb_text_eq_cstr(n->text, "ModifyTreeViewNode")) return true;
    }
    if ((n->kind == N_VAR || n->kind == N_CALL || n->kind == N_FIELD || n->kind == N_INDEX) && n->text) {
        if (nb_infer_type(n->text) == NB_TYPE_STRING) return true;
    }
    if (nb_tree_needs_runtime(n->a) || nb_tree_needs_runtime(n->b) ||
        nb_tree_needs_runtime(n->c) || nb_tree_needs_runtime(n->d)) return true;
    for (int32_t i = 0; i < n->list_count; i++) {
        if (nb_tree_needs_runtime(n->list[i])) return true;
    }
    return false;
}

// ---- Tipos de los argumentos de las incorporadas ----
//
// Antes, pasar un numero donde una incorporada espera una cadena -- o al
// reves -- compilaba sin una queja: Left$(12345, 2) le daba el 12345
// como si fuera la direccion de una cadena, y el programa caia al
// ejecutarse; Sqr("hola") calculaba la raiz de una direccion de memoria.
// Hallazgo de la auditoria del compilador viejo que seguia vivo en el
// nuevo. Ahora una pasada sobre todo el arbol, ANTES de generar codigo,
// comprueba cada llamada a una incorporada contra su firma:
//   S = cadena, N = numero (entero o decimal).
// Las posiciones que pasen de la firma no se comprueban (Instr y Mid$
// tienen un argumento opcional). Si el programa define una funcion con el
// mismo nombre, manda la suya.
// OJO -- NOMBRES COMO ARRAYS, NO PUNTEROS. nbc.pro se enlaza
// en la direccion 0 y el cargador de Nemo OS lo copia sin reubicar los
// punteros de DATOS: una tabla { const char *nombre; ... } conservaria
// direcciones de enlace (0x16948...) y leerla dentro de Nemo OS es un Data
// Abort, y solo al compilar DENTRO del sistema: en el Mac funciona. Misma
// regla que nb_lexer.c y que NB_RUNTIME_SYMS (tools/nb_elf_extract.py).
// (El nombre mide 24 y no 20: "SelectedTreeViewNode" tiene 20 letras y, en
// 20 bytes, C la guardaria SIN terminador y sin avisar.)
typedef struct { char nombre[24]; char firma[6]; } nb_firma_t;
static const nb_firma_t nb_firmas[] = {
    { "Left$", "SN" }, { "Right$", "SN" }, { "Mid$", "SNN" }, { "Len", "S" }, { "Val", "S" },
    { "Instr", "SSN" }, { "Val#", "S" }, { "Upper$", "S" }, { "Lower$", "S" }, { "Trim$", "S" },
    { "Replace$", "SSS" }, { "Asc", "S" }, { "Chr$", "N" }, { "Str$", "N" },
    { "Sqr", "N" }, { "Sqr#", "N" }, { "Sin", "N" }, { "Sin#", "N" }, { "Cos", "N" }, { "Cos#", "N" },
    { "Tan", "N" }, { "Tan#", "N" }, { "ATan", "N" }, { "ATan#", "N" }, { "Exp", "N" }, { "Exp#", "N" },
    { "Log", "N" }, { "Log#", "N" }, { "Floor", "N" }, { "Floor#", "N" }, { "Ceil", "N" }, { "Ceil#", "N" },
    { "Abs", "N" }, { "Abs#", "N" }, { "Abs%", "N" }, { "Int", "N" }, { "Int%", "N" }, { "Float", "N" },
    { "Float#", "N" }, { "Sgn", "N" }, { "Min", "NN" }, { "Max", "NN" }, { "Seed", "N" }, { "Rnd", "N" }, { "Rand", "NN" },
    { "KeyDown", "N" }, { "KeyHit", "N" },
    { "CreateButton", "SNNNN" }, { "CreateLabel", "SNNNN" }, { "CreatePanel", "NNNN" },
    { "CreateTextField", "NNNN" }, { "CreateListBox", "NNNN" }, { "SetGadgetText", "NS" },
    { "GadgetText$", "N" }, { "FreeGadget", "N" }, { "AddGadgetItem", "NS" }, { "ClearGadgetItems", "N" },
    { "SelectedGadgetItem", "N" }, { "SelectGadgetItem", "NN" }, { "CountGadgetItems", "N" },
    { "OpenFile", "S" }, { "ReadLine$", "N" }, { "Eof", "N" }, { "CloseFile", "N" }, { "WriteFile", "SS" },
    { "LoadImage", "S" }, { "CreateImage", "NN" }, { "DrawImage", "NNN" }, { "HandleImage", "NNN" },
    { "LoadAnimImage", "SNNNN" }, { "DrawBlock", "NNN" },
    // Filas de pixeles. "NNNNN" son exactamente las 5 letras
    // que caben en firma[6] con su terminador -- de ahi que RowRun y
    // RowSkip sean dos comandos en vez de uno con argumento de modo.
    { "FillRow", "NNNNN" }, { "RowRun", "NNNNN" }, { "RowSkip", "NNNNN" },
    // ReadRow/WriteRow llevan CUATRO letras a proposito: su quinto
    // argumento es un array declarado con Dim, no una expresion, y se
    // valida en el generador (las posiciones que pasan de la firma no se
    // comprueban aqui).
    { "ReadRow", "NNNN" }, { "WriteRow", "NNNN" },
    // datos del sistema. Solo los que llevan argumento: los de cero (CpuCores,
    // TotalRam, TaskCount, Hour...) no tienen nada que comprobar.
    { "PartitionType", "N" }, { "PartitionStart", "N" }, { "PartitionSectors", "N" },
    // editor: medir texto y leer un TextArea de vuelta
    { "TextWidth", "S" }, { "TextAreaText$", "N" }, { "TextAreaLen", "NN" },
    { "TextAreaLineLen", "NN" }, { "TextAreaLineOfChar", "NN" },
    { "TaskSlot", "N" }, { "TaskWindow", "N" }, { "TaskTurns", "N" }, { "TaskName$", "N" },
    // sonido: el archivo es un WAV; volumen y pan son decimales, pero "N"
    // vale para entero y decimal por igual
    { "LoadSound", "S" }, { "FreeSound", "N" }, { "PlaySound", "N" },
    { "SoundVolume", "NN" }, { "SoundPan", "NN" }, { "SoundPitch", "NN" },
    { "MaskImage", "NN" }, { "ImageWidth", "N" }, { "ImageHeight", "N" }, { "FreeImage", "N" },
    { "CopyImage", "N" }, { "SaveImage", "NS" },
    { "GpioMode", "NN" }, { "GpioWrite", "NN" }, { "GpioRead", "N" }, { "GpioPwm", "NNN" }, { "WindowButtons", "NNN" },
    { "CreateCheckBox", "SNNNN" }, { "CreateRadio", "SNNNN" }, { "CreateTextArea", "NNNN" }, { "CreateProgBar", "NNNN" },
    { "CreateComboBox", "NNNN" }, { "CreateTabber", "NNNN" }, { "CreateSlider", "NNNNN" }, { "CreateScrollBar", "NNNNN" }, { "UpdateProgBar", "NN" },
    { "ButtonState", "N" }, { "SetButtonState", "NN" }, { "SetSliderRange", "NNN" }, { "SetSliderValue", "NN" },
    { "SliderValue", "N" }, { "HideGadget", "N" }, { "ShowGadget", "N" }, { "DisableGadget", "N" }, { "EnableGadget", "N" },
    { "ActivateGadget", "N" }, { "RemoveGadgetItem", "NN" }, { "InsertGadgetItem", "NNS" }, { "ModifyGadgetItem", "NNS" },
    { "SetTextAreaText", "NS" }, { "AddTextAreaText", "NS" }, { "SetPanelColor", "NNNN" },
    { "CreateMenu", "SNN" }, { "ShowContextMenu", "NNN" }, { "CheckMenu", "N" }, { "UncheckMenu", "N" }, { "EnableMenu", "N" }, { "DisableMenu", "N" },
    { "UpdateWindowMenu", "N" }, { "CreateToolBar", "SNNNN" }, { "SetToolBarTips", "NS" }, { "EnableToolBarItem", "NN" },
    { "DisableToolBarItem", "NN" }, { "SetPanelImage", "NS" }, { "CreateTimer", "N" }, { "FreeTimer", "N" },
    { "PauseTimer", "N" }, { "ResumeTimer", "N" }, { "ResetTimer", "N" }, { "TimerTicks", "N" },
    { "MouseHit", "N" }, { "ClsColor", "NNN" },
    { "SetGadgetShape", "NNNNN" }, { "GadgetX", "N" }, { "GadgetY", "N" }, { "GadgetWidth", "N" }, { "GadgetHeight", "N" },
    { "ClientWidth", "" }, { "ClientHeight", "" }, { "MicroSecs", "" }, { "KeyBank", "N" },
    { "WriteLine", "NS" }, { "SeekFile", "NN" }, { "FilePos", "N" }, { "FileLength", "N" },
    { "FileSize", "S" }, { "FileType", "S" }, { "FileExists", "S" }, { "DeleteFile", "S" }, { "CreateDir", "S" },
    { "ReadDir", "S" }, { "NextFile$", "N" }, { "CloseDir", "N" }, { "RenameFile", "SS" },
    { "SetGadgetGroup", "NN" }, { "GadgetGroup", "N" },
    { "String$", "SN" }, { "LSet$", "SN" }, { "RSet$", "SN" }, { "Hex$", "N" }, { "Bin$", "N" },
    { "CreateTreeView", "NNNN" }, { "TreeViewRoot", "N" }, { "AddTreeViewNode", "SN" }, { "InsertTreeViewNode", "NSN" },
    { "ModifyTreeViewNode", "NS" }, { "FreeTreeViewNode", "N" }, { "ExpandTreeViewNode", "N" }, { "CollapseTreeViewNode", "N" },
    { "CountTreeViewNodes", "N" }, { "SelectedTreeViewNode", "N" }, { "SelectTreeViewNode", "N" },
    { "CreateCanvas", "NNNN" }, { "CanvasBuffer", "N" }, { "SetBuffer", "N" }, { "ImageBuffer", "N" }, { "FlipCanvas", "N" },
    { "I2cWrite", "NS" }, { "I2cRead$", "NN" }, { "SpiTransfer$", "SNNN" },
    // Red. La IP de UdpSend va en el segundo sitio y es una
    // CADENA ("192.168.1.40"); poner ahi un numero por despiste daria una
    // direccion de memoria como direccion de red, y sin esta firma
    // compilaria sin una queja.
    { "UdpOpen", "N" }, { "UdpClose", "N" }, { "UdpSend", "NSNS" },
    { "UdpRecv$", "N" }, { "UdpFrom$", "N" }, { "UdpFromPort", "N" }, { "UdpLost", "N" },
    { "UdpPending", "N" }, { "UdpPort", "N" },
    // HTTP. Los cuatro de cero argumentos (HttpState, HttpCode, HttpBody$,
    // HttpFail$) no llevan firma porque no hay nada que comprobar.
    { "HttpGet", "SNS" }, { "HttpPost", "SNSS" },
    { "", "" }
};

static bool nb_es_funcion_propia(nb_node_t *program, nb_string_t *name) {
    for (int32_t i = 0; i < program->list_count; i++)
        if (program->list[i]->kind == N_FUNCDEF && nb_string_eq(program->list[i]->text, name)) return true;
    return false;
}

// ---- Nombres alternativos ----
//
// El mismo comando con el nombre que le pone otro Basic. No hay nada nuevo
// por debajo: se cambia el nombre en el arbol ANTES de cualquier otra
// pasada, y a partir de ahi el compilador entero --firmas, generacion,
// avisos-- ve el nombre principal y no se entera de que hubo un alias.
//
// Se hace asi, y no repartiendo comprobaciones de alias por el generador,
// porque el generador reconoce cada comando por su nombre en veinte sitios
// distintos: anadir un alias a mano en todos ellos es garantizar que uno se
// quede fuera y que ese alias falle solo en un caso concreto.
//
// Si el programa define una Function con ese nombre, manda la suya: el
// alias no le quita el nombre a nadie.
typedef struct { char alias[24]; char real[24]; } nb_alias_t;
static const nb_alias_t nb_alias[] = {
    { "CreateListView",     "CreateListBox" },
    { "AddToList",          "AddGadgetItem" },      // AddGadgetItem es generico:
    { "AddToComboBox",      "AddGadgetItem" },      // sirve para cualquier lista
    { "ClearList",          "ClearGadgetItems" },
    { "SelectedList",       "SelectedGadgetItem" },
    { "CreateProgressBar",  "CreateProgBar" },
    { "UpdateProgressBar",  "UpdateProgBar" },
    { "SetSoundVolume",     "SoundVolume" },
    { "SetSoundPitch",      "SoundPitch" },
    { "SetSoundPan",        "SoundPan" },
    // La zona de dibujo es lo que un programa necesita saber: el ancho de la
    // VENTANA incluiria el marco y el titulo, y dibujar contando con el
    // pintaria debajo de ellos.
    { "WindowWidth",        "ClientWidth" },
    { "WindowHeight",       "ClientHeight" },
    { "RndInt",             "Rnd" },
    { "", "" }
};

static void nb_traducir_alias(nb_node_t *n, nb_node_t *program) {
    if (!n) return;
    if (n->kind == N_CALL && n->text && !nb_es_funcion_propia(program, n->text)) {
        for (const nb_alias_t *a = nb_alias; a->alias[0]; a++) {
            if (nb_text_eq_cstr(n->text, a->alias)) {
                uint32_t largo = 0;
                while (a->real[largo]) largo++;
                nb_string_t *nuevo = nb_string_new(a->real, largo);
                if (nuevo) { nb_string_release(n->text); n->text = nuevo; }
                break;
            }
        }
    }
    nb_traducir_alias(n->a, program); nb_traducir_alias(n->b, program);
    nb_traducir_alias(n->c, program); nb_traducir_alias(n->d, program);
    for (int32_t i = 0; i < n->list_count; i++) nb_traducir_alias(n->list[i], program);
}

// Sin snprintf: este generador corre tambien DENTRO de Nemo OS (nbc.pro),
// donde no hay biblioteca estandar de C.
static uint32_t nb_msg_poner(char *msg, uint32_t k, uint32_t max, const char *t) {
    while (*t && k < max - 1) msg[k++] = *t++;
    return k;
}
static const char *nb_msg_argumento(int32_t num, const char *nombre, bool cadena) {
    // (sin lista de punteros: vease la nota de nb_firma_t)
    static char msg[96];
    char cifra[2] = { (char)('0' + (num % 10)), 0 };
    uint32_t k = 0;
    k = nb_msg_poner(msg, k, sizeof msg, "el argumento ");
    k = nb_msg_poner(msg, k, sizeof msg, cifra);
    k = nb_msg_poner(msg, k, sizeof msg, " de ");
    k = nb_msg_poner(msg, k, sizeof msg, nombre);
    k = nb_msg_poner(msg, k, sizeof msg, cadena ? " tiene que ser una cadena" : " tiene que ser un numero");
    msg[k] = 0;
    return msg;
}

// La firma de una orden incorporada ("NNS", "SN"...), para saber
// si un argumento va como ENTERO. Sin esto, un decimal pasado a Mid$ o a
// DrawImage llegaba como los bits del double.
const char *nb_firma_de(nb_string_t *nombre) {
    for (uint32_t i = 0; i < sizeof nb_firmas / sizeof nb_firmas[0]; i++)
        if (nb_text_eq_cstr(nombre, nb_firmas[i].nombre)) return nb_firmas[i].firma;
    return NULL;
}

static void nb_comprobar_argumentos(nb_node_t *llamada, const char *nombre, const char *firma) {
    for (int32_t i = 0; firma[i] && i < llamada->list_count; i++) {
        nb_var_type_t t = nb_expr_type(llamada->list[i]);
        bool es_cadena = t == NB_TYPE_STRING;
        if (firma[i] == 'S' && !es_cadena) { nb_codegen_error(nb_msg_argumento(i + 1, nombre, true), llamada->line); return; }
        if (firma[i] == 'N' && es_cadena)  { nb_codegen_error(nb_msg_argumento(i + 1, nombre, false), llamada->line); return; }
    }
}

static void nb_comprobar_llamadas(nb_node_t *n, nb_node_t *program) {
    if (!n) return;
    if (n->kind == N_CALL && n->text && !nb_es_funcion_propia(program, n->text)) {
        for (const nb_firma_t *f = nb_firmas; f->nombre[0]; f++)
            if (nb_text_eq_cstr(n->text, f->nombre)) { nb_comprobar_argumentos(n, f->nombre, f->firma); break; }
    }
    if (n->kind == N_TEXT) nb_comprobar_argumentos(n, "Text", "NNS");
    if (n->kind == N_CREATEWINDOW) nb_comprobar_argumentos(n, "CreateWindow", "SNNNN");
    nb_comprobar_llamadas(n->a, program); nb_comprobar_llamadas(n->b, program);
    nb_comprobar_llamadas(n->c, program); nb_comprobar_llamadas(n->d, program);
    for (int32_t i = 0; i < n->list_count; i++) nb_comprobar_llamadas(n->list[i], program);
}

// ---------------------------------------------------------------
// AVISOS: variables compartidas sin declarar
//
// En Nemo Basic, dentro de una funcion todo lo que no sea parametro ni
// Local es GLOBAL. Es la misma regla que Lua, y no va a cambiar: los
// programas que ya funcionan la usan, y cambiarla los romperia SIN dar
// ningun error -- compilarian igual y se portarian distinto.
//
// Lo que si se puede hacer es SEÑALARLO. Si una funcion usa una
// variable que tambien se usa fuera de ella, hay estado compartido
// entre la funcion y el resto del programa. Eso casi siempre es a
// proposito (un marcador, un contador, una constante del juego), pero
// tambien es como aparece el bug: dos sitios que creian tener cada uno
// su variable 'i' o su 'y' y en realidad comparten una.
//
// El aviso NO es un error: el programa se compila igual. Declarar la
// variable con Global (o Const) lo apaga, y de paso deja escrito en el
// programa que ese estado se comparte a proposito.
//
// Solo avisa de las COMPARTIDAS. Las variables que solo existen dentro
// de una funcion son globales de hecho, pero no las usa nadie mas, asi
// que no pueden chocar con nada y no vale la pena decir nada.
#define NB_MAX_AVISOS 64
#define NB_AVISO_NOM 40
typedef struct {
    char variable[NB_AVISO_NOM];
    char funcion[NB_AVISO_NOM];
    int32_t line;
} nb_aviso_t;
static nb_aviso_t g_avisos[NB_MAX_AVISOS];
static int32_t g_num_avisos = 0;
static bool g_avisos_desbordados = false;

static void nb_aviso_copiar(char *dst, nb_string_t *s) {
    uint32_t len = nb_string_len(s);
    const char *d = nb_string_cstr(s);
    if (len > NB_AVISO_NOM - 1) len = NB_AVISO_NOM - 1;
    for (uint32_t i = 0; i < len; i++) dst[i] = d[i];
    dst[len] = 0;
}

int32_t nb_codegen_num_avisos(void) { return g_num_avisos; }
bool nb_codegen_avisos_desbordados(void) { return g_avisos_desbordados; }

bool nb_codegen_aviso(int32_t i, const char **variable, const char **funcion, int32_t *line) {
    if (i < 0 || i >= g_num_avisos) return false;
    if (variable) *variable = g_avisos[i].variable;
    if (funcion) *funcion = g_avisos[i].funcion;
    if (line) *line = g_avisos[i].line;
    return true;
}

// ¿aparece 'name' como variable en este arbol? 'en_funciones' decide si
// se entra en los cuerpos de las funciones que haya dentro.
static bool nb_usa_var(nb_node_t *n, nb_string_t *name, bool en_funciones) {
    if (!n) return false;
    if (n->kind == N_FUNCDEF && !en_funciones) return false;
    if ((n->kind == N_VAR || n->kind == N_INDEX) && n->text && nb_string_eq(n->text, name)) return true;
    if (nb_usa_var(n->a, name, en_funciones)) return true;
    if (nb_usa_var(n->b, name, en_funciones)) return true;
    if (nb_usa_var(n->c, name, en_funciones)) return true;
    if (nb_usa_var(n->d, name, en_funciones)) return true;
    for (int32_t i = 0; i < n->list_count; i++)
        if (nb_usa_var(n->list[i], name, en_funciones)) return true;
    return false;
}

// ¿esta 'name' declarada con Global o con Const en cualquier parte del
// programa? Se busca en TODO el arbol y no solo al principio: un Global
// puede estar dentro de un If, y el de un archivo incluido tambien vale.
static bool nb_declarada(nb_node_t *n, nb_string_t *name) {
    if (!n) return false;
    if (n->kind == N_VARDECL && (n->op == TOK_KW_GLOBAL || n->op == TOK_KW_CONST)) {
        for (int32_t i = 0; i < n->list_count; i++) {
            nb_node_t *it = n->list[i];
            if (it && it->a && it->a->text && nb_string_eq(it->a->text, name)) return true;
        }
    }
    if (nb_declarada(n->a, name) || nb_declarada(n->b, name) ||
        nb_declarada(n->c, name) || nb_declarada(n->d, name)) return true;
    for (int32_t i = 0; i < n->list_count; i++)
        if (nb_declarada(n->list[i], name)) return true;
    return false;
}

static bool nb_es_suya(nb_node_t *f, nb_string_t *name);   // adelantada

// ¿esta 'name' declarada con Local dentro de este arbol?
static bool nb_declarada_local(nb_node_t *n, nb_string_t *name) {
    if (!n) return false;
    if (n->kind == N_VARDECL && n->op == TOK_KW_LOCAL) {
        for (int32_t i = 0; i < n->list_count; i++) {
            nb_node_t *it = n->list[i];
            if (it && it->a && it->a->text && nb_string_eq(it->a->text, name)) return true;
        }
    }
    if (nb_declarada_local(n->a, name) || nb_declarada_local(n->b, name) ||
        nb_declarada_local(n->c, name) || nb_declarada_local(n->d, name)) return true;
    for (int32_t i = 0; i < n->list_count; i++)
        if (nb_declarada_local(n->list[i], name)) return true;
    return false;
}

// ¿es 'name' un array declarado con Dim? Los arrays son otra cosa: su
// almacenamiento es global siempre y compartirlos es lo normal.
static bool nb_es_array(nb_node_t *n, nb_string_t *name) {
    if (!n) return false;
    if (n->kind == N_DIM && n->text && nb_string_eq(n->text, name)) return true;
    if (nb_es_array(n->a, name) || nb_es_array(n->b, name) ||
        nb_es_array(n->c, name) || nb_es_array(n->d, name)) return true;
    for (int32_t i = 0; i < n->list_count; i++)
        if (nb_es_array(n->list[i], name)) return true;
    return false;
}

// ¿usa 'name' alguien que NO sea la funcion 'f'? Se mira el cuerpo
// principal del programa y las DEMAS funciones, saltandose las que la
// tengan como parametro o Local suya -- esa es otra variable distinta.
static bool nb_usa_var_fuera_de(nb_node_t *program, nb_string_t *name, nb_node_t *f) {
    for (int32_t i = 0; i < program->list_count; i++) {
        nb_node_t *s = program->list[i];
        if (!s) continue;
        if (s->kind == N_FUNCDEF) {
            if (s == f) continue;
            if (nb_es_suya(s, name)) continue;
            if (nb_usa_var(s->d, name, true)) return true;
        } else {
            if (nb_usa_var(s, name, false)) return true;
        }
    }
    return false;
}

// ¿es 'name' parametro o Local de esta funcion?
static bool nb_es_suya(nb_node_t *f, nb_string_t *name) {
    for (int32_t i = 0; i < f->list_count; i++)
        if (f->list[i] && f->list[i]->text && nb_string_eq(f->list[i]->text, name)) return true;
    return nb_declarada_local(f->d, name);
}

static void nb_apuntar_aviso(nb_string_t *var, nb_string_t *fn, int32_t line) {
    for (int32_t i = 0; i < g_num_avisos; i++) {
        // una vez por variable y funcion: repetirlo en cada linea seria ruido
        if (g_avisos[i].line != -1 && nb_text_eq_cstr(var, g_avisos[i].variable) &&
            nb_text_eq_cstr(fn, g_avisos[i].funcion)) return;
    }
    if (g_num_avisos >= NB_MAX_AVISOS) { g_avisos_desbordados = true; return; }
    nb_aviso_copiar(g_avisos[g_num_avisos].variable, var);
    nb_aviso_copiar(g_avisos[g_num_avisos].funcion, fn);
    g_avisos[g_num_avisos].line = line;
    g_num_avisos++;
}

// Recorre el cuerpo de 'f' buscando variables compartidas con el exterior
static void nb_revisar_funcion(nb_node_t *n, nb_node_t *f, nb_node_t *program) {
    if (!n) return;
    if ((n->kind == N_VAR || n->kind == N_INDEX) && n->text) {
        nb_string_t *name = n->text;
        if (!nb_es_suya(f, name) && !nb_declarada(program, name) && !nb_es_array(program, name)) {
            bool compartida = nb_usa_var_fuera_de(program, name, f);
            if (compartida) nb_apuntar_aviso(name, f->text, n->line);
        }
    }
    nb_revisar_funcion(n->a, f, program); nb_revisar_funcion(n->b, f, program);
    nb_revisar_funcion(n->c, f, program); nb_revisar_funcion(n->d, f, program);
    for (int32_t i = 0; i < n->list_count; i++) nb_revisar_funcion(n->list[i], f, program);
}

static void nb_avisar_globales(nb_node_t *program) {
    g_num_avisos = 0;
    g_avisos_desbordados = false;
    if (!program) return;
    for (int32_t i = 0; i < program->list_count; i++) {
        nb_node_t *f = program->list[i];
        if (f && f->kind == N_FUNCDEF && f->text) nb_revisar_funcion(f->d, f, program);
    }
}

void nb_emit_program(nb_codegen_ctx_t *ctx, nb_node_t *program) {
    // Cada compilacion empieza sin errores pendientes de la anterior
    g_codegen_error = false;
    g_codegen_error_msg = NULL;
    g_codegen_error_line = 0;
    g_codegen_bss_size = 0;
    nb_traducir_alias(program, program);       // los nombres alternativos, ANTES que nada
    nb_comprobar_llamadas(program, program);   // tipos de los argumentos de las incorporadas
    nb_avisar_globales(program);               // variables compartidas sin declarar

    nb_func_table_t ft; nb_func_table_init(&ft);
    ctx->functions = &ft;
    nb_array_set_t arr; nb_array_set_init(&arr);
    ctx->arrays = &arr;
    // Los parametros declarados "nombre()" son arrays, y hay que saberlo
    // ANTES de generar nada: si se apuntaran al llegar a cada Function, una
    // llamada escrita mas arriba en el programa todavia no sabria que ese
    // parametro es un array y le pasaria el valor de una variable en vez del
    // bloque. Aqui se recorren todas las funciones primero, asi que el orden
    // en que esten escritas dentro del archivo da igual.
    for (int32_t i = 0; i < program->list_count; i++) {
        nb_node_t *f = program->list[i];
        if (f->kind != N_FUNCDEF) continue;
        for (int32_t k = 0; k < f->list_count; k++)
            if (f->list[k]->op == NB_PARAM_ARRAY && f->list[k]->text)
                nb_array_set_add(&arr, f->list[k]->text);
    }
    nb_label_table_t lbl; nb_label_table_init(&lbl);
    nb_scan_labels(program, &lbl); // etiquetas de NIVEL SUPERIOR (no entra en las funciones)
    ctx->labels = &lbl;

    // Rutina de "indice fuera de rango": solo se crea si el programa
    // declara algun array. Un programa sin Dim no puede salirse de
    // ninguno, asi que no tiene por que cargar con el mensaje ni con
    // las instrucciones de la rutina.
    // En TODO el arbol: antes solo se miraba el nivel superior, y
    // un Dim dentro de un If, un bucle o una FUNCION no contaba -- esos arrays
    // no tenian ninguna comprobacion, ni siquiera la de indice.
    bool usa_arrays = nb_arbol_tiene_dim(program);
    nb_label_t *bounds_err = usa_arrays ? nb_label_new() : NULL;
    ctx->bounds_error_label = bounds_err;
    // errores en tiempo de ejecucion: la tabla de lineas y las rutinas
    g_lin_n = 0;
    g_usa_indice = g_usa_div0 = g_usa_null = g_usa_dim = false;
    g_err_div0 = nb_label_new(); g_err_null = nb_label_new(); g_err_dim = nb_label_new();
    g_h_copia = nb_label_new(); g_h_digitos = nb_label_new();
    g_usa_copia = g_usa_digitos = false;
    g_t_apuntar = nb_label_new(); g_t_liberar = nb_label_new(); g_t_marca = nb_label_new();
    g_usa_temporales = false;
    g_temp_area = 0;   // el hueco de la lista se reserva solo si el programa la usa

    // Data/Read/Restore: barrido de TODO el programa (a diferencia
    // del de etiquetas, este SI entra en las funciones -- Data es
    // compartido por todo el programa). Si no aparece ningun valor de
    // Data en ningun sitio, next_offset no cambia -- en ese caso no
    // hace falta ni la variable de cursor ni su inicializacion.
    nb_restore_table_t rst; nb_restore_table_init(&rst);
    uint32_t offset_antes = ctx->globals->next_offset;
    uint32_t patches_antes = ctx->globals->data_patch_count;
    nb_data_cadenas_t cad = { NULL, NULL, 0, 0 };
    nb_scan_data(ctx->globals, program, &rst, &cad);
    // las letras de las cadenas, todas detras de la tabla de entradas
    for (uint32_t k = 0; k < cad.n; k++) {
        nb_node_t *val = cad.nodos[k];
        uint32_t content_off = nb_symtab_add_literal_bytes(ctx->globals,
            (const uint8_t *)nb_string_cstr(val->text), nb_string_len(val->text));
        nb_symtab_add_data_patch(ctx->globals, cad.slots[k], content_off);
    }
    if (cad.nodos) nb_free(cad.nodos);
    if (cad.slots) nb_free(cad.slots);
    bool needs_data = ctx->globals->next_offset != offset_antes;
    ctx->data_start_offset = offset_antes;
    bool needs_data_strings = ctx->globals->data_patch_count != patches_antes; // Data con al menos una cadena -- necesita nb_string_new
    ctx->restore = &rst;
    nb_string_t *cursor_name = NULL;
    if (needs_data) {
        cursor_name = nb_string_new("__data_cursor", 13);
        ctx->data_cursor = cursor_name;
    }

    // Color activo: variable global oculta, igual que el cursor de
    // Data. Se crea SIEMPRE (cuesta 8 bytes) para no tener que
    // decidir de antemano si el programa usa graficos -- si no los
    // usa, simplemente nadie la lee.
    nb_string_t *color_name = nb_tree_uses_graphics(program) ? nb_string_new("__color", 7) : NULL;
    ctx->color_var = color_name;
    nb_string_t *cls_color_name = nb_tree_uses_graphics(program) ? nb_string_new("__clscolor", 10) : NULL;
    ctx->cls_color_var = cls_color_name;   // el color de ClsColor (0 = negro, como siempre)

    // Type/New/Delete/campos: barrido de nivel superior (Type se
    // declara igual que Function, no dentro de otro bloque).
    nb_field_table_t flds; nb_field_table_init(&flds);
    nb_type_table_t typs; nb_type_table_init(&typs);
    nb_scan_types(program, ctx->globals, &flds, &typs);
    ctx->fields = &flds;
    ctx->types = &typs;

    // Primer barrido: registrar cada funcion de nivel superior con su
    // propia etiqueta, ANTES de generar ninguna instruccion -- para
    // que cualquier llamada (incluida una funcion llamandose a si
    // misma, o dos funciones llamandose mutuamente) resuelva bien sin
    // importar el orden de aparicion en el fuente.
    for (int32_t i = 0; i < program->list_count; i++) {
        nb_node_t *s = program->list[i];
        if (s->kind == N_FUNCDEF) {
            int32_t pc = s->list_count > 8 ? 8 : s->list_count;
            nb_func_entry_t *nfe = nb_func_table_add(&ft, s->text, pc);
            if (nfe) nfe->def = s;
        }
    }

    // Tabla de funciones del bloque de runtime -- SOLO si el programa
    // usa cadenas en algun sitio (barrido previo, igual en espiritu al
    // de las funciones de usuario). Si no las usa, ni se crea la tabla
    // ni se incrusta el bloque -- un programa sin cadenas no paga
    // ningun coste por la existencia de esta pieza.
    bool needs_runtime = nb_tree_needs_runtime(program) || needs_data_strings;
    if (needs_runtime) g_codegen_bss_size = NB_RUNTIME_BLOB_BSS_SIZE;
    nb_runtime_table_t rt;
    if (needs_runtime) {
        for (uint32_t i = 0; i < NB_RUNTIME_SYMS_COUNT; i++) {
            rt.entries[i].sym_index = i;
            rt.entries[i].label = nb_label_new();
        }
        ctx->runtime = &rt;
    }

    // Secuencia principal: todo lo que NO sea una definicion de
    // funcion, en su orden original -- las funciones en si se generan
    // aparte, despues, para que cualquiera pueda llamar a cualquiera.
    // El salto de aqui solo hace falta si hay funciones de usuario O
    // si se va a incrustar el bloque de runtime -- si no hay ninguna
    // de las dos cosas, la secuencia principal es todo el programa, y
    // no hay nada detras de lo que protegerse.
    // Guardar x30 ANTES de generar nada mas -- task_trampoline deja
    // ahi la direccion de retorno real (el stub de salida de la
    // tarea), y CUALQUIER llamada a funcion de usuario (bl) en la
    // secuencia principal la pisa. Sin esto, el 'ret' final saltaria
    // a "donde volvio la ultima llamada" en vez de al stub -- un
    // salto hacia DENTRO del propio programa, bucle infinito, con un
    // planificador cooperativo eso cuelga el sistema ENTERO (la
    // tarea nunca cede CPU). Encontrado en la primera ejecucion real
    // de un .pro de esta reescritura. Patron EXACTO del nbc_main.c
    // viejo (su propio _start hace lo mismo, con el mismo comentario
    // palabra por palabra) -- stp x29,x30 en PAR, con x29 apuntando
    // de verdad al marco, no solo x30 suelto.
    nb_codebuf_emit(ctx->cb, nb_enc_stp_pre(29, 30, NB_REG_SP, -2));
    nb_codebuf_emit(ctx->cb, nb_enc_add_imm(29, NB_REG_SP, 0)); // mov x29, sp
    // Color activo por defecto: blanco, como en BlitzPlus.
    if (color_name) {
        nb_emit_load_imm64(ctx->cb, 0, 0xFFFFFFu);
        nb_emit_store_var_from_x0(ctx, color_name);
    }
    if (needs_data) {
        // El cursor arranca en el desplazamiento del PRIMER valor de
        // Data de todo el programa -- exactamente 'offset_antes',
        // capturado antes de que el barrido de Data añadiera nada a
        // la region compartida.
        nb_emit_load_imm64(ctx->cb, 0, offset_antes);
        nb_emit_store_var_from_x0(ctx, cursor_name);
    }
    for (int32_t i = 0; i < program->list_count; i++) {
        if (program->list[i]->kind != N_FUNCDEF) nb_emit_stmt(ctx, program->list[i]);
    }
    // Epilogo AQUI MISMO, justo donde termina la secuencia principal
    // -- no al final de todo el binario. Antes iba despues de las
    // funciones Y del bloque de runtime Y de sus ~2MB de .bss, con un
    // salto para llegar hasta alli: las dos ultimas instrucciones del
    // programa quedaban a 2MB de distancia del resto del codigo, al
    // otro lado del monton entero. No hay ninguna razon para eso -- a
    // las funciones solo se llega con 'bl', nunca cayendo en ellas
    // desde arriba, asi que la secuencia principal puede terminar
    // aqui sin saltarse nada.
    nb_codebuf_emit(ctx->cb, nb_enc_ldp_post(29, 30, NB_REG_SP, 2));
    nb_codebuf_emit(ctx->cb, nb_enc_ret());

    // Rutina de "indice fuera de rango", UNA para todo el programa.
    //
    // Va aqui, detras del epilogo: no se llega a ella cayendo desde
    // arriba, solo saltando, asi que no estorba a la secuencia normal.
    //
    // Escribe el mensaje y termina la tarea. Se prefiere parar a
    // seguir: un acceso fuera de rango escribe encima de otras
    // variables y el programa falla DESPUES, en otro sitio, con
    // sintomas que no tienen nada que ver. Es mucho mejor decir que
    // paso y donde.
    // (la rutina de "indice fuera de rango" va ahora con las demas rutinas
    // de error, despues de las funciones: ver nb_emit_rutinas_error)

    int32_t fi = 0;
    for (int32_t i = 0; i < program->list_count; i++) {
        nb_node_t *s = program->list[i];
        if (s->kind == N_FUNCDEF) {
            nb_emit_funcdef(ctx, &ft.entries[fi], s);
            fi++;
        }
    }
    // Las rutinas de error, con la tabla de lineas ya completa (todas las
    // sentencias, las de las funciones incluidas, ya estan generadas)
    nb_emit_rutinas_error(ctx);
    // Las etiquetas se liberan DESPUES de generar las funciones: dentro
    // de una funcion tambien puede haber accesos a arrays, y cada uno
    // añade un parche pendiente a esta etiqueta. Liberarla antes seria
    // escribir en memoria ya devuelta.
    if (bounds_err) nb_label_free(bounds_err);
    ctx->bounds_error_label = NULL;
    nb_label_free(g_err_div0); nb_label_free(g_err_null); nb_label_free(g_err_dim);
    nb_label_free(g_h_copia); nb_label_free(g_h_digitos);
    nb_label_free(g_t_apuntar); nb_label_free(g_t_liberar); nb_label_free(g_t_marca);
    g_t_apuntar = g_t_liberar = g_t_marca = NULL;
    g_err_div0 = g_err_null = g_err_dim = NULL; g_h_copia = g_h_digitos = NULL;

    // Bloque de runtime en si: alineado a 4096, porque sus referencias
    // internas vienen ya resueltas de nb_elf_extract.py suponiendo eso
    // (adrp trabaja con paginas de 4 KB -- ver la nota de
    // nb_codebuf_align). Ahora que se sabe donde cae, se definen las
    // etiquetas creadas al principio -- esto corrige de golpe TODAS
    // las llamadas que ya se generaron mas arriba.
    // La region de datos va AQUI: justo detras del codigo y de las
    // funciones, pero ANTES del bloque de runtime. Motivo: el .bss del
    // bloque (el monton de nb_alloc, megabytes de ceros) queda asi lo
    // ULTIMO de todo, y no hace falta escribirlo en el .pro -- el area
    // de cada tarea ya viene entera a cero desde task_spawn_from_file.
    // Antes los datos iban al final del todo, detras de esos megas de
    // ceros, lo que hacia cada .pro absurdamente grande (y en la
    // practica lo dejaba truncado al copiarlo con el explorador, que
    // corta a 262 KB en silencio). Aqui solo se RESERVA el hueco; el
    // contenido real lo rellena el ensamblador final, una vez
    // resueltas todas las direcciones.
    ctx->globals->data_region_pos = nb_codebuf_here(ctx->cb) * 4;
    for (uint32_t i = 0; i < (ctx->globals->next_offset + 3) / 4; i++) nb_codebuf_emit(ctx->cb, 0);

    if (needs_runtime) {
        nb_codebuf_align(ctx->cb, 4096);
        uint32_t blob_base_words = nb_codebuf_here(ctx->cb);
        nb_codebuf_emit_bytes(ctx->cb, NB_RUNTIME_BLOB_TEXT, NB_RUNTIME_BLOB_TEXT_SIZE);
        // El codigo del bloque (ya incrustado arriba) espera
        // encontrar su .bss privado (NB_RUNTIME_BLOB_BSS_SIZE, ~8MB
        // -- el monton de nb_alloc.c) justo despues, a partir de
        // aqui. Decision tomada: se escriben los ceros DE VERDAD en
        // el binario final (no se asume que el cargador de Nemo OS
        // soporte "esta memoria de mas no necesita espacio en el
        // archivo", como el p_memsz > p_filesz de un ELF real, sin
        // haberlo confirmado) -- cada .pro que use cadenas crece unos
        // 8MB por esto. Es un coste de espacio en disco conocido y
        // aceptado por ahora, no un problema de correccion; revisar
        // si el cargador real ofrece algo mejor es una optimizacion
        // futura, no un bloqueante.
        // El .bss del bloque (el monton) NO se escribe en el archivo:
        // vive justo detras de este punto, en memoria que el kernel ya
        // dejo a cero al crear la tarea. El .pro termina aqui.
        // (NB_RUNTIME_BLOB_BSS_SIZE sigue siendo su tamaño real, solo
        // que ahora es espacio reservado, no bytes guardados.)
        for (uint32_t i = 0; i < NB_RUNTIME_SYMS_COUNT; i++) {
            nb_label_define_at(ctx->cb, rt.entries[i].label, blob_base_words + NB_RUNTIME_SYMS[i].offset / 4);
        }
    }

    if (needs_runtime) {
        for (uint32_t i = 0; i < NB_RUNTIME_SYMS_COUNT; i++) nb_label_free(rt.entries[i].label);
        ctx->runtime = NULL;
    }

    nb_array_set_free(&arr);
    ctx->arrays = NULL;
    nb_label_table_free(&lbl);
    ctx->labels = NULL;
    nb_restore_table_free(&rst);
    ctx->restore = NULL;
    if (cursor_name) { nb_string_release(cursor_name); ctx->data_cursor = NULL; }
    if (color_name) { nb_string_release(color_name); ctx->color_var = NULL; }
    if (cls_color_name) { nb_string_release(cls_color_name); ctx->cls_color_var = NULL; }
    nb_field_table_free(&flds);
    ctx->fields = NULL;
    nb_type_table_free(&typs);
    ctx->types = NULL;
    nb_func_table_free(&ft);
    ctx->functions = NULL;
}


// ¿El programa se declaro como aplicacion de CONSOLA?
//
// Busca un "Console" suelto en el nivel superior del programa. No
// entra en funciones ni en bucles a proposito: es una declaracion
// sobre el programa entero, y tiene que poder leerse de un vistazo al
// principio del archivo.
//
// Por defecto (sin declaracion) un programa es de ESCRITORIO, que es
// como se comportaban todos hasta ahora: asi los .pro ya compilados
// siguen abriendose igual.
bool nb_tree_is_console_app(nb_node_t *program) {
    if (!program) return false;
    for (int32_t i = 0; i < program->list_count; i++) {
        if (program->list[i] && program->list[i]->kind == N_CONSOLEAPP) return true;
    }
    return false;
}
