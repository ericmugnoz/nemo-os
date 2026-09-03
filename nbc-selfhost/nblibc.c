// nblibc.c — ver nblibc.h

#include "nblibc.h"

void *nb_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *nb_memset(void *dst, int value, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)value;
    return dst;
}

uint32_t nb_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

int nb_strcmp(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int nb_strncmp(const char *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0') return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
    }
    return 0;
}

char *nb_strncpy(char *dst, const char *src, uint32_t n) {
    // Copia hasta 'n' caracteres (no n-1) y SIEMPRE termina en nulo
    // -- coincide con como se llama en TODO el compilador
    // (nb_strncpy(dst, src, sizeof(dst)-1), la misma convencion que
    // la strncpy real de C). Antes restaba un -1 de mas aqui dentro,
    // recortando un caracter de mas en CADA llamada del compilador
    // autohospedado entero -- invisible mientras ninguna cadena
    // llegara justo al limite del buffer, hasta que una cadena de 69
    // caracteres (truncada a 63 por Node->text) revelo la diferencia
    // frente al host.
    uint32_t i = 0;
    while (i < n && src[i] != '\0') { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return dst;
}

bool nb_isalpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool nb_isdigit(char c) { return c >= '0' && c <= '9'; }
bool nb_isalnum(char c) { return nb_isalpha(c) || nb_isdigit(c); }
bool nb_isxdigit(char c) { return nb_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool nb_isspace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
char nb_toupper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }
char nb_tolower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

int64_t nb_atoi64(const char *s) {
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    int64_t value = 0;
    while (nb_isdigit(*s)) { value = value * 10 + (*s - '0'); s++; }
    return neg ? -value : value;
}

void nb_itoa(int64_t value, char *buf, uint32_t buf_size) {
    if (buf_size == 0) return;
    uint32_t pos = 0;
    bool neg = value < 0;
    uint64_t v = neg ? (uint64_t)(-value) : (uint64_t)value;
    char digits[24];
    int n = 0;
    if (v == 0) digits[n++] = '0';
    while (v > 0 && n < 24) { digits[n++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg && pos < buf_size - 1) buf[pos++] = '-';
    while (n > 0 && pos < buf_size - 1) buf[pos++] = digits[--n];
    buf[pos] = '\0';
}

// Buffer de asignacion "de avance" -- 4MB deberian sobrar de sobra
// para compilar cualquier programa .bb razonable (el propio
// compilador, con todos sus arboles y tablas, ocupa mucho menos que
// eso incluso para archivos grandes).
#define NB_ALLOC_POOL_SIZE (4 * 1024 * 1024)
static uint8_t nb_alloc_pool[NB_ALLOC_POOL_SIZE];
static uint32_t nb_alloc_pos = 0;

void nb_alloc_reset(void) { nb_alloc_pos = 0; }

void *nb_alloc(uint32_t size) {
    // Alineado a 8 bytes -- varias estructuras del compilador
    // guardan enteros de 64 bits, y un acceso desalineado en ARM64
    // puede ser mas lento (o directamente fallar segun la
    // configuracion de la MMU).
    uint32_t aligned_size = (size + 7) & ~7u;
    if (nb_alloc_pos + aligned_size > NB_ALLOC_POOL_SIZE) {
        return 0; // sin memoria -- el llamador debe comprobarlo
    }
    void *ptr = &nb_alloc_pool[nb_alloc_pos];
    nb_alloc_pos += aligned_size;
    nb_memset(ptr, 0, size); // igual que calloc, no como malloc
    return ptr;
}
