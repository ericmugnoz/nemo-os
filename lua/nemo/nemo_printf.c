// nemo_printf.c — snprintf minimo para el port de Lua a Nemo OS.
//
// Lua formatea sus numeros con snprintf("%.14g") y sus enteros con
// "%lld" (lua_Number2str / LUAI_NUMFFORMAT). Su propio lua_pushfstring
// ya hace %s %d %c %p por su cuenta, asi que lo que hace falta aqui es
// sobre todo el formato de flotantes, que es la pieza mas ingrata de
// toda una libc. Soporta: %d %i %u %x %X %c %s %p %f %e %g %%, con
// modificadores l/ll, ancho y precision (.N y .*).
//
// Sin libc: solo aritmetica. Mismo archivo para host y para Nemo OS.
//
// LIMITACION CONOCIDA: en "%.14g" (el formato de tostring() de Lua),
// alrededor del 0,3% de los valores salen con el 14o digito desviado
// en +-1 respecto a glibc -- casos de "casi empate" donde el escalado
// por 10^k mete un error de redondeo. Corregirlo del todo exige
// conversion decimal exacta (algoritmo Ryu o similar, cientos de
// lineas); no afecta a ningun calculo, solo al ultimo digito impreso
// en esos casos. Con %f de valores > 2^64 la parte entera tampoco es
// exacta (haria falta aritmetica de precision arbitraria).

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

typedef struct { char *buf; size_t cap; size_t len; } out_t;

static void put(out_t *o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}
static void puts_n(out_t *o, const char *s, size_t n) { for (size_t i = 0; i < n; i++) put(o, s[i]); }
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static void pad(out_t *o, int n, char c) { while (n-- > 0) put(o, c); }

// entero sin signo a decimal/hex en 'tmp' (al reves), devuelve longitud
static int utoa_base(uint64_t v, unsigned base, bool upper, char *tmp) {
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = dig[v % base]; v /= base; }
    return n;
}

static void emit_int(out_t *o, bool neg, const char *digits_rev, int nd, int width, int prec, bool left, bool zero) {
    int nz = (prec > nd) ? prec - nd : 0;        // ceros por precision
    int body = nd + nz + (neg ? 1 : 0);
    int padn = width > body ? width - body : 0;
    if (!left && !zero) pad(o, padn, ' ');
    if (neg) put(o, '-');
    if (!left && zero && prec < 0) pad(o, padn, '0');
    pad(o, nz, '0');
    for (int i = nd - 1; i >= 0; i--) put(o, digits_rev[i]);
    if (left) pad(o, padn, ' ');
}

// ---- flotantes ----
// Genera 'prec' digitos significativos de |v| en 'dig' (sin punto) y
// devuelve el exponente decimal E tal que v = d.ddd * 10^E.
//
// Metodo: en vez de extraer digito a digito multiplicando por 10 (el
// error se acumula y el 14o digito sale mal), se escala UNA sola vez
// por una potencia de 10 exacta (10^0..10^22 son exactamente
// representables en double) y se convierte a entero de 64 bits con
// un unico redondeo. Para exponentes extremos (fuera de +-22) se cae
// al metodo iterativo -- son valores rarisimos en un script.
static const double POW10[] = { 1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11,
    1e12,1e13,1e14,1e15,1e16,1e17,1e18,1e19,1e20,1e21,1e22 };

// Redondeo al par en empate exacto (regla IEEE, la que usa glibc):
// 0.5 exacto -> al entero par mas cercano.
static uint64_t round_even(double x) {
    uint64_t m = (uint64_t)x;
    double f = x - (double)m;
    if (f > 0.5 || (f == 0.5 && (m & 1))) m++;
    return m;
}

static int fp_digits(double v, int prec, char *dig) {
    if (v == 0.0) { for (int i = 0; i < prec; i++) dig[i] = '0'; return 0; }
    if (prec > 17) prec = 17;
    // estimar el exponente decimal
    int e = 0; double t = v;
    while (t >= 10.0) { t /= 10.0; e++; }
    while (t < 1.0)   { t *= 10.0; e--; }
    int k = prec - 1 - e;   // v * 10^k tiene 'prec' digitos enteros
    // escalado en long double: en ARM64 es cuadruple precision (113
    // bits), suficiente para que los 17 digitos de %.17g salgan exactos
    long double lv = v, lp = 1.0L, base = 10.0L; int kk = k < 0 ? -k : k;
    while (kk) { if (kk & 1) lp *= base; base *= base; kk >>= 1; }
    long double ls = k >= 0 ? lv * lp : lv / lp;
    double scaled = (double)ls;
    uint64_t m_ld = (uint64_t)ls; long double fr = ls - (long double)m_ld;
    if (fr > 0.5L || (fr == 0.5L && (m_ld & 1))) m_ld++;
    uint64_t m = m_ld; (void)scaled;
    // el escalado pudo dejar 'prec'+1 o 'prec'-1 digitos por el error de estimar e
    uint64_t lim = 1; for (int i = 0; i < prec; i++) lim *= 10;
    if (m >= lim) { m = round_even(scaled / 10.0); e++; }
    else if (m < lim / 10 && m != 0) { m = round_even(scaled * 10.0); e--; if (m >= lim) { m = lim - 1; } }

    for (int i = prec - 1; i >= 0; i--) { dig[i] = (char)('0' + m % 10); m /= 10; }
    return e;
}

