// nemo_platform_nemo.c — la plataforma real: Nemo OS via syscalls.
#include "nemo_platform.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/setjmp.h"

#define SYS_GET_TICKS         2
#define SYS_GET_LAUNCH_ARG    6
#define SYS_WRITE_STRING     11
#define SYS_PUMP             14
#define SYS_FILE_OPEN        20
#define SYS_FILE_READ        21
#define SYS_FILE_SIZE_BY_NAME 81
#define VOLUME_NEMOFS         0
#define SYS_MEM_PEDIR       261

static inline uint64_t syscall5(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    register uint64_t x0 __asm__("x0") = a0; register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2; register uint64_t x3 __asm__("x3") = a3;
    register uint64_t x4 __asm__("x4") = a4; register uint64_t x8 __asm__("x8") = num;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
    return x0;
}
// Seis argumentos (x0-x5): los usa SYS_SPI. El kernel los
// recibe del marco de la excepcion.
static inline uint64_t syscall6(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    register uint64_t x0 __asm__("x0") = a0; register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2; register uint64_t x3 __asm__("x3") = a3;
    register uint64_t x4 __asm__("x4") = a4; register uint64_t x5 __asm__("x5") = a5;
    register uint64_t x8 __asm__("x8") = num;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
    return x0;
}
uint64_t nemo_plat_syscall(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return syscall6(num, a0, a1, a2, a3, a4, a5); }

void nemo_plat_write(int fd, const char *s, size_t len) {
    (void)fd; // la consola de Nemo OS no distingue salida de error
    // SYS_WRITE_STRING quiere NUL final: copiamos por trozos
    char tmp[256];
    while (len > 0) {
        size_t n = len < sizeof tmp - 1 ? len : sizeof tmp - 1;
        memcpy(tmp, s, n); tmp[n] = 0;
        syscall5(SYS_WRITE_STRING, (uint64_t)tmp, 0, 0, 0, 0);
        s += n; len -= n;
    }
}
void nemo_plat_log(const char *s) {
    syscall5(28, (uint64_t)s, 0, 0, 0, 0);    // SYS_DEBUG_LOG: siempre al puerto serie
}
int nemo_plat_read_file(const char *path, unsigned char **buf, size_t *size) {
    // comprobar primero: SYS_FILE_OPEN CREA el archivo si no existe
    int64_t sz = (int64_t)syscall5(SYS_FILE_SIZE_BY_NAME, (uint64_t)path, 0, 0, 0, 0);
    if (sz < 0) return -1;
    unsigned char *b = malloc((size_t)sz + 1);
    if (!b) return -1;
    int64_t id = (int64_t)syscall5(SYS_FILE_OPEN, (uint64_t)path, 0, VOLUME_NEMOFS, 0, 0);
    if (id < 0) { free(b); return -1; }
    int64_t got = (int64_t)syscall5(SYS_FILE_READ, (uint64_t)id, (uint64_t)b, (uint64_t)sz, VOLUME_NEMOFS, 0);
    if (got < 0) { free(b); return -1; }
    b[got] = 0; *buf = b; *size = (size_t)got;
    return 0;
}
// Version con carpeta explicita, por inodo -- SYS_FILE_OPEN si admite
// un padre concreto (a diferencia de SYS_FILE_SIZE_BY_NAME, que solo
// busca en raiz+DOCUMENTOS): se usa directamente sin comprobar el
// tamaño antes, con un buffer generoso de sobra para cualquier script
// razonable, y se recorta al tamaño real leido.
int nemo_plat_read_file_en(uint32_t parent_inode, const char *name, unsigned char **buf, size_t *size) {
    int64_t id = (int64_t)syscall5(SYS_FILE_OPEN, (uint64_t)name, (uint64_t)parent_inode, VOLUME_NEMOFS, 0, 0);
    if (id < 0) return -1;
    size_t cap = 262144; // 256 KB -- de sobra para cualquier script Lua razonable
    unsigned char *b = malloc(cap + 1);
    if (!b) return -1;
    int64_t got = (int64_t)syscall5(SYS_FILE_READ, (uint64_t)id, (uint64_t)b, (uint64_t)cap, VOLUME_NEMOFS, 0);
    if (got < 0) { free(b); return -1; }
    b[got] = 0; *buf = b; *size = (size_t)got;
    return 0;
}
int64_t nemo_plat_ticks(void) { return (int64_t)syscall5(SYS_GET_TICKS, 0, 0, 0, 0, 0); }
int nemo_plat_get_arg(char *buf, size_t max) { return (int)syscall5(SYS_GET_LAUNCH_ARG, (uint64_t)buf, (uint64_t)max, 0, 0, 0); }
void nemo_plat_pump(void) { syscall5(SYS_PUMP, 0, 0, 0, 0, 0); }

