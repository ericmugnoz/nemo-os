#ifndef NEMO_STDIO_H
#define NEMO_STDIO_H
#include <stddef.h>
#include <stdarg.h>
// FILE minima: los archivos se leen ENTEROS a memoria al abrir (los
// scripts .lua son pequeños; NemoFS no tiene lectura parcial por
// posicion), y stdout/stderr van a la consola via syscall.
typedef struct nemo_FILE {
    unsigned char *buf; size_t size; size_t pos;
    int err; int eof; int is_console; int fd_console; // 1=stdout 2=stderr
} FILE;
extern FILE *stdin, *stdout, *stderr;
#define EOF (-1)
#define BUFSIZ 512
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define L_tmpnam 32
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
FILE  *fopen(const char *path, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *f);
int    fclose(FILE *f);
size_t fread(void *p, size_t sz, size_t n, FILE *f);
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f);
int    getc(FILE *f);
int    fgetc(FILE *f);
int    ungetc(int c, FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
int    fflush(FILE *f);
int    fputs(const char *s, FILE *f);
char  *fgets(char *s, int n, FILE *f);
int    fprintf(FILE *f, const char *fmt, ...);
int    printf(const char *fmt, ...);
int    snprintf(char *buf, size_t cap, const char *fmt, ...);
int    vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int    sprintf(char *buf, const char *fmt, ...);
#endif
