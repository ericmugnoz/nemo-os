// nb_math.c — funciones matemáticas del runtime de Nemo-Blitz 2.0
// que NO se pueden resolver con una sola instrucción del procesador.
//
// Sqr, Abs, Min, Max, Int y Float no están aquí: el generador de
// código las emite directamente (fsqrt, fabs, cmp+cneg, csel,
// fcvtzs, scvtf). Aquí viven solo las que necesitan un algoritmo de
// verdad: seno, coseno, tangente y arcotangente.
//
// ÁNGULOS EN GRADOS, no en radianes — misma convención que
// BlitzPlus/Blitz3D, y la que ya usaba el compilador anterior (tenía
// sus propias constantes deg2rad). Sin(90) = 1, no Sin(1.5707...).
//
// Sin libm y sin libc: mismo entorno que nb_alloc.c y nb_string.c.
// Todo con aritmética de coma flotante de doble precisión, que el
// propio ARM64 hace en hardware — lo único que no trae el procesador
// es la serie polinómica, que es justo lo que está aquí.

#include <stdint.h>

#define NB_PI       3.14159265358979323846
#define NB_TWO_PI   6.28318530717958647692
#define NB_HALF_PI  1.57079632679489661923
#define NB_DEG2RAD  0.01745329251994329577
#define NB_RAD2DEG 57.29577951308232087680

// Seno, con el ángulo en GRADOS.
//
// Tres pasos: pasar a radianes, reducir el ángulo al rango donde la
// serie converge bien, y aplicar la serie de Taylor.
//
// La reducción es la parte que importa: la serie de Taylor del seno
// es exacta cerca de cero pero pierde precisión rápido según crece el
// ángulo, así que primero se lleva el valor a [-pi, pi] restando
// vueltas enteras, y luego a [-pi/2, pi/2] aprovechando que
// sin(pi - x) = sin(x). Dentro de ese rango, los siete términos de
// abajo dan más precisión de la que cualquier programa BASIC va a
// necesitar.
double nb_sin(double degrees) {
    double r = degrees * NB_DEG2RAD;

    // Quitar las vueltas completas: r -= round(r / 2pi) * 2pi
    double q = r / NB_TWO_PI;
    int64_t k = (int64_t)(q >= 0.0 ? q + 0.5 : q - 0.5);
    r -= (double)k * NB_TWO_PI;

    // Reflejar al primer/cuarto cuadrante: sin(pi - x) = sin(x)
    if (r > NB_HALF_PI) r = NB_PI - r;
    else if (r < -NB_HALF_PI) r = -NB_PI - r;

    // Serie de Taylor: x - x^3/3! + x^5/5! - x^7/7! + ...
    // escrita en forma anidada (Horner) para hacer solo
    // multiplicaciones y sumas, sin potencias ni divisiones.
    //
    // Llega hasta x^17/17!: con menos términos el error se notaba
    // justo en el peor sitio, los múltiplos de 90 grados, donde la
    // reducción deja el valor pegado a pi/2 (que es lo más lejos de
    // cero que puede quedar, y por tanto donde la serie trabaja más
    // forzada). Con trece términos daba 6.6e-10; con estos, del
    // orden de 1e-12.
    double x2 = r * r;
    return r * (1.0 + x2 * (-1.0 / 6.0
              + x2 * (1.0 / 120.0
              + x2 * (-1.0 / 5040.0
              + x2 * (1.0 / 362880.0
              + x2 * (-1.0 / 39916800.0
              + x2 * (1.0 / 6227020800.0
              + x2 * (-1.0 / 1307674368000.0
              + x2 * (1.0 / 355687428096000.0)))))))));
}

// Coseno: cos(x) = sin(x + 90 grados). No hace falta repetir la
// serie entera.
double nb_cos(double degrees) {
    return nb_sin(degrees + 90.0);
}

