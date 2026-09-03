// codegen.c — compilador Nemo-Blitz
//
// Recorre el AST y emite ensamblador AArch64 en TEXTO (compatible con
// aarch64-elf-as). Reglas fijas para todo el codigo que se genera
// aqui:
//
//   1. Los valores flotantes ('nombre#') son doubles de 64 bits IEEE-754,
//      pero se TRANSPORTAN entre expresiones/variables como si fueran
//      enteros de 64 bits -- el mismo patron de bits, movido con x0
//      igual que cualquier otro valor. Solo "se activan" como
//      flotantes de verdad al entrar en una operacion aritmetica
//      (fmov d0,x0 los reinterpreta; scvtf convierte un entero de
//      verdad si hace falta promocionarlo). Todo lo demas sigue
//      siendo entero de 64 bits.
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

#include <stdint.h>
#include "codegen.h"
#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static Node *func_defs[MAX_FUNCS]; // el N_FUNCDEF entero, para poder mirar los valores por defecto de sus parametros
static int func_count = 0;

// ---- arrays declarados con Dim ----
#define MAX_ARRAYS 32
#define MAX_ARRAY_DIMS 4
typedef struct {
    char name[64];
    int dim_sizes[MAX_ARRAY_DIMS]; // tamaño +1 de cada dimension (Dim x(10) -> indices 0..10)
    int dim_count;
    int total_size; // producto de todas las dimensiones, para el .space en .bss
} ArrayInfo;
static ArrayInfo arrays[MAX_ARRAYS];
static int array_count = 0;

// ---- Data / Read / Restore: tabla de constantes recogida de TODO el
// programa (esten donde esten los "Data" y las etiquetas), en el
// orden en que aparecen -- se vuelca despues como una unica tabla en
// .rodata, con las etiquetas como marcadores de posicion reales del
// ensamblador (asi "Restore etiqueta" es sencillamente cargar esa
// direccion en el puntero de lectura, sin tabla de indices aparte). ----
#define MAX_DATA_ENTRIES 1024
typedef struct {
    bool is_label;
    bool is_string;
    char label_name[64];
    double num_value;
    char str_label[32]; // etiqueta ya interned (ver intern_string), no el texto crudo
} DataEntry;
static DataEntry data_entries[MAX_DATA_ENTRIES];
static int data_entry_count = 0;

// ---- Type / Field ----
//
// Cada Type declarado se representa en tiempo de ejecucion como una
// "pool" fija en .bss (nada de reserva dinamica de verdad -- igual
// que los arrays de Dim), con hasta MAX_TYPE_INSTANCES huecos. Cada
// instancia ocupa (1+num_campos)*8 bytes: el primer hueco de 8 bytes
// es un puntero 'next' (lista enlazada SIMPLE, solo hacia adelante --
// de sobra para soportar New/Delete/Each/First/Last), y despues los
// campos, en el mismo orden en que se declararon.
//
// Una instancia se identifica por su DIRECCION en memoria (el mismo
// valor que devuelve New, y el que se compara con '=' o se guarda en
// una variable). "variable.Tipo" en BlitzPlus real es redundante con
// lo que ya dice "= New Tipo" / "= Each Tipo" -- lo aceptamos a nivel
// de lexer mas por fidelidad sintactica que porque lo necesitemos: la
// asociacion variable->tipo se calcula SOLO mirando esos dos sitios.
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

static FILE *out;
static int label_counter = 0;

// literales de cadena recogidos durante la generacion, volcados al
// final en .rodata
#define MAX_STRINGS 256
typedef struct { char label[32]; char value[256]; } StringLit;
static StringLit strings[MAX_STRINGS];
static int string_count = 0;

// literales flotantes: van a .rodata como constantes .double, igual
// que las cadenas van como .asciz -- un literal como "3.14" no cabe
// en un 'mov' inmediato de ARM64 (rango demasiado pequeño), asi que
// se carga desde memoria con adrp+ldr, como cualquier otra constante.
#define MAX_FLOAT_CONSTS 128
typedef struct { char label[32]; double value; } FloatConst;
static FloatConst float_consts[MAX_FLOAT_CONSTS];
static int float_const_count = 0;

static int new_label(void) { return label_counter++; }

// -- pila de "fin del bucle actual", para Exit --
//
// Cada For/While/Repeat empuja su propia etiqueta de fin (con su
// prefijo, porque cada tipo de bucle nombra la suya distinto) antes
// de generar el cuerpo, y la retira al terminar -- asi Exit, este
// donde este anidado, siempre sabe a que etiqueta saltar: la de la
// cima de la pila.
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
static void emit_asciz(FILE *f, const char *label, const char *value) {
    fprintf(f, "%s: .asciz \"", label);
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        switch (*p) {
            case '\n': fputs("\\n", f); break;
            case '\t': fputs("\\t", f); break;
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            default:   fputc((int)*p, f); break;
        }
    }
    fputs("\"\n", f);
}

// Los literales flotantes se vuelcan como .quad con el patron de bits
// IEEE-754 del double ya calculado -- asi no hace falta enseñarle a
// nuestro ensamblador ninguna directiva ".double" nueva.
static void emit_float_const(FILE *f, const char *label, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    fprintf(f, "%s: .quad %lld\n", label, (long long)bits);
}

static const char *intern_string(const char *value) {
    for (int i = 0; i < string_count; i++) {
        if (strcmp(strings[i].value, value) == 0) return strings[i].label;
    }
    snprintf(strings[string_count].label, sizeof(strings[0].label), "str_%d", string_count);
    strncpy(strings[string_count].value, value, sizeof(strings[0].value) - 1);
    const char *lbl = strings[string_count].label;
    string_count++;
    return lbl;
}

static const char *intern_float(double value) {
    for (int i = 0; i < float_const_count; i++) {
        if (float_consts[i].value == value) return float_consts[i].label;
    }
    snprintf(float_consts[float_const_count].label, sizeof(float_consts[0].label), "flt_%d", float_const_count);
    float_consts[float_const_count].value = value;
    const char *lbl = float_consts[float_const_count].label;
    float_const_count++;
    return lbl;
}

// Comparacion de identificadores SIN distinguir mayusculas/minusculas
// -- BlitzPlus real es insensible a mayusculas en TODOS los
// identificadores (variables, tipos, campos, funciones), confirmado
// con la documentacion oficial ("Blitz Docpack": "Identifiers are not
// case sensitive. For example, Test, TEST and test are all the same
// identifiers") y con ejemplos oficiales reales que mezclan
// mayusculas para la MISMA variable sin declararla dos veces (ej.
// "id=WaitEvent()" seguido de "If ID=$803 Then End").
//
// BUG REAL CORREGIDO: una sesion anterior habia asumido (con un
// comentario que decia "los NOMBRES DE VARIABLE si distinguen
// mayusculas") que esto solo aplicaba a nombres de Tipo, dejando las
// VARIABLES sensibles a mayusculas -- verificado con un ejemplo
// oficial real (CreateMenu.bb) que esto rompia programas reales: "id"
// y "ID" se registraban como DOS variables independientes en vez de
// una sola, hacienda que "If ID=$803 Then End" nunca se cumpliera (el
// boton de cerrar ventana no funcionaba).
static int strcasecmp_ascii(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
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
    if (global_count < MAX_GLOBALS) strncpy(globals[global_count++], name, 63);
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
    int len = (int)strlen(name);
    return len > 0 && name[len - 1] == '$';
}

