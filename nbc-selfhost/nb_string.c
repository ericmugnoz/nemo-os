// nb_string.c — cadenas del runtime de Nemo-Blitz 2.0, con conteo de
// referencias sobre el montón privado de nb_alloc.c.
//
// Esto es el arreglo de fondo de H15 (el hallazgo con más impacto
// práctico de toda la auditoría del compilador viejo): un pool
// circular de 8 celdas de 128 bytes, compartido por toda función que
// producía una cadena nueva, sin comprobar el tamaño del resultado
// (desbordaba la celda) ni si seguía en uso (la reciclaba encima de
// una variable viva). Aquí cada cadena es su propia reserva de
// memoria, del tamaño exacto que necesita, y solo desaparece cuando
// de verdad ya no la usa nadie -- nunca antes, sea cual sea la
// longitud o cuántas otras cadenas se hayan calculado mientras tanto.
//
// Convención de propiedad (para el generador de código, más
// adelante): toda función de este archivo que CREA una cadena nueva
// (nb_string_new, nb_string_concat, nb_string_left, ...) la devuelve
// con refcount=1 -- el llamador es dueño de esa referencia. Asignar
// esa cadena recién creada a una variable NO necesita otro retain (ya
// se es dueño); asignar una cadena YA EXISTENTE a una segunda
// variable (B$ = A$) sí necesita nb_string_retain. Reasignar una
// variable que ya apuntaba a una cadena, o que sale de ámbito, debe
// llamar a nb_string_release sobre el valor antiguo antes de olvidarlo.
//
// Sin libc, mismo entorno que nb_alloc.c.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

extern void *nb_alloc(uint32_t n);
extern void nb_free(void *p);

typedef struct {
    uint32_t len;        // longitud en bytes, SIN contar el terminador
    uint32_t refcount;   // numero de variables/temporales que la usan
    // los 'len' bytes de la cadena, mas un terminador nulo, siguen
    // inmediatamente despues de esta cabecera en memoria.
} nb_string_t;

static inline char *nb_string_data(nb_string_t *s) {
    return (char *)(s + 1);
}

// Cadena vacía compartida, para no reservar memoria por cada "" que
// aparezca en un programa. OJO: es INMUTABLE y su refcount no se
// toca nunca (nb_string_retain/release la reconocen por puntero y no
// hacen nada) -- ver la nota en esas dos funciones.
static nb_string_t nb_empty_string_storage = { 0, 0 };
static char nb_empty_string_byte = 0;
#define NB_EMPTY (&nb_empty_string_storage)

nb_string_t *nb_string_new(const char *data, uint32_t len) {
    if (len == 0) return NB_EMPTY;
    nb_string_t *s = (nb_string_t *)nb_alloc((uint32_t)sizeof(nb_string_t) + len + 1);
    if (!s) return NULL; // memoria agotada -- ver la nota grande al final del archivo
    s->len = len;
    s->refcount = 1;
    char *d = nb_string_data(s);
    for (uint32_t i = 0; i < len; i++) d[i] = data[i];
    d[len] = 0;
    return s;
}

// Para construir una cadena de longitud conocida directamente en su
// sitio (evita reservar un búfer aparte y copiarlo después) -- el
// llamador escribe en el puntero devuelto por nb_string_data_mut y
// debe rellenar exactamente 'len' bytes antes de usar la cadena.
nb_string_t *nb_string_new_uninit(uint32_t len, char **out_data) {
    if (len == 0) { *out_data = &nb_empty_string_byte; return NB_EMPTY; }
    nb_string_t *s = (nb_string_t *)nb_alloc((uint32_t)sizeof(nb_string_t) + len + 1);
    if (!s) { *out_data = NULL; return NULL; }
    s->len = len;
    s->refcount = 1;
    char *d = nb_string_data(s);
    d[len] = 0;
    *out_data = d;
    return s;
}

void nb_string_retain(nb_string_t *s) {
    if (s == NULL || s == NB_EMPTY) return;
    s->refcount++;
}

void nb_string_release(nb_string_t *s) {
    if (s == NULL || s == NB_EMPTY) return;
    if (s->refcount == 0) return; // no deberia pasar nunca; defensivo
    s->refcount--;
    if (s->refcount == 0) nb_free(s);
}

uint32_t nb_string_len(nb_string_t *s) {
    return s ? s->len : 0;
}

const char *nb_string_cstr(nb_string_t *s) {
    return s ? nb_string_data(s) : &nb_empty_string_byte;
}

