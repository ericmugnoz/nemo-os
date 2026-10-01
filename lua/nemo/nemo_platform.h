// nemo_platform.h — lo unico que cambia entre correr dentro de Nemo OS
// (syscalls) y correr en el host para pruebas (Linux). Todo lo demas
// (libc minima, Lua, la libreria nemo.*) es identico.
#ifndef NEMO_PLATFORM_H
#define NEMO_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
void     nemo_plat_write(int fd, const char *s, size_t len);   // fd 1=salida, 2=error
// Una linea en el registro del sistema (en Nemo OS, la terminal
// de QEMU / el puerto serie), vaya donde vaya la consola del programa
void     nemo_plat_log(const char *s);
int      nemo_plat_read_file(const char *path, unsigned char **buf, size_t *size); // 0 ok, -1 no existe
// Como nemo_plat_read_file, pero buscando DENTRO de una carpeta
// concreta por su inodo, no solo en la raiz -- la usa nemo_main.c
// para encontrar el script cuando no vive en la raiz (ver el bug
// real corregido junto a la construccion del argumento en
// task_spawn_from_file, src/tasks.c). En el host, 'parent_inode' no
// se usa -- no hay carpetas por inodo en un Linux normal.
int      nemo_plat_read_file_en(uint32_t parent_inode, const char *name, unsigned char **buf, size_t *size);
int64_t  nemo_plat_ticks(void);
void     nemo_plat_exit(int code) __attribute__((noreturn));
int      nemo_plat_get_arg(char *buf, size_t max);              // argumento de lanzamiento
void     nemo_plat_pump(void);                                   // ceder CPU (SYS_PUMP); no-op en host
uint64_t nemo_plat_syscall(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5); // para la libreria nemo.* (x0-x5)
#endif