// Variables flotantes: terminan en '#', igual que las de cadena
// terminan en '$'. Los literales flotantes se distinguen porque el
// parser marca n->text[0]='#' si el propio numero llevaba punto
// decimal en el codigo fuente (ver parse_primary).
static bool is_float_name(const char *name) {
    int len = (int)strlen(name);
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
            if (func_count < MAX_FUNCS) { strncpy(func_names[func_count], s->text, 63); func_defs[func_count] = s; func_count++; }
        } else if (s->kind == N_BLOCK) {
            // "Dim a(N),b(N),c(N)" (varios arrays en una sola sentencia
            // Dim) se envuelve en un N_BLOCK por el parser -- hay que
            // entrar a mirar dentro para encontrar cada N_DIM.
            collect_toplevel_names(s);
        } else if (s->kind == N_DIM) {
            if (array_count < MAX_ARRAYS) {
                strncpy(arrays[array_count].name, s->text, 63);
                int dc = s->list_count;
                if (dc > MAX_ARRAY_DIMS) dc = MAX_ARRAY_DIMS;
                int total = 1;
                for (int d = 0; d < dc; d++) {
                    // Los tamaños de Dim tienen que ser literales -- no
                    // hay reserva dinamica en tiempo de ejecucion (eso
                    // seria un proyecto aparte, con heap de verdad).
                    // "Dim x(10)" da indices 0..10, es decir 11 huecos.
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

// Recoge, EN ORDEN, todos los "Data" y etiquetas de nivel superior --
// intencionadamente NO entra en el interior de las funciones (Data va
// casi siempre a nivel de programa, tipicamente al final del archivo).
static void collect_data(Node *block) {
    for (int i = 0; i < block->list_count; i++) {
        Node *s = block->list[i];
        if (s->kind == N_DATALABEL) {
            if (data_entry_count < MAX_DATA_ENTRIES) {
                data_entries[data_entry_count].is_label = true;
                strncpy(data_entries[data_entry_count].label_name, s->text, 63);
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
                    strncpy(e->str_label, intern_string(v->text), sizeof(e->str_label) - 1);
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
// Devuelve el nombre CANONICO del tipo (tal como se escribio en su
// declaracion "Type X") a partir de cualquier variante de mayusculas
// con la que se haya escrito despues (New chair / New CHAIR / New
// Chair deben generar TODOS la misma etiqueta type_Chair_head, no
// tres etiquetas distintas). Si no se encuentra (no deberia pasar si
// ya se valido con find_type antes), devuelve el texto tal cual.
static const char *canon_type_name(const char *raw) {
    TypeInfo *t = find_type(raw);
    return t ? t->name : raw;
}
static const char *find_var_type(const char *varname) {
    for (int i = 0; i < var_type_count; i++) if (strcasecmp_ascii(var_types[i].var_name, varname) == 0) return var_types[i].type_name;
    return NULL;
}
static void add_var_type(const char *varname, const char *tname) {
    if (find_var_type(varname)) return; // la primera declaracion manda
    if (var_type_count < MAX_VAR_TYPES) {
        strncpy(var_types[var_type_count].var_name, varname, 63);
        strncpy(var_types[var_type_count].type_name, tname, 63);
        var_type_count++;
    }
}
// Intenta averiguar de que Tipo es una expresion que representa una
// instancia -- usado por Before (que SI necesita saber en que lista
// buscar, a diferencia de After). Cubre los patrones reales
// observados: una variable ya asociada a un tipo (via New/Each/
// Before/After anteriores), el resultado directo de First/Last Tipo,
// o encadenamientos de Before/After sobre cualquiera de los
// anteriores (el tipo se propaga sin cambiar).
static const char *infer_type_name_from_expr(Node *expr) {
    if (!expr) return NULL;
    if (expr->kind == N_VAR) return find_var_type(expr->text);
    if (expr->kind == N_FIRSTLAST) return expr->text; // First/Last Tipo ya lo dice directamente
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

// Recorre TODO el programa (incluido el interior de las funciones,
// a diferencia de collect_data/mark_globals_stmt) buscando:
//   - declaraciones "Type ... End Type" -- registra sus campos.
//   - "variable = New Tipo" -- asocia esa variable con ese tipo.
//   - "For variable = Each Tipo" -- lo mismo, por este otro camino.
// La primera asociacion que se encuentra para una variable es la que
// vale (no hace falta re-declarar el tipo en cada uso posterior).
static void collect_type_info(Node *n) {
    if (!n) return;
    if (n->kind == N_TYPEDEF) {
        if (type_count < MAX_TYPES) {
            strncpy(types[type_count].name, n->text, 63);
            types[type_count].field_count = 0;
            for (int i = 0; i < n->list_count && i < MAX_FIELDS_PER_TYPE; i++) {
                strncpy(types[type_count].field_names[i], n->list[i]->text, 63);
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
        // igual que con New, pero el nombre del tipo se infiere de la
        // expresion (variable ya tipada, First/Last Tipo, o
        // encadenamiento de Before/After) -- ver infer_type_name_from_expr
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
    if (n->kind == N_FOR) add_global_if_new(n->text); // la variable del bucle es solo texto, no un N_VAR -- la recoleccion generica de mas abajo no la veria por si sola
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
    if (sc->count >= MAX_LOCALS) { fprintf(stderr, "demasiadas variables locales en una funcion\n"); exit(1); }
    strncpy(sc->locals[sc->count].name, name, 63);
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
    if (n->kind == N_FOR) local_add_if_new(sc, n->text); // mismo motivo que en mark_globals_stmt
    collect_locals_expr(sc, n->a); collect_locals_expr(sc, n->b);
    collect_locals_expr(sc, n->c); collect_locals_expr(sc, n->d);
    for (int i = 0; i < n->list_count; i++) collect_locals_stmt(sc, n->list[i]);
}

static Local *find_local(FuncScope *sc, const char *name) {
    for (int i = 0; i < sc->count; i++) if (strcasecmp_ascii(sc->locals[i].name, name) == 0) return &sc->locals[i];
    return NULL;
}

// ---- emision de codigo ----

// true mientras se genera el CUERPO de una funcion con nombre (entre
// emit_function() y su epilogo) -- lo consulta N_RETURN para saber
// si "Return" significa "salir de esta funcion" (b .Lfunc_end_NOMBRE)
// o "volver de un Gosub" (bl rt_gosub_return), ya que ambos casos
// comparten la misma palabra clave en BlitzPlus real.
static bool g_in_user_function = false;
// Nombre de la funcion cuyo cuerpo se esta generando ahora mismo --
// BUG REAL CORREGIDO: antes ".Lfunc_end" era un nombre FIJO, igual
// para todas las funciones, asi que un programa con 2+ funciones
// definidas producia una etiqueta duplicada y el ensamblador la
// rechazaba. Ahora se construye como ".Lfunc_end_NOMBRE", reutilizando
// el mismo patron ya usado (y ya probado) para la etiqueta de entrada
// "func_NOMBRE:" -- los nombres de funcion ya son unicos dentro de un
// programa BlitzPlus, asi que esto es seguro.
static const char *g_current_func_name = NULL;

static ValType emit_expr(FuncScope *sc, Node *n);
static void emit_stmt(FuncScope *sc, Node *n);
static void emit_block(FuncScope *sc, Node *block);

static void push_x0(void) { fprintf(out, "    str x0, [sp, #-16]!\n"); }
static void pop_to_x1(void) { fprintf(out, "    ldr x1, [sp], #16\n"); }

// Carga la direccion de una variable (global o local) en x0. Los
// arrays no pasan por aqui -- ver emit_index.
// Los simbolos de ensamblador no pueden llevar '#' (es el prefijo de
// los inmediatos en la sintaxis ARM, ej. "mov x0, #5") -- asi que una
// variable BlitzPlus como "a#" no puede convertirse tal cual en la
// etiqueta "var_a#". La sustituimos por un sufijo seguro SOLO para el
// nombre que aparece en el .s generado; el nombre real (con su '#')
// sigue viviendo en n->text para todo lo demas, como la deteccion de
// tipo (is_float_name). TAMBIEN normalizamos a minusculas -- BlitzPlus
// real es insensible a mayusculas en los nombres de variable, asi que
// "id" e "ID" deben generar la MISMA etiqueta "var_id" (si no, dos
// referencias a la "misma" variable con distinta capitalizacion
// generarian dos simbolos de ensamblador DISTINTOS, cada uno con su
// propia zona de memoria -- el mismo bug que motivo este cambio,
// version silenciosa: no fallaria al ensamblar, pero la variable se
// comportaria como si tuviera dos copias independientes).
static void sanitize_sym(const char *name, char *out, size_t out_size) {
    size_t i = 0, j = 0;
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
        fprintf(out, "    add x0, x29, #%d\n", loc->offset);
    } else {
        char sym[80];
        sanitize_sym(name, sym, sizeof(sym));
        fprintf(out, "    adrp x0, var_%s\n", sym);
        fprintf(out, "    add x0, x0, :lo12:var_%s\n", sym);
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
            if (strcmp(n->text, "Str$") == 0 || strcmp(n->text, "Chr$") == 0) return TY_STRING;
            return is_string_name(n->text) ? TY_STRING : is_float_name(n->text) ? TY_FLOAT : TY_INT;
        case N_INDEX: return TY_INT; // arrays de enteros unicamente en v1
        case N_FIELD: return is_string_name(n->text) ? TY_STRING : is_float_name(n->text) ? TY_FLOAT : TY_INT;
        case N_NEW: return TY_INT; // una instancia es, en el fondo, su direccion
        case N_FIRSTLAST: return TY_INT;
        case N_BEFORE: case N_AFTER: return TY_INT; // igual: una instancia (o 0) es su direccion
        case N_UNOP: return infer_type(sc, n->a); // -x# sigue siendo flotante
        case N_BINOP: {
            ValType lt = infer_type(sc, n->a), rt = infer_type(sc, n->b);
            if (n->op == T_PLUS && (lt == TY_STRING || rt == TY_STRING)) return TY_STRING;
            if (is_arith_or_cmp_op(n->op) && (lt == TY_FLOAT || rt == TY_FLOAT)) {
                return is_cmp_op(n->op) ? TY_INT : TY_FLOAT; // las comparaciones siempre dan un entero (0/1)
            }
            return TY_INT;
        }
        default: return TY_INT;
    }
}

static void emit_index_address(FuncScope *sc, Node *n) {
    ArrayInfo *arr = find_array(n->text);
    if (!arr) { fprintf(stderr, "linea %d: array '%s' no declarado con Dim\n", n->line, n->text); exit(1); }
    int dc = n->list_count;
    if (dc > arr->dim_count) dc = arr->dim_count;

    // Regla de Horner para arrays de N dimensiones, en orden de fila
    // (row-major): offset = ((i0*dim1 + i1)*dim2 + i2)*dim3 + i3 ...
    // Cada indice se evalua por separado (puede ser una expresion
    // cualquiera), asi que el acumulado se guarda en la pila entre
    // medias -- emit_expr no promete conservar ningun registro.
    emit_expr(sc, n->list[0]); // x0 = indice_0 (acumulado inicial)
    for (int d = 1; d < dc; d++) {
        push_x0(); // acumulado hasta ahora
        emit_expr(sc, n->list[d]); // x0 = indice_d
        fprintf(out, "    mov x1, x0\n");
        fprintf(out, "    ldr x0, [sp], #16\n"); // x0 = acumulado
        fprintf(out, "    mov x2, #%d\n", arr->dim_sizes[d]);
        fprintf(out, "    mul x0, x0, x2\n");
        fprintf(out, "    add x0, x0, x1\n");
    }
    fprintf(out, "    mov x1, x0\n"); // x1 = offset final, en elementos
    fprintf(out, "    adrp x0, arr_%s\n", n->text);
    fprintf(out, "    add x0, x0, :lo12:arr_%s\n", n->text);
    fprintf(out, "    add x0, x0, x1, lsl #3\n"); // x0 = base + offset*8
}

// var\campo -- deja en x0 la DIRECCION del campo (offset 8 = despues
// del puntero 'next' de la lista enlazada, + el indice del campo*8).
static void emit_field_address(FuncScope *sc, Node *n) {
    const char *tname = (n->a->kind == N_VAR) ? find_var_type(n->a->text) : NULL;
    if (!tname) {
        fprintf(stderr, "linea %d: no se pudo determinar el tipo de la variable para el campo '%s' (¿le falta un '= New Tipo' antes?)\n", n->line, n->text);
        exit(1);
    }
    int field_idx = find_field_offset(tname, n->text);
    if (field_idx < 0) {
        fprintf(stderr, "linea %d: el tipo '%s' no tiene un campo '%s'\n", n->line, tname, n->text);
        exit(1);
    }
    emit_expr(sc, n->a); // x0 = puntero a la instancia
    fprintf(out, "    add x0, x0, #%d\n", 8 + field_idx * 8);
}

static ValType emit_expr(FuncScope *sc, Node *n) {
    switch (n->kind) {
        case N_NUM:
            if (is_float_literal(n)) {
                // No cabe en un 'mov' inmediato (rango insuficiente) --
                // lo cargamos desde .rodata como cualquier otra
                // constante. x0 lleva el patron de bits del double,
                // TRANSPORTADO como si fuera un entero cualquiera --
                // solo se "activa" como flotante de verdad cuando algo
                // lo usa en una operacion aritmetica (fmov d0,x0).
                const char *lbl = intern_float(n->num_value);
                fprintf(out, "    adrp x0, %s\n", lbl);
                fprintf(out, "    add x0, x0, :lo12:%s\n", lbl);
                fprintf(out, "    ldr x0, [x0]\n");
                return TY_FLOAT;
            }
            fprintf(out, "    mov x0, #%lld\n", (long long)n->num_value);
            return TY_INT;

        case N_STR: {
            const char *lbl = intern_string(n->text);
            fprintf(out, "    adrp x0, %s\n", lbl);
            fprintf(out, "    add x0, x0, :lo12:%s\n", lbl);
            return TY_STRING;
        }

        case N_VAR: {
            emit_var_address(sc, n->text);
            fprintf(out, "    ldr x0, [x0]\n");
            return is_string_name(n->text) ? TY_STRING : TY_INT;
        }

        case N_INDEX: {
            emit_index_address(sc, n);
            fprintf(out, "    ldr x0, [x0]\n");
            return TY_INT;
        }

        case N_FIELD: {
            emit_field_address(sc, n);
            fprintf(out, "    ldr x0, [x0]\n");
            return is_string_name(n->text) ? TY_STRING : is_float_name(n->text) ? TY_FLOAT : TY_INT;
        }

        case N_NEW: {
            // Reservamos el siguiente hueco libre de la pool de este
            // tipo (el contador solo crece -- ver nota de diseño mas
            // arriba: Delete NO devuelve el hueco a un banco de
            // reciclado, asi que un programa que cree y borre muchas
            // instancias en un bucle largo puede agotar la pool).
            TypeInfo *t = find_type(n->text);
            if (!t) { fprintf(stderr, "linea %d: tipo '%s' no declarado con Type\n", n->line, n->text); exit(1); }
            const char *ctype = canon_type_name(n->text); // misma etiqueta pase lo que pase con las mayusculas usadas aqui
            int inst_size = (1 + t->field_count) * 8;
            fprintf(out, "    adrp x9, type_%s_next_idx\n", ctype);
            fprintf(out, "    add x9, x9, :lo12:type_%s_next_idx\n", ctype);
            fprintf(out, "    ldr x10, [x9]\n");
            fprintf(out, "    add x11, x10, #1\n");
            fprintf(out, "    str x11, [x9]\n");
            fprintf(out, "    mov x12, #%d\n", inst_size);
            fprintf(out, "    mul x10, x10, x12\n");
            fprintf(out, "    adrp x11, type_%s_pool\n", ctype);
            fprintf(out, "    add x11, x11, :lo12:type_%s_pool\n", ctype);
            fprintf(out, "    add x11, x11, x10\n"); // x11 = direccion de la nueva instancia
            fprintf(out, "    mov x13, #0\n");
            for (int i = 0; i <= t->field_count; i++) { // next + todos los campos, a cero
                fprintf(out, "    str x13, [x11, #%d]\n", i * 8);
            }
            // enlazamos al final de la lista de este tipo
            fprintf(out, "    adrp x9, type_%s_tail\n", ctype);
            fprintf(out, "    add x9, x9, :lo12:type_%s_tail\n", ctype);
            fprintf(out, "    ldr x10, [x9]\n");
            int l = new_label();
            fprintf(out, "    cbz x10, .Lnew_empty_%d\n", l);
            fprintf(out, "    str x11, [x10]\n"); // tail->next = nueva instancia
            fprintf(out, "    b .Lnew_linked_%d\n", l);
            fprintf(out, ".Lnew_empty_%d:\n", l);
            fprintf(out, "    adrp x12, type_%s_head\n", ctype);
            fprintf(out, "    add x12, x12, :lo12:type_%s_head\n", ctype);
            fprintf(out, "    str x11, [x12]\n"); // head = nueva instancia (lista vacia hasta ahora)
            fprintf(out, ".Lnew_linked_%d:\n", l);
            fprintf(out, "    str x11, [x9]\n"); // tail = nueva instancia
            fprintf(out, "    mov x0, x11\n");
            return TY_INT;
        }

        case N_FIRSTLAST: {
            const char *which = n->op == T_KW_FIRST ? "head" : "tail";
            const char *ctype = canon_type_name(n->text);
            fprintf(out, "    adrp x0, type_%s_%s\n", ctype, which);
            fprintf(out, "    add x0, x0, :lo12:type_%s_%s\n", ctype, which);
            fprintf(out, "    ldr x0, [x0]\n");
            return TY_INT;
        }

        case N_AFTER: {
            // Simple: el "siguiente" de una instancia es directamente
            // su campo 'next' (offset 0) -- la lista solo esta
            // enlazada hacia adelante. No necesita saber el tipo, asi
            // que acepta CUALQUIER expresion (confirmado con un
            // ejemplo real: "After First File").
            emit_expr(sc, n->a); // evalua la expresion -> x0 (la instancia, o 0)
            int l = new_label();
            fprintf(out, "    cbz x0, .Lafter_null_%d\n", l);
            fprintf(out, "    ldr x0, [x0]\n"); // campo 'next'
            fprintf(out, ".Lafter_null_%d:\n", l);
            return TY_INT;
        }

        case N_BEFORE: {
            // No hay puntero 'anterior' guardado (lista SOLO enlazada
            // hacia adelante) -- recorremos desde la cabeza de la
            // lista de este tipo hasta encontrar el nodo cuyo 'next'
            // sea nuestra instancia objetivo. Si es la cabeza (no hay
            // anterior) o no se encuentra, devolvemos 0. A diferencia
            // de After, SI necesita saber el tipo (para elegir la
            // lista donde buscar) -- se infiere de la expresion misma.
            const char *tname = infer_type_name_from_expr(n->a);
            if (!tname) {
                fprintf(stderr, "linea %d: no se pudo determinar el tipo de la instancia en 'Before' (¿le falta un '= New Tipo' antes, o usar 'First Tipo'/'Last Tipo'?)\n", n->line);
                exit(1);
            }
            tname = canon_type_name(tname);
            emit_expr(sc, n->a); // x0 = instancia objetivo
            fprintf(out, "    mov x9, x0\n");
            fprintf(out, "    adrp x10, type_%s_head\n", tname);
            fprintf(out, "    add x10, x10, :lo12:type_%s_head\n", tname);
            fprintf(out, "    ldr x10, [x10]\n"); // x10 = nodo actual, arranca en head
            int l = new_label();
            fprintf(out, ".Lbefore_loop_%d:\n", l);
            fprintf(out, "    cbz x10, .Lbefore_none_%d\n", l);
            fprintf(out, "    ldr x11, [x10]\n"); // next del nodo actual
            fprintf(out, "    cmp x11, x9\n");
            fprintf(out, "    beq .Lbefore_found_%d\n", l);
            fprintf(out, "    mov x10, x11\n");
            fprintf(out, "    b .Lbefore_loop_%d\n", l);
            fprintf(out, ".Lbefore_found_%d:\n", l);
            fprintf(out, "    mov x0, x10\n");
            fprintf(out, "    b .Lbefore_done_%d\n", l);
            fprintf(out, ".Lbefore_none_%d:\n", l);
            fprintf(out, "    mov x0, #0\n");
            fprintf(out, ".Lbefore_done_%d:\n", l);
            return TY_INT;
        }

        case N_UNOP: {
            ValType t = emit_expr(sc, n->a);
            if (n->op == T_MINUS) {
                if (t == TY_FLOAT) {
                    fprintf(out, "    fmov d0, x0\n    fneg d0, d0\n    fmov x0, d0\n");
                    return TY_FLOAT;
                }
                fprintf(out, "    neg x0, x0\n");
                return TY_INT;
            }
            fprintf(out, "    cmp x0, #0\n    cset x0, eq\n"); // Not
            return TY_INT;
        }

        case N_BINOP: {
            ValType lt = infer_type(sc, n->a);
            ValType rt = infer_type(sc, n->b);

            if (n->op == T_PLUS && (lt == TY_STRING || rt == TY_STRING)) {
                emit_expr(sc, n->a);
                if (lt == TY_FLOAT) fprintf(out, "    bl rt_float_to_str\n");
                else if (lt == TY_INT) fprintf(out, "    bl rt_int_to_str\n");
                push_x0();
                emit_expr(sc, n->b);
                if (rt == TY_FLOAT) fprintf(out, "    bl rt_float_to_str\n");
                else if (rt == TY_INT) fprintf(out, "    bl rt_int_to_str\n");
                pop_to_x1(); // x1 = a (cadena), x0 = b (cadena)
                fprintf(out, "    bl rt_str_concat\n"); // concat(x1, x0) -> x0
                return TY_STRING;
            }

            // Aritmetica en coma flotante: si CUALQUIERA de los dos
            // operandos es flotante, promocionamos el otro (si es
            // entero) y operamos con registros d0/d1. Las
            // comparaciones siguen devolviendo un entero (0/1), como
            // siempre -- solo +,-,*,/ dan un resultado flotante.
            if (is_arith_or_cmp_op(n->op) && (lt == TY_FLOAT || rt == TY_FLOAT)) {
                emit_expr(sc, n->a);
                if (lt == TY_FLOAT) fprintf(out, "    fmov d0, x0\n");
                else fprintf(out, "    scvtf d0, x0\n");
                fprintf(out, "    fmov x0, d0\n");
                push_x0();
                emit_expr(sc, n->b);
                if (rt == TY_FLOAT) fprintf(out, "    fmov d0, x0\n");
                else fprintf(out, "    scvtf d0, x0\n");
                fprintf(out, "    fmov x0, d0\n");
                pop_to_x1(); // x1 = bits de a (flotante), x0 = bits de b (flotante)
                fprintf(out, "    fmov d0, x1\n");
                fprintf(out, "    fmov d1, x0\n");
                switch (n->op) {
                    case T_PLUS:  fprintf(out, "    fadd d0, d0, d1\n"); break;
                    case T_MINUS: fprintf(out, "    fsub d0, d0, d1\n"); break;
                    case T_STAR:  fprintf(out, "    fmul d0, d0, d1\n"); break;
                    case T_SLASH: fprintf(out, "    fdiv d0, d0, d1\n"); break;
                    case T_EQ: fprintf(out, "    fcmp d0, d1\n    cset x0, eq\n"); return TY_INT;
                    case T_NE: fprintf(out, "    fcmp d0, d1\n    cset x0, ne\n"); return TY_INT;
                    case T_LT: fprintf(out, "    fcmp d0, d1\n    cset x0, mi\n"); return TY_INT;
                    case T_GT: fprintf(out, "    fcmp d0, d1\n    cset x0, gt\n"); return TY_INT;
                    case T_LE: fprintf(out, "    fcmp d0, d1\n    cset x0, ls\n"); return TY_INT;
                    case T_GE: fprintf(out, "    fcmp d0, d1\n    cset x0, ge\n"); return TY_INT;
                    default: fprintf(stderr, "linea %d: operador no valido entre flotantes\n", n->line); exit(1);
                }
                fprintf(out, "    fmov x0, d0\n");
                return TY_FLOAT;
            }

            emit_expr(sc, n->a);
            push_x0();
            emit_expr(sc, n->b);
            pop_to_x1(); // x1 = a, x0 = b -- listos para operar sin mas barajeo
            switch (n->op) {
                case T_PLUS:  fprintf(out, "    add x0, x1, x0\n"); break;
                case T_MINUS: fprintf(out, "    sub x0, x1, x0\n"); break;
                case T_STAR:  fprintf(out, "    mul x0, x1, x0\n"); break;
                case T_SLASH: fprintf(out, "    sdiv x0, x1, x0\n"); break;
                case T_KW_MOD:
                    fprintf(out, "    sdiv x2, x1, x0\n");
                    fprintf(out, "    msub x0, x2, x0, x1\n");
                    break;
                case T_EQ: fprintf(out, "    cmp x1, x0\n    cset x0, eq\n"); break;
                case T_NE: fprintf(out, "    cmp x1, x0\n    cset x0, ne\n"); break;
                case T_LT: fprintf(out, "    cmp x1, x0\n    cset x0, lt\n"); break;
                case T_GT: fprintf(out, "    cmp x1, x0\n    cset x0, gt\n"); break;
                case T_LE: fprintf(out, "    cmp x1, x0\n    cset x0, le\n"); break;
                case T_GE: fprintf(out, "    cmp x1, x0\n    cset x0, ge\n"); break;
                case T_KW_AND: fprintf(out, "    and x0, x1, x0\n"); break;
                case T_KW_OR:  fprintf(out, "    orr x0, x1, x0\n"); break;
                case T_KW_XOR: fprintf(out, "    eor x0, x1, x0\n"); break;
                case T_KW_SHL: fprintf(out, "    lsl x0, x1, x0\n"); break;
                case T_KW_SHR: fprintf(out, "    lsr x0, x1, x0\n"); break;
                case T_KW_SAR: fprintf(out, "    asr x0, x1, x0\n"); break;
                default: fprintf(stderr, "operador binario no soportado\n"); exit(1);
            }
            return TY_INT;
        }

        case N_CALL: {
            if (strcmp(n->text, "Str$") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                fprintf(out, "    bl %s\n", t == TY_FLOAT ? "rt_float_to_str" : "rt_int_to_str");
                return TY_STRING;
            }

            // -- funciones de cadenas --
            if (strcmp(n->text, "Len") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_strlen\n");
                return TY_INT;
            }
            if (strcmp(n->text, "Left$") == 0 || strcmp(n->text, "Right$") == 0 ||
                strcmp(n->text, "Left") == 0 || strcmp(n->text, "Right") == 0) {
                bool is_left = strcmp(n->text, "Left$") == 0 || strcmp(n->text, "Left") == 0;
                const char *helper = is_left ? "rt_left" : "rt_right";
                emit_expr(sc, n->list[0]); push_x0(); // cadena
                emit_expr(sc, n->list[1]);             // n -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl %s\n", helper);
                return TY_STRING;
            }
            if (strcmp(n->text, "Mid$") == 0 || strcmp(n->text, "Mid") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // cadena
                emit_expr(sc, n->list[1]); push_x0(); // inicio
                if (n->list_count > 2) emit_expr(sc, n->list[2]);
                else fprintf(out, "    mov x0, #-1\n"); // sin longitud -> hasta el final
                fprintf(out, "    mov x2, x0\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl rt_mid\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "Upper$") == 0 || strcmp(n->text, "Lower$") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl %s\n", strcmp(n->text, "Upper$") == 0 ? "rt_upper" : "rt_lower");
                return TY_STRING;
            }
            if (strcmp(n->text, "Trim$") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_trim\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "LSet$") == 0 || strcmp(n->text, "RSet$") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // cadena
                emit_expr(sc, n->list[1]);             // n -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl %s\n", strcmp(n->text, "LSet$") == 0 ? "rt_lset" : "rt_rset");
                return TY_STRING;
            }
            if (strcmp(n->text, "Hex$") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_hex\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "Bin$") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_bin\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "String$") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // cadena
                emit_expr(sc, n->list[1]);             // repeticiones -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl rt_string_repeat\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "Locate") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]);             // y -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl rt_locate\n");
                return TY_INT;
            }
            if (strcmp(n->text, "StringWidth") == 0) {
                // Ahora respeta la escala real de la fuente activa
                // (SetFont) -- antes usaba un multiplicador fijo de 6,
                // que quedaba desactualizado en cuanto Text empezo a
                // dibujar de verdad a mayor tamaño.
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_strlen\n");
                push_x0(); // longitud de la cadena
                fprintf(out, "    mov x8, #206\n    svc #0\n"); // SYS_FONT_CHAR_ADVANCE
                fprintf(out, "    mov x1, x0\n"); // avance por caracter
                fprintf(out, "    ldr x0, [sp], #16\n"); // longitud
                fprintf(out, "    mul x0, x0, x1\n");
                return TY_INT;
            }
            if (strcmp(n->text, "StringHeight") == 0) {
                emit_expr(sc, n->list[0]); // se evalua por si tiene efectos secundarios
                fprintf(out, "    mov x8, #196\n    svc #0\n"); // SYS_FONT_HEIGHT -- ya respeta la escala activa
                return TY_INT;
            }
            if (strcmp(n->text, "ColorRed") == 0 || strcmp(n->text, "ColorGreen") == 0 || strcmp(n->text, "ColorBlue") == 0) {
                fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n    ldr x0, [x9]\n");
                if (strcmp(n->text, "ColorRed") == 0) fprintf(out, "    lsr x0, x0, #16\n    and x0, x0, #0xFF\n");
                else if (strcmp(n->text, "ColorGreen") == 0) fprintf(out, "    lsr x0, x0, #8\n    and x0, x0, #0xFF\n");
                else fprintf(out, "    and x0, x0, #0xFF\n");
                return TY_INT;
            }
            if (strcmp(n->text, "ClsColor") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // r
                emit_expr(sc, n->list[1]); push_x0(); // g
                emit_expr(sc, n->list[2]);             // b -> x0
                fprintf(out, "    and x3, x0, #0xFF\n");
                fprintf(out, "    ldr x2, [sp], #16\n    and x2, x2, #0xFF\n    lsl x2, x2, #8\n");
                fprintf(out, "    ldr x1, [sp], #16\n    and x1, x1, #0xFF\n    lsl x1, x1, #16\n");
                fprintf(out, "    orr x0, x1, x2\n    orr x0, x0, x3\n");
                fprintf(out, "    adrp x9, rt_cls_color\n    add x9, x9, :lo12:rt_cls_color\n    str x0, [x9]\n");
                return TY_INT;
            }
            if (strcmp(n->text, "GraphicsWidth") == 0 || strcmp(n->text, "GraphicsHeight") == 0) {
                fprintf(out, "    mov x8, #33\n    svc #0\n"); // SYS_GET_WINDOW_SIZE
                if (strcmp(n->text, "GraphicsWidth") == 0) fprintf(out, "    lsr x0, x0, #32\n");
                else fprintf(out, "    and x0, x0, #0xFFFFFFFF\n");
                return TY_INT;
            }
            if (strcmp(n->text, "GraphicsDepth") == 0) {
                fprintf(out, "    mov x0, #32\n"); // siempre color verdadero de 32 bits
                return TY_INT;
            }
            if (strcmp(n->text, "CountGFXModes") == 0) {
                fprintf(out, "    mov x0, #1\n"); // solo tenemos un modo, el actual
                return TY_INT;
            }
            if (strcmp(n->text, "GFXModeWidth") == 0 || strcmp(n->text, "GFXModeHeight") == 0) {
                emit_expr(sc, n->list[0]); // indice de modo -- ignorado, solo hay uno
                fprintf(out, "    mov x8, #35\n    svc #0\n"); // SYS_GET_SCREEN_SIZE
                if (strcmp(n->text, "GFXModeWidth") == 0) fprintf(out, "    lsr x0, x0, #32\n");
                else fprintf(out, "    and x0, x0, #0xFFFFFFFF\n");
                return TY_INT;
            }
            if (strcmp(n->text, "GFXModeDepth") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x0, #32\n");
                return TY_INT;
            }
            if (strcmp(n->text, "GfxModeExists") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // w esperado
                emit_expr(sc, n->list[1]); push_x0(); // h esperado
                if (n->list_count > 2) emit_expr(sc, n->list[2]); // profundidad, ignorada
                fprintf(out, "    mov x8, #35\n    svc #0\n"); // SYS_GET_SCREEN_SIZE
                fprintf(out, "    lsr x9, x0, #32\n");
                fprintf(out, "    and x10, x0, #0xFFFFFFFF\n");
                fprintf(out, "    ldr x11, [sp], #16\n"); // h esperado
                fprintf(out, "    ldr x12, [sp], #16\n"); // w esperado
                fprintf(out, "    cmp x12, x9\n    cset x13, eq\n");
                fprintf(out, "    cmp x11, x10\n    cset x14, eq\n");
                fprintf(out, "    and x0, x13, x14\n");
                return TY_INT;
            }
            if (strcmp(n->text, "SetGfxDriver") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "AppTitle") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #130\n    svc #0\n"); // SYS_SET_TITLE
                return TY_INT;
            }
            if (strcmp(n->text, "AutoSuspend") == 0) {
                // LIMITACION DOCUMENTADA: controla si el programa se
                // pausa solo al minimizarse -- ya detectamos
                // minimizado/maximizado (MinimizeWindow/WindowMinimized),
                // pero pausar la TAREA de verdad en el planificador es
                // una pieza aparte, no implementada. Se acepta el
                // parametro (se evalua por si tiene efectos
                // secundarios) pero no hace nada por ahora.
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "SetGadgetFont") == 0) {
                // LIMITACION DOCUMENTADA: necesita un "handle de
                // fuente" de LoadFont (Fase 5, sin implementar) --
                // nuestro sistema de fuentes es un bitmap fijo 5x7,
                // sin concepto de fuentes cargables todavia. Se
                // aceptan y evaluan ambos parametros (por si tienen
                // efectos secundarios) pero no hacen nada por ahora.
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "CommandLine$") == 0) {
                fprintf(out, "    bl rt_commandline\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "CurrentDate$") == 0) {
                fprintf(out, "    bl rt_currentdate\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "CurrentTime$") == 0) {
                fprintf(out, "    bl rt_currenttime\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "Stop") == 0) {
                // LIMITACION DOCUMENTADA (verificada contra el manual):
                // en BlitzPlus real, Stop es un comando de DEPURACION
                // -- "Para la ejecucion del programa durante la
                // depuracion... te permite parar la ejecucion y
                // devolverte al editor donde puedes avanzar linea a
                // linea". No es lo mismo que End (que termina el
                // programa del todo). Nemo OS no tiene un depurador
                // paso a paso con el que "retomar" la ejecucion, asi
                // que como unica alternativa razonable, Stop se
                // comporta igual que End.
                fprintf(out, "    mov x0, #0\n    mov x8, #0\n    svc #0\n"); // SYS_EXIT
                return TY_INT;
            }
            if (strcmp(n->text, "FreeTimer") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #131\n    svc #0\n"); // SYS_FREE_TIMER
                return TY_INT;
            }
            if (strcmp(n->text, "WaitTimer") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_waittimer\n");
                return TY_INT;
            }
            if (strcmp(n->text, "CountGfxDrivers") == 0) {
                fprintf(out, "    mov x0, #1\n");
                return TY_INT;
            }
            if (strcmp(n->text, "AvailVidMem") == 0) {
                fprintf(out, "    mov x0, #67108864\n"); // valor fijo razonable (64MB), no tenemos el concepto real
                return TY_INT;
            }
            if (strcmp(n->text, "ScanLine") == 0) {
                fprintf(out, "    mov x0, #0\n"); // sin hardware de barrido real que consultar
                return TY_INT;
            }
            if (strcmp(n->text, "VWait") == 0) {
                fprintf(out, "    mov x8, #14\n    svc #0\n"); // SYS_PUMP -- cede el control, como Flip
                return TY_INT;
            }
            if (strcmp(n->text, "Origin") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]);             // y -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #85\n    svc #0\n"); // SYS_SET_ORIGIN
                return TY_INT;
            }
            if (strcmp(n->text, "Viewport") == 0) {
                // Recorta el dibujo a un rectangulo, SEPARADO de Origin
                // (que solo desplaza, sin recortar) -- coincide con los
                // dos metodos distintos que usa BlitzPlus real
                // (setOrigin vs setViewport). Sin argumentos w,h
                // (o con 0), desactiva el recorte.
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else fprintf(out, "    mov x0, #0\n");
                push_x0(); // ancho
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else fprintf(out, "    mov x0, #0\n");
                // alto -> x0
                fprintf(out, "    mov x3, x0\n");
                fprintf(out, "    ldr x2, [sp], #16\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #135\n    svc #0\n"); // SYS_SET_VIEWPORT
                return TY_INT;
            }
            if (strcmp(n->text, "GetColor") == 0) {
                // En BlitzPlus real, GetColor(x,y) es un COMANDO que
                // fija el color de dibujo ACTUAL al del pixel leido
                // (se usa como "GetColor x,y", no como "c=GetColor(x,y)").
                // Mantenemos tambien el valor de retorno por
                // flexibilidad, pero el efecto real es este.
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]);             // y -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #86\n    svc #0\n"); // SYS_GET_PIXEL
                fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n    str x0, [x9]\n");
                return TY_INT;
            }
            if (strcmp(n->text, "CopyRect") == 0) {
                // src/dest son OPCIONALES (handle de imagen) -- si se
                // omiten, se usa la ventana actual, igual que en
                // BlitzPlus real ("CopyRect sx,sy,w,h,dx,dy" sin mas).
                emit_expr(sc, n->list[0]); push_x0(); // sx
                emit_expr(sc, n->list[1]); push_x0(); // sy
                emit_expr(sc, n->list[2]); push_x0(); // ancho
                emit_expr(sc, n->list[3]); push_x0(); // alto
                emit_expr(sc, n->list[4]); push_x0(); // dx
                emit_expr(sc, n->list[5]); push_x0(); // dy
                if (n->list_count > 6) emit_expr(sc, n->list[6]);
                else fprintf(out, "    mov x0, #0\n    sub x0, x0, #1\n"); // -1 = ventana, por defecto
                push_x0(); // src
                if (n->list_count > 7) emit_expr(sc, n->list[7]);
                else fprintf(out, "    mov x0, #0\n    sub x0, x0, #1\n"); // dest -> x0
                fprintf(out, "    add x0, x0, #1\n"); // dst_enc (0=ventana, handle+1=imagen)
                fprintf(out, "    mov x12, x0\n");
                fprintf(out, "    ldr x9, [sp], #16\n"); // src
                fprintf(out, "    add x9, x9, #1\n"); // src_enc
                fprintf(out, "    ldr x10, [sp], #16\n"); // dy
                fprintf(out, "    ldr x11, [sp], #16\n"); // dx
                fprintf(out, "    lsl x11, x11, #32\n");
                fprintf(out, "    orr x4, x11, x10\n"); // (dx<<32|dy)
                fprintf(out, "    ldr x13, [sp], #16\n"); // alto
                fprintf(out, "    lsl x12, x12, #16\n");
                fprintf(out, "    orr x3, x12, x13\n"); // (dst_enc<<16|alto)
                fprintf(out, "    ldr x13, [sp], #16\n"); // ancho
                fprintf(out, "    lsl x9, x9, #16\n");
                fprintf(out, "    orr x2, x9, x13\n"); // (src_enc<<16|ancho)
                fprintf(out, "    ldr x1, [sp], #16\n"); // sy
                fprintf(out, "    ldr x0, [sp], #16\n"); // sx
                fprintf(out, "    mov x8, #87\n    svc #0\n"); // SYS_COPY_RECT
                return TY_INT;
            }
            if (strcmp(n->text, "FreeImage") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #88\n    svc #0\n"); // SYS_FREE_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "LoadSound$") == 0 || strcmp(n->text, "LoadSound") == 0) {
                // LoadSound(filename$) -- .wav real (PCM sin comprimir),
                // convertido al cargar a 44100Hz/16 bits/estereo.
                emit_expr(sc, n->list[0]); // nombre$ -> x0
                fprintf(out, "    mov x8, #226\n    svc #0\n"); // SYS_LOAD_SOUND
                return TY_INT;
            }
            if (strcmp(n->text, "FreeSound") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #227\n    svc #0\n"); // SYS_FREE_SOUND
                return TY_INT;
            }
            if (strcmp(n->text, "PlaySound") == 0) {
                // PlaySound(sonido[,flags]) -- BLOQUEA hasta que
                // termina de sonar (V1 sincrono, ver limitacion
                // documentada junto a las syscalls de sonido).
                // Devuelve un "canal" -- en V1 es el mismo handle del
                // sonido, ya que no existe un canal persistente
                // despues de que esta llamada ya ha vuelto.
                emit_expr(sc, n->list[0]); push_x0(); // sonido
                if (n->list_count > 1) emit_expr(sc, n->list[1]); // flags -- se evalua, sin efecto real
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #228\n    svc #0\n"); // SYS_PLAY_SOUND
                return TY_INT;
            }
            if (strcmp(n->text, "SoundVolume") == 0) {
                // El kernel no puede usar coma flotante de verdad (ver
                // la nota junto a sound_volume_permil en syscall.c) --
                // convertimos aqui mismo, con hardware real (esto es
                // ensamblado de USUARIO, sin esa restriccion), a un
                // entero "por mil" (0-1000) antes de la syscall.
                emit_expr(sc, n->list[0]); push_x0(); // sonido
                ValType t = emit_expr(sc, n->list[1]); // volumen#
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n"); else fprintf(out, "    fmov d0, x0\n");
                fprintf(out, "    mov x9, #1000\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #229\n    svc #0\n"); // SYS_SOUND_VOLUME
                return TY_INT;
            }
            if (strcmp(n->text, "SoundPan") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // sonido
                ValType t = emit_expr(sc, n->list[1]); // pan#
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n"); else fprintf(out, "    fmov d0, x0\n");
                fprintf(out, "    mov x9, #1000\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #230\n    svc #0\n"); // SYS_SOUND_PAN
                return TY_INT;
            }
            if (strcmp(n->text, "SoundPitch") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // sonido
                emit_expr(sc, n->list[1]); // hertz -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #231\n    svc #0\n"); // SYS_SOUND_PITCH
                return TY_INT;
            }
            if (strcmp(n->text, "PauseChannel") == 0 || strcmp(n->text, "ResumeChannel") == 0 ||
                strcmp(n->text, "StopChannel") == 0 || strcmp(n->text, "LoopSound") == 0) {
                // LIMITACION DOCUMENTADA: V1 reproduce de forma SINCRONA
                // (PlaySound bloquea hasta que termina de sonar), asi
                // que no existe un canal en curso sobre el que estos
                // comandos puedan actuar de verdad -- se acepta el
                // argumento (por si tiene efectos secundarios) sin
                // cambiar nada real. Ver la nota grande en sound.c.
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "ChannelPlaying") == 0) {
                // Misma limitacion -- en V1, cuando esta consulta se
                // pudiera hacer, la reproduccion (sincrona) ya termino
                // siempre, asi que devolvemos 'false' de forma honesta.
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "ChannelVolume") == 0 || strcmp(n->text, "ChannelPan") == 0 ||
                strcmp(n->text, "ChannelPitch") == 0) {
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "PlayMusic") == 0 || strcmp(n->text, "PlayCDTrack") == 0) {
                // LIMITACION DOCUMENTADA: pensados para musica de fondo
                // en bucle/streaming -- nuestro almacen de sonidos
                // (V1) tiene un limite de 5 segundos por sonido y no
                // hace streaming, asi que una "musica" real quedaria
                // truncada de forma poco util. Se acepta el argumento
                // sin reproducir nada.
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "HandleImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]);             // y -> x0
                fprintf(out, "    mov x2, x0\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #89\n    svc #0\n"); // SYS_SET_IMAGE_HANDLE
                return TY_INT;
            }
            if (strcmp(n->text, "MidHandle") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle, lo necesitamos dos veces
                fprintf(out, "    ldr x0, [sp]\n"); // consultamos sin sacarlo aun
                fprintf(out, "    mov x8, #51\n    svc #0\n"); // SYS_IMAGE_SIZE -> (ancho<<32|alto)
                fprintf(out, "    lsr x1, x0, #33\n"); // ancho/2 (lsr 32 aisla el ancho, +1 mas lo divide entre 2)
                fprintf(out, "    and x2, x0, #0xFFFFFFFF\n");
                fprintf(out, "    lsr x2, x2, #1\n"); // alto/2
                fprintf(out, "    ldr x0, [sp], #16\n"); // handle, ahora si lo sacamos
                fprintf(out, "    mov x8, #89\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "AutoMidHandle") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #91\n    svc #0\n"); // SYS_SET_AUTO_MID_HANDLE
                return TY_INT;
            }
            if (strcmp(n->text, "ImageXHandle") == 0 || strcmp(n->text, "ImageYHandle") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #90\n    svc #0\n"); // SYS_GET_IMAGE_HANDLE -> (handle_x<<32|handle_y)
                if (strcmp(n->text, "ImageXHandle") == 0) fprintf(out, "    asr x0, x0, #32\n");
                else fprintf(out, "    lsl x0, x0, #32\n    asr x0, x0, #32\n");
                return TY_INT;
            }
            if (strcmp(n->text, "MaskImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]); push_x0(); // r
                emit_expr(sc, n->list[2]); push_x0(); // g
                emit_expr(sc, n->list[3]);             // b -> x0
                fprintf(out, "    and x3, x0, #0xFF\n");
                fprintf(out, "    ldr x2, [sp], #16\n    and x2, x2, #0xFF\n    lsl x2, x2, #8\n");
                fprintf(out, "    ldr x1, [sp], #16\n    and x1, x1, #0xFF\n    lsl x1, x1, #16\n");
                fprintf(out, "    orr x1, x1, x2\n    orr x1, x1, x3\n"); // color empaquetado -> x1
                fprintf(out, "    ldr x0, [sp], #16\n"); // handle
                fprintf(out, "    mov x8, #92\n    svc #0\n"); // SYS_MASK_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "CopyImage") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #93\n    svc #0\n"); // SYS_COPY_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "SaveImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]);             // nombre$ -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #94\n    svc #0\n"); // SYS_SAVE_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "GrabImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                emit_expr(sc, n->list[2]); push_x0(); // ancho
                emit_expr(sc, n->list[3]);             // alto -> x0
                fprintf(out, "    mov x3, x0\n");
                fprintf(out, "    ldr x2, [sp], #16\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #95\n    svc #0\n"); // SYS_GRAB_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "DrawBlock") == 0) {
                // A diferencia de DrawImage, DrawBlock es OPACO --
                // ignora la transparencia por completo (bit 31 de a3
                // activado). Misma distincion que hace BlitzPlus real.
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else fprintf(out, "    mov x0, #0\n"); // frame -> x0
                fprintf(out, "    mov x9, #1\n    lsl x9, x9, #31\n");
                fprintf(out, "    orr x3, x0, x9\n"); // frame | bit_solido
                fprintf(out, "    ldr x2, [sp], #16\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #50\n    svc #0\n"); // SYS_DRAW_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "TFormFilter") == 0) {
                // Solo tenemos filtrado "vecino mas cercano" -- se
                // acepta el argumento (por si tiene efectos secundarios)
                // pero no cambia nada de verdad.
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "TFormImage") == 0) {
                // TFormImage(image,a#,b#,c#,d#) -- transforma la
                // imagen IN PLACE segun la matriz 2x2 (a b; c d),
                // centrada. El kernel no puede usar coma flotante de
                // verdad (ver la nota junto a FP_SHIFT en syscall.c)
                // -- convertimos cada componente aqui mismo a punto
                // fijo Q16.16 (x65536) antes de la syscall.
                emit_expr(sc, n->list[0]); push_x0(); // handle de imagen
                ValType ta = emit_expr(sc, n->list[1]);
                if (ta != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n"); else fprintf(out, "    fmov d0, x0\n");
                fprintf(out, "    mov x9, #65536\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                push_x0(); // a# (Q16.16)
                ValType tb = emit_expr(sc, n->list[2]);
                if (tb != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n"); else fprintf(out, "    fmov d0, x0\n");
                fprintf(out, "    mov x9, #65536\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                push_x0(); // b# (Q16.16)
                ValType tc = emit_expr(sc, n->list[3]);
                if (tc != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n"); else fprintf(out, "    fmov d0, x0\n");
                fprintf(out, "    mov x9, #65536\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                push_x0(); // c# (Q16.16)
                ValType td = emit_expr(sc, n->list[4]); // d# -> x0
                if (td != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n"); else fprintf(out, "    fmov d0, x0\n");
                fprintf(out, "    mov x9, #65536\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                fprintf(out, "    mov x4, x0\n"); // x4 = d# (Q16.16)
                fprintf(out, "    ldr x3, [sp], #16\n"); // x3 = c#
                fprintf(out, "    ldr x2, [sp], #16\n"); // x2 = b#
                fprintf(out, "    ldr x1, [sp], #16\n"); // x1 = a#
                fprintf(out, "    ldr x0, [sp], #16\n"); // x0 = handle de imagen
                fprintf(out, "    mov x8, #225\n    svc #0\n"); // SYS_TFORM_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "ImagesOverlap") == 0) {
                // Igual que ImagesCollide, pero sin parametros de
                // fotograma (no distinguimos animaciones todavia).
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #51\n    svc #0\n");
                push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[3]);
                fprintf(out, "    mov x8, #51\n    svc #0\n");
                push_x0();
                emit_expr(sc, n->list[4]); push_x0();
                emit_expr(sc, n->list[5]); push_x0();

                fprintf(out, "    ldr x5, [sp], #16\n");
                fprintf(out, "    ldr x4, [sp], #16\n");
                fprintf(out, "    ldr x9, [sp], #16\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    ldr x8, [sp], #16\n");
                fprintf(out, "    lsr x2, x8, #32\n");
                fprintf(out, "    and x3, x8, #0xFFFFFFFF\n");
                fprintf(out, "    lsr x6, x9, #32\n");
                fprintf(out, "    and x7, x9, #0xFFFFFFFF\n");

                int l = new_label();
                fprintf(out, "    add x8, x4, x6\n    cmp x0, x8\n    bge .Lio_false_%d\n", l);
                fprintf(out, "    add x8, x0, x2\n    cmp x4, x8\n    bge .Lio_false_%d\n", l);
                fprintf(out, "    add x8, x5, x7\n    cmp x1, x8\n    bge .Lio_false_%d\n", l);
                fprintf(out, "    add x8, x1, x3\n    cmp x5, x8\n    bge .Lio_false_%d\n", l);
                fprintf(out, "    mov x0, #1\n    b .Lio_done_%d\n", l);
                fprintf(out, ".Lio_false_%d:\n    mov x0, #0\n", l);
                fprintf(out, ".Lio_done_%d:\n", l);
                return TY_INT;
            }
            if (strcmp(n->text, "ImageRectOverlap") == 0 || strcmp(n->text, "ImageRectCollide") == 0) {
                // ImageRectOverlap: img,x,y,rx,ry,rw,rh (7 args).
                // ImageRectCollide: img,x,y,FRAME,rx,ry,rw,rh (8 args,
                // BlitzPlus real inserta el fotograma en la posicion 3,
                // desplazando el resto) -- lo aceptamos pero lo
                // ignoramos, ya que no rastreamos un tamaño de caja
                // distinto por fotograma.
                bool is_collide = strcmp(n->text, "ImageRectCollide") == 0;
                int rect_base = is_collide ? 4 : 3;

                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #51\n    svc #0\n");
                push_x0(); // (ancho<<32|alto) de la imagen
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                if (is_collide) emit_expr(sc, n->list[3]); // fotograma -- se evalua por si tiene efectos secundarios, pero se ignora
                emit_expr(sc, n->list[rect_base]); push_x0(); // rx
                emit_expr(sc, n->list[rect_base + 1]); push_x0(); // ry
                emit_expr(sc, n->list[rect_base + 2]); push_x0(); // rw
                emit_expr(sc, n->list[rect_base + 3]);             // rh -> x0

                fprintf(out, "    mov x7, x0\n");        // rh
                fprintf(out, "    ldr x6, [sp], #16\n");  // rw
                fprintf(out, "    ldr x5, [sp], #16\n");   // ry
                fprintf(out, "    ldr x4, [sp], #16\n");    // rx
                fprintf(out, "    ldr x1, [sp], #16\n");     // y
                fprintf(out, "    ldr x0, [sp], #16\n");      // x
                fprintf(out, "    ldr x8, [sp], #16\n");       // (ancho<<32|alto)
                fprintf(out, "    lsr x2, x8, #32\n");            // ancho de la imagen
                fprintf(out, "    and x3, x8, #0xFFFFFFFF\n");     // alto de la imagen

                int l = new_label();
                fprintf(out, "    add x8, x4, x6\n    cmp x0, x8\n    bge .Lirc_false_%d\n", l);
                fprintf(out, "    add x8, x0, x2\n    cmp x4, x8\n    bge .Lirc_false_%d\n", l);
                fprintf(out, "    add x8, x5, x7\n    cmp x1, x8\n    bge .Lirc_false_%d\n", l);
                fprintf(out, "    add x8, x1, x3\n    cmp x5, x8\n    bge .Lirc_false_%d\n", l);
                fprintf(out, "    mov x0, #1\n    b .Lirc_done_%d\n", l);
                fprintf(out, ".Lirc_false_%d:\n    mov x0, #0\n", l);
                fprintf(out, ".Lirc_done_%d:\n", l);
                return TY_INT;
            }
            if (strcmp(n->text, "ResizeImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]); push_x0(); // ancho
                emit_expr(sc, n->list[2]);             // alto -> x0
                fprintf(out, "    mov x2, x0\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #96\n    svc #0\n"); // SYS_RESIZE_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "ScaleImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                ValType tx = emit_expr(sc, n->list[1]); // xscale -> x0
                if (tx != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                push_x0();
                ValType ty = emit_expr(sc, n->list[2]); // yscale -> x0
                if (ty != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    fmov d1, x0\n"); // yscale
                fprintf(out, "    ldr x9, [sp], #16\n");
                fprintf(out, "    fmov d0, x9\n"); // xscale
                fprintf(out, "    ldr x0, [sp]\n"); // consultamos el handle sin sacarlo aun
                fprintf(out, "    mov x8, #51\n    svc #0\n"); // SYS_IMAGE_SIZE
                fprintf(out, "    lsr x9, x0, #32\n");
                fprintf(out, "    and x10, x0, #0xFFFFFFFF\n");
                fprintf(out, "    scvtf d2, x9\n    scvtf d3, x10\n");
                fprintf(out, "    fmul d2, d2, d0\n    fmul d3, d3, d1\n");
                fprintf(out, "    fcvtzs x1, d2\n    fcvtzs x2, d3\n");
                fprintf(out, "    ldr x0, [sp], #16\n"); // handle
                fprintf(out, "    mov x8, #96\n    svc #0\n"); // SYS_RESIZE_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "RotateImage") == 0) {
                // El kernel no puede usar coma flotante de verdad (ver
                // la nota junto a FP_SHIFT en syscall.c) -- convertimos
                // el angulo aqui mismo a punto fijo Q16.16 (x65536)
                // antes de la syscall, con hardware real.
                emit_expr(sc, n->list[0]); push_x0(); // handle
                ValType t = emit_expr(sc, n->list[1]); // angulo -> x0
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n"); else fprintf(out, "    fmov d0, x0\n");
                fprintf(out, "    mov x9, #65536\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #97\n    svc #0\n"); // SYS_ROTATE_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "DrawImageRect") == 0 || strcmp(n->text, "DrawBlockRect") == 0) {
                bool is_block = strcmp(n->text, "DrawBlockRect") == 0;
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                emit_expr(sc, n->list[3]); push_x0(); // rx
                emit_expr(sc, n->list[4]); push_x0(); // ry
                emit_expr(sc, n->list[5]); push_x0(); // rw
                emit_expr(sc, n->list[6]);             // rh -> x0
                fprintf(out, "    mov x9, x0\n"); // rh
                fprintf(out, "    ldr x10, [sp], #16\n"); // rw
                fprintf(out, "    lsl x10, x10, #16\n");
                fprintf(out, "    orr x4, x10, x9\n"); // (rw<<16|rh)
                fprintf(out, "    ldr x10, [sp], #16\n"); // ry
                fprintf(out, "    ldr x11, [sp], #16\n"); // rx
                fprintf(out, "    lsl x11, x11, #16\n");
                fprintf(out, "    orr x3, x11, x10\n"); // (rx<<16|ry)
                if (is_block) {
                    // DrawBlockRect es OPACO -- ignora la transparencia
                    // (bit 31 de a3), a diferencia de DrawImageRect.
                    fprintf(out, "    mov x9, #1\n    lsl x9, x9, #31\n");
                    fprintf(out, "    orr x3, x3, x9\n");
                }
                fprintf(out, "    ldr x2, [sp], #16\n"); // y
                fprintf(out, "    ldr x1, [sp], #16\n"); // x
                fprintf(out, "    ldr x0, [sp], #16\n"); // handle
                fprintf(out, "    mov x8, #98\n    svc #0\n"); // SYS_DRAW_IMAGE_RECT
                return TY_INT;
            }
            if (strcmp(n->text, "LoadAnimImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // nombre$
                emit_expr(sc, n->list[1]); push_x0(); // cellwidth
                emit_expr(sc, n->list[2]);             // cellheight -> x0
                fprintf(out, "    mov x9, x0\n");
                fprintf(out, "    ldr x10, [sp], #16\n");
                fprintf(out, "    lsl x10, x10, #16\n");
                fprintf(out, "    orr x0, x10, x9\n"); // (cellwidth<<16|cellheight) -> x0
                push_x0(); // empaquetado de celda
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else fprintf(out, "    mov x0, #0\n"); // first -> x0
                push_x0();
                if (n->list_count > 4) emit_expr(sc, n->list[4]); else fprintf(out, "    mov x0, #0\n"); // count -> x0 (0 = todos los que quepan, por defecto)
                fprintf(out, "    mov x3, x0\n"); // count
                fprintf(out, "    ldr x2, [sp], #16\n"); // first
                fprintf(out, "    ldr x1, [sp], #16\n"); // empaquetado de celda
                fprintf(out, "    ldr x0, [sp], #16\n"); // nombre$
                fprintf(out, "    mov x8, #99\n    svc #0\n"); // SYS_LOAD_ANIM_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "TileImage") == 0 || strcmp(n->text, "TileBlock") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                if (n->list_count > 1) emit_expr(sc, n->list[1]); else fprintf(out, "    mov x0, #0\n");
                push_x0(); // x (0 por defecto, para el uso simple TileImage(img))
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else fprintf(out, "    mov x0, #0\n");
                // y -> x0
                fprintf(out, "    mov x2, x0\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x3, #%d\n", strcmp(n->text, "TileBlock") == 0 ? 1 : 0); // TileBlock=opaco, TileImage=mezcla alfa
                fprintf(out, "    bl rt_tileimage\n");
                return TY_INT;
            }
            if (strcmp(n->text, "Chr$") == 0 || strcmp(n->text, "Chr") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_chr\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "Asc") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_asc\n");
                return TY_INT;
            }
            if (strcmp(n->text, "Instr") == 0) {
                // Instr(cadena$, buscada$, [offset]) -> posicion (base
                // 1), 0 si no aparece. offset es OPCIONAL (confirmado
                // en el manual) -- posicion desde donde empezar a
                // buscar, por defecto 1 (desde el principio).
                emit_expr(sc, n->list[0]); push_x0(); // cadena
                emit_expr(sc, n->list[1]); push_x0(); // buscada
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else fprintf(out, "    mov x0, #1\n");
                fprintf(out, "    mov x2, x0\n"); // offset
                fprintf(out, "    ldr x1, [sp], #16\n"); // buscada
                fprintf(out, "    ldr x0, [sp], #16\n"); // cadena
                fprintf(out, "    bl rt_instr\n");
                return TY_INT;
            }
            if (strcmp(n->text, "Replace$") == 0) {
                // Nombre real confirmado en el manual (con signo de
                // dolar, ya que devuelve una cadena) -- "Replace" sin
                // el simbolo NUNCA coincide con como lo tokeniza el
                // lexer (el $ forma parte del nombre), asi que un
                // programa real que llamara a Replace$(...) fallaba
                // con "funcion no declarada".
                emit_expr(sc, n->list[0]); push_x0(); // cadena
                emit_expr(sc, n->list[1]); push_x0(); // buscada
                emit_expr(sc, n->list[2]);             // sustituta -> x0
                fprintf(out, "    mov x2, x0\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl rt_replace\n");
                return TY_STRING;
            }

            // -- funciones numericas --
            if (strcmp(n->text, "Abs") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_abs\n");
                return TY_INT;
            }
            if (strcmp(n->text, "Sgn") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_sgn\n");
                return TY_INT;
            }
            if (strcmp(n->text, "Min") == 0 || strcmp(n->text, "Max") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl %s\n", strcmp(n->text, "Min") == 0 ? "rt_min" : "rt_max");
                return TY_INT;
            }
            if (strcmp(n->text, "Rnd") == 0) {
                // Version FLOTANTE de verdad (asi es Rnd en BlitzPlus
                // real) -- Rnd(max) es [0,max), Rnd(min,max) es [min,max).
                if (n->list_count >= 2) {
                    ValType t0 = emit_expr(sc, n->list[0]);
                    if (t0 != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                    push_x0();
                    ValType t1 = emit_expr(sc, n->list[1]);
                    if (t1 != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                    fprintf(out, "    mov x1, x0\n");
                    fprintf(out, "    ldr x0, [sp], #16\n");
                } else {
                    ValType t = emit_expr(sc, n->list[0]);
                    if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                    fprintf(out, "    mov x1, x0\n");
                    fprintf(out, "    mov x0, #0\n"); // 0.0 en bits IEEE-754 es simplemente 0
                }
                fprintf(out, "    bl rt_rnd_float\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "Rand") == 0) {
                // Version ENTERA. El limite inferior es OPCIONAL y por
                // defecto es 1 (confirmado en el manual: "low value =
                // opcional - por defecto 1"), NO 0.
                if (n->list_count >= 2) {
                    emit_expr(sc, n->list[0]); push_x0();
                    emit_expr(sc, n->list[1]);
                    fprintf(out, "    mov x1, x0\n");
                    fprintf(out, "    ldr x0, [sp], #16\n");
                } else {
                    emit_expr(sc, n->list[0]);
                    fprintf(out, "    mov x1, x0\n");
                    fprintf(out, "    mov x0, #1\n"); // limite inferior por defecto = 1
                }
                fprintf(out, "    bl rt_rand\n");
                return TY_INT;
            }
            if (strcmp(n->text, "SeedRnd") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_seedrnd\n");
                return TY_INT;
            }
            if (strcmp(n->text, "RndSeed") == 0) {
                fprintf(out, "    bl rt_rndseed\n");
                return TY_INT;
            }
            if (strcmp(n->text, "KeyHit") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #53\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "GetKey") == 0) {
                // BlitzPlus real devuelve el valor ASCII, no el
                // scancode -- "si necesitas SHIFT/ALT... usa KeyHit o
                // KeyDown" (confirmado en el manual). Reutilizamos la
                // misma cola de caracteres que ya usa Input$.
                fprintf(out, "    mov x8, #12\n    svc #0\n"); // SYS_READ_CHAR
                return TY_INT;
            }
            if (strcmp(n->text, "WaitKey") == 0) {
                fprintf(out, "    mov x8, #13\n    svc #0\n"); // SYS_READ_CHAR_WAIT
                return TY_INT;
            }
            if (strcmp(n->text, "FlushKeys") == 0) {
                fprintf(out, "    mov x8, #55\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "FlushMouse") == 0) {
                fprintf(out, "    mov x8, #138\n    svc #0\n"); // SYS_FLUSH_MOUSE
                return TY_INT;
            }
            if (strcmp(n->text, "MoveMouse") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]);             // y -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #58\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "MouseHit") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #56\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "GetMouse") == 0) {
                // BlitzPlus real: "Comprueba si un boton del raton
                // esta siendo pulsado y devuelve el NUMERO del boton"
                // (1/2/3), o 0 si ninguno -- no el valor empaquetado
                // de SYS_GET_MOUSE. Si hay varios pulsados a la vez,
                // solo devuelve uno (izquierdo primero).
                int l = new_label();
                fprintf(out, "    mov x8, #34\n    svc #0\n"); // SYS_GET_MOUSE
                fprintf(out, "    and x9, x0, #1\n"); // bit0 = izquierdo
                fprintf(out, "    cbz x9, .Lgm_chkright_%d\n", l);
                fprintf(out, "    mov x0, #1\n");
                fprintf(out, "    b .Lgm_done_%d\n", l);
                fprintf(out, ".Lgm_chkright_%d:\n", l);
                fprintf(out, "    lsr x9, x0, #1\n");
                fprintf(out, "    and x9, x9, #1\n"); // bit1 = derecho
                fprintf(out, "    cbz x9, .Lgm_chkmid_%d\n", l);
                fprintf(out, "    mov x0, #2\n");
                fprintf(out, "    b .Lgm_done_%d\n", l);
                fprintf(out, ".Lgm_chkmid_%d:\n", l);
                fprintf(out, "    lsr x9, x0, #2\n");
                fprintf(out, "    and x9, x9, #1\n"); // bit2 = central
                fprintf(out, "    cbz x9, .Lgm_none_%d\n", l);
                fprintf(out, "    mov x0, #3\n");
                fprintf(out, "    b .Lgm_done_%d\n", l);
                fprintf(out, ".Lgm_none_%d:\n", l);
                fprintf(out, "    mov x0, #0\n");
                fprintf(out, ".Lgm_done_%d:\n", l);
                return TY_INT;
            }
            if (strcmp(n->text, "WaitMouse") == 0) {
                fprintf(out, "    bl rt_waitmouse\n");
                return TY_INT;
            }
            if (strcmp(n->text, "MouseXSpeed") == 0) {
                fprintf(out, "    mov x8, #57\n    svc #0\n");
                fprintf(out, "    asr x0, x0, #32\n");
                return TY_INT;
            }
            if (strcmp(n->text, "MouseYSpeed") == 0) {
                fprintf(out, "    mov x8, #57\n    svc #0\n");
                fprintf(out, "    lsl x0, x0, #32\n    asr x0, x0, #32\n");
                return TY_INT;
            }
            if (strcmp(n->text, "MouseZ") == 0) {
                // Posicion ACUMULADA de la rueda (nunca se resetea al
                // leerla) -- DISTINTA de MouseXSpeed/YSpeed, que si
                // consumen su valor.
                fprintf(out, "    mov x8, #137\n    svc #0\n"); // SYS_GET_MOUSE_Z
                return TY_INT;
            }
            if (strcmp(n->text, "MouseZSpeed") == 0) {
                // -1/0/1 segun el SIGNO del delta de rueda consumido
                // desde la ultima lectura (a diferencia de MouseZ, que
                // es la posicion acumulada y nunca se resetea).
                fprintf(out, "    mov x8, #45\n    svc #0\n"); // SYS_GET_MOUSE_WHEEL
                fprintf(out, "    cmp x0, #0\n");
                fprintf(out, "    cset x9, gt\n"); // 1 si > 0
                fprintf(out, "    cmp x0, #0\n");
                fprintf(out, "    cset x10, lt\n"); // 1 si < 0
                fprintf(out, "    sub x0, x9, x10\n"); // 1, 0 o -1
                return TY_INT;
            }
            if (strcmp(n->text, "Input$") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]);
                else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    bl rt_input\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "CreateBank") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #59\n    svc #0\n"); // SYS_CREATE_BANK
                return TY_INT;
            }
            if (strcmp(n->text, "FreeBank") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #60\n    svc #0\n"); // SYS_FREE_BANK
                return TY_INT;
            }
            if (strcmp(n->text, "BankSize") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #61\n    svc #0\n"); // SYS_BANK_SIZE
                return TY_INT;
            }
            if (strcmp(n->text, "ResizeBank") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]);             // nuevo_tamaño -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #62\n    svc #0\n"); // SYS_RESIZE_BANK
                return TY_INT;
            }
            if (strcmp(n->text, "CopyBank") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // banco origen
                emit_expr(sc, n->list[1]); push_x0(); // offset origen
                emit_expr(sc, n->list[2]); push_x0(); // banco destino
                emit_expr(sc, n->list[3]); push_x0(); // offset destino
                emit_expr(sc, n->list[4]);             // cantidad -> x0
                fprintf(out, "    mov x4, x0\n");
                fprintf(out, "    ldr x3, [sp], #16\n");
                fprintf(out, "    ldr x2, [sp], #16\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #63\n    svc #0\n"); // SYS_COPY_BANK
                return TY_INT;
            }
            if (strcmp(n->text, "ReadBytes") == 0) {
                // ReadBytes bank,archivo,offset,cantidad
                emit_expr(sc, n->list[0]); push_x0(); // banco
                emit_expr(sc, n->list[1]); push_x0(); // archivo
                emit_expr(sc, n->list[2]); push_x0(); // offset
                emit_expr(sc, n->list[3]);             // cantidad -> x0
                fprintf(out, "    mov x3, x0\n");
                fprintf(out, "    ldr x2, [sp], #16\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #220\n    svc #0\n"); // SYS_READ_BYTES_BANK
                return TY_INT;
            }
            if (strcmp(n->text, "WriteBytes") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // banco
                emit_expr(sc, n->list[1]); push_x0(); // archivo
                emit_expr(sc, n->list[2]); push_x0(); // offset
                emit_expr(sc, n->list[3]);             // cantidad -> x0
                fprintf(out, "    mov x3, x0\n");
                fprintf(out, "    ldr x2, [sp], #16\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #221\n    svc #0\n"); // SYS_WRITE_BYTES_BANK
                return TY_INT;
            }
            if (strcmp(n->text, "PeekByte") == 0 || strcmp(n->text, "PeekShort") == 0 || strcmp(n->text, "PeekInt") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]);             // offset -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                int sysnum = strcmp(n->text, "PeekByte") == 0 ? 64 : (strcmp(n->text, "PeekShort") == 0 ? 65 : 66);
                fprintf(out, "    mov x8, #%d\n    svc #0\n", sysnum);
                return TY_INT;
            }
            if (strcmp(n->text, "PeekFloat") == 0) {
                // Los bancos guardan floats de 32 bits (BlitzPlus
                // real), nosotros usamos 64 -- reutilizamos la misma
                // syscall que PeekInt (los mismos 4 bytes en crudo) y
                // convertimos con FCVT a nuestra representacion interna.
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]);             // offset -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #66\n    svc #0\n"); // SYS_PEEK_INT
                fprintf(out, "    fmov s0, w0\n");
                fprintf(out, "    fcvt d0, s0\n");
                fprintf(out, "    fmov x0, d0\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "PokeByte") == 0 || strcmp(n->text, "PokeShort") == 0 || strcmp(n->text, "PokeInt") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]); push_x0(); // offset
                emit_expr(sc, n->list[2]);             // valor -> x0
                fprintf(out, "    mov x2, x0\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                int sysnum = strcmp(n->text, "PokeByte") == 0 ? 67 : (strcmp(n->text, "PokeShort") == 0 ? 68 : 69);
                fprintf(out, "    mov x8, #%d\n    svc #0\n", sysnum);
                return TY_INT;
            }
            if (strcmp(n->text, "PokeFloat") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]); push_x0(); // offset
                ValType t = emit_expr(sc, n->list[2]); // valor -> x0
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    fmov d0, x0\n");
                fprintf(out, "    fcvt s0, d0\n"); // reducimos a precision simple (con perdida, esperada)
                fprintf(out, "    fmov w2, s0\n");   // los 32 bits resultantes -> tercer argumento
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #69\n    svc #0\n"); // SYS_POKE_INT
                return TY_INT;
            }
            if (strcmp(n->text, "Pi") == 0) {
                fprintf(out, "    adrp x9, rt_const_pi\n    add x9, x9, :lo12:rt_const_pi\n    ldr x0, [x9]\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "Float") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "Int") == 0) {
                // Convierte a entero truncando hacia cero (convencion
                // universal de Int() en BASIC, y la misma que ya usa
                // este compilador para la coercion automatica al
                // asignar un flotante a una variable entera).
                ValType t = emit_expr(sc, n->list[0]);
                if (t == TY_FLOAT) {
                    fprintf(out, "    fmov d0, x0\n    fcvtzs x0, d0\n");
                }
                return TY_INT;
            }
            if (strcmp(n->text, "Floor#") == 0 || strcmp(n->text, "Ceil#") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    fmov d0, x0\n");
                fprintf(out, "    %s d0, d0\n", strcmp(n->text, "Floor#") == 0 ? "frintm" : "frintp");
                fprintf(out, "    fmov x0, d0\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "Tan") == 0) {
                // Tan(x) = Sin(x)/Cos(x) -- necesitamos el angulo dos
                // veces (una para cada llamada), asi que lo guardamos
                // en la pila mientras tanto.
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                push_x0(); // [angulo]
                fprintf(out, "    bl rt_sin\n");
                push_x0(); // [angulo, sin(x)]
                fprintf(out, "    ldr x0, [sp, #16]\n"); // recuperamos el angulo (sin desapilar)
                fprintf(out, "    bl rt_cos\n");
                fprintf(out, "    fmov d1, x0\n");         // d1 = cos(x)
                fprintf(out, "    ldr x0, [sp], #16\n");    // saca sin(x), sp ahora apunta al angulo
                fprintf(out, "    add sp, sp, #16\n");       // descarta tambien el angulo
                fprintf(out, "    fmov d0, x0\n");
                fprintf(out, "    fdiv d0, d0, d1\n");
                fprintf(out, "    fmov x0, d0\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "ATan") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    bl rt_atan\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "ASin") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    bl rt_asin\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "ACos") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    bl rt_acos\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "ATan2") == 0) {
                ValType t0 = emit_expr(sc, n->list[0]); // y
                if (t0 != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                push_x0();
                ValType t1 = emit_expr(sc, n->list[1]); // x
                if (t1 != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl rt_atan2\n");
                return TY_FLOAT;
            }

            // -- bucle de eventos estilo BlitzPlus --
            if (strcmp(n->text, "CreateWindow") == 0) {
                // CreateWindow(titulo$, x, y, ancho, alto[, grupo[, style]])
                // -- LIMITACION DOCUMENTADA: 'grupo'/'style' se
                // evaluan (por si tienen efectos secundarios) pero NO
                // se aplican. 'style' en BlitzPlus real controla
                // bordes/redimensionable/menu/barra de estado/tipo
                // herramienta mediante banderas de bit (confirmado
                // contra la documentacion oficial: 1=barra de titulo,
                // 2=redimensionable, 4=con menu, 8=con barra de
                // estado, 16=ventana herramienta, 32=coordenadas de
                // cliente; 15=por defecto). Implementar esto de
                // verdad exigiria que la altura de la barra de titulo
                // (TITLE_BAR_H) fuera por-ventana en vez de una
                // constante fija, lo que toca demasiados sitios
                // interdependientes de wm.c (arrastre, dibujo, calculo
                // de coordenadas) como para hacerlo con seguridad sin
                // poder probarlo visualmente en QEMU. Por ahora, TODA
                // ventana se crea con barra de titulo + bordes,
                // independientemente del 'style' pedido.
                emit_expr(sc, n->list[0]); push_x0(); // titulo
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                emit_expr(sc, n->list[3]); push_x0(); // ancho
                emit_expr(sc, n->list[4]); push_x0(); // alto
                if (n->list_count > 5) emit_expr(sc, n->list[5]); // grupo -- evaluado, ignorado
                if (n->list_count > 6) emit_expr(sc, n->list[6]); // style -- evaluado, ignorado
                fprintf(out, "    ldr x4, [sp], #16\n"); // alto
                fprintf(out, "    ldr x3, [sp], #16\n"); // ancho
                fprintf(out, "    ldr x2, [sp], #16\n"); // y
                fprintf(out, "    ldr x1, [sp], #16\n"); // x
                fprintf(out, "    ldr x0, [sp], #16\n"); // titulo
                fprintf(out, "    mov x8, #40\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "WaitEvent") == 0) {
                if (n->list_count >= 1) emit_expr(sc, n->list[0]);
                else fprintf(out, "    mov x0, #-1\n"); // -1 = sin limite de tiempo
                fprintf(out, "    bl rt_wait_event\n");
                return TY_INT;
            }
            if (strcmp(n->text, "PollEvent") == 0) {
                fprintf(out, "    bl rt_poll_event\n");
                return TY_INT;
            }
            if (strcmp(n->text, "PeekEvent") == 0) {
                fprintf(out, "    mov x8, #139\n    svc #0\n"); // SYS_PEEK_EVENT
                return TY_INT;
            }
            if (strcmp(n->text, "FlushEvents") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x8, #140\n    svc #0\n"); // SYS_FLUSH_EVENTS
                return TY_INT;
            }
            if (strcmp(n->text, "EventID") == 0) {
                fprintf(out, "    adrp x0, rt_last_event_id\n");
                fprintf(out, "    add x0, x0, :lo12:rt_last_event_id\n");
                fprintf(out, "    ldr x0, [x0]\n");
                return TY_INT;
            }
            if (strcmp(n->text, "EventSource") == 0) {
                fprintf(out, "    mov x8, #9\n    svc #0\n    lsr x0, x0, #32\n");
                return TY_INT;
            }
            if (strcmp(n->text, "EventData") == 0) {
                fprintf(out, "    mov x8, #9\n    svc #0\n    mov w0, w0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "EventX") == 0 || strcmp(n->text, "EventY") == 0) {
                // LIMITACION DOCUMENTADA: nuestro modelo de eventos no
                // guarda coordenadas x/y todavia -- ningun gadget
                // actual (Button, ListBox, Menu, TextField) las
                // produce. Se activara de verdad cuando implementemos
                // Slider (Fase 3 del roadmap), que si las necesita.
                // Por ahora, devolver 0 es honesto: no hay dato que
                // perder.
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "MenuChecked") == 0) {
                // Reutiliza el mismo campo 'checked' que ButtonState
                // -- vive en TODOS los gadgets, no solo en menus.
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #141\n    svc #0\n"); // SYS_BUTTON_STATE
                return TY_INT;
            }
            if (strcmp(n->text, "MenuEnabled") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #144\n    svc #0\n"); // SYS_GADGET_ENABLED
                return TY_INT;
            }
            if (strcmp(n->text, "MenuText$") == 0) {
                // Reutiliza la misma subrutina que GadgetText$ -- el
                // texto de una entrada de menu vive en el mismo campo
                // 'text' que cualquier otro gadget.
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_gadget_text\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "SetMenuText") == 0) {
                // Reutiliza la misma syscall que SetGadgetText.
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // texto -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #105\n    svc #0\n"); // SYS_GADGET_SET_TEXT
                return TY_INT;
            }
            if (strcmp(n->text, "Notify") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // mensaje
                if (n->list_count > 1) emit_expr(sc, n->list[1]); // 'serious' -- se evalua por si tiene efectos secundarios, no cambia el aspecto visual
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl rt_notify\n");
                return TY_INT;
            }
            if (strcmp(n->text, "Confirm") == 0) {
                // Confirm(mensaje$[,serious]) -- dialogo modal DE
                // VERDAD con botones Si/No (a diferencia de Notify,
                // que solo muestra un mensaje sin interaccion).
                emit_expr(sc, n->list[0]); push_x0(); // mensaje
                if (n->list_count > 1) emit_expr(sc, n->list[1]); // 'serious' -- se evalua, sin cambiar el aspecto
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl rt_confirm\n");
                return TY_INT;
            }
            if (strcmp(n->text, "Proceed") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // mensaje
                if (n->list_count > 1) emit_expr(sc, n->list[1]);
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl rt_proceed\n");
                return TY_INT;
            }
            if (strcmp(n->text, "RequestColor") == 0) {
                // RequestColor(r,g,b) -- dialogo con 3 Slider reales
                emit_expr(sc, n->list[0]); push_x0(); // r
                emit_expr(sc, n->list[1]); push_x0(); // g
                emit_expr(sc, n->list[2]);             // b -> x0
                fprintf(out, "    mov x2, x0\n");
                fprintf(out, "    ldr x1, [sp], #16\n"); // g
                fprintf(out, "    ldr x0, [sp], #16\n"); // r
                fprintf(out, "    bl rt_request_color\n");
                return TY_INT;
            }
            if (strcmp(n->text, "RequestedRed") == 0) {
                fprintf(out, "    adrp x0, rt_requested_r\n    add x0, x0, :lo12:rt_requested_r\n    ldr x0, [x0]\n");
                return TY_INT;
            }
            if (strcmp(n->text, "RequestedGreen") == 0) {
                fprintf(out, "    adrp x0, rt_requested_g\n    add x0, x0, :lo12:rt_requested_g\n    ldr x0, [x0]\n");
                return TY_INT;
            }
            if (strcmp(n->text, "RequestedBlue") == 0) {
                fprintf(out, "    adrp x0, rt_requested_b\n    add x0, x0, :lo12:rt_requested_b\n    ldr x0, [x0]\n");
                return TY_INT;
            }
            if (strcmp(n->text, "RequestDir$") == 0) {
                // RequestDir$(title$) -- LIMITACION DOCUMENTADA:
                // nuestro sistema de archivos se direcciona por
                // inodo, no por ruta de texto real, asi que
                // devolvemos el inodo elegido como cadena decimal --
                // NO es directamente compatible con ChangeDir
                // (que espera un NOMBRE). Sirve para pasarselo a
                // otras rutinas que ya trabajen con inodos.
                if (n->list_count > 0) emit_expr(sc, n->list[0]); // titulo -- se evalua, no se muestra (ver limitacion en rt_request_dir)
                fprintf(out, "    bl rt_request_dir\n");
                fprintf(out, "    cbnz x0, 430f\n");
                fprintf(out, "    adrp x0, rt_empty_str\n    add x0, x0, :lo12:rt_empty_str\n");
                fprintf(out, "    b 431f\n");
                fprintf(out, "430:\n");
                fprintf(out, "    adrp x0, rt_requested_dir_inode\n    add x0, x0, :lo12:rt_requested_dir_inode\n    ldr x0, [x0]\n");
                fprintf(out, "    bl rt_int_to_str\n");
                fprintf(out, "431:\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "RequestFile$") == 0) {
                // RequestFile$([title$[,exts$[,save[,defname$]]]]) --
                // LIMITACION DOCUMENTADA: solo elegir un archivo
                // EXISTENTE para abrir -- ni filtro de extensiones
                // (exts$), ni modo "guardar" (save/defname$, que
                // pediria un campo de texto para escribir un nombre
                // nuevo). Todos los parametros se evaluan (por si
                // tienen efectos secundarios) pero se descartan.
                // Devuelve "inodo:nombre" (formato ya usado por
                // editor.c/explorer.c), o cadena vacia si se cancelo.
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                fprintf(out, "    bl rt_request_file\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "ClientWidth") == 0 || strcmp(n->text, "ClientHeight") == 0) {
                if (n->list_count >= 1) emit_expr(sc, n->list[0]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    bl %s\n", strcmp(n->text, "ClientWidth") == 0 ? "rt_client_width" : "rt_client_height");
                return TY_INT;
            }
            if (strcmp(n->text, "Desktop") == 0) {
                fprintf(out, "    mov x0, #0x7FFFFFFF\n"); // "handle" centinela -- ver rt_client_width/height
                return TY_INT;
            }

            // -- Gadgets estilo BlitzPlus, ver gadgets.h del kernel --
            // La ventana se deduce sola en el kernel (la de la propia
            // tarea) -- aqui nunca hace falta pasarla.

            if (strcmp(n->text, "CreateButton") == 0) {
                // CreateButton(texto$, x, y, ancho, alto, [grupo], [style])
                // -- 'grupo' se registra (ver GadgetGroup) pero no
                // afecta al renderizado (v1: una sola ventana por
                // programa); 'style' SI se usa: 2=casilla,
                // 3=radio (el resto se trata como boton normal).
                emit_expr(sc, n->list[0]); push_x0(); // texto
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                emit_expr(sc, n->list[3]); push_x0(); // ancho
                emit_expr(sc, n->list[4]); push_x0();  // alto
                if (n->list_count > 6) emit_expr(sc, n->list[6]); else fprintf(out, "    mov x0, #1\n"); // style (por defecto: push normal)
                fprintf(out, "    mov x4, x0\n"); // x4 = style
                fprintf(out, "    ldr x1, [sp], #16\n");             // x1 = alto
                fprintf(out, "    ldr x0, [sp], #16\n");              // x0 = ancho
                fprintf(out, "    lsl x0, x0, #16\n");
                fprintf(out, "    orr x3, x0, x1\n");                   // x3 = (ancho<<16|alto)
                fprintf(out, "    ldr x2, [sp], #16\n");                 // x2 = y
                fprintf(out, "    ldr x1, [sp], #16\n");                  // x1 = x
                fprintf(out, "    ldr x0, [sp], #16\n");                   // x0 = texto
                fprintf(out, "    mov x8, #100\n    svc #0\n");
                if (n->list_count > 5) {
                    push_x0();
                    emit_expr(sc, n->list[5]); // grupo -> x0
                    fprintf(out, "    mov x1, x0\n");
                    fprintf(out, "    ldr x0, [sp], #16\n");
                    fprintf(out, "    mov x9, x0\n");
                    fprintf(out, "    mov x8, #223\n    svc #0\n"); // SYS_SET_GADGET_GROUP
                    fprintf(out, "    mov x0, x9\n");
                }
                return TY_INT;
            }
            if (strcmp(n->text, "CreateLabel") == 0) {
                // CreateLabel(texto$, x, y, ancho, alto, [grupo], [style])
                // -- misma disposicion que CreateButton (confirmado en
                // la documentacion oficial): 'grupo' es el argumento 6
                // (indice 5), 'style' el 7 (indice 6) -- 0=sin borde
                // (por defecto), 1=plano, 2=sin borde, 3=3D hundido.
                emit_expr(sc, n->list[0]); push_x0(); // texto
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                emit_expr(sc, n->list[3]); push_x0(); // ancho
                emit_expr(sc, n->list[4]); push_x0();  // alto
                if (n->list_count > 6) emit_expr(sc, n->list[6]); else fprintf(out, "    mov x0, #0\n"); // style (por defecto: sin borde)
                fprintf(out, "    mov x4, x0\n"); // x4 = style
                fprintf(out, "    ldr x1, [sp], #16\n");             // x1 = alto
                fprintf(out, "    ldr x0, [sp], #16\n");              // x0 = ancho
                fprintf(out, "    lsl x0, x0, #16\n");
                fprintf(out, "    orr x3, x0, x1\n");                   // x3 = (ancho<<16|alto)
                fprintf(out, "    ldr x2, [sp], #16\n");                 // x2 = y
                fprintf(out, "    ldr x1, [sp], #16\n");                  // x1 = x
                fprintf(out, "    ldr x0, [sp], #16\n");                   // x0 = texto
                fprintf(out, "    mov x8, #160\n    svc #0\n"); // SYS_CREATE_LABEL
                if (n->list_count > 5) {
                    push_x0();
                    emit_expr(sc, n->list[5]); // grupo -> x0
                    fprintf(out, "    mov x1, x0\n");
                    fprintf(out, "    ldr x0, [sp], #16\n");
                    fprintf(out, "    mov x9, x0\n");
                    fprintf(out, "    mov x8, #223\n    svc #0\n"); // SYS_SET_GADGET_GROUP
                    fprintf(out, "    mov x0, x9\n");
                }
                return TY_INT;
            }
            if (strcmp(n->text, "ButtonState") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #141\n    svc #0\n"); // SYS_BUTTON_STATE
                return TY_INT;
            }
            if (strcmp(n->text, "SetButtonState") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // estado -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #142\n    svc #0\n"); // SYS_SET_BUTTON_STATE
                return TY_INT;
            }
            if (strcmp(n->text, "HotKeyEvent") == 0) {
                // HotKeyEvent rawkey,modifier,event_id[,event_data,x,y,z,event_source]
                // -- x,y,z se evaluan (por si tienen efectos
                // secundarios) pero se descartan: no tenemos
                // EventX/EventY/EventZ implementados todavia.
                emit_expr(sc, n->list[0]); push_x0(); // rawkey
                emit_expr(sc, n->list[1]);             // modifier -> x0
                fprintf(out, "    mov x9, x0\n");
                fprintf(out, "    ldr x10, [sp], #16\n"); // rawkey
                fprintf(out, "    lsl x10, x10, #8\n");
                fprintf(out, "    orr x0, x10, x9\n"); // (rawkey<<8|modifier) -> x0
                push_x0();
                emit_expr(sc, n->list[2]); push_x0(); // event_id
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else fprintf(out, "    mov x0, #0\n");
                push_x0(); // event_data
                for (int i = 4; i < 7 && i < n->list_count; i++) emit_expr(sc, n->list[i]); // x,y,z -- se evaluan y se descartan
                if (n->list_count > 7) emit_expr(sc, n->list[7]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x3, x0\n"); // event_source
                fprintf(out, "    ldr x2, [sp], #16\n"); // event_data
                fprintf(out, "    ldr x1, [sp], #16\n"); // event_id
                fprintf(out, "    ldr x0, [sp], #16\n"); // (rawkey<<8|modifier)
                fprintf(out, "    mov x8, #143\n    svc #0\n"); // SYS_HOTKEY_EVENT
                return TY_INT;
            }
            if (strcmp(n->text, "CreatePanel") == 0 || strcmp(n->text, "CreateTextField") == 0 ||
                strcmp(n->text, "CreateListBox") == 0 || strcmp(n->text, "CreateTextArea") == 0 ||
                strcmp(n->text, "CreateProgBar") == 0 || strcmp(n->text, "CreateComboBox") == 0 ||
                strcmp(n->text, "CreateTabber") == 0 || strcmp(n->text, "CreateTreeView") == 0 ||
                strcmp(n->text, "CreateCanvas") == 0) {
                // Los nueve comparten firma: (x, y, ancho, alto[, ...])
                int sys_num = strcmp(n->text, "CreatePanel") == 0 ? 101
                            : strcmp(n->text, "CreateTextField") == 0 ? 102
                            : strcmp(n->text, "CreateListBox") == 0 ? 103
                            : strcmp(n->text, "CreateTextArea") == 0 ? 125
                            : strcmp(n->text, "CreateProgBar") == 0 ? 161
                            : strcmp(n->text, "CreateComboBox") == 0 ? 167
                            : strcmp(n->text, "CreateTabber") == 0 ? 168
                            : strcmp(n->text, "CreateTreeView") == 0 ? 175 : 188;
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                emit_expr(sc, n->list[2]); push_x0(); // ancho
                emit_expr(sc, n->list[3]);             // alto -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    lsl x0, x0, #16\n");
                fprintf(out, "    orr x2, x0, x1\n");    // x2 = (ancho<<16|alto)
                fprintf(out, "    ldr x1, [sp], #16\n");   // x1 = y
                fprintf(out, "    ldr x0, [sp], #16\n");    // x0 = x
                fprintf(out, "    mov x8, #%d\n    svc #0\n", sys_num);
                if (n->list_count > 4) {
                    // 'grupo' (5o argumento, opcional) -- se registra
                    // solo con fines informativos (ver GadgetGroup),
                    // sin afectar al renderizado ni anidado real.
                    push_x0(); // guardamos el id nuevo del gadget
                    emit_expr(sc, n->list[4]); // grupo -> x0
                    fprintf(out, "    mov x1, x0\n");
                    fprintf(out, "    ldr x0, [sp], #16\n");
                    fprintf(out, "    mov x9, x0\n"); // guardamos el id (el svc puede tocar x0)
                    fprintf(out, "    mov x8, #223\n    svc #0\n"); // SYS_SET_GADGET_GROUP
                    fprintf(out, "    mov x0, x9\n"); // restauramos el id como valor de retorno
                }
                return TY_INT;
            }
            if (strcmp(n->text, "UpdateProgBar") == 0) {
                // UpdateProgBar(progbar, valor#) -- el kernel no puede
                // usar coma flotante de verdad (ver la nota junto a
                // sound_volume_permil en syscall.c) -- convertimos
                // aqui mismo, con hardware real, a un entero "por mil"
                // (0-1000) antes de la syscall.
                emit_expr(sc, n->list[0]); push_x0(); // id
                ValType t = emit_expr(sc, n->list[1]); // valor -> x0
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n"); else fprintf(out, "    fmov d0, x0\n");
                fprintf(out, "    mov x9, #1000\n    scvtf d1, x9\n    fmul d0, d0, d1\n    fcvtzs x0, d0\n");
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #162\n    svc #0\n"); // SYS_UPDATE_PROGBAR
                return TY_INT;
            }
            if (strcmp(n->text, "CreateSlider") == 0) {
                // CreateSlider(x,y,ancho,alto,[grupo],[style]) -- style:
                // 1=horizontal (por defecto), 2=vertical
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                emit_expr(sc, n->list[2]); push_x0(); // ancho
                emit_expr(sc, n->list[3]); push_x0();  // alto
                if (n->list_count > 5) emit_expr(sc, n->list[5]); else fprintf(out, "    mov x0, #1\n"); // style
                fprintf(out, "    mov x3, x0\n"); // x3 = style
                fprintf(out, "    ldr x1, [sp], #16\n");            // x1 = alto
                fprintf(out, "    ldr x0, [sp], #16\n");             // x0 = ancho
                fprintf(out, "    lsl x0, x0, #16\n");
                fprintf(out, "    orr x2, x0, x1\n");                  // x2 = (ancho<<16|alto)
                fprintf(out, "    ldr x1, [sp], #16\n");                // x1 = y
                fprintf(out, "    ldr x0, [sp], #16\n");                 // x0 = x
                fprintf(out, "    mov x8, #163\n    svc #0\n"); // SYS_CREATE_SLIDER
                if (n->list_count > 4) {
                    push_x0();
                    emit_expr(sc, n->list[4]); // grupo -> x0
                    fprintf(out, "    mov x1, x0\n");
                    fprintf(out, "    ldr x0, [sp], #16\n");
                    fprintf(out, "    mov x9, x0\n");
                    fprintf(out, "    mov x8, #223\n    svc #0\n"); // SYS_SET_GADGET_GROUP
                    fprintf(out, "    mov x0, x9\n");
                }
                return TY_INT;
            }
            if (strcmp(n->text, "SetSliderRange") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]); push_x0(); // visible
                emit_expr(sc, n->list[2]);             // total -> x0
                fprintf(out, "    mov x2, x0\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #164\n    svc #0\n"); // SYS_SET_SLIDER_RANGE
                return TY_INT;
            }
            if (strcmp(n->text, "SetSliderValue") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // valor -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #165\n    svc #0\n"); // SYS_SET_SLIDER_VALUE
                return TY_INT;
            }
            if (strcmp(n->text, "SliderValue") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #166\n    svc #0\n"); // SYS_SLIDER_VALUE
                return TY_INT;
            }
            if (strcmp(n->text, "WindowMenu") == 0) {
                fprintf(out, "    mov x8, #120\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "CreateMenu") == 0) {
                // CreateMenu(texto$, tag, padre)
                emit_expr(sc, n->list[0]); push_x0(); // texto
                emit_expr(sc, n->list[1]); push_x0(); // tag
                emit_expr(sc, n->list[2]);             // padre -> x0
                fprintf(out, "    mov x2, x0\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #121\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "MenuTag") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #124\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "HideGadget") == 0 || strcmp(n->text, "ShowGadget") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x1, #%d\n", strcmp(n->text, "ShowGadget") == 0 ? 1 : 0);
                fprintf(out, "    mov x8, #110\n    svc #0\n"); // SYS_GADGET_SHOW
                return TY_INT;
            }
            if (strcmp(n->text, "DisableGadget") == 0 || strcmp(n->text, "EnableGadget") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x1, #%d\n", strcmp(n->text, "EnableGadget") == 0 ? 1 : 0);
                fprintf(out, "    mov x8, #111\n    svc #0\n"); // SYS_GADGET_ENABLE
                return TY_INT;
            }
            if (strcmp(n->text, "ActivateGadget") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #112\n    svc #0\n"); // SYS_GADGET_ACTIVATE
                return TY_INT;
            }
            if (strcmp(n->text, "FreeGadget") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #104\n"); // SYS_GADGET_FREE
                fprintf(out, "    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "ActivateWindow") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #150\n    svc #0\n"); // SYS_ACTIVATE_WINDOW
                return TY_INT;
            }
            if (strcmp(n->text, "ActiveWindow") == 0) {
                fprintf(out, "    mov x8, #151\n    svc #0\n"); // SYS_ACTIVE_WINDOW
                return TY_INT;
            }
            if (strcmp(n->text, "MaximizeWindow") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #152\n    svc #0\n"); // SYS_MAXIMIZE_WINDOW
                return TY_INT;
            }
            if (strcmp(n->text, "MinimizeWindow") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #153\n    svc #0\n"); // SYS_MINIMIZE_WINDOW
                return TY_INT;
            }
            if (strcmp(n->text, "WindowMaximized") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #154\n    svc #0\n"); // SYS_WINDOW_MAXIMIZED
                return TY_INT;
            }
            if (strcmp(n->text, "WindowMinimized") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #155\n    svc #0\n"); // SYS_WINDOW_MINIMIZED
                return TY_INT;
            }
            if (strcmp(n->text, "SetMinWindowSize") == 0) {
                // SetMinWindowSize(win[,ancho,alto]) -- omitidos = tamaño actual
                emit_expr(sc, n->list[0]); push_x0(); // ventana
                if (n->list_count > 1) emit_expr(sc, n->list[1]); else fprintf(out, "    mov x0, #0\n");
                push_x0(); // ancho
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x2, x0\n"); // alto
                fprintf(out, "    ldr x1, [sp], #16\n"); // ancho
                fprintf(out, "    ldr x0, [sp], #16\n"); // ventana
                fprintf(out, "    mov x8, #156\n    svc #0\n"); // SYS_SET_MIN_WINDOW_SIZE
                return TY_INT;
            }
            if (strcmp(n->text, "DisableMenu") == 0 || strcmp(n->text, "EnableMenu") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x1, #%d\n", strcmp(n->text, "EnableMenu") == 0 ? 1 : 0);
                fprintf(out, "    mov x8, #123\n    svc #0\n"); // SYS_MENU_ENABLE
                return TY_INT;
            }
            if (strcmp(n->text, "CheckMenu") == 0 || strcmp(n->text, "UncheckMenu") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x1, #%d\n", strcmp(n->text, "CheckMenu") == 0 ? 1 : 0);
                fprintf(out, "    mov x8, #122\n    svc #0\n"); // SYS_MENU_CHECK
                return TY_INT;
            }
            if (strcmp(n->text, "UpdateWindowMenu") == 0) {
                // No-op seguro: nuestro menu se redibuja leyendo
                // enabled/checked DIRECTAMENTE en cada vuelta, no hay
                // un menu nativo del SO que sincronizar aparte.
                emit_expr(sc, n->list[0]); // se evalua por si tiene efectos secundarios
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "GadgetEvent") == 0) {
                fprintf(out, "    mov x8, #113\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "Pump") == 0) {
                fprintf(out, "    mov x8, #14\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "SetGadgetText") == 0) {
                // SetGadgetText(id, texto$)
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // texto -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #105\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "AddGadgetItem") == 0) {
                // AddGadgetItem(id, texto$) -- añade un elemento a un ListBox
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // texto -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #114\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "ClearGadgetItems") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #115\n    svc #0\n"); // SYS_LISTBOX_CLEAR
                return TY_INT;
            }
            if (strcmp(n->text, "SelectedGadgetItem") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #116\n    svc #0\n"); // SYS_LISTBOX_SELECTED
                return TY_INT;
            }
            if (strcmp(n->text, "SelectGadgetItem") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #117\n    svc #0\n"); // SYS_LISTBOX_SELECT
                return TY_INT;
            }
            if (strcmp(n->text, "CountGadgetItems") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #118\n    svc #0\n"); // SYS_LISTBOX_ITEM_COUNT
                return TY_INT;
            }
            if (strcmp(n->text, "GadgetItemText$") == 0) {
                // GadgetItemText$(id, indice) -- reutiliza el mismo
                // patron de pool rotatorio que GadgetText$/rt_gadget_text,
                // pero via una subrutina propia porque necesita DOS
                // argumentos (id + indice), no uno.
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // indice -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl rt_gadget_item_text\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "InsertGadgetItem") == 0) {
                // InsertGadgetItem(gadget,index,item$[,icon]) -- 'icon'
                // se evalua (por si tiene efectos secundarios) pero se
                // descarta: no tenemos sistema de tiras de iconos.
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]); push_x0(); // indice
                emit_expr(sc, n->list[2]);             // texto -> x0
                if (n->list_count > 3) { push_x0(); emit_expr(sc, n->list[3]); fprintf(out, "    ldr x0, [sp], #16\n"); } // descartamos icon, recuperamos texto
                fprintf(out, "    mov x2, x0\n"); // texto
                fprintf(out, "    ldr x1, [sp], #16\n"); // indice
                fprintf(out, "    ldr x0, [sp], #16\n"); // id
                fprintf(out, "    mov x8, #157\n    svc #0\n"); // SYS_GADGET_INSERT_ITEM
                return TY_INT;
            }
            if (strcmp(n->text, "RemoveGadgetItem") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #158\n    svc #0\n"); // SYS_GADGET_REMOVE_ITEM
                return TY_INT;
            }
            if (strcmp(n->text, "ModifyGadgetItem") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]); push_x0(); // indice
                emit_expr(sc, n->list[2]);             // texto -> x0
                if (n->list_count > 3) { push_x0(); emit_expr(sc, n->list[3]); fprintf(out, "    ldr x0, [sp], #16\n"); }
                fprintf(out, "    mov x2, x0\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #159\n    svc #0\n"); // SYS_GADGET_MODIFY_ITEM
                return TY_INT;
            }
            if (strcmp(n->text, "SetTextAreaText") == 0) {
                // SetTextAreaText(id, texto$) -- reemplaza TODO el contenido
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // texto -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #126\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "AddTextAreaText") == 0) {
                // AddTextAreaText(id, texto$) -- añade al final
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // texto -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #145\n    svc #0\n"); // SYS_TEXTAREA_ADD_TEXT
                return TY_INT;
            }
            if (strcmp(n->text, "TextAreaLen") == 0) {
                // TextAreaLen(id[, units]) -- 1=caracteres (por defecto), 2=lineas
                emit_expr(sc, n->list[0]); push_x0();
                if (n->list_count > 1) emit_expr(sc, n->list[1]); else fprintf(out, "    mov x0, #1\n");
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #146\n    svc #0\n"); // SYS_TEXTAREA_LEN
                return TY_INT;
            }
            if (strcmp(n->text, "TextAreaLineLen") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #147\n    svc #0\n"); // SYS_TEXTAREA_LINE_LEN
                return TY_INT;
            }
            if (strcmp(n->text, "TextAreaLine") == 0) {
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #148\n    svc #0\n"); // SYS_TEXTAREA_LINE_OF_CHAR
                return TY_INT;
            }
            if (strcmp(n->text, "TextAreaText$") == 0) {
                // TextAreaText$(id[, start[, count]]) -- start/count
                // omitidos = todo el texto. count omitido = hasta el
                // final desde start. Usamos -1 como sentinela de
                // "hasta el final" (ver gadget_textarea_get_text).
                emit_expr(sc, n->list[0]); push_x0(); // id
                if (n->list_count > 1) emit_expr(sc, n->list[1]); else fprintf(out, "    mov x0, #0\n");
                push_x0(); // start
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else fprintf(out, "    mov x0, #-1\n");
                fprintf(out, "    mov x2, x0\n"); // count
                fprintf(out, "    ldr x1, [sp], #16\n"); // start
                fprintf(out, "    ldr x0, [sp], #16\n"); // id
                fprintf(out, "    bl rt_textarea_text\n");
                return TY_STRING;
            }

            // -- geometria de gadgets: reutiliza SYS_GADGET_RECT (107,
            // ya devuelve x/y/ancho/alto empaquetados) y SYS_GADGET_MOVE
            // (108) + SYS_GADGET_RESIZE (109), que ya existian --
            if (strcmp(n->text, "GadgetX") == 0 || strcmp(n->text, "GadgetY") == 0 ||
                strcmp(n->text, "GadgetWidth") == 0 || strcmp(n->text, "GadgetHeight") == 0) {
                emit_expr(sc, n->list[0]); // id -> x0
                fprintf(out, "    mov x8, #107\n    svc #0\n");
                if (strcmp(n->text, "GadgetX") == 0) {
                    fprintf(out, "    lsr x0, x0, #48\n");
                } else if (strcmp(n->text, "GadgetY") == 0) {
                    fprintf(out, "    lsr x0, x0, #32\n    and x0, x0, #0xFFFF\n");
                } else if (strcmp(n->text, "GadgetWidth") == 0) {
                    fprintf(out, "    lsr x0, x0, #16\n    and x0, x0, #0xFFFF\n");
                } else {
                    fprintf(out, "    and x0, x0, #0xFFFF\n");
                }
                return TY_INT;
            }
            if (strcmp(n->text, "GadgetGroup") == 0) {
                emit_expr(sc, n->list[0]); // id -> x0
                fprintf(out, "    mov x8, #224\n    svc #0\n"); // SYS_GADGET_GROUP
                return TY_INT;
            }
            if (strcmp(n->text, "SetGadgetShape") == 0) {
                // SetGadgetShape(id, x, y, ancho, alto) -- un MOVE y un
                // RESIZE seguidos, sin syscall nueva.
                emit_expr(sc, n->list[0]); push_x0(); // id      [sp+48]
                emit_expr(sc, n->list[1]); push_x0(); // x       [sp+32]
                emit_expr(sc, n->list[2]); push_x0(); // y       [sp+16]
                emit_expr(sc, n->list[3]); push_x0(); // ancho   [sp+0]
                emit_expr(sc, n->list[4]);             // alto -> x0
                fprintf(out, "    mov x3, x0\n");        // x3 = alto, aparte
                fprintf(out, "    ldr x0, [sp, #48]\n"); // id
                fprintf(out, "    ldr x1, [sp, #32]\n"); // x
                fprintf(out, "    ldr x2, [sp, #16]\n"); // y
                fprintf(out, "    mov x8, #108\n    svc #0\n"); // MOVE(id,x,y)
                fprintf(out, "    ldr x0, [sp, #48]\n"); // id
                fprintf(out, "    ldr x1, [sp]\n");       // ancho
                fprintf(out, "    mov x2, x3\n");          // alto
                fprintf(out, "    mov x8, #109\n    svc #0\n"); // RESIZE(id,ancho,alto)
                fprintf(out, "    add sp, sp, #64\n");
                return TY_INT;
            }
            if (strcmp(n->text, "CreateTimer") == 0) {
                emit_expr(sc, n->list[0]); // hertz -> x0
                fprintf(out, "    mov x8, #127\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "PauseTimer") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #216\n    svc #0\n"); // SYS_PAUSE_TIMER
                return TY_INT;
            }
            if (strcmp(n->text, "ResumeTimer") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #217\n    svc #0\n"); // SYS_RESUME_TIMER
                return TY_INT;
            }
            if (strcmp(n->text, "ResetTimer") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #218\n    svc #0\n"); // SYS_RESET_TIMER
                return TY_INT;
            }
            if (strcmp(n->text, "TimerTicks") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #219\n    svc #0\n"); // SYS_TIMER_TICKS
                return TY_INT;
            }

            // -- depuracion / archivos de texto --
            if (strcmp(n->text, "DebugLog") == 0) {
                emit_expr(sc, n->list[0]); // mensaje -> x0
                fprintf(out, "    mov x8, #28\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "RuntimeError") == 0) {
                // Mostramos el mensaje (reutilizando rt_notify) y
                // terminamos el programa exactamente igual que End --
                // no hace falta ninguna syscall nueva para esto.
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_notify\n");
                fprintf(out, "    b .Lprogram_end\n");
                return TY_INT;
            }
            if (strcmp(n->text, "ReadFile") == 0) {
                emit_expr(sc, n->list[0]); // nombre$ -> x0
                fprintf(out, "    mov x8, #41\n    svc #0\n");
                int l = new_label();
                // BlitzPlus real: "el handle seria igual a 0" si no
                // se pudo abrir -- la syscall devuelve -1 en ese caso.
                fprintf(out, "    cmp x0, #0\n    bge .Lreadf_done_%d\n    mov x0, #0\n", l);
                fprintf(out, ".Lreadf_done_%d:\n", l);
                return TY_INT;
            }
            if (strcmp(n->text, "ReadLine$") == 0 || strcmp(n->text, "ReadLine") == 0) {
                emit_expr(sc, n->list[0]); // handle -> x0
                fprintf(out, "    bl rt_readline\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "GadgetText$") == 0 || strcmp(n->text, "TextFieldText$") == 0) {
                // TextFieldText$ es un sinonimo real de GadgetText$
                // (documentacion oficial: "returns the text entered by
                // the user into a textfield gadget") -- misma rutina.
                emit_expr(sc, n->list[0]); // id de gadget -> x0
                fprintf(out, "    bl rt_gadget_text\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "QueryObject") == 0) {
                // LIMITACION DOCUMENTADA: pide handles especificos de
                // Win32 (HWND, HFONT) -- no tenemos ese concepto en un
                // SO ARM64 sin ventanas nativas del sistema operativo
                // real. Se evaluan los argumentos (por si tuvieran
                // efectos secundarios) y se devuelve 0.
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]);
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "Eof") == 0) {
                // Los handles de OpenFile/WriteFile llevan un offset
                // de +100 respecto a los de ReadFile, para poder
                // distinguirlos aqui y despachar a la syscall correcta
                // de cada sistema (son dos pools de handles distintos).
                // Este comando se expande EN LINEA (no es una
                // subrutina con 'bl'), asi que necesita una etiqueta
                // NUEVA cada vez -- si se llama mas de una vez en el
                // mismo programa y usaramos una fija, colisionarian.
                emit_expr(sc, n->list[0]);
                int l = new_label();
                fprintf(out, "    cmp x0, #100\n");
                fprintf(out, "    blt .Leof_old_%d\n", l);
                fprintf(out, "    sub x0, x0, #100\n");
                fprintf(out, "    mov x8, #76\n    svc #0\n"); // SYS_GENFILE_EOF
                fprintf(out, "    b .Leof_done_%d\n", l);
                fprintf(out, ".Leof_old_%d:\n", l);
                fprintf(out, "    mov x8, #43\n    svc #0\n"); // SYS_READ_FILE_EOF (el de siempre)
                fprintf(out, ".Leof_done_%d:\n", l);
                return TY_INT;
            }
            if (strcmp(n->text, "CloseFile") == 0) {
                emit_expr(sc, n->list[0]);
                int l = new_label();
                fprintf(out, "    cmp x0, #100\n");
                fprintf(out, "    blt .Lclose_old_%d\n", l);
                fprintf(out, "    sub x0, x0, #100\n");
                fprintf(out, "    mov x8, #77\n    svc #0\n"); // SYS_GENFILE_CLOSE
                fprintf(out, "    b .Lclose_done_%d\n", l);
                fprintf(out, ".Lclose_old_%d:\n", l);
                fprintf(out, "    mov x8, #44\n    svc #0\n"); // SYS_READ_FILE_CLOSE (el de siempre)
                fprintf(out, ".Lclose_done_%d:\n", l);
                return TY_INT;
            }
            if (strcmp(n->text, "OpenFile") == 0) {
                emit_expr(sc, n->list[0]);
                int l = new_label();
                fprintf(out, "    mov x1, #0\n"); // modo 0 = lee+escribe -- FALLA si no existe (no crea)
                fprintf(out, "    mov x8, #70\n    svc #0\n"); // SYS_GENFILE_OPEN
                fprintf(out, "    cmp x0, #0\n    blt .Lopenf_fail_%d\n    add x0, x0, #100\n    b .Lopenf_done_%d\n", l, l);
                fprintf(out, ".Lopenf_fail_%d:\n    mov x0, #0\n", l); // BlitzPlus real: handle=0 si no se pudo abrir
                fprintf(out, ".Lopenf_done_%d:\n", l);
                return TY_INT;
            }
            if (strcmp(n->text, "ExecFile") == 0) {
                // LIMITACION DOCUMENTADA: BlitzPlus real usa ShellExecute
                // de Windows para abrir CUALQUIER archivo con su
                // programa asociado -- nosotros no tenemos ese
                // concepto. Reinterpretado: lanza un programa .pro real
                // de Nemo OS como una tarea nueva (ver exec_program en
                // syscall.c), con su salida redirigida a la ventana de
                // quien lo lanza.
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #232\n    svc #0\n"); // SYS_EXEC_FILE
                return TY_INT;
            }
            if (strcmp(n->text, "CreateProcess") == 0) {
                // LIMITACION DOCUMENTADA: al igual que ExecFile, lanza
                // un programa .pro real de Nemo OS (partiendo command$
                // en "programa argumentos" por el primer espacio), pero
                // SIN una tuberia de verdad detras -- ReadLine/
                // WriteLine/Eof sobre el "stream" devuelto se
                // comportan como si estuviera vacio y cerrado (ver
                // Eof, mas abajo), en vez de fallar de forma confusa.
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #233\n    svc #0\n"); // SYS_CREATE_PROCESS
                return TY_INT;
            }
            if (strcmp(n->text, "CallDLL") == 0) {
                // LIMITACION DOCUMENTADA: pide cargar una .dll de
                // Windows (formato PE) y llamar a una funcion nativa
                // dentro de ella -- no tenemos ningun cargador de
                // codigo nativo dinamico en Nemo OS (solo sabemos
                // cargar nuestro propio formato .pro). Se evaluan los
                // argumentos (por si tuvieran efectos secundarios,
                // como CreateBank) y se devuelve 0.
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "WriteFile") == 0) {
                emit_expr(sc, n->list[0]);
                int l = new_label();
                fprintf(out, "    mov x1, #1\n"); // modo 1 = siempre vacio
                fprintf(out, "    mov x8, #70\n    svc #0\n");
                fprintf(out, "    cmp x0, #0\n    blt .Lwritef_fail_%d\n    add x0, x0, #100\n    b .Lwritef_done_%d\n", l, l);
                fprintf(out, ".Lwritef_fail_%d:\n    mov x0, #0\n", l); // BlitzPlus real: handle=0 si no se pudo abrir
                fprintf(out, ".Lwritef_done_%d:\n", l);
                return TY_INT;
            }
            if (strcmp(n->text, "FilePos") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    sub x0, x0, #100\n");
                fprintf(out, "    mov x8, #73\n    svc #0\n"); // SYS_GENFILE_POS
                return TY_INT;
            }
            if (strcmp(n->text, "SeekFile") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]);             // posicion -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    sub x0, x0, #100\n");
                fprintf(out, "    mov x8, #74\n    svc #0\n"); // SYS_GENFILE_SEEK
                return TY_INT;
            }
            if (strcmp(n->text, "ReadAvail") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // guardamos el handle, lo necesitamos dos veces
                fprintf(out, "    ldr x0, [sp]\n");     // consultamos sin sacarlo de la pila todavia
                fprintf(out, "    sub x0, x0, #100\n");
                fprintf(out, "    mov x8, #75\n    svc #0\n"); // SYS_GENFILE_SIZE
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    sub x0, x0, #100\n");
                fprintf(out, "    mov x8, #73\n    svc #0\n"); // SYS_GENFILE_POS
                fprintf(out, "    sub x0, x1, x0\n"); // disponible = tamaño - posicion
                return TY_INT;
            }
            if (strcmp(n->text, "FileSize") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #81\n    svc #0\n"); // SYS_FILE_SIZE_BY_NAME
                return TY_INT;
            }
            if (strcmp(n->text, "FileType") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #82\n    svc #0\n"); // SYS_FILE_TYPE_BY_NAME
                return TY_INT;
            }
            if (strcmp(n->text, "DeleteFile") == 0 || strcmp(n->text, "DeleteDir") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #84\n    svc #0\n"); // SYS_DELETE_ANYWHERE -- vale para archivo o carpeta
                return TY_INT;
            }
            if (strcmp(n->text, "CreateDir") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // nombre
                fprintf(out, "    adrp x9, rt_current_dir_inode\n    add x9, x9, :lo12:rt_current_dir_inode\n    ldr x1, [x9]\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x2, #0\n"); // VOLUME_NEMOFS
                fprintf(out, "    mov x8, #24\n    svc #0\n"); // SYS_DIR_CREATE
                return TY_INT;
            }
            if (strcmp(n->text, "ChangeDir") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_changedir\n");
                return TY_INT;
            }
            if (strcmp(n->text, "CurrentDir$") == 0) {
                fprintf(out, "    bl rt_currentdir\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "ReadDir") == 0) {
                emit_expr(sc, n->list[0]); // nombre$ -- vacio = carpeta actual
                fprintf(out, "    bl rt_readdir\n");
                return TY_INT;
            }
            if (strcmp(n->text, "NextFile$") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_nextfile\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "CloseDir") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #80\n    svc #0\n"); // SYS_DIR_CLOSE
                return TY_INT;
            }
            if (strcmp(n->text, "ReadByte") == 0 || strcmp(n->text, "ReadShort") == 0 || strcmp(n->text, "ReadInt") == 0) {
                // Funciona tanto con handles de OpenFile/WriteFile
                // (>=100) como de ReadFile (<100) -- confirmado en el
                // manual: "una variable valida establecida con
                // OpenFile, ReadFile o OpenTCPStream".
                emit_expr(sc, n->list[0]);
                int nbytes = strcmp(n->text, "ReadByte") == 0 ? 1 : (strcmp(n->text, "ReadShort") == 0 ? 2 : 4);
                int l = new_label();
                fprintf(out, "    cmp x0, #100\n");
                fprintf(out, "    blt .Lrdn_old_%d\n", l);
                fprintf(out, "    sub x0, x0, #100\n");
                fprintf(out, "    adrp x1, rt_file_io_buf\n    add x1, x1, :lo12:rt_file_io_buf\n");
                fprintf(out, "    mov x2, #%d\n", nbytes);
                fprintf(out, "    mov x8, #71\n    svc #0\n"); // SYS_GENFILE_READ_BYTES
                fprintf(out, "    b .Lrdn_done_%d\n", l);
                fprintf(out, ".Lrdn_old_%d:\n", l);
                fprintf(out, "    adrp x1, rt_file_io_buf\n    add x1, x1, :lo12:rt_file_io_buf\n");
                fprintf(out, "    mov x2, #%d\n", nbytes);
                fprintf(out, "    mov x8, #136\n    svc #0\n"); // SYS_READ_FILE_READ_BYTES
                fprintf(out, ".Lrdn_done_%d:\n", l);
                fprintf(out, "    adrp x9, rt_file_io_buf\n    add x9, x9, :lo12:rt_file_io_buf\n");
                fprintf(out, "    ldrb w0, [x9, #%d]\n", nbytes - 1);
                for (int i = nbytes - 2; i >= 0; i--) {
                    fprintf(out, "    lsl x0, x0, #8\n");
                    fprintf(out, "    ldrb w10, [x9, #%d]\n", i);
                    fprintf(out, "    orr x0, x0, x10\n");
                }
                return TY_INT;
            }
            if (strcmp(n->text, "ReadFloat") == 0) {
                emit_expr(sc, n->list[0]);
                int l = new_label();
                fprintf(out, "    cmp x0, #100\n");
                fprintf(out, "    blt .Lrdf_old_%d\n", l);
                fprintf(out, "    sub x0, x0, #100\n");
                fprintf(out, "    adrp x1, rt_file_io_buf\n    add x1, x1, :lo12:rt_file_io_buf\n");
                fprintf(out, "    mov x2, #4\n");
                fprintf(out, "    mov x8, #71\n    svc #0\n");
                fprintf(out, "    b .Lrdf_done_%d\n", l);
                fprintf(out, ".Lrdf_old_%d:\n", l);
                fprintf(out, "    adrp x1, rt_file_io_buf\n    add x1, x1, :lo12:rt_file_io_buf\n");
                fprintf(out, "    mov x2, #4\n");
                fprintf(out, "    mov x8, #136\n    svc #0\n"); // SYS_READ_FILE_READ_BYTES
                fprintf(out, ".Lrdf_done_%d:\n", l);
                fprintf(out, "    adrp x9, rt_file_io_buf\n    add x9, x9, :lo12:rt_file_io_buf\n");
                fprintf(out, "    ldrb w0, [x9, #3]\n");
                fprintf(out, "    lsl x0, x0, #8\n    ldrb w10, [x9, #2]\n    orr x0, x0, x10\n");
                fprintf(out, "    lsl x0, x0, #8\n    ldrb w10, [x9, #1]\n    orr x0, x0, x10\n");
                fprintf(out, "    lsl x0, x0, #8\n    ldrb w10, [x9, #0]\n    orr x0, x0, x10\n");
                fprintf(out, "    fmov s0, w0\n    fcvt d0, s0\n    fmov x0, d0\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "WriteByte") == 0 || strcmp(n->text, "WriteShort") == 0 || strcmp(n->text, "WriteInt") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]);             // valor -> x0
                int nbytes = strcmp(n->text, "WriteByte") == 0 ? 1 : (strcmp(n->text, "WriteShort") == 0 ? 2 : 4);
                fprintf(out, "    adrp x9, rt_file_io_buf\n    add x9, x9, :lo12:rt_file_io_buf\n");
                for (int i = 0; i < nbytes; i++) {
                    fprintf(out, "    strb w0, [x9, #%d]\n", i);
                    if (i < nbytes - 1) fprintf(out, "    lsr x0, x0, #8\n");
                }
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    sub x0, x0, #100\n");
                fprintf(out, "    mov x1, x9\n");
                fprintf(out, "    mov x2, #%d\n", nbytes);
                fprintf(out, "    mov x8, #72\n    svc #0\n"); // SYS_GENFILE_WRITE_BYTES
                return TY_INT;
            }
            if (strcmp(n->text, "WriteFloat") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                ValType t = emit_expr(sc, n->list[1]); // valor -> x0
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    fmov d0, x0\n    fcvt s0, d0\n    fmov w0, s0\n");
                fprintf(out, "    adrp x9, rt_file_io_buf\n    add x9, x9, :lo12:rt_file_io_buf\n");
                fprintf(out, "    strb w0, [x9]\n");
                fprintf(out, "    lsr x0, x0, #8\n    strb w0, [x9, #1]\n");
                fprintf(out, "    lsr x0, x0, #8\n    strb w0, [x9, #2]\n");
                fprintf(out, "    lsr x0, x0, #8\n    strb w0, [x9, #3]\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    sub x0, x0, #100\n");
                fprintf(out, "    mov x1, x9\n    mov x2, #4\n");
                fprintf(out, "    mov x8, #72\n    svc #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "WriteLine$") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]);             // cadena -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl rt_writeline\n");
                return TY_INT;
            }
            if (strcmp(n->text, "WriteString$") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]);             // cadena -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl rt_writestring\n");
                return TY_INT;
            }
            if (strcmp(n->text, "ReadString$") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_readstring\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "CopyFile") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // origen
                emit_expr(sc, n->list[1]);             // destino -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    bl rt_copyfile\n");
                return TY_INT;
            }
            if (strcmp(n->text, "Exp") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    bl rt_exp\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "Log") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    bl rt_log\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "Log10") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    bl rt_log10\n");
                return TY_FLOAT;
            }

            // -- Graphics 2D: modo clasico, sin depender del sistema
            // de ventanas con eventos. Graphics() crea/redimensiona la
            // ventana; SetBuffer/BackBuffer/FrontBuffer son casi
            // decorativos porque en Nemo OS TODO dibujo ya va a un
            // buffer que se vuelca solo (no distinguimos buffers de
            // verdad); Flip cede el control (SYS_PUMP) para que el
            // resto del sistema (raton, teclado, redibujado) siga vivo
            // -- imprescindible, porque somos cooperativos, no
            // expulsivos: sin esto, un bucle de juego que nunca cede
            // dejaria el sistema congelado.
            if (strcmp(n->text, "Graphics") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // ancho
                emit_expr(sc, n->list[1]);             // alto -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #46\n    svc #0\n"); // SYS_GRAPHICS_MODE
                for (int i = 2; i < n->list_count; i++) emit_expr(sc, n->list[i]); // profundidad/modo -- se evaluan y se ignoran
                return TY_INT;
            }
            if (strcmp(n->text, "SetBuffer") == 0) {
                // SetBuffer(id) es quien de verdad redirige el dibujo
                // ahora -- id=0 (BackBuffer/FrontBuffer/GraphicsBuffer)
                // significa "la ventana", id=handle+1 (de ImageBuffer)
                // significa esa imagen. Asi "SetBuffer ImageBuffer(img)"
                // funciona igual que en BlitzPlus real.
                if (n->list_count > 0) emit_expr(sc, n->list[0]);
                else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    sub x0, x0, #1\n"); // id-1 (0 -> -1 = "vuelve a la ventana" para el kernel)
                fprintf(out, "    mov x8, #128\n    svc #0\n"); // SYS_SET_IMAGE_BUFFER
                return TY_INT;
            }
            if (strcmp(n->text, "BackBuffer") == 0 || strcmp(n->text, "FrontBuffer") == 0 || strcmp(n->text, "GraphicsBuffer") == 0) {
                fprintf(out, "    mov x0, #0\n"); // 0 = "la ventana" -- no distinguimos buffers de verdad
                return TY_INT;
            }
            if (strcmp(n->text, "ImageBuffer") == 0) {
                // Devuelve un identificador de buffer (handle+1, para
                // distinguirlo del 0 de "ventana") -- NO redirige por
                // si solo; hace falta pasarlo a SetBuffer, igual que en
                // BlitzPlus real ("SetBuffer ImageBuffer(img)").
                if (n->list_count > 1) emit_expr(sc, n->list[1]); // fotograma -- aceptado pero ignorado, no rastreamos buffers por fotograma
                emit_expr(sc, n->list[0]); // handle -> x0 (el ultimo evaluado, para que su resultado sea el que quede)
                fprintf(out, "    add x0, x0, #1\n");
                return TY_INT;
            }
            if (strcmp(n->text, "CanvasBuffer") == 0) {
                // Igual que ImageBuffer, pero en el rango numerico
                // reservado para Canvas (CANVAS_BUFFER_OFFSET=100000
                // en el kernel) -- se distinguen sin ambiguedad.
                emit_expr(sc, n->list[0]); // id del canvas -> x0
                fprintf(out, "    add x0, x0, #100000\n");
                fprintf(out, "    add x0, x0, #1\n");
                return TY_INT;
            }
            if (strcmp(n->text, "FlipCanvas") == 0) {
                // FlipCanvas canvas[,vwait] -- ambos se evaluan (por si
                // tienen efectos secundarios) pero se ignoran: nuestro
                // dibujo ya es directo a la ventana, sin doble buffer
                // por canvas que "voltear" de verdad -- se trata igual
                // que Flip (sincroniza el redibujado).
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]);
                fprintf(out, "    mov x8, #14\n    svc #0\n"); // SYS_PUMP
                return TY_INT;
            }
            if (strcmp(n->text, "DesktopBuffer") == 0) {
                // LIMITACION DOCUMENTADA: no tenemos acceso de dibujo
                // directo al escritorio (fuera de las ventanas) --
                // devolvemos 0 ("la ventana actual") como alternativa segura.
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "EndGraphics") == 0) {
                // No-op seguro: nuestro modo grafico ya esta siempre
                // activo (una sola ventana por programa), no hay un
                // "modo sin graficos" real al que volver.
                fprintf(out, "    mov x0, #0\n");
                return TY_INT;
            }
            if (strcmp(n->text, "LockBuffer") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x8, #208\n    svc #0\n"); // SYS_LOCK_BUFFER
                return TY_INT;
            }
            if (strcmp(n->text, "UnlockBuffer") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x8, #209\n    svc #0\n"); // SYS_UNLOCK_BUFFER
                return TY_INT;
            }
            if (strcmp(n->text, "LockedPixels") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x8, #210\n    svc #0\n"); // SYS_LOCKED_PIXELS
                return TY_INT;
            }
            if (strcmp(n->text, "LockedPitch") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x8, #211\n    svc #0\n"); // SYS_LOCKED_PITCH
                return TY_INT;
            }
            if (strcmp(n->text, "LockedFormat") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x8, #212\n    svc #0\n"); // SYS_LOCKED_FORMAT
                return TY_INT;
            }
            if (strcmp(n->text, "ReadPixel") == 0) {
                // ReadPixel(x,y,[buffer]) -- buffer omitido = 0 = ventana actual
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x2, x0\n"); // buffer
                fprintf(out, "    ldr x1, [sp], #16\n"); // y
                fprintf(out, "    ldr x0, [sp], #16\n"); // x
                fprintf(out, "    mov x8, #185\n    svc #0\n"); // SYS_READ_PIXEL
                return TY_INT;
            }
            if (strcmp(n->text, "WritePixel") == 0) {
                // WritePixel x,y,argb,[buffer]
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                emit_expr(sc, n->list[2]); push_x0(); // argb
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x3, x0\n"); // buffer
                fprintf(out, "    ldr x2, [sp], #16\n"); // argb
                fprintf(out, "    ldr x1, [sp], #16\n"); // y
                fprintf(out, "    ldr x0, [sp], #16\n"); // x
                fprintf(out, "    mov x8, #186\n    svc #0\n"); // SYS_WRITE_PIXEL
                return TY_INT;
            }
            if (strcmp(n->text, "CopyPixel") == 0) {
                // CopyPixel src_x,src_y,src_buffer,dest_x,dest_y,[dest_buffer]
                emit_expr(sc, n->list[0]); push_x0(); // src_x
                emit_expr(sc, n->list[1]); push_x0(); // src_y
                emit_expr(sc, n->list[2]); push_x0(); // src_buffer
                emit_expr(sc, n->list[3]); push_x0(); // dest_x
                emit_expr(sc, n->list[4]); push_x0(); // dest_y
                if (n->list_count > 5) emit_expr(sc, n->list[5]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x3, x0\n"); // dest_buffer
                fprintf(out, "    ldr x0, [sp], #16\n"); // dest_y
                fprintf(out, "    mov x9, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n"); // dest_x
                fprintf(out, "    lsl x0, x0, #16\n");
                fprintf(out, "    orr x2, x0, x9\n"); // x2 = (dest_x<<16|dest_y)
                fprintf(out, "    ldr x1, [sp], #16\n"); // src_buffer
                fprintf(out, "    ldr x0, [sp], #16\n"); // src_y
                fprintf(out, "    mov x9, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n"); // src_x
                fprintf(out, "    lsl x0, x0, #16\n");
                fprintf(out, "    orr x0, x0, x9\n"); // x0 = (src_x<<16|src_y)
                fprintf(out, "    mov x8, #187\n    svc #0\n"); // SYS_COPY_PIXEL
                return TY_INT;
            }
            if (strcmp(n->text, "ReadPixelFast") == 0) {
                // Igual que ReadPixel, pero EXIGE un buffer bloqueado
                // (LockBuffer) -- lo comprueba el propio kernel.
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x2, x0\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #213\n    svc #0\n"); // SYS_READ_PIXEL_FAST
                return TY_INT;
            }
            if (strcmp(n->text, "WritePixelFast") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // x
                emit_expr(sc, n->list[1]); push_x0(); // y
                emit_expr(sc, n->list[2]); push_x0(); // argb
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x3, x0\n");
                fprintf(out, "    ldr x2, [sp], #16\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #214\n    svc #0\n"); // SYS_WRITE_PIXEL_FAST
                return TY_INT;
            }
            if (strcmp(n->text, "CopyPixelFast") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // src_x
                emit_expr(sc, n->list[1]); push_x0(); // src_y
                emit_expr(sc, n->list[2]); push_x0(); // src_buffer
                emit_expr(sc, n->list[3]); push_x0(); // dest_x
                emit_expr(sc, n->list[4]); push_x0(); // dest_y
                if (n->list_count > 5) emit_expr(sc, n->list[5]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x3, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x9, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    lsl x0, x0, #16\n");
                fprintf(out, "    orr x2, x0, x9\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x9, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    lsl x0, x0, #16\n");
                fprintf(out, "    orr x0, x0, x9\n");
                fprintf(out, "    mov x8, #215\n    svc #0\n"); // SYS_COPY_PIXEL_FAST
                return TY_INT;
            }
            if (strcmp(n->text, "Flip") == 0) {
                for (int i = 0; i < n->list_count; i++) emit_expr(sc, n->list[i]); // "sync" opcional, se ignora
                fprintf(out, "    mov x8, #14\n    svc #0\n"); // SYS_PUMP
                return TY_INT;
            }
            if (strcmp(n->text, "Color") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // r
                emit_expr(sc, n->list[1]); push_x0(); // g
                emit_expr(sc, n->list[2]);             // b -> x0
                fprintf(out, "    and x3, x0, #0xFF\n");
                fprintf(out, "    ldr x2, [sp], #16\n");
                fprintf(out, "    and x2, x2, #0xFF\n");
                fprintf(out, "    lsl x2, x2, #8\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    and x1, x1, #0xFF\n");
                fprintf(out, "    lsl x1, x1, #16\n");
                fprintf(out, "    orr x0, x1, x2\n");
                fprintf(out, "    orr x0, x0, x3\n");
                fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n");
                fprintf(out, "    str x0, [x9]\n");
                return TY_INT;
            }
            if (strcmp(n->text, "Oval") == 0) {
                // De momento siempre relleno -- el argumento 'solido'
                // se acepta por compatibilidad de sintaxis pero se
                // ignora (ver nota en SYS_DRAW_OVAL: solo 5 argumentos
                // disponibles por syscall, ya usados por x/y/w/h/color).
                emit_expr(sc, n->list[0]); push_x0();
                emit_expr(sc, n->list[1]); push_x0();
                emit_expr(sc, n->list[2]); push_x0();
                emit_expr(sc, n->list[3]);
                fprintf(out, "    mov x3, x0\n");
                fprintf(out, "    ldr x2, [sp], #16\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n    ldr x4, [x9]\n");
                fprintf(out, "    mov x8, #47\n    svc #0\n"); // SYS_DRAW_OVAL
                if (n->list_count > 4) emit_expr(sc, n->list[4]);
                return TY_INT;
            }
            if (strcmp(n->text, "Text") == 0) {
                // Text x,y,texto$,[center_x],[center_y] -- si
                // center_x/center_y son TRUE, se resta la mitad del
                // ancho/alto del texto antes de dibujar (confirmado en
                // el manual). StringWidth = strlen*6, FontHeight = 7
                // (entero, /2 = 3).
                emit_expr(sc, n->list[0]); push_x0(); // x -> [sp,#32]
                emit_expr(sc, n->list[1]); push_x0(); // y -> [sp,#16]
                emit_expr(sc, n->list[2]); push_x0(); // texto$ -> [sp,#0]

                if (n->list_count > 3) {
                    emit_expr(sc, n->list[3]); // center_x -> x0
                    int l = new_label();
                    fprintf(out, "    cbz x0, .Ltext_nocx_%d\n", l);
                    fprintf(out, "    ldr x0, [sp]\n"); // texto$ (no se desapila, aun hace falta)
                    fprintf(out, "    bl rt_strlen\n");
                    fprintf(out, "    mov x1, #3\n    mul x0, x0, x1\n"); // ancho/2 = strlen*6/2
                    fprintf(out, "    ldr x1, [sp, #32]\n"); // x
                    fprintf(out, "    sub x1, x1, x0\n");
                    fprintf(out, "    str x1, [sp, #32]\n"); // x centrada, guardada de vuelta
                    fprintf(out, ".Ltext_nocx_%d:\n", l);
                }
                if (n->list_count > 4) {
                    emit_expr(sc, n->list[4]); // center_y -> x0
                    int l = new_label();
                    fprintf(out, "    cbz x0, .Ltext_nocy_%d\n", l);
                    fprintf(out, "    ldr x1, [sp, #16]\n"); // y
                    fprintf(out, "    sub x1, x1, #3\n"); // alto/2 = 7/2 = 3 (entero)
                    fprintf(out, "    str x1, [sp, #16]\n");
                    fprintf(out, ".Ltext_nocy_%d:\n", l);
                }

                fprintf(out, "    ldr x2, [sp], #16\n"); // texto$
                fprintf(out, "    ldr x1, [sp], #16\n"); // y
                fprintf(out, "    ldr x0, [sp], #16\n"); // x
                fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n    ldr x3, [x9]\n");
                fprintf(out, "    mov x8, #%d\n    svc #0\n", SYS_DRAW_TEXT);
                return TY_INT;
            }
            if (strcmp(n->text, "KeyDown") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #48\n    svc #0\n"); // SYS_KEY_DOWN
                return TY_INT;
            }
            if (strcmp(n->text, "MouseX") == 0 || strcmp(n->text, "MouseY") == 0 || strcmp(n->text, "MouseDown") == 0) {
                // Las tres leen SYS_GET_MOUSE (34) y se quedan con su
                // parte -- si el raton no esta sobre la ventana (o no
                // tiene el foco), la syscall devuelve -1; en ese caso
                // damos 0 en vez de un numero sin sentido.
                bool is_down = strcmp(n->text, "MouseDown") == 0;
                if (is_down) {
                    if (n->list_count > 0) emit_expr(sc, n->list[0]); else fprintf(out, "    mov x0, #1\n");
                    push_x0(); // boton pedido -- lo necesitamos DESPUES de la syscall
                }
                fprintf(out, "    mov x8, #%d\n    svc #0\n", SYS_GET_MOUSE);
                int l = new_label();
                fprintf(out, "    add x9, x0, #1\n"); // x0 == -1  <=>  x0+1 == 0
                fprintf(out, "    cbz x9, .Lmouse_unfocused_%d\n", l);
                if (strcmp(n->text, "MouseX") == 0) {
                    fprintf(out, "    lsr x0, x0, #32\n    and x0, x0, #0xFFFF\n");
                } else if (strcmp(n->text, "MouseY") == 0) {
                    fprintf(out, "    lsr x0, x0, #16\n    and x0, x0, #0xFFFF\n");
                } else {
                    // MouseDown(boton): distinguimos DE VERDAD el
                    // boton pedido (1=izq bit0, 2=der bit1, 3=centro
                    // bit2) -- antes se devolvia la mascara combinada
                    // de los tres, sin importar cual se pedia.
                    fprintf(out, "    and x9, x0, #7\n"); // bits de los tres botones
                    fprintf(out, "    ldr x10, [sp], #16\n"); // boton pedido
                    fprintf(out, "    cmp x10, #1\n");
                    fprintf(out, "    beq .Lmd_b1_%d\n", l);
                    fprintf(out, "    cmp x10, #2\n");
                    fprintf(out, "    beq .Lmd_b2_%d\n", l);
                    fprintf(out, "    lsr x9, x9, #2\n    and x0, x9, #1\n"); // boton 3 (central)
                    fprintf(out, "    b .Lmouse_done_%d\n", l);
                    fprintf(out, ".Lmd_b1_%d:\n", l);
                    fprintf(out, "    and x0, x9, #1\n");
                    fprintf(out, "    b .Lmouse_done_%d\n", l);
                    fprintf(out, ".Lmd_b2_%d:\n", l);
                    fprintf(out, "    lsr x9, x9, #1\n    and x0, x9, #1\n");
                }
                fprintf(out, "    b .Lmouse_done_%d\n", l);
                fprintf(out, ".Lmouse_unfocused_%d:\n", l);
                if (is_down) fprintf(out, "    ldr x11, [sp], #16\n"); // deshacemos el push del boton, no hizo falta
                fprintf(out, "    mov x0, #0\n");
                fprintf(out, ".Lmouse_done_%d:\n", l);
                return TY_INT;
            }
            if (strcmp(n->text, "MilliSecs") == 0) {
                // SYS_GET_TICKS: cada tick = 10ms (100 ticks/segundo)
                fprintf(out, "    mov x8, #2\n    svc #0\n");
                fprintf(out, "    mov x1, #10\n    mul x0, x0, x1\n");
                return TY_INT;
            }
            if (strcmp(n->text, "RectsOverlap") == 0) {
                // Pura aritmetica, sin syscall -- x1<x2+w2 && x2<x1+w1
                // && y1<y2+h2 && y2<y1+h1, el clasico test AABB.
                emit_expr(sc, n->list[0]); push_x0(); // x1
                emit_expr(sc, n->list[1]); push_x0(); // y1
                emit_expr(sc, n->list[2]); push_x0(); // w1
                emit_expr(sc, n->list[3]); push_x0(); // h1
                emit_expr(sc, n->list[4]); push_x0(); // x2
                emit_expr(sc, n->list[5]); push_x0(); // y2
                emit_expr(sc, n->list[6]); push_x0(); // w2
                emit_expr(sc, n->list[7]);             // h2 -> x0
                fprintf(out, "    mov x7, x0\n");        // h2
                fprintf(out, "    ldr x6, [sp], #16\n");  // w2
                fprintf(out, "    ldr x5, [sp], #16\n");  // y2
                fprintf(out, "    ldr x4, [sp], #16\n");  // x2
                fprintf(out, "    ldr x3, [sp], #16\n");  // h1
                fprintf(out, "    ldr x2, [sp], #16\n");  // w1
                fprintf(out, "    ldr x1, [sp], #16\n");  // y1
                fprintf(out, "    ldr x0, [sp], #16\n");  // x1
                int l = new_label();
                fprintf(out, "    add x8, x4, x6\n    cmp x0, x8\n    bge .Lro_false_%d\n", l); // x1 >= x2+w2 ?
                fprintf(out, "    add x8, x0, x2\n    cmp x4, x8\n    bge .Lro_false_%d\n", l); // x2 >= x1+w1 ?
                fprintf(out, "    add x8, x5, x7\n    cmp x1, x8\n    bge .Lro_false_%d\n", l); // y1 >= y2+h2 ?
                fprintf(out, "    add x8, x1, x3\n    cmp x5, x8\n    bge .Lro_false_%d\n", l); // y2 >= y1+h1 ?
                fprintf(out, "    mov x0, #1\n    b .Lro_done_%d\n", l);
                fprintf(out, ".Lro_false_%d:\n    mov x0, #0\n", l);
                fprintf(out, ".Lro_done_%d:\n", l);
                return TY_INT;
            }

            // -- LoadImage/DrawImage/ImageWidth/ImageHeight/ImagesCollide --
            //
            // Imagenes cargadas desde disco en nuestro propio formato
            // "NIMG" (ver la nota junto al pool de imagenes en
            // syscall.c) -- nada de PNG/JPEG de verdad. ImagesCollide
            // usa la caja delimitadora de cada imagen (su ancho/alto
            // real), no colision pixel a pixel.
            if (strcmp(n->text, "LoadImage") == 0) {
                emit_expr(sc, n->list[0]); // nombre$ -> x0
                fprintf(out, "    mov x8, #49\n    svc #0\n"); // SYS_LOAD_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "LoadIconStrip") == 0) {
                // LIMITACION DOCUMENTADA: BlitzPlus real espera un
                // .bmp de verdad; nosotros usamos nuestro propio
                // formato NIMG (convertir con nimg_convert.py) --
                // mismo mecanismo que LoadImage, solo que aqui la
                // imagen se interpreta como tira de iconos cuadrados.
                emit_expr(sc, n->list[0]); // nombre$ -> x0
                fprintf(out, "    mov x8, #169\n    svc #0\n"); // SYS_LOAD_ICON_STRIP
                return TY_INT;
            }
            if (strcmp(n->text, "FreeIconStrip") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #170\n    svc #0\n"); // SYS_FREE_ICON_STRIP
                return TY_INT;
            }
            if (strcmp(n->text, "SetGadgetIconStrip") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id de gadget
                emit_expr(sc, n->list[1]);             // handle de tira -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #171\n    svc #0\n"); // SYS_SET_GADGET_ICON_STRIP
                return TY_INT;
            }
            if (strcmp(n->text, "SetPanelColor") == 0) {
                // SetPanelColor panel, r, g, b
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]); push_x0(); // r
                emit_expr(sc, n->list[2]); push_x0(); // g
                emit_expr(sc, n->list[3]);             // b -> x0
                fprintf(out, "    mov x9, x0\n"); // b
                fprintf(out, "    ldr x0, [sp], #16\n"); // g
                fprintf(out, "    lsl x0, x0, #8\n");
                fprintf(out, "    orr x9, x9, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n"); // r
                fprintf(out, "    lsl x0, x0, #16\n");
                fprintf(out, "    orr x1, x9, x0\n"); // x1 = (r<<16|g<<8|b)
                fprintf(out, "    ldr x0, [sp], #16\n"); // id
                fprintf(out, "    mov x8, #207\n    svc #0\n"); // SYS_SET_PANEL_COLOR
                return TY_INT;
            }
            if (strcmp(n->text, "SetPanelImage") == 0) {
                // SetPanelImage panel, file$ -- carga la imagen y la
                // dibuja en mosaico, tal como documenta BlitzPlus real.
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // file$ -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #222\n    svc #0\n"); // SYS_SET_PANEL_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "CreateToolBar") == 0) {
                // CreateToolBar(image$,x,y,ancho,alto,[grupo]) -- ver
                // la misma limitacion de formato que LoadIconStrip.
                emit_expr(sc, n->list[0]); push_x0(); // image$
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                emit_expr(sc, n->list[3]); push_x0(); // ancho
                emit_expr(sc, n->list[4]);             // alto -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    lsl x0, x0, #16\n");
                fprintf(out, "    orr x3, x0, x1\n");   // x3 = (ancho<<16|alto)
                fprintf(out, "    ldr x2, [sp], #16\n");  // x2 = y
                fprintf(out, "    ldr x1, [sp], #16\n");   // x1 = x
                fprintf(out, "    ldr x0, [sp], #16\n");    // x0 = image$
                fprintf(out, "    mov x8, #172\n    svc #0\n"); // SYS_CREATE_TOOLBAR
                return TY_INT;
            }
            if (strcmp(n->text, "EnableToolBarItem") == 0 || strcmp(n->text, "DisableToolBarItem") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // indice -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    mov x2, #%d\n", strcmp(n->text, "EnableToolBarItem") == 0 ? 1 : 0);
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #173\n    svc #0\n"); // SYS_ENABLE_TOOLBAR_ITEM
                return TY_INT;
            }
            if (strcmp(n->text, "SetToolBarTips") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // id
                emit_expr(sc, n->list[1]);             // texto -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #174\n    svc #0\n"); // SYS_SET_TOOLBAR_TIPS
                return TY_INT;
            }
            if (strcmp(n->text, "TreeViewRoot") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #176\n    svc #0\n"); // SYS_TREEVIEW_ROOT
                return TY_INT;
            }
            if (strcmp(n->text, "AddTreeViewNode") == 0) {
                // AddTreeViewNode(texto$, padre)
                emit_expr(sc, n->list[0]); push_x0(); // texto
                emit_expr(sc, n->list[1]);             // padre -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #177\n    svc #0\n"); // SYS_ADD_TREEVIEW_NODE
                return TY_INT;
            }
            if (strcmp(n->text, "InsertTreeViewNode") == 0) {
                // InsertTreeViewNode(indice, texto$, padre)
                emit_expr(sc, n->list[0]); push_x0(); // indice
                emit_expr(sc, n->list[1]); push_x0(); // texto
                emit_expr(sc, n->list[2]);             // padre -> x0
                fprintf(out, "    mov x2, x0\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #178\n    svc #0\n"); // SYS_INSERT_TREEVIEW_NODE
                return TY_INT;
            }
            if (strcmp(n->text, "ModifyTreeViewNode") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // nodo
                emit_expr(sc, n->list[1]);             // texto -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #179\n    svc #0\n"); // SYS_MODIFY_TREEVIEW_NODE
                return TY_INT;
            }
            if (strcmp(n->text, "FreeTreeViewNode") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #180\n    svc #0\n"); // SYS_FREE_TREEVIEW_NODE
                return TY_INT;
            }
            if (strcmp(n->text, "ExpandTreeViewNode") == 0 || strcmp(n->text, "CollapseTreeViewNode") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x1, #%d\n", strcmp(n->text, "ExpandTreeViewNode") == 0 ? 1 : 0);
                fprintf(out, "    mov x8, #181\n    svc #0\n"); // SYS_EXPAND_TREEVIEW_NODE
                return TY_INT;
            }
            if (strcmp(n->text, "CountTreeViewNodes") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #182\n    svc #0\n"); // SYS_COUNT_TREEVIEW_NODES
                return TY_INT;
            }
            if (strcmp(n->text, "SelectedTreeViewNode") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #183\n    svc #0\n"); // SYS_SELECTED_TREEVIEW_NODE
                return TY_INT;
            }
            if (strcmp(n->text, "SelectTreeViewNode") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #184\n    svc #0\n"); // SYS_SELECT_TREEVIEW_NODE
                return TY_INT;
            }
            if (strcmp(n->text, "TreeViewNodeText$") == 0) {
                // Reutiliza rt_gadget_text -- el texto de un nodo vive
                // en el mismo campo 'text' que cualquier otro gadget.
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_gadget_text\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "CreateImage") == 0) {
                // Lienzo vacio (transparente), sin cargar nada de disco.
                emit_expr(sc, n->list[0]); push_x0(); // ancho
                emit_expr(sc, n->list[1]);             // alto -> x0
                fprintf(out, "    mov x1, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #52\n    svc #0\n"); // SYS_CREATE_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "LoadFont") == 0) {
                // LoadFont(nombre$[,alto][,negrita][,cursiva][,subrayado])
                // -- LIMITACION DOCUMENTADA: no renderizamos TrueType
                // de verdad, ver la nota en syscall.c junto a font_load.
                emit_expr(sc, n->list[0]); push_x0(); // nombre
                if (n->list_count > 1) emit_expr(sc, n->list[1]); else fprintf(out, "    mov x0, #12\n");
                push_x0(); // alto
                if (n->list_count > 2) emit_expr(sc, n->list[2]); else fprintf(out, "    mov x0, #0\n");
                push_x0(); // negrita
                if (n->list_count > 3) emit_expr(sc, n->list[3]); else fprintf(out, "    mov x0, #0\n");
                push_x0(); // cursiva
                if (n->list_count > 4) emit_expr(sc, n->list[4]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x4, x0\n"); // subrayado
                fprintf(out, "    ldr x3, [sp], #16\n"); // cursiva
                fprintf(out, "    ldr x2, [sp], #16\n"); // negrita
                fprintf(out, "    ldr x1, [sp], #16\n"); // alto
                fprintf(out, "    ldr x0, [sp], #16\n"); // nombre
                fprintf(out, "    mov x8, #189\n    svc #0\n"); // SYS_LOAD_FONT
                return TY_INT;
            }
            if (strcmp(n->text, "FreeFont") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #190\n    svc #0\n"); // SYS_FREE_FONT
                return TY_INT;
            }
            if (strcmp(n->text, "SetFont") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #191\n    svc #0\n"); // SYS_SET_FONT
                return TY_INT;
            }
            if (strcmp(n->text, "FontName$") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_font_name\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "FontSize") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #193\n    svc #0\n"); // SYS_FONT_SIZE
                return TY_INT;
            }
            if (strcmp(n->text, "FontStyle") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #194\n    svc #0\n"); // SYS_FONT_STYLE
                return TY_INT;
            }
            if (strcmp(n->text, "FontWidth") == 0) {
                fprintf(out, "    mov x8, #195\n    svc #0\n"); // SYS_FONT_WIDTH
                return TY_INT;
            }
            if (strcmp(n->text, "FontHeight") == 0) {
                fprintf(out, "    mov x8, #196\n    svc #0\n"); // SYS_FONT_HEIGHT
                return TY_INT;
            }
            if (strcmp(n->text, "SetGamma") == 0) {
                // SetGamma red,green,blue,dest_red#,dest_green#,dest_blue#
                emit_expr(sc, n->list[0]); push_x0(); // r
                emit_expr(sc, n->list[1]); push_x0(); // g
                emit_expr(sc, n->list[2]); push_x0(); // b
                emit_expr(sc, n->list[3]); push_x0(); // dest_r
                emit_expr(sc, n->list[4]); push_x0(); // dest_g
                emit_expr(sc, n->list[5]);             // dest_b -> x0
                fprintf(out, "    mov x9, x0\n"); // dest_b
                fprintf(out, "    ldr x0, [sp], #16\n"); // dest_g
                fprintf(out, "    lsl x0, x0, #8\n");
                fprintf(out, "    orr x9, x9, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n"); // dest_r
                fprintf(out, "    lsl x0, x0, #16\n");
                fprintf(out, "    orr x1, x9, x0\n"); // x1 = (dest_r<<16|dest_g<<8|dest_b)
                fprintf(out, "    ldr x0, [sp], #16\n"); // b
                fprintf(out, "    mov x9, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n"); // g
                fprintf(out, "    lsl x0, x0, #8\n");
                fprintf(out, "    orr x9, x9, x0\n");
                fprintf(out, "    ldr x0, [sp], #16\n"); // r
                fprintf(out, "    lsl x0, x0, #16\n");
                fprintf(out, "    orr x0, x9, x0\n"); // x0 = (r<<16|g<<8|b)
                fprintf(out, "    mov x8, #197\n    svc #0\n"); // SYS_SET_GAMMA
                return TY_INT;
            }
            if (strcmp(n->text, "UpdateGamma") == 0) {
                if (n->list_count > 0) emit_expr(sc, n->list[0]); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x8, #198\n    svc #0\n"); // SYS_UPDATE_GAMMA
                return TY_INT;
            }
            if (strcmp(n->text, "GammaRed") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #199\n    svc #0\n"); // SYS_GAMMA_RED
                return TY_INT;
            }
            if (strcmp(n->text, "GammaGreen") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #200\n    svc #0\n"); // SYS_GAMMA_GREEN
                return TY_INT;
            }
            if (strcmp(n->text, "GammaBlue") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #201\n    svc #0\n"); // SYS_GAMMA_BLUE
                return TY_INT;
            }
            if (strcmp(n->text, "GfxDriverName$") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    bl rt_gfx_driver_name\n");
                return TY_STRING;
            }
            if (strcmp(n->text, "GfxModeFormat") == 0) {
                emit_expr(sc, n->list[0]); // modo -- se evalua pero se ignora (un solo formato posible)
                fprintf(out, "    mov x8, #203\n    svc #0\n"); // SYS_GFX_MODE_FORMAT
                return TY_INT;
            }
            if (strcmp(n->text, "GraphicsFormat") == 0) {
                fprintf(out, "    mov x8, #204\n    svc #0\n"); // SYS_GRAPHICS_FORMAT
                return TY_INT;
            }
            if (strcmp(n->text, "TotalVidMem") == 0) {
                fprintf(out, "    mov x8, #205\n    svc #0\n"); // SYS_TOTAL_VID_MEM
                return TY_INT;
            }
            if (strcmp(n->text, "DrawImage") == 0) {
                emit_expr(sc, n->list[0]); push_x0(); // handle
                emit_expr(sc, n->list[1]); push_x0(); // x
                emit_expr(sc, n->list[2]); push_x0(); // y
                if (n->list_count > 3) emit_expr(sc, n->list[3]); // frame -> x0
                else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    mov x3, x0\n"); // frame -- solo importa si la imagen viene de LoadAnimImage
                fprintf(out, "    ldr x2, [sp], #16\n");
                fprintf(out, "    ldr x1, [sp], #16\n");
                fprintf(out, "    ldr x0, [sp], #16\n");
                fprintf(out, "    mov x8, #50\n    svc #0\n"); // SYS_DRAW_IMAGE
                return TY_INT;
            }
            if (strcmp(n->text, "ImageWidth") == 0 || strcmp(n->text, "ImageHeight") == 0) {
                emit_expr(sc, n->list[0]);
                fprintf(out, "    mov x8, #51\n    svc #0\n"); // SYS_IMAGE_SIZE -> (ancho<<32 | alto)
                if (strcmp(n->text, "ImageWidth") == 0) fprintf(out, "    lsr x0, x0, #32\n");
                else fprintf(out, "    and x0, x0, #0xFFFFFFFF\n");
                return TY_INT;
            }
            if (strcmp(n->text, "ImagesCollide") == 0) {
                emit_expr(sc, n->list[0]); // img1 -> x0
                fprintf(out, "    mov x8, #51\n    svc #0\n");
                push_x0(); // (w1<<32|h1)
                emit_expr(sc, n->list[1]); push_x0(); // x1
                emit_expr(sc, n->list[2]); push_x0(); // y1
                emit_expr(sc, n->list[4]); // img2 -> x0
                fprintf(out, "    mov x8, #51\n    svc #0\n");
                push_x0(); // (w2<<32|h2)
                emit_expr(sc, n->list[5]); push_x0(); // x2
                emit_expr(sc, n->list[6]); push_x0(); // y2

                fprintf(out, "    ldr x5, [sp], #16\n"); // y2
                fprintf(out, "    ldr x4, [sp], #16\n"); // x2
                fprintf(out, "    ldr x9, [sp], #16\n"); // (w2<<32|h2)
                fprintf(out, "    ldr x1, [sp], #16\n"); // y1
                fprintf(out, "    ldr x0, [sp], #16\n"); // x1
                fprintf(out, "    ldr x8, [sp], #16\n"); // (w1<<32|h1)
                fprintf(out, "    lsr x2, x8, #32\n");           // w1
                fprintf(out, "    and x3, x8, #0xFFFFFFFF\n");    // h1
                fprintf(out, "    lsr x6, x9, #32\n");             // w2
                fprintf(out, "    and x7, x9, #0xFFFFFFFF\n");      // h2

                int l = new_label();
                fprintf(out, "    add x8, x4, x6\n    cmp x0, x8\n    bge .Lic_false_%d\n", l);
                fprintf(out, "    add x8, x0, x2\n    cmp x4, x8\n    bge .Lic_false_%d\n", l);
                fprintf(out, "    add x8, x5, x7\n    cmp x1, x8\n    bge .Lic_false_%d\n", l);
                fprintf(out, "    add x8, x1, x3\n    cmp x5, x8\n    bge .Lic_false_%d\n", l);
                fprintf(out, "    mov x0, #1\n    b .Lic_done_%d\n", l);
                fprintf(out, ".Lic_false_%d:\n    mov x0, #0\n", l);
                fprintf(out, ".Lic_done_%d:\n", l);
                return TY_INT;
            }

            // -- Sqr/Sin/Cos: coma flotante de verdad. Sqr usa la
            // instruccion de raiz cuadrada del propio procesador;
            // Sin/Cos son una aproximacion por serie de Taylor escrita
            // a mano (ver rt_sin/rt_cos/rt_deg_reduce), ya que no
            // tenemos ninguna libreria matematica enlazada. Los
            // angulos van en GRADOS, como en BlitzPlus real.
            if (strcmp(n->text, "Sqr") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    fmov d0, x0\n    fsqrt d0, d0\n    fmov x0, d0\n");
                return TY_FLOAT;
            }
            if (strcmp(n->text, "Sin") == 0 || strcmp(n->text, "Cos") == 0) {
                ValType t = emit_expr(sc, n->list[0]);
                if (t != TY_FLOAT) fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
                fprintf(out, "    bl %s\n", strcmp(n->text, "Sin") == 0 ? "rt_sin" : "rt_cos");
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
                memset(&fake_index, 0, sizeof(fake_index));
                fake_index.kind = N_INDEX;
                fake_index.line = n->line;
                strncpy(fake_index.text, n->text, sizeof(fake_index.text) - 1);
                fake_index.list = n->list;
                fake_index.list_count = n->list_count;
                emit_index_address(sc, &fake_index);
                fprintf(out, "    ldr x0, [x0]\n");
                return TY_INT;
            }

            if (!is_function(n->text)) {
                fprintf(stderr, "linea %d: funcion '%s' no declarada\n", n->line, n->text);
                exit(1);
            }
            // Evaluamos los argumentos dados y los apilamos en orden;
            // si la funcion declara mas parametros de los que nos dan
            // aqui, completamos los que faltan con su valor por
            // defecto (ej. "group=0, style=15" en la definicion) --
            // asi "CreateWindow(titulo,x,y,w,h)" funciona igual que
            // "CreateWindow(titulo,x,y,w,h,0,15)".
            Node *fdef = find_funcdef(n->text);
            int declared = fdef ? fdef->list_count : n->list_count;
            for (int i = 0; i < n->list_count; i++) {
                emit_expr(sc, n->list[i]);
                push_x0();
            }
            for (int i = n->list_count; i < declared && i < 8; i++) {
                Node *param = fdef->list[i];
                if (param->b) emit_expr(sc, param->b);
                else fprintf(out, "    mov x0, #0\n"); // sin valor dado ni por defecto
                push_x0();
            }
            int total = (n->list_count > declared) ? n->list_count : declared;
            if (total > 8) total = 8;
            for (int i = total - 1; i >= 0; i--) {
                fprintf(out, "    ldr x%d, [sp], #16\n", i);
            }
            fprintf(out, "    bl func_%s\n", n->text);
            return is_string_name(n->text) ? TY_STRING : TY_INT;
        }

        default:
            fprintf(stderr, "linea %d: nodo de expresion no soportado (%d)\n", n->line, n->kind);
            exit(1);
    }
}