nb_string_t *nb_string_concat(nb_string_t *a, nb_string_t *b) {
    uint32_t la = nb_string_len(a), lb = nb_string_len(b);
    char *d; nb_string_t *r = nb_string_new_uninit(la + lb, &d);
    if (!r) return NULL;
    const char *da = nb_string_cstr(a), *db = nb_string_cstr(b);
    for (uint32_t i = 0; i < la; i++) d[i] = da[i];
    for (uint32_t i = 0; i < lb; i++) d[la + i] = db[i];
    return r;
}

nb_string_t *nb_string_left(nb_string_t *s, uint32_t n) {
    uint32_t len = nb_string_len(s);
    if (n > len) n = len;
    return nb_string_new(nb_string_cstr(s), n);
}

nb_string_t *nb_string_right(nb_string_t *s, uint32_t n) {
    uint32_t len = nb_string_len(s);
    if (n > len) n = len;
    return nb_string_new(nb_string_cstr(s) + (len - n), n);
}

// start es 1-based (convencion BASIC: Mid$(cad,1,...) empieza en el
// primer caracter), igual que en el compilador viejo.
nb_string_t *nb_string_mid(nb_string_t *s, uint32_t start, uint32_t count) {
    uint32_t len = nb_string_len(s);
    if (start < 1) start = 1;
    if (start > len) return NB_EMPTY;
    uint32_t avail = len - (start - 1);
    if (count > avail) count = avail;
    return nb_string_new(nb_string_cstr(s) + (start - 1), count);
}

bool nb_string_eq(nb_string_t *a, nb_string_t *b) {
    uint32_t la = nb_string_len(a), lb = nb_string_len(b);
    if (la != lb) return false;
    const char *da = nb_string_cstr(a), *db = nb_string_cstr(b);
    for (uint32_t i = 0; i < la; i++) if (da[i] != db[i]) return false;
    return true;
}

// Comparacion de orden (para <, >, <=, >=) -- orden byte a byte, como
// strcmp. Devuelve <0, 0, >0.
int32_t nb_string_cmp(nb_string_t *a, nb_string_t *b) {
    uint32_t la = nb_string_len(a), lb = nb_string_len(b);
    const char *da = nb_string_cstr(a), *db = nb_string_cstr(b);
    uint32_t n = la < lb ? la : lb;
    for (uint32_t i = 0; i < n; i++) {
        if ((uint8_t)da[i] != (uint8_t)db[i]) return (int32_t)(uint8_t)da[i] - (int32_t)(uint8_t)db[i];
    }
    return (int32_t)la - (int32_t)lb;
}

// Nota sobre memoria agotada: por ahora, cualquier funcion de este
// archivo devuelve NULL si nb_alloc no tiene sitio, y nb_string_len/
// nb_string_cstr tratan NULL igual que la cadena vacia -- un programa
// que se quede sin memoria para cadenas ve resultados vacios en vez
// de reventar, pero no hay ningun aviso de que eso ha pasado. Esto es
// una solucion PROVISIONAL: cuando se diseñe el manejo de errores
// real (esteroide #1 del documento de diseño), quedarse sin memoria
// al crear una cadena deberia ser un error capturable (Try/Catch), no
// un vaciado silencioso. Anotado aqui para no olvidarlo cuando llegue
// ese momento.

// ---------------------------------------------------------------------
// Busqueda y transformacion de cadenas
// ---------------------------------------------------------------------
// Todas siguen la misma regla que las de arriba: NUNCA modifican la
// cadena que reciben (una cadena puede estar compartida por varias
// variables gracias al conteo de referencias). Las que transforman
// crean una cadena NUEVA.

// Instr(cadena$, buscar$ [, desde]) -- posicion de la primera
// aparicion, empezando a contar en 1, o 0 si no aparece. 'desde'
// tambien empieza en 1, como en BlitzPlus.
//
// Busqueda directa, sin indices ni tablas: para las cadenas de un
// programa BASIC (decenas o cientos de caracteres) es mas rapido que
// construir cualquier estructura auxiliar.
int64_t nb_string_instr(nb_string_t *s, nb_string_t *buscar, int64_t desde) {
    if (!s || !buscar) return 0;
    uint32_t ls = nb_string_len(s), lb = nb_string_len(buscar);
    if (lb == 0) return 1;      // la cadena vacia esta en todas partes
    if (lb > ls) return 0;

    uint32_t inicio = (desde > 1) ? (uint32_t)(desde - 1) : 0;
    if (inicio >= ls) return 0;

    const char *a = nb_string_cstr(s);
    const char *b = nb_string_cstr(buscar);
    for (uint32_t i = inicio; i + lb <= ls; i++) {
        uint32_t j = 0;
        while (j < lb && a[i + j] == b[j]) j++;
        if (j == lb) return (int64_t)(i + 1);   // 1-indexado
    }
    return 0;
}

