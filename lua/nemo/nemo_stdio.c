// nemo_stdio.c — stdio minima para Lua sobre nemo_platform.h.
#include "include/stdio.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/errno.h"
#include "nemo_platform.h"

int nemo_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

static FILE f_stdin  = { 0, 0, 0, 0, 1, 1, 0 };
static FILE f_stdout = { 0, 0, 0, 0, 0, 1, 1 };
static FILE f_stderr = { 0, 0, 0, 0, 0, 1, 2 };
FILE *stdin = &f_stdin, *stdout = &f_stdout, *stderr = &f_stderr;

FILE *fopen(const char *path, const char *mode) {
    if (mode[0] != 'r') { errno = EINVAL; return NULL; }   // solo lectura por ahora
    unsigned char *buf; size_t size;
    if (nemo_plat_read_file(path, &buf, &size) != 0) { errno = ENOENT; return NULL; }
    FILE *f = malloc(sizeof(FILE));
    if (!f) { free(buf); return NULL; }
    f->buf = buf; f->size = size; f->pos = 0; f->err = 0; f->eof = 0; f->is_console = 0; f->fd_console = 0;
    return f;
}
FILE *freopen(const char *path, const char *mode, FILE *f) {
    // luaL_loadfilex la usa para reabrir en binario tras leer la primera linea
    if (f->is_console) return f;
    free(f->buf);
    unsigned char *buf; size_t size;
    if (nemo_plat_read_file(path, &buf, &size) != 0) { f->buf = 0; f->size = 0; f->err = 1; return NULL; }
    f->buf = buf; f->size = size; f->pos = 0; f->eof = 0; f->err = 0; (void)mode;
    return f;
}
int fclose(FILE *f) { if (!f || f->is_console) return 0; free(f->buf); free(f); return 0; }
size_t fread(void *p, size_t sz, size_t n, FILE *f) {
    if (f->is_console) return 0;
    size_t want = sz * n, avail = f->size - f->pos;
    if (want > avail) { want = avail; f->eof = 1; }
    memcpy(p, f->buf + f->pos, want); f->pos += want;
    return sz ? want / sz : 0;
}
int getc(FILE *f) { if (f->is_console || f->pos >= f->size) { f->eof = 1; return EOF; } return f->buf[f->pos++]; }
int fgetc(FILE *f) { return getc(f); }
int ungetc(int c, FILE *f) { if (f->is_console || f->pos == 0) return EOF; f->pos--; f->eof = 0; return c; }
int feof(FILE *f) { return f->eof; }
int ferror(FILE *f) { return f->err; }
int fflush(FILE *f) { (void)f; return 0; }
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f) {
    if (!f->is_console || f->fd_console == 0) return 0;
    nemo_plat_write(f->fd_console, (const char *)p, sz * n);
    return n;
}
int fputs(const char *s, FILE *f) { fwrite(s, 1, strlen(s), f); return 0; }
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) { return nemo_vsnprintf(buf, cap, fmt, ap); }
int snprintf(char *buf, size_t cap, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = nemo_vsnprintf(buf, cap, fmt, ap); va_end(ap); return n; }
int sprintf(char *buf, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = nemo_vsnprintf(buf, 1 << 30, fmt, ap); va_end(ap); return n; }
int fprintf(FILE *f, const char *fmt, ...) {
    char tmp[512]; va_list ap; va_start(ap, fmt);
    int n = nemo_vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
    fwrite(tmp, 1, (size_t)n, f); return n;
}
int printf(const char *fmt, ...) {
    char tmp[512]; va_list ap; va_start(ap, fmt);
    int n = nemo_vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
    fwrite(tmp, 1, (size_t)n, stdout); return n;
}

// -- stdlib sobre el allocator y la plataforma --
// malloc/realloc/free de C: al asignador de bloques grandes, que guarda
// el tamaño en su cabecera. NO a nemo_lua_alloc: sus cajones para objetos
// pequeños necesitan que se les diga el tamaño al liberar, y free() no lo
// sabe. Ver nemo_alloc.c.
void *nemo_c_malloc(size_t n);
void *nemo_c_realloc(void *p, size_t n);
void  nemo_c_free(void *p);
void *malloc(size_t n) { return nemo_c_malloc(n); }
void *realloc(void *p, size_t n) { return nemo_c_realloc(p, n); }
void  free(void *p) { nemo_c_free(p); }
void abort(void) { nemo_plat_write(2, "abort()\n", 8); nemo_plat_exit(134); }
void exit(int c) { nemo_plat_exit(c); }
long long time(long long *t) { long long v = nemo_plat_ticks() / 1000; if (t) *t = v; return v; }
long long clock(void) { return nemo_plat_ticks(); }
char *fgets(char *s, int n, FILE *f) {
    // solo lectura desde archivo en memoria; desde consola no hay entrada de linea aqui
    if (f->is_console || f->pos >= f->size || n <= 1) return NULL;
    int i = 0;
    while (i < n - 1 && f->pos < f->size) { char c = (char)f->buf[f->pos++]; s[i++] = c; if (c == '\n') break; }
    s[i] = 0; return s;
}
