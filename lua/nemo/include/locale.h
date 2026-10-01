#ifndef NEMO_LOCALE_H
#define NEMO_LOCALE_H
struct lconv { char *decimal_point; };
struct lconv *localeconv(void);
#define LC_ALL 0
#define LC_NUMERIC 4
char *setlocale(int cat, const char *loc);
#endif
