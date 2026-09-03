// codegen.c — compilador Nemo-Blitz
//
// Recorre el AST y emite ensamblador AArch64 en TEXTO (compatible con
// aarch64-elf-as). Reglas fijas para todo el codigo que se genera
// aqui:
//
//   1. Nada de coma flotante -- todo son enteros de 64 bits.
//   2. Cualquier acceso a datos globales o a literales de cadena usa
//      SIEMPRE adrp+add (relativo al PC) -- nunca una tabla de
//      direcciones absolutas. Es la misma regla que descubrimos con
//      el bug de las tablas de punteros al escribir el editor.
//   3. Las expresiones se evaluan con una maquina de pila simple: cada
//      subexpresion deja su resultado en x0; los operandos temporales
//      se empujan/sacan de la pila del programa. Mas lento que asignar
//      registros de verdad, pero MUCHISIMO mas facil de generar bien
//      a la primera -- y para un compilador que aun no tiene banco de
//      pruebas automatizado, la correccion pesa mas que la velocidad.

#include "codegen.h"
#include "lexer.h"
#include "nblibc.h"
#include "nb_output.h"
#include "nb_runtime.h"
#include <stdbool.h>

// -- numeros de syscall de Nemo OS que usa el codigo generado --
#define SYS_WRITE_STRING 11
#define SYS_DRAW_RECT    30
#define SYS_DRAW_TEXT    31
#define SYS_GET_MOUSE    34

typedef enum { TY_INT, TY_STRING, TY_FLOAT } ValType;

// ---- tabla de simbolos locales (una por funcion que se este generando) ----
#define MAX_LOCALS 64
typedef struct {
    char name[64];
    int offset; // offset en bytes desde x29 (negativo, hacia abajo en la pila)
} Local;

typedef struct {
    Local locals[MAX_LOCALS];
    int count;
    int frame_size; // multiplo de 16, incluye sitio para todos los locales
} FuncScope;

// ---- variables globales detectadas en el programa ----
#define MAX_GLOBALS 128
static char globals[MAX_GLOBALS][64];
static int global_count = 0;

// ---- funciones declaradas (para saber si "nombre(...)" es llamada a
// funcion o indexado de array al generar codigo) ----
#define MAX_FUNCS 64
static char func_names[MAX_FUNCS][64];
static Node *func_defs[MAX_FUNCS];
static int func_count = 0;

// ---- arrays declarados con Dim ----
#define MAX_ARRAYS 32
#define MAX_ARRAY_DIMS 4
typedef struct {
    char name[64];
    int dim_sizes[MAX_ARRAY_DIMS];
    int dim_count;
    int total_size;
} ArrayInfo;
static ArrayInfo arrays[MAX_ARRAYS];
static int array_count = 0;

// ---- Data / Read / Restore ----
#define MAX_DATA_ENTRIES 1024
typedef struct {
    bool is_label;
    bool is_string;
    char label_name[64];
    int64_t num_value;
    char str_label[32];
} DataEntry;
static DataEntry data_entries[MAX_DATA_ENTRIES];
static int data_entry_count = 0;

// ---- Type / Field ----
#define MAX_TYPES 16
#define MAX_FIELDS_PER_TYPE 32
#define MAX_TYPE_INSTANCES 64
typedef struct {
    char name[64];
    char field_names[MAX_FIELDS_PER_TYPE][64];
    int field_count;
} TypeInfo;
static TypeInfo types[MAX_TYPES];
static int type_count = 0;

#define MAX_VAR_TYPES 64
typedef struct { char var_name[64]; char type_name[64]; } VarTypeAssoc;
static VarTypeAssoc var_types[MAX_VAR_TYPES];
static int var_type_count = 0;

static NBOut *out;
static int label_counter = 0;

// literales de cadena recogidos durante la generacion, volcados al
// final en .rodata
#define MAX_STRINGS 256
typedef struct { char label[32]; char value[256]; } StringLit;
static StringLit strings[MAX_STRINGS];
static int string_count = 0;

#define MAX_FLOAT_CONSTS 128
typedef struct { char label[32]; double value; } FloatConst;
static FloatConst float_consts[MAX_FLOAT_CONSTS];
static int float_const_count = 0;

static int new_label(void) { return label_counter++; }

// -- pila de "fin del bucle actual", para Exit --
#define MAX_LOOP_DEPTH 16
typedef struct { int label; const char *prefix; } LoopFrame;
static LoopFrame loop_stack[MAX_LOOP_DEPTH];
static int loop_depth = 0;

static void push_loop(int label, const char *prefix) {
    if (loop_depth < MAX_LOOP_DEPTH) { loop_stack[loop_depth].label = label; loop_stack[loop_depth].prefix = prefix; loop_depth++; }
}
static void pop_loop(void) { if (loop_depth > 0) loop_depth--; }

// Escribe un literal de cadena para .asciz, escapando los caracteres
// que el ensamblador necesita como secuencia de dos caracteres (\n,
// \t, \", \\) -- si no, un salto de linea REAL dentro de la cadena
// (como el que usamos para el salto de linea automatico de Print)
// rompe la sintaxis del ensamblador a mitad de la cadena.
static void emit_asciz(NBOut *f, const char *label, const char *value) {
    nb_fprintf(f, "%s: .asciz \"", label);
    char one_char[2];
    one_char[1] = '\0';
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        switch (*p) {
            case '\n': nb_fprintf(f, "\\n"); break;
            case '\t': nb_fprintf(f, "\\t"); break;
            case '"':  nb_fprintf(f, "\\\""); break;
            case '\\': nb_fprintf(f, "\\\\"); break;
            default:
                one_char[0] = (char)*p;
                nb_fprintf(f, "%s", one_char);
                break;
        }
    }
    nb_fprintf(f, "\"\n");
}

static const char *intern_string(const char *value) {
    for (int i = 0; i < string_count; i++) {
        if (nb_strcmp(strings[i].value, value) == 0) return strings[i].label;
    }
    {
        // "str_%d" -- sin snprintf, lo construimos a mano
        char *lbl = strings[string_count].label;
        lbl[0] = 's'; lbl[1] = 't'; lbl[2] = 'r'; lbl[3] = '_';
        char num_buf[12];
        nb_itoa(string_count, num_buf, sizeof(num_buf));
        nb_strncpy(lbl + 4, num_buf, sizeof(strings[0].label) - 4);
    }
    nb_strncpy(strings[string_count].value, value, sizeof(strings[0].value) - 1);
    const char *lbl = strings[string_count].label;
    string_count++;
    return lbl;
}

static const char *intern_float(double value) {
    for (int i = 0; i < float_const_count; i++) {
        if (float_consts[i].value == value) return float_consts[i].label;
    }
    {
        char *lbl = float_consts[float_const_count].label;
        lbl[0] = 'f'; lbl[1] = 'l'; lbl[2] = 't'; lbl[3] = '_';
        char num_buf[12];
        nb_itoa(float_const_count, num_buf, sizeof(num_buf));
        nb_strncpy(lbl + 4, num_buf, sizeof(float_consts[0].label) - 4);
    }
    float_consts[float_const_count].value = value;
    const char *lbl = float_consts[float_const_count].label;
    float_const_count++;
    return lbl;
}