static void emit_assign(FuncScope *sc, Node *n) {
    Node *target = n->a;
    if (target->kind == N_INDEX) {
        emit_index_address(sc, target); // direccion -> x0
        push_x0();
        emit_expr(sc, n->b); // valor -> x0
        pop_to_x1(); // x1 = direccion, x0 = valor
        fprintf(out, "    str x0, [x1]\n");
        return;
    }
    if (target->kind == N_FIELD) {
        emit_field_address(sc, target); // direccion -> x0
        push_x0();
        ValType src_t = emit_expr(sc, n->b); // valor -> x0
        if (is_float_name(target->text) && src_t == TY_INT) {
            fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
        } else if (!is_float_name(target->text) && !is_string_name(target->text) && src_t == TY_FLOAT) {
            fprintf(out, "    fmov d0, x0\n    fcvtzs x0, d0\n");
        }
        pop_to_x1(); // x1 = direccion, x0 = valor
        fprintf(out, "    str x0, [x1]\n");
        return;
    }
    ValType src_t = emit_expr(sc, n->b); // valor -> x0
    // Si el tipo del valor no coincide con el de la variable destino
    // (entero/flotante -- las cadenas no se mezclan, eso ya seria un
    // error de otro tipo), convertimos automaticamente, igual que
    // BlitzPlus real.
    if (is_float_name(target->text) && src_t == TY_INT) {
        fprintf(out, "    scvtf d0, x0\n    fmov x0, d0\n");
    } else if (!is_float_name(target->text) && !is_string_name(target->text) && src_t == TY_FLOAT) {
        fprintf(out, "    fmov d0, x0\n    fcvtzs x0, d0\n");
    }
    push_x0();
    emit_var_address(sc, target->text); // direccion -> x0
    pop_to_x1(); // x1 = valor, x0 = direccion
    fprintf(out, "    str x1, [x0]\n");
}