// Tangente = seno / coseno. Cuando el coseno es cero (90, 270
// grados...) la tangente es infinita; aquí se devuelve 0 en vez de
// dividir por cero, que en coma flotante daría un infinito o un NaN
// y probablemente estropearía los cálculos siguientes sin avisar.
double nb_tan(double degrees) {
    double c = nb_cos(degrees);
    if (c == 0.0) return 0.0;
    return nb_sin(degrees) / c;
}

// Arcotangente: recibe una razón, devuelve GRADOS entre -90 y 90.
//
// La serie de Taylor del arcotangente converge muy despacio cerca de
// |x| = 1, así que en vez de usarla se emplea una aproximación
// polinómica ajustada para el rango [-1, 1], y los valores fuera de
// ese rango se reducen con la identidad
//   atan(x) = pi/2 - atan(1/x)   (para x > 1)
// que los trae de vuelta al rango bueno.
double nb_atan(double v) {
    int negativo = 0;
    int invertido = 0;

    if (v < 0.0) { v = -v; negativo = 1; }
    if (v > 1.0) { v = 1.0 / v; invertido = 1; }

    double x2 = v * v;
    double r = v * (0.99986600 + x2 * (-0.33029953
                  + x2 * (0.18014568
                  + x2 * (-0.08513300
                  + x2 * 0.02083510))));

    if (invertido) r = NB_HALF_PI - r;
    if (negativo) r = -r;
    return r * NB_RAD2DEG;
}

// Generador de números aleatorios (LCG -- Linear Congruential Generator).
// No es criptográfico ni especialmente bueno estadísticamente, pero es
// suficiente para juegos: distribución uniforme, barato, sin libm, sin
// estado en el kernel -- vive entero aqui, en el propio programa.
//
// Parámetros de Knuth (MMIX): m=2^64, a=6364136223846793005, c=1442695040888963407
// El estado es una variable global dentro del bloque de runtime (zona .bss
// del bloque, a cero al arrancar el programa -- la primera llamada
// produce el mismo resultado siempre, lo que es correcto para demos; para
// resultados distintos en cada partida el programa puede hacer
// Seed(MilliSecs()) al principio).

// El estado empieza en 0 (en .bss, no en .data) para que el script
// nb_elf_extract.py no tenga que manejar la seccion .data, que tiene
// contenido real y necesitaria un tratamiento distinto al del .bss
// (que es todo ceros y no se escribe en el archivo .pro).
// El LCG funciona igual desde 0: el primer paso da
// 1442695040888963407, que es un valor de arranque perfectamente valido.
static uint64_t rnd_state;

// Rnd(n) -- entero uniforme en [0, n-1]. Con n=0 devuelve 0.
int64_t nb_rnd(int64_t n) {
    rnd_state = rnd_state * 6364136223846793005ULL + 1442695040888963407ULL;
    if (n <= 0) return 0;
    // La mitad alta del estado tiene mejor calidad estadística
    uint64_t hi = rnd_state >> 33;
    return (int64_t)(hi % (uint64_t)n);
}

// Rnd#() -- flotante uniforme en [0.0, 1.0)
double nb_rnd_float(void) {
    rnd_state = rnd_state * 6364136223846793005ULL + 1442695040888963407ULL;
    // 53 bits de mantisa para doble precision
    uint64_t bits = rnd_state >> 11;
    return (double)bits * (1.0 / 9007199254740992.0); // / 2^53
}

// Seed(n) -- fijar el estado del generador. Permite reproducibilidad
// o, con MilliSecs(), resultados distintos en cada ejecucion.
void nb_rnd_seed(int64_t seed) {
    rnd_state = (uint64_t)seed;
    // Un solo paso para evitar que semillas pequenas (como 0) den
    // malos primeros resultados
    rnd_state = rnd_state * 6364136223846793005ULL + 1442695040888963407ULL;
}

// ---------------------------------------------------------------------
// Exponencial y logaritmo
// ---------------------------------------------------------------------
// Mismo enfoque que las trigonometricas: reducir el argumento al rango
// donde la serie converge rapido, aplicar la serie, y deshacer la
// reduccion.

