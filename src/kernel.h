// kernel.h — Nemo OS
//
// Accesor minimo para que otros archivos (tasks.c, entre otros)
// puedan consultar el inodo de la carpeta PROGRAMAS sin duplicar la
// busqueda ni exponer la variable estatica de kernel.c directamente.
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

// Inodo de /PROGRAMAS, resuelto una vez en el arranque (ver
// g_programas_dir en kernel.c). NEMOFS_ROOT_INODE si por lo que sea
// no se encontro la carpeta al arrancar.
uint32_t kernel_get_programas_dir(void);
// Inodo de /ACCESORIOS (programas escritos en Lua). tasks.c lo usa
// como carpeta de respaldo al lanzar un .lua que no esta en la carpeta
// actual -- asi 'run reloj.lua' funciona desde cualquier sitio.
uint32_t kernel_get_accesorios_dir(void);

#endif