// Read: lee el siguiente valor de rt_data_table (donde apunte
// rt_data_ptr en este momento) y avanza el puntero 8 bytes. El valor
// se copia tal cual a la variable destino -- sea un entero o un
// puntero a cadena, no hace falta distinguir tipos aqui: cada Data ya
// puso el valor correcto en su sitio al generarse la tabla.
static void emit_read(FuncScope *sc, Node *n) {
    fprintf(out, "    adrp x9, rt_data_ptr\n");
    fprintf(out, "    add x9, x9, :lo12:rt_data_ptr\n");
    fprintf(out, "    ldr x10, [x9]\n");
    fprintf(out, "    ldr x0, [x10]\n");
    fprintf(out, "    add x10, x10, #8\n");
    fprintf(out, "    str x10, [x9]\n");
    push_x0();
    if (n->a->kind == N_INDEX) {
        emit_index_address(sc, n->a); // direccion del elemento -> x0
    } else {
        emit_var_address(sc, n->a->text); // direccion de la variable -> x0
    }
    pop_to_x1(); // x1 = valor leido, x0 = direccion destino
    fprintf(out, "    str x1, [x0]\n");
}

// Restore: mueve el puntero de lectura a una etiqueta (o al principio
// de la tabla si no se da ninguna).
static void emit_restore(Node *n) {
    fprintf(out, "    adrp x9, rt_data_ptr\n");
    fprintf(out, "    add x9, x9, :lo12:rt_data_ptr\n");
    if (n->text[0] != '\0') {
        fprintf(out, "    adrp x10, dl_%s\n", n->text);
        fprintf(out, "    add x10, x10, :lo12:dl_%s\n", n->text);
    } else {
        fprintf(out, "    adrp x10, rt_data_table\n");
        fprintf(out, "    add x10, x10, :lo12:rt_data_table\n");
    }
    fprintf(out, "    str x10, [x9]\n");
}