#define NB_LN2  0.69314718055994530942
#define NB_E    2.71828182845904523536

// Exp(x) -- e elevado a x.
//
// Reduccion: e^x = 2^k * e^r, donde k es el entero mas cercano a
// x/ln(2) y r = x - k*ln(2) queda en [-ln(2)/2, ln(2)/2], un rango
// donde la serie de Taylor converge en muy pocos terminos.
// El 2^k se aplica al final construyendo el exponente del propio
// numero en coma flotante, sin bucles.
double nb_exp(double x) {
    // Limites: fuera de ellos el resultado no cabe en un double
    if (x > 709.0) return 1.0e308 * 1.0e10;   // infinito
    if (x < -745.0) return 0.0;

    double q = x / NB_LN2;
    int64_t k = (int64_t)(q >= 0.0 ? q + 0.5 : q - 0.5);
    double r = x - (double)k * NB_LN2;

    // Serie de Taylor de e^r: 1 + r + r^2/2! + r^3/3! + ...
    //
    // Llega hasta r^13/13!. Con |r| <= ln(2)/2 = 0.347, cortar en r^9
    // dejaba un error relativo de 1.5e-12 -- pequeno, pero medible y
    // evitable: cada termino extra cuesta una multiplicacion y una
    // suma. Con estos, el error baja al limite de la doble precision.
    double s = 1.0 + r * (1.0
             + r * (1.0/2.0
             + r * (1.0/6.0
             + r * (1.0/24.0
             + r * (1.0/120.0
             + r * (1.0/720.0
             + r * (1.0/5040.0
             + r * (1.0/40320.0
             + r * (1.0/362880.0
             + r * (1.0/3628800.0
             + r * (1.0/39916800.0
             + r * (1.0/479001600.0
             + r * (1.0/6227020800.0)))))))))))));

    // Multiplicar por 2^k montando el numero a mano: el exponente de
    // un double ocupa los bits 62..52, con sesgo 1023.
    union { double d; uint64_t u; } dosk;
    int64_t exponente = 1023 + k;
    if (exponente <= 0) return 0.0;
    if (exponente >= 2047) return 1.0e308 * 1.0e10;
    dosk.u = (uint64_t)exponente << 52;
    return s * dosk.d;
}

// Log(x) -- logaritmo NATURAL (base e), como en BlitzPlus.
//
// Reduccion: todo double es m * 2^e con m en [1,2). Entonces
// log(x) = log(m) + e*log(2). Para log(m) se usa la serie del
// logaritmo en terminos de z=(m-1)/(m+1), que converge mucho mas
// rapido que la serie directa.
double nb_log(double x) {
    if (x <= 0.0) return -1.0e308 * 1.0e10;   // -infinito (o NaN para x<0)

    union { double d; uint64_t u; } v;
    v.d = x;
    int64_t e = (int64_t)((v.u >> 52) & 0x7FF) - 1023;
    // Dejar la mantisa como un numero en [1,2): exponente = 1023
    v.u = (v.u & 0x000FFFFFFFFFFFFFULL) | (1023ULL << 52);
    double m = v.d;

    // Centrar mejor: si m > sqrt(2), usar m/2 y sumar 1 al exponente.
    if (m > 1.41421356237309504880) { m *= 0.5; e += 1; }

    double z = (m - 1.0) / (m + 1.0);
    double z2 = z * z;
    // log(m) = 2z * (1 + z^2/3 + z^4/5 + z^6/7 + ...)
    double s = 2.0 * z * (1.0
             + z2 * (1.0/3.0
             + z2 * (1.0/5.0
             + z2 * (1.0/7.0
             + z2 * (1.0/9.0
             + z2 * (1.0/11.0
             + z2 * (1.0/13.0)))))));

    return s + (double)e * NB_LN2;
}
