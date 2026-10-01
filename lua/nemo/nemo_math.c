// nemo_math.c — libm minima para el port de Lua a Nemo OS.
//
// Lua 5.5 usa flotantes de verdad dentro de la VM: floor para la
// division entera (//), fmod para el modulo (%), pow para ^, y el
// resto para la libreria math. Sin estas 18 funciones no enlaza.
//
// Precision: las que ARM64 tiene como instruccion (fabs, floor, ceil,
// sqrt) son exactas. exp/log/pow/trig usan reduccion de rango + serie
// de Taylor con suficientes terminos para toda la precision de un
// double; en las pruebas contra glibc salen a 0-2 ULP salvo en
// argumentos enormes de sin/cos (reduccion de rango de 3 partes,
// precisa hasta ~1e9 radianes; mas alla degrada -- ningun script
// razonable calcula sin(1e15)).
//
// Mismo archivo para host (tests) y para Nemo OS.

#include <stdint.h>
#include "include/math.h"

typedef union { double d; uint64_t u; } dbits;

int nemo_signbit(double x) { dbits b; b.d = x; return (b.u >> 63) != 0; }

// ---- las que ARM64 hace en una instruccion ----
#if defined(__aarch64__)
double fabs(double x)  { double r; __asm__("fabs %d0, %d1" : "=w"(r) : "w"(x)); return r; }
double floor(double x) { double r; __asm__("frintm %d0, %d1" : "=w"(r) : "w"(x)); return r; }
double ceil(double x)  { double r; __asm__("frintp %d0, %d1" : "=w"(r) : "w"(x)); return r; }
double trunc(double x) { double r; __asm__("frintz %d0, %d1" : "=w"(r) : "w"(x)); return r; }
double sqrt(double x)  { double r; __asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x)); return r; }
#else
double fabs(double x) { dbits b; b.d = x; b.u &= ~(1ull << 63); return b.d; }
double trunc(double x) {
    if (x != x || x == 1.0/0.0 || x == -1.0/0.0) return x;
    if (fabs(x) >= 4503599627370496.0) return x;   // ya es entero
    int64_t i = (int64_t)x; return (double)i;
}
double floor(double x) { double t = trunc(x); return (t > x) ? t - 1.0 : t; }
double ceil(double x)  { double t = trunc(x); return (t < x) ? t + 1.0 : t; }
double sqrt(double x) {
    if (x < 0) return 0.0/0.0; if (x == 0 || x != x || x == 1.0/0.0) return x;
    dbits b; b.d = x; b.u = (b.u >> 1) + 0x1FF8000000000000ull; double r = b.d;
    for (int i = 0; i < 6; i++) r = 0.5 * (r + x / r);
    // correccion final al double correctamente redondeado, comprobando
    // los vecinos en long double (solo en el host; en ARM64 es fsqrt)
    dbits lo, hi; lo.d = r; hi.d = r; lo.u--; hi.u++;
    long double xl = x;
    long double er = (long double)r * r - xl; if (er < 0) er = -er;
    long double elo = (long double)lo.d * lo.d - xl; if (elo < 0) elo = -elo;
    long double ehi = (long double)hi.d * hi.d - xl; if (ehi < 0) ehi = -ehi;
    if (elo < er && elo <= ehi) return lo.d;
    if (ehi < er) return hi.d;
    return r;
}
#endif

double frexp(double x, int *e) {
    dbits b; b.d = x;
    int exp = (int)((b.u >> 52) & 0x7FF);
    if (exp == 0) { if (x == 0) { *e = 0; return x; } // subnormal: normalizar
        x *= 18446744073709551616.0; b.d = x; exp = (int)((b.u >> 52) & 0x7FF) - 64; }
    else if (exp == 0x7FF) { *e = 0; return x; }
    *e = exp - 1022;
    b.u = (b.u & 0x800FFFFFFFFFFFFFull) | (1022ull << 52);
    return b.d;
}
double ldexp(double x, int e) {
    // multiplicar por 2^e en pasos que no desborden el exponente
    while (e > 1000) { x *= 1.0715086071862673e301; e -= 1000; }
    while (e < -1000) { x *= 9.332636185032189e-302; e += 1000; }
    dbits b; b.u = (uint64_t)(e + 1023) << 52;
    return x * b.d;
}

double fmod(double x, double y) {
    if (y == 0 || x != x || y != y || x == 1.0/0.0 || x == -1.0/0.0) return 0.0/0.0;
    if (y == 1.0/0.0 || y == -1.0/0.0) return x;
    double ax = fabs(x), ay = fabs(y);
    if (ax < ay) return x;
    // restar y*2^k con k decreciente: cada resta es exacta (Sterbenz)
    int ex, ey; frexp(ax, &ex); frexp(ay, &ey);
    double r = ax;
    for (int k = ex - ey; k >= 0; k--) {
        double t = ldexp(ay, k);
        if (r >= t) r -= t;
    }
    return x < 0 ? -r : r;
}

