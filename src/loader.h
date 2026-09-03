// loader.h — Nemo OS
#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>
#include <stdbool.h>

// Carga un archivo .pro desde NemoFS en 'dest' (de tamaño 'dest_size'),
// y devuelve en '*out_entry' un puntero a funcion listo para llamar.
// No lo ejecuta -- lo usa tasks.c para crear tareas independientes,
// cada una con su propia area de codigo.
bool loader_load_into(const char *filename, uint32_t parent_inode,
                       uint8_t *dest, uint32_t dest_size,
                       void (**out_entry)(void));

// Instala el programa de prueba embebido en el kernel como
// "/hello.pro" dentro de NemoFS. Sirve para tener algo real que cargar
// sin depender todavia de herramientas externas de desarrollo.
bool loader_install_embedded_test(void);
bool loader_install_embedded_syscall_test(uint32_t parent_inode);
bool loader_install_embedded_shell(uint32_t parent_inode);
bool loader_install_embedded_explorer(uint32_t parent_inode);
bool loader_install_embedded_editor(uint32_t parent_inode);
bool loader_install_embedded_gadgetdemo(uint32_t parent_inode);
bool loader_install_embedded_ide(uint32_t parent_inode);
bool loader_install_embedded_nbc(uint32_t parent_inode);

// Carga y ejecuta un archivo .pro desde NemoFS. El programa corre en
// modo kernel (EL1) -- todavia no hay aislamiento de procesos.
// Carga y ejecuta un archivo .pro desde NemoFS, asociandolo a una
// ventana concreta -- asi sus syscalls de graficos (SYS_DRAW_RECT,
// SYS_DRAW_TEXT) saben en que ventana dibujar. Pasa window_idx=-1
// para programas que no usan graficos (como el hello.pro original).
bool loader_run_in_window(const char *filename, uint32_t parent_inode, int32_t window_idx);

bool loader_run_from_nemofs(const char *filename, uint32_t parent_inode);

#endif