// Salida: en Nemo OS un programa termina VOLVIENDO de _start (nunca
// SYS_EXIT -- ver GUIA_EJECUCION_PROGRAMAS_NEMO_OS.md). abort()/exit()
// desde dentro de Lua saltan aqui con longjmp y nemo_start_c retorna.
static jmp_buf exit_jmp;
void nemo_plat_exit(int code) { (void)code; nemo_longjmp(exit_jmp, 1); }

int nemo_lua_main(void);
void nemo_start_c(void) {
    if (nemo_setjmp(exit_jmp) == 0) nemo_lua_main();
}

// Pila propia de 1 MB: la que da el kernel a cada tarea son 16 KB,
// insuficientes para el parser de Lua (recursion en C). El area de la
// tarea se calcula sola a partir del ELF (herramientas/pro_sizes.py):
// monton (NEMO_ALLOC_POOL_SIZE, 4 MB) + esta pila + ~0.25 MB de codigo.
static uint8_t lua_stack[1024 * 1024] __attribute__((aligned(16)));
uint8_t *const lua_stack_top = lua_stack + sizeof lua_stack;

// _start en ensamblador: se ejecuta ANTES de la reubicacion, asi que
// no puede tocar ningun dato con punteros. Pasos:
//   1. base = direccion real de _start (enlazado en 0 -> es la base)
//   2. nemo_relocate(base): arreglar todos los punteros guardados
//   3. guardar sp/x30 del kernel, cambiar a la pila de 1 MB
//   4. nemo_start_c()
//   5. restaurar sp/x30 y VOLVER al kernel (regla de Nemo OS)
__asm__(
    ".section .text.start,\"ax\"\n"
    ".global _start\n"
    "_start:\n"
    // BUG real, ya corregido: x19 es un registro que la convencion de
    // llamada obliga a conservar (callee-saved) -- el kernel llama a
    // la tarea como una funcion normal y puede tener algo vivo en x19
    // al hacerlo. La primera version lo usaba como scratch SIN
    // guardarlo, así que al volver de la tarea el kernel se
    // encontraba x19 corrompido -- x20 se guarda junto por parejas
    // (stp/ldp exigen 16 bytes), aunque no se use.
    "    stp x29, x30, [sp, #-32]!\n"
    "    stp x19, x20, [sp, #16]\n"
    "    mov x29, sp\n"
    "    adr x0, _start\n"              // direccion REAL de _start = base (enlazado en 0)
    "    bl nemo_relocate\n"
    "    mov x19, sp\n"                 // sp del kernel, a salvo en un registro YA conservado
    "    adrp x0, lua_stack_top\n"      // el puntero ya esta reubicado: ahora si se puede leer
    "    add x0, x0, :lo12:lua_stack_top\n"
    "    ldr x0, [x0]\n"
    "    mov sp, x0\n"
    "    bl nemo_start_c\n"
    "    mov sp, x19\n"
    "    ldp x19, x20, [sp, #16]\n"
    "    ldp x29, x30, [sp], #32\n"
    "    ret\n"
    ".text\n");


// Otra zona de memoria para el monton de Lua (nemo_alloc.c, fase 4): el
// kernel la reserva, la marca como de este programa y la devuelve puesta
// a cero; se libera sola al terminar. NULL si no hay sitio.
void *nemo_plat_pedir_memoria(uint64_t bytes) {
    return (void *)(uintptr_t)syscall5(SYS_MEM_PEDIR, bytes, 0, 0, 0, 0);
}
