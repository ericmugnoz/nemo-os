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

// Limpia la cache de datos y refresca la de instrucciones para
// [addr, addr+size). Obligatorio despues de escribir codigo en
// memoria y antes de ejecutarlo. La usa tasks.c para el stub de
// salida que copia al area de cada tarea.
void sync_icache(void *addr, uint32_t size);

// Instala el programa de prueba embebido en el kernel como
// "/hello.pro" dentro de NemoFS. Sirve para tener algo real que cargar
// sin depender todavia de herramientas externas de desarrollo.
bool loader_install_embedded_test(void);
bool loader_install_embedded_syscall_test(uint32_t parent_inode);
bool loader_install_embedded_shell(uint32_t parent_inode);
bool loader_install_embedded_explorer(uint32_t parent_inode);
bool loader_install_embedded_desktoped(uint32_t parent_inode);
bool loader_install_embedded_screensettings(uint32_t parent_inode);
bool loader_install_embedded_editor(uint32_t parent_inode);
bool loader_install_embedded_gadgetdemo(uint32_t parent_inode);
bool loader_install_embedded_ide(uint32_t parent_inode);
bool loader_install_embedded_nbc(uint32_t parent_inode);

// Instala un blob de codigo crudo (sin cabecera) como un .pro en
// NemoFS, añadiendole la cabecera NEXE. Sobreescribe si ya existe,
// igual que los programas del sistema -- asi el archivo en disco
// siempre corresponde a la version embebida en el kernel.
bool loader_install_pro_blob(const uint8_t *code_start, const uint8_t *code_end,
                             uint32_t parent_inode, const char *filename, const char *label,
                             uint32_t mem_size);

// Camino "sincrono" antiguo: carga un .pro en un buffer propio del
// kernel y lo ejecuta como una llamada normal, en EL1. Hoy solo lo usa
// el test de arranque de kernel_main (hello.pro). Las tareas de
// verdad van por tasks.c, que desde la Fase 1 de la separacion
// kernel/programas las ejecuta en EL0 sobre su propia area de
// programa (la unica memoria que EL0 puede tocar, ver mmu_pi4.c).
// Asocia el programa a una ventana concreta -- asi sus syscalls de
// graficos (SYS_DRAW_RECT, SYS_DRAW_TEXT) saben en que ventana
// dibujar. Pasa window_idx=-1 para programas sin graficos.
bool loader_run_in_window(const char *filename, uint32_t parent_inode, int32_t window_idx);

bool loader_run_from_nemofs(const char *filename, uint32_t parent_inode);

#endif

// Memoria que declara necesitar un programa (cabecera version 3), o 0
// si no la declara. Lee solo la cabecera. Ver loader.c.
uint32_t loader_required_size(const char *filename, uint32_t parent_inode);