// ---- exp / log ----
static const double LN2 = 0.6931471805599453094;
static const double LN2_HI = 6.93147180369123816490e-01, LN2_LO = 1.90821492927058770002e-10;

double exp(double x) {
    if (x != x) return x;
    if (x > 709.782712893384) return 1.0/0.0;
    if (x < -745.13321910194111) return 0.0;
    // x = k*ln2 + r, |r| <= ln2/2
    double kf = floor(x / LN2 + 0.5); int k = (int)kf;
    double r = (x - kf * LN2_HI) - kf * LN2_LO;
    // exp(r) por Horner de grado 17, de dentro hacia fuera: el termino
    // mas pequeno se suma primero y el error no se acumula
    double p = 1.0;
    for (int i = 17; i >= 1; i--) p = 1.0 + p * r / i;
    return ldexp(p, k);
}

double log(double x) {
    if (x != x || x < 0) return 0.0/0.0;
    if (x == 0) return -1.0/0.0;
    if (x == 1.0/0.0) return x;
    int e; double m = frexp(x, &e);           // x = m * 2^e, m en [0.5,1)
    if (m < 0.70710678118654752) { m *= 2; e--; } // m en [sqrt(1/2), sqrt(2))
    // log(m) = 2*atanh(s), s=(m-1)/(m+1), |s| < 0.172
    double s = (m - 1) / (m + 1), s2 = s * s, term = s, sum = 0;
    for (int i = 1; i < 40; i += 2) { sum += term / i; term *= s2; if (fabs(term) < 1e-18) break; }
    return 2 * sum + e * LN2;
}
double log2(double x)  { return log(x) / LN2; }
double log10(double x) { return log(x) / 2.30258509299404568402; }

// ---- pow ----
// exp(y*log(x)) en double multiplica el error: un fallo de 1 ULP en
// y*log(x) (que puede valer 700) es un error RELATIVO de 1e-13 en el
// resultado, cientos de ULP. Se evalua en long double (cuadruple
// precision por software en ARM64 -- lento, pero x^y con exponente no
// entero es raro en un script; los enteros y 0.5 van por atajos exactos).
static long double logl_(long double x) {
    int e = 0;
    while (x >= 2.0L) { x *= 0.5L; e++; }
    while (x < 1.0L)  { x *= 2.0L; e--; }
    if (x > 1.41421356237309504880L) { x *= 0.5L; e++; }
    long double s = (x - 1) / (x + 1), s2 = s * s, term = s, sum = 0;
    for (int i = 1; i < 80; i += 2) { sum += term / i; term *= s2; if (term < 1e-36L && term > -1e-36L) break; }
    return 2 * sum + e * 0.693147180559945309417232121458176568L;
}
static long double expl_(long double x) {
    long double kf = floor((double)(x / 0.693147180559945309417232121458176568L) + 0.5);
    long double r = x - kf * 0.693147180559945309417232121458176568L;
    long double p = 1.0L;
    for (int i = 27; i >= 1; i--) p = 1.0L + p * r / i;
    return ldexp((double)p, (int)kf);   // p en [0.7,1.42], ldexp exacto
}
static double powi(double x, int64_t n) {   // exponente entero: exacto por cuadrados
    int neg = n < 0; if (neg) n = -n;
    double r = 1;
    while (n) { if (n & 1) r *= x; x *= x; n >>= 1; }
    return neg ? 1.0 / r : r;
}
double pow(double x, double y) {
    if (y == 0) return 1.0;
    if (x == 1.0) return 1.0;
    if (x != x || y != y) return 0.0/0.0;
    double iy = trunc(y);
    if (y == iy && fabs(y) <= 1024) return powi(x, (int64_t)y);   // x^2, x^-1, x^10...
    if (y == 0.5 && x >= 0) return sqrt(x);                        // x^0.5, el idioma de Lua
    if (x == 0) { if (y < 0) return 1.0/0.0; return (y == iy && fmod(y, 2) == 1) ? x : 0.0; }
    if (x == 1.0/0.0) return y > 0 ? x : 0.0;
    if (x == -1.0/0.0) { if (y == iy && fmod(fabs(y), 2) == 1) return y > 0 ? x : -0.0; return y > 0 ? 1.0/0.0 : 0.0; }
    if (x < 0) {
        if (y != iy) return 0.0/0.0;                       // base negativa, exponente no entero
        double r = (double)expl_((long double)y * logl_(-x));
        return (fmod(fabs(y), 2) == 1) ? -r : r;
    }
    long double t = (long double)y * logl_(x);
    if (t > 709.78L) return 1.0/0.0;
    if (t < -745.2L) return 0.0;
    return (double)expl_(t);
}

