// nemo_platform_hostlinux.c — plataforma de PRUEBA: Linux x86-64 sin
// libc (syscalls directas). Sirve para ejecutar Lua con la libc minima
// de Nemo OS en el host antes de tocar hardware: si aqui funciona, la
// unica diferencia con la Pi 4 son las syscalls de esta capa.
//
// SOLO Linux x86-64. En cualquier otra combinacion (macOS, Linux
// ARM64...) 'make host' no aplica -- usa el flujo real: 'make' con el
// cross-compilador ARM64, que no toca este archivo para nada.
#if !defined(__linux__) || !defined(__x86_64__)
#error "nemo_platform_hostlinux.c es solo para probar en Linux x86-64. En Mac/ARM64, usa 'make' (no 'make host') para el build real de Nemo OS."
#endif

#include "nemo_platform.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/setjmp.h"

static long lsys(long n, long a, long b, long c, long d, long e) {
    long r; register long r10 __asm__("r10") = d; register long r8 __asm__("r8") = e;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return r;
}
uint64_t nemo_plat_syscall(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)num; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return (uint64_t)-1; // no hay kernel Nemo aqui
}
void nemo_plat_write(int fd, const char *s, size_t len) { lsys(1, fd, (long)s, (long)len, 0, 0); }
void nemo_plat_log(const char *s) { size_t n = 0; while (s[n]) n++; lsys(1, 2, (long)"LOG: ", 5, 0, 0); lsys(1, 2, (long)s, (long)n, 0, 0); lsys(1, 2, (long)"\n", 1, 0, 0); }
int nemo_plat_read_file(const char *path, unsigned char **buf, size_t *size) {
    long fd = lsys(2, (long)path, 0, 0, 0, 0); if (fd < 0) return -1;
    long sz = lsys(8, fd, 0, 2, 0, 0); lsys(8, fd, 0, 0, 0, 0);   // lseek END / SET
    unsigned char *b = malloc((size_t)sz + 1); if (!b) { lsys(3, fd, 0, 0, 0, 0); return -1; }
    long got = 0; while (got < sz) { long r = lsys(0, fd, (long)(b + got), sz - got, 0, 0); if (r <= 0) break; got += r; }
    lsys(3, fd, 0, 0, 0, 0); b[got] = 0; *buf = b; *size = (size_t)got; return 0;
}
// En el host no hay carpetas por inodo (es Linux de verdad) -- el
// parametro se ignora y se comporta igual que la version normal.
int nemo_plat_read_file_en(uint32_t parent_inode, const char *name, unsigned char **buf, size_t *size) {
    (void)parent_inode;
    return nemo_plat_read_file(name, buf, size);
}
int64_t nemo_plat_ticks(void) { long ts[2]; lsys(228, 1, (long)ts, 0, 0, 0); return ts[0] * 1000 + ts[1] / 1000000; }
static char g_arg[256];
int nemo_plat_get_arg(char *buf, size_t max) { size_t n = strlen(g_arg); if (n >= max) n = max - 1; memcpy(buf, g_arg, n); buf[n] = 0; return (int)n; }
void nemo_plat_pump(void) {}
void nemo_plat_exit(int code) { lsys(60, code, 0, 0, 0, 0); for (;;); }

int nemo_lua_main(void);
extern uint64_t nemo_reloc_base;
void nemo_start_c(long *sp) {
    long argc = sp[0]; char **argv = (char **)(sp + 1);
    if (argc > 2) { char h[32]; int n = 0; uint64_t b = nemo_reloc_base; for (int i = 60; i >= 0; i -= 4) h[n++] = "0123456789abcdef"[(b >> i) & 15]; h[n++] = 10; nemo_plat_write(1, "base real: 0x", 13); nemo_plat_write(1, h, n); }
    if (argc > 1) strncpy(g_arg, argv[1], sizeof g_arg - 1);
    nemo_plat_exit(nemo_lua_main());
}
__asm__(
    ".global _start\n"
    ".section .text.start,\"ax\"\n"
    "_start:\n"
    "  mov %rsp, %rbx\n"                 /* guardar el sp original (argc/argv) */
    "  and $-16, %rsp\n"
    "  lea __ehdr_start(%rip), %rdi\n"   /* cabecera ELF: enlazada en 0, luego su direccion real ES la base */
    "  call nemo_relocate\n"             /* arreglar todos los punteros guardados */
    "  mov %rbx, %rdi\n"
    "  call nemo_start_c\n"
    "  hlt\n"
    ".text\n");


// En el anfitrion el monton no crece: la version de pruebas ya compila con
// uno de 64 MB (ver lua/Makefile). Ver nemo_alloc.c.
void *nemo_plat_pedir_memoria(uint64_t bytes) { (void)bytes; return 0; }