// Delete variable -- desenlaza la instancia de la lista de su tipo.
// Como usamos lista enlazada SIMPLE (solo 'next'), hay que recorrer
// desde la cabeza llevando la cuenta del "anterior" hasta encontrar
// la instancia buscada -- O(n), pero sencillo y de sobra para el
// tamaño de listas que se esperan en la practica.
static void emit_delete(Node *n) {
    const char *tname = (n->a->kind == N_VAR) ? find_var_type(n->a->text) : NULL;
    if (!tname) {
        fprintf(stderr, "linea %d: no se pudo determinar el tipo de la variable en 'Delete' (¿le falta un '= New Tipo' antes?)\n", n->line);
        exit(1);
    }
    tname = canon_type_name(tname);
    int l = new_label();
    emit_expr(NULL, n->a); // x0 = instancia a borrar (target)
    fprintf(out, "    mov x19, x0\n"); // registro llamador-guarda -- ver nota abajo
    fprintf(out, "    adrp x9, type_%s_head\n", tname);
    fprintf(out, "    add x9, x9, :lo12:type_%s_head\n", tname);
    fprintf(out, "    ldr x10, [x9]\n");   // x10 = actual, empieza en head
    fprintf(out, "    mov x11, #0\n");      // x11 = anterior (0 = ninguno aun)
    fprintf(out, ".Ldel_scan_%d:\n", l);
    fprintf(out, "    cmp x10, x19\n");
    fprintf(out, "    beq .Ldel_found_%d\n", l);
    fprintf(out, "    cbz x10, .Ldel_done_%d\n", l); // no estaba en la lista -- no hacemos nada
    fprintf(out, "    mov x11, x10\n");
    fprintf(out, "    ldr x10, [x10]\n");
    fprintf(out, "    b .Ldel_scan_%d\n", l);
    fprintf(out, ".Ldel_found_%d:\n", l);
    fprintf(out, "    ldr x12, [x10]\n"); // x12 = target->next
    fprintf(out, "    cbz x11, .Ldel_was_head_%d\n", l);
    fprintf(out, "    str x12, [x11]\n"); // anterior->next = target->next
    fprintf(out, "    b .Ldel_fix_tail_%d\n", l);
    fprintf(out, ".Ldel_was_head_%d:\n", l);
    fprintf(out, "    str x12, [x9]\n"); // head = target->next
    fprintf(out, ".Ldel_fix_tail_%d:\n", l);
    fprintf(out, "    adrp x13, type_%s_tail\n", tname);
    fprintf(out, "    add x13, x13, :lo12:type_%s_tail\n", tname);
    fprintf(out, "    ldr x14, [x13]\n");
    fprintf(out, "    cmp x14, x19\n");
    fprintf(out, "    bne .Ldel_done_%d\n", l);
    fprintf(out, "    str x11, [x13]\n"); // tail = anterior (borrabamos el ultimo de la lista)
    fprintf(out, ".Ldel_done_%d:\n", l);
}