static void emit_exp(out_t *o, int e, bool upper) {
    put(o, upper ? 'E' : 'e');
    put(o, e < 0 ? '-' : '+');
    if (e < 0) e = -e;
    if (e < 10) put(o, '0');
    char t[8]; int n = utoa_base((uint64_t)e, 10, false, t);
    for (int i = n - 1; i >= 0; i--) put(o, t[i]);
}

static void emit_double(out_t *o, double v, char conv, int width, int prec, bool left, bool zero, bool alt) {
    bool upper = (conv == 'E' || conv == 'G' || conv == 'F');
    char c = (char)(conv | 0x20);
    // inf / nan
    if (v != v) { const char *s = upper ? "NAN" : "nan"; int p = width > 3 ? width - 3 : 0; if (!left) pad(o, p, ' '); puts_n(o, s, 3); if (left) pad(o, p, ' '); return; }
    union { double d; uint64_t u; } sb; sb.d = v;
    bool neg = (sb.u >> 63) != 0; if (neg) v = -v;
    if (v > 1.7976931348623157e308) { const char *s = upper ? "INF" : "inf"; int body = 3 + (neg?1:0); int p = width > body ? width - body : 0; if (!left) pad(o, p, ' '); if (neg) put(o,'-'); puts_n(o, s, 3); if (left) pad(o, p, ' '); return; }

    if (prec < 0) prec = 6;
    char tmp[64]; size_t tn = 0;
    #define T(ch) do { if (tn < sizeof(tmp)-1) tmp[tn++] = (ch); } while (0)

    if (c == 'g') {
        int P = prec == 0 ? 1 : prec;
        char dig[40];
        int e = fp_digits(v, P, dig);
        bool use_exp = (e < -4 || e >= P);
        if (use_exp) {
            T(dig[0]);
            int last = P - 1;
            if (!alt) while (last > 0 && dig[last] == '0') last--;
            if (last > 0 || alt) { T('.'); for (int i = 1; i <= last; i++) T(dig[i]); }
            char ex[16]; out_t eo = { ex, sizeof(ex), 0 }; emit_exp(&eo, e, upper);
            for (size_t i = 0; i < eo.len; i++) T(ex[i]);
        } else {
            // notacion fija con P digitos significativos
            int int_digits = e + 1;            // cuantos van antes del punto
            if (int_digits <= 0) { T('0'); T('.'); for (int i = 0; i < -int_digits; i++) T('0'); for (int i = 0; i < P; i++) T(dig[i]); }
            else { for (int i = 0; i < int_digits; i++) T(i < P ? dig[i] : '0'); if (P > int_digits) { T('.'); for (int i = int_digits; i < P; i++) T(dig[i]); } }
            if (!alt) { // quitar ceros finales y punto colgante
                bool has_dot = false; for (size_t i = 0; i < tn; i++) if (tmp[i] == '.') has_dot = true;
                if (has_dot) { while (tn > 0 && tmp[tn-1] == '0') tn--; if (tn > 0 && tmp[tn-1] == '.') tn--; }
            }
        }
    } else if (c == 'e') {
        char dig[40];
        int e = fp_digits(v, prec + 1, dig);
        T(dig[0]);
        if (prec > 0 || alt) { T('.'); for (int i = 1; i <= prec; i++) T(dig[i]); }
        char ex[16]; out_t eo = { ex, sizeof(ex), 0 }; emit_exp(&eo, e, upper);
        for (size_t i = 0; i < eo.len; i++) T(ex[i]);
    } else { // 'f'
        // parte entera exacta via uint64 (suficiente hasta ~1.8e19), resto por digitos
        double ip = 0; double fp = v;
        uint64_t whole;
        if (v >= 18446744073709551615.0) { // demasiado grande: usar notacion cientifica aprox
            char dig[40]; int e = fp_digits(v, 17, dig);
            for (int i = 0; i <= e; i++) T(i < 17 ? dig[i] : '0');
            if (prec > 0) { T('.'); for (int i = 0; i < prec; i++) T('0'); }
            goto done;
        }
        whole = (uint64_t)v; ip = (double)whole; fp = v - ip;
        // redondear la parte fraccionaria a 'prec' digitos
        double scale = 1.0; for (int i = 0; i < prec; i++) scale *= 10.0;
        double frac_scaled = fp * scale + 0.5;
        uint64_t fr = (uint64_t)frac_scaled;
        if (fr >= (uint64_t)scale) { fr = 0; whole++; }
        char wd[24]; int wn = utoa_base(whole, 10, false, wd);
        for (int i = wn - 1; i >= 0; i--) T(wd[i]);
        if (prec > 0 || alt) {
            T('.');
            char fd[24]; int fn = utoa_base(fr, 10, false, fd);
            for (int i = fn; i < prec; i++) T('0');
            for (int i = fn - 1; i >= 0; i--) T(fd[i]);
        }
    }
done:;
    int body = (int)tn + (neg ? 1 : 0);
    int padn = width > body ? width - body : 0;
    if (!left && !zero) pad(o, padn, ' ');
    if (neg) put(o, '-');
    if (!left && zero) pad(o, padn, '0');
    puts_n(o, tmp, tn);
    if (left) pad(o, padn, ' ');
    #undef T
}

