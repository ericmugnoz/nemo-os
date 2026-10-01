// nemofs.h — Nemo OS
// Sistema de archivos propio, con soporte de carpetas anidadas.
// v2: inodos proporcionales al disco al formatear (256..16384),
// archivos de hasta 8656896 bytes (doble indireccion), reserva de
// inodos/bloques con mapa en memoria. Los discos v1 se actualizan
// solos al montar, sin perder nada. Limite que sigue de v1: el bitmap
// cubre 256MB de datos como maximo. Ver la cabecera de nemofs.c.
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
bool nemofs_rename(uint32_t parent, const char *old_name, const char *new_name);
bool nemofs_delete(uint32_t parent, const char *name);

// Busca un hijo por nombre dentro de 'parent'. Devuelve su índice de
// inodo, o -1 si no existe.
int32_t nemofs_find_child(uint32_t parent, const char *name);
// Tipo de un inodo por su indice: NEMOFS_TYPE_FILE (1), NEMOFS_TYPE_DIR
// (2), o -1 si el indice no es valido. Necesaria para saber si un
// hijo devuelto por nemofs_find_child() es un archivo o una carpeta
// -- antes no habia forma de saberlo sin pasar por el listado
// completo de FILE_LIST (formato binario mixto texto+enteros, mas
// caro de leer desde un lenguaje de guion como Lua).
int32_t nemofs_type_by_inode(uint32_t idx);
// Espacio en disco -- ver la documentacion junto a la implementacion
// en nemofs.c. Cada bloque son SECTOR_SIZE (512) bytes.
void nemofs_disk_usage(uint32_t *total_blocks, uint32_t *used_blocks);
// Crecer al espacio nuevo sin reformatear, si el mapa de bloques
// reservado al formatear tiene sitio. Devuelve los bloques resultantes, o 0.
bool nemofs_puede_crecer(uint32_t nuevos_sectores, uint32_t *bloques_nuevos);
uint32_t nemofs_crecer(uint32_t nuevos_sectores);

bool nemofs_write_file(uint32_t inode, const void *buf, uint32_t size);
// Añade 'size' bytes AL FINAL de un archivo, sin tener que tener el
// archivo entero en memoria. Hasta ahora solo existia escribir el
// archivo completo de una vez: copiar un archivo de 4MB obligaba a
// reservar 4MB en el programa que copiaba. false si no cabe (el techo
// sigue siendo el mismo de nemofs_write_file) o si algo falla.
bool nemofs_append(uint32_t inode, const void *buf, uint32_t size);
// Como nemofs_write_file, pero solo escribe si el contenido cambia. Para
// lo que el kernel instala en cada arranque: ver nemofs.c.
bool nemofs_write_file_if_changed(uint32_t inode, const void *buf, uint32_t size);
int32_t nemofs_read_file(uint32_t inode, void *buf, uint32_t max_size);

// Cuanto mide un archivo, por inodo: -1 si no existe o no es un archivo.
// Para reservar un buffer del tamaño justo ANTES de leerlo.
int32_t nemofs_file_size(uint32_t inode);
// Leer por partes: hasta len bytes desde offset -> leidos, -1 si falla.
int32_t nemofs_read_at(uint32_t inode, uint32_t offset, void *buf, uint32_t len);

// Rellena 'out' con hasta 'max_count' entradas del directorio 'parent'.
// Devuelve cuántas entradas reales tiene el directorio (puede ser mayor
// que max_count si no cabían todas).
uint32_t nemofs_list_dir(uint32_t parent, nemofs_dirent_t *out, uint32_t max_count);

#endif
