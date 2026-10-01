#ifndef NEMO_MATH_H
#define NEMO_MATH_H
#define HUGE_VAL (1.0/0.0)
#define INFINITY (1.0f/0.0f)
#define NAN (0.0/0.0)
#define M_PI 3.14159265358979323846
double fabs(double x); double floor(double x); double ceil(double x); double sqrt(double x);
double fmod(double x, double y); double frexp(double x, int *e); double ldexp(double x, int e);
double exp(double x); double log(double x); double log2(double x); double log10(double x);
double pow(double x, double y);
double sin(double x); double cos(double x); double tan(double x);
double atan(double x); double atan2(double y, double x); double asin(double x); double acos(double x);
double trunc(double x);
#define isnan(x) ((x) != (x))
#define isinf(x) ((x) == HUGE_VAL || (x) == -HUGE_VAL)
#define signbit(x) nemo_signbit(x)
int nemo_signbit(double x);
#endif
