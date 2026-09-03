// nemofs.h — Nemo OS
// Sistema de archivos propio, con soporte de carpetas anidadas.
#ifndef NEMOFS_H
#define NEMOFS_H

#include <stdint.h>
#include <stdbool.h>

#define NEMOFS_MAX_NAME 31 // + 1 byte para el terminador nulo
#define NEMOFS_TYPE_FREE 0
#define NEMOFS_TYPE_FILE 1
#define NEMOFS_TYPE_DIR  2

#define NEMOFS_ROOT_INODE 0

typedef struct {
    uint32_t inode;
    uint8_t type;
    char name[NEMOFS_MAX_NAME + 1];
    uint32_t size;
} nemofs_dirent_t;

bool nemofs_mount(void);

// Crea un archivo o carpeta dentro de 'parent'. Devuelve el índice del
// nuevo inodo, o -1 si falla (nombre duplicado, sin espacio, etc.)
int32_t nemofs_create(uint32_t parent, const char *name, uint8_t type);

// Borra un archivo o una carpeta VACIA de 'parent'. No soporta borrar
// carpetas con contenido dentro todavia (habria que hacerlo
// recursivamente). Devuelve false si no existe, si es una carpeta no
// vacia, o si algo falla al escribir en disco.
bool nemofs_delete(uint32_t parent, const char *name);

// Busca un hijo por nombre dentro de 'parent'. Devuelve su índice de
// inodo, o -1 si no existe.
int32_t nemofs_find_child(uint32_t parent, const char *name);

bool nemofs_write_file(uint32_t inode, const void *buf, uint32_t size);
int32_t nemofs_read_file(uint32_t inode, void *buf, uint32_t max_size);

// Rellena 'out' con hasta 'max_count' entradas del directorio 'parent'.
// Devuelve cuántas entradas reales tiene el directorio (puede ser mayor
// que max_count si no cabían todas).
uint32_t nemofs_list_dir(uint32_t parent, nemofs_dirent_t *out, uint32_t max_count);

#endif
