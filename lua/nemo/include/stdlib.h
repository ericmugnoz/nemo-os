#ifndef NEMO_STDLIB_H
#define NEMO_STDLIB_H
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
void  *malloc(size_t n);
void  *realloc(void *p, size_t n);
void   free(void *p);
void   abort(void) __attribute__((noreturn));
void   exit(int code) __attribute__((noreturn));
int    abs(int x);
long long llabs(long long x);
double strtod(const char *s, char **end);
char  *getenv(const char *name);
#endif
