// nb_syscalls.h — wrappers de las syscalls de Nemo OS que necesita
// el compilador/ensamblador autohospedado (nbc.pro). Mismo patron que
// usan shell.c/explorer.c/editor.c: syscall5(numero, a0..a4).

#ifndef NB_SYSCALLS_H
#define NB_SYSCALLS_H

#include <stdint.h>

#define SYS_EXIT             0
#define SYS_LAUNCH_PROGRAM   5
#define SYS_GET_LAUNCH_ARG   6
#define SYS_WRITE_STRING    11
#define SYS_PUMP            14
#define SYS_FILE_OPEN       20
#define SYS_FILE_READ       21
#define SYS_FILE_WRITE      22
#define SYS_FILE_LIST       23
#define SYS_DIR_CREATE      24
#define SYS_FILE_SIZE_BY_NAME 81
#define SYS_FILE_TYPE_BY_NAME 82
#define SYS_FIND_CHILD        83

#define VOLUME_NEMOFS 0
#define VOLUME_FAT    1

#define NEMOFS_ROOT_INODE 0

static inline uint64_t syscall5(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    register uint64_t x4 __asm__("x4") = a4;
    register uint64_t x8 __asm__("x8") = num;
    __asm__ volatile("svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
        : "memory");
    return x0;
}

static inline void nb_write_string(const char *s) {
    syscall5(SYS_WRITE_STRING, (uint64_t)s, 0, 0, 0, 0);
}

// Devuelve el identificador del archivo (inodo o handle FAT), o
// (uint64_t)-1 si no se pudo abrir/crear.
static inline int64_t nb_file_open(const char *name, uint32_t parent_inode, uint32_t volume) {
    return (int64_t)syscall5(SYS_FILE_OPEN, (uint64_t)name, parent_inode, volume, 0, 0);
}

static inline int64_t nb_file_read(int64_t handle, void *buf, uint32_t max_size, uint32_t volume) {
    return (int64_t)syscall5(SYS_FILE_READ, (uint64_t)handle, (uint64_t)buf, max_size, volume, 0);
}

static inline int64_t nb_file_write(int64_t handle, const void *buf, uint32_t size, uint32_t volume) {
    return (int64_t)syscall5(SYS_FILE_WRITE, (uint64_t)handle, (uint64_t)buf, size, volume, 0);
}

static inline int64_t nb_file_list(uint32_t parent_inode, void *buf, uint32_t max_entries, uint32_t volume) {
    return (int64_t)syscall5(SYS_FILE_LIST, parent_inode, (uint64_t)buf, max_entries, volume, 0);
}

// Las tres de abajo aceptan RUTAS con carpetas ("JUEGOS/NEMO/utiles.nb"), y
// entonces el kernel la camina el. Es la unica forma de resolver una ruta sin
// crear nada: nb_file_open crea el archivo si no existe, y eso convertiria un
// Include mal escrito en un archivo vacio compilado en silencio.

// Inodo de un nombre o de una ruta, o -1. Con ruta, 'padre' se ignora.
static inline int64_t nb_find_child(const char *name, uint32_t padre) {
    return (int64_t)syscall5(SYS_FIND_CHILD, (uint64_t)name, padre, 0, 0, 0);
}

// 0 = no existe, 1 = archivo, 2 = carpeta.
static inline int64_t nb_file_type(const char *name) {
    return (int64_t)syscall5(SYS_FILE_TYPE_BY_NAME, (uint64_t)name, 0, 0, 0, 0);
}

// Tamaño en bytes, o -1 si no existe.
static inline int64_t nb_file_size(const char *name) {
    return (int64_t)syscall5(SYS_FILE_SIZE_BY_NAME, (uint64_t)name, 0, 0, 0, 0);
}

static inline uint32_t nb_get_launch_arg(char *buf, uint32_t max_size) {
    return (uint32_t)syscall5(SYS_GET_LAUNCH_ARG, (uint64_t)buf, max_size, 0, 0, 0);
}

static inline void nb_exit(void) {
    syscall5(SYS_EXIT, 0, 0, 0, 0, 0);
}

// A diferencia de un bucle vacio (que bloquearia TODO el sistema,
// ya que nuestro planificador es cooperativo y nadie mas puede
// actuar hasta que cedamos el turno), esto SI deja avanzar al resto
// de tareas. Se usa despues de SYS_EXIT, cuando ya no queda nada mas
// que hacer pero el programa aun no ha "vuelto" del todo.
static inline void nb_pump(void) {
    syscall5(SYS_PUMP, 0, 0, 0, 0, 0);
}

#endif