// ---- trigonometria ----
// reduccion de rango: x = n*(pi/2) + r. pi/2 partido en 4 trozos de
// 24 bits cada uno: n*trozo es exacto mientras n < 2^29, es decir,
// |x| hasta ~8e8 radianes sin perder precision (con los 3 trozos de
// 33 bits clasicos de fdlibm el limite era ~1e6).
static const double PIO2_A = 1.57079625129699707031e+00, PIO2_B = 7.54978941586159635335e-08,
                    PIO2_C = 5.39030252995776476554e-15, PIO2_D = 3.28200354287350047444e-22;
static int reduce(double x, double *r) {
    double n = floor(x / 1.57079632679489661923 + 0.5);
    *r = (((x - n * PIO2_A) - n * PIO2_B) - n * PIO2_C) - n * PIO2_D;
    return (int)fmod(n, 4.0);
}
static double sin_k(double r) { double r2 = r*r, term = r, sum = r; for (int i = 1; i < 12; i++) { term *= -r2 / ((2*i) * (2*i+1)); sum += term; } return sum; }
static double cos_k(double r) { double r2 = r*r, term = 1, sum = 1; for (int i = 1; i < 12; i++) { term *= -r2 / ((2*i-1) * (2*i)); sum += term; } return sum; }

double sin(double x) {
    if (x != x || x == 1.0/0.0 || x == -1.0/0.0) return 0.0/0.0;
    double r; int q = reduce(x, &r); if (q < 0) q += 4;
    switch (q) { case 0: return sin_k(r); case 1: return cos_k(r); case 2: return -sin_k(r); default: return -cos_k(r); }
}
double cos(double x) {
    if (x != x || x == 1.0/0.0 || x == -1.0/0.0) return 0.0/0.0;
    double r; int q = reduce(x, &r); if (q < 0) q += 4;
    switch (q) { case 0: return cos_k(r); case 1: return -sin_k(r); case 2: return -cos_k(r); default: return sin_k(r); }
}
double tan(double x) { return sin(x) / cos(x); }

static double atan_k(double x) { // |x| <= tan(pi/12) ~ 0.268: serie
    double x2 = x*x, term = x, sum = x;
    for (int i = 1; i < 30; i++) { term *= -x2; sum += term / (2*i+1); if (fabs(term) < 1e-18) break; }
    return sum;
}
double atan(double x) {
    if (x != x) return x;
    if (x == 1.0/0.0) return 1.57079632679489661923;
    if (x == -1.0/0.0) return -1.57079632679489661923;
    int neg = x < 0; if (neg) x = -x;
    int inv = x > 1; if (inv) x = 1 / x;
    // reducir a |x| <= tan(pi/12) con atan(x) = pi/6 + atan((x*sqrt3 - 1)/(x + sqrt3))
    double r;
    if (x > 0.26794919243112270647) r = 0.52359877559829887308 + atan_k((x * 1.7320508075688772935 - 1) / (x + 1.7320508075688772935));
    else r = atan_k(x);
    if (inv) r = 1.57079632679489661923 - r;
    return neg ? -r : r;
}
double atan2(double y, double x) {
    if (x != x || y != y) return 0.0/0.0;
    if (x == 0 && y == 0) return nemo_signbit(x) ? (nemo_signbit(y) ? -M_PI : M_PI) : (nemo_signbit(y) ? -0.0 : 0.0);
    if (x == 1.0/0.0) { if (y == 1.0/0.0) return M_PI/4; if (y == -1.0/0.0) return -M_PI/4; return nemo_signbit(y) ? -0.0 : 0.0; }
    if (x == -1.0/0.0) { if (y == 1.0/0.0) return 3*M_PI/4; if (y == -1.0/0.0) return -3*M_PI/4; return nemo_signbit(y) ? -M_PI : M_PI; }
    if (y == 1.0/0.0) return M_PI/2; if (y == -1.0/0.0) return -M_PI/2;
    if (x == 0) return y > 0 ? M_PI/2 : -M_PI/2;
    double a = atan(y / x);
    if (x < 0) return y >= 0 && !nemo_signbit(y) ? a + M_PI : a - M_PI;
    return a;
}
// Cerca de |x|=1, atan2(x, sqrt(1-x^2)) pierde precision porque 1-x^2
// cancela. Identidad estable: asin(x) = pi/2 - 2*asin(sqrt((1-x)/2)).
double asin(double x) {
    if (x > 1 || x < -1) return 0.0/0.0;
    double ax = fabs(x);
    if (ax <= 0.5) return atan2(x, sqrt((1 - x) * (1 + x)));
    double r = 1.57079632679489661923 - 2 * atan2(sqrt((1 - ax) / 2), sqrt((1 + ax) / 2));
    return x < 0 ? -r : r;
}
double acos(double x) {
    if (x > 1 || x < -1) return 0.0/0.0;
    if (fabs(x) <= 0.5) return 1.57079632679489661923 - asin(x);
    if (x > 0) return 2 * atan2(sqrt((1 - x) / 2), sqrt((1 + x) / 2));
    return M_PI - 2 * atan2(sqrt((1 + x) / 2), sqrt((1 - x) / 2));
}