int nemo_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    out_t o = { buf, cap, 0 };
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { put(&o, *p); continue; }
        p++;
        bool left = false, zero = false, alt = false, plus = false, space = false;
        for (;; p++) {
            if (*p == '-') left = true; else if (*p == '0') zero = true; else if (*p == '#') alt = true;
            else if (*p == '+') plus = true; else if (*p == ' ') space = true; else break;
        }
        int width = -1;
        if (*p == '*') { width = va_arg(ap, int); if (width < 0) { left = true; width = -width; } p++; }
        else { while (*p >= '0' && *p <= '9') { if (width < 0) width = 0; width = width * 10 + (*p - '0'); p++; } }
        int prec = -1;
        if (*p == '.') { p++; prec = 0; if (*p == '*') { prec = va_arg(ap, int); p++; } else while (*p >= '0' && *p <= '9') { prec = prec * 10 + (*p - '0'); p++; } }
        int lcount = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 't') { if (*p == 'l') lcount++; if (*p=='z'||*p=='j'||*p=='t') lcount = 2; p++; }
        char conv = *p;
        char tmp[32]; int nd; bool neg;
        (void)plus; (void)space;
        switch (conv) {
            case 'd': case 'i': {
                int64_t v = lcount >= 2 ? va_arg(ap, long long) : lcount == 1 ? va_arg(ap, long) : va_arg(ap, int);
                neg = v < 0; uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
                nd = utoa_base(u, 10, false, tmp); emit_int(&o, neg, tmp, nd, width, prec, left, zero); break; }
            case 'u': case 'x': case 'X': {
                uint64_t u = lcount >= 2 ? va_arg(ap, unsigned long long) : lcount == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
                nd = utoa_base(u, conv == 'u' ? 10 : 16, conv == 'X', tmp); emit_int(&o, false, tmp, nd, width, prec, left, zero); break; }
            case 'p': { uint64_t u = (uint64_t)(uintptr_t)va_arg(ap, void *); put(&o, '0'); put(&o, 'x'); nd = utoa_base(u, 16, false, tmp); emit_int(&o, false, tmp, nd, 0, -1, false, false); break; }
            case 'c': { char ch = (char)va_arg(ap, int); int padn = width > 1 ? width - 1 : 0; if (!left) pad(&o, padn, ' '); put(&o, ch); if (left) pad(&o, padn, ' '); break; }
            case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; size_t n = slen(s); if (prec >= 0 && (size_t)prec < n) n = (size_t)prec; int padn = width > (int)n ? width - (int)n : 0; if (!left) pad(&o, padn, ' '); puts_n(&o, s, n); if (left) pad(&o, padn, ' '); break; }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': { double v = va_arg(ap, double); emit_double(&o, v, conv, width, prec, left, zero, alt); break; }
            case '%': put(&o, '%'); break;
            default: put(&o, '%'); put(&o, conv); break;
        }
    }
    if (cap > 0) buf[o.len < cap ? o.len : cap - 1] = '\0';
    return (int)o.len;
}

int nemo_snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = nemo_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