// Insert instancia Before/After objetivo -- mueve una instancia YA
// EXISTENTE a una posicion nueva dentro de la lista enlazada de su
// tipo. Se hace en dos pasos: (1) desenlazar la instancia de donde
// este ahora (identico a Delete, pero sin perderla), (2) reenlazarla
// justo antes o despues del objetivo. "Before" necesita un SEGUNDO
// recorrido para encontrar el anterior del objetivo -- se hace SOBRE
// la lista ya actualizada tras el paso 1, asi que no puede toparse
// con la propia instancia que acabamos de sacar.
static void emit_insert(Node *n) {
    const char *tname = infer_type_name_from_expr(n->a);
    if (!tname) tname = infer_type_name_from_expr(n->b);
    if (!tname) {
        fprintf(stderr, "linea %d: no se pudo determinar el tipo de la instancia en 'Insert'\n", n->line);
        exit(1);
    }
    tname = canon_type_name(tname);
    int l = new_label();
    emit_expr(NULL, n->a); // x0 = instancia a mover
    fprintf(out, "    mov x19, x0\n");
    emit_expr(NULL, n->b); // x0 = instancia objetivo
    fprintf(out, "    mov x20, x0\n");

    fprintf(out, "    adrp x9, type_%s_head\n", tname);
    fprintf(out, "    add x9, x9, :lo12:type_%s_head\n", tname);
    fprintf(out, "    adrp x13, type_%s_tail\n", tname);
    fprintf(out, "    add x13, x13, :lo12:type_%s_tail\n", tname);

    // Paso 1: desenlazar x19 de donde este ahora
    fprintf(out, "    ldr x10, [x9]\n");   // x10 = actual, empieza en head
    fprintf(out, "    mov x11, #0\n");      // x11 = anterior
    fprintf(out, ".Lins_scan1_%d:\n", l);
    fprintf(out, "    cmp x10, x19\n");
    fprintf(out, "    beq .Lins_found1_%d\n", l);
    fprintf(out, "    cbz x10, .Lins_relink_%d\n", l); // no deberia pasar, pero por seguridad
    fprintf(out, "    mov x11, x10\n");
    fprintf(out, "    ldr x10, [x10]\n");
    fprintf(out, "    b .Lins_scan1_%d\n", l);
    fprintf(out, ".Lins_found1_%d:\n", l);
    fprintf(out, "    ldr x12, [x10]\n"); // x12 = x19->next
    fprintf(out, "    cbz x11, .Lins_was_head1_%d\n", l);
    fprintf(out, "    str x12, [x11]\n"); // anterior->next = x19->next
    fprintf(out, "    b .Lins_fix_tail1_%d\n", l);
    fprintf(out, ".Lins_was_head1_%d:\n", l);
    fprintf(out, "    str x12, [x9]\n"); // head = x19->next
    fprintf(out, ".Lins_fix_tail1_%d:\n", l);
    fprintf(out, "    ldr x14, [x13]\n");
    fprintf(out, "    cmp x14, x19\n");
    fprintf(out, "    bne .Lins_relink_%d\n", l);
    fprintf(out, "    str x11, [x13]\n"); // tail = anterior (x19 era el ultimo)

    // Paso 2: reenlazar x19 antes o despues de x20
    fprintf(out, ".Lins_relink_%d:\n", l);
    if (n->op == T_KW_AFTER) {
        fprintf(out, "    ldr x21, [x20]\n"); // x21 = objetivo->next (0 si era el ultimo)
        fprintf(out, "    str x21, [x19]\n"); // x19->next = objetivo->next
        fprintf(out, "    str x19, [x20]\n"); // objetivo->next = x19
        fprintf(out, "    cbnz x21, .Lins_done_%d\n", l);
        fprintf(out, "    str x19, [x13]\n"); // tail = x19 (el objetivo era el ultimo)
    } else { // T_KW_BEFORE
        fprintf(out, "    ldr x10, [x9]\n"); // reiniciamos el recorrido desde el head (ya actualizado)
        fprintf(out, "    mov x11, #0\n");
        fprintf(out, ".Lins_scan2_%d:\n", l);
        fprintf(out, "    cmp x10, x20\n");
        fprintf(out, "    beq .Lins_found2_%d\n", l);
        fprintf(out, "    cbz x10, .Lins_done_%d\n", l); // no deberia pasar
        fprintf(out, "    mov x11, x10\n");
        fprintf(out, "    ldr x10, [x10]\n");
        fprintf(out, "    b .Lins_scan2_%d\n", l);
        fprintf(out, ".Lins_found2_%d:\n", l);
        fprintf(out, "    str x20, [x19]\n"); // x19->next = objetivo
        fprintf(out, "    cbz x11, .Lins_was_head2_%d\n", l);
        fprintf(out, "    str x19, [x11]\n"); // anterior_del_objetivo->next = x19
        fprintf(out, "    b .Lins_done_%d\n", l);
        fprintf(out, ".Lins_was_head2_%d:\n", l);
        fprintf(out, "    str x19, [x9]\n"); // head = x19 (el objetivo era el primero)
    }
    fprintf(out, ".Lins_done_%d:\n", l);
}