// Comparacion de identificadores SIN distinguir mayusculas/minusculas
// -- BlitzPlus real es insensible a mayusculas en TODOS los
// identificadores (variables, tipos, campos, funciones). BUG REAL
// CORREGIDO: antes solo se aplicaba a nombres de Tipo -- ver la nota
// grande equivalente en el compilador del host.
static int strcasecmp_ascii(const char *a, const char *b) {
    while (*a && *b) {
        char ca = nb_toupper(*a);
        char cb = nb_toupper(*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return *a - *b;
}

static bool is_global(const char *name) {
    for (int i = 0; i < global_count; i++) if (strcasecmp_ascii(globals[i], name) == 0) return true;
    return false;
}
static void add_global_if_new(const char *name) {
    if (is_global(name)) return;
    if (global_count < MAX_GLOBALS) nb_strncpy(globals[global_count++], name, 63);
}
static bool is_function(const char *name) {
    for (int i = 0; i < func_count; i++) if (strcasecmp_ascii(func_names[i], name) == 0) return true;
    return false;
}
static Node *find_funcdef(const char *name) {
    for (int i = 0; i < func_count; i++) if (strcasecmp_ascii(func_names[i], name) == 0) return func_defs[i];
    return NULL;
}
static ArrayInfo *find_array(const char *name) {
    for (int i = 0; i < array_count; i++) if (strcasecmp_ascii(arrays[i].name, name) == 0) return &arrays[i];
    return NULL;
}

static bool is_string_name(const char *name) {
    int len = (int)nb_strlen(name);
    return len > 0 && name[len - 1] == '$';
}

static bool is_float_name(const char *name) {
    int len = (int)nb_strlen(name);
    return len > 0 && name[len - 1] == '#';
}
static bool is_float_literal(Node *n) {
    return n->kind == N_NUM && n->text[0] == '#';
}

// ---- recoleccion previa: que funciones, arrays y globales existen ----

static void collect_toplevel_names(Node *block) {
    for (int i = 0; i < block->list_count; i++) {
        Node *s = block->list[i];
        if (s->kind == N_FUNCDEF) {
            if (func_count < MAX_FUNCS) { nb_strncpy(func_names[func_count], s->text, 63); func_defs[func_count] = s; func_count++; }
        } else if (s->kind == N_BLOCK) {
            // "Dim a(N),b(N),c(N)" se envuelve en un N_BLOCK -- ver la
            // nota equivalente en el compilador del host.
            collect_toplevel_names(s);
        } else if (s->kind == N_DIM) {
            if (array_count < MAX_ARRAYS) {
                nb_strncpy(arrays[array_count].name, s->text, 63);
                int dc = s->list_count;
                if (dc > MAX_ARRAY_DIMS) dc = MAX_ARRAY_DIMS;
                int total = 1;
                for (int d = 0; d < dc; d++) {
                    int sz = (int)s->list[d]->num_value + 1;
                    if (sz < 1) sz = 1;
                    arrays[array_count].dim_sizes[d] = sz;
                    total *= sz;
                }
                arrays[array_count].dim_count = dc;
                arrays[array_count].total_size = total;
                array_count++;
            }
        }
    }
}

// Recoge, EN ORDEN, todos los "Data" y etiquetas de nivel superior.
static void collect_data(Node *block) {
    for (int i = 0; i < block->list_count; i++) {
        Node *s = block->list[i];
        if (s->kind == N_DATALABEL) {
            if (data_entry_count < MAX_DATA_ENTRIES) {
                data_entries[data_entry_count].is_label = true;
                nb_strncpy(data_entries[data_entry_count].label_name, s->text, 63);
                data_entry_count++;
            }
        } else if (s->kind == N_DATA) {
            for (int j = 0; j < s->list_count && data_entry_count < MAX_DATA_ENTRIES; j++) {
                Node *v = s->list[j];
                DataEntry *e = &data_entries[data_entry_count];
                e->is_label = false;
                if (v->kind == N_UNOP && v->op == T_MINUS) {
                    e->is_string = false;
                    e->num_value = -v->a->num_value;
                } else if (v->kind == N_STR) {
                    e->is_string = true;
                    nb_strncpy(e->str_label, intern_string(v->text), sizeof(e->str_label) - 1);
                } else {
                    e->is_string = false;
                    e->num_value = v->num_value;
                }
                data_entry_count++;
            }
        }
    }
}

// ---- Type / Field: registro y recoleccion previa ----

static TypeInfo *find_type(const char *name) {
    for (int i = 0; i < type_count; i++) if (strcasecmp_ascii(types[i].name, name) == 0) return &types[i];
    return NULL;
}
// Devuelve el nombre CANONICO del tipo -- ver la nota equivalente en
// el compilador del host.
static const char *canon_type_name(const char *raw) {
    TypeInfo *t = find_type(raw);
    return t ? t->name : raw;
}
static const char *find_var_type(const char *varname) {
    for (int i = 0; i < var_type_count; i++) if (strcasecmp_ascii(var_types[i].var_name, varname) == 0) return var_types[i].type_name;
    return NULL;
}
static void add_var_type(const char *varname, const char *tname) {
    if (find_var_type(varname)) return;
    if (var_type_count < MAX_VAR_TYPES) {
        nb_strncpy(var_types[var_type_count].var_name, varname, 63);
        nb_strncpy(var_types[var_type_count].type_name, tname, 63);
        var_type_count++;
    }
}
// Intenta averiguar de que Tipo es una expresion que representa una
// instancia -- ver la nota equivalente en el compilador del host.
static const char *infer_type_name_from_expr(Node *expr) {
    if (!expr) return NULL;
    if (expr->kind == N_VAR) return find_var_type(expr->text);
    if (expr->kind == N_FIRSTLAST) return expr->text;
    if (expr->kind == N_BEFORE || expr->kind == N_AFTER) return infer_type_name_from_expr(expr->a);
    return NULL;
}
static int find_field_offset(const char *tname, const char *fieldname) {
    TypeInfo *t = find_type(tname);
    if (!t) return -1;
    for (int i = 0; i < t->field_count; i++) {
        if (strcasecmp_ascii(t->field_names[i], fieldname) == 0) return i;
    }
    return -1;
}

static void collect_type_info(Node *n) {
    if (!n) return;
    if (n->kind == N_TYPEDEF) {
        if (type_count < MAX_TYPES) {
            nb_strncpy(types[type_count].name, n->text, 63);
            types[type_count].field_count = 0;
            for (int i = 0; i < n->list_count && i < MAX_FIELDS_PER_TYPE; i++) {
                nb_strncpy(types[type_count].field_names[i], n->list[i]->text, 63);
                types[type_count].field_count++;
            }
            type_count++;
        }
        return;
    }
    if (n->kind == N_ASSIGN && n->a->kind == N_VAR && n->b && n->b->kind == N_NEW) {
        add_var_type(n->a->text, n->b->text);
    }
    if (n->kind == N_ASSIGN && n->a->kind == N_VAR && n->b &&
        (n->b->kind == N_BEFORE || n->b->kind == N_AFTER)) {
        const char *tname = infer_type_name_from_expr(n->b->a);
        if (tname) add_var_type(n->a->text, tname);
    }
    if (n->kind == N_FOREACH) {
        add_var_type(n->text, n->a->text);
    }
    collect_type_info(n->a); collect_type_info(n->b); collect_type_info(n->c); collect_type_info(n->d);
    for (int i = 0; i < n->list_count; i++) collect_type_info(n->list[i]);
}

// Recorre una expresion/sentencia de nivel superior anotando que
// nombres de variable aparecen, para reservarles espacio como
// globales.
static void mark_globals_expr(Node *n) {
    if (!n) return;
    if (n->kind == N_VAR) add_global_if_new(n->text);
    if (n->kind == N_INDEX) { /* el propio array ya se registro via Dim */ }
    mark_globals_expr(n->a); mark_globals_expr(n->b); mark_globals_expr(n->c); mark_globals_expr(n->d);
    for (int i = 0; i < n->list_count; i++) mark_globals_expr(n->list[i]);
}

static void mark_globals_stmt(Node *n) {
    if (!n) return;
    if (n->kind == N_FUNCDEF) return; // el interior de una funcion nunca crea globales (ver diseño)
    if (n->kind == N_FOR) add_global_if_new(n->text); // la variable del bucle es solo texto, no un N_VAR
    if (n->kind == N_VARDECL) {
        for (int i = 0; i < n->list_count; i++) {
            add_global_if_new(n->list[i]->a->text);
            mark_globals_expr(n->list[i]->b);
        }
        return;
    }
    mark_globals_expr(n->a); mark_globals_expr(n->b); mark_globals_expr(n->c); mark_globals_expr(n->d);
    for (int i = 0; i < n->list_count; i++) mark_globals_stmt(n->list[i]);
}

// ---- localizacion de variables locales dentro de una funcion ----

static void local_add_if_new(FuncScope *sc, const char *name) {
    for (int i = 0; i < sc->count; i++) if (strcasecmp_ascii(sc->locals[i].name, name) == 0) return;
    if (sc->count >= MAX_LOCALS) { nb_fatal(0, "demasiadas variables locales en una funcion", NULL); }
    nb_strncpy(sc->locals[sc->count].name, name, 63);
    sc->locals[sc->count].offset = -(sc->count + 1) * 8;
    sc->count++;
}

static void collect_locals_expr(FuncScope *sc, Node *n) {
    if (!n) return;
    if (n->kind == N_VAR) local_add_if_new(sc, n->text);
    collect_locals_expr(sc, n->a); collect_locals_expr(sc, n->b);
    collect_locals_expr(sc, n->c); collect_locals_expr(sc, n->d);
    for (int i = 0; i < n->list_count; i++) collect_locals_expr(sc, n->list[i]);
}
static void collect_locals_stmt(FuncScope *sc, Node *n) {
    if (!n) return;
    if (n->kind == N_FOR) local_add_if_new(sc, n->text);
    collect_locals_expr(sc, n->a); collect_locals_expr(sc, n->b);
    collect_locals_expr(sc, n->c); collect_locals_expr(sc, n->d);
    for (int i = 0; i < n->list_count; i++) collect_locals_stmt(sc, n->list[i]);
}

static Local *find_local(FuncScope *sc, const char *name) {
    for (int i = 0; i < sc->count; i++) if (strcasecmp_ascii(sc->locals[i].name, name) == 0) return &sc->locals[i];
    return NULL;
}

// ---- emision de codigo ----

static bool g_in_user_function = false;
// Nombre de la funcion en generacion (ver la misma nota en el
// codegen.c del host) -- corrige el bug real de ".Lfunc_end" con
// nombre fijo, que colisionaba entre funciones distintas.
static const char *g_current_func_name = NULL;

static ValType emit_expr(FuncScope *sc, Node *n);
static void emit_stmt(FuncScope *sc, Node *n);
static void emit_block(FuncScope *sc, Node *block);

static void push_x0(void) { nb_fprintf(out, "    str x0, [sp, #-16]!\n"); }
static void pop_to_x1(void) { nb_fprintf(out, "    ldr x1, [sp], #16\n"); }

// Carga la direccion de una variable (global o local) en x0. Los
// arrays no pasan por aqui -- ver emit_index.
// Ver la nota equivalente en el compilador del host: '#' no es valido
// dentro de un simbolo de ensamblador (es el prefijo de los
// inmediatos en la sintaxis ARM), asi que lo sustituimos SOLO para el
// nombre que aparece en el .s generado.
static void sanitize_sym(const char *name, char *out, uint32_t out_size) {
    uint32_t i = 0, j = 0;
    while (name[i] != '\0' && j + 1 < out_size) {
        if (name[i] == '#') {
            const char *suf = "_FLT";
            for (int k = 0; suf[k] && j + 1 < out_size; k++) out[j++] = suf[k];
        } else if (name[i] >= 'A' && name[i] <= 'Z') {
            out[j++] = (char)(name[i] + 32);
        } else {
            out[j++] = name[i];
        }
        i++;
    }
    out[j] = '\0';
}

static void emit_var_address(FuncScope *sc, const char *name) {
    Local *loc = sc ? find_local(sc, name) : NULL;
    if (loc) {
        nb_fprintf(out, "    add x0, x29, #%d\n", loc->offset);
    } else {
        char sym[80];
        sanitize_sym(name, sym, sizeof(sym));
        nb_fprintf(out, "    adrp x0, var_%s\n", sym);
        nb_fprintf(out, "    add x0, x0, :lo12:var_%s\n", sym);
    }
}

static bool is_cmp_op(int op) {
    return op == T_EQ || op == T_NE || op == T_LT || op == T_GT || op == T_LE || op == T_GE;
}
static bool is_arith_or_cmp_op(int op) {
    return op == T_PLUS || op == T_MINUS || op == T_STAR || op == T_SLASH || is_cmp_op(op);
}

static ValType infer_type(FuncScope *sc, Node *n) {
    switch (n->kind) {
        case N_STR: return TY_STRING;
        case N_NUM: return is_float_literal(n) ? TY_FLOAT : TY_INT;
        case N_VAR: return is_string_name(n->text) ? TY_STRING : is_float_name(n->text) ? TY_FLOAT : TY_INT;
        case N_CALL:
            if (nb_strcmp(n->text, "Str$") == 0 || nb_strcmp(n->text, "Chr$") == 0) return TY_STRING;
            return is_string_name(n->text) ? TY_STRING : is_float_name(n->text) ? TY_FLOAT : TY_INT;
        case N_INDEX: return TY_INT; // arrays de enteros unicamente en v1
        case N_FIELD: return is_string_name(n->text) ? TY_STRING : is_float_name(n->text) ? TY_FLOAT : TY_INT;
        case N_NEW: return TY_INT;
        case N_FIRSTLAST: return TY_INT;
        case N_BEFORE: case N_AFTER: return TY_INT;
        case N_UNOP: return infer_type(sc, n->a);
        case N_BINOP: {
            ValType lt = infer_type(sc, n->a), rt = infer_type(sc, n->b);
            if (n->op == T_PLUS && (lt == TY_STRING || rt == TY_STRING)) return TY_STRING;
            if (is_arith_or_cmp_op(n->op) && (lt == TY_FLOAT || rt == TY_FLOAT)) {
                return is_cmp_op(n->op) ? TY_INT : TY_FLOAT;
            }
            return TY_INT;
        }
        default: return TY_INT;
    }
}

static void emit_index_address(FuncScope *sc, Node *n) {
    ArrayInfo *arr = find_array(n->text);
    if (!arr) { nb_fatal(n->line, "array no declarado con Dim", n->text); }
    int dc = n->list_count;
    if (dc > arr->dim_count) dc = arr->dim_count;

    emit_expr(sc, n->list[0]); // x0 = indice_0 (acumulado inicial)
    for (int d = 1; d < dc; d++) {
        push_x0();
        emit_expr(sc, n->list[d]);
        nb_fprintf(out, "    mov x1, x0\n");
        nb_fprintf(out, "    ldr x0, [sp], #16\n");
        nb_fprintf(out, "    mov x2, #%d\n", arr->dim_sizes[d]);
        nb_fprintf(out, "    mul x0, x0, x2\n");
        nb_fprintf(out, "    add x0, x0, x1\n");
    }
    nb_fprintf(out, "    mov x1, x0\n");
    nb_fprintf(out, "    adrp x0, arr_%s\n", n->text);
    nb_fprintf(out, "    add x0, x0, :lo12:arr_%s\n", n->text);
    nb_fprintf(out, "    add x0, x0, x1, lsl #3\n");
}

// var\campo -- direccion del campo (offset 8 = tras el 'next' de la
// lista enlazada, + el indice del campo*8).
static void emit_field_address(FuncScope *sc, Node *n) {
    const char *tname = (n->a->kind == N_VAR) ? find_var_type(n->a->text) : NULL;
    if (!tname) {
        nb_fatal(n->line, "no se pudo determinar el tipo de la variable para el campo (falta '= New Tipo')", n->text);
    }
    int field_idx = find_field_offset(tname, n->text);
    if (field_idx < 0) {
        nb_fatal(n->line, "el tipo no tiene ese campo", n->text);
    }
    emit_expr(sc, n->a); // x0 = puntero a la instancia
    nb_fprintf(out, "    add x0, x0, #%d\n", 8 + field_idx * 8);
}

static ValType emit_expr(FuncScope *sc, Node *n) {
    switch (n->kind) {
        case N_NUM:
            if (is_float_literal(n)) {
                const char *lbl = intern_float(n->num_value);
                nb_fprintf(out, "    adrp x0, %s\n", lbl);
                nb_fprintf(out, "    add x0, x0, :lo12:%s\n", lbl);
                nb_fprintf(out, "    ldr x0, [x0]\n");
                return TY_FLOAT;
            }
            nb_fprintf(out, "    mov x0, #%lld\n", (long long)n->num_value);
            return TY_INT;

        case N_STR: {
            const char *lbl = intern_string(n->text);
            nb_fprintf(out, "    adrp x0, %s\n", lbl);
            nb_fprintf(out, "    add x0, x0, :lo12:%s\n", lbl);
            return TY_STRING;
        }

        case N_VAR: {
            emit_var_address(sc, n->text);
            nb_fprintf(out, "    ldr x0, [x0]\n");
            return is_string_name(n->text) ? TY_STRING : TY_INT;
        }

        case N_INDEX: {
            emit_index_address(sc, n);
            nb_fprintf(out, "    ldr x0, [x0]\n");
            return TY_INT;
        }

        case N_FIELD: {
            emit_field_address(sc, n);
            nb_fprintf(out, "    ldr x0, [x0]\n");
            return is_string_name(n->text) ? TY_STRING : is_float_name(n->text) ? TY_FLOAT : TY_INT;
        }

        case N_NEW: {
            TypeInfo *t = find_type(n->text);
            if (!t) { nb_fatal(n->line, "tipo no declarado con Type", n->text); }
            const char *ctype = canon_type_name(n->text);
            int inst_size = (1 + t->field_count) * 8;
            nb_fprintf(out, "    adrp x9, type_%s_next_idx\n", ctype);
            nb_fprintf(out, "    add x9, x9, :lo12:type_%s_next_idx\n", ctype);
            nb_fprintf(out, "    ldr x10, [x9]\n");
            nb_fprintf(out, "    add x11, x10, #1\n");
            nb_fprintf(out, "    str x11, [x9]\n");
            nb_fprintf(out, "    mov x12, #%d\n", inst_size);
            nb_fprintf(out, "    mul x10, x10, x12\n");
            nb_fprintf(out, "    adrp x11, type_%s_pool\n", ctype);
            nb_fprintf(out, "    add x11, x11, :lo12:type_%s_pool\n", ctype);
            nb_fprintf(out, "    add x11, x11, x10\n");
            nb_fprintf(out, "    mov x13, #0\n");
            for (int i = 0; i <= t->field_count; i++) {
                nb_fprintf(out, "    str x13, [x11, #%d]\n", i * 8);
            }
            nb_fprintf(out, "    adrp x9, type_%s_tail\n", ctype);
            nb_fprintf(out, "    add x9, x9, :lo12:type_%s_tail\n", ctype);
            nb_fprintf(out, "    ldr x10, [x9]\n");
            int l = new_label();
            nb_fprintf(out, "    cbz x10, .Lnew_empty_%d\n", l);
            nb_fprintf(out, "    str x11, [x10]\n");
            nb_fprintf(out, "    b .Lnew_linked_%d\n", l);
            nb_fprintf(out, ".Lnew_empty_%d:\n", l);
            nb_fprintf(out, "    adrp x12, type_%s_head\n", ctype);
            nb_fprintf(out, "    add x12, x12, :lo12:type_%s_head\n", ctype);
            nb_fprintf(out, "    str x11, [x12]\n");
            nb_fprintf(out, ".Lnew_linked_%d:\n", l);
            nb_fprintf(out, "    str x11, [x9]\n");
            nb_fprintf(out, "    mov x0, x11\n");
            return TY_INT;
        }

        case N_FIRSTLAST: {
            const char *which = n->op == T_KW_FIRST ? "head" : "tail";
            const char *ctype = canon_type_name(n->text);
            nb_fprintf(out, "    adrp x0, type_%s_%s\n", ctype, which);
            nb_fprintf(out, "    add x0, x0, :lo12:type_%s_%s\n", ctype, which);
            nb_fprintf(out, "    ldr x0, [x0]\n");
            return TY_INT;
        }

        case N_AFTER: {
            emit_expr(sc, n->a); // evalua la expresion -> x0
            int l = new_label();
            nb_fprintf(out, "    cbz x0, .Lafter_null_%d\n", l);
            nb_fprintf(out, "    ldr x0, [x0]\n");
            nb_fprintf(out, ".Lafter_null_%d:\n", l);
            return TY_INT;
        }

        case N_BEFORE: {
            const char *tname = infer_type_name_from_expr(n->a);
            if (!tname) {
                nb_fatal(n->line, "no se pudo determinar el tipo de la instancia en 'Before'", NULL);
            }
            tname = canon_type_name(tname);
            emit_expr(sc, n->a); // x0 = instancia objetivo
            nb_fprintf(out, "    mov x9, x0\n");
            nb_fprintf(out, "    adrp x10, type_%s_head\n", tname);
            nb_fprintf(out, "    add x10, x10, :lo12:type_%s_head\n", tname);
            nb_fprintf(out, "    ldr x10, [x10]\n");
            int l = new_label();
            nb_fprintf(out, ".Lbefore_loop_%d:\n", l);
            nb_fprintf(out, "    cbz x10, .Lbefore_none_%d\n", l);
            nb_fprintf(out, "    ldr x11, [x10]\n");
            nb_fprintf(out, "    cmp x11, x9\n");
            nb_fprintf(out, "    beq .Lbefore_found_%d\n", l);
            nb_fprintf(out, "    mov x10, x11\n");
            nb_fprintf(out, "    b .Lbefore_loop_%d\n", l);
            nb_fprintf(out, ".Lbefore_found_%d:\n", l);
            nb_fprintf(out, "    mov x0, x10\n");
            nb_fprintf(out, "    b .Lbefore_done_%d\n", l);
            nb_fprintf(out, ".Lbefore_none_%d:\n", l);
            nb_fprintf(out, "    mov x0, #0\n");
            nb_fprintf(out, ".Lbefore_done_%d:\n", l);
            return TY_INT;
        }

        case N_UNOP: {
            ValType t = emit_expr(sc, n->a);
            if (n->op == T_MINUS) {
                if (t == TY_FLOAT) {
                    nb_fprintf(out, "    fmov d0, x0\n    fneg d0, d0\n    fmov x0, d0\n");
                    return TY_FLOAT;
                }
                nb_fprintf(out, "    neg x0, x0\n");
                return TY_INT;
            }
            nb_fprintf(out, "    cmp x0, #0\n    cset x0, eq\n"); // Not
            return TY_INT;
        }

        case N_BINOP: {
            ValType lt = infer_type(sc, n->a);
            ValType rt = infer_type(sc, n->b);

            if (n->op == T_PLUS && (lt == TY_STRING || rt == TY_STRING)) {
                emit_expr(sc, n->a);
                if (lt == TY_FLOAT) nb_fprintf(out, "    bl rt_float_to_str\n");
                else if (lt == TY_INT) nb_fprintf(out, "    bl rt_int_to_str\n");
                push_x0();
                emit_expr(sc, n->b);
                if (rt == TY_FLOAT) nb_fprintf(out, "    bl rt_float_to_str\n");
                else if (rt == TY_INT) nb_fprintf(out, "    bl rt_int_to_str\n");
                pop_to_x1(); // x1 = a (cadena), x0 = b (cadena)
                nb_fprintf(out, "    bl rt_str_concat\n"); // concat(x1, x0) -> x0
                return TY_STRING;
            }

            if (is_arith_or_cmp_op(n->op) && (lt == TY_FLOAT || rt == TY_FLOAT)) {
                emit_expr(sc, n->a);
                if (lt == TY_FLOAT) nb_fprintf(out, "    fmov d0, x0\n");
                else nb_fprintf(out, "    scvtf d0, x0\n");
                nb_fprintf(out, "    fmov x0, d0\n");
                push_x0();
                emit_expr(sc, n->b);
                if (rt == TY_FLOAT) nb_fprintf(out, "    fmov d0, x0\n");
                else nb_fprintf(out, "    scvtf d0, x0\n");
                nb_fprintf(out, "    fmov x0, d0\n");
                pop_to_x1();
                nb_fprintf(out, "    fmov d0, x1\n");
                nb_fprintf(out, "    fmov d1, x0\n");
                switch (n->op) {
                    case T_PLUS:  nb_fprintf(out, "    fadd d0, d0, d1\n"); break;
                    case T_MINUS: nb_fprintf(out, "    fsub d0, d0, d1\n"); break;
                    case T_STAR:  nb_fprintf(out, "    fmul d0, d0, d1\n"); break;
                    case T_SLASH: nb_fprintf(out, "    fdiv d0, d0, d1\n"); break;
                    case T_EQ: nb_fprintf(out, "    fcmp d0, d1\n    cset x0, eq\n"); return TY_INT;
                    case T_NE: nb_fprintf(out, "    fcmp d0, d1\n    cset x0, ne\n"); return TY_INT;
                    case T_LT: nb_fprintf(out, "    fcmp d0, d1\n    cset x0, mi\n"); return TY_INT;
                    case T_GT: nb_fprintf(out, "    fcmp d0, d1\n    cset x0, gt\n"); return TY_INT;
                    case T_LE: nb_fprintf(out, "    fcmp d0, d1\n    cset x0, ls\n"); return TY_INT;
                    case T_GE: nb_fprintf(out, "    fcmp d0, d1\n    cset x0, ge\n"); return TY_INT;
                    default: nb_fatal(n->line, "operador no valido entre flotantes", NULL);
                }
                nb_fprintf(out, "    fmov x0, d0\n");
                return TY_FLOAT;
            }

            emit_expr(sc, n->a);
            push_x0();
            emit_expr(sc, n->b);
            pop_to_x1(); // x1 = a, x0 = b -- listos para operar sin mas barajeo
            switch (n->op) {
                case T_PLUS:  nb_fprintf(out, "    add x0, x1, x0\n"); break;
                case T_MINUS: nb_fprintf(out, "    sub x0, x1, x0\n"); break;
                case T_STAR:  nb_fprintf(out, "    mul x0, x1, x0\n"); break;
                case T_SLASH: nb_fprintf(out, "    sdiv x0, x1, x0\n"); break;
                case T_KW_MOD:
                    nb_fprintf(out, "    sdiv x2, x1, x0\n");
                    nb_fprintf(out, "    msub x0, x2, x0, x1\n");
                    break;
                case T_EQ: nb_fprintf(out, "    cmp x1, x0\n    cset x0, eq\n"); break;
                case T_NE: nb_fprintf(out, "    cmp x1, x0\n    cset x0, ne\n"); break;
                case T_LT: nb_fprintf(out, "    cmp x1, x0\n    cset x0, lt\n"); break;
                case T_GT: nb_fprintf(out, "    cmp x1, x0\n    cset x0, gt\n"); break;
                case T_LE: nb_fprintf(out, "    cmp x1, x0\n    cset x0, le\n"); break;
                case T_GE: nb_fprintf(out, "    cmp x1, x0\n    cset x0, ge\n"); break;
                case T_KW_AND: nb_fprintf(out, "    and x0, x1, x0\n"); break;
                case T_KW_OR:  nb_fprintf(out, "    orr x0, x1, x0\n"); break;
                case T_KW_XOR: nb_fprintf(out, "    eor x0, x1, x0\n"); break;
                case T_KW_SHL: nb_fprintf(out, "    lsl x0, x1, x0\n"); break;
                case T_KW_SHR: nb_fprintf(out, "    lsr x0, x1, x0\n"); break;
                case T_KW_SAR: nb_fprintf(out, "    asr x0, x1, x0\n"); break;
                default: nb_fatal(n->line, "operador binario no soportado", NULL);
            }
            return TY_INT;
        }

        case N_CALL: {
            if (nb_strcmp(n->text, "Str$") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl %s\n", t == TY_FLOAT ? "rt_float_to_str" : "rt_int_to_str");
                return TY_STRING;
            }

            // -- funciones de cadenas --
            if (nb_strcmp(n->text, "Len") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_strlen\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Left$") == 0 || nb_strcmp(n->text, "Right$") == 0 ||
                nb_strcmp(n->text, "Left") == 0 || nb_strcmp(n->text, "Right") == 0) {
                bool is_left = nb_strcmp(n->text, "Left$") == 0 || nb_strcmp(n->text, "Left") == 0;
                const char *helper = is_left ? "rt_left" : "rt_right";
                emit_expr(sc, n->list[0]); push_x0(); // cadena
                emit_expr(sc, n->list[1]);             // n -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl %s\n", helper);
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "Mid$") == 0 || nb_strcmp(n->text, "Mid") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // cadena
                emit_expr(sc, n->list[1]); push_x0(); // inicio
                if (n->list_count > 2) emit_expr(sc, n->list[2]);
                else nb_fprintf(out, "    mov x0, #-1\n"); // sin longitud -> hasta el final
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_mid\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "Upper$") == 0 || nb_strcmp(n->text, "Lower$") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl %s\n", nb_strcmp(n->text, "Upper$") == 0 ? "rt_upper" : "rt_lower");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "Trim$") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_trim\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "LSet$") == 0 || nb_strcmp(n->text, "RSet$") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl %s\n", nb_strcmp(n->text, "LSet$") == 0 ? "rt_lset" : "rt_rset");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "Hex$") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_hex\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "Bin$") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_bin\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "String$") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_string_repeat\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "Locate") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_locate\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "StringWidth") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_strlen\n");
                push_x0();
                nb_fprintf(out, "    mov x8, #206\n    svc #0\n");
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mul x0, x0, x1\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "StringHeight") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #196\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ColorRed") == 0 || nb_strcmp(n->text, "ColorGreen") == 0 || nb_strcmp(n->text, "ColorBlue") == 0) {
                nb_fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n    ldr x0, [x9]\n");
                if (nb_strcmp(n->text, "ColorRed") == 0) nb_fprintf(out, "    lsr x0, x0, #16\n    and x0, x0, #0xFF\n");
                else if (nb_strcmp(n->text, "ColorGreen") == 0) nb_fprintf(out, "    lsr x0, x0, #8\n    and x0, x0, #0xFF\n");
                else nb_fprintf(out, "    and x0, x0, #0xFF\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ClsColor") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]);
                nb_fprintf(out, "    and x3, x0, #0xFF\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n    and x2, x2, #0xFF\n    lsl x2, x2, #8\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n    and x1, x1, #0xFF\n    lsl x1, x1, #16\n");
                nb_fprintf(out, "    orr x0, x1, x2\n    orr x0, x0, x3\n");
                nb_fprintf(out, "    adrp x9, rt_cls_color\n    add x9, x9, :lo12:rt_cls_color\n    str x0, [x9]\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GraphicsWidth") == 0 || nb_strcmp(n->text, "GraphicsHeight") == 0) {
                nb_fprintf(out, "    mov x8, #33\n    svc #0\n");
                if (nb_strcmp(n->text, "GraphicsWidth") == 0) nb_fprintf(out, "    lsr x0, x0, #32\n");
                else nb_fprintf(out, "    and x0, x0, #0xFFFFFFFF\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GraphicsDepth") == 0) {
                nb_fprintf(out, "    mov x0, #32\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CountGFXModes") == 0) {
                nb_fprintf(out, "    mov x0, #1\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GFXModeWidth") == 0 || nb_strcmp(n->text, "GFXModeHeight") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #35\n    svc #0\n");
                if (nb_strcmp(n->text, "GFXModeWidth") == 0) nb_fprintf(out, "    lsr x0, x0, #32\n");
                else nb_fprintf(out, "    and x0, x0, #0xFFFFFFFF\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GFXModeDepth") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x0, #32\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GfxModeExists") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                if (n->list_count > 2) emit_expr(sc, n->list[2]);
                nb_fprintf(out, "    mov x8, #35\n    svc #0\n");
                nb_fprintf(out, "    lsr x9, x0, #32\n");
                nb_fprintf(out, "    and x10, x0, #0xFFFFFFFF\n");
                nb_fprintf(out, "    ldr x11, [sp], #16\n");
                nb_fprintf(out, "    ldr x12, [sp], #16\n");
                nb_fprintf(out, "    cmp x12, x9\n    cset x13, eq\n");
                nb_fprintf(out, "    cmp x11, x10\n    cset x14, eq\n");
                nb_fprintf(out, "    and x0, x13, x14\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetGfxDriver") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "AppTitle") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #130\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "AutoSuspend") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetGadgetFont") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CommandLine$") == 0) {
                nb_fprintf(out, "    bl rt_commandline\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "CurrentDate$") == 0) {
                nb_fprintf(out, "    bl rt_currentdate\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "CurrentTime$") == 0) {
                nb_fprintf(out, "    bl rt_currenttime\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "Stop") == 0) {
                nb_fprintf(out, "    mov x0, #0\n    mov x8, #0\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FreeTimer") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #131\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WaitTimer") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_waittimer\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CountGfxDrivers") == 0) {
                nb_fprintf(out, "    mov x0, #1\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "AvailVidMem") == 0) {
                nb_fprintf(out, "    mov x0, #67108864\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ScanLine") == 0) {
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "VWait") == 0) {
                nb_fprintf(out, "    mov x8, #14\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Origin") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #85\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Viewport") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else nb_fprintf(out, "    mov x0, #0\n");
                push_x0();
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #135\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GetColor") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #86\n    svc #0\n");
                nb_fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n    str x0, [x9]\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CopyRect") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[3]); push_x0();
                emit_expr(sc, n->list[4]); push_x0();
                emit_expr(sc, n->list[5]); push_x0();
                if (n->list_count > 6) emit_expr(sc, n->list[6]);
                else nb_fprintf(out, "    mov x0, #0\n    sub x0, x0, #1\n");
                push_x0();
                if (n->list_count > 7) emit_expr(sc, n->list[7]);
                else nb_fprintf(out, "    mov x0, #0\n    sub x0, x0, #1\n");
                nb_fprintf(out, "    add x0, x0, #1\n");
                nb_fprintf(out, "    mov x12, x0\n");
                nb_fprintf(out, "    ldr x9, [sp], #16\n");
                nb_fprintf(out, "    add x9, x9, #1\n");
                nb_fprintf(out, "    ldr x10, [sp], #16\n");
                nb_fprintf(out, "    ldr x11, [sp], #16\n");
                nb_fprintf(out, "    lsl x11, x11, #32\n");
                nb_fprintf(out, "    orr x4, x11, x10\n");
                nb_fprintf(out, "    ldr x13, [sp], #16\n");
                nb_fprintf(out, "    lsl x12, x12, #16\n");
                nb_fprintf(out, "    orr x3, x12, x13\n");
                nb_fprintf(out, "    ldr x13, [sp], #16\n");
                nb_fprintf(out, "    lsl x9, x9, #16\n");
                nb_fprintf(out, "    orr x2, x9, x13\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #87\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FreeImage") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #88\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "LoadSound$") == 0 || nb_strcmp(n->text, "LoadSound") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #226\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FreeSound") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #227\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "PlaySound") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                if (n->list_count > 1) emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #228\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SoundVolume") == 0) {
                // El kernel no puede usar coma flotante de verdad (ver
                // la nota junto a sound_volume_permil en syscall.c) --
                // convertimos aqui mismo, con hardware real, a un
                // entero "por mil" (0-1000) antes de la syscall.
                emit_expr(sc, n->list[0]); push_x0();
                ValType t = emit_expr(sc, n->list[1]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n"); else nb_fprintf(out, "    fmov d0, x0\n");
                nb_fprintf(out, "    mov x9, #1000\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #229\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SoundPan") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                ValType t = emit_expr(sc, n->list[1]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n"); else nb_fprintf(out, "    fmov d0, x0\n");
                nb_fprintf(out, "    mov x9, #1000\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #230\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SoundPitch") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #231\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "PauseChannel") == 0 || nb_strcmp(n->text, "ResumeChannel") == 0 ||
                nb_strcmp(n->text, "StopChannel") == 0 || nb_strcmp(n->text, "LoopSound") == 0) {
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ChannelPlaying") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ChannelVolume") == 0 || nb_strcmp(n->text, "ChannelPan") == 0 ||
                nb_strcmp(n->text, "ChannelPitch") == 0) {
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "PlayMusic") == 0 || nb_strcmp(n->text, "PlayCDTrack") == 0) {
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "HandleImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]);
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #89\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MidHandle") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                nb_fprintf(out, "    ldr x0, [sp]\n");
                nb_fprintf(out, "    mov x8, #51\n    svc #0\n");
                nb_fprintf(out, "    lsr x1, x0, #33\n");
                nb_fprintf(out, "    and x2, x0, #0xFFFFFFFF\n");
                nb_fprintf(out, "    lsr x2, x2, #1\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #89\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "AutoMidHandle") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #91\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ImageXHandle") == 0 || nb_strcmp(n->text, "ImageYHandle") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #90\n    svc #0\n");
                if (nb_strcmp(n->text, "ImageXHandle") == 0) nb_fprintf(out, "    asr x0, x0, #32\n");
                else nb_fprintf(out, "    lsl x0, x0, #32\n    asr x0, x0, #32\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MaskImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[3]);
                nb_fprintf(out, "    and x3, x0, #0xFF\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n    and x2, x2, #0xFF\n    lsl x2, x2, #8\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n    and x1, x1, #0xFF\n    lsl x1, x1, #16\n");
                nb_fprintf(out, "    orr x1, x1, x2\n    orr x1, x1, x3\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #92\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CopyImage") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #93\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SaveImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #94\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GrabImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[3]);
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #95\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "DrawBlock") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x9, #1\n    lsl x9, x9, #31\n");
                nb_fprintf(out, "    orr x3, x0, x9\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #50\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "TFormFilter") == 0) {
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "TFormImage") == 0) {
                // El kernel no puede usar coma flotante de verdad --
                // convertimos cada componente aqui mismo a punto fijo
                // Q16.16 (x65536) antes de la syscall.
                emit_expr(sc, n->list[0]); push_x0(); // handle de imagen
                ValType ta = emit_expr(sc, n->list[1]);
                if (ta != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n"); else nb_fprintf(out, "    fmov d0, x0\n");
                nb_fprintf(out, "    mov x9, #65536\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                push_x0(); // a# (Q16.16)
                ValType tb = emit_expr(sc, n->list[2]);
                if (tb != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n"); else nb_fprintf(out, "    fmov d0, x0\n");
                nb_fprintf(out, "    mov x9, #65536\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                push_x0(); // b# (Q16.16)
                ValType tc = emit_expr(sc, n->list[3]);
                if (tc != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n"); else nb_fprintf(out, "    fmov d0, x0\n");
                nb_fprintf(out, "    mov x9, #65536\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                push_x0(); // c# (Q16.16)
                ValType td = emit_expr(sc, n->list[4]); // d# -> x0
                if (td != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n"); else nb_fprintf(out, "    fmov d0, x0\n");
                nb_fprintf(out, "    mov x9, #65536\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                nb_fprintf(out, "    mov x4, x0\n"); // x4 = d# (Q16.16)
                nb_fprintf(out, "    ldr x3, [sp], #16\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #225\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ImagesOverlap") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #51\n    svc #0\n");
                push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[3]);
                nb_fprintf(out, "    mov x8, #51\n    svc #0\n");
                push_x0();
                emit_expr(sc, n->list[4]); push_x0();
                emit_expr(sc, n->list[5]); push_x0();

                nb_fprintf(out, "    ldr x5, [sp], #16\n");
                nb_fprintf(out, "    ldr x4, [sp], #16\n");
                nb_fprintf(out, "    ldr x9, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    ldr x8, [sp], #16\n");
                nb_fprintf(out, "    lsr x2, x8, #32\n");
                nb_fprintf(out, "    and x3, x8, #0xFFFFFFFF\n");
                nb_fprintf(out, "    lsr x6, x9, #32\n");
                nb_fprintf(out, "    and x7, x9, #0xFFFFFFFF\n");

                int l = new_label();
                nb_fprintf(out, "    add x8, x4, x6\n    cmp x0, x8\n    bge .Lio_false_%d\n", l);
                nb_fprintf(out, "    add x8, x0, x2\n    cmp x4, x8\n    bge .Lio_false_%d\n", l);
                nb_fprintf(out, "    add x8, x5, x7\n    cmp x1, x8\n    bge .Lio_false_%d\n", l);
                nb_fprintf(out, "    add x8, x1, x3\n    cmp x5, x8\n    bge .Lio_false_%d\n", l);
                nb_fprintf(out, "    mov x0, #1\n    b .Lio_done_%d\n", l);
                nb_fprintf(out, ".Lio_false_%d:\n    mov x0, #0\n", l);
                nb_fprintf(out, ".Lio_done_%d:\n", l);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ImageRectOverlap") == 0 || nb_strcmp(n->text, "ImageRectCollide") == 0) {
                bool is_collide = nb_strcmp(n->text, "ImageRectCollide") == 0;
                int rect_base = is_collide ? 4 : 3;

                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #51\n    svc #0\n");
                push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                if (is_collide) emit_expr(sc, n->list[3]);
                emit_expr(sc, n->list[rect_base]); push_x0();
                emit_expr(sc, n->list[rect_base + 1]); push_x0();
                emit_expr(sc, n->list[rect_base + 2]); push_x0();
                emit_expr(sc, n->list[rect_base + 3]);

                nb_fprintf(out, "    mov x7, x0\n");
                nb_fprintf(out, "    ldr x6, [sp], #16\n");
                nb_fprintf(out, "    ldr x5, [sp], #16\n");
                nb_fprintf(out, "    ldr x4, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    ldr x8, [sp], #16\n");
                nb_fprintf(out, "    lsr x2, x8, #32\n");
                nb_fprintf(out, "    and x3, x8, #0xFFFFFFFF\n");

                int l = new_label();
                nb_fprintf(out, "    add x8, x4, x6\n    cmp x0, x8\n    bge .Lirc_false_%d\n", l);
                nb_fprintf(out, "    add x8, x0, x2\n    cmp x4, x8\n    bge .Lirc_false_%d\n", l);
                nb_fprintf(out, "    add x8, x5, x7\n    cmp x1, x8\n    bge .Lirc_false_%d\n", l);
                nb_fprintf(out, "    add x8, x1, x3\n    cmp x5, x8\n    bge .Lirc_false_%d\n", l);
                nb_fprintf(out, "    mov x0, #1\n    b .Lirc_done_%d\n", l);
                nb_fprintf(out, ".Lirc_false_%d:\n    mov x0, #0\n", l);
                nb_fprintf(out, ".Lirc_done_%d:\n", l);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ResizeImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]);
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #96\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ScaleImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                ValType tx = emit_expr(sc, n->list[1]);
                if (tx != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                push_x0();
                ValType ty = emit_expr(sc, n->list[2]);
                if (ty != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    fmov d1, x0\n");
                nb_fprintf(out, "    ldr x9, [sp], #16\n");
                nb_fprintf(out, "    fmov d0, x9\n");
                nb_fprintf(out, "    ldr x0, [sp]\n");
                nb_fprintf(out, "    mov x8, #51\n    svc #0\n");
                nb_fprintf(out, "    lsr x9, x0, #32\n");
                nb_fprintf(out, "    and x10, x0, #0xFFFFFFFF\n");
                nb_fprintf(out, "    scvtf d2, x9\n    scvtf d3, x10\n");
                nb_fprintf(out, "    fmul d2, d2, d0\n    fmul d3, d3, d1\n");
                nb_fprintf(out, "    fcvtzs x1, d2\n    fcvtzs x2, d3\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #96\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "RotateImage") == 0) {
                // El kernel no puede usar coma flotante de verdad --
                // convertimos el angulo aqui mismo a punto fijo Q16.16
                // (x65536) antes de la syscall.
                emit_expr(sc, n->list[0]); push_x0();
                ValType t = emit_expr(sc, n->list[1]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n"); else nb_fprintf(out, "    fmov d0, x0\n");
                nb_fprintf(out, "    mov x9, #65536\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #97\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "DrawImageRect") == 0 || nb_strcmp(n->text, "DrawBlockRect") == 0) {
                bool is_block = nb_strcmp(n->text, "DrawBlockRect") == 0;
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[3]); push_x0();
                emit_expr(sc, n->list[4]); push_x0();
                emit_expr(sc, n->list[5]); push_x0();
                emit_expr(sc, n->list[6]);
                nb_fprintf(out, "    mov x9, x0\n");
                nb_fprintf(out, "    ldr x10, [sp], #16\n");
                nb_fprintf(out, "    lsl x10, x10, #16\n");
                nb_fprintf(out, "    orr x4, x10, x9\n");
                nb_fprintf(out, "    ldr x10, [sp], #16\n");
                nb_fprintf(out, "    ldr x11, [sp], #16\n");
                nb_fprintf(out, "    lsl x11, x11, #16\n");
                nb_fprintf(out, "    orr x3, x11, x10\n");
                if (is_block) {
                    nb_fprintf(out, "    mov x9, #1\n    lsl x9, x9, #31\n");
                    nb_fprintf(out, "    orr x3, x3, x9\n");
                }
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #98\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "LoadAnimImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]);
                nb_fprintf(out, "    mov x9, x0\n");
                nb_fprintf(out, "    ldr x10, [sp], #16\n");
                nb_fprintf(out, "    lsl x10, x10, #16\n");
                nb_fprintf(out, "    orr x0, x10, x9\n");
                push_x0();
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else nb_fprintf(out, "    mov x0, #0\n");
                push_x0();
                if (n->list_count > 4) emit_expr(sc, n->list[4]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #99\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "TileImage") == 0 || nb_strcmp(n->text, "TileBlock") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                if (n->list_count > 1) emit_expr(sc, n->list[1]); else nb_fprintf(out, "    mov x0, #0\n");
                push_x0();
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x3, #%d\n", nb_strcmp(n->text, "TileBlock") == 0 ? 1 : 0);
                nb_fprintf(out, "    bl rt_tileimage\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Chr$") == 0 || nb_strcmp(n->text, "Chr") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_chr\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "Asc") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_asc\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Instr") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // cadena
                emit_expr(sc, n->list[1]); push_x0(); // buscada
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else nb_fprintf(out, "    mov x0, #1\n");
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_instr\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Replace$") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // cadena
                emit_expr(sc, n->list[1]); push_x0(); // buscada
                emit_expr(sc, n->list[2]);             // sustituta -> x0
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_replace\n");
                return TY_STRING;
            }

            // -- funciones numericas --
            if (nb_strcmp(n->text, "Abs") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_abs\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Sgn") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_sgn\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Min") == 0 || nb_strcmp(n->text, "Max") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl %s\n", nb_strcmp(n->text, "Min") == 0 ? "rt_min" : "rt_max");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Rnd") == 0) {
                if (n->list_count >= 2) {
                    ValType t0 = emit_expr(sc, n->list[0]);
                    if (t0 != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                    push_x0();
                    ValType t1 = emit_expr(sc, n->list[1]);
                    if (t1 != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                    nb_fprintf(out, "    mov x1, x0\n");
                    nb_fprintf(out, "    ldr x0, [sp], #16\n");
                } else {
                    ValType t = emit_expr(sc, n->list[0]);
                    if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                    nb_fprintf(out, "    mov x1, x0\n");
                    nb_fprintf(out, "    mov x0, #0\n");
                }
                nb_fprintf(out, "    bl rt_rnd_float\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "Rand") == 0) {
                if (n->list_count >= 2) {
                    emit_expr(sc, n->list[0]); push_x0();
                    emit_expr(sc, n->list[1]);
                    nb_fprintf(out, "    mov x1, x0\n");
                    nb_fprintf(out, "    ldr x0, [sp], #16\n");
                } else {
                    emit_expr(sc, n->list[0]);
                    nb_fprintf(out, "    mov x1, x0\n");
                    nb_fprintf(out, "    mov x0, #1\n");
                }
                nb_fprintf(out, "    bl rt_rand\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SeedRnd") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_seedrnd\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "RndSeed") == 0) {
                nb_fprintf(out, "    bl rt_rndseed\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "KeyHit") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #53\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GetKey") == 0) {
                nb_fprintf(out, "    mov x8, #12\n    svc #0\n"); // SYS_READ_CHAR
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WaitKey") == 0) {
                nb_fprintf(out, "    mov x8, #13\n    svc #0\n"); // SYS_READ_CHAR_WAIT
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FlushKeys") == 0) {
                nb_fprintf(out, "    mov x8, #55\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FlushMouse") == 0) {
                nb_fprintf(out, "    mov x8, #138\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MoveMouse") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #58\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MouseHit") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #56\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GetMouse") == 0) {
                int l = new_label();
                nb_fprintf(out, "    mov x8, #34\n    svc #0\n");
                nb_fprintf(out, "    and x9, x0, #1\n");
                nb_fprintf(out, "    cbz x9, .Lgm_chkright_%d\n", l);
                nb_fprintf(out, "    mov x0, #1\n");
                nb_fprintf(out, "    b .Lgm_done_%d\n", l);
                nb_fprintf(out, ".Lgm_chkright_%d:\n", l);
                nb_fprintf(out, "    lsr x9, x0, #1\n");
                nb_fprintf(out, "    and x9, x9, #1\n");
                nb_fprintf(out, "    cbz x9, .Lgm_chkmid_%d\n", l);
                nb_fprintf(out, "    mov x0, #2\n");
                nb_fprintf(out, "    b .Lgm_done_%d\n", l);
                nb_fprintf(out, ".Lgm_chkmid_%d:\n", l);
                nb_fprintf(out, "    lsr x9, x0, #2\n");
                nb_fprintf(out, "    and x9, x9, #1\n");
                nb_fprintf(out, "    cbz x9, .Lgm_none_%d\n", l);
                nb_fprintf(out, "    mov x0, #3\n");
                nb_fprintf(out, "    b .Lgm_done_%d\n", l);
                nb_fprintf(out, ".Lgm_none_%d:\n", l);
                nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, ".Lgm_done_%d:\n", l);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WaitMouse") == 0) {
                nb_fprintf(out, "    bl rt_waitmouse\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MouseXSpeed") == 0) {
                nb_fprintf(out, "    mov x8, #57\n    svc #0\n");
                nb_fprintf(out, "    asr x0, x0, #32\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MouseYSpeed") == 0) {
                nb_fprintf(out, "    mov x8, #57\n    svc #0\n");
                nb_fprintf(out, "    lsl x0, x0, #32\n    asr x0, x0, #32\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MouseZ") == 0) {
                nb_fprintf(out, "    mov x8, #137\n    svc #0\n"); // SYS_GET_MOUSE_Z
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MouseZSpeed") == 0) {
                nb_fprintf(out, "    mov x8, #45\n    svc #0\n"); // SYS_GET_MOUSE_WHEEL
                nb_fprintf(out, "    cmp x0, #0\n");
                nb_fprintf(out, "    cset x9, gt\n");
                nb_fprintf(out, "    cmp x0, #0\n");
                nb_fprintf(out, "    cset x10, lt\n");
                nb_fprintf(out, "    sub x0, x9, x10\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Input$") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]);
                else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    bl rt_input\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "CreateBank") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #59\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FreeBank") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #60\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "BankSize") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #61\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ResizeBank") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #62\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CopyBank") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[3]); push_x0();
                emit_expr(sc, n->list[4]);
                nb_fprintf(out, "    mov x4, x0\n");
                nb_fprintf(out, "    ldr x3, [sp], #16\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #63\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ReadBytes") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[3]);
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #220\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WriteBytes") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[3]);
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #221\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "PeekByte") == 0 || nb_strcmp(n->text, "PeekShort") == 0 || nb_strcmp(n->text, "PeekInt") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                int sysnum = nb_strcmp(n->text, "PeekByte") == 0 ? 64 : (nb_strcmp(n->text, "PeekShort") == 0 ? 65 : 66);
                nb_fprintf(out, "    mov x8, #%d\n    svc #0\n", sysnum);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "PeekFloat") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #66\n    svc #0\n");
                nb_fprintf(out, "    fmov s0, w0\n");
                nb_fprintf(out, "    fcvt d0, s0\n");
                nb_fprintf(out, "    fmov x0, d0\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "PokeByte") == 0 || nb_strcmp(n->text, "PokeShort") == 0 || nb_strcmp(n->text, "PokeInt") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]);
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                int sysnum = nb_strcmp(n->text, "PokeByte") == 0 ? 67 : (nb_strcmp(n->text, "PokeShort") == 0 ? 68 : 69);
                nb_fprintf(out, "    mov x8, #%d\n    svc #0\n", sysnum);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "PokeFloat") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                ValType t = emit_expr(sc, n->list[2]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    fmov d0, x0\n");
                nb_fprintf(out, "    fcvt s0, d0\n");
                nb_fprintf(out, "    fmov w2, s0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #69\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Pi") == 0) {
                nb_fprintf(out, "    adrp x9, rt_const_pi\n    add x9, x9, :lo12:rt_const_pi\n    ldr x0, [x9]\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "Float") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "Int") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t == TY_FLOAT) {
                    nb_fprintf(out, "    fmov d0, x0\n    fcvtzs x0, d0\n");
                }
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Floor#") == 0 || nb_strcmp(n->text, "Ceil#") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    fmov d0, x0\n");
                nb_fprintf(out, "    %s d0, d0\n", nb_strcmp(n->text, "Floor#") == 0 ? "frintm" : "frintp");
                nb_fprintf(out, "    fmov x0, d0\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "Tan") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                push_x0();
                nb_fprintf(out, "    bl rt_sin\n");
                push_x0();
                nb_fprintf(out, "    ldr x0, [sp, #16]\n");
                nb_fprintf(out, "    bl rt_cos\n");
                nb_fprintf(out, "    fmov d1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    add sp, sp, #16\n");
                nb_fprintf(out, "    fmov d0, x0\n");
                nb_fprintf(out, "    fdiv d0, d0, d1\n");
                nb_fprintf(out, "    fmov x0, d0\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "ATan") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    bl rt_atan\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "ASin") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    bl rt_asin\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "ACos") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    bl rt_acos\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "ATan2") == 0) {
                ValType t0 = emit_expr(sc, n->list[0]);
                if (t0 != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                push_x0();
                ValType t1 = emit_expr(sc, n->list[1]);
                if (t1 != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_atan2\n");
                return TY_FLOAT;
            }

            // -- bucle de eventos estilo BlitzPlus --
            if (nb_strcmp(n->text, "CreateWindow") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // titulo
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                emit_expr(sc, n->list[3]); push_x0(); // ancho
                emit_expr(sc, n->list[4]); push_x0(); // alto
                if (n->list_count > 5) emit_expr(sc, n->list[5]); // grupo -- evaluado, ignorado
                if (n->list_count > 6) emit_expr(sc, n->list[6]); // style -- evaluado, ignorado
                nb_fprintf(out, "    ldr x4, [sp], #16\n");
                nb_fprintf(out, "    ldr x3, [sp], #16\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #40\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WaitEvent") == 0) {
                if (n->list_count >= 1) emit_expr(sc, n->list[0]);
                else nb_fprintf(out, "    mov x0, #-1\n");
                nb_fprintf(out, "    bl rt_wait_event\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "PollEvent") == 0) {
                nb_fprintf(out, "    bl rt_poll_event\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "PeekEvent") == 0) {
                nb_fprintf(out, "    mov x8, #139\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FlushEvents") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x8, #140\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "EventID") == 0) {
                nb_fprintf(out, "    adrp x0, rt_last_event_id\n");
                nb_fprintf(out, "    add x0, x0, :lo12:rt_last_event_id\n");
                nb_fprintf(out, "    ldr x0, [x0]\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "EventSource") == 0) {
                nb_fprintf(out, "    mov x8, #9\n    svc #0\n    lsr x0, x0, #32\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "EventData") == 0) {
                nb_fprintf(out, "    mov x8, #9\n    svc #0\n    mov w0, w0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "EventX") == 0 || nb_strcmp(n->text, "EventY") == 0) {
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MenuChecked") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #141\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MenuEnabled") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #144\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MenuText$") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_gadget_text\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "SetMenuText") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #105\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Notify") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                if (n->list_count > 1) emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_notify\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Confirm") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                if (n->list_count > 1) emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_confirm\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Proceed") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                if (n->list_count > 1) emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_proceed\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "RequestColor") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // r
                emit_expr(sc, n->list[1]); push_x0(); // g
                emit_expr(sc, n->list[2]);             // b -> x0
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_request_color\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "RequestedRed") == 0) {
                nb_fprintf(out, "    adrp x0, rt_requested_r\n    add x0, x0, :lo12:rt_requested_r\n    ldr x0, [x0]\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "RequestedGreen") == 0) {
                nb_fprintf(out, "    adrp x0, rt_requested_g\n    add x0, x0, :lo12:rt_requested_g\n    ldr x0, [x0]\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "RequestedBlue") == 0) {
                nb_fprintf(out, "    adrp x0, rt_requested_b\n    add x0, x0, :lo12:rt_requested_b\n    ldr x0, [x0]\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "RequestDir$") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_request_dir\n");
                nb_fprintf(out, "    cbnz x0, 430f\n");
                nb_fprintf(out, "    adrp x0, rt_empty_str\n    add x0, x0, :lo12:rt_empty_str\n");
                nb_fprintf(out, "    b 431f\n");
                nb_fprintf(out, "430:\n");
                nb_fprintf(out, "    adrp x0, rt_requested_dir_inode\n    add x0, x0, :lo12:rt_requested_dir_inode\n    ldr x0, [x0]\n");
                nb_fprintf(out, "    bl rt_int_to_str\n");
                nb_fprintf(out, "431:\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "RequestFile$") == 0) {
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                nb_fprintf(out, "    bl rt_request_file\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "ClientWidth") == 0 || nb_strcmp(n->text, "ClientHeight") == 0) {
                if (n->list_count >= 1) emit_expr(sc, n->list[0]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    bl %s\n", nb_strcmp(n->text, "ClientWidth") == 0 ? "rt_client_width" : "rt_client_height");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Desktop") == 0) {
                nb_fprintf(out, "    mov x0, #0x7FFFFFFF\n");
                return TY_INT;
            }

            // -- Gadgets estilo BlitzPlus, ver gadgets.h del kernel --
            // La ventana se deduce sola en el kernel (la de la propia
            // tarea) -- aqui nunca hace falta pasarla.

            if (nb_strcmp(n->text, "CreateButton") == 0) {
                // CreateButton(texto$, x, y, ancho, alto, [grupo], [style])
                emit_expr(sc, n->list[0]); push_x0(); // texto
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                emit_expr(sc, n->list[3]); push_x0(); // ancho
                emit_expr(sc, n->list[4]); push_x0();  // alto
                if (n->list_count > 6) emit_expr(sc, n->list[6]); else nb_fprintf(out, "    mov x0, #1\n"); // style
                nb_fprintf(out, "    mov x4, x0\n"); // x4 = style
                nb_fprintf(out, "    ldr x1, [sp], #16\n");             // x1 = alto
                nb_fprintf(out, "    ldr x0, [sp], #16\n");              // x0 = ancho
                nb_fprintf(out, "    lsl x0, x0, #16\n");
                nb_fprintf(out, "    orr x3, x0, x1\n");                   // x3 = (ancho<<16|alto)
                nb_fprintf(out, "    ldr x2, [sp], #16\n");                 // x2 = y
                nb_fprintf(out, "    ldr x1, [sp], #16\n");                  // x1 = x
                nb_fprintf(out, "    ldr x0, [sp], #16\n");                   // x0 = texto
                nb_fprintf(out, "    mov x8, #100\n    svc #0\n");
                if (n->list_count > 5) {
                    push_x0();
                    emit_expr(sc, n->list[5]); // grupo -> x0
                    nb_fprintf(out, "    mov x1, x0\n");
                    nb_fprintf(out, "    ldr x0, [sp], #16\n");
                    nb_fprintf(out, "    mov x9, x0\n");
                    nb_fprintf(out, "    mov x8, #223\n    svc #0\n");
                    nb_fprintf(out, "    mov x0, x9\n");
                }
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CreateLabel") == 0) {
                // CreateLabel(texto$, x, y, ancho, alto, [grupo], [style])
                // -- misma disposicion que CreateButton: 'grupo' es el
                // indice 5, 'style' el indice 6.
                emit_expr(sc, n->list[0]); push_x0(); // texto
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                emit_expr(sc, n->list[3]); push_x0(); // ancho
                emit_expr(sc, n->list[4]); push_x0();  // alto
                if (n->list_count > 6) emit_expr(sc, n->list[6]); else nb_fprintf(out, "    mov x0, #0\n"); // style
                nb_fprintf(out, "    mov x4, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #16\n");
                nb_fprintf(out, "    orr x3, x0, x1\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #160\n    svc #0\n"); // SYS_CREATE_LABEL
                if (n->list_count > 5) {
                    push_x0();
                    emit_expr(sc, n->list[5]); // grupo -> x0
                    nb_fprintf(out, "    mov x1, x0\n");
                    nb_fprintf(out, "    ldr x0, [sp], #16\n");
                    nb_fprintf(out, "    mov x9, x0\n");
                    nb_fprintf(out, "    mov x8, #223\n    svc #0\n");
                    nb_fprintf(out, "    mov x0, x9\n");
                }
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ButtonState") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #141\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetButtonState") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #142\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "HotKeyEvent") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // rawkey
                emit_expr(sc, n->list[1]);             // modifier -> x0
                nb_fprintf(out, "    mov x9, x0\n");
                nb_fprintf(out, "    ldr x10, [sp], #16\n"); // rawkey
                nb_fprintf(out, "    lsl x10, x10, #8\n");
                nb_fprintf(out, "    orr x0, x10, x9\n");
                push_x0();
                emit_expr(sc, n->list[2]); push_x0(); // event_id
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else nb_fprintf(out, "    mov x0, #0\n");
                push_x0(); // event_data
                for (int i = 4; i < 7 && i < n->list_count; i++) emit_expr(sc, n->list[i]); // x,y,z -- descartados
                if (n->list_count > 7) emit_expr(sc, n->list[7]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x3, x0\n"); // event_source
                nb_fprintf(out, "    ldr x2, [sp], #16\n"); // event_data
                nb_fprintf(out, "    ldr x1, [sp], #16\n"); // event_id
                nb_fprintf(out, "    ldr x0, [sp], #16\n"); // (rawkey<<8|modifier)
                nb_fprintf(out, "    mov x8, #143\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CreatePanel") == 0 || nb_strcmp(n->text, "CreateTextField") == 0 ||
                nb_strcmp(n->text, "CreateListBox") == 0 || nb_strcmp(n->text, "CreateTextArea") == 0 ||
                nb_strcmp(n->text, "CreateProgBar") == 0 || nb_strcmp(n->text, "CreateComboBox") == 0 ||
                nb_strcmp(n->text, "CreateTabber") == 0 || nb_strcmp(n->text, "CreateTreeView") == 0 ||
                nb_strcmp(n->text, "CreateCanvas") == 0) {
                int sys_num = nb_strcmp(n->text, "CreatePanel") == 0 ? 101
                            : nb_strcmp(n->text, "CreateTextField") == 0 ? 102
                            : nb_strcmp(n->text, "CreateListBox") == 0 ? 103
                            : nb_strcmp(n->text, "CreateTextArea") == 0 ? 125
                            : nb_strcmp(n->text, "CreateProgBar") == 0 ? 161
                            : nb_strcmp(n->text, "CreateComboBox") == 0 ? 167
                            : nb_strcmp(n->text, "CreateTabber") == 0 ? 168
                            : nb_strcmp(n->text, "CreateTreeView") == 0 ? 175 : 188;
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                emit_expr(sc, n->list[2]); push_x0(); // ancho
                emit_expr(sc, n->list[3]);             // alto -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #16\n");
                nb_fprintf(out, "    orr x2, x0, x1\n");    // x2 = (ancho<<16|alto)
                nb_fprintf(out, "    ldr x1, [sp], #16\n");   // x1 = y
                nb_fprintf(out, "    ldr x0, [sp], #16\n");    // x0 = x
                nb_fprintf(out, "    mov x8, #%d\n    svc #0\n", sys_num);
                if (n->list_count > 4) {
                    push_x0();
                    emit_expr(sc, n->list[4]); // grupo -> x0
                    nb_fprintf(out, "    mov x1, x0\n");
                    nb_fprintf(out, "    ldr x0, [sp], #16\n");
                    nb_fprintf(out, "    mov x9, x0\n");
                    nb_fprintf(out, "    mov x8, #223\n    svc #0\n");
                    nb_fprintf(out, "    mov x0, x9\n");
                }
                return TY_INT;
            }
            if (nb_strcmp(n->text, "UpdateProgBar") == 0) {
                // El kernel no puede usar coma flotante de verdad --
                // convertimos aqui mismo a un entero "por mil" antes
                // de la syscall.
                emit_expr(sc, n->list[0]); push_x0(); // id
                ValType t = emit_expr(sc, n->list[1]); // valor -> x0
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n"); else nb_fprintf(out, "    fmov d0, x0\n");
                nb_fprintf(out, "    mov x9, #1000\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #162\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CreateSlider") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                emit_expr(sc, n->list[2]); push_x0(); // ancho
                emit_expr(sc, n->list[3]); push_x0();  // alto
                if (n->list_count > 5) emit_expr(sc, n->list[5]); else nb_fprintf(out, "    mov x0, #1\n");
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #16\n");
                nb_fprintf(out, "    orr x2, x0, x1\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #163\n    svc #0\n");
                if (n->list_count > 4) {
                    push_x0();
                    emit_expr(sc, n->list[4]); // grupo -> x0
                    nb_fprintf(out, "    mov x1, x0\n");
                    nb_fprintf(out, "    ldr x0, [sp], #16\n");
                    nb_fprintf(out, "    mov x9, x0\n");
                    nb_fprintf(out, "    mov x8, #223\n    svc #0\n");
                    nb_fprintf(out, "    mov x0, x9\n");
                }
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetSliderRange") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]); push_x0(); // visible
                emit_expr(sc, n->list[2]);             // total -> x0
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #164\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetSliderValue") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // valor -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #165\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SliderValue") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #166\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WindowMenu") == 0) {
                nb_fprintf(out, "    mov x8, #120\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CreateMenu") == 0) {
                // CreateMenu(texto$, tag, padre)
                emit_expr(sc, n->list[0]); push_x0(); // texto
                emit_expr(sc, n->list[1]); push_x0(); // tag
                emit_expr(sc, n->list[2]);             // padre -> x0
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #121\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MenuTag") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #124\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "HideGadget") == 0 || nb_strcmp(n->text, "ShowGadget") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x1, #%d\n", nb_strcmp(n->text, "ShowGadget") == 0 ? 1 : 0);
                nb_fprintf(out, "    mov x8, #110\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "DisableGadget") == 0 || nb_strcmp(n->text, "EnableGadget") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x1, #%d\n", nb_strcmp(n->text, "EnableGadget") == 0 ? 1 : 0);
                nb_fprintf(out, "    mov x8, #111\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ActivateGadget") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #112\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FreeGadget") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #104\n");
                nb_fprintf(out, "    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ActivateWindow") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #150\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ActiveWindow") == 0) {
                nb_fprintf(out, "    mov x8, #151\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MaximizeWindow") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #152\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MinimizeWindow") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #153\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WindowMaximized") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #154\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WindowMinimized") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #155\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetMinWindowSize") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                if (n->list_count > 1) emit_expr(sc, n->list[1]); else nb_fprintf(out, "    mov x0, #0\n");
                push_x0();
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #156\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "DisableMenu") == 0 || nb_strcmp(n->text, "EnableMenu") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x1, #%d\n", nb_strcmp(n->text, "EnableMenu") == 0 ? 1 : 0);
                nb_fprintf(out, "    mov x8, #123\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CheckMenu") == 0 || nb_strcmp(n->text, "UncheckMenu") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x1, #%d\n", nb_strcmp(n->text, "CheckMenu") == 0 ? 1 : 0);
                nb_fprintf(out, "    mov x8, #122\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "UpdateWindowMenu") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GadgetEvent") == 0) {
                nb_fprintf(out, "    mov x8, #113\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Pump") == 0) {
                nb_fprintf(out, "    mov x8, #14\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetGadgetText") == 0) {
                // SetGadgetText(id, texto$)
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // texto -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #105\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "AddGadgetItem") == 0) {
                // AddGadgetItem(id, texto$) -- añade un elemento a un ListBox
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // texto -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #114\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ClearGadgetItems") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #115\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SelectedGadgetItem") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #116\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SelectGadgetItem") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #117\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CountGadgetItems") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #118\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GadgetItemText$") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // indice -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_gadget_item_text\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "InsertGadgetItem") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]); push_x0(); // indice
                emit_expr(sc, n->list[2]);             // texto -> x0
                if (n->list_count > 3) { push_x0(); emit_expr(sc, n->list[3]); nb_fprintf(out, "    ldr x0, [sp], #16\n"); }
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #157\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "RemoveGadgetItem") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #158\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ModifyGadgetItem") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]); push_x0(); // indice
                emit_expr(sc, n->list[2]);             // texto -> x0
                if (n->list_count > 3) { push_x0(); emit_expr(sc, n->list[3]); nb_fprintf(out, "    ldr x0, [sp], #16\n"); }
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #159\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetTextAreaText") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // texto -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #126\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "AddTextAreaText") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #145\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "TextAreaLen") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                if (n->list_count > 1) emit_expr(sc, n->list[1]); else nb_fprintf(out, "    mov x0, #1\n");
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #146\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "TextAreaLineLen") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #147\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "TextAreaLine") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #148\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "TextAreaText$") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                if (n->list_count > 1) emit_expr(sc, n->list[1]); else nb_fprintf(out, "    mov x0, #0\n");
                push_x0(); // start
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else nb_fprintf(out, "    mov x0, #-1\n");
                nb_fprintf(out, "    mov x2, x0\n"); // count
                nb_fprintf(out, "    ldr x1, [sp], #16\n"); // start
                nb_fprintf(out, "    ldr x0, [sp], #16\n"); // id
                nb_fprintf(out, "    bl rt_textarea_text\n");
                return TY_STRING;
            }

            // -- geometria de gadgets --
            if (nb_strcmp(n->text, "GadgetX") == 0 || nb_strcmp(n->text, "GadgetY") == 0 ||
                nb_strcmp(n->text, "GadgetWidth") == 0 || nb_strcmp(n->text, "GadgetHeight") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #107\n    svc #0\n");
                if (nb_strcmp(n->text, "GadgetX") == 0) {
                    nb_fprintf(out, "    lsr x0, x0, #48\n");
                } else if (nb_strcmp(n->text, "GadgetY") == 0) {
                    nb_fprintf(out, "    lsr x0, x0, #32\n    and x0, x0, #0xFFFF\n");
                } else if (nb_strcmp(n->text, "GadgetWidth") == 0) {
                    nb_fprintf(out, "    lsr x0, x0, #16\n    and x0, x0, #0xFFFF\n");
                } else {
                    nb_fprintf(out, "    and x0, x0, #0xFFFF\n");
                }
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GadgetGroup") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #224\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetGadgetShape") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                emit_expr(sc, n->list[3]); push_x0(); // ancho
                emit_expr(sc, n->list[4]);             // alto -> x0
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x0, [sp, #48]\n");
                nb_fprintf(out, "    ldr x1, [sp, #32]\n");
                nb_fprintf(out, "    ldr x2, [sp, #16]\n");
                nb_fprintf(out, "    mov x8, #108\n    svc #0\n");
                nb_fprintf(out, "    ldr x0, [sp, #48]\n");
                nb_fprintf(out, "    ldr x1, [sp]\n");
                nb_fprintf(out, "    mov x2, x3\n");
                nb_fprintf(out, "    mov x8, #109\n    svc #0\n");
                nb_fprintf(out, "    add sp, sp, #64\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CreateTimer") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #127\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "PauseTimer") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #216\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ResumeTimer") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #217\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ResetTimer") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #218\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "TimerTicks") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #219\n    svc #0\n");
                return TY_INT;
            }

            // -- depuracion / archivos de texto --
            if (nb_strcmp(n->text, "DebugLog") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #28\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "RuntimeError") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_notify\n");
                nb_fprintf(out, "    b .Lprogram_end\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ReadFile") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #41\n    svc #0\n");
                int l = new_label();
                nb_fprintf(out, "    cmp x0, #0\n    bge .Lreadf_done_%d\n    mov x0, #0\n", l);
                nb_fprintf(out, ".Lreadf_done_%d:\n", l);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ReadLine$") == 0 || nb_strcmp(n->text, "ReadLine") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_readline\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "GadgetText$") == 0 || nb_strcmp(n->text, "TextFieldText$") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_gadget_text\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "QueryObject") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Eof") == 0) {
                emit_expr(sc, n->list[0]);
                int l = new_label();
                nb_fprintf(out, "    cmp x0, #100\n");
                nb_fprintf(out, "    blt .Leof_old_%d\n", l);
                nb_fprintf(out, "    sub x0, x0, #100\n");
                nb_fprintf(out, "    mov x8, #76\n    svc #0\n");
                nb_fprintf(out, "    b .Leof_done_%d\n", l);
                nb_fprintf(out, ".Leof_old_%d:\n", l);
                nb_fprintf(out, "    mov x8, #43\n    svc #0\n");
                nb_fprintf(out, ".Leof_done_%d:\n", l);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CloseFile") == 0) {
                emit_expr(sc, n->list[0]);
                int l = new_label();
                nb_fprintf(out, "    cmp x0, #100\n");
                nb_fprintf(out, "    blt .Lclose_old_%d\n", l);
                nb_fprintf(out, "    sub x0, x0, #100\n");
                nb_fprintf(out, "    mov x8, #77\n    svc #0\n");
                nb_fprintf(out, "    b .Lclose_done_%d\n", l);
                nb_fprintf(out, ".Lclose_old_%d:\n", l);
                nb_fprintf(out, "    mov x8, #44\n    svc #0\n");
                nb_fprintf(out, ".Lclose_done_%d:\n", l);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "OpenFile") == 0) {
                emit_expr(sc, n->list[0]);
                int l = new_label();
                nb_fprintf(out, "    mov x1, #0\n");
                nb_fprintf(out, "    mov x8, #70\n    svc #0\n");
                nb_fprintf(out, "    cmp x0, #0\n    blt .Lopenf_fail_%d\n    add x0, x0, #100\n    b .Lopenf_done_%d\n", l, l);
                nb_fprintf(out, ".Lopenf_fail_%d:\n    mov x0, #0\n", l);
                nb_fprintf(out, ".Lopenf_done_%d:\n", l);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ExecFile") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #232\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CreateProcess") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #233\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CallDLL") == 0) {
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WriteFile") == 0) {
                emit_expr(sc, n->list[0]);
                int l = new_label();
                nb_fprintf(out, "    mov x1, #1\n");
                nb_fprintf(out, "    mov x8, #70\n    svc #0\n");
                nb_fprintf(out, "    cmp x0, #0\n    blt .Lwritef_fail_%d\n    add x0, x0, #100\n    b .Lwritef_done_%d\n", l, l);
                nb_fprintf(out, ".Lwritef_fail_%d:\n    mov x0, #0\n", l);
                nb_fprintf(out, ".Lwritef_done_%d:\n", l);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FilePos") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    sub x0, x0, #100\n");
                nb_fprintf(out, "    mov x8, #73\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SeekFile") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    sub x0, x0, #100\n");
                nb_fprintf(out, "    mov x8, #74\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ReadAvail") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                nb_fprintf(out, "    ldr x0, [sp]\n");
                nb_fprintf(out, "    sub x0, x0, #100\n");
                nb_fprintf(out, "    mov x8, #75\n    svc #0\n");
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    sub x0, x0, #100\n");
                nb_fprintf(out, "    mov x8, #73\n    svc #0\n");
                nb_fprintf(out, "    sub x0, x1, x0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FileSize") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #81\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FileType") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #82\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "DeleteFile") == 0 || nb_strcmp(n->text, "DeleteDir") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #84\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CreateDir") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                nb_fprintf(out, "    adrp x9, rt_current_dir_inode\n    add x9, x9, :lo12:rt_current_dir_inode\n    ldr x1, [x9]\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x2, #0\n");
                nb_fprintf(out, "    mov x8, #24\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ChangeDir") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_changedir\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CurrentDir$") == 0) {
                nb_fprintf(out, "    bl rt_currentdir\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "ReadDir") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_readdir\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "NextFile$") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_nextfile\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "CloseDir") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #80\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ReadByte") == 0 || nb_strcmp(n->text, "ReadShort") == 0 || nb_strcmp(n->text, "ReadInt") == 0) {
                emit_expr(sc, n->list[0]);
                int nbytes = nb_strcmp(n->text, "ReadByte") == 0 ? 1 : (nb_strcmp(n->text, "ReadShort") == 0 ? 2 : 4);
                int l = new_label();
                nb_fprintf(out, "    cmp x0, #100\n");
                nb_fprintf(out, "    blt .Lrdn_old_%d\n", l);
                nb_fprintf(out, "    sub x0, x0, #100\n");
                nb_fprintf(out, "    adrp x1, rt_file_io_buf\n    add x1, x1, :lo12:rt_file_io_buf\n");
                nb_fprintf(out, "    mov x2, #%d\n", nbytes);
                nb_fprintf(out, "    mov x8, #71\n    svc #0\n");
                nb_fprintf(out, "    b .Lrdn_done_%d\n", l);
                nb_fprintf(out, ".Lrdn_old_%d:\n", l);
                nb_fprintf(out, "    adrp x1, rt_file_io_buf\n    add x1, x1, :lo12:rt_file_io_buf\n");
                nb_fprintf(out, "    mov x2, #%d\n", nbytes);
                nb_fprintf(out, "    mov x8, #136\n    svc #0\n");
                nb_fprintf(out, ".Lrdn_done_%d:\n", l);
                nb_fprintf(out, "    adrp x9, rt_file_io_buf\n    add x9, x9, :lo12:rt_file_io_buf\n");
                nb_fprintf(out, "    ldrb w0, [x9, #%d]\n", nbytes - 1);
                for (int i = nbytes - 2; i >= 0; i--) {
                    nb_fprintf(out, "    lsl x0, x0, #8\n");
                    nb_fprintf(out, "    ldrb w10, [x9, #%d]\n", i);
                    nb_fprintf(out, "    orr x0, x0, x10\n");
                }
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ReadFloat") == 0) {
                emit_expr(sc, n->list[0]);
                int l = new_label();
                nb_fprintf(out, "    cmp x0, #100\n");
                nb_fprintf(out, "    blt .Lrdf_old_%d\n", l);
                nb_fprintf(out, "    sub x0, x0, #100\n");
                nb_fprintf(out, "    adrp x1, rt_file_io_buf\n    add x1, x1, :lo12:rt_file_io_buf\n");
                nb_fprintf(out, "    mov x2, #4\n");
                nb_fprintf(out, "    mov x8, #71\n    svc #0\n");
                nb_fprintf(out, "    b .Lrdf_done_%d\n", l);
                nb_fprintf(out, ".Lrdf_old_%d:\n", l);
                nb_fprintf(out, "    adrp x1, rt_file_io_buf\n    add x1, x1, :lo12:rt_file_io_buf\n");
                nb_fprintf(out, "    mov x2, #4\n");
                nb_fprintf(out, "    mov x8, #136\n    svc #0\n");
                nb_fprintf(out, ".Lrdf_done_%d:\n", l);
                nb_fprintf(out, "    adrp x9, rt_file_io_buf\n    add x9, x9, :lo12:rt_file_io_buf\n");
                nb_fprintf(out, "    ldrb w0, [x9, #3]\n");
                nb_fprintf(out, "    lsl x0, x0, #8\n    ldrb w10, [x9, #2]\n    orr x0, x0, x10\n");
                nb_fprintf(out, "    lsl x0, x0, #8\n    ldrb w10, [x9, #1]\n    orr x0, x0, x10\n");
                nb_fprintf(out, "    lsl x0, x0, #8\n    ldrb w10, [x9, #0]\n    orr x0, x0, x10\n");
                nb_fprintf(out, "    fmov s0, w0\n    fcvt d0, s0\n    fmov x0, d0\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "WriteByte") == 0 || nb_strcmp(n->text, "WriteShort") == 0 || nb_strcmp(n->text, "WriteInt") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                int nbytes = nb_strcmp(n->text, "WriteByte") == 0 ? 1 : (nb_strcmp(n->text, "WriteShort") == 0 ? 2 : 4);
                nb_fprintf(out, "    adrp x9, rt_file_io_buf\n    add x9, x9, :lo12:rt_file_io_buf\n");
                for (int i = 0; i < nbytes; i++) {
                    nb_fprintf(out, "    strb w0, [x9, #%d]\n", i);
                    if (i < nbytes - 1) nb_fprintf(out, "    lsr x0, x0, #8\n");
                }
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    sub x0, x0, #100\n");
                nb_fprintf(out, "    mov x1, x9\n");
                nb_fprintf(out, "    mov x2, #%d\n", nbytes);
                nb_fprintf(out, "    mov x8, #72\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WriteFloat") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                ValType t = emit_expr(sc, n->list[1]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    fmov d0, x0\n    fcvt s0, d0\n    fmov w0, s0\n");
                nb_fprintf(out, "    adrp x9, rt_file_io_buf\n    add x9, x9, :lo12:rt_file_io_buf\n");
                nb_fprintf(out, "    strb w0, [x9]\n");
                nb_fprintf(out, "    lsr x0, x0, #8\n    strb w0, [x9, #1]\n");
                nb_fprintf(out, "    lsr x0, x0, #8\n    strb w0, [x9, #2]\n");
                nb_fprintf(out, "    lsr x0, x0, #8\n    strb w0, [x9, #3]\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    sub x0, x0, #100\n");
                nb_fprintf(out, "    mov x1, x9\n    mov x2, #4\n");
                nb_fprintf(out, "    mov x8, #72\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WriteLine$") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_writeline\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WriteString$") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_writestring\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ReadString$") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_readstring\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "CopyFile") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    bl rt_copyfile\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Exp") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    bl rt_exp\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "Log") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    bl rt_log\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "Log10") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    bl rt_log10\n");
                return TY_FLOAT;
            }

            // -- Graphics 2D: modo clasico, sin depender del sistema
            // de ventanas con eventos -- ver la nota equivalente en el
            // compilador del host para el porque de cada decision.
            if (nb_strcmp(n->text, "Graphics") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #46\n    svc #0\n"); // SYS_GRAPHICS_MODE
                for (int i = 2; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetBuffer") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]);
                else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    sub x0, x0, #1\n");
                nb_fprintf(out, "    mov x8, #128\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "BackBuffer") == 0 || nb_strcmp(n->text, "FrontBuffer") == 0 || nb_strcmp(n->text, "GraphicsBuffer") == 0) {
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ImageBuffer") == 0) {
                if (n->list_count > 1) emit_expr(sc, n->list[1]);
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    add x0, x0, #1\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CanvasBuffer") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    add x0, x0, #100000\n");
                nb_fprintf(out, "    add x0, x0, #1\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FlipCanvas") == 0) {
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                nb_fprintf(out, "    mov x8, #14\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "DesktopBuffer") == 0) {
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "EndGraphics") == 0) {
                nb_fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "LockBuffer") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x8, #208\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "UnlockBuffer") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x8, #209\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "LockedPixels") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x8, #210\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "LockedPitch") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x8, #211\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "LockedFormat") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x8, #212\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ReadPixel") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #185\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WritePixel") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                emit_expr(sc, n->list[2]); push_x0(); // argb
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #186\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CopyPixel") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // src_x
                emit_expr(sc, n->list[1]); push_x0(); // src_y
                emit_expr(sc, n->list[2]); push_x0(); // src_buffer
                emit_expr(sc, n->list[3]); push_x0(); // dest_x
                emit_expr(sc, n->list[4]); push_x0(); // dest_y
                if (n->list_count > 5) emit_expr(sc, n->list[5]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x9, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #16\n");
                nb_fprintf(out, "    orr x2, x0, x9\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x9, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #16\n");
                nb_fprintf(out, "    orr x0, x0, x9\n");
                nb_fprintf(out, "    mov x8, #187\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ReadPixelFast") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #213\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "WritePixelFast") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                emit_expr(sc, n->list[2]); push_x0(); // argb
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #214\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CopyPixelFast") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // src_x
                emit_expr(sc, n->list[1]); push_x0(); // src_y
                emit_expr(sc, n->list[2]); push_x0(); // src_buffer
                emit_expr(sc, n->list[3]); push_x0(); // dest_x
                emit_expr(sc, n->list[4]); push_x0(); // dest_y
                if (n->list_count > 5) emit_expr(sc, n->list[5]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x9, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #16\n");
                nb_fprintf(out, "    orr x2, x0, x9\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x9, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #16\n");
                nb_fprintf(out, "    orr x0, x0, x9\n");
                nb_fprintf(out, "    mov x8, #215\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Flip") == 0) {
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                nb_fprintf(out, "    mov x8, #14\n    svc #0\n"); // SYS_PUMP
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Color") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // r
                emit_expr(sc, n->list[1]); push_x0(); // g
                emit_expr(sc, n->list[2]);             // b -> x0
                nb_fprintf(out, "    and x3, x0, #0xFF\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    and x2, x2, #0xFF\n");
                nb_fprintf(out, "    lsl x2, x2, #8\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    and x1, x1, #0xFF\n");
                nb_fprintf(out, "    lsl x1, x1, #16\n");
                nb_fprintf(out, "    orr x0, x1, x2\n");
                nb_fprintf(out, "    orr x0, x0, x3\n");
                nb_fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n");
                nb_fprintf(out, "    str x0, [x9]\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Oval") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[3]);
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n    ldr x4, [x9]\n");
                nb_fprintf(out, "    mov x8, #47\n    svc #0\n"); // SYS_DRAW_OVAL
                if (n->list_count > 4) emit_expr(sc, n->list[4]);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "Text") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // x -> [sp,#32]
                emit_expr(sc, n->list[1]); push_x0(); // y -> [sp,#16]
                emit_expr(sc, n->list[2]); push_x0(); // texto$ -> [sp,#0]

                if (n->list_count > 3) {
                    emit_expr(sc, n->list[3]);
                    int l = new_label();
                    nb_fprintf(out, "    cbz x0, .Ltext_nocx_%d\n", l);
                    nb_fprintf(out, "    ldr x0, [sp]\n");
                    nb_fprintf(out, "    bl rt_strlen\n");
                    nb_fprintf(out, "    mov x1, #3\n    mul x0, x0, x1\n");
                    nb_fprintf(out, "    ldr x1, [sp, #32]\n");
                    nb_fprintf(out, "    sub x1, x1, x0\n");
                    nb_fprintf(out, "    str x1, [sp, #32]\n");
                    nb_fprintf(out, ".Ltext_nocx_%d:\n", l);
                }
                if (n->list_count > 4) {
                    emit_expr(sc, n->list[4]);
                    int l = new_label();
                    nb_fprintf(out, "    cbz x0, .Ltext_nocy_%d\n", l);
                    nb_fprintf(out, "    ldr x1, [sp, #16]\n");
                    nb_fprintf(out, "    sub x1, x1, #3\n");
                    nb_fprintf(out, "    str x1, [sp, #16]\n");
                    nb_fprintf(out, ".Ltext_nocy_%d:\n", l);
                }

                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n    ldr x3, [x9]\n");
                nb_fprintf(out, "    mov x8, #%d\n    svc #0\n", SYS_DRAW_TEXT);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "KeyDown") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #48\n    svc #0\n"); // SYS_KEY_DOWN
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MouseX") == 0 || nb_strcmp(n->text, "MouseY") == 0 || nb_strcmp(n->text, "MouseDown") == 0) {
                bool is_down = nb_strcmp(n->text, "MouseDown") == 0;
                if (is_down) {
                    if (n->list_count > 0) emit_expr(sc, n->list[0]); else nb_fprintf(out, "    mov x0, #1\n");
                    push_x0();
                }
                nb_fprintf(out, "    mov x8, #%d\n    svc #0\n", SYS_GET_MOUSE);
                int l = new_label();
                nb_fprintf(out, "    add x9, x0, #1\n");
                nb_fprintf(out, "    cbz x9, .Lmouse_unfocused_%d\n", l);
                if (nb_strcmp(n->text, "MouseX") == 0) {
                    nb_fprintf(out, "    lsr x0, x0, #32\n    and x0, x0, #0xFFFF\n");
                } else if (nb_strcmp(n->text, "MouseY") == 0) {
                    nb_fprintf(out, "    lsr x0, x0, #16\n    and x0, x0, #0xFFFF\n");
                } else {
                    nb_fprintf(out, "    and x9, x0, #7\n");
                    nb_fprintf(out, "    ldr x10, [sp], #16\n");
                    nb_fprintf(out, "    cmp x10, #1\n");
                    nb_fprintf(out, "    beq .Lmd_b1_%d\n", l);
                    nb_fprintf(out, "    cmp x10, #2\n");
                    nb_fprintf(out, "    beq .Lmd_b2_%d\n", l);
                    nb_fprintf(out, "    lsr x9, x9, #2\n    and x0, x9, #1\n");
                    nb_fprintf(out, "    b .Lmouse_done_%d\n", l);
                    nb_fprintf(out, ".Lmd_b1_%d:\n", l);
                    nb_fprintf(out, "    and x0, x9, #1\n");
                    nb_fprintf(out, "    b .Lmouse_done_%d\n", l);
                    nb_fprintf(out, ".Lmd_b2_%d:\n", l);
                    nb_fprintf(out, "    lsr x9, x9, #1\n    and x0, x9, #1\n");
                }
                nb_fprintf(out, "    b .Lmouse_done_%d\n", l);
                nb_fprintf(out, ".Lmouse_unfocused_%d:\n", l);
                if (is_down) nb_fprintf(out, "    ldr x11, [sp], #16\n");
                nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, ".Lmouse_done_%d:\n", l);
                return TY_INT;
            }
            if (nb_strcmp(n->text, "MilliSecs") == 0) {
                nb_fprintf(out, "    mov x8, #2\n    svc #0\n");
                nb_fprintf(out, "    mov x1, #10\n    mul x0, x0, x1\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "RectsOverlap") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[3]); push_x0();
                emit_expr(sc, n->list[4]); push_x0();
                emit_expr(sc, n->list[5]); push_x0();
                emit_expr(sc, n->list[6]); push_x0();
                emit_expr(sc, n->list[7]);
                nb_fprintf(out, "    mov x7, x0\n");
                nb_fprintf(out, "    ldr x6, [sp], #16\n");
                nb_fprintf(out, "    ldr x5, [sp], #16\n");
                nb_fprintf(out, "    ldr x4, [sp], #16\n");
                nb_fprintf(out, "    ldr x3, [sp], #16\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                int l = new_label();
                nb_fprintf(out, "    add x8, x4, x6\n    cmp x0, x8\n    bge .Lro_false_%d\n", l);
                nb_fprintf(out, "    add x8, x0, x2\n    cmp x4, x8\n    bge .Lro_false_%d\n", l);
                nb_fprintf(out, "    add x8, x5, x7\n    cmp x1, x8\n    bge .Lro_false_%d\n", l);
                nb_fprintf(out, "    add x8, x1, x3\n    cmp x5, x8\n    bge .Lro_false_%d\n", l);
                nb_fprintf(out, "    mov x0, #1\n    b .Lro_done_%d\n", l);
                nb_fprintf(out, ".Lro_false_%d:\n    mov x0, #0\n", l);
                nb_fprintf(out, ".Lro_done_%d:\n", l);
                return TY_INT;
            }

            // -- LoadImage/DrawImage/ImageWidth/ImageHeight/ImagesCollide --
            if (nb_strcmp(n->text, "LoadImage") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #49\n    svc #0\n"); // SYS_LOAD_IMAGE
                return TY_INT;
            }
            if (nb_strcmp(n->text, "LoadIconStrip") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #169\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FreeIconStrip") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #170\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetGadgetIconStrip") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #171\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetPanelColor") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]); push_x0(); // r
                emit_expr(sc, n->list[2]); push_x0(); // g
                emit_expr(sc, n->list[3]);             // b -> x0
                nb_fprintf(out, "    mov x9, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #8\n");
                nb_fprintf(out, "    orr x9, x9, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #16\n");
                nb_fprintf(out, "    orr x1, x9, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #207\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetPanelImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #222\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CreateToolBar") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // image$
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                emit_expr(sc, n->list[3]); push_x0(); // ancho
                emit_expr(sc, n->list[4]);             // alto -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #16\n");
                nb_fprintf(out, "    orr x3, x0, x1\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #172\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "EnableToolBarItem") == 0 || nb_strcmp(n->text, "DisableToolBarItem") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // indice -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    mov x2, #%d\n", nb_strcmp(n->text, "EnableToolBarItem") == 0 ? 1 : 0);
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #173\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetToolBarTips") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // texto -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #174\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "TreeViewRoot") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #176\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "AddTreeViewNode") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // texto
                emit_expr(sc, n->list[1]);             // padre -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #177\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "InsertTreeViewNode") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // indice
                emit_expr(sc, n->list[1]); push_x0(); // texto
                emit_expr(sc, n->list[2]);             // padre -> x0
                nb_fprintf(out, "    mov x2, x0\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #178\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ModifyTreeViewNode") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // nodo
                emit_expr(sc, n->list[1]);             // texto -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #179\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FreeTreeViewNode") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #180\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ExpandTreeViewNode") == 0 || nb_strcmp(n->text, "CollapseTreeViewNode") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x1, #%d\n", nb_strcmp(n->text, "ExpandTreeViewNode") == 0 ? 1 : 0);
                nb_fprintf(out, "    mov x8, #181\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "CountTreeViewNodes") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #182\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SelectedTreeViewNode") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #183\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SelectTreeViewNode") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #184\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "TreeViewNodeText$") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_gadget_text\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "CreateImage") == 0) {
                // Lienzo vacio (transparente), sin cargar nada de disco.
                emit_expr(sc, n->list[0]); push_x0(); // ancho
                emit_expr(sc, n->list[1]);             // alto -> x0
                nb_fprintf(out, "    mov x1, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #52\n    svc #0\n"); // SYS_CREATE_IMAGE
                return TY_INT;
            }
            if (nb_strcmp(n->text, "LoadFont") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // nombre
                if (n->list_count > 1) emit_expr(sc, n->list[1]); else nb_fprintf(out, "    mov x0, #12\n");
                push_x0(); // alto
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else nb_fprintf(out, "    mov x0, #0\n");
                push_x0(); // negrita
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else nb_fprintf(out, "    mov x0, #0\n");
                push_x0(); // cursiva
                if (n->list_count > 4) emit_expr(sc, n->list[4]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x4, x0\n");
                nb_fprintf(out, "    ldr x3, [sp], #16\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #189\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FreeFont") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #190\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetFont") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #191\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FontName$") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_font_name\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "FontSize") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #193\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FontStyle") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #194\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FontWidth") == 0) {
                nb_fprintf(out, "    mov x8, #195\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "FontHeight") == 0) {
                nb_fprintf(out, "    mov x8, #196\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "SetGamma") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // r
                emit_expr(sc, n->list[1]); push_x0(); // g
                emit_expr(sc, n->list[2]); push_x0(); // b
                emit_expr(sc, n->list[3]); push_x0(); // dest_r
                emit_expr(sc, n->list[4]); push_x0(); // dest_g
                emit_expr(sc, n->list[5]);             // dest_b -> x0
                nb_fprintf(out, "    mov x9, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #8\n");
                nb_fprintf(out, "    orr x9, x9, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #16\n");
                nb_fprintf(out, "    orr x1, x9, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x9, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #8\n");
                nb_fprintf(out, "    orr x9, x9, x0\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    lsl x0, x0, #16\n");
                nb_fprintf(out, "    orr x0, x9, x0\n");
                nb_fprintf(out, "    mov x8, #197\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "UpdateGamma") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x8, #198\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GammaRed") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #199\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GammaGreen") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #200\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GammaBlue") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #201\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GfxDriverName$") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    bl rt_gfx_driver_name\n");
                return TY_STRING;
            }
            if (nb_strcmp(n->text, "GfxModeFormat") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #203\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "GraphicsFormat") == 0) {
                nb_fprintf(out, "    mov x8, #204\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "TotalVidMem") == 0) {
                nb_fprintf(out, "    mov x8, #205\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "DrawImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                if (n->list_count > 3) emit_expr(sc, n->list[3]);
                else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    mov x3, x0\n");
                nb_fprintf(out, "    ldr x2, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    mov x8, #50\n    svc #0\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ImageWidth") == 0 || nb_strcmp(n->text, "ImageHeight") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #51\n    svc #0\n"); // SYS_IMAGE_SIZE
                if (nb_strcmp(n->text, "ImageWidth") == 0) nb_fprintf(out, "    lsr x0, x0, #32\n");
                else nb_fprintf(out, "    and x0, x0, #0xFFFFFFFF\n");
                return TY_INT;
            }
            if (nb_strcmp(n->text, "ImagesCollide") == 0) {
                emit_expr(sc, n->list[0]);
                nb_fprintf(out, "    mov x8, #51\n    svc #0\n");
                push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[4]);
                nb_fprintf(out, "    mov x8, #51\n    svc #0\n");
                push_x0();
                emit_expr(sc, n->list[5]); push_x0();
                emit_expr(sc, n->list[6]); push_x0();

                nb_fprintf(out, "    ldr x5, [sp], #16\n");
                nb_fprintf(out, "    ldr x4, [sp], #16\n");
                nb_fprintf(out, "    ldr x9, [sp], #16\n");
                nb_fprintf(out, "    ldr x1, [sp], #16\n");
                nb_fprintf(out, "    ldr x0, [sp], #16\n");
                nb_fprintf(out, "    ldr x8, [sp], #16\n");
                nb_fprintf(out, "    lsr x2, x8, #32\n");
                nb_fprintf(out, "    and x3, x8, #0xFFFFFFFF\n");
                nb_fprintf(out, "    lsr x6, x9, #32\n");
                nb_fprintf(out, "    and x7, x9, #0xFFFFFFFF\n");

                int l = new_label();
                nb_fprintf(out, "    add x8, x4, x6\n    cmp x0, x8\n    bge .Lic_false_%d\n", l);
                nb_fprintf(out, "    add x8, x0, x2\n    cmp x4, x8\n    bge .Lic_false_%d\n", l);
                nb_fprintf(out, "    add x8, x5, x7\n    cmp x1, x8\n    bge .Lic_false_%d\n", l);
                nb_fprintf(out, "    add x8, x1, x3\n    cmp x5, x8\n    bge .Lic_false_%d\n", l);
                nb_fprintf(out, "    mov x0, #1\n    b .Lic_done_%d\n", l);
                nb_fprintf(out, ".Lic_false_%d:\n    mov x0, #0\n", l);
                nb_fprintf(out, ".Lic_done_%d:\n", l);
                return TY_INT;
            }

            if (nb_strcmp(n->text, "Sqr") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    fmov d0, x0\n    fsqrt d0, d0\n    fmov x0, d0\n");
                return TY_FLOAT;
            }
            if (nb_strcmp(n->text, "Sin") == 0 || nb_strcmp(n->text, "Cos") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                nb_fprintf(out, "    bl %s\n", nb_strcmp(n->text, "Sin") == 0 ? "rt_sin" : "rt_cos");
                return TY_FLOAT;
            }

            // -- indexado de array o llamada a funcion de usuario, ver mas abajo --

            // "nombre(indice)" es sintacticamente identico a una
            // llamada a funcion -- el parser no puede saber cual es
            // sin conocer las declaraciones, asi que lo decidimos
            // aqui: si 'nombre' es un array declarado con Dim, es una
            // LECTURA de ese array, no una llamada.
            ArrayInfo *arr = find_array(n->text);
            if (arr) {
                Node fake_index;
                nb_memset(&fake_index, 0, sizeof(fake_index));
                fake_index.kind = N_INDEX;
                fake_index.line = n->line;
                nb_strncpy(fake_index.text, n->text, sizeof(fake_index.text) - 1);
                fake_index.list = n->list;
                fake_index.list_count = n->list_count;
                emit_index_address(sc, &fake_index);
                nb_fprintf(out, "    ldr x0, [x0]\n");
                return TY_INT;
            }

            if (!is_function(n->text)) {
                nb_fatal(n->line, "funcion no declarada", n->text);
            }
            // Evaluamos los argumentos dados; si faltan, se completan
            // con su valor por defecto (ver parse_funcdef).
            Node *fdef = find_funcdef(n->text);
            int declared = fdef ? fdef->list_count : n->list_count;
            for (int i = 0; i < n->list_count; i++) {
                emit_expr(sc, n->list[i]);
                push_x0();
            }
            for (int i = n->list_count; i < declared && i < 8; i++) {
                Node *param = fdef->list[i];
                if (param->b) emit_expr(sc, param->b);
                else nb_fprintf(out, "    mov x0, #0\n");
                push_x0();
            }
            int total = (n->list_count > declared) ? n->list_count : declared;
            if (total > 8) total = 8;
            for (int i = total - 1; i >= 0; i--) {
                nb_fprintf(out, "    ldr x%d, [sp], #16\n", i);
            }
            nb_fprintf(out, "    bl func_%s\n", n->text);
            return is_string_name(n->text) ? TY_STRING : TY_INT;
        }

        default: {
            char kind_str[12];
            nb_itoa(n->kind, kind_str, sizeof(kind_str));
            nb_fatal(n->line, "nodo de expresion no soportado (tipo de nodo)", kind_str);
        }
    }
}

static void emit_assign(FuncScope *sc, Node *n) {
    Node *target = n->a;
    if (target->kind == N_INDEX) {
        emit_index_address(sc, target); // direccion -> x0
        push_x0();
        emit_expr(sc, n->b); // valor -> x0
        pop_to_x1(); // x1 = direccion, x0 = valor
        nb_fprintf(out, "    str x0, [x1]\n");
        return;
    }
    if (target->kind == N_FIELD) {
        emit_field_address(sc, target); // direccion -> x0
        push_x0();
        ValType src_t = emit_expr(sc, n->b); // valor -> x0
        if (is_float_name(target->text) && src_t == TY_INT) {
            nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
        } else if (!is_float_name(target->text) && !is_string_name(target->text) && src_t == TY_FLOAT) {
            nb_fprintf(out, "    fmov d0, x0\n    fcvtzs x0, d0\n");
        }
        pop_to_x1(); // x1 = direccion, x0 = valor
        nb_fprintf(out, "    str x0, [x1]\n");
        return;
    }
    ValType src_t = emit_expr(sc, n->b); // valor -> x0
    if (is_float_name(target->text) && src_t == TY_INT) {
        nb_fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
    } else if (!is_float_name(target->text) && !is_string_name(target->text) && src_t == TY_FLOAT) {
        nb_fprintf(out, "    fmov d0, x0\n    fcvtzs x0, d0\n");
    }
    push_x0();
    emit_var_address(sc, target->text); // direccion -> x0
    pop_to_x1(); // x1 = valor, x0 = direccion
    nb_fprintf(out, "    str x1, [x0]\n");
}

static void emit_read(FuncScope *sc, Node *n) {
    nb_fprintf(out, "    adrp x9, rt_data_ptr\n");
    nb_fprintf(out, "    add x9, x9, :lo12:rt_data_ptr\n");
    nb_fprintf(out, "    ldr x10, [x9]\n");
    nb_fprintf(out, "    ldr x0, [x10]\n");
    nb_fprintf(out, "    add x10, x10, #8\n");
    nb_fprintf(out, "    str x10, [x9]\n");
    push_x0();
    if (n->a->kind == N_INDEX) {
        emit_index_address(sc, n->a);
    } else {
        emit_var_address(sc, n->a->text);
    }
    pop_to_x1();
    nb_fprintf(out, "    str x1, [x0]\n");
}

static void emit_restore(Node *n) {
    nb_fprintf(out, "    adrp x9, rt_data_ptr\n");
    nb_fprintf(out, "    add x9, x9, :lo12:rt_data_ptr\n");
    if (n->text[0] != '\0') {
        nb_fprintf(out, "    adrp x10, dl_%s\n", n->text);
        nb_fprintf(out, "    add x10, x10, :lo12:dl_%s\n", n->text);
    } else {
        nb_fprintf(out, "    adrp x10, rt_data_table\n");
        nb_fprintf(out, "    add x10, x10, :lo12:rt_data_table\n");
    }
    nb_fprintf(out, "    str x10, [x9]\n");
}

static void emit_delete(Node *n) {
    const char *tname = (n->a->kind == N_VAR) ? find_var_type(n->a->text) : NULL;
    if (!tname) {
        nb_fatal(n->line, "no se pudo determinar el tipo de la variable en Delete", NULL);
    }
    tname = canon_type_name(tname);
    int l = new_label();
    emit_expr(NULL, n->a); // x0 = instancia a borrar (target)
    nb_fprintf(out, "    mov x19, x0\n");
    nb_fprintf(out, "    adrp x9, type_%s_head\n", tname);
    nb_fprintf(out, "    add x9, x9, :lo12:type_%s_head\n", tname);
    nb_fprintf(out, "    ldr x10, [x9]\n");
    nb_fprintf(out, "    mov x11, #0\n");
    nb_fprintf(out, ".Ldel_scan_%d:\n", l);
    nb_fprintf(out, "    cmp x10, x19\n");
    nb_fprintf(out, "    beq .Ldel_found_%d\n", l);
    nb_fprintf(out, "    cbz x10, .Ldel_done_%d\n", l);
    nb_fprintf(out, "    mov x11, x10\n");
    nb_fprintf(out, "    ldr x10, [x10]\n");
    nb_fprintf(out, "    b .Ldel_scan_%d\n", l);
    nb_fprintf(out, ".Ldel_found_%d:\n", l);
    nb_fprintf(out, "    ldr x12, [x10]\n");
    nb_fprintf(out, "    cbz x11, .Ldel_was_head_%d\n", l);
    nb_fprintf(out, "    str x12, [x11]\n");
    nb_fprintf(out, "    b .Ldel_fix_tail_%d\n", l);
    nb_fprintf(out, ".Ldel_was_head_%d:\n", l);
    nb_fprintf(out, "    str x12, [x9]\n");
    nb_fprintf(out, ".Ldel_fix_tail_%d:\n", l);
    nb_fprintf(out, "    adrp x13, type_%s_tail\n", tname);
    nb_fprintf(out, "    add x13, x13, :lo12:type_%s_tail\n", tname);
    nb_fprintf(out, "    ldr x14, [x13]\n");
    nb_fprintf(out, "    cmp x14, x19\n");
    nb_fprintf(out, "    bne .Ldel_done_%d\n", l);
    nb_fprintf(out, "    str x11, [x13]\n");
    nb_fprintf(out, ".Ldel_done_%d:\n", l);
}

static void emit_insert(Node *n) {
    const char *tname = infer_type_name_from_expr(n->a);
    if (!tname) tname = infer_type_name_from_expr(n->b);
    if (!tname) {
        nb_fatal(n->line, "no se pudo determinar el tipo de la instancia en 'Insert'", NULL);
    }
    tname = canon_type_name(tname);
    int l = new_label();
    emit_expr(NULL, n->a); // x0 = instancia a mover
    nb_fprintf(out, "    mov x19, x0\n");
    emit_expr(NULL, n->b); // x0 = instancia objetivo
    nb_fprintf(out, "    mov x20, x0\n");

    nb_fprintf(out, "    adrp x9, type_%s_head\n", tname);
    nb_fprintf(out, "    add x9, x9, :lo12:type_%s_head\n", tname);
    nb_fprintf(out, "    adrp x13, type_%s_tail\n", tname);
    nb_fprintf(out, "    add x13, x13, :lo12:type_%s_tail\n", tname);

    // Paso 1: desenlazar x19 de donde este ahora
    nb_fprintf(out, "    ldr x10, [x9]\n");
    nb_fprintf(out, "    mov x11, #0\n");
    nb_fprintf(out, ".Lins_scan1_%d:\n", l);
    nb_fprintf(out, "    cmp x10, x19\n");
    nb_fprintf(out, "    beq .Lins_found1_%d\n", l);
    nb_fprintf(out, "    cbz x10, .Lins_relink_%d\n", l);
    nb_fprintf(out, "    mov x11, x10\n");
    nb_fprintf(out, "    ldr x10, [x10]\n");
    nb_fprintf(out, "    b .Lins_scan1_%d\n", l);
    nb_fprintf(out, ".Lins_found1_%d:\n", l);
    nb_fprintf(out, "    ldr x12, [x10]\n");
    nb_fprintf(out, "    cbz x11, .Lins_was_head1_%d\n", l);
    nb_fprintf(out, "    str x12, [x11]\n");
    nb_fprintf(out, "    b .Lins_fix_tail1_%d\n", l);
    nb_fprintf(out, ".Lins_was_head1_%d:\n", l);
    nb_fprintf(out, "    str x12, [x9]\n");
    nb_fprintf(out, ".Lins_fix_tail1_%d:\n", l);
    nb_fprintf(out, "    ldr x14, [x13]\n");
    nb_fprintf(out, "    cmp x14, x19\n");
    nb_fprintf(out, "    bne .Lins_relink_%d\n", l);
    nb_fprintf(out, "    str x11, [x13]\n");

    // Paso 2: reenlazar x19 antes o despues de x20
    nb_fprintf(out, ".Lins_relink_%d:\n", l);
    if (n->op == T_KW_AFTER) {
        nb_fprintf(out, "    ldr x21, [x20]\n");
        nb_fprintf(out, "    str x21, [x19]\n");
        nb_fprintf(out, "    str x19, [x20]\n");
        nb_fprintf(out, "    cbnz x21, .Lins_done_%d\n", l);
        nb_fprintf(out, "    str x19, [x13]\n");
    } else { // T_KW_BEFORE
        nb_fprintf(out, "    ldr x10, [x9]\n");
        nb_fprintf(out, "    mov x11, #0\n");
        nb_fprintf(out, ".Lins_scan2_%d:\n", l);
        nb_fprintf(out, "    cmp x10, x20\n");
        nb_fprintf(out, "    beq .Lins_found2_%d\n", l);
        nb_fprintf(out, "    cbz x10, .Lins_done_%d\n", l);
        nb_fprintf(out, "    mov x11, x10\n");
        nb_fprintf(out, "    ldr x10, [x10]\n");
        nb_fprintf(out, "    b .Lins_scan2_%d\n", l);
        nb_fprintf(out, ".Lins_found2_%d:\n", l);
        nb_fprintf(out, "    str x20, [x19]\n");
        nb_fprintf(out, "    cbz x11, .Lins_was_head2_%d\n", l);
        nb_fprintf(out, "    str x19, [x11]\n");
        nb_fprintf(out, "    b .Lins_done_%d\n", l);
        nb_fprintf(out, ".Lins_was_head2_%d:\n", l);
        nb_fprintf(out, "    str x19, [x9]\n");
    }
    nb_fprintf(out, ".Lins_done_%d:\n", l);
}

static void emit_foreach(FuncScope *sc, Node *n) {
    const char *tname = canon_type_name(n->a->text);
    int l = new_label();
    push_loop(l, "foreach");

    emit_var_address(sc, n->text);
    nb_fprintf(out, "    adrp x9, type_%s_head\n", tname);
    nb_fprintf(out, "    add x9, x9, :lo12:type_%s_head\n", tname);
    nb_fprintf(out, "    ldr x9, [x9]\n");
    nb_fprintf(out, "    str x9, [x0]\n");

    nb_fprintf(out, ".Lforeach_%d:\n", l);
    emit_var_address(sc, n->text);
    nb_fprintf(out, "    ldr x1, [x0]\n");
    nb_fprintf(out, "    cmp x1, #0\n");
    nb_fprintf(out, "    beq .Lforeach_end_%d\n", l);

    emit_block(sc, n->b);

    emit_var_address(sc, n->text);
    nb_fprintf(out, "    ldr x1, [x0]\n");
    nb_fprintf(out, "    ldr x1, [x1]\n");
    nb_fprintf(out, "    str x1, [x0]\n");
    nb_fprintf(out, "    b .Lforeach_%d\n", l);
    nb_fprintf(out, ".Lforeach_end_%d:\n", l);
    pop_loop();
}

static void emit_if(FuncScope *sc, Node *n) {
    int l = new_label();
    emit_expr(sc, n->a);
    nb_fprintf(out, "    cmp x0, #0\n");
    nb_fprintf(out, "    beq .Lelse_%d\n", l);
    emit_block(sc, n->b);
    nb_fprintf(out, "    b .Lend_%d\n", l);
    nb_fprintf(out, ".Lelse_%d:\n", l);

    for (int i = 0; i < n->list_count; i++) {
        Node *ei = n->list[i];
        int el = new_label();
        emit_expr(sc, ei->a);
        nb_fprintf(out, "    cmp x0, #0\n");
        nb_fprintf(out, "    beq .Lelseif_%d\n", el);
        emit_block(sc, ei->b);
        nb_fprintf(out, "    b .Lend_%d\n", l);
        nb_fprintf(out, ".Lelseif_%d:\n", el);
    }
    if (n->c) emit_block(sc, n->c);
    nb_fprintf(out, ".Lend_%d:\n", l);
}

static void emit_for(FuncScope *sc, Node *n) {
    int l = new_label();
    push_loop(l, "for");
    emit_var_address(sc, n->text);
    push_x0(); // direccion de la variable del bucle, la necesitaremos varias veces
    emit_expr(sc, n->a); // valor inicial
    nb_fprintf(out, "    ldr x1, [sp]\n");
    nb_fprintf(out, "    str x0, [x1]\n");

    nb_fprintf(out, ".Lfor_%d:\n", l);
    emit_expr(sc, n->b); // limite "To"
    push_x0();
    nb_fprintf(out, "    ldr x1, [sp, #16]\n"); // direccion de la variable (esta 16 bytes mas abajo)
    nb_fprintf(out, "    ldr x0, [x1]\n");
    nb_fprintf(out, "    ldr x2, [sp], #16\n"); // limite
    nb_fprintf(out, "    cmp x0, x2\n");
    nb_fprintf(out, "    bgt .Lfor_end_%d\n", l); // v1: solo pasos positivos (step<=0 no soportado aun)

    emit_block(sc, n->d);

    nb_fprintf(out, "    ldr x1, [sp]\n");
    nb_fprintf(out, "    ldr x0, [x1]\n");
    if (n->c) {
        emit_expr(sc, n->c);
        nb_fprintf(out, "    ldr x1, [sp]\n");
        nb_fprintf(out, "    ldr x2, [x1]\n");
        nb_fprintf(out, "    add x2, x2, x0\n");
        nb_fprintf(out, "    str x2, [x1]\n");
    } else {
        nb_fprintf(out, "    add x0, x0, #1\n");
        nb_fprintf(out, "    str x0, [x1]\n");
    }
    nb_fprintf(out, "    b .Lfor_%d\n", l);
    nb_fprintf(out, ".Lfor_end_%d:\n", l);
    nb_fprintf(out, "    add sp, sp, #16\n"); // liberamos la direccion que guardamos al principio
    pop_loop();
}

static void emit_while(FuncScope *sc, Node *n) {
    int l = new_label();
    push_loop(l, "while");
    nb_fprintf(out, ".Lwhile_%d:\n", l);
    emit_expr(sc, n->a);
    nb_fprintf(out, "    cmp x0, #0\n");
    nb_fprintf(out, "    beq .Lwhile_end_%d\n", l);
    emit_block(sc, n->b);
    nb_fprintf(out, "    b .Lwhile_%d\n", l);
    nb_fprintf(out, ".Lwhile_end_%d:\n", l);
    pop_loop();
}

static void emit_repeat(FuncScope *sc, Node *n) {
    int l = new_label();
    push_loop(l, "repeat");
    nb_fprintf(out, ".Lrepeat_%d:\n", l);
    emit_block(sc, n->a);
    if (n->b) {
        emit_expr(sc, n->b);
        nb_fprintf(out, "    cmp x0, #0\n");
        nb_fprintf(out, "    beq .Lrepeat_%d\n", l);
    } else {
        nb_fprintf(out, "    b .Lrepeat_%d\n", l); // Forever
    }
    nb_fprintf(out, ".Lrepeat_end_%d:\n", l);
    pop_loop();
}

static void emit_print(FuncScope *sc, Node *n) {
    for (int i = 0; i < n->list_count; i++) {
        ValType t = emit_expr(sc, n->list[i]);
        if (t == TY_INT) nb_fprintf(out, "    bl rt_int_to_str\n");
        else if (t == TY_FLOAT) nb_fprintf(out, "    bl rt_float_to_str\n");
        nb_fprintf(out, "    mov x8, #%d\n", SYS_WRITE_STRING);
        nb_fprintf(out, "    svc #0\n");
    }
    const char *nl = intern_string("\n");
    nb_fprintf(out, "    adrp x0, %s\n    add x0, x0, :lo12:%s\n", nl, nl);
    nb_fprintf(out, "    mov x8, #%d\n    svc #0\n", SYS_WRITE_STRING);
}

// Cls/Plot/Rect se apoyan directamente en SYS_DRAW_RECT (Cls rellena
// toda la ventana; Plot es un rectangulo de 1x1). Line usa una
// rutina auxiliar (rt_draw_line) porque no tenemos una syscall de
// linea todavia -- ver nota en el mensaje de esta ronda.
static void emit_draw_rect_call(FuncScope *sc, Node *xn, Node *yn, Node *wn, Node *hn, Node *colorn) {
    emit_expr(sc, xn); push_x0();
    emit_expr(sc, yn); push_x0();
    emit_expr(sc, wn); push_x0();
    emit_expr(sc, hn); push_x0();
    if (colorn) emit_expr(sc, colorn);
    else nb_fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n    ldr x0, [x9]\n");
    nb_fprintf(out, "    mov x4, x0\n");
    nb_fprintf(out, "    ldr x3, [sp], #16\n");
    nb_fprintf(out, "    ldr x2, [sp], #16\n");
    nb_fprintf(out, "    ldr x1, [sp], #16\n");
    nb_fprintf(out, "    ldr x0, [sp], #16\n");
    nb_fprintf(out, "    mov x8, #%d\n", SYS_DRAW_RECT);
    nb_fprintf(out, "    svc #0\n");
}

static void emit_stmt(FuncScope *sc, Node *n) {
    switch (n->kind) {
        case N_ASSIGN: emit_assign(sc, n); return;
        case N_IF: emit_if(sc, n); return;
        case N_FOR: emit_for(sc, n); return;
        case N_WHILE: emit_while(sc, n); return;
        case N_REPEAT: emit_repeat(sc, n); return;
        case N_PRINT: emit_print(sc, n); return;
        case N_RETURN:
            if (g_in_user_function) {
                if (n->a) emit_expr(sc, n->a); else nb_fprintf(out, "    mov x0, #0\n");
                nb_fprintf(out, "    b .Lfunc_end_%s\n", g_current_func_name);
            } else {
                nb_fprintf(out, "    bl rt_gosub_return\n");
            }
            return;
        case N_LABEL:
            nb_fprintf(out, ".Luser_lbl_%s:\n", n->text);
            return;
        case N_GOTO:
            nb_fprintf(out, "    b .Luser_lbl_%s\n", n->text);
            return;
        case N_GOSUB: {
            int l = new_label();
            nb_fprintf(out, "    adrp x9, .Lgosub_ret_%d\n", l);
            nb_fprintf(out, "    add x9, x9, :lo12:.Lgosub_ret_%d\n", l);
            nb_fprintf(out, "    adrp x10, rt_gosub_sp\n    add x10, x10, :lo12:rt_gosub_sp\n    ldr x11, [x10]\n");
            nb_fprintf(out, "    adrp x12, rt_gosub_stack\n    add x12, x12, :lo12:rt_gosub_stack\n");
            nb_fprintf(out, "    lsl x13, x11, #3\n    add x13, x12, x13\n    str x9, [x13]\n");
            nb_fprintf(out, "    add x11, x11, #1\n    str x11, [x10]\n");
            nb_fprintf(out, "    b .Luser_lbl_%s\n", n->text);
            nb_fprintf(out, ".Lgosub_ret_%d:\n", l);
            return;
        }
        case N_EXPRSTMT: emit_expr(sc, n->a); return;
        case N_VARDECL:
            for (int i = 0; i < n->list_count; i++) {
                Node *item = n->list[i];
                if (item->b) {
                    emit_expr(sc, item->b); // valor -> x0
                    push_x0();
                    emit_var_address(sc, item->a->text); // direccion -> x0
                    pop_to_x1(); // x1 = valor, x0 = direccion
                    nb_fprintf(out, "    str x1, [x0]\n");
                }
            }
            return;
        case N_DIM: return; // el espacio ya se reservo en la fase de recoleccion
        case N_CLS: {
            nb_fprintf(out, "    mov x0, #0\n    mov x1, #0\n    mov x2, #4096\n    mov x3, #4096\n");
            nb_fprintf(out, "    adrp x9, rt_cls_color\n    add x9, x9, :lo12:rt_cls_color\n    ldr x4, [x9]\n");
            nb_fprintf(out, "    mov x8, #%d\n    svc #0\n", SYS_DRAW_RECT);
            return;
        }
        case N_PLOT: {
            Node one; nb_memset(&one, 0, sizeof(one)); one.kind = N_NUM; one.num_value = 1;
            emit_draw_rect_call(sc, n->list[0], n->list[1], &one, &one, n->list_count > 2 ? n->list[2] : NULL);
            return;
        }
        case N_RECT: {
            emit_draw_rect_call(sc, n->list[0], n->list[1], n->list[2], n->list[3],
                                 n->list_count > 4 ? n->list[4] : NULL);
            return;
        }
        case N_LINE: {
            emit_expr(sc, n->list[0]); push_x0();
            emit_expr(sc, n->list[1]); push_x0();
            emit_expr(sc, n->list[2]); push_x0();
            emit_expr(sc, n->list[3]); push_x0();
            if (n->list_count > 4) emit_expr(sc, n->list[4]);
            else nb_fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n    ldr x0, [x9]\n");
            nb_fprintf(out, "    mov x4, x0\n");
            nb_fprintf(out, "    ldr x3, [sp], #16\n");
            nb_fprintf(out, "    ldr x2, [sp], #16\n");
            nb_fprintf(out, "    ldr x1, [sp], #16\n");
            nb_fprintf(out, "    ldr x0, [sp], #16\n");
            nb_fprintf(out, "    bl rt_draw_line\n");
            return;
        }
        case N_DELAY: {
            // SYS_SLEEP (numero 1): a0=ticks (100 ticks = 1 segundo)
            emit_expr(sc, n->list[0]);
            nb_fprintf(out, "    mov x8, #1\n");
            nb_fprintf(out, "    svc #0\n");
            return;
        }
        case N_ENDPROGRAM:
            // Salta directo al epilogo de _start -- funciona igual
            // este 'End' este dentro de un bucle, un If, o donde sea.
            nb_fprintf(out, "    b .Lprogram_end\n");
            return;
        case N_BLOCK:
            // Un bloque puede aparecer como sentencia suelta -- lo usa
            // el desazucarado de Select/Case (variable temporal +
            // cadena de If/ElseIf, empaquetados juntos en un bloque).
            emit_block(sc, n);
            return;
        case N_EXIT:
            if (loop_depth == 0) {
                nb_fatal(n->line, "'Exit' fuera de un bucle", NULL);
            }
            nb_fprintf(out, "    b .L%s_end_%d\n", loop_stack[loop_depth - 1].prefix, loop_stack[loop_depth - 1].label);
            return;
        case N_DATA:      return;
        case N_DATALABEL:
            nb_fprintf(out, ".Luser_lbl_%s:\n", n->text);
            return;
        case N_READ:      emit_read(sc, n); return;
        case N_RESTORE:   emit_restore(n); return;
        case N_TYPEDEF:   return;
        case N_DELETE:    emit_delete(n); return;
        case N_INSERT:    emit_insert(n); return;
        case N_FOREACH:   emit_foreach(sc, n); return;
        default: {
            char kind_str[12];
            nb_itoa(n->kind, kind_str, sizeof(kind_str));
            nb_fatal(n->line, "sentencia no soportada (tipo de nodo)", kind_str);
        }
    }
}

static void emit_block(FuncScope *sc, Node *block) {
    for (int i = 0; i < block->list_count; i++) emit_stmt(sc, block->list[i]);
}

// ---- rutinas de apoyo, incluidas una vez en todo programa compilado ----

static void emit_runtime_helpers(void) {
    nb_fprintf(out,
        "\n"
        "// Las dos rutinas de abajo comparten un unico 'pool' rotatorio\n"
        "// de 8 huecos de 128 bytes cada uno (1024 en total) para sus\n"
        "// resultados de texto -- huecos de tamaño FIJO a proposito,\n"
        "// para no tener que calcular avances de longitud variable (mas\n"
        "// facil de verificar a mano sin poder ensamblar y probar aqui\n"
        "// mismo). 128 bytes de sobra para cualquier entero de 64 bits\n"
        "// o concatenacion corta tipica.\n"
        "\n"
        "// rt_int_to_str: convierte el entero de x0 a texto decimal,\n"
        "// devuelve el puntero en x0.\n"
        "rt_int_to_str:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    adrp x9, rt_str_pos\n"
        "    add x9, x9, :lo12:rt_str_pos\n"
        "    ldr x10, [x9]\n"          // hueco actual (offset dentro del pool)
        "    adrp x11, rt_str_pool\n"
        "    add x11, x11, :lo12:rt_str_pool\n"
        "    add x11, x11, x10\n"      // x11 = inicio del hueco para este resultado
        "    add x10, x10, #128\n"
        "    and x10, x10, #1023\n"    // avanzamos al siguiente hueco, damos la vuelta a los 1024 bytes
        "    str x10, [x9]\n"
        "    mov x12, x11\n"           // x12 = puntero de escritura
        "    mov x13, #0\n"            // x13 = 1 si el numero es negativo
        "    cmp x0, #0\n"
        "    bge 1f\n"
        "    mov x13, #1\n"
        "    neg x0, x0\n"
        "1:\n"
        "    mov x14, #10\n"
        "2:\n"                          // escribimos los digitos en orden inverso (de derecha a izquierda)
        "    udiv x15, x0, x14\n"
        "    msub x16, x15, x14, x0\n"  // x16 = x0 % 10
        "    add x16, x16, #48\n"
        "    strb w16, [x12], #1\n"
        "    mov x0, x15\n"
        "    cbnz x0, 2b\n"
        "    cbz x13, 3f\n"
        "    mov w16, #45\n"            // '-'
        "    strb w16, [x12], #1\n"
        "3:\n"
        "    mov w16, #0\n"
        "    strb w16, [x12]\n"         // terminador nulo (todavia no lo contamos para invertir)
        "    mov x0, x11\n"             // inicio del resultado (aun invertido)
        "    sub x1, x12, #1\n"         // ultimo digito escrito
        "4:\n"                          // invertimos los digitos en su sitio
        "    cmp x0, x1\n"
        "    bge 5f\n"
        "    ldrb w2, [x0]\n"
        "    ldrb w3, [x1]\n"
        "    strb w3, [x0], #1\n"
        "    strb w2, [x1], #-1\n"
        "    b 4b\n"
        "5:\n"
        "    mov x0, x11\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_float_to_str: x0=patron de bits de un double -- lo\n"
        "// convierte a texto decimal con hasta 6 cifras despues del\n"
        "// punto (recortando ceros sobrantes).\n"
        "rt_float_to_str:\n"
        "    stp x29, x30, [sp, #-48]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    stp x21, x22, [sp, #32]\n"
        "    mov x21, x0\n"
        "    fmov d0, x0\n"
        "    fcvtzs x0, d0\n"
        "    bl rt_int_to_str\n"
        "    mov x19, x0\n"
        "    fmov d0, x21\n"
        "    fabs d0, d0\n"
        "    fcvtzs x9, d0\n"
        "    scvtf d1, x9\n"
        "    fsub d0, d0, d1\n"
        "    adrp x9, rt_half\n"
        "    add x9, x9, :lo12:rt_half\n"
        "    ldr x9, [x9]\n"
        "    fmov d2, x9\n"
        "    mov x9, #1000000\n"
        "    scvtf d1, x9\n"
        "    fmul d0, d0, d1\n"
        "    fadd d0, d0, d2\n"
        "    fcvtzs x22, d0\n"
        "    adrp x9, rt_str_pos\n"
        "    add x9, x9, :lo12:rt_str_pos\n"
        "    ldr x10, [x9]\n"
        "    adrp x11, rt_str_pool\n"
        "    add x11, x11, :lo12:rt_str_pool\n"
        "    add x11, x11, x10\n"
        "    add x10, x10, #128\n"
        "    and x10, x10, #1023\n"
        "    str x10, [x9]\n"
        "    mov x12, x11\n"
        "67:\n"
        "    ldrb w13, [x19], #1\n"
        "    cbz w13, 68f\n"
        "    strb w13, [x12], #1\n"
        "    b 67b\n"
        "68:\n"
        "    mov w13, #46\n"
        "    strb w13, [x12], #1\n"
        "    mov x14, #100000\n"
        "69:\n"
        "    udiv x15, x22, x14\n"
        "    add x16, x15, #48\n"
        "    strb w16, [x12], #1\n"
        "    msub x22, x15, x14, x22\n"
        "    mov x17, #10\n"
        "    udiv x14, x14, x17\n"
        "    cbnz x14, 69b\n"
        "71:\n"
        "    ldrb w17, [x12, #-1]\n"
        "    cmp w17, #48\n"
        "    bne 72f\n"
        "    sub x12, x12, #1\n"
        "    b 71b\n"
        "72:\n"
        "    ldrb w17, [x12, #-1]\n"
        "    cmp w17, #46\n"
        "    bne 73f\n"
        "    sub x12, x12, #1\n"
        "73:\n"
        "    mov w17, #0\n"
        "    strb w17, [x12]\n"
        "    mov x0, x11\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x21, x22, [sp, #32]\n"
        "    ldp x29, x30, [sp], #48\n"
        "    ret\n"
        "\n"
        "// rt_str_concat: concatena la cadena de x1 con la de x0,\n"
        "// devuelve el puntero al resultado en x0. Usa registros\n"
        "// x9-x15 como bloc de notas (no hace falta guardarlos: esta\n"
        "// rutina no llama a nada mas por dentro, asi que el ABI no\n"
        "// exige preservarlos para quien nos llamo).\n"
        "rt_str_concat:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x1\n"             // x9 = puntero a la cadena 'a'
        "    mov x10, x0\n"            // x10 = puntero a la cadena 'b'
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"      // x13 = inicio del hueco de destino
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"    // siguiente hueco, damos la vuelta si hace falta
        "    str x12, [x11]\n"
        "    mov x14, x13\n"           // x14 = puntero de escritura
        "rt_concat_copy_a:\n"
        "    ldrb w15, [x9], #1\n"
        "    cbz w15, rt_concat_copy_a_done\n"
        "    strb w15, [x14], #1\n"
        "    b rt_concat_copy_a\n"
        "rt_concat_copy_a_done:\n"
        "rt_concat_copy_b:\n"
        "    ldrb w15, [x10], #1\n"
        "    strb w15, [x14], #1\n"    // copiamos tambien el terminador nulo de 'b'
        "    cbnz w15, rt_concat_copy_b\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_draw_line: dibuja una linea de (x0,x1) a (x2,x3) color x4,\n"
        "// interpolando el mayor de los dos ejes y llamando a\n"
        "// SYS_DRAW_RECT una vez por punto (sencillo, no el algoritmo\n"
        "// de Bresenham de verdad, pero correcto y facil de revisar).\n"
        "rt_draw_line:\n"
        "    stp x29, x30, [sp, #-64]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    stp x21, x22, [sp, #32]\n"
        "    stp x23, x24, [sp, #48]\n"
        "    mov x19, x0\n"           // x1s
        "    mov x20, x1\n"           // y1
        "    mov x21, x2\n"           // x2s
        "    mov x22, x3\n"           // y2
        "    mov x23, x4\n"           // color
        "    subs x24, x21, x19\n"    // dx
        "    cneg x24, x24, mi\n"     // abs(dx)
        "    subs x9, x22, x20\n"     // dy
        "    cneg x9, x9, mi\n"       // abs(dy)
        "    cmp x24, x9\n"
        "    bge .Lline_horiz\n"
        "    b .Lline_vert\n"
        ".Lline_horiz:\n"             // recorremos por X
        "    cmp x19, x21\n"
        "    ble .Lline_h_fwd\n"
        "    mov x9, x19\n    mov x19, x21\n    mov x21, x9\n"
        "    mov x9, x20\n    mov x20, x22\n    mov x22, x9\n"
        ".Lline_h_fwd:\n"
        "    subs x24, x21, x19\n"
        "    mov x9, #0\n"            // x9 = contador
        "6:\n"
        "    cmp x19, x21\n"
        "    bgt .Lline_done\n"
        "    cbz x24, .Lline_h_single\n"
        "    sub x10, x22, x20\n"     // dy total
        "    mul x10, x10, x9\n"
        "    sdiv x10, x10, x24\n"
        "    add x10, x20, x10\n"     // y interpolada
        "    b .Lline_h_plot\n"
        ".Lline_h_single:\n"
        "    mov x10, x20\n"
        ".Lline_h_plot:\n"
        "    mov x0, x19\n    mov x1, x10\n    mov x2, #1\n    mov x3, #1\n    mov x4, x23\n"
        "    mov x8, #30\n    svc #0\n"
        "    add x19, x19, #1\n"
        "    add x9, x9, #1\n"
        "    b 6b\n"
        ".Lline_vert:\n"              // recorremos por Y (simetrico al caso anterior)
        "    cmp x20, x22\n"
        "    ble .Lline_v_fwd\n"
        "    mov x9, x19\n    mov x19, x21\n    mov x21, x9\n"
        "    mov x9, x20\n    mov x20, x22\n    mov x22, x9\n"
        ".Lline_v_fwd:\n"
        "    subs x9, x22, x20\n"     // dy total (v1: reutilizamos x9 con otro sentido aqui)
        "    mov x11, #0\n"
        "7:\n"
        "    cmp x20, x22\n"
        "    bgt .Lline_done\n"
        "    cbz x9, .Lline_v_single\n"
        "    sub x10, x21, x19\n"
        "    mul x10, x10, x11\n"
        "    sdiv x10, x10, x9\n"
        "    add x10, x19, x10\n"
        "    b .Lline_v_plot\n"
        ".Lline_v_single:\n"
        "    mov x10, x19\n"
        ".Lline_v_plot:\n"
        "    mov x0, x10\n    mov x1, x20\n    mov x2, #1\n    mov x3, #1\n    mov x4, x23\n"
        "    mov x8, #30\n    svc #0\n"
        "    add x20, x20, #1\n"
        "    add x11, x11, #1\n"
        "    b 7b\n"
        ".Lline_done:\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x21, x22, [sp, #32]\n"
        "    ldp x23, x24, [sp, #48]\n"
        "    ldp x29, x30, [sp], #64\n"
        "    ret\n"
        "\n"
        "// -- funciones de cadenas: Len, Left$, Right$, Mid$, Upper$, Lower$, Chr$, Asc --\n"
        "\n"
        "// rt_strlen: longitud de la cadena de x0, en x0.\n"
        "rt_strlen:\n"
        "    mov x1, x0\n"
        "    mov x0, #0\n"
        "20:\n"
        "    ldrb w2, [x1], #1\n"
        "    cbz w2, 21f\n"
        "    add x0, x0, #1\n"
        "    b 20b\n"
        "21:\n"
        "    ret\n"
        "\n"
        "// rt_left: primeros x1 caracteres de la cadena x0 (o menos, si\n"
        "// es mas corta). Devuelve el puntero al resultado en x0.\n"
        "rt_left:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    mov x10, x1\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x14, x13\n"
        "22:\n"
        "    cbz x10, 23f\n"
        "    ldrb w15, [x9], #1\n"
        "    cbz w15, 23f\n"
        "    strb w15, [x14], #1\n"
        "    sub x10, x10, #1\n"
        "    b 22b\n"
        "23:\n"
        "    mov w15, #0\n"
        "    strb w15, [x14]\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_right: ultimos x1 caracteres de la cadena x0.\n"
        "rt_right:\n"
        "    stp x29, x30, [sp, #-32]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    mov x19, x0\n"
        "    mov x20, x1\n"
        "    bl rt_strlen\n"
        "    mov x9, x0\n"
        "    cmp x20, x9\n"
        "    bge 24f\n"
        "    sub x9, x9, x20\n"
        "    add x19, x19, x9\n"
        "24:\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x14, x13\n"
        "25:\n"
        "    ldrb w15, [x19], #1\n"
        "    strb w15, [x14], #1\n"
        "    cbnz w15, 25b\n"
        "    mov x0, x13\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x29, x30, [sp], #32\n"
        "    ret\n"
        "\n"
        "// rt_mid: subcadena de x0 empezando en x1 (base 1), longitud\n"
        "// x2 (o -1 para 'hasta el final').\n"
        "rt_mid:\n"
        "    stp x29, x30, [sp, #-32]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    mov x19, x0\n"
        "    sub x9, x1, #1\n"
        "    cmp x9, #0\n"
        "    bge 26f\n"
        "    mov x9, #0\n"
        "26:\n"
        "27:\n"
        "    cbz x9, 28f\n"
        "    ldrb w10, [x19]\n"
        "    cbz w10, 28f\n"
        "    add x19, x19, #1\n"
        "    sub x9, x9, #1\n"
        "    b 27b\n"
        "28:\n"
        "    mov x20, x2\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x14, x13\n"
        "29:\n"
        "    cmp x20, #0\n"
        "    beq 30f\n"
        "    ldrb w15, [x19], #1\n"
        "    cbz w15, 30f\n"
        "    strb w15, [x14], #1\n"
        "    sub x20, x20, #1\n"
        "    b 29b\n"
        "30:\n"
        "    mov w15, #0\n"
        "    strb w15, [x14]\n"
        "    mov x0, x13\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x29, x30, [sp], #32\n"
        "    ret\n"
        "\n"
        "// rt_upper / rt_lower: copia de x0 en mayusculas/minusculas.\n"
        "rt_upper:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x14, x13\n"
        "31:\n"
        "    ldrb w15, [x9], #1\n"
        "    cbz w15, 32f\n"
        "    cmp w15, #97\n"
        "    blt 33f\n"
        "    cmp w15, #122\n"
        "    bgt 33f\n"
        "    sub w15, w15, #32\n"
        "33:\n"
        "    strb w15, [x14], #1\n"
        "    b 31b\n"
        "32:\n"
        "    mov w15, #0\n"
        "    strb w15, [x14]\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "rt_lower:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x14, x13\n"
        "34:\n"
        "    ldrb w15, [x9], #1\n"
        "    cbz w15, 35f\n"
        "    cmp w15, #65\n"
        "    blt 36f\n"
        "    cmp w15, #90\n"
        "    bgt 36f\n"
        "    add w15, w15, #32\n"
        "36:\n"
        "    strb w15, [x14], #1\n"
        "    b 34b\n"
        "35:\n"
        "    mov w15, #0\n"
        "    strb w15, [x14]\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_trim:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x8, x0\n"
        "    mov x10, x8\n"
        "101:\n"
        "    ldrb w11, [x10]\n"
        "    cbz w11, 102f\n"
        "    cmp w11, #32\n"
        "    bgt 102f\n"
        "    add x10, x10, #1\n"
        "    b 101b\n"
        "102:\n"
        "    mov x12, x10\n"
        "103:\n"
        "    ldrb w11, [x12]\n"
        "    cbz w11, 105f\n"
        "    add x12, x12, #1\n"
        "    b 103b\n"
        "105:\n"
        "    cmp x12, x10\n"
        "    ble 106f\n"
        "    ldrb w11, [x12, #-1]\n"
        "    cmp w11, #32\n"
        "    bgt 106f\n"
        "    sub x12, x12, #1\n"
        "    b 105b\n"
        "106:\n"
        "    adrp x14, rt_str_pos\n"
        "    add x14, x14, :lo12:rt_str_pos\n"
        "    ldr x15, [x14]\n"
        "    adrp x16, rt_str_pool\n"
        "    add x16, x16, :lo12:rt_str_pool\n"
        "    add x16, x16, x15\n"
        "    add x15, x15, #128\n"
        "    and x15, x15, #1023\n"
        "    str x15, [x14]\n"
        "    mov x17, x16\n"
        "107:\n"
        "    cmp x10, x12\n"
        "    bge 108f\n"
        "    ldrb w11, [x10], #1\n"
        "    strb w11, [x17], #1\n"
        "    b 107b\n"
        "108:\n"
        "    mov w11, #0\n"
        "    strb w11, [x17]\n"
        "    mov x0, x16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_lset:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x8, x0\n"
        "    mov x10, x1\n"
        "    adrp x14, rt_str_pos\n"
        "    add x14, x14, :lo12:rt_str_pos\n"
        "    ldr x15, [x14]\n"
        "    adrp x16, rt_str_pool\n"
        "    add x16, x16, :lo12:rt_str_pool\n"
        "    add x16, x16, x15\n"
        "    add x15, x15, #128\n"
        "    and x15, x15, #1023\n"
        "    str x15, [x14]\n"
        "    mov x17, x16\n"
        "    mov x9, x8\n"
        "    mov x11, #0\n"
        "120:\n"
        "    cmp x11, x10\n"
        "    bge 123f\n"
        "    ldrb w12, [x9]\n"
        "    cbz w12, 121f\n"
        "    strb w12, [x17], #1\n"
        "    add x9, x9, #1\n"
        "    add x11, x11, #1\n"
        "    b 120b\n"
        "121:\n"
        "    cmp x11, x10\n"
        "    bge 123f\n"
        "    mov w12, #32\n"
        "    strb w12, [x17], #1\n"
        "    add x11, x11, #1\n"
        "    b 121b\n"
        "123:\n"
        "    mov w12, #0\n"
        "    strb w12, [x17]\n"
        "    mov x0, x16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_rset:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x8, x0\n"
        "    mov x10, x1\n"
        "    mov x11, x8\n"
        "110:\n"
        "    ldrb w12, [x11]\n"
        "    cbz w12, 111f\n"
        "    add x11, x11, #1\n"
        "    b 110b\n"
        "111:\n"
        "    sub x13, x11, x8\n"
        "    adrp x14, rt_str_pos\n"
        "    add x14, x14, :lo12:rt_str_pos\n"
        "    ldr x15, [x14]\n"
        "    adrp x16, rt_str_pool\n"
        "    add x16, x16, :lo12:rt_str_pool\n"
        "    add x16, x16, x15\n"
        "    add x15, x15, #128\n"
        "    and x15, x15, #1023\n"
        "    str x15, [x14]\n"
        "    mov x17, x16\n"
        "    cmp x13, x10\n"
        "    ble 113f\n"
        "    sub x9, x13, x10\n"
        "    add x9, x8, x9\n"
        "    mov x11, #0\n"
        "112:\n"
        "    cmp x11, x10\n"
        "    bge 119f\n"
        "    ldrb w12, [x9], #1\n"
        "    strb w12, [x17], #1\n"
        "    add x11, x11, #1\n"
        "    b 112b\n"
        "113:\n"
        "    sub x11, x10, x13\n"
        "114:\n"
        "    cbz x11, 115f\n"
        "    mov w12, #32\n"
        "    strb w12, [x17], #1\n"
        "    sub x11, x11, #1\n"
        "    b 114b\n"
        "115:\n"
        "    mov x9, x8\n"
        "116:\n"
        "    ldrb w12, [x9]\n"
        "    cbz w12, 119f\n"
        "    strb w12, [x17], #1\n"
        "    add x9, x9, #1\n"
        "    b 116b\n"
        "119:\n"
        "    mov w12, #0\n"
        "    strb w12, [x17]\n"
        "    mov x0, x16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_hex:\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x9, x0\n"
        "    mov x10, #7\n"
        "130:\n"
        "    and x14, x9, #15\n"
        "    cmp x14, #9\n"
        "    bgt 131f\n"
        "    add x14, x14, #48\n"
        "    b 132f\n"
        "131:\n"
        "    add x14, x14, #55\n"
        "132:\n"
        "    add x18, x13, x10\n"
        "    strb w14, [x18]\n"
        "    lsr x9, x9, #4\n"
        "    sub x10, x10, #1\n"
        "    cmp x10, #0\n"
        "    bge 130b\n"
        "    mov w14, #0\n"
        "    strb w14, [x13, #8]\n"
        "    mov x0, x13\n"
        "    ret\n"
        "\n"
        "rt_bin:\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x9, x0\n"
        "    mov x10, #31\n"
        "140:\n"
        "    and x14, x9, #1\n"
        "    add x14, x14, #48\n"
        "    add x18, x13, x10\n"
        "    strb w14, [x18]\n"
        "    lsr x9, x9, #1\n"
        "    sub x10, x10, #1\n"
        "    cmp x10, #0\n"
        "    bge 140b\n"
        "    mov w14, #0\n"
        "    strb w14, [x13, #32]\n"
        "    mov x0, x13\n"
        "    ret\n"
        "\n"
        "rt_string_repeat:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x8, x0\n"
        "    mov x10, x1\n"
        "    adrp x14, rt_str_pos\n"
        "    add x14, x14, :lo12:rt_str_pos\n"
        "    ldr x15, [x14]\n"
        "    adrp x16, rt_str_pool\n"
        "    add x16, x16, :lo12:rt_str_pool\n"
        "    add x16, x16, x15\n"
        "    add x15, x15, #128\n"
        "    and x15, x15, #1023\n"
        "    str x15, [x14]\n"
        "    mov x17, x16\n"
        "    mov x11, #0\n"
        "150:\n"
        "    cmp x11, x10\n"
        "    bge 153f\n"
        "    mov x9, x8\n"
        "151:\n"
        "    ldrb w12, [x9]\n"
        "    cbz w12, 152f\n"
        "    strb w12, [x17], #1\n"
        "    add x9, x9, #1\n"
        "    b 151b\n"
        "152:\n"
        "    add x11, x11, #1\n"
        "    b 150b\n"
        "153:\n"
        "    mov w12, #0\n"
        "    strb w12, [x17]\n"
        "    mov x0, x16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_chr: cadena de un solo caracter, codigo x0.\n"
        "rt_chr:\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    strb w0, [x13]\n"
        "    mov w9, #0\n"
        "    strb w9, [x13, #1]\n"
        "    mov x0, x13\n"
        "    ret\n"
        "\n"
        "// rt_asc: codigo del primer caracter de x0 (0 si esta vacia).\n"
        "rt_asc:\n"
        "    ldrb w0, [x0]\n"
        "    ret\n"
        "\n"
        "// -- funciones numericas: Abs, Sgn, Min, Max, Rnd --\n"
        "\n"
        "rt_abs:\n"
        "    cmp x0, #0\n"
        "    bge 37f\n"
        "    neg x0, x0\n"
        "37:\n"
        "    ret\n"
        "rt_sgn:\n"
        "    cmp x0, #0\n"
        "    beq 39f\n"
        "    bgt 38f\n"
        "    mov x0, #-1\n"
        "    ret\n"
        "38:\n"
        "    mov x0, #1\n"
        "    ret\n"
        "39:\n"
        "    mov x0, #0\n"
        "    ret\n"
        "rt_min:\n"
        "    cmp x0, x1\n"
        "    ble 40f\n"
        "    mov x0, x1\n"
        "40:\n"
        "    ret\n"
        "rt_max:\n"
        "    cmp x0, x1\n"
        "    bge 41f\n"
        "    mov x0, x1\n"
        "41:\n"
        "    ret\n"
        "\n"
        "// rt_rand: entero pseudoaleatorio, INCLUSIVO en [x0, x1] (o [x1,x0]\n"
        "// si vienen invertidos -- se intercambian). Generador\n"
        "// congruencial lineal sencillo (mismos coeficientes que la\n"
        "// libreria C clasica); la semilla se inicializa una vez en\n"
        "// _start con SYS_GET_TICKS, asi que cada ejecucion arranca\n"
        "// con una secuencia distinta.\n"
        "rt_rand:\n"
        "    cmp x1, x0\n"
        "    bge 42f\n"
        "    mov x2, x0\n"
        "    mov x0, x1\n"
        "    mov x1, x2\n"
        "42:\n"
        "    adrp x9, rt_rnd_seed\n"
        "    add x9, x9, :lo12:rt_rnd_seed\n"
        "    ldr x10, [x9]\n"
        "    mov x11, #1103515245\n"
        "    mul x10, x10, x11\n"
        "    mov x11, #12345\n"
        "    add x10, x10, x11\n"
        "    str x10, [x9]\n"
        "    lsr x12, x10, #16\n"
        "    sub x13, x1, x0\n"
        "    add x13, x13, #1\n"
        "    udiv x14, x12, x13\n"
        "    msub x15, x14, x13, x12\n"
        "    add x0, x0, x15\n"
        "    ret\n"
        "\n"
        "rt_rnd_float:\n"
        "    fmov d4, x0\n"
        "    fmov d5, x1\n"
        "    adrp x9, rt_rnd_seed\n"
        "    add x9, x9, :lo12:rt_rnd_seed\n"
        "    ldr x10, [x9]\n"
        "    mov x11, #1103515245\n"
        "    mul x10, x10, x11\n"
        "    mov x11, #12345\n"
        "    add x10, x10, x11\n"
        "    str x10, [x9]\n"
        "    lsr x12, x10, #16\n"
        "    and x12, x12, #0x7FFFFFFF\n"
        "    scvtf d0, x12\n"
        "    adrp x9, rt_const_2_31\n"
        "    add x9, x9, :lo12:rt_const_2_31\n"
        "    ldr x9, [x9]\n"
        "    fmov d1, x9\n"
        "    fdiv d0, d0, d1\n"
        "    fsub d2, d5, d4\n"
        "    fmul d0, d0, d2\n"
        "    fadd d0, d0, d4\n"
        "    fmov x0, d0\n"
        "    ret\n"
        "\n"
        "rt_seedrnd:\n"
        "    adrp x9, rt_rnd_seed\n"
        "    add x9, x9, :lo12:rt_rnd_seed\n"
        "    str x0, [x9]\n"
        "    ret\n"
        "\n"
        "rt_locate:\n"
        "    adrp x9, rt_locate_buf\n"
        "    add x9, x9, :lo12:rt_locate_buf\n"
        "    mov w10, #1\n"
        "    strb w10, [x9]\n"
        "    add x11, x0, #1\n"
        "    strb w11, [x9, #1]\n"
        "    add x12, x1, #1\n"
        "    strb w12, [x9, #2]\n"
        "    mov w13, #0\n"
        "    strb w13, [x9, #3]\n"
        "    mov x0, x9\n"
        "    mov x8, #11\n"
        "    svc #0\n"
        "    ret\n"
        "\n"
        "rt_waitkey:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "200:\n"
        "    mov x8, #14\n"
        "    svc #0\n"
        "    mov x8, #54\n"
        "    svc #0\n"
        "    cbz x0, 200b\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_waitmouse:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "201:\n"
        "    mov x8, #14\n"
        "    svc #0\n"
        "    mov x0, #1\n"
        "    mov x8, #56\n"
        "    svc #0\n"
        "    cbz x0, 202f\n"
        "    mov x0, #1\n"
        "    b 204f\n"
        "202:\n"
        "    mov x0, #2\n"
        "    mov x8, #56\n"
        "    svc #0\n"
        "    cbz x0, 205f\n"
        "    mov x0, #2\n"
        "    b 204f\n"
        "205:\n"
        "    mov x0, #3\n"
        "    mov x8, #56\n"
        "    svc #0\n"
        "    cbz x0, 203f\n"
        "    mov x0, #3\n"
        "    b 204f\n"
        "203:\n"
        "    b 201b\n"
        "204:\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_input:\n"
        "    stp x29, x30, [sp, #-32]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    cbz x0, 211f\n"
        "    mov x8, #11\n"
        "    svc #0\n"
        "211:\n"
        "    adrp x9, rt_str_pos\n"
        "    add x9, x9, :lo12:rt_str_pos\n"
        "    ldr x10, [x9]\n"
        "    adrp x11, rt_str_pool\n"
        "    add x11, x11, :lo12:rt_str_pool\n"
        "    add x11, x11, x10\n"
        "    add x10, x10, #128\n"
        "    and x10, x10, #1023\n"
        "    str x10, [x9]\n"
        "    mov x19, x11\n"
        "    mov x20, #0\n"
        "    mov w12, #0\n"
        "    strb w12, [x19]\n"
        "210:\n"
        "    mov x8, #13\n"
        "    svc #0\n"
        "    cmp x0, #0\n"
        "    blt 213f\n"
        "    cmp x0, #10\n"
        "    beq 213f\n"
        "    cmp x0, #8\n"
        "    beq 215f\n"
        "    cmp x20, #126\n"
        "    bge 210b\n"
        "    add x15, x19, x20\n"
        "    strb w0, [x15]\n"
        "    add x20, x20, #1\n"
        "    add x15, x19, x20\n"
        "    strb wzr, [x15]\n"
        "    adrp x13, rt_input_echo_buf\n"
        "    add x13, x13, :lo12:rt_input_echo_buf\n"
        "    strb w0, [x13]\n"
        "    mov w14, #0\n"
        "    strb w14, [x13, #1]\n"
        "    mov x0, x13\n"
        "    mov x8, #11\n"
        "    svc #0\n"
        "    b 210b\n"
        "215:\n"
        "    cbz x20, 210b\n"
        "    sub x20, x20, #1\n"
        "    add x15, x19, x20\n"
        "    strb wzr, [x15]\n"
        "    adrp x13, rt_input_echo_buf\n"
        "    add x13, x13, :lo12:rt_input_echo_buf\n"
        "    mov w14, #8\n"
        "    strb w14, [x13]\n"
        "    mov w14, #0\n"
        "    strb w14, [x13, #1]\n"
        "    mov x0, x13\n"
        "    mov x8, #11\n"
        "    svc #0\n"
        "    b 210b\n"
        "213:\n"
        "    adrp x13, rt_input_echo_buf\n"
        "    add x13, x13, :lo12:rt_input_echo_buf\n"
        "    mov w14, #10\n"
        "    strb w14, [x13]\n"
        "    mov w14, #0\n"
        "    strb w14, [x13, #1]\n"
        "    mov x0, x13\n"
        "    mov x8, #11\n"
        "    svc #0\n"
        "    mov x0, x19\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_rndseed:\n"
        "    adrp x9, rt_rnd_seed\n"
        "    add x9, x9, :lo12:rt_rnd_seed\n"
        "    ldr x0, [x9]\n"
        "    ret\n"
        "\n"
        "rt_tileimage:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #-16]!\n"
        "    stp x21, x22, [sp, #-16]!\n"
        "    stp x23, x24, [sp, #-16]!\n"
        "    stp x25, x26, [sp, #-16]!\n"
        "    stp x27, x28, [sp, #-16]!\n"
        "    mov x19, x0\n"
        "    mov x20, x1\n"
        "    mov x21, x2\n"
        "    lsl x28, x3, #31\n"
        "    mov x0, x19\n"
        "    mov x8, #51\n"
        "    svc #0\n"
        "    lsr x22, x0, #32\n"
        "    and x23, x0, #0xFFFFFFFF\n"
        "    cbz x22, 276f\n"
        "    cbz x23, 276f\n"
        "    mov x8, #33\n"
        "    svc #0\n"
        "    lsr x24, x0, #32\n"
        "    and x25, x0, #0xFFFFFFFF\n"
        "    sdiv x9, x20, x22\n"
        "    mul x10, x9, x22\n"
        "    sub x9, x20, x10\n"
        "    cmp x9, #0\n"
        "    bge 270f\n"
        "    add x9, x9, x22\n"
        "270:\n"
        "    sdiv x10, x21, x23\n"
        "    mul x11, x10, x23\n"
        "    sub x10, x21, x11\n"
        "    cmp x10, #0\n"
        "    bge 271f\n"
        "    add x10, x10, x23\n"
        "271:\n"
        "    neg x20, x9\n"
        "    neg x21, x10\n"
        "    mov x26, x21\n"
        "272:\n"
        "    cmp x26, x25\n"
        "    bge 275f\n"
        "    mov x27, x20\n"
        "273:\n"
        "    cmp x27, x24\n"
        "    bge 274f\n"
        "    mov x0, x19\n"
        "    mov x1, x27\n"
        "    mov x2, x26\n"
        "    mov x3, x28\n"
        "    mov x8, #50\n"
        "    svc #0\n"
        "    add x27, x27, x22\n"
        "    b 273b\n"
        "274:\n"
        "    add x26, x26, x23\n"
        "    b 272b\n"
        "275:\n"
        "276:\n"
        "    ldp x27, x28, [sp], #16\n"
        "    ldp x25, x26, [sp], #16\n"
        "    ldp x23, x24, [sp], #16\n"
        "    ldp x21, x22, [sp], #16\n"
        "    ldp x19, x20, [sp], #16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_currentdate:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x8, #134\n"
        "    svc #0\n"
        "    mov x9, x0\n"
        "    adrp x10, rt_str_pos\n"
        "    add x10, x10, :lo12:rt_str_pos\n"
        "    ldr x11, [x10]\n"
        "    adrp x12, rt_str_pool\n"
        "    add x12, x12, :lo12:rt_str_pool\n"
        "    add x12, x12, x11\n"
        "    add x11, x11, #128\n"
        "    and x11, x11, #1023\n"
        "    str x11, [x10]\n"
        "    lsr x0, x9, #32\n"
        "    and x0, x0, #0xFF\n"
        "    mov x13, #10\n"
        "    udiv x14, x0, x13\n"
        "    mul x15, x14, x13\n"
        "    sub x0, x0, x15\n"
        "    add x14, x14, #48\n"
        "    add x0, x0, #48\n"
        "    strb w14, [x12]\n"
        "    strb w0, [x12, #1]\n"
        "    mov w0, #45\n"
        "    strb w0, [x12, #2]\n"
        "    lsr x0, x9, #40\n"
        "    and x0, x0, #0xFF\n"
        "    udiv x14, x0, x13\n"
        "    mul x15, x14, x13\n"
        "    sub x0, x0, x15\n"
        "    add x14, x14, #48\n"
        "    add x0, x0, #48\n"
        "    strb w14, [x12, #3]\n"
        "    strb w0, [x12, #4]\n"
        "    mov w0, #45\n"
        "    strb w0, [x12, #5]\n"
        "    lsr x0, x9, #48\n"
        "    and x0, x0, #0xFFFF\n"
        "    mov x13, #1000\n"
        "    udiv x14, x0, x13\n"
        "    mul x15, x14, x13\n"
        "    sub x0, x0, x15\n"
        "    add x14, x14, #48\n"
        "    mov x13, #100\n"
        "    udiv x9, x0, x13\n"
        "    mul x15, x9, x13\n"
        "    sub x0, x0, x15\n"
        "    add x9, x9, #48\n"
        "    mov x13, #10\n"
        "    udiv x16, x0, x13\n"
        "    mul x15, x16, x13\n"
        "    sub x0, x0, x15\n"
        "    add x16, x16, #48\n"
        "    add x0, x0, #48\n"
        "    strb w14, [x12, #6]\n"
        "    strb w9, [x12, #7]\n"
        "    strb w16, [x12, #8]\n"
        "    strb w0, [x12, #9]\n"
        "    mov w0, #0\n"
        "    strb w0, [x12, #10]\n"
        "    mov x0, x12\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_currenttime:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x8, #134\n"
        "    svc #0\n"
        "    mov x9, x0\n"
        "    adrp x10, rt_str_pos\n"
        "    add x10, x10, :lo12:rt_str_pos\n"
        "    ldr x11, [x10]\n"
        "    adrp x12, rt_str_pool\n"
        "    add x12, x12, :lo12:rt_str_pool\n"
        "    add x12, x12, x11\n"
        "    add x11, x11, #128\n"
        "    and x11, x11, #1023\n"
        "    str x11, [x10]\n"
        "    lsr x0, x9, #24\n"
        "    and x0, x0, #0xFF\n"
        "    mov x13, #10\n"
        "    udiv x14, x0, x13\n"
        "    mul x15, x14, x13\n"
        "    sub x0, x0, x15\n"
        "    add x14, x14, #48\n"
        "    add x0, x0, #48\n"
        "    strb w14, [x12]\n"
        "    strb w0, [x12, #1]\n"
        "    mov w0, #58\n"
        "    strb w0, [x12, #2]\n"
        "    lsr x0, x9, #16\n"
        "    and x0, x0, #0xFF\n"
        "    udiv x14, x0, x13\n"
        "    mul x15, x14, x13\n"
        "    sub x0, x0, x15\n"
        "    add x14, x14, #48\n"
        "    add x0, x0, #48\n"
        "    strb w14, [x12, #3]\n"
        "    strb w0, [x12, #4]\n"
        "    mov w0, #58\n"
        "    strb w0, [x12, #5]\n"
        "    lsr x0, x9, #8\n"
        "    and x0, x0, #0xFF\n"
        "    udiv x14, x0, x13\n"
        "    mul x15, x14, x13\n"
        "    sub x0, x0, x15\n"
        "    add x14, x14, #48\n"
        "    add x0, x0, #48\n"
        "    strb w14, [x12, #6]\n"
        "    strb w0, [x12, #7]\n"
        "    mov w0, #0\n"
        "    strb w0, [x12, #8]\n"
        "    mov x0, x12\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_commandline:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    adrp x9, rt_str_pos\n"
        "    add x9, x9, :lo12:rt_str_pos\n"
        "    ldr x10, [x9]\n"
        "    adrp x11, rt_str_pool\n"
        "    add x11, x11, :lo12:rt_str_pool\n"
        "    add x11, x11, x10\n"
        "    add x10, x10, #128\n"
        "    and x10, x10, #1023\n"
        "    str x10, [x9]\n"
        "    mov x0, x11\n"
        "    mov x1, #126\n"
        "    mov x8, #6\n"
        "    svc #0\n"
        "    mov x0, x11\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_waittimer:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #-16]!\n"
        "    mov x19, x0\n"
        "280:\n"
        "    mov x8, #14\n"
        "    svc #0\n"
        "    mov x0, x19\n"
        "    mov x8, #132\n"
        "    svc #0\n"
        "    cbz x0, 280b\n"
        "    mov x0, x19\n"
        "    mov x8, #133\n"
        "    svc #0\n"
        "    ldp x19, x20, [sp], #16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_gosub_return:\n"
        "    adrp x10, rt_gosub_sp\n"
        "    add x10, x10, :lo12:rt_gosub_sp\n"
        "    ldr x11, [x10]\n"
        "    sub x11, x11, #1\n"
        "    str x11, [x10]\n"
        "    adrp x12, rt_gosub_stack\n"
        "    add x12, x12, :lo12:rt_gosub_stack\n"
        "    lsl x13, x11, #3\n"
        "    add x13, x12, x13\n"
        "    ldr x14, [x13]\n"
        "    br x14\n"
        "\n"
        "rt_changedir:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #-16]!\n"
        "    mov x19, x0\n"
        "    ldrb w10, [x19]\n"
        "    cmp w10, #46\n"
        "    bne 230f\n"
        "    ldrb w11, [x19, #1]\n"
        "    cmp w11, #46\n"
        "    bne 230f\n"
        "    ldrb w12, [x19, #2]\n"
        "    cbnz w12, 230f\n"
        "    mov x10, #0\n"
        "    adrp x11, rt_current_dir_inode\n"
        "    add x11, x11, :lo12:rt_current_dir_inode\n"
        "    str x10, [x11]\n"
        "    adrp x9, rt_current_dir_name\n"
        "    add x9, x9, :lo12:rt_current_dir_name\n"
        "    mov w10, #0\n"
        "    strb w10, [x9]\n"
        "    mov x0, #0\n"
        "    b 236f\n"
        "230:\n"
        "    adrp x11, rt_current_dir_inode\n"
        "    add x11, x11, :lo12:rt_current_dir_inode\n"
        "    ldr x1, [x11]\n"
        "    mov x0, x19\n"
        "    mov x8, #83\n"
        "    svc #0\n"
        "    cmp x0, #0\n"
        "    blt 235f\n"
        "    str x0, [x11]\n"
        "    adrp x9, rt_current_dir_name\n"
        "    add x9, x9, :lo12:rt_current_dir_name\n"
        "    mov x20, #0\n"
        "231:\n"
        "    add x13, x19, x20\n"
        "    ldrb w12, [x13]\n"
        "    cbz w12, 233f\n"
        "    cmp x20, #30\n"
        "    bge 233f\n"
        "    add x14, x9, x20\n"
        "    strb w12, [x14]\n"
        "    add x20, x20, #1\n"
        "    b 231b\n"
        "233:\n"
        "    add x14, x9, x20\n"
        "    mov w12, #0\n"
        "    strb w12, [x14]\n"
        "    mov x0, #0\n"
        "    b 236f\n"
        "235:\n"
        "    mov x0, #-1\n"
        "236:\n"
        "    ldp x19, x20, [sp], #16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_currentdir:\n"
        "    adrp x9, rt_str_pos\n"
        "    add x9, x9, :lo12:rt_str_pos\n"
        "    ldr x10, [x9]\n"
        "    adrp x11, rt_str_pool\n"
        "    add x11, x11, :lo12:rt_str_pool\n"
        "    add x11, x11, x10\n"
        "    add x10, x10, #128\n"
        "    and x10, x10, #1023\n"
        "    str x10, [x9]\n"
        "    mov w12, #47\n"
        "    strb w12, [x11]\n"
        "    adrp x13, rt_current_dir_name\n"
        "    add x13, x13, :lo12:rt_current_dir_name\n"
        "    mov x14, #0\n"
        "237:\n"
        "    add x15, x13, x14\n"
        "    ldrb w12, [x15]\n"
        "    cbz w12, 238f\n"
        "    add x17, x14, #1\n"
        "    add x16, x11, x17\n"
        "    strb w12, [x16]\n"
        "    add x14, x14, #1\n"
        "    b 237b\n"
        "238:\n"
        "    add x17, x14, #1\n"
        "    add x16, x11, x17\n"
        "    mov w12, #0\n"
        "    strb w12, [x16]\n"
        "    mov x0, x11\n"
        "    ret\n"
        "\n"
        "rt_readdir:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    ldrb w9, [x0]\n"
        "    cbnz w9, 240f\n"
        "    adrp x9, rt_current_dir_inode\n"
        "    add x9, x9, :lo12:rt_current_dir_inode\n"
        "    ldr x0, [x9]\n"
        "    b 242f\n"
        "240:\n"
        "    adrp x9, rt_current_dir_inode\n"
        "    add x9, x9, :lo12:rt_current_dir_inode\n"
        "    ldr x1, [x9]\n"
        "    mov x8, #83\n"
        "    svc #0\n"
        "    cmp x0, #0\n"
        "    bge 242f\n"
        "    mov x0, #-1\n"
        "    b 243f\n"
        "242:\n"
        "    mov x8, #78\n"
        "    svc #0\n"
        "243:\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_nextfile:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    adrp x10, rt_str_pos\n"
        "    add x10, x10, :lo12:rt_str_pos\n"
        "    ldr x11, [x10]\n"
        "    adrp x12, rt_str_pool\n"
        "    add x12, x12, :lo12:rt_str_pool\n"
        "    add x12, x12, x11\n"
        "    add x11, x11, #128\n"
        "    and x11, x11, #1023\n"
        "    str x11, [x10]\n"
        "    mov x0, x9\n"
        "    mov x1, x12\n"
        "    mov x2, #64\n"
        "    mov x8, #79\n"
        "    svc #0\n"
        "    mov x0, x12\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_writeline:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #-16]!\n"
        "    sub x19, x0, #100\n"
        "    mov x20, x1\n"
        "250:\n"
        "    ldrb w9, [x20]\n"
        "    cbz w9, 251f\n"
        "    adrp x10, rt_file_io_buf\n"
        "    add x10, x10, :lo12:rt_file_io_buf\n"
        "    strb w9, [x10]\n"
        "    mov x0, x19\n"
        "    mov x1, x10\n"
        "    mov x2, #1\n"
        "    mov x8, #72\n"
        "    svc #0\n"
        "    add x20, x20, #1\n"
        "    b 250b\n"
        "251:\n"
        "    adrp x10, rt_file_io_buf\n"
        "    add x10, x10, :lo12:rt_file_io_buf\n"
        "    mov w9, #10\n"
        "    strb w9, [x10]\n"
        "    mov x0, x19\n"
        "    mov x1, x10\n"
        "    mov x2, #1\n"
        "    mov x8, #72\n"
        "    svc #0\n"
        "    ldp x19, x20, [sp], #16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_writestring:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #-16]!\n"
        "    sub x19, x0, #100\n"
        "    mov x20, x1\n"
        "    mov x9, x20\n"
        "252:\n"
        "    ldrb w10, [x9]\n"
        "    cbz w10, 253f\n"
        "    add x9, x9, #1\n"
        "    b 252b\n"
        "253:\n"
        "    sub x9, x9, x20\n"
        "    adrp x10, rt_file_io_buf\n"
        "    add x10, x10, :lo12:rt_file_io_buf\n"
        "    mov x11, x9\n"
        "    strb w11, [x10]\n"
        "    lsr x11, x11, #8\n"
        "    strb w11, [x10, #1]\n"
        "    lsr x11, x11, #8\n"
        "    strb w11, [x10, #2]\n"
        "    lsr x11, x11, #8\n"
        "    strb w11, [x10, #3]\n"
        "    mov x0, x19\n"
        "    mov x1, x10\n"
        "    mov x2, #4\n"
        "    mov x8, #72\n"
        "    svc #0\n"
        "    mov x12, #0\n"
        "254:\n"
        "    cmp x12, x9\n"
        "    bge 255f\n"
        "    add x13, x20, x12\n"
        "    ldrb w14, [x13]\n"
        "    strb w14, [x10]\n"
        "    mov x0, x19\n"
        "    mov x1, x10\n"
        "    mov x2, #1\n"
        "    mov x8, #72\n"
        "    svc #0\n"
        "    add x12, x12, #1\n"
        "    b 254b\n"
        "255:\n"
        "    ldp x19, x20, [sp], #16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_readstring:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #-16]!\n"
        "    stp x21, x22, [sp, #-16]!\n"
        "    cmp x0, #100\n"
        "    blt 290f\n"
        "    sub x19, x0, #100\n"
        "    mov x21, #71\n"
        "    b 291f\n"
        "290:\n"
        "    mov x19, x0\n"
        "    mov x21, #136\n"
        "291:\n"
        "    adrp x10, rt_file_io_buf\n"
        "    add x10, x10, :lo12:rt_file_io_buf\n"
        "    mov x0, x19\n"
        "    mov x1, x10\n"
        "    mov x2, #4\n"
        "    mov x8, x21\n"
        "    svc #0\n"
        "    ldrb w9, [x10, #3]\n"
        "    lsl x9, x9, #8\n"
        "    ldrb w11, [x10, #2]\n"
        "    orr x9, x9, x11\n"
        "    lsl x9, x9, #8\n"
        "    ldrb w11, [x10, #1]\n"
        "    orr x9, x9, x11\n"
        "    lsl x9, x9, #8\n"
        "    ldrb w11, [x10]\n"
        "    orr x9, x9, x11\n"
        "    adrp x14, rt_str_pos\n"
        "    add x14, x14, :lo12:rt_str_pos\n"
        "    ldr x15, [x14]\n"
        "    adrp x16, rt_str_pool\n"
        "    add x16, x16, :lo12:rt_str_pool\n"
        "    add x16, x16, x15\n"
        "    add x15, x15, #128\n"
        "    and x15, x15, #1023\n"
        "    str x15, [x14]\n"
        "    mov x20, x16\n"
        "    cmp x9, #126\n"
        "    ble 256f\n"
        "    mov x9, #126\n"
        "256:\n"
        "    mov x12, #0\n"
        "257:\n"
        "    cmp x12, x9\n"
        "    bge 258f\n"
        "    mov x0, x19\n"
        "    adrp x10, rt_file_io_buf\n"
        "    add x10, x10, :lo12:rt_file_io_buf\n"
        "    mov x1, x10\n"
        "    mov x2, #1\n"
        "    mov x8, x21\n"
        "    svc #0\n"
        "    ldrb w13, [x10]\n"
        "    add x14, x20, x12\n"
        "    strb w13, [x14]\n"
        "    add x12, x12, #1\n"
        "    b 257b\n"
        "258:\n"
        "    add x14, x20, x12\n"
        "    mov w13, #0\n"
        "    strb w13, [x14]\n"
        "    mov x0, x20\n"
        "    ldp x21, x22, [sp], #16\n"
        "    ldp x19, x20, [sp], #16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_copyfile:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #-16]!\n"
        "    mov x19, x0\n"
        "    mov x20, x1\n"
        "    mov x0, x19\n"
        "    mov x1, #0\n"
        "    mov x8, #70\n"
        "    svc #0\n"
        "    cmp x0, #0\n"
        "    blt 262f\n"
        "    mov x19, x0\n"
        "    mov x0, x20\n"
        "    mov x1, #1\n"
        "    mov x8, #70\n"
        "    svc #0\n"
        "    cmp x0, #0\n"
        "    blt 263f\n"
        "    mov x20, x0\n"
        "260:\n"
        "    mov x0, x19\n"
        "    sub x0, x0, #100\n"
        "    adrp x9, rt_copyfile_buf\n"
        "    add x9, x9, :lo12:rt_copyfile_buf\n"
        "    mov x1, x9\n"
        "    mov x2, #1024\n"
        "    mov x8, #71\n"
        "    svc #0\n"
        "    cbz x0, 261f\n"
        "    mov x2, x0\n"
        "    mov x0, x20\n"
        "    sub x0, x0, #100\n"
        "    mov x1, x9\n"
        "    mov x8, #72\n"
        "    svc #0\n"
        "    b 260b\n"
        "261:\n"
        "    mov x0, x20\n"
        "    sub x0, x0, #100\n"
        "    mov x8, #77\n"
        "    svc #0\n"
        "    mov x0, x19\n"
        "    sub x0, x0, #100\n"
        "    mov x8, #77\n"
        "    svc #0\n"
        "    mov x0, #0\n"
        "    b 264f\n"
        "263:\n"
        "    mov x0, x19\n"
        "    sub x0, x0, #100\n"
        "    mov x8, #77\n"
        "    svc #0\n"
        "262:\n"
        "    mov x0, #-1\n"
        "264:\n"
        "    ldp x19, x20, [sp], #16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_exp:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    fmov d0, x0\n"
        "    adrp x9, rt_const_ln2\n    add x9, x9, :lo12:rt_const_ln2\n    ldr x9, [x9]\n    fmov d1, x9\n"
        "    fdiv d2, d0, d1\n"
        "    fcvtzs x10, d2\n"
        "    scvtf d3, x10\n"
        "    fmul d4, d3, d1\n"
        "    fsub d5, d0, d4\n"
        "    adrp x9, rt_exp_c9\n    add x9, x9, :lo12:rt_exp_c9\n    ldr x9, [x9]\n    fmov d6, x9\n"
        "    adrp x9, rt_exp_c8\n    add x9, x9, :lo12:rt_exp_c8\n    ldr x9, [x9]\n    fmov d7, x9\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    adrp x9, rt_exp_c7\n    add x9, x9, :lo12:rt_exp_c7\n    ldr x9, [x9]\n    fmov d7, x9\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    adrp x9, rt_exp_c6\n    add x9, x9, :lo12:rt_exp_c6\n    ldr x9, [x9]\n    fmov d7, x9\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    adrp x9, rt_exp_c5\n    add x9, x9, :lo12:rt_exp_c5\n    ldr x9, [x9]\n    fmov d7, x9\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    adrp x9, rt_exp_c4\n    add x9, x9, :lo12:rt_exp_c4\n    ldr x9, [x9]\n    fmov d7, x9\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    adrp x9, rt_exp_c3\n    add x9, x9, :lo12:rt_exp_c3\n    ldr x9, [x9]\n    fmov d7, x9\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    adrp x9, rt_exp_c2\n    add x9, x9, :lo12:rt_exp_c2\n    ldr x9, [x9]\n    fmov d7, x9\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    adrp x9, rt_exp_c1\n    add x9, x9, :lo12:rt_exp_c1\n    ldr x9, [x9]\n    fmov d7, x9\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    adrp x9, rt_exp_c0\n    add x9, x9, :lo12:rt_exp_c0\n    ldr x9, [x9]\n    fmov d7, x9\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    add x11, x10, #1023\n"
        "    lsl x11, x11, #52\n"
        "    fmov d2, x11\n"
        "    fmul d0, d6, d2\n"
        "    fmov x0, d0\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_log:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    lsr x10, x9, #52\n"
        "    and x10, x10, #0x7FF\n"
        "    sub x11, x10, #1023\n"
        "    mov x12, #1023\n"
        "    lsl x12, x12, #52\n"
        "    mov x13, #0x7FF\n"
        "    lsl x13, x13, #52\n"
        "    mov x16, #0\n"
        "    sub x16, x16, #1\n"
        "    eor x13, x13, x16\n"
        "    and x14, x9, x13\n"
        "    orr x14, x14, x12\n"
        "    fmov d0, x14\n"
        "    adrp x15, rt_exp_c0\n    add x15, x15, :lo12:rt_exp_c0\n    ldr x15, [x15]\n    fmov d1, x15\n"
        "    fsub d2, d0, d1\n"
        "    fadd d3, d0, d1\n"
        "    fdiv d4, d2, d3\n"
        "    fmul d5, d4, d4\n"
        "    adrp x15, rt_log_c6\n    add x15, x15, :lo12:rt_log_c6\n    ldr x15, [x15]\n    fmov d6, x15\n"
        "    adrp x15, rt_log_c5\n    add x15, x15, :lo12:rt_log_c5\n    ldr x15, [x15]\n    fmov d7, x15\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    adrp x15, rt_log_c4\n    add x15, x15, :lo12:rt_log_c4\n    ldr x15, [x15]\n    fmov d7, x15\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    adrp x15, rt_log_c3\n    add x15, x15, :lo12:rt_log_c3\n    ldr x15, [x15]\n    fmov d7, x15\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    adrp x15, rt_log_c2\n    add x15, x15, :lo12:rt_log_c2\n    ldr x15, [x15]\n    fmov d7, x15\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    adrp x15, rt_log_c1\n    add x15, x15, :lo12:rt_log_c1\n    ldr x15, [x15]\n    fmov d7, x15\n"
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n"
        "    fmul d6, d6, d4\n"
        "    fadd d6, d6, d6\n"
        "    scvtf d2, x11\n"
        "    adrp x15, rt_const_ln2\n    add x15, x15, :lo12:rt_const_ln2\n    ldr x15, [x15]\n    fmov d3, x15\n"
        "    fmul d2, d2, d3\n"
        "    fadd d0, d6, d2\n"
        "    fmov x0, d0\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_log10:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    bl rt_log\n"
        "    fmov d0, x0\n"
        "    adrp x9, rt_const_log10_recip\n    add x9, x9, :lo12:rt_const_log10_recip\n    ldr x9, [x9]\n    fmov d1, x9\n"
        "    fmul d0, d0, d1\n"
        "    fmov x0, d0\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_atan_core:\n"
        "    fmov d0, x0\n"
        "    fmul d1, d0, d0\n"
        "    adrp x9, rt_atan_c0\n    add x9, x9, :lo12:rt_atan_c0\n    ldr x9, [x9]\n    fmov d2, x9\n"
        "    fadd d1, d1, d2\n"
        "    fsqrt d1, d1\n"
        "    fadd d1, d1, d2\n"
        "    fdiv d5, d0, d1\n"
        "    fmul d6, d5, d5\n"
        "    adrp x9, rt_atan_c5\n    add x9, x9, :lo12:rt_atan_c5\n    ldr x9, [x9]\n    fmov d7, x9\n"
        "    adrp x9, rt_atan_c4\n    add x9, x9, :lo12:rt_atan_c4\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d7, d7, d6\n    fadd d7, d7, d3\n"
        "    adrp x9, rt_atan_c3\n    add x9, x9, :lo12:rt_atan_c3\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d7, d7, d6\n    fadd d7, d7, d3\n"
        "    adrp x9, rt_atan_c2\n    add x9, x9, :lo12:rt_atan_c2\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d7, d7, d6\n    fadd d7, d7, d3\n"
        "    adrp x9, rt_atan_c1\n    add x9, x9, :lo12:rt_atan_c1\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d7, d7, d6\n    fadd d7, d7, d3\n"
        "    adrp x9, rt_atan_c0\n    add x9, x9, :lo12:rt_atan_c0\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d7, d7, d6\n    fadd d7, d7, d3\n"
        "    fmul d0, d7, d5\n"
        "    fadd d0, d0, d0\n"
        "    fmov x0, d0\n"
        "    ret\n"
        "\n"
        "rt_atan:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    fmov d0, x0\n"
        "    fabs d1, d0\n"
        "    adrp x9, rt_atan_c0\n    add x9, x9, :lo12:rt_atan_c0\n    ldr x9, [x9]\n    fmov d2, x9\n"
        "    fcmp d1, d2\n"
        "    ble 90f\n"
        "    fmov x10, d0\n"
        "    lsr x11, x10, #63\n"
        "    adrp x9, rt_const_pi\n    add x9, x9, :lo12:rt_const_pi\n    ldr x9, [x9]\n    fmov d8, x9\n"
        "    adrp x9, rt_half\n    add x9, x9, :lo12:rt_half\n    ldr x9, [x9]\n    fmov d4, x9\n"
        "    fmul d8, d8, d4\n"
        "    cbz x11, 91f\n"
        "    fneg d8, d8\n"
        "91:\n"
        "    fdiv d0, d2, d0\n"
        "    fmov x0, d0\n"
        "    bl rt_atan_core\n"
        "    fmov d7, x0\n"
        "    fsub d7, d8, d7\n"
        "    b 92f\n"
        "90:\n"
        "    bl rt_atan_core\n"
        "    fmov d7, x0\n"
        "92:\n"
        "    adrp x9, rt_const_rad2deg\n    add x9, x9, :lo12:rt_const_rad2deg\n    ldr x9, [x9]\n    fmov d9, x9\n"
        "    fmul d7, d7, d9\n"
        "    fmov x0, d7\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_asin:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    fmov d0, x0\n"
        "    fmul d1, d0, d0\n"
        "    adrp x9, rt_atan_c0\n    add x9, x9, :lo12:rt_atan_c0\n    ldr x9, [x9]\n    fmov d2, x9\n"
        "    fsub d1, d2, d1\n"
        "    fsqrt d1, d1\n"
        "    fdiv d0, d0, d1\n"
        "    fmov x0, d0\n"
        "    bl rt_atan\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_acos:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    bl rt_asin\n"
        "    fmov d0, x0\n"
        "    mov x9, #90\n"
        "    scvtf d1, x9\n"
        "    fsub d0, d1, d0\n"
        "    fmov x0, d0\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_atan2:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp d10, d11, [sp, #-16]!\n"
        "    fmov d10, x0\n"
        "    fmov d11, x1\n"
        "    fmov x9, d11\n"
        "    cbnz x9, 95f\n"
        "    fmov x10, d10\n"
        "    cbz x10, 96f\n"
        "    lsr x11, x10, #63\n"
        "    mov x9, #90\n"
        "    scvtf d0, x9\n"
        "    cbz x11, 97f\n"
        "    fneg d0, d0\n"
        "97:\n"
        "    fmov x0, d0\n"
        "    b 98f\n"
        "96:\n"
        "    mov x0, #0\n"
        "    b 98f\n"
        "95:\n"
        "    fdiv d0, d10, d11\n"
        "    fmov x0, d0\n"
        "    bl rt_atan\n"
        "    fmov x9, d11\n"
        "    lsr x9, x9, #63\n"
        "    cbz x9, 98f\n"
        "    fmov d1, x0\n"
        "    fmov x10, d10\n"
        "    lsr x10, x10, #63\n"
        "    mov x11, #180\n"
        "    scvtf d2, x11\n"
        "    cbnz x10, 99f\n"
        "    fadd d1, d1, d2\n"
        "    b 100f\n"
        "99:\n"
        "    fsub d1, d1, d2\n"
        "100:\n"
        "    fmov x0, d1\n"
        "98:\n"
        "    ldp d10, d11, [sp], #16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// -- bucle de eventos: WaitEvent, PollEvent, Notify, ClientWidth/Height --\n"
        "\n"
        "rt_wait_event:\n"
        "    stp x29, x30, [sp, #-32]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    mov x19, x0\n"
        "    cmp x19, #0\n"
        "    blt 43f\n"
        "    mov x9, #10\n"
        "    sdiv x20, x19, x9\n"
        "    mov x8, #2\n"
        "    svc #0\n"
        "    add x20, x20, x0\n"
        "    b 44f\n"
        "43:\n"
        "    mov x20, #0\n"
        "44:\n"
        "45:\n"
        "    mov x8, #14\n"
        "    svc #0\n"
        "    mov x8, #8\n"
        "    svc #0\n"
        "    cbnz x0, 47f\n"
        "    cmp x19, #0\n"
        "    blt 45b\n"
        "    mov x8, #2\n"
        "    svc #0\n"
        "    cmp x0, x20\n"
        "    blt 45b\n"
        "    mov x0, #0\n"
        "47:\n"
        "    adrp x9, rt_last_event_id\n"
        "    add x9, x9, :lo12:rt_last_event_id\n"
        "    str x0, [x9]\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x29, x30, [sp], #32\n"
        "    ret\n"
        "\n"
        "rt_poll_event:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x8, #14\n"
        "    svc #0\n"
        "    mov x8, #8\n"
        "    svc #0\n"
        "    adrp x9, rt_last_event_id\n"
        "    add x9, x9, :lo12:rt_last_event_id\n"
        "    str x0, [x9]\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_client_width:\n"
        "    mov x9, #0x7FFFFFFF\n"
        "    cmp x0, x9\n"
        "    beq 48f\n"
        "    mov x8, #33\n"
        "    svc #0\n"
        "    lsr x0, x0, #32\n"
        "    ret\n"
        "48:\n"
        "    mov x8, #35\n"
        "    svc #0\n"
        "    lsr x0, x0, #32\n"
        "    ret\n"
        "rt_client_height:\n"
        "    mov x9, #0x7FFFFFFF\n"
        "    cmp x0, x9\n"
        "    beq 49f\n"
        "    mov x8, #33\n"
        "    svc #0\n"
        "    mov w0, w0\n"
        "    ret\n"
        "49:\n"
        "    mov x8, #35\n"
        "    svc #0\n"
        "    mov w0, w0\n"
        "    ret\n"
        "\n"
        "rt_notify:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    mov x8, #33\n"
        "    svc #0\n"
        "    lsr x10, x0, #32\n"
        "    mov w11, w0\n"
        "    mov x12, #200\n"
        "    mov x13, #60\n"
        "    subs x14, x10, x12\n"
        "    bge 50f\n"
        "    mov x14, #0\n"
        "50:\n"
        "    lsr x14, x14, #1\n"
        "    subs x15, x11, x13\n"
        "    bge 51f\n"
        "    mov x15, #0\n"
        "51:\n"
        "    lsr x15, x15, #1\n"
        "    mov x0, x14\n"
        "    mov x1, x15\n"
        "    mov x2, x12\n"
        "    mov x3, x13\n"
        "    mov x4, #0xF0F0F0\n"
        "    mov x8, #30\n"
        "    svc #0\n"
        "    add x0, x14, #10\n"
        "    add x1, x15, #20\n"
        "    mov x2, x9\n"
        "    mov x3, #0\n"
        "    mov x8, #31\n"
        "    svc #0\n"
        "    mov x0, #150\n"
        "    mov x8, #1\n"
        "    svc #0\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_confirm:\n"
        "    stp x29, x30, [sp, #-80]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    stp x21, x22, [sp, #32]\n"
        "    stp x23, x24, [sp, #48]\n"
        "    str x25, [sp, #64]\n"
        "    mov x19, x0\n"
        "    mov x8, #33\n" // SYS_GET_WINDOW_SIZE
        "    svc #0\n"
        "    lsr x9, x0, #32\n"
        "    mov w10, w0\n"
        "    cmp x9, #220\n"
        "    bge 320f\n"
        "    mov x20, #0\n"
        "    b 321f\n"
        "320:\n"
        "    sub x20, x9, #220\n"
        "    lsr x20, x20, #1\n"
        "321:\n"
        "    cmp x10, #90\n"
        "    bge 322f\n"
        "    mov x21, #0\n"
        "    b 323f\n"
        "322:\n"
        "    sub x21, x10, #90\n"
        "    lsr x21, x21, #1\n"
        "323:\n"
        "    mov x0, x20\n"
        "    mov x1, x21\n"
        "    mov x2, #220\n"
        "    lsl x2, x2, #16\n"
        "    mov x9, #90\n"
        "    orr x2, x2, x9\n"
        "    mov x8, #101\n" // SYS_CREATE_PANEL
        "    svc #0\n"
        "    mov x22, x0\n"
        "    mov x0, x19\n"
        "    add x1, x20, #10\n"
        "    add x2, x21, #10\n"
        "    mov x3, #200\n"
        "    lsl x3, x3, #16\n"
        "    mov x9, #20\n"
        "    orr x3, x3, x9\n"
        "    mov x4, #0\n"
        "    mov x8, #160\n" // SYS_CREATE_LABEL
        "    svc #0\n"
        "    mov x23, x0\n"
        "    adrp x0, rt_str_si\n"
        "    add x0, x0, :lo12:rt_str_si\n"
        "    add x1, x20, #30\n"
        "    add x2, x21, #50\n"
        "    mov x3, #70\n"
        "    lsl x3, x3, #16\n"
        "    mov x9, #24\n"
        "    orr x3, x3, x9\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n" // SYS_CREATE_BUTTON
        "    svc #0\n"
        "    mov x24, x0\n"
        "    adrp x0, rt_str_no\n"
        "    add x0, x0, :lo12:rt_str_no\n"
        "    add x1, x20, #120\n"
        "    add x2, x21, #50\n"
        "    mov x3, #70\n"
        "    lsl x3, x3, #16\n"
        "    mov x9, #24\n"
        "    orr x3, x3, x9\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n"
        "    svc #0\n"
        "    mov x25, x0\n"
        "300:\n"
        "    mov x8, #14\n" // SYS_PUMP
        "    svc #0\n"
        "    mov x8, #8\n" // ultimo id de evento (no bloqueante)
        "    svc #0\n"
        "    mov x9, #0x401\n" // EVENT_GADGETACTION
        "    cmp x0, x9\n"
        "    beq 301f\n"
        "    mov x9, #0x803\n" // EVENT_WINDOWCLOSE -- se trata como 'No'
        "    cmp x0, x9\n"
        "    beq 303f\n"
        "    b 300b\n"
        "301:\n"
        "    mov x8, #9\n" // fuente+datos del ultimo evento
        "    svc #0\n"
        "    lsr x9, x0, #32\n"
        "    cmp x9, x24\n"
        "    beq 302f\n"
        "    cmp x9, x25\n"
        "    beq 303f\n"
        "    b 300b\n"
        "302:\n"
        "    mov x19, #1\n"
        "    b 304f\n"
        "303:\n"
        "    mov x19, #0\n"
        "304:\n"
        "    mov x0, x22\n"
        "    mov x8, #104\n" // SYS_GADGET_FREE
        "    svc #0\n"
        "    mov x0, x23\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x24\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x25\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x19\n"
        "    ldr x25, [sp, #64]\n"
        "    ldp x23, x24, [sp, #48]\n"
        "    ldp x21, x22, [sp, #32]\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x29, x30, [sp], #80\n"
        "    ret\n"
        "\n"
        "// rt_proceed: igual que rt_confirm, pero con TRES botones\n"
        "// (Si/No/Cancelar) -- devuelve 1, 0 o -1. Registros: los\n"
        "// mismos que rt_confirm, mas x26=id boton Cancelar.\n"
        "rt_proceed:\n"
        "    stp x29, x30, [sp, #-96]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    stp x21, x22, [sp, #32]\n"
        "    stp x23, x24, [sp, #48]\n"
        "    stp x25, x26, [sp, #64]\n"
        "    mov x19, x0\n"
        "    mov x8, #33\n"
        "    svc #0\n"
        "    lsr x9, x0, #32\n"
        "    mov w10, w0\n"
        "    cmp x9, #300\n"
        "    bge 330f\n"
        "    mov x20, #0\n"
        "    b 331f\n"
        "330:\n"
        "    sub x20, x9, #300\n"
        "    lsr x20, x20, #1\n"
        "331:\n"
        "    cmp x10, #90\n"
        "    bge 332f\n"
        "    mov x21, #0\n"
        "    b 333f\n"
        "332:\n"
        "    sub x21, x10, #90\n"
        "    lsr x21, x21, #1\n"
        "333:\n"
        "    mov x0, x20\n"
        "    mov x1, x21\n"
        "    mov x2, #300\n"
        "    lsl x2, x2, #16\n"
        "    mov x9, #90\n"
        "    orr x2, x2, x9\n"
        "    mov x8, #101\n"
        "    svc #0\n"
        "    mov x22, x0\n"
        "    mov x0, x19\n"
        "    add x1, x20, #10\n"
        "    add x2, x21, #10\n"
        "    mov x3, #280\n"
        "    lsl x3, x3, #16\n"
        "    mov x9, #20\n"
        "    orr x3, x3, x9\n"
        "    mov x4, #0\n"
        "    mov x8, #160\n"
        "    svc #0\n"
        "    mov x23, x0\n"
        "    adrp x0, rt_str_si\n"
        "    add x0, x0, :lo12:rt_str_si\n"
        "    add x1, x20, #30\n"
        "    add x2, x21, #50\n"
        "    mov x3, #70\n"
        "    lsl x3, x3, #16\n"
        "    mov x9, #24\n"
        "    orr x3, x3, x9\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n"
        "    svc #0\n"
        "    mov x24, x0\n"
        "    adrp x0, rt_str_no\n"
        "    add x0, x0, :lo12:rt_str_no\n"
        "    add x1, x20, #120\n"
        "    add x2, x21, #50\n"
        "    mov x3, #70\n"
        "    lsl x3, x3, #16\n"
        "    mov x9, #24\n"
        "    orr x3, x3, x9\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n"
        "    svc #0\n"
        "    mov x25, x0\n"
        "    adrp x0, rt_str_cancelar\n"
        "    add x0, x0, :lo12:rt_str_cancelar\n"
        "    add x1, x20, #210\n"
        "    add x2, x21, #50\n"
        "    mov x3, #80\n"
        "    lsl x3, x3, #16\n"
        "    mov x9, #24\n"
        "    orr x3, x3, x9\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n"
        "    svc #0\n"
        "    mov x26, x0\n"
        "310:\n"
        "    mov x8, #14\n"
        "    svc #0\n"
        "    mov x8, #8\n"
        "    svc #0\n"
        "    mov x9, #0x401\n"
        "    cmp x0, x9\n"
        "    beq 311f\n"
        "    mov x9, #0x803\n"
        "    cmp x0, x9\n"
        "    beq 314f\n"
        "    b 310b\n"
        "311:\n"
        "    mov x8, #9\n"
        "    svc #0\n"
        "    lsr x9, x0, #32\n"
        "    cmp x9, x24\n"
        "    beq 312f\n"
        "    cmp x9, x25\n"
        "    beq 313f\n"
        "    cmp x9, x26\n"
        "    beq 314f\n"
        "    b 310b\n"
        "312:\n"
        "    mov x19, #1\n"
        "    b 315f\n"
        "313:\n"
        "    mov x19, #0\n"
        "    b 315f\n"
        "314:\n"
        "    mov x19, #-1\n"
        "315:\n"
        "    mov x0, x22\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x23\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x24\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x25\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x26\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x19\n"
        "    ldp x25, x26, [sp, #64]\n"
        "    ldp x23, x24, [sp, #48]\n"
        "    ldp x21, x22, [sp, #32]\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x29, x30, [sp], #96\n"
        "    ret\n"
        "\n"
        "// rt_request_color: x0=r, x1=g, x2=b (color inicial) --\n"
        "// dialogo modal con 3 Slider (0-255) + un Panel de vista\n"
        "// previa que se actualiza en vivo + botones OK/Cancelar.\n"
        "// Devuelve 1 si se acepto (y guarda el resultado en\n"
        "// rt_requested_r/g/b para RequestedRed/Green/Blue), 0 si se\n"
        "// cancelo. Registros: x19/x20/x21/x22/x23 cambian de\n"
        "// significado entre la fase de creacion y la de bucle (ver\n"
        "// comentarios en cada fase), x24=id boton OK, x25=id boton\n"
        "// Cancelar, x26=resultado final (1/0).\n"
        "rt_request_color:\n"
        "    stp x29, x30, [sp, #-96]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    stp x21, x22, [sp, #32]\n"
        "    stp x23, x24, [sp, #48]\n"
        "    stp x25, x26, [sp, #64]\n"
        "    mov x21, x0\n" // r inicial
        "    mov x22, x1\n" // g inicial
        "    mov x23, x2\n" // b inicial
        "    mov x8, #33\n" // SYS_GET_WINDOW_SIZE
        "    svc #0\n"
        "    lsr x9, x0, #32\n"
        "    mov w10, w0\n"
        "    cmp x9, #260\n"
        "    bge 340f\n"
        "    mov x19, #0\n"
        "    b 341f\n"
        "340:\n"
        "    sub x19, x9, #260\n"
        "    lsr x19, x19, #1\n"
        "341:\n"
        "    cmp x10, #210\n"
        "    bge 342f\n"
        "    mov x20, #0\n"
        "    b 343f\n"
        "342:\n"
        "    sub x20, x10, #210\n"
        "    lsr x20, x20, #1\n"
        "343:\n"
        // Panel de fondo
        "    mov x0, x19\n"
        "    mov x1, x20\n"
        "    mov x2, #260\n"
        "    lsl x2, x2, #16\n"
        "    mov x9, #210\n"
        "    orr x2, x2, x9\n"
        "    mov x8, #101\n" // SYS_CREATE_PANEL
        "    svc #0\n"
        "    stp x19, x20, [sp, #80]\n" // guardamos panel_x/panel_y en la pila -- se necesitan mas veces todavia
        "    mov x19, x0\n" // reutilizado desde aqui: id del panel de fondo
        // Slider R
        "    ldp x9, x10, [sp, #80]\n"
        "    add x0, x9, #10\n"
        "    add x1, x10, #10\n"
        "    mov x2, #220\n"
        "    lsl x2, x2, #16\n"
        "    mov x11, #20\n"
        "    orr x2, x2, x11\n"
        "    mov x3, #1\n"
        "    mov x8, #163\n" // SYS_CREATE_SLIDER
        "    svc #0\n"
        "    mov x11, x0\n" // id temporal del slider R
        "    mov x0, x11\n"
        "    mov x1, #1\n"
        "    mov x2, #256\n"
        "    mov x8, #164\n" // SYS_SET_SLIDER_RANGE
        "    svc #0\n"
        "    mov x0, x11\n"
        "    mov x1, x21\n" // r inicial
        "    mov x8, #165\n" // SYS_SET_SLIDER_VALUE
        "    svc #0\n"
        "    mov x21, x11\n" // reutilizado desde aqui: id del slider R
        // Slider G
        "    ldp x9, x10, [sp, #80]\n"
        "    add x0, x9, #10\n"
        "    add x1, x10, #40\n"
        "    mov x2, #220\n"
        "    lsl x2, x2, #16\n"
        "    mov x11, #20\n"
        "    orr x2, x2, x11\n"
        "    mov x3, #1\n"
        "    mov x8, #163\n"
        "    svc #0\n"
        "    mov x11, x0\n"
        "    mov x0, x11\n"
        "    mov x1, #1\n"
        "    mov x2, #256\n"
        "    mov x8, #164\n"
        "    svc #0\n"
        "    mov x0, x11\n"
        "    mov x1, x22\n"
        "    mov x8, #165\n"
        "    svc #0\n"
        "    mov x22, x11\n" // reutilizado desde aqui: id del slider G
        // Slider B
        "    ldp x9, x10, [sp, #80]\n"
        "    add x0, x9, #10\n"
        "    add x1, x10, #70\n"
        "    mov x2, #220\n"
        "    lsl x2, x2, #16\n"
        "    mov x11, #20\n"
        "    orr x2, x2, x11\n"
        "    mov x3, #1\n"
        "    mov x8, #163\n"
        "    svc #0\n"
        "    mov x11, x0\n"
        "    mov x0, x11\n"
        "    mov x1, #1\n"
        "    mov x2, #256\n"
        "    mov x8, #164\n"
        "    svc #0\n"
        "    mov x0, x11\n"
        "    mov x1, x23\n"
        "    mov x8, #165\n"
        "    svc #0\n"
        "    mov x23, x11\n" // reutilizado desde aqui: id del slider B
        // Panel de vista previa (swatch)
        "    ldp x9, x10, [sp, #80]\n"
        "    add x0, x9, #10\n"
        "    add x1, x10, #100\n"
        "    mov x2, #220\n"
        "    lsl x2, x2, #16\n"
        "    mov x11, #40\n"
        "    orr x2, x2, x11\n"
        "    mov x8, #101\n" // SYS_CREATE_PANEL
        "    svc #0\n"
        "    mov x20, x0\n" // reutilizado desde aqui: id del panel de vista previa
        // Boton OK
        "    ldp x9, x10, [sp, #80]\n"
        "    adrp x0, rt_str_si\n"
        "    add x0, x0, :lo12:rt_str_si\n"
        "    add x1, x9, #30\n"
        "    add x2, x10, #160\n"
        "    mov x3, #90\n"
        "    lsl x3, x3, #16\n"
        "    mov x11, #24\n"
        "    orr x3, x3, x11\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n" // SYS_CREATE_BUTTON
        "    svc #0\n"
        "    mov x24, x0\n"
        // Boton Cancelar
        "    ldp x9, x10, [sp, #80]\n"
        "    adrp x0, rt_str_cancelar\n"
        "    add x0, x0, :lo12:rt_str_cancelar\n"
        "    add x1, x9, #150\n"
        "    add x2, x10, #160\n"
        "    mov x3, #90\n"
        "    lsl x3, x3, #16\n"
        "    mov x11, #24\n"
        "    orr x3, x3, x11\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n"
        "    svc #0\n"
        "    mov x25, x0\n"
        "350:\n" // bucle principal
        "    mov x8, #14\n" // SYS_PUMP
        "    svc #0\n"
        // Releemos los 3 sliders y actualizamos la vista previa en vivo
        "    mov x0, x21\n"
        "    mov x8, #166\n" // SYS_SLIDER_VALUE
        "    svc #0\n"
        "    mov x9, x0\n" // r actual
        "    mov x0, x22\n"
        "    mov x8, #166\n"
        "    svc #0\n"
        "    mov x10, x0\n" // g actual
        "    mov x0, x23\n"
        "    mov x8, #166\n"
        "    svc #0\n"
        "    mov x11, x0\n" // b actual
        "    mov x0, x11\n" // b
        "    lsl x12, x10, #8\n" // g<<8
        "    orr x0, x0, x12\n"
        "    lsl x12, x9, #16\n" // r<<16
        "    orr x1, x0, x12\n" // x1 = (r<<16|g<<8|b)
        "    mov x0, x20\n" // id del swatch
        "    mov x8, #207\n" // SYS_SET_PANEL_COLOR
        "    svc #0\n"
        // Consultamos el ultimo evento (no bloqueante)
        "    mov x8, #8\n"
        "    svc #0\n"
        "    mov x12, #0x401\n" // EVENT_GADGETACTION
        "    cmp x0, x12\n"
        "    beq 351f\n"
        "    mov x12, #0x803\n" // EVENT_WINDOWCLOSE -- se trata como Cancelar
        "    cmp x0, x12\n"
        "    beq 353f\n"
        "    b 350b\n"
        "351:\n"
        "    mov x8, #9\n"
        "    svc #0\n"
        "    lsr x12, x0, #32\n"
        "    cmp x12, x24\n"
        "    beq 352f\n"
        "    cmp x12, x25\n"
        "    beq 353f\n"
        "    b 350b\n"
        "352:\n"
        "    mov x26, #1\n"
        "    b 354f\n"
        "353:\n"
        "    mov x26, #0\n"
        "354:\n"
        // Guardamos el resultado final ANTES de liberar los sliders
        // (SliderValue ya no funcionaria sobre un gadget liberado)
        "    mov x0, x21\n"
        "    mov x8, #166\n"
        "    svc #0\n"
        "    adrp x9, rt_requested_r\n"
        "    add x9, x9, :lo12:rt_requested_r\n"
        "    str x0, [x9]\n"
        "    mov x0, x22\n"
        "    mov x8, #166\n"
        "    svc #0\n"
        "    adrp x9, rt_requested_g\n"
        "    add x9, x9, :lo12:rt_requested_g\n"
        "    str x0, [x9]\n"
        "    mov x0, x23\n"
        "    mov x8, #166\n"
        "    svc #0\n"
        "    adrp x9, rt_requested_b\n"
        "    add x9, x9, :lo12:rt_requested_b\n"
        "    str x0, [x9]\n"
        // Liberamos los 6 gadgets (fondo, 3 sliders, vista previa, OK, Cancelar)
        "    mov x0, x19\n"
        "    mov x8, #104\n" // SYS_GADGET_FREE
        "    svc #0\n"
        "    mov x0, x20\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x21\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x22\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x23\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x24\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x25\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x26\n"
        "    ldp x25, x26, [sp, #64]\n"
        "    ldp x23, x24, [sp, #48]\n"
        "    ldp x21, x22, [sp, #32]\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x29, x30, [sp], #96\n"
        "    ret\n"
        "\n"
        "// rt_populate_dir_listbox: x0=inodo del directorio, x1=id del\n"
        "// ListBox -- lo vacia y lo vuelve a rellenar con las entradas\n"
        "// de ese directorio (SYS_FILE_LIST), con el prefijo '[D] '\n"
        "// para las carpetas -- asi despues, con solo mirar el TEXTO\n"
        "// del item seleccionado, sabemos si es carpeta o archivo sin\n"
        "// necesitar una tabla aparte de metadatos por fila.\n"
        "rt_populate_dir_listbox:\n"
        "    stp x29, x30, [sp, #-32]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    mov x19, x0\n" // inodo del directorio
        "    mov x20, x1\n" // id del listbox
        "    mov x0, x20\n"
        "    mov x8, #115\n" // SYS_LISTBOX_CLEAR
        "    svc #0\n"
        "    mov x0, x19\n"
        "    adrp x1, rt_dir_listing_buf\n"
        "    add x1, x1, :lo12:rt_dir_listing_buf\n"
        "    mov x2, #32\n"
        "    mov x3, #0\n" // volumen 0 = NemoFS
        "    mov x4, #0\n"
        "    mov x8, #23\n" // SYS_FILE_LIST
        "    svc #0\n"
        "    cmp x0, #32\n"
        "    ble 400f\n"
        "    mov x0, #32\n"
        "400:\n"
        "    mov x9, x0\n" // contador de entradas restantes
        "    adrp x10, rt_dir_listing_buf\n"
        "    add x10, x10, :lo12:rt_dir_listing_buf\n"
        "401:\n"
        "    cbz x9, 403f\n"
        "    ldr w11, [x10, #4]\n" // tipo de esta entrada
        "    add x12, x10, #12\n" // puntero al nombre
        "    adrp x13, rt_dir_item_buf\n"
        "    add x13, x13, :lo12:rt_dir_item_buf\n"
        "    cmp x11, #2\n" // TYPE_DIR
        "    bne 402f\n"
        "    mov w14, #91\n" // '['
        "    strb w14, [x13]\n"
        "    mov w14, #68\n" // 'D'
        "    strb w14, [x13, #1]\n"
        "    mov w14, #93\n" // ']'
        "    strb w14, [x13, #2]\n"
        "    mov w14, #32\n" // ' '
        "    strb w14, [x13, #3]\n"
        "    add x13, x13, #4\n"
        "402:\n"
        "    mov x15, x12\n" // puntero de lectura
        "    mov x16, x13\n" // puntero de escritura
        "    mov x17, #0\n" // contador
        "405:\n"
        "    cmp x17, #27\n"
        "    bge 406f\n" // limite alcanzado -- forzamos terminador y salimos
        "    ldrb w14, [x15], #1\n"
        "    strb w14, [x16], #1\n"
        "    cbz w14, 407f\n" // ya copiamos el propio terminador -- terminado
        "    add x17, x17, #1\n"
        "    b 405b\n"
        "406:\n"
        "    mov w14, #0\n"
        "    strb w14, [x16]\n"
        "407:\n"
        "    mov x0, x20\n"
        "    adrp x1, rt_dir_item_buf\n"
        "    add x1, x1, :lo12:rt_dir_item_buf\n"
        "    mov x8, #114\n" // SYS_LISTBOX_ADD_ITEM
        "    svc #0\n"
        "    add x10, x10, #40\n"
        "    sub x9, x9, #1\n"
        "    b 401b\n"
        "403:\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x29, x30, [sp], #32\n"
        "    ret\n"
        "\n"
        "// rt_request_dir: dialogo de eleccion de carpeta -- ListBox\n"
        "// navegable (Entrar desciende, Elegir confirma la carpeta\n"
        "// ACTUAL, Cancelar aborta). Guarda el inodo elegido en\n"
        "// rt_requested_dir_inode. Devuelve 1 si se eligio, 0 si se\n"
        "// cancelo. x19 hace doble papel: x del panel durante la\n"
        "// creacion, inodo del directorio actual durante el bucle.\n"
        "rt_request_dir:\n"
        "    stp x29, x30, [sp, #-96]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    stp x21, x22, [sp, #32]\n"
        "    stp x23, x24, [sp, #48]\n"
        "    str x25, [sp, #64]\n"
        "    mov x8, #33\n" // SYS_GET_WINDOW_SIZE
        "    svc #0\n"
        "    lsr x9, x0, #32\n"
        "    mov w10, w0\n"
        "    cmp x9, #320\n"
        "    bge 410f\n"
        "    mov x19, #0\n"
        "    b 411f\n"
        "410:\n"
        "    sub x19, x9, #320\n"
        "    lsr x19, x19, #1\n"
        "411:\n"
        "    cmp x10, #280\n"
        "    bge 412f\n"
        "    mov x20, #0\n"
        "    b 413f\n"
        "412:\n"
        "    sub x20, x10, #280\n"
        "    lsr x20, x20, #1\n"
        "413:\n"
        "    stp x19, x20, [sp, #72]\n"
        "    mov x0, x19\n"
        "    mov x1, x20\n"
        "    mov x2, #320\n"
        "    lsl x2, x2, #16\n"
        "    mov x9, #280\n"
        "    orr x2, x2, x9\n"
        "    mov x8, #101\n" // SYS_CREATE_PANEL
        "    svc #0\n"
        "    mov x21, x0\n" // id del panel de fondo
        "    ldp x9, x10, [sp, #72]\n"
        "    add x0, x9, #10\n"
        "    add x1, x10, #10\n"
        "    mov x2, #300\n"
        "    lsl x2, x2, #16\n"
        "    mov x11, #180\n"
        "    orr x2, x2, x11\n"
        "    mov x8, #103\n" // SYS_CREATE_LISTBOX
        "    svc #0\n"
        "    mov x20, x0\n" // reutilizado desde aqui: id del listbox
        "    ldp x9, x10, [sp, #72]\n"
        "    adrp x0, rt_str_entrar\n"
        "    add x0, x0, :lo12:rt_str_entrar\n"
        "    add x1, x9, #10\n"
        "    add x2, x10, #200\n"
        "    mov x3, #90\n"
        "    lsl x3, x3, #16\n"
        "    mov x11, #24\n"
        "    orr x3, x3, x11\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n" // SYS_CREATE_BUTTON
        "    svc #0\n"
        "    mov x22, x0\n"
        "    ldp x9, x10, [sp, #72]\n"
        "    adrp x0, rt_str_elegir\n"
        "    add x0, x0, :lo12:rt_str_elegir\n"
        "    add x1, x9, #110\n"
        "    add x2, x10, #200\n"
        "    mov x3, #90\n"
        "    lsl x3, x3, #16\n"
        "    mov x11, #24\n"
        "    orr x3, x3, x11\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n"
        "    svc #0\n"
        "    mov x23, x0\n"
        "    ldp x9, x10, [sp, #72]\n"
        "    adrp x0, rt_str_cancelar\n"
        "    add x0, x0, :lo12:rt_str_cancelar\n"
        "    add x1, x9, #210\n"
        "    add x2, x10, #200\n"
        "    mov x3, #90\n"
        "    lsl x3, x3, #16\n"
        "    mov x11, #24\n"
        "    orr x3, x3, x11\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n"
        "    svc #0\n"
        "    mov x24, x0\n"
        "    mov x19, #0\n" // reutilizado desde aqui: inodo del directorio actual (empieza en la raiz)
        "    mov x0, x19\n"
        "    mov x1, x20\n"
        "    bl rt_populate_dir_listbox\n"
        "420:\n"
        "    mov x8, #14\n" // SYS_PUMP
        "    svc #0\n"
        "    mov x8, #8\n"
        "    svc #0\n"
        "    mov x9, #0x401\n" // EVENT_GADGETACTION
        "    cmp x0, x9\n"
        "    beq 421f\n"
        "    mov x9, #0x803\n" // EVENT_WINDOWCLOSE -- se trata como Cancelar
        "    cmp x0, x9\n"
        "    beq 425f\n"
        "    b 420b\n"
        "421:\n"
        "    mov x8, #9\n"
        "    svc #0\n"
        "    lsr x9, x0, #32\n"
        "    cmp x9, x22\n"
        "    beq 422f\n" // Entrar
        "    cmp x9, x23\n"
        "    beq 424f\n" // Elegir
        "    cmp x9, x24\n"
        "    beq 425f\n" // Cancelar
        "    b 420b\n"
        "422:\n"
        "    mov x0, x20\n"
        "    mov x8, #116\n" // SYS_LISTBOX_SELECTED
        "    svc #0\n"
        "    cmp x0, #0\n"
        "    blt 420b\n" // nada seleccionado -- ignoramos el clic
        "    mov x9, x0\n"
        "    mov x0, x20\n"
        "    mov x1, x9\n"
        "    adrp x2, rt_dir_item_buf\n"
        "    add x2, x2, :lo12:rt_dir_item_buf\n"
        "    mov x3, #40\n"
        "    mov x8, #119\n" // SYS_LISTBOX_ITEM_TEXT
        "    svc #0\n"
        "    adrp x10, rt_dir_item_buf\n"
        "    add x10, x10, :lo12:rt_dir_item_buf\n"
        "    ldrb w11, [x10]\n"
        "    cmp w11, #91\n" // '['
        "    bne 420b\n" // no es carpeta -- ignoramos "Entrar" sobre un archivo
        "    add x10, x10, #4\n" // saltamos "[D] "
        "    mov x0, x10\n"
        "    mov x1, x19\n"
        "    mov x2, #0\n"
        "    mov x8, #20\n" // SYS_FILE_OPEN
        "    svc #0\n"
        "    mov x19, x0\n"
        "    mov x0, x19\n"
        "    mov x1, x20\n"
        "    bl rt_populate_dir_listbox\n"
        "    b 420b\n"
        "424:\n"
        "    mov x25, #1\n"
        "    b 426f\n"
        "425:\n"
        "    mov x25, #0\n"
        "426:\n"
        "    adrp x9, rt_requested_dir_inode\n"
        "    add x9, x9, :lo12:rt_requested_dir_inode\n"
        "    str x19, [x9]\n"
        "    mov x0, x21\n"
        "    mov x8, #104\n" // SYS_GADGET_FREE
        "    svc #0\n"
        "    mov x0, x20\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x22\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x23\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x24\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x25\n"
        "    ldr x25, [sp, #64]\n"
        "    ldp x23, x24, [sp, #48]\n"
        "    ldp x21, x22, [sp, #32]\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x29, x30, [sp], #96\n"
        "    ret\n"
        "\n"
        "// rt_request_file: dialogo de eleccion de ARCHIVO -- misma\n"
        "// estructura que rt_request_dir (ListBox navegable con\n"
        "// Entrar/Elegir/Cancelar), pero 'Elegir' solo acepta\n"
        "// archivos (ignora el clic sobre una carpeta), y el\n"
        "// resultado es \"inodo:nombre\" (mismo formato que ya usa\n"
        "// editor.c/explorer.c internamente), devuelto DIRECTAMENTE\n"
        "// como puntero a cadena en x0 (cadena vacia si se cancelo).\n"
"rt_request_file:\n"
        "    stp x29, x30, [sp, #-96]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    stp x21, x22, [sp, #32]\n"
        "    stp x23, x24, [sp, #48]\n"
        "    str x25, [sp, #64]\n"
        "    mov x8, #33\n" // SYS_GET_WINDOW_SIZE
        "    svc #0\n"
        "    lsr x9, x0, #32\n"
        "    mov w10, w0\n"
        "    cmp x9, #320\n"
        "    bge 440f\n"
        "    mov x19, #0\n"
        "    b 441f\n"
        "440:\n"
        "    sub x19, x9, #320\n"
        "    lsr x19, x19, #1\n"
        "441:\n"
        "    cmp x10, #280\n"
        "    bge 442f\n"
        "    mov x20, #0\n"
        "    b 443f\n"
        "442:\n"
        "    sub x20, x10, #280\n"
        "    lsr x20, x20, #1\n"
        "443:\n"
        "    stp x19, x20, [sp, #72]\n"
        "    mov x0, x19\n"
        "    mov x1, x20\n"
        "    mov x2, #320\n"
        "    lsl x2, x2, #16\n"
        "    mov x9, #280\n"
        "    orr x2, x2, x9\n"
        "    mov x8, #101\n" // SYS_CREATE_PANEL
        "    svc #0\n"
        "    mov x21, x0\n" // id del panel de fondo
        "    ldp x9, x10, [sp, #72]\n"
        "    add x0, x9, #10\n"
        "    add x1, x10, #10\n"
        "    mov x2, #300\n"
        "    lsl x2, x2, #16\n"
        "    mov x11, #180\n"
        "    orr x2, x2, x11\n"
        "    mov x8, #103\n" // SYS_CREATE_LISTBOX
        "    svc #0\n"
        "    mov x20, x0\n" // reutilizado desde aqui: id del listbox
        "    ldp x9, x10, [sp, #72]\n"
        "    adrp x0, rt_str_entrar\n"
        "    add x0, x0, :lo12:rt_str_entrar\n"
        "    add x1, x9, #10\n"
        "    add x2, x10, #200\n"
        "    mov x3, #90\n"
        "    lsl x3, x3, #16\n"
        "    mov x11, #24\n"
        "    orr x3, x3, x11\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n" // SYS_CREATE_BUTTON
        "    svc #0\n"
        "    mov x22, x0\n"
        "    ldp x9, x10, [sp, #72]\n"
        "    adrp x0, rt_str_elegir\n"
        "    add x0, x0, :lo12:rt_str_elegir\n"
        "    add x1, x9, #110\n"
        "    add x2, x10, #200\n"
        "    mov x3, #90\n"
        "    lsl x3, x3, #16\n"
        "    mov x11, #24\n"
        "    orr x3, x3, x11\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n"
        "    svc #0\n"
        "    mov x23, x0\n"
        "    ldp x9, x10, [sp, #72]\n"
        "    adrp x0, rt_str_cancelar\n"
        "    add x0, x0, :lo12:rt_str_cancelar\n"
        "    add x1, x9, #210\n"
        "    add x2, x10, #200\n"
        "    mov x3, #90\n"
        "    lsl x3, x3, #16\n"
        "    mov x11, #24\n"
        "    orr x3, x3, x11\n"
        "    mov x4, #1\n"
        "    mov x8, #100\n"
        "    svc #0\n"
        "    mov x24, x0\n"
        "    mov x19, #0\n" // reutilizado desde aqui: inodo del directorio actual (empieza en la raiz)
        "    mov x0, x19\n"
        "    mov x1, x20\n"
        "    bl rt_populate_dir_listbox\n"
        "450:\n"
        "    mov x8, #14\n" // SYS_PUMP
        "    svc #0\n"
        "    mov x8, #8\n"
        "    svc #0\n"
        "    mov x9, #0x401\n" // EVENT_GADGETACTION
        "    cmp x0, x9\n"
        "    beq 451f\n"
        "    mov x9, #0x803\n" // EVENT_WINDOWCLOSE -- se trata como Cancelar
        "    cmp x0, x9\n"
        "    beq 455f\n"
        "    b 450b\n"
        "451:\n"
        "    mov x8, #9\n"
        "    svc #0\n"
        "    lsr x9, x0, #32\n"
        "    cmp x9, x22\n"
        "    beq 452f\n" // Entrar
        "    cmp x9, x23\n"
        "    beq 454f\n" // Elegir
        "    cmp x9, x24\n"
        "    beq 455f\n" // Cancelar
        "    b 450b\n"
        "452:\n"
        "    mov x0, x20\n"
        "    mov x8, #116\n" // SYS_LISTBOX_SELECTED
        "    svc #0\n"
        "    cmp x0, #0\n"
        "    blt 450b\n" // nada seleccionado -- ignoramos el clic
        "    mov x9, x0\n"
        "    mov x0, x20\n"
        "    mov x1, x9\n"
        "    adrp x2, rt_dir_item_buf\n"
        "    add x2, x2, :lo12:rt_dir_item_buf\n"
        "    mov x3, #40\n"
        "    mov x8, #119\n" // SYS_LISTBOX_ITEM_TEXT
        "    svc #0\n"
        "    adrp x10, rt_dir_item_buf\n"
        "    add x10, x10, :lo12:rt_dir_item_buf\n"
        "    ldrb w11, [x10]\n"
        "    cmp w11, #91\n" // '['
        "    bne 450b\n" // no es carpeta -- ignoramos "Entrar" sobre un archivo
        "    add x10, x10, #4\n" // saltamos "[D] "
        "    mov x0, x10\n"
        "    mov x1, x19\n"
        "    mov x2, #0\n"
        "    mov x8, #20\n" // SYS_FILE_OPEN
        "    svc #0\n"
        "    mov x19, x0\n"
        "    mov x0, x19\n"
        "    mov x1, x20\n"
        "    bl rt_populate_dir_listbox\n"
        "    b 450b\n"
        "454:\n"
        "    mov x0, x20\n"
        "    mov x8, #116\n" // SYS_LISTBOX_SELECTED
        "    svc #0\n"
        "    cmp x0, #0\n"
        "    blt 450b\n" // nada seleccionado -- ignoramos el clic
        "    mov x9, x0\n"
        "    mov x0, x20\n"
        "    mov x1, x9\n"
        "    adrp x2, rt_dir_item_buf\n"
        "    add x2, x2, :lo12:rt_dir_item_buf\n"
        "    mov x3, #40\n"
        "    mov x8, #119\n" // SYS_LISTBOX_ITEM_TEXT
        "    svc #0\n"
        "    adrp x10, rt_dir_item_buf\n"
        "    add x10, x10, :lo12:rt_dir_item_buf\n"
        "    ldrb w11, [x10]\n"
        "    cmp w11, #91\n" // '[' -- es carpeta, 'Elegir' no aplica (usar 'Entrar')
        "    beq 450b\n"
        "    mov x0, x19\n" // inodo del directorio actual
        "    bl rt_int_to_str\n"
        "    mov x1, x0\n"
        "    adrp x0, rt_str_colon\n"
        "    add x0, x0, :lo12:rt_str_colon\n"
        "    bl rt_str_concat\n" // "inodo:"
        "    mov x1, x0\n"
        "    adrp x0, rt_dir_item_buf\n"
        "    add x0, x0, :lo12:rt_dir_item_buf\n"
        "    bl rt_str_concat\n" // "inodo:nombre"
        "    mov x25, x0\n"
        "    b 456f\n"
        "455:\n"
        "    adrp x25, rt_empty_str\n"
        "    add x25, x25, :lo12:rt_empty_str\n"
        "456:\n"
        "    mov x0, x21\n"
        "    mov x8, #104\n" // SYS_GADGET_FREE
        "    svc #0\n"
        "    mov x0, x20\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x22\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x23\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x24\n"
        "    mov x8, #104\n"
        "    svc #0\n"
        "    mov x0, x25\n"
        "    ldr x25, [sp, #64]\n"
        "    ldp x23, x24, [sp, #48]\n"
        "    ldp x21, x22, [sp, #32]\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x29, x30, [sp], #96\n"
        "    ret\n"
        "\n"
        "rt_instr:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    cmp x2, #1\n"
        "    bge 57f\n"
        "    mov x2, #1\n"
        "57:\n"
        "    mov x9, x0\n"
        "    mov x10, #1\n"
        "58:\n"
        "    cmp x10, x2\n"
        "    bge 52f\n"
        "    ldrb w16, [x9]\n"
        "    cbz w16, 55f\n"
        "    add x9, x9, #1\n"
        "    add x10, x10, #1\n"
        "    b 58b\n"
        "52:\n"
        "    ldrb w11, [x9]\n"
        "    cbz w11, 55f\n"
        "    mov x12, x9\n"
        "    mov x13, x1\n"
        "53:\n"
        "    ldrb w14, [x13]\n"
        "    cbz w14, 54f\n"
        "    ldrb w15, [x12]\n"
        "    cbz w15, 55f\n"
        "    cmp w14, w15\n"
        "    bne 56f\n"
        "    add x12, x12, #1\n"
        "    add x13, x13, #1\n"
        "    b 53b\n"
        "54:\n"
        "    mov x0, x10\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "56:\n"
        "    add x9, x9, #1\n"
        "    add x10, x10, #1\n"
        "    b 52b\n"
        "55:\n"
        "    mov x0, #0\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_replace:\n"
        "    stp x29, x30, [sp, #-48]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    stp x21, x22, [sp, #32]\n"
        "    mov x19, x0\n"
        "    mov x20, x1\n"
        "    mov x21, x2\n"
        "    adrp x9, rt_str_pos\n"
        "    add x9, x9, :lo12:rt_str_pos\n"
        "    ldr x10, [x9]\n"
        "    adrp x11, rt_str_pool\n"
        "    add x11, x11, :lo12:rt_str_pool\n"
        "    add x11, x11, x10\n"
        "    add x10, x10, #128\n"
        "    and x10, x10, #1023\n"
        "    str x10, [x9]\n"
        "    mov x22, x11\n"
        "60:\n"
        "    ldrb w12, [x19]\n"
        "    cbz w12, 65f\n"
        "    mov x13, x19\n"
        "    mov x14, x20\n"
        "61:\n"
        "    ldrb w15, [x14]\n"
        "    cbz w15, 63f\n"
        "    ldrb w16, [x13]\n"
        "    cbz w16, 62f\n"
        "    cmp w15, w16\n"
        "    bne 62f\n"
        "    add x13, x13, #1\n"
        "    add x14, x14, #1\n"
        "    b 61b\n"
        "63:\n"
        "    mov x14, x21\n"
        "64:\n"
        "    ldrb w15, [x14], #1\n"
        "    cbz w15, 66f\n"
        "    strb w15, [x22], #1\n"
        "    b 64b\n"
        "66:\n"
        "    mov x19, x13\n"
        "    b 60b\n"
        "62:\n"
        "    strb w12, [x22], #1\n"
        "    add x19, x19, #1\n"
        "    b 60b\n"
        "65:\n"
        "    mov w12, #0\n"
        "    strb w12, [x22]\n"
        "    mov x0, x11\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x21, x22, [sp, #32]\n"
        "    ldp x29, x30, [sp], #48\n"
        "    ret\n"
        "\n"
        "rt_readline:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x0, x9\n"
        "    mov x1, x13\n"
        "    mov x2, #128\n"
        "    mov x8, #42\n"
        "    svc #0\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_gadget_text:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x0, x9\n"
        "    mov x1, x13\n"
        "    mov x2, #128\n"
        "    mov x8, #106\n"
        "    svc #0\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_font_name:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x0, x9\n"
        "    mov x1, x13\n"
        "    mov x2, #128\n"
        "    mov x8, #192\n"
        "    svc #0\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_gfx_driver_name:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x0, x9\n"
        "    mov x1, x13\n"
        "    mov x2, #128\n"
        "    mov x8, #202\n"
        "    svc #0\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_gadget_item_text:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    mov x14, x1\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x0, x9\n"
        "    mov x1, x14\n"
        "    mov x2, x13\n"
        "    mov x3, #128\n"
        "    mov x8, #119\n"
        "    svc #0\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_textarea_text:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    mov x14, x1\n"
        "    mov x15, x2\n"
        "    adrp x11, rt_str_pos\n"
        "    add x11, x11, :lo12:rt_str_pos\n"
        "    ldr x12, [x11]\n"
        "    adrp x13, rt_str_pool\n"
        "    add x13, x13, :lo12:rt_str_pool\n"
        "    add x13, x13, x12\n"
        "    add x12, x12, #128\n"
        "    and x12, x12, #1023\n"
        "    str x12, [x11]\n"
        "    mov x0, x9\n"
        "    mov x1, x14\n"
        "    mov x2, x15\n"
        "    mov x3, x13\n"
        "    mov x4, #128\n"
        "    mov x8, #149\n"
        "    svc #0\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_deg_reduce:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    fmov d0, x0\n"
        "    adrp x9, rt_const_180\n"
        "    add x9, x9, :lo12:rt_const_180\n"
        "    ldr x9, [x9]\n"
        "    fmov d1, x9\n"
        "    fadd d0, d0, d1\n"
        "    adrp x9, rt_const_360\n"
        "    add x9, x9, :lo12:rt_const_360\n"
        "    ldr x9, [x9]\n"
        "    fmov d2, x9\n"
        "    fdiv d3, d0, d2\n"
        "    fcvtzs x10, d3\n"
        "    scvtf d4, x10\n"
        "    fmul d5, d4, d2\n"
        "    fsub d6, d0, d5\n"
        "    fmov x11, d6\n"
        "    cmp x11, #0\n"
        "    bge 80f\n"
        "    fadd d6, d6, d2\n"
        "80:\n"
        "    fsub d6, d6, d1\n"
        "    adrp x9, rt_const_deg2rad\n"
        "    add x9, x9, :lo12:rt_const_deg2rad\n"
        "    ldr x9, [x9]\n"
        "    fmov d7, x9\n"
        "    fmul d0, d6, d7\n"
        "    fmov x0, d0\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_sin:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    bl rt_deg_reduce\n"
        "    fmov d0, x0\n"
        "    fmul d1, d0, d0\n"
        "    adrp x9, rt_sin_c6\n    add x9, x9, :lo12:rt_sin_c6\n    ldr x9, [x9]\n    fmov d2, x9\n"
        "    adrp x9, rt_sin_c5\n    add x9, x9, :lo12:rt_sin_c5\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d2, d2, d1\n    fadd d2, d2, d3\n"
        "    adrp x9, rt_sin_c4\n    add x9, x9, :lo12:rt_sin_c4\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d2, d2, d1\n    fadd d2, d2, d3\n"
        "    adrp x9, rt_sin_c3\n    add x9, x9, :lo12:rt_sin_c3\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d2, d2, d1\n    fadd d2, d2, d3\n"
        "    adrp x9, rt_sin_c2\n    add x9, x9, :lo12:rt_sin_c2\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d2, d2, d1\n    fadd d2, d2, d3\n"
        "    adrp x9, rt_sin_c1\n    add x9, x9, :lo12:rt_sin_c1\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d2, d2, d1\n    fadd d2, d2, d3\n"
        "    adrp x9, rt_sin_c0\n    add x9, x9, :lo12:rt_sin_c0\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d2, d2, d1\n    fadd d2, d2, d3\n"
        "    fmul d0, d2, d0\n"
        "    fmov x0, d0\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "rt_cos:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    bl rt_deg_reduce\n"
        "    fmov d0, x0\n"
        "    fmul d1, d0, d0\n"
        "    adrp x9, rt_cos_c6\n    add x9, x9, :lo12:rt_cos_c6\n    ldr x9, [x9]\n    fmov d2, x9\n"
        "    adrp x9, rt_cos_c5\n    add x9, x9, :lo12:rt_cos_c5\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d2, d2, d1\n    fadd d2, d2, d3\n"
        "    adrp x9, rt_cos_c4\n    add x9, x9, :lo12:rt_cos_c4\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d2, d2, d1\n    fadd d2, d2, d3\n"
        "    adrp x9, rt_cos_c3\n    add x9, x9, :lo12:rt_cos_c3\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d2, d2, d1\n    fadd d2, d2, d3\n"
        "    adrp x9, rt_cos_c2\n    add x9, x9, :lo12:rt_cos_c2\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d2, d2, d1\n    fadd d2, d2, d3\n"
        "    adrp x9, rt_cos_c1\n    add x9, x9, :lo12:rt_cos_c1\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d2, d2, d1\n    fadd d2, d2, d3\n"
        "    adrp x9, rt_cos_c0\n    add x9, x9, :lo12:rt_cos_c0\n    ldr x9, [x9]\n    fmov d3, x9\n"
        "    fmul d2, d2, d1\n    fadd d2, d2, d3\n"
        "    fmov x0, d2\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
    );
}

// ---- funciones definidas por el usuario ----

static void emit_function(Node *fn) {
    FuncScope sc; nb_memset(&sc, 0, sizeof(sc));
    for (int i = 0; i < fn->list_count; i++) local_add_if_new(&sc, fn->list[i]->text);
    collect_locals_stmt(&sc, fn->d);
    sc.frame_size = ((sc.count * 8) + 15) & ~15;
    if (sc.frame_size == 0) sc.frame_size = 16;

    nb_fprintf(out, "\nfunc_%s:\n", fn->text);
    nb_fprintf(out, "    stp x29, x30, [sp, #-16]!\n");
    nb_fprintf(out, "    mov x29, sp\n");
    nb_fprintf(out, "    sub sp, sp, #%d\n", sc.frame_size);
    for (int i = 0; i < fn->list_count && i < 8; i++) {
        nb_fprintf(out, "    str x%d, [x29, #%d]\n", i, sc.locals[i].offset);
    }
    g_current_func_name = fn->text;
    g_in_user_function = true;
    emit_block(&sc, fn->d);
    g_in_user_function = false;
    nb_fprintf(out, "    mov x0, #0\n");
    nb_fprintf(out, ".Lfunc_end_%s:\n", fn->text);
    nb_fprintf(out, "    add sp, x29, #0\n");
    nb_fprintf(out, "    ldp x29, x30, [sp], #16\n");
    nb_fprintf(out, "    ret\n");
}

// ---- punto de entrada del generador ----

void codegen_generate(Node *program, NBOut *output) {
    out = output;

    collect_toplevel_names(program);
    mark_globals_stmt(program);
    collect_data(program);
    collect_type_info(program);

    nb_fprintf(out, "// Generado por el compilador Nemo-Blitz -- no editar a mano.\n");
    nb_fprintf(out, ".section .text.start\n");
    nb_fprintf(out, ".global _start\n");
    nb_fprintf(out, "_start:\n");
    // _start tiene que comportarse como cualquier funcion normal:
    // guardar x30 (direccion de retorno hacia quien nos lanzo) al
    // principio, y restaurarla antes de su propio 'ret'. Si no, cada
    // 'bl' que hagamos por el camino (imprimir un numero, llamar a
    // una funcion...) va sobrescribiendo x30, y al llegar al final
    // saltariamos a la ULTIMA llamada interna en vez de volver a
    // quien nos ejecuto -- el programa se queda saltando dentro de si
    // mismo para siempre, en vez de terminar.
    nb_fprintf(out, "    stp x29, x30, [sp, #-16]!\n");
    nb_fprintf(out, "    mov x29, sp\n");

    // Semilla inicial de Rnd() -- el reloj del sistema (SYS_GET_TICKS,
    // numero 2), para que cada ejecucion de un mismo programa no
    // repita siempre la misma secuencia "aleatoria".
    nb_fprintf(out, "    mov x8, #2\n    svc #0\n");
    nb_fprintf(out, "    adrp x9, rt_rnd_seed\n    add x9, x9, :lo12:rt_rnd_seed\n    str x0, [x9]\n");

    // Color activo por defecto: blanco -- ver la nota equivalente en
    // el compilador del host.
    nb_fprintf(out, "    mov x9, #0xFFFFFF\n");
    nb_fprintf(out, "    adrp x10, rt_current_color\n    add x10, x10, :lo12:rt_current_color\n    str x9, [x10]\n");

    // Puntero de lectura de Data -- empieza al principio de la tabla.
    nb_fprintf(out, "    adrp x9, rt_data_table\n    add x9, x9, :lo12:rt_data_table\n");
    nb_fprintf(out, "    adrp x10, rt_data_ptr\n    add x10, x10, :lo12:rt_data_ptr\n    str x9, [x10]\n");

    // Las sentencias de nivel superior se ejecutan directamente aqui;
    // las N_FUNCDEF se saltan (van despues, como subrutinas aparte).
    FuncScope top; nb_memset(&top, 0, sizeof(top));
    for (int i = 0; i < program->list_count; i++) {
        if (program->list[i]->kind != N_FUNCDEF) emit_stmt(&top, program->list[i]);
    }
    nb_fprintf(out, ".Lprogram_end:\n");
    nb_fprintf(out, "    ldp x29, x30, [sp], #16\n");
    nb_fprintf(out, "    ret\n");

    for (int i = 0; i < program->list_count; i++) {
        if (program->list[i]->kind == N_FUNCDEF) emit_function(program->list[i]);
    }

    emit_runtime_helpers();

    nb_fprintf(out, "\n.section .rodata\n");
    nb_fprintf(out, "rt_half: .quad 0x3FE0000000000000\n");
    nb_fprintf(out, "rt_const_180: .quad 4640537203540230144\n");
    nb_fprintf(out, "rt_const_360: .quad 4645040803167600640\n");
    nb_fprintf(out, "rt_const_deg2rad: .quad 4580687790476533049\n");
    nb_fprintf(out, "rt_sin_c0: .quad 4607182418800017408\n");
    nb_fprintf(out, "rt_sin_c1: .quad 13818544856648471893\n");
    nb_fprintf(out, "rt_sin_c2: .quad 4575957461383581969\n");
    nb_fprintf(out, "rt_sin_c3: .quad 13774824197408792602\n");
    nb_fprintf(out, "rt_sin_c4: .quad 4523617214285662004\n");
    nb_fprintf(out, "rt_sin_c5: .quad 13716528800881525988\n");
    nb_fprintf(out, "rt_sin_c6: .quad 4460272573143870729\n");
    nb_fprintf(out, "rt_cos_c0: .quad 4607182418800017408\n");
    nb_fprintf(out, "rt_cos_c1: .quad 13826050856027422720\n");
    nb_fprintf(out, "rt_cos_c2: .quad 4586165620538955093\n");
    nb_fprintf(out, "rt_cos_c3: .quad 13787419979223755799\n");
    nb_fprintf(out, "rt_cos_c4: .quad 4537941361671905306\n");
    nb_fprintf(out, "rt_cos_c5: .quad 13732177094651715420\n");
    nb_fprintf(out, "rt_cos_c6: .quad 4477122120089393304\n");
    nb_fprintf(out, "rt_const_pi: .quad 4614256656552045848\n");
    nb_fprintf(out, "rt_const_2_31: .quad 4746794007248502784\n");
    nb_fprintf(out, "rt_const_rad2deg: .quad 4633260481411531256\n");
    nb_fprintf(out, "rt_atan_c0: .quad 4607182418800017408\n");
    nb_fprintf(out, "rt_atan_c1: .quad 13823048456275842389\n");
    nb_fprintf(out, "rt_atan_c2: .quad 4596373779694328218\n");
    nb_fprintf(out, "rt_atan_c3: .quad 13817687028148020370\n");
    nb_fprintf(out, "rt_atan_c4: .quad 4592670820000712476\n");
    nb_fprintf(out, "rt_atan_c5: .quad 13814587147885025094\n");
    nb_fprintf(out, "rt_const_ln2: .quad 4604418534313441775\n");
    nb_fprintf(out, "rt_const_log10_recip: .quad 4601495173785380109\n");
    nb_fprintf(out, "rt_exp_c0: .quad 4607182418800017408\n");
    nb_fprintf(out, "rt_exp_c1: .quad 4607182418800017408\n");
    nb_fprintf(out, "rt_exp_c2: .quad 4602678819172646912\n");
    nb_fprintf(out, "rt_exp_c3: .quad 4595172819793696085\n");
    nb_fprintf(out, "rt_exp_c4: .quad 4586165620538955093\n");
    nb_fprintf(out, "rt_exp_c5: .quad 4575957461383581969\n");
    nb_fprintf(out, "rt_exp_c6: .quad 4564047942368979991\n");
    nb_fprintf(out, "rt_exp_c7: .quad 4551452160554016794\n");
    nb_fprintf(out, "rt_exp_c8: .quad 4537941361671905306\n");
    nb_fprintf(out, "rt_exp_c9: .quad 4523617214285662004\n");
    nb_fprintf(out, "rt_log_c1: .quad 4607182418800017408\n");
    nb_fprintf(out, "rt_log_c2: .quad 4599676419421066581\n");
    nb_fprintf(out, "rt_log_c3: .quad 4596373779694328218\n");
    nb_fprintf(out, "rt_log_c4: .quad 4594314991293244562\n");
    nb_fprintf(out, "rt_log_c5: .quad 4592670820000712476\n");
    nb_fprintf(out, "rt_log_c6: .quad 4591215111030249286\n");
    emit_asciz(out, "rt_str_si", "Si");
    emit_asciz(out, "rt_str_no", "No");
    emit_asciz(out, "rt_str_cancelar", "Cancelar");
    emit_asciz(out, "rt_str_entrar", "Entrar");
    emit_asciz(out, "rt_str_elegir", "Elegir");
    emit_asciz(out, "rt_empty_str", "");
    emit_asciz(out, "rt_str_colon", ":");
    for (int i = 0; i < string_count; i++) {
        emit_asciz(out, strings[i].label, strings[i].value);
    }
    for (int i = 0; i < float_const_count; i++) {
        uint64_t bits;
        nb_memcpy(&bits, &float_consts[i].value, sizeof(bits));
        nb_fprintf(out, "%s: .quad %lld\n", float_consts[i].label, (long long)bits);
    }

    nb_fprintf(out, "\nrt_data_table:\n");
    for (int i = 0; i < data_entry_count; i++) {
        DataEntry *e = &data_entries[i];
        if (e->is_label) {
            nb_fprintf(out, "dl_%s:\n", e->label_name);
        } else if (e->is_string) {
            nb_fprintf(out, "    .quad %s\n", e->str_label);
        } else {
            nb_fprintf(out, "    .quad %lld\n", (long long)e->num_value);
        }
    }
    nb_fprintf(out, "rt_data_end:\n");

    nb_fprintf(out, "\n.section .bss\n.align 3\n");
    for (int i = 0; i < global_count; i++) {
        char sym[80];
        sanitize_sym(globals[i], sym, sizeof(sym));
        nb_fprintf(out, "var_%s: .space 8\n", sym);
    }
    for (int i = 0; i < array_count; i++) {
        nb_fprintf(out, "arr_%s: .space %d\n", arrays[i].name, arrays[i].total_size * 8);
    }
    for (int i = 0; i < type_count; i++) {
        int inst_size = (1 + types[i].field_count) * 8;
        nb_fprintf(out, "type_%s_pool: .space %d\n", types[i].name, inst_size * MAX_TYPE_INSTANCES);
        nb_fprintf(out, "type_%s_head: .space 8\n", types[i].name);
        nb_fprintf(out, "type_%s_tail: .space 8\n", types[i].name);
        nb_fprintf(out, "type_%s_next_idx: .space 8\n", types[i].name);
    }
    nb_fprintf(out, "rt_str_pool: .space 1024\n");
    nb_fprintf(out, "rt_str_pos: .space 8\n");
    nb_fprintf(out, "rt_rnd_seed: .space 8\n");
    nb_fprintf(out, "rt_locate_buf: .space 8\n");
    nb_fprintf(out, "rt_cls_color: .space 8\n");
    nb_fprintf(out, "rt_input_echo_buf: .space 8\n");
    nb_fprintf(out, "rt_current_dir_inode: .space 8\n");
    nb_fprintf(out, "rt_current_dir_name: .space 32\n");
    nb_fprintf(out, "rt_file_io_buf: .space 8\n");
    nb_fprintf(out, "rt_copyfile_buf: .space 1024\n");
    nb_fprintf(out, "rt_gosub_stack: .space 128\n");
    nb_fprintf(out, "rt_gosub_sp: .space 8\n");
    nb_fprintf(out, "rt_last_event_id: .space 8\n");
    nb_fprintf(out, "rt_data_ptr: .space 8\n");
    nb_fprintf(out, "rt_current_color: .space 8\n");
    nb_fprintf(out, "rt_requested_r: .space 8\n");
    nb_fprintf(out, "rt_requested_g: .space 8\n");
    nb_fprintf(out, "rt_requested_b: .space 8\n");
    nb_fprintf(out, "rt_dir_listing_buf: .space 1280\n");
    nb_fprintf(out, "rt_dir_item_buf: .space 40\n");
    nb_fprintf(out, "rt_requested_dir_inode: .space 8\n");
}