// Upper$ / Lower$ -- solo ASCII a-z / A-Z. Las vocales acentuadas y la
// enye no se tocan: en UTF-8 ocupan dos bytes y cambiarlos de caja
// requeriria una tabla de mapeo que no compensa aqui.
nb_string_t *nb_string_upper(nb_string_t *s) {
    if (!s) return 0;
    uint32_t n = nb_string_len(s);
    char *d;
    nb_string_t *r = nb_string_new_uninit(n, &d);
    if (!r) return 0;
    const char *src = nb_string_cstr(s);
    for (uint32_t i = 0; i < n; i++) {
        char c = src[i];
        d[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    return r;
}

nb_string_t *nb_string_lower(nb_string_t *s) {
    if (!s) return 0;
    uint32_t n = nb_string_len(s);
    char *d;
    nb_string_t *r = nb_string_new_uninit(n, &d);
    if (!r) return 0;
    const char *src = nb_string_cstr(s);
    for (uint32_t i = 0; i < n; i++) {
        char c = src[i];
        d[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    return r;
}

// Trim$ -- quita espacios, tabuladores y saltos de linea de los DOS
// extremos. Los de dentro se quedan.
nb_string_t *nb_string_trim(nb_string_t *s) {
    if (!s) return 0;
    uint32_t n = nb_string_len(s);
    const char *src = nb_string_cstr(s);
    uint32_t ini = 0, fin = n;
    while (ini < fin && (src[ini] == ' ' || src[ini] == '\t' ||
                         src[ini] == '\n' || src[ini] == '\r')) ini++;
    while (fin > ini && (src[fin-1] == ' ' || src[fin-1] == '\t' ||
                         src[fin-1] == '\n' || src[fin-1] == '\r')) fin--;
    return nb_string_new(src + ini, fin - ini);
}

// Replace$(cadena$, de$, a$) -- sustituye TODAS las apariciones.
//
// Dos pasadas: la primera cuenta cuantas hay para saber el tamano
// exacto del resultado, la segunda copia. Asi se reserva memoria una
// sola vez, en vez de ir ampliando la cadena a cada sustitucion.
nb_string_t *nb_string_replace(nb_string_t *s, nb_string_t *de, nb_string_t *a) {
    if (!s || !de || !a) return 0;
    uint32_t ls = nb_string_len(s), ld = nb_string_len(de), la = nb_string_len(a);
    if (ld == 0 || ld > ls) return nb_string_new(nb_string_cstr(s), ls);

    const char *src = nb_string_cstr(s);
    const char *pd  = nb_string_cstr(de);
    const char *pa  = nb_string_cstr(a);

    // Primera pasada: contar
    uint32_t veces = 0;
    for (uint32_t i = 0; i + ld <= ls; ) {
        uint32_t j = 0;
        while (j < ld && src[i + j] == pd[j]) j++;
        if (j == ld) { veces++; i += ld; } else { i++; }
    }
    if (veces == 0) return nb_string_new(src, ls);

    // Segunda pasada: copiar
    uint32_t nueva_len = ls - veces * ld + veces * la;
    char *d;
    nb_string_t *r = nb_string_new_uninit(nueva_len, &d);
    if (!r) return 0;
    uint32_t w = 0;
    for (uint32_t i = 0; i < ls; ) {
        if (i + ld <= ls) {
            uint32_t j = 0;
            while (j < ld && src[i + j] == pd[j]) j++;
            if (j == ld) {
                for (uint32_t k = 0; k < la; k++) d[w++] = pa[k];
                i += ld;
                continue;
            }
        }
        d[w++] = src[i++];
    }
    return r;
}

// Chr$(codigo) -- cadena de UN caracter con ese codigo ASCII.
nb_string_t *nb_string_chr(int64_t codigo) {
    char c = (char)(codigo & 0xFF);
    return nb_string_new(&c, 1);
}

// Asc(cadena$) -- codigo del primer caracter, o -1 si esta vacia.
// Devuelve 0..255 (sin signo), no el valor con signo del char.
int64_t nb_string_asc(nb_string_t *s) {
    if (!s || nb_string_len(s) == 0) return -1;
    return (int64_t)(uint8_t)nb_string_cstr(s)[0];
}