// For variable = Each Tipo ... Next -- recorre la lista enlazada
// simple de ese tipo desde el head, en orden de creacion.
static void emit_foreach(FuncScope *sc, Node *n) {
    const char *tname = canon_type_name(n->a->text);
    int l = new_label();
    push_loop(l, "foreach");

    emit_var_address(sc, n->text); // direccion de la variable -> x0
    fprintf(out, "    adrp x9, type_%s_head\n", tname);
    fprintf(out, "    add x9, x9, :lo12:type_%s_head\n", tname);
    fprintf(out, "    ldr x9, [x9]\n"); // x9 = head
    fprintf(out, "    str x9, [x0]\n"); // variable = head

    fprintf(out, ".Lforeach_%d:\n", l);
    emit_var_address(sc, n->text);
    fprintf(out, "    ldr x1, [x0]\n"); // x1 = valor actual de la variable
    fprintf(out, "    cmp x1, #0\n");
    fprintf(out, "    beq .Lforeach_end_%d\n", l);

    emit_block(sc, n->b);

    emit_var_address(sc, n->text);
    fprintf(out, "    ldr x1, [x0]\n"); // x1 = instancia actual
    fprintf(out, "    ldr x1, [x1]\n"); // x1 = instancia actual->next
    fprintf(out, "    str x1, [x0]\n"); // variable = variable->next
    fprintf(out, "    b .Lforeach_%d\n", l);
    fprintf(out, ".Lforeach_end_%d:\n", l);
    pop_loop();
}

static void emit_if(FuncScope *sc, Node *n) {
    int l = new_label();
    emit_expr(sc, n->a);
    fprintf(out, "    cmp x0, #0\n");
    fprintf(out, "    beq .Lelse_%d\n", l);
    emit_block(sc, n->b);
    fprintf(out, "    b .Lend_%d\n", l);
    fprintf(out, ".Lelse_%d:\n", l);

    for (int i = 0; i < n->list_count; i++) {
        Node *ei = n->list[i];
        int el = new_label();
        emit_expr(sc, ei->a);
        fprintf(out, "    cmp x0, #0\n");
        fprintf(out, "    beq .Lelseif_%d\n", el);
        emit_block(sc, ei->b);
        fprintf(out, "    b .Lend_%d\n", l);
        fprintf(out, ".Lelseif_%d:\n", el);
    }
    if (n->c) emit_block(sc, n->c);
    fprintf(out, ".Lend_%d:\n", l);
}

static void emit_for(FuncScope *sc, Node *n) {
    int l = new_label();
    push_loop(l, "for");
    emit_var_address(sc, n->text);
    push_x0(); // direccion de la variable del bucle, la necesitaremos varias veces
    emit_expr(sc, n->a); // valor inicial
    fprintf(out, "    ldr x1, [sp]\n");
    fprintf(out, "    str x0, [x1]\n");

    fprintf(out, ".Lfor_%d:\n", l);
    emit_expr(sc, n->b); // limite "To"
    push_x0();
    fprintf(out, "    ldr x1, [sp, #16]\n"); // direccion de la variable (esta 16 bytes mas abajo)
    fprintf(out, "    ldr x0, [x1]\n");
    fprintf(out, "    ldr x2, [sp], #16\n"); // limite
    fprintf(out, "    cmp x0, x2\n");
    fprintf(out, "    bgt .Lfor_end_%d\n", l); // v1: solo pasos positivos (step<=0 no soportado aun)

    emit_block(sc, n->d);

    fprintf(out, "    ldr x1, [sp]\n");
    fprintf(out, "    ldr x0, [x1]\n");
    if (n->c) {
        emit_expr(sc, n->c);
        fprintf(out, "    ldr x1, [sp]\n");
        fprintf(out, "    ldr x2, [x1]\n");
        fprintf(out, "    add x2, x2, x0\n");
        fprintf(out, "    str x2, [x1]\n");
    } else {
        fprintf(out, "    add x0, x0, #1\n");
        fprintf(out, "    str x0, [x1]\n");
    }
    fprintf(out, "    b .Lfor_%d\n", l);
    fprintf(out, ".Lfor_end_%d:\n", l);
    fprintf(out, "    add sp, sp, #16\n"); // liberamos la direccion que guardamos al principio
    pop_loop();
}

static void emit_while(FuncScope *sc, Node *n) {
    int l = new_label();
    push_loop(l, "while");
    fprintf(out, ".Lwhile_%d:\n", l);
    emit_expr(sc, n->a);
    fprintf(out, "    cmp x0, #0\n");
    fprintf(out, "    beq .Lwhile_end_%d\n", l);
    emit_block(sc, n->b);
    fprintf(out, "    b .Lwhile_%d\n", l);
    fprintf(out, ".Lwhile_end_%d:\n", l);
    pop_loop();
}

static void emit_repeat(FuncScope *sc, Node *n) {
    int l = new_label();
    push_loop(l, "repeat");
    fprintf(out, ".Lrepeat_%d:\n", l);
    emit_block(sc, n->a);
    if (n->b) {
        emit_expr(sc, n->b);
        fprintf(out, "    cmp x0, #0\n");
        fprintf(out, "    beq .Lrepeat_%d\n", l);
    } else {
        fprintf(out, "    b .Lrepeat_%d\n", l); // Forever
    }
    fprintf(out, ".Lrepeat_end_%d:\n", l);
    pop_loop();
}

static void emit_print(FuncScope *sc, Node *n) {
    for (int i = 0; i < n->list_count; i++) {
        ValType t = emit_expr(sc, n->list[i]);
        if (t == TY_INT) fprintf(out, "    bl rt_int_to_str\n");
        else if (t == TY_FLOAT) fprintf(out, "    bl rt_float_to_str\n");
        fprintf(out, "    mov x8, #%d\n", SYS_WRITE_STRING);
        fprintf(out, "    svc #0\n");
    }
    const char *nl = intern_string("\n");
    fprintf(out, "    adrp x0, %s\n    add x0, x0, :lo12:%s\n", nl, nl);
    fprintf(out, "    mov x8, #%d\n    svc #0\n", SYS_WRITE_STRING);
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
    else fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n    ldr x0, [x9]\n");
    fprintf(out, "    mov x4, x0\n");
    fprintf(out, "    ldr x3, [sp], #16\n");
    fprintf(out, "    ldr x2, [sp], #16\n");
    fprintf(out, "    ldr x1, [sp], #16\n");
    fprintf(out, "    ldr x0, [sp], #16\n");
    fprintf(out, "    mov x8, #%d\n", SYS_DRAW_RECT);
    fprintf(out, "    svc #0\n");
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
                if (n->a) emit_expr(sc, n->a); else fprintf(out, "    mov x0, #0\n");
                fprintf(out, "    b .Lfunc_end_%s\n", g_current_func_name);
            } else {
                // "Return" fuera de una funcion -- viene de un Gosub,
                // usa la pila software propia (no la pila de llamadas
                // normal, que un Gosub anidado sobreescribiria).
                fprintf(out, "    bl rt_gosub_return\n");
            }
            return;
        case N_LABEL:
            fprintf(out, ".Luser_lbl_%s:\n", n->text);
            return;
        case N_GOTO:
            fprintf(out, "    b .Luser_lbl_%s\n", n->text);
            return;
        case N_GOSUB: {
            int l = new_label();
            fprintf(out, "    adrp x9, .Lgosub_ret_%d\n", l);
            fprintf(out, "    add x9, x9, :lo12:.Lgosub_ret_%d\n", l);
            fprintf(out, "    adrp x10, rt_gosub_sp\n    add x10, x10, :lo12:rt_gosub_sp\n    ldr x11, [x10]\n");
            fprintf(out, "    adrp x12, rt_gosub_stack\n    add x12, x12, :lo12:rt_gosub_stack\n");
            fprintf(out, "    lsl x13, x11, #3\n    add x13, x12, x13\n    str x9, [x13]\n");
            fprintf(out, "    add x11, x11, #1\n    str x11, [x10]\n");
            fprintf(out, "    b .Luser_lbl_%s\n", n->text);
            fprintf(out, ".Lgosub_ret_%d:\n", l);
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
                    fprintf(out, "    str x1, [x0]\n");
                }
            }
            return;
        case N_DIM: return; // el espacio ya se reservo en la fase de recoleccion
        case N_CLS: {
            // Usa rt_cls_color (por defecto negro, via el cero de .bss),
            // SEPARADA de rt_current_color (la de dibujo, por defecto
            // blanco) -- asi coincide con BlitzPlus real, donde Cls y
            // Color son dos estados independientes. ClsColor() cambia
            // esta.
            fprintf(out, "    mov x0, #0\n    mov x1, #0\n    mov x2, #4096\n    mov x3, #4096\n");
            fprintf(out, "    adrp x9, rt_cls_color\n    add x9, x9, :lo12:rt_cls_color\n    ldr x4, [x9]\n");
            fprintf(out, "    mov x8, #%d\n    svc #0\n", SYS_DRAW_RECT);
            return;
        }
        case N_PLOT: {
            Node one; memset(&one, 0, sizeof(one)); one.kind = N_NUM; one.num_value = 1;
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
            else fprintf(out, "    adrp x9, rt_current_color\n    add x9, x9, :lo12:rt_current_color\n    ldr x0, [x9]\n");
            fprintf(out, "    mov x4, x0\n");
            fprintf(out, "    ldr x3, [sp], #16\n");
            fprintf(out, "    ldr x2, [sp], #16\n");
            fprintf(out, "    ldr x1, [sp], #16\n");
            fprintf(out, "    ldr x0, [sp], #16\n");
            fprintf(out, "    bl rt_draw_line\n");
            return;
        }
        case N_DELAY: {
            // SYS_SLEEP (numero 1): a0=ticks (100 ticks = 1 segundo)
            emit_expr(sc, n->list[0]);
            fprintf(out, "    mov x8, #1\n");
            fprintf(out, "    svc #0\n");
            return;
        }
        case N_ENDPROGRAM:
            // Salta directo al epilogo de _start -- funciona igual
            // este 'End' este dentro de un bucle, un If, o donde sea.
            fprintf(out, "    b .Lprogram_end\n");
            return;
        case N_BLOCK:
            // Un bloque puede aparecer como sentencia suelta -- lo usa
            // el desazucarado de Select/Case (variable temporal +
            // cadena de If/ElseIf, empaquetados juntos en un bloque).
            emit_block(sc, n);
            return;
        case N_EXIT:
            if (loop_depth == 0) {
                fprintf(stderr, "linea %d: 'Exit' fuera de un bucle\n", n->line);
                exit(1);
            }
            fprintf(out, "    b .L%s_end_%d\n", loop_stack[loop_depth - 1].prefix, loop_stack[loop_depth - 1].label);
            return;
        case N_DATA:      return; // ya recogido por collect_data -- no genera codigo aqui
        case N_DATALABEL:
            // En BlitzPlus real, ".etiqueta" sirve TANTO para Restore
            // (que ya funciona via una tabla aparte en tiempo de
            // compilacion, sin tocar esto) COMO para Goto/Gosub (que
            // SI necesitan una etiqueta de ensamblador de verdad aqui).
            fprintf(out, ".Luser_lbl_%s:\n", n->text);
            return;
        case N_READ:      emit_read(sc, n); return;
        case N_RESTORE:   emit_restore(n); return;
        case N_TYPEDEF:   return; // ya recogido por collect_type_info -- no genera codigo aqui
        case N_DELETE:    emit_delete(n); return;
        case N_INSERT:    emit_insert(n); return;
        case N_FOREACH:   emit_foreach(sc, n); return;
        default:
            fprintf(stderr, "linea %d: sentencia no soportada (%d)\n", n->line, n->kind);
            exit(1);
    }
}

static void emit_block(FuncScope *sc, Node *block) {
    for (int i = 0; i < block->list_count; i++) emit_stmt(sc, block->list[i]);
}

// ---- rutinas de apoyo, incluidas una vez en todo programa compilado ----

