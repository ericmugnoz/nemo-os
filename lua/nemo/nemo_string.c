// nemo_string.c — string.h, ctype.h, strtod y utilidades varias para
// el port de Lua a Nemo OS. Sin dependencias. Mismo archivo para host
// (tests) y para Nemo OS.
#include <stddef.h>
#include <stdint.h>
#include "include/string.h"
#include "include/ctype.h"
#include "include/stdlib.h"
#include "include/errno.h"
#include "include/locale.h"

int errno;

void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) { while (n--) *a++ = *b++; }
    else if (a > b) { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, size_t n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }
int memcmp(const void *x, const void *y, size_t n) {
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
void *memchr(const void *s, int c, size_t n) { const unsigned char *p = s; for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p; return NULL; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) { for (; n; n--, a++, b++) { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; if (!*a) return 0; } return 0; }
int strcoll(const char *a, const char *b) { return strcmp(a, b); } // locale "C": orden de bytes
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strncpy(char *d, const char *s, size_t n) { char *r = d; while (n && (*d = *s)) { d++; s++; n--; } while (n--) *d++ = 0; return r; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; } }
char *strrchr(const char *s, int c) { const char *r = NULL; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
char *strstr(const char *h, const char *n) {
    size_t ln = strlen(n); if (!ln) return (char *)h;
    for (; *h; h++) if (*h == *n && !strncmp(h, n, ln)) return (char *)h;
    return NULL;
}
size_t strspn(const char *s, const char *acc) { size_t n = 0; for (; s[n] && strchr(acc, s[n]); n++); return n; }
size_t strcspn(const char *s, const char *rej) { size_t n = 0; for (; s[n] && !strchr(rej, s[n]); n++); return n; }
char *strpbrk(const char *s, const char *acc) { for (; *s; s++) if (strchr(acc, *s)) return (char *)s; return NULL; }
char *strerror(int e) { (void)e; return "error de E/S"; }

int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

int abs(int x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
char *getenv(const char *n) { (void)n; return NULL; }

static struct lconv nemo_lconv = { "." };
struct lconv *localeconv(void) { return &nemo_lconv; }
char *setlocale(int cat, const char *loc) { (void)cat; (void)loc; return "C"; }

// strtod: decimal (con exponente) y hexadecimal (0x1.8p3, que Lua
// acepta). Construye el valor con enteros de 64 bits y una sola
// escalada final por potencia de 10, para minimizar el error.
// Escalado en long double (en ARM64: cuadruple precision por software,
// via libgcc; en x86 host: 80 bits). Con la mantisa de 19 digitos y
// la potencia de 10 en precision extendida, el unico redondeo real es
// la conversion final a double -- asi la mayoria de literales de 16-17
// cifras salen exactos, que con double a secas se iban 1 ULP.
static long double pow10l(int e) {
    long double r = 1.0L, b = 10.0L; int n = e < 0 ? -e : e;
    while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
    return e < 0 ? 1.0L / r : r;
}
static double scale10(uint64_t m, int e) {
    if (e > 340) return 1.0 / 0.0;
    if (e < -400) return 0.0;
    if (e >= 0) return (double)((long double)m * pow10l(e));
    // dividir en vez de multiplicar por 10^-e: mas preciso que 1/10^e
    return (double)((long double)m / pow10l(-e));
}
double strtod(const char *s, char **end) {
    const char *p = s;
    while (isspace(*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    double v = 0; int any = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && (isxdigit(p[2]) || (p[2] == '.' && isxdigit(p[3])))) {
        p += 2; int exp2 = 0;
        for (; isxdigit(*p); p++) { any = 1; int d = isdigit(*p) ? *p - '0' : (tolower(*p) - 'a' + 10); v = v * 16 + d; }
        if (*p == '.') { p++; for (; isxdigit(*p); p++) { any = 1; int d = isdigit(*p) ? *p - '0' : (tolower(*p) - 'a' + 10); v = v * 16 + d; exp2 -= 4; } }
        if (any && (*p == 'p' || *p == 'P')) { const char *q = p + 1; int en = 0; if (*q == '+' || *q == '-') { en = (*q == '-'); q++; } if (isdigit(*q)) { int e = 0; for (; isdigit(*q); q++) e = e * 10 + (*q - '0'); exp2 += en ? -e : e; p = q; } }
        while (exp2 > 0) { v *= 2; exp2--; } while (exp2 < 0) { v /= 2; exp2++; }
    } else {
        // decimal: acumular hasta 19 digitos significativos en uint64
        uint64_t m = 0; int nd = 0, exp10 = 0;
        for (; isdigit(*p); p++) { any = 1; if (nd < 19) { m = m * 10 + (*p - '0'); nd++; } else exp10++; }
        if (*p == '.') { p++; for (; isdigit(*p); p++) { any = 1; if (nd < 19) { m = m * 10 + (*p - '0'); nd++; exp10--; } } }
        if (!any) { if (end) *end = (char *)s; return 0; }
        if (*p == 'e' || *p == 'E') { const char *q = p + 1; int en = 0; if (*q == '+' || *q == '-') { en = (*q == '-'); q++; } if (isdigit(*q)) { int e = 0; for (; isdigit(*q) && e < 100000; q++) e = e * 10 + (*q - '0'); exp10 += en ? -e : e; p = q; } }
        v = scale10(m, exp10);
        if (v > 1.7976931348623157e308) errno = ERANGE;
    }
    if (!any) { if (end) *end = (char *)s; return 0; }
    if (end) *end = (char *)p;
    return neg ? -v : v;
}