static void emit_runtime_helpers(void) {
    fprintf(out,
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
        "// punto (recortando ceros sobrantes). Reutiliza rt_int_to_str\n"
        "// para la parte entera (con su signo), y calcula la parte\n"
        "// fraccionaria por separado, redondeando al sumar 0.5 antes\n"
        "// de truncar.\n"
        "rt_float_to_str:\n"
        "    stp x29, x30, [sp, #-48]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    stp x21, x22, [sp, #32]\n"
        "    mov x21, x0\n"          // bits originales del double
        "    fmov d0, x0\n"
        "    fcvtzs x0, d0\n"        // parte entera, truncada hacia 0 (con signo)
        "    bl rt_int_to_str\n"
        "    mov x19, x0\n"          // x19 = texto de la parte entera (ya con su signo)
        "    fmov d0, x21\n"
        "    fabs d0, d0\n"          // |valor|
        "    fcvtzs x9, d0\n"        // parte entera de |valor|
        "    scvtf d1, x9\n"
        "    fsub d0, d0, d1\n"      // fraccion, en [0,1)
        "    adrp x9, rt_half\n"
        "    add x9, x9, :lo12:rt_half\n"
        "    ldr x9, [x9]\n"
        "    fmov d2, x9\n"
        "    mov x9, #1000000\n"
        "    scvtf d1, x9\n"
        "    fmul d0, d0, d1\n"
        "    fadd d0, d0, d2\n"      // +0.5 para redondear
        "    fcvtzs x22, d0\n"       // digitos fraccionarios (0..1000000)
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
        "67:\n"                     // copiamos la parte entera (con su signo)
        "    ldrb w13, [x19], #1\n"
        "    cbz w13, 68f\n"
        "    strb w13, [x12], #1\n"
        "    b 67b\n"
        "68:\n"
        "    mov w13, #46\n"        // '.'
        "    strb w13, [x12], #1\n"
        "    mov x14, #100000\n"    // 6 digitos fraccionarios, de mayor a menor peso
        "69:\n"
        "    udiv x15, x22, x14\n"
        "    add x16, x15, #48\n"
        "    strb w16, [x12], #1\n"
        "    msub x22, x15, x14, x22\n"
        "    mov x17, #10\n"
        "    udiv x14, x14, x17\n"
        "    cbnz x14, 69b\n"
        "71:\n"                     // recortamos ceros finales sobrantes
        "    ldrb w17, [x12, #-1]\n"
        "    cmp w17, #48\n"
        "    bne 72f\n"
        "    sub x12, x12, #1\n"
        "    b 71b\n"
        "72:\n"                     // y el punto decimal, si no queda nada tras el
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
        "// rt_trim: x0=cadena -- quita espacios/caracteres no\n"
        "// imprimibles de ambos extremos (igual que BlitzPlus real,\n"
        "// que usa isgraph()).\n"
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
        "// rt_lset: x0=cadena, x1=n -- trunca si es mas larga que n, o\n"
        "// rellena con espacios A LA DERECHA si es mas corta (justifica\n"
        "// a la izquierda).\n"
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
        "// rt_rset: x0=cadena, x1=n -- si es mas larga que n, se queda\n"
        "// con los ULTIMOS n caracteres; si es mas corta, rellena con\n"
        "// espacios A LA IZQUIERDA (justifica a la derecha).\n"
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
        "// rt_hex: x0=entero -- cadena de 8 digitos hexadecimales en\n"
        "// mayusculas, siempre con ceros a la izquierda (no se recortan).\n"
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
        "// rt_bin: x0=entero -- cadena de 32 digitos binarios, siempre\n"
        "// con ceros a la izquierda.\n"
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
        "// rt_string_repeat: x0=cadena, x1=repeticiones -- concatena la\n"
        "// cadena consigo misma n veces.\n"
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
        // rt_rand: version ENTERA -- la usa Rand(min,max). En BlitzPlus
        // real es INCLUSIVA en los dos extremos (no como Mod), y si
        // 'to' viene menor que 'from' los intercambia en vez de fallar
        // -- verificado contra el codigo fuente real de BlitzPlus.
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
        "    add x13, x13, #1\n" // rango INCLUSIVO: to-from+1
        "    udiv x14, x12, x13\n"
        "    msub x15, x14, x13, x12\n"
        "    add x0, x0, x15\n"
        "    ret\n"
        "\n"
        "// rt_rnd_float: x0=bits de minimo(double), x1=bits de\n"
        "// maximo(double) -- devuelve un flotante uniforme en [min,max),\n"
        "// la version de verdad de Rnd() en BlitzPlus real (a diferencia\n"
        "// de Rand(), que es la version entera).\n"
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
        "    and x12, x12, #0x7FFFFFFF\n" // 31 bits, siempre no-negativo
        "    scvtf d0, x12\n"
        "    adrp x9, rt_const_2_31\n"
        "    add x9, x9, :lo12:rt_const_2_31\n"
        "    ldr x9, [x9]\n"
        "    fmov d1, x9\n"
        "    fdiv d0, d0, d1\n"          // d0 = aleatorio en [0,1)
        "    fsub d2, d5, d4\n"           // d2 = max-min
        "    fmul d0, d0, d2\n"
        "    fadd d0, d0, d4\n"
        "    fmov x0, d0\n"
        "    ret\n"
        "\n"
        "// rt_seedrnd: x0=nueva semilla -- para reproducir la misma\n"
        "// secuencia \"aleatoria\" a proposito.\n"
        "rt_seedrnd:\n"
        "    adrp x9, rt_rnd_seed\n"
        "    add x9, x9, :lo12:rt_rnd_seed\n"
        "    str x0, [x9]\n"
        "    ret\n"
        "\n"
        "// rt_locate: x0=x, x1=y -- construye una secuencia de escape de\n"
        "// 3 bytes (marcador 0x01, x+1, y+1 -- el +1 evita mandar un\n"
        "// byte 0 real, que cortaria la cadena antes de tiempo) y la\n"
        "// envia por SYS_WRITE_STRING, el mismo canal que ya usa Print.\n"
        "// shell.pro reconoce el marcador y posiciona su cursor de\n"
        "// texto ahi, en vez de tratarlo como texto normal.\n"
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
        "    mov x8, #11\n" // SYS_WRITE_STRING
        "    svc #0\n"
        "    ret\n"
        "\n"
        "// rt_waitkey: bombea el sistema y consulta GetKey en bucle hasta\n"
        "// que llegue una tecla. Devuelve su scancode.\n"
        "rt_waitkey:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "200:\n"
        "    mov x8, #14\n" // SYS_PUMP
        "    svc #0\n"
        "    mov x8, #54\n" // SYS_GET_KEY
        "    svc #0\n"
        "    cbz x0, 200b\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_waitmouse: igual que rt_waitkey pero para los botones del\n"
        "// raton -- devuelve 1 si fue el izquierdo, 2 si fue el derecho.\n"
        "rt_waitmouse:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "201:\n"
        "    mov x8, #14\n"
        "    svc #0\n"
        "    mov x0, #1\n"
        "    mov x8, #56\n" // SYS_MOUSE_HIT
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
        "// rt_input: x0=puntero al prompt (o bits=0 si no hay) --\n"
        "// muestra el prompt, y va leyendo caracteres con eco visual y\n"
        "// soporte de backspace hasta Enter, devolviendo lo escrito.\n"
        "rt_input:\n"
        "    stp x29, x30, [sp, #-32]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    cbz x0, 211f\n"
        "    mov x8, #11\n" // SYS_WRITE_STRING
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
        "    mov x8, #13\n" // SYS_READ_CHAR_WAIT
        "    svc #0\n"
        "    cmp x0, #0\n"
        "    blt 213f\n" // negativo (ventana cerrada/redimensionada): tratamos como fin de entrada
        "    cmp x0, #10\n"
        "    beq 213f\n"
        "    cmp x0, #8\n"
        "    beq 215f\n"
        "    cmp x20, #126\n"
        "    bge 210b\n" // buffer lleno -- ignoramos el caracter, seguimos esperando
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
        "// rt_rndseed: devuelve la semilla actual del generador -- para\n"
        "// poder guardarla y reproducir la misma secuencia mas tarde.\n"
        "rt_rndseed:\n"
        "    adrp x9, rt_rnd_seed\n"
        "    add x9, x9, :lo12:rt_rnd_seed\n"
        "    ldr x0, [x9]\n"
        "    ret\n"
        "\n"
        "// rt_tileimage: x0=handle, x1=x, x2=y -- cubre TODA la ventana\n"
        "// con copias de la imagen, en una rejilla de su mismo tamaño.\n"
        "// x,y son un desplazamiento de fase (para efectos de scroll):\n"
        "// se reducen con modulo CON SIGNO al tamaño de la imagen antes\n"
        "// de calcular donde empieza la rejilla, asi que funcionan tanto\n"
        "// positivos como negativos. LIMITACION: como SYS_DRAW_IMAGE no\n"
        "// recorta en coordenadas negativas, los mosaicos parciales que\n"
        "// caerian fuera por la izquierda/arriba no se dibujan -- para\n"
        "// mosaico fijo desde (0,0) esto no afecta nada.\n"
        "rt_tileimage:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #-16]!\n"
        "    stp x21, x22, [sp, #-16]!\n"
        "    stp x23, x24, [sp, #-16]!\n"
        "    stp x25, x26, [sp, #-16]!\n"
        "    stp x27, x28, [sp, #-16]!\n"
        "    mov x19, x0\n" // handle
        "    mov x20, x1\n"  // x (temporal, se convierte en start_x)
        "    mov x21, x2\n"   // y (temporal, se convierte en start_y)
        "    lsl x28, x3, #31\n" // bit solido ya desplazado -- lo usamos tal cual como a3 en cada dibujado (frame siempre 0 al tilear)
        "    mov x0, x19\n"
        "    mov x8, #51\n" // SYS_IMAGE_SIZE
        "    svc #0\n"
        "    lsr x22, x0, #32\n" // img_w
        "    and x23, x0, #0xFFFFFFFF\n" // img_h
        "    cbz x22, 276f\n"
        "    cbz x23, 276f\n"
        "    mov x8, #33\n" // SYS_GET_WINDOW_SIZE
        "    svc #0\n"
        "    lsr x24, x0, #32\n" // win_w
        "    and x25, x0, #0xFFFFFFFF\n" // win_h
        "    sdiv x9, x20, x22\n"
        "    mul x10, x9, x22\n"
        "    sub x9, x20, x10\n" // fase_x (con signo)
        "    cmp x9, #0\n"
        "    bge 270f\n"
        "    add x9, x9, x22\n" // normalizamos a [0,img_w)
        "270:\n"
        "    sdiv x10, x21, x23\n"
        "    mul x11, x10, x23\n"
        "    sub x10, x21, x11\n" // fase_y (con signo)
        "    cmp x10, #0\n"
        "    bge 271f\n"
        "    add x10, x10, x23\n"
        "271:\n"
        "    neg x20, x9\n"  // start_x
        "    neg x21, x10\n"  // start_y
        "    mov x26, x21\n"   // ty = start_y
        "272:\n"
        "    cmp x26, x25\n"
        "    bge 275f\n"
        "    mov x27, x20\n" // tx = start_x
        "273:\n"
        "    cmp x27, x24\n"
        "    bge 274f\n"
        "    mov x0, x19\n"
        "    mov x1, x27\n"
        "    mov x2, x26\n"
        "    mov x3, x28\n" // frame=0 | bit_solido (TileImage no anima, solo importa transparencia)
        "    mov x8, #50\n" // SYS_DRAW_IMAGE
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
        "// rt_currentdate: \"DD-MM-YYYY\", a partir de SYS_RTC_CIVIL.\n"
        "rt_currentdate:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x8, #134\n" // SYS_RTC_CIVIL
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
        // dia (bits 32-39)
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
        "    mov w0, #45\n" // '-'
        "    strb w0, [x12, #2]\n"
        // mes (bits 40-47)
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
        // año (bits 48-63)
        "    lsr x0, x9, #48\n"
        "    and x0, x0, #0xFFFF\n"
        "    mov x13, #1000\n"
        "    udiv x14, x0, x13\n"
        "    mul x15, x14, x13\n"
        "    sub x0, x0, x15\n"
        "    add x14, x14, #48\n" // millares
        "    mov x13, #100\n"
        "    udiv x9, x0, x13\n" // reutilizamos x9, el valor empaquetado original ya no hace falta
        "    mul x15, x9, x13\n"
        "    sub x0, x0, x15\n"
        "    add x9, x9, #48\n" // centenas
        "    mov x13, #10\n"
        "    udiv x16, x0, x13\n"
        "    mul x15, x16, x13\n"
        "    sub x0, x0, x15\n"
        "    add x16, x16, #48\n" // decenas
        "    add x0, x0, #48\n"    // unidades
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
        "// rt_currenttime: \"HH:MM:SS\", a partir de SYS_RTC_CIVIL.\n"
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
        // hora (bits 24-31)
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
        "    mov w0, #58\n" // ':'
        "    strb w0, [x12, #2]\n"
        // minuto (bits 16-23)
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
        // segundo (bits 8-15)
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
        "// rt_commandline: reutiliza SYS_GET_LAUNCH_ARG, el mismo dato\n"
        "// que ya usa GetLaunchArg internamente.\n"
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
        "    mov x8, #6\n" // SYS_GET_LAUNCH_ARG
        "    svc #0\n"
        "    mov x0, x11\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_waittimer: x0=handle -- bombea el sistema hasta que toque\n"
        "// el siguiente disparo del temporizador, y lo consume.\n"
        "rt_waittimer:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #-16]!\n"
        "    mov x19, x0\n"
        "280:\n"
        "    mov x8, #14\n" // SYS_PUMP
        "    svc #0\n"
        "    mov x0, x19\n"
        "    mov x8, #132\n" // SYS_TIMER_READY
        "    svc #0\n"
        "    cbz x0, 280b\n"
        "    mov x0, x19\n"
        "    mov x8, #133\n" // SYS_TIMER_CONSUME
        "    svc #0\n"
        "    ldp x19, x20, [sp], #16\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_gosub_return: saca la ultima direccion de retorno de\n"
        "// nuestra pila software (rt_gosub_stack), y salta alli --\n"
        "// separada de la pila de llamadas normal (x30/bl/ret), que un\n"
        "// Gosub anidado sobreescribiria sin darnos cuenta.\n"
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
        "// rt_changedir: x0=nombre de la carpeta (o \"..\" para volver a\n"
        "// la raiz) -- busca esa carpeta DENTRO de la actual y, si\n"
        "// existe, actualiza rt_current_dir_inode/name. Devuelve 0 si\n"
        "// ok, -1 si no se encontro.\n"
        "rt_changedir:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #-16]!\n"
        "    mov x19, x0\n"
        "    ldrb w10, [x19]\n"
        "    cmp w10, #46\n" // '.'
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
        "    mov x8, #83\n" // SYS_FIND_CHILD
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
        "// rt_currentdir: construye \"/\" + el nombre de la carpeta\n"
        "// actual (o solo \"/\" si estamos en la raiz).\n"
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
        "    mov w12, #47\n" // '/'
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
        "// rt_readdir: x0=nombre (vacio = carpeta actual) -- resuelve a\n"
        "// un inodo y abre su iterador (SYS_DIR_OPEN).\n"
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
        "    mov x8, #78\n" // SYS_DIR_OPEN
        "    svc #0\n"
        "243:\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_nextfile: x0=handle de directorio -- siguiente nombre, o\n"
        "// cadena vacia si ya no quedan mas.\n"
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
        "    mov x8, #79\n" // SYS_DIR_NEXT
        "    svc #0\n"
        "    mov x0, x12\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_writeline: x0=handle (ya con el offset +100), x1=cadena\n"
        "// -- escribe sus bytes uno a uno, y un salto de linea al final.\n"
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
        "    mov x8, #72\n" // SYS_GENFILE_WRITE_BYTES
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
        "// rt_writestring: x0=handle (+100), x1=cadena -- escribe su\n"
        "// longitud como entero de 4 bytes, seguida de sus bytes (sin\n"
        "// terminador nulo), el formato clasico de cadena binaria.\n"
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
        "// rt_readstring: x0=handle (+100) -- lee una cadena escrita\n"
        "// por WriteString$ (longitud de 4 bytes + esos bytes).\n"
        "rt_readstring:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #-16]!\n"
        "    stp x21, x22, [sp, #-16]!\n"
        "    cmp x0, #100\n"
        "    blt 290f\n"
        "    sub x19, x0, #100\n"
        "    mov x21, #71\n" // SYS_GENFILE_READ_BYTES
        "    b 291f\n"
        "290:\n"
        "    mov x19, x0\n"
        "    mov x21, #136\n" // SYS_READ_FILE_READ_BYTES
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
        "// rt_copyfile: x0=nombre origen, x1=nombre destino -- copia el\n"
        "// contenido entero, en bloques de 1024 bytes.\n"
        "rt_copyfile:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #-16]!\n"
        "    mov x19, x0\n"
        "    mov x20, x1\n"
        "    mov x0, x19\n"
        "    mov x1, #0\n"
        "    mov x8, #70\n" // SYS_GENFILE_OPEN (lectura)
        "    svc #0\n"
        "    cmp x0, #0\n"
        "    blt 262f\n"
        "    mov x19, x0\n"
        "    mov x0, x20\n"
        "    mov x1, #1\n"
        "    mov x8, #70\n" // SYS_GENFILE_OPEN (escritura)
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
        "    mov x8, #71\n" // SYS_GENFILE_READ_BYTES
        "    svc #0\n"
        "    cbz x0, 261f\n"
        "    mov x2, x0\n"
        "    mov x0, x20\n"
        "    sub x0, x0, #100\n"
        "    mov x1, x9\n"
        "    mov x8, #72\n" // SYS_GENFILE_WRITE_BYTES
        "    svc #0\n"
        "    b 260b\n"
        "261:\n"
        "    mov x0, x20\n"
        "    sub x0, x0, #100\n"
        "    mov x8, #77\n" // SYS_GENFILE_CLOSE
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
        "// rt_exp: x0=bits de x -- e^x. Reduccion de rango: x=n*ln2+r con\n"
        "// r pequeño, e^x = e^r * 2^n. e^r via Horner (10 terminos), y\n"
        "// 2^n se construye manipulando directamente el campo de\n"
        "// exponente IEEE-754 (exacto, sin bucles ni multiplicaciones\n"
        "// repetidas).\n"
        "rt_exp:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    fmov d0, x0\n"
        "    adrp x9, rt_const_ln2\n    add x9, x9, :lo12:rt_const_ln2\n    ldr x9, [x9]\n    fmov d1, x9\n"
        "    fdiv d2, d0, d1\n"
        "    fcvtzs x10, d2\n"
        "    scvtf d3, x10\n"
        "    fmul d4, d3, d1\n"
        "    fsub d5, d0, d4\n" // r = x - n*ln2
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
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n" // d6 = e^r
        "    add x11, x10, #1023\n"
        "    lsl x11, x11, #52\n"
        "    fmov d2, x11\n" // d2 = 2^n (reutilizamos, ya no hace falta su valor anterior)
        "    fmul d0, d6, d2\n"
        "    fmov x0, d0\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_log: x0=bits de x (x>0) -- ln(x). Separamos x=m*2^e con m\n"
        "// en [1,2) leyendo el campo de exponente IEEE-754 directamente\n"
        "// de los bits, y calculamos ln(m) via la identidad\n"
        "// ln(m)=2*atanh((m-1)/(m+1)) (converge mucho mas rapido que una\n"
        "// serie de Taylor directa, ya que (m-1)/(m+1) esta siempre en\n"
        "// [0,1/3) para m en [1,2)). El resultado final es ln(m)+e*ln2.\n"
        "rt_log:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    mov x9, x0\n"
        "    lsr x10, x9, #52\n"
        "    and x10, x10, #0x7FF\n"
        "    sub x11, x10, #1023\n" // e
        "    mov x12, #1023\n"
        "    lsl x12, x12, #52\n"
        "    mov x13, #0x7FF\n"
        "    lsl x13, x13, #52\n"
        "    mov x16, #0\n"
        "    sub x16, x16, #1\n" // x16 = -1 (todos los bits a 1) -- evitamos 'mov' con inmediato negativo directo
        "    eor x13, x13, x16\n"  // x13 = NOT(0x7FF<<52) -- 'bic' no esta soportado por nuestro ensamblador
        "    and x14, x9, x13\n"
        "    orr x14, x14, x12\n"
        "    fmov d0, x14\n" // d0 = m, en [1,2)
        "    adrp x15, rt_exp_c0\n    add x15, x15, :lo12:rt_exp_c0\n    ldr x15, [x15]\n    fmov d1, x15\n" // 1.0
        "    fsub d2, d0, d1\n" // m-1
        "    fadd d3, d0, d1\n" // m+1
        "    fdiv d4, d2, d3\n" // z
        "    fmul d5, d4, d4\n"  // z^2
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
        "    fmul d6, d6, d5\n    fadd d6, d6, d7\n" // d6 = suma de atanh
        "    fmul d6, d6, d4\n" // *z
        "    fadd d6, d6, d6\n" // *2 -> ln(m)
        "    scvtf d2, x11\n" // e como double
        "    adrp x15, rt_const_ln2\n    add x15, x15, :lo12:rt_const_ln2\n    ldr x15, [x15]\n    fmov d3, x15\n"
        "    fmul d2, d2, d3\n" // e*ln2
        "    fadd d0, d6, d2\n"
        "    fmov x0, d0\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_log10: log en base 10 -- reutiliza rt_log entero,\n"
        "// multiplicando el resultado por 1/ln(10).\n"
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
        "// rt_atan_core: x0=bits de x, con |x|<=1 -- devuelve atan(x) en\n"
        "// RADIANES (bits), via la identidad de medio-angulo (encoge el\n"
        "// rango antes de aplicar la serie de Taylor, asi converge con\n"
        "// pocos terminos incluso cerca de |x|=1) mas una serie de Horner\n"
        "// de 6 terminos. No hace ninguna llamada interna -- no necesita\n"
        "// guardar x30.\n"
        "rt_atan_core:\n"
        "    fmov d0, x0\n"
        "    fmul d1, d0, d0\n"
        "    adrp x9, rt_atan_c0\n    add x9, x9, :lo12:rt_atan_c0\n    ldr x9, [x9]\n    fmov d2, x9\n" // 1.0
        "    fadd d1, d1, d2\n"        // 1+x^2
        "    fsqrt d1, d1\n"            // sqrt(1+x^2)
        "    fadd d1, d1, d2\n"          // 1+sqrt(1+x^2)
        "    fdiv d5, d0, d1\n"           // xr = x/(1+sqrt(1+x^2))
        "    fmul d6, d5, d5\n"            // xr^2
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
        "    fmul d0, d7, d5\n"      // acumulado * xr
        "    fadd d0, d0, d0\n"       // *2, por la reduccion de medio-angulo
        "    fmov x0, d0\n"
        "    ret\n"
        "\n"
        "// rt_atan: x0=bits de x (la razon, SIN unidades) -- devuelve\n"
        "// atan(x) en GRADOS. Para |x|>1 usa atan(x)=signo(x)*90-atan(1/x)\n"
        "// para reducir siempre al caso |x|<=1 antes de rt_atan_core.\n"
        "rt_atan:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    fmov d0, x0\n"
        "    fabs d1, d0\n"
        "    adrp x9, rt_atan_c0\n    add x9, x9, :lo12:rt_atan_c0\n    ldr x9, [x9]\n    fmov d2, x9\n"
        "    fcmp d1, d2\n"
        "    ble 90f\n"
        "    fmov x10, d0\n"
        "    lsr x11, x10, #63\n"        // signo de x (bit 63 de su patron de bits)
        "    adrp x9, rt_const_pi\n    add x9, x9, :lo12:rt_const_pi\n    ldr x9, [x9]\n    fmov d8, x9\n"
        "    adrp x9, rt_half\n    add x9, x9, :lo12:rt_half\n    ldr x9, [x9]\n    fmov d4, x9\n"
        "    fmul d8, d8, d4\n"           // d8 = pi/2, PRESERVADO a traves del bl (d8-d15 lo son en la ABI)
        "    cbz x11, 91f\n"
        "    fneg d8, d8\n"
        "91:\n"
        "    fdiv d0, d2, d0\n"            // xr = 1/x
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
        "// rt_asin: x0=bits de x, en [-1,1] -- asin(x)=atan(x/sqrt(1-x^2)),\n"
        "// asi reutilizamos rt_atan entero sin escribir otra serie mas.\n"
        "rt_asin:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    fmov d0, x0\n"
        "    fmul d1, d0, d0\n"
        "    adrp x9, rt_atan_c0\n    add x9, x9, :lo12:rt_atan_c0\n    ldr x9, [x9]\n    fmov d2, x9\n"
        "    fsub d1, d2, d1\n"            // 1-x^2
        "    fsqrt d1, d1\n"
        "    fdiv d0, d0, d1\n"
        "    fmov x0, d0\n"
        "    bl rt_atan\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_acos: acos(x) = 90 - asin(x)\n"
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
        "// rt_atan2: x0=bits de y, x1=bits de x -- arcotangente de dos\n"
        "// argumentos, con correccion de cuadrante. d10/d11 (preservados\n"
        "// por la ABI) guardan y/x a traves de los 'bl' internos.\n"
        "rt_atan2:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    stp d10, d11, [sp, #-16]!\n" // guardamos NOSOTROS d10/d11 -- son preservados para QUIEN NOS llama, pero los vamos a usar
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
        "// rt_wait_event: x0=timeout en milisegundos, o -1 para esperar\n"
        "// sin limite. Bombea el resto del sistema (SYS_PUMP) y consulta\n"
        "// el evento pendiente (SYS_POLL_EVENT) en bucle hasta que llegue\n"
        "// uno, o hasta que expire el timeout (en ese caso devuelve 0).\n"
        "// El id devuelto se cachea en rt_last_event_id para EventID().\n"
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
        "// rt_poll_event: igual que rt_wait_event, pero UNA sola consulta\n"
        "// (sin esperar) -- version 'no bloqueante' para PollEvent().\n"
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
        "// rt_client_width / rt_client_height: x0=handle de ventana (el\n"
        "// centinela 0x7FFFFFFF que devuelve Desktop() pide las medidas\n"
        "// de TODA la pantalla en vez de las de la ventana propia).\n"
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
        "// rt_notify: x0=puntero al mensaje. Aproximacion sencilla de\n"
        "// Notify() -- dibuja una caja centrada con el texto y espera\n"
        "// 1.5 segundos, sin necesitar ningun dialogo modal nuevo en el\n"
        "// kernel (solo syscalls que ya existian).\n"
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
        "// rt_confirm: x0=mensaje -- dialogo modal DE VERDAD (a\n"
        "// diferencia de Notify): crea Panel+Label+2 Botones como\n"
        "// gadgets normales, y bucle-espera (bombeando el planificador\n"
        "// con SYS_PUMP en cada vuelta, igual que WaitEvent) hasta que\n"
        "// se pulse uno de los dos. Los libera todos al terminar.\n"
        "// Registros preservados: x19=mensaje/resultado, x20=x del\n"
        "// panel, x21=y del panel, x22=id panel, x23=id label,\n"
        "// x24=id boton Si, x25=id boton No.\n"
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
        "// rt_instr: posicion (base 1) de la primera aparicion de x1\n"
        "// dentro de x0, o 0 si no aparece.\n"
        "// rt_instr: x0=cadena, x1=buscada, x2=offset (base 1, desde\n"
        "// donde empezar a buscar) -- avanza byte a byte hasta el\n"
        "// offset pedido (parando si la cadena se acaba antes, para no\n"
        "// leer fuera de ella si offset es mayor que su longitud).\n"
        "rt_instr:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    cmp x2, #1\n"
        "    bge 57f\n"
        "    mov x2, #1\n" // offset<1 -- lo tratamos como 1
        "57:\n"
        "    mov x9, x0\n"     // haystack, arranca al principio
        "    mov x10, #1\n"    // posicion actual (base 1)
        "58:\n"
        "    cmp x10, x2\n"
        "    bge 52f\n"        // ya llegamos al offset pedido
        "    ldrb w16, [x9]\n"
        "    cbz w16, 55f\n"   // la cadena se acabo antes de llegar al offset -- sin coincidencia posible
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
        "    cbz w14, 54f\n"    // se acabo 'buscada' -> coincidencia completa
        "    ldrb w15, [x12]\n"
        "    cbz w15, 55f\n"    // se acabo 'cadena' antes -- ya no habra match
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
        "// rt_replace: x0=cadena, x1=buscada, x2=sustituta -- devuelve\n"
        "// una cadena nueva con TODAS las apariciones sustituidas.\n"
        "rt_replace:\n"
        "    stp x29, x30, [sp, #-48]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    stp x21, x22, [sp, #32]\n"
        "    mov x19, x0\n"     // cadena, avanza
        "    mov x20, x1\n"     // buscada
        "    mov x21, x2\n"     // sustituta
        "    adrp x9, rt_str_pos\n"
        "    add x9, x9, :lo12:rt_str_pos\n"
        "    ldr x10, [x9]\n"
        "    adrp x11, rt_str_pool\n"
        "    add x11, x11, :lo12:rt_str_pool\n"
        "    add x11, x11, x10\n"
        "    add x10, x10, #128\n"
        "    and x10, x10, #1023\n"
        "    str x10, [x9]\n"
        "    mov x22, x11\n"    // destino, avanza
        "60:\n"
        "    ldrb w12, [x19]\n"
        "    cbz w12, 65f\n"
        "    mov x13, x19\n"
        "    mov x14, x20\n"
        "61:\n"
        "    ldrb w15, [x14]\n"
        "    cbz w15, 63f\n"    // 'buscada' se acabo -> coincidencia completa
        "    ldrb w16, [x13]\n"
        "    cbz w16, 62f\n"    // 'cadena' se acabo antes -- no hay match aqui
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
        "// rt_readline: x0=handle de archivo -- pide al kernel la\n"
        "// siguiente linea directamente sobre un hueco del pool\n"
        "// rotatorio de cadenas, y devuelve ese mismo puntero.\n"
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
        "// rt_gadget_text: x0=id de gadget -- pide al kernel el texto\n"
        "// actual de ese gadget (funciona con TextField, TextArea, etc.)\n"
        "// sobre un hueco del pool rotatorio de cadenas, igual que\n"
        "// rt_readline.\n"
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
        "    mov x8, #106\n" // SYS_GADGET_GET_TEXT
        "    svc #0\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_font_name: x0=handle de fuente -- mismo patron que\n"
        "// rt_gadget_text, pero via SYS_FONT_NAME (FontName$).\n"
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
        "    mov x8, #192\n" // SYS_FONT_NAME
        "    svc #0\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_gfx_driver_name: x0=indice -- pide al kernel el nombre\n"
        "// del driver de graficos (GfxDriverName$).\n"
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
        "    mov x8, #202\n" // SYS_GFX_DRIVER_NAME
        "    svc #0\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_gadget_item_text: x0=id, x1=indice -- pide al kernel el\n"
        "// texto de un item de ListBox (GadgetItemText$).\n"
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
        "    mov x8, #119\n" // SYS_LISTBOX_ITEM_TEXT
        "    svc #0\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_textarea_text: x0=id, x1=start, x2=count -- pide al\n"
        "// kernel un trozo del texto de un TextArea (TextAreaText$)\n"
        "// sobre un hueco del pool rotatorio de cadenas.\n"
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
        "    mov x8, #149\n" // SYS_TEXTAREA_GET_TEXT
        "    svc #0\n"
        "    mov x0, x13\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// x0 los bits del angulo reducido a RADIANES, en [-pi,pi). La\n"
        "// reduccion usa el truco habitual de coma flotante: truncar\n"
        "// (shifted/360) da el 'floor' salvo cuando el resto sale\n"
        "// negativo, en cuyo caso se corrige sumando 360 una vez.\n"
        "rt_deg_reduce:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    fmov d0, x0\n"
        "    adrp x9, rt_const_180\n"
        "    add x9, x9, :lo12:rt_const_180\n"
        "    ldr x9, [x9]\n"
        "    fmov d1, x9\n"
        "    fadd d0, d0, d1\n"          // d0 = grados + 180
        "    adrp x9, rt_const_360\n"
        "    add x9, x9, :lo12:rt_const_360\n"
        "    ldr x9, [x9]\n"
        "    fmov d2, x9\n"
        "    fdiv d3, d0, d2\n"
        "    fcvtzs x10, d3\n"            // trunc(shifted/360)
        "    scvtf d4, x10\n"
        "    fmul d5, d4, d2\n"
        "    fsub d6, d0, d5\n"            // resto = shifted - n*360
        "    fmov x11, d6\n"
        "    cmp x11, #0\n"                 // el bit de signo IEEE-754 coincide con el signo en complemento a dos
        "    bge 80f\n"
        "    fadd d6, d6, d2\n"              // resto negativo -- lo corregimos sumando 360 una vez
        "80:\n"
        "    fsub d6, d6, d1\n"               // volvemos a [-180,180)
        "    adrp x9, rt_const_deg2rad\n"
        "    add x9, x9, :lo12:rt_const_deg2rad\n"
        "    ldr x9, [x9]\n"
        "    fmov d7, x9\n"
        "    fmul d0, d6, d7\n"
        "    fmov x0, d0\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "\n"
        "// rt_sin/rt_cos: x0=bits de un angulo en GRADOS -- devuelven\n"
        "// la aproximacion via serie de Taylor (7 terminos, regla de\n"
        "// Horner) tras reducir el angulo con rt_deg_reduce.\n"
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
        "    fmul d0, d2, d0\n"            // sin(x) = acumulado * x
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
        "    fmov x0, d2\n"                // cos(x) = acumulado (sin multiplicar por x)
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
    );
}

// ---- funciones definidas por el usuario ----

static void emit_function(Node *fn) {
    FuncScope sc; memset(&sc, 0, sizeof(sc));
    for (int i = 0; i < fn->list_count; i++) local_add_if_new(&sc, fn->list[i]->text);
    collect_locals_stmt(&sc, fn->d);
    sc.frame_size = ((sc.count * 8) + 15) & ~15;
    if (sc.frame_size == 0) sc.frame_size = 16;

    fprintf(out, "\nfunc_%s:\n", fn->text);
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n");
    fprintf(out, "    mov x29, sp\n");
    fprintf(out, "    sub sp, sp, #%d\n", sc.frame_size);
    for (int i = 0; i < fn->list_count && i < 8; i++) {
        fprintf(out, "    str x%d, [x29, #%d]\n", i, sc.locals[i].offset);
    }
    g_current_func_name = fn->text;
    g_in_user_function = true;
    emit_block(&sc, fn->d);
    g_in_user_function = false;
    fprintf(out, "    mov x0, #0\n");
    fprintf(out, ".Lfunc_end_%s:\n", fn->text);
    fprintf(out, "    add sp, x29, #0\n");
    fprintf(out, "    ldp x29, x30, [sp], #16\n");
    fprintf(out, "    ret\n");
}

// ---- punto de entrada del generador ----

void codegen_generate(Node *program, FILE *output) {
    out = output;

    collect_toplevel_names(program);
    mark_globals_stmt(program);
    collect_data(program);
    collect_type_info(program);

    fprintf(out, "// Generado por el compilador Nemo-Blitz -- no editar a mano.\n");
    fprintf(out, ".section .text.start\n");
    fprintf(out, ".global _start\n");
    fprintf(out, "_start:\n");
    // _start tiene que comportarse como cualquier funcion normal:
    // guardar x30 (direccion de retorno hacia quien nos lanzo) al
    // principio, y restaurarla antes de su propio 'ret'. Si no, cada
    // 'bl' que hagamos por el camino (imprimir un numero, llamar a
    // una funcion...) va sobrescribiendo x30, y al llegar al final
    // saltariamos a la ULTIMA llamada interna en vez de volver a
    // quien nos ejecuto -- el programa se queda saltando dentro de si
    // mismo para siempre, en vez de terminar.
    fprintf(out, "    stp x29, x30, [sp, #-16]!\n");
    fprintf(out, "    mov x29, sp\n");

    // Semilla inicial de Rnd() -- el reloj del sistema (SYS_GET_TICKS,
    // numero 2), para que cada ejecucion de un mismo programa no
    // repita siempre la misma secuencia "aleatoria".
    fprintf(out, "    mov x8, #2\n    svc #0\n");
    fprintf(out, "    adrp x9, rt_rnd_seed\n    add x9, x9, :lo12:rt_rnd_seed\n    str x0, [x9]\n");

    // Color activo por defecto: blanco -- Plot/Line/Rect/Oval sin
    // color explicito usaban 0xFFFFFF a fuego; ahora leen esta
    // variable, que Color() puede cambiar, pero el valor de salida es
    // el mismo si el programa nunca llama a Color().
    fprintf(out, "    mov x9, #0xFFFFFF\n");
    fprintf(out, "    adrp x10, rt_current_color\n    add x10, x10, :lo12:rt_current_color\n    str x9, [x10]\n");

    // Puntero de lectura de Data -- empieza al principio de la tabla.
    fprintf(out, "    adrp x9, rt_data_table\n    add x9, x9, :lo12:rt_data_table\n");
    fprintf(out, "    adrp x10, rt_data_ptr\n    add x10, x10, :lo12:rt_data_ptr\n    str x9, [x10]\n");

    // Las sentencias de nivel superior se ejecutan directamente aqui;
    // las N_FUNCDEF se saltan (van despues, como subrutinas aparte).
    FuncScope top; memset(&top, 0, sizeof(top));
    for (int i = 0; i < program->list_count; i++) {
        if (program->list[i]->kind != N_FUNCDEF) emit_stmt(&top, program->list[i]);
    }
    fprintf(out, ".Lprogram_end:\n");
    fprintf(out, "    ldp x29, x30, [sp], #16\n");
    fprintf(out, "    ret\n");

    for (int i = 0; i < program->list_count; i++) {
        if (program->list[i]->kind == N_FUNCDEF) emit_function(program->list[i]);
    }

    emit_runtime_helpers();

    fprintf(out, "\n.section .rodata\n");
    fprintf(out, "rt_half: .quad 0x3FE0000000000000\n"); // 0.5 en IEEE-754, para redondear rt_float_to_str

    // Constantes para Sin/Cos: reduccion de angulo (grados -> [-180,180)
    // -> radianes) y coeficientes de la serie de Taylor (7 terminos,
    // error maximo ~0.0001 en todo el rango [-pi,pi] -- de sobra para
    // graficos de juego).
    fprintf(out, "rt_const_180: .quad 4640537203540230144\n");       // 180.0
    fprintf(out, "rt_const_360: .quad 4645040803167600640\n");       // 360.0
    fprintf(out, "rt_const_deg2rad: .quad 4580687790476533049\n");   // pi/180
    fprintf(out, "rt_sin_c0: .quad 4607182418800017408\n");          // 1
    fprintf(out, "rt_sin_c1: .quad 13818544856648471893\n");         // -1/3!
    fprintf(out, "rt_sin_c2: .quad 4575957461383581969\n");          // 1/5!
    fprintf(out, "rt_sin_c3: .quad 13774824197408792602\n");         // -1/7!
    fprintf(out, "rt_sin_c4: .quad 4523617214285662004\n");          // 1/9!
    fprintf(out, "rt_sin_c5: .quad 13716528800881525988\n");         // -1/11!
    fprintf(out, "rt_sin_c6: .quad 4460272573143870729\n");          // 1/13!
    fprintf(out, "rt_cos_c0: .quad 4607182418800017408\n");          // 1
    fprintf(out, "rt_cos_c1: .quad 13826050856027422720\n");         // -1/2!
    fprintf(out, "rt_cos_c2: .quad 4586165620538955093\n");          // 1/4!
    fprintf(out, "rt_cos_c3: .quad 13787419979223755799\n");         // -1/6!
    fprintf(out, "rt_cos_c4: .quad 4537941361671905306\n");          // 1/8!
    fprintf(out, "rt_cos_c5: .quad 13732177094651715420\n");         // -1/10!
    fprintf(out, "rt_cos_c6: .quad 4477122120089393304\n");          // 1/12!
    fprintf(out, "rt_const_pi: .quad 4614256656552045848\n");
    fprintf(out, "rt_const_2_31: .quad 4746794007248502784\n");      // 2^31, para normalizar Rnd a [0,1)
    fprintf(out, "rt_const_rad2deg: .quad 4633260481411531256\n");   // 180/pi
    fprintf(out, "rt_atan_c0: .quad 4607182418800017408\n");         // 1
    fprintf(out, "rt_atan_c1: .quad 13823048456275842389\n");        // -1/3
    fprintf(out, "rt_atan_c2: .quad 4596373779694328218\n");         // 1/5
    fprintf(out, "rt_atan_c3: .quad 13817687028148020370\n");        // -1/7
    fprintf(out, "rt_atan_c4: .quad 4592670820000712476\n");         // 1/9
    fprintf(out, "rt_atan_c5: .quad 13814587147885025094\n");        // -1/11
    fprintf(out, "rt_const_ln2: .quad 4604418534313441775\n");       // ln(2)
    fprintf(out, "rt_const_log10_recip: .quad 4601495173785380109\n"); // 1/ln(10)
    fprintf(out, "rt_exp_c0: .quad 4607182418800017408\n");          // 1/0!
    fprintf(out, "rt_exp_c1: .quad 4607182418800017408\n");          // 1/1!
    fprintf(out, "rt_exp_c2: .quad 4602678819172646912\n");          // 1/2!
    fprintf(out, "rt_exp_c3: .quad 4595172819793696085\n");          // 1/3!
    fprintf(out, "rt_exp_c4: .quad 4586165620538955093\n");          // 1/4!
    fprintf(out, "rt_exp_c5: .quad 4575957461383581969\n");          // 1/5!
    fprintf(out, "rt_exp_c6: .quad 4564047942368979991\n");          // 1/6!
    fprintf(out, "rt_exp_c7: .quad 4551452160554016794\n");          // 1/7!
    fprintf(out, "rt_exp_c8: .quad 4537941361671905306\n");          // 1/8!
    fprintf(out, "rt_exp_c9: .quad 4523617214285662004\n");          // 1/9!
    fprintf(out, "rt_log_c1: .quad 4607182418800017408\n");          // 1/1
    fprintf(out, "rt_log_c2: .quad 4599676419421066581\n");          // 1/3
    fprintf(out, "rt_log_c3: .quad 4596373779694328218\n");          // 1/5
    fprintf(out, "rt_log_c4: .quad 4594314991293244562\n");          // 1/7
    fprintf(out, "rt_log_c5: .quad 4592670820000712476\n");          // 1/9
    fprintf(out, "rt_log_c6: .quad 4591215111030249286\n");          // 1/11
    // Cadenas fijas para los botones de los dialogos modales
    // (Confirm/Proceed) -- no dependen de nada que el programa
    // escriba, asi que van como etiquetas propias, no por el pool de
    // literales del programa.
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
        emit_float_const(out, float_consts[i].label, float_consts[i].value);
    }

    // Tabla de Data -- las etiquetas (".nombre") se emiten como
    // etiquetas de ensamblador REALES, justo antes del primer valor
    // que les sigue -- asi "Restore nombre" es simplemente cargar esa
    // direccion, sin necesitar una tabla de indices aparte.
    fprintf(out, "\nrt_data_table:\n");
    for (int i = 0; i < data_entry_count; i++) {
        DataEntry *e = &data_entries[i];
        if (e->is_label) {
            fprintf(out, "dl_%s:\n", e->label_name);
        } else if (e->is_string) {
            fprintf(out, "    .quad %s\n", e->str_label);
        } else {
            fprintf(out, "    .quad %lld\n", (long long)e->num_value);
        }
    }
    fprintf(out, "rt_data_end:\n");

    fprintf(out, "\n.section .bss\n.align 3\n");
    for (int i = 0; i < global_count; i++) {
        char sym[80];
        sanitize_sym(globals[i], sym, sizeof(sym));
        fprintf(out, "var_%s: .space 8\n", sym);
    }
    for (int i = 0; i < array_count; i++) {
        fprintf(out, "arr_%s: .space %d\n", arrays[i].name, arrays[i].total_size * 8);
    }
    for (int i = 0; i < type_count; i++) {
        int inst_size = (1 + types[i].field_count) * 8;
        fprintf(out, "type_%s_pool: .space %d\n", types[i].name, inst_size * MAX_TYPE_INSTANCES);
        fprintf(out, "type_%s_head: .space 8\n", types[i].name);
        fprintf(out, "type_%s_tail: .space 8\n", types[i].name);
        fprintf(out, "type_%s_next_idx: .space 8\n", types[i].name);
    }
    fprintf(out, "rt_str_pool: .space 1024\n");
    fprintf(out, "rt_str_pos: .space 8\n");
    fprintf(out, "rt_rnd_seed: .space 8\n");
    fprintf(out, "rt_locate_buf: .space 8\n"); // marcador + x+1 + y+1 + terminador, para Locate
    fprintf(out, "rt_cls_color: .space 8\n"); // negro por defecto (el cero de .bss), separado de rt_current_color
    fprintf(out, "rt_input_echo_buf: .space 8\n"); // caracter + terminador, para el eco de Input$
    fprintf(out, "rt_current_dir_inode: .space 8\n"); // 0 = raiz, para ChangeDir/CurrentDir$/ReadDir/CreateDir
    fprintf(out, "rt_current_dir_name: .space 32\n"); // vacio si estamos en la raiz
    fprintf(out, "rt_file_io_buf: .space 8\n"); // hasta 4 bytes, para ReadByte/Short/Int/Float y sus Write
    fprintf(out, "rt_copyfile_buf: .space 1024\n"); // bloque de trabajo para CopyFile
    fprintf(out, "rt_gosub_stack: .space 128\n"); // hasta 16 niveles de Gosub anidados (8 bytes cada uno)
    fprintf(out, "rt_gosub_sp: .space 8\n"); // indice actual en la pila de Gosub (0 = vacia, via el cero de .bss)
    fprintf(out, "rt_last_event_id: .space 8\n");
    fprintf(out, "rt_data_ptr: .space 8\n");
    fprintf(out, "rt_current_color: .space 8\n");
    fprintf(out, "rt_requested_r: .space 8\n");
    fprintf(out, "rt_requested_g: .space 8\n");
    fprintf(out, "rt_requested_b: .space 8\n");
    fprintf(out, "rt_dir_listing_buf: .space 1280\n"); // 32 entradas * 40 bytes
    fprintf(out, "rt_dir_item_buf: .space 40\n"); // "[D] " + hasta 27 caracteres + terminador, con margen
    fprintf(out, "rt_requested_dir_inode: .space 8\n");
}
