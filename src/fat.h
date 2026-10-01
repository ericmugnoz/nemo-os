// fat.h — Nemo OS
// Driver FAT16/32 para interoperar con Mac/Windows/Linux. Soporta
// nombres largos (VFAT/LFN) ademas de los cortos 8.3, permite
// sobrescribir y borrar archivos, y navegar subcarpetas (listar y
// abrir archivos dentro de ellas -- crear/escribir/borrar DENTRO de
// una subcarpeta sigue limitado a la raiz por ahora).
#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include <stdbool.h>

#define FAT_NAME_LEN 64 // nombre largo (LFN) + terminador, con margen de sobra

typedef struct {
    char name[FAT_NAME_LEN];
    bool is_dir;
    uint32_t size;
    uint32_t first_cluster;
} fat_dirent_t;

// Monta (o formatea si no hay un FAT valido) el disco indicado.
bool fat_mount(uint8_t disk_index);

// Version general: lista/busca en CUALQUIER directorio, dado su
// primer cluster ('cluster == 0' significa "la raiz" -- funciona
// igual en FAT16 (donde la raiz es una region de tamaño fijo) que en
// FAT32 (donde la raiz es una cadena de clusters normal, resuelta
// internamente); un cluster distinto de 0 es SIEMPRE una subcarpeta
// real, en cualquiera de los dos formatos.
uint32_t fat_list_dir(uint32_t cluster, fat_dirent_t *out, uint32_t max_count);
bool fat_find_in_dir(uint32_t cluster, const char *name, fat_dirent_t *out);

// Atajos que operan sobre la raiz -- por compatibilidad con el codigo
// que ya los usaba antes de que existiera navegacion de subcarpetas.
uint32_t fat_list_root(fat_dirent_t *out, uint32_t max_count);
bool fat_find_root(const char *name, fat_dirent_t *out);

bool fat_read_file(const fat_dirent_t *entry, void *buf, uint32_t max_size, uint32_t *out_size);

// ---- Leer por partes (fase 5 de la memoria) ----
// Llegar a una posicion de un archivo FAT exige recorrer su cadena de
// clusters desde el primero. El cursor recuerda el ultimo cluster
// visitado y su numero de orden: leer seguido nunca retrocede, y el coste
// total de leer un archivo entero a trozos es lineal, no cuadratico.
// Un cursor a cero ({0, 0}) se inicializa solo.
typedef struct { uint32_t cluster; uint32_t indice; } fat_cursor_t;

// Lee hasta 'len' bytes desde 'offset'. *out_size = leidos (0 si offset
// esta en el final o mas alla). false si falla el disco o la cadena.
bool fat_read_at(const fat_dirent_t *entry, fat_cursor_t *cursor, uint32_t offset,
                 void *buf, uint32_t len, uint32_t *out_size);

// Crea un archivo NUEVO -- falla si ya existe (usa fat_write_file si
// quieres sobrescribir sin comprobarlo tu mismo antes).
bool fat_create_file(const char *name, const void *buf, uint32_t size);

// Escribe un archivo, sobrescribiendo su contenido si ya existia (lo
// borra primero y lo vuelve a crear limpio) o creandolo si no.
bool fat_write_file(const char *name, const void *buf, uint32_t size);

// Borra un archivo (y sus entradas de nombre largo, si las tenia) y
// libera su cadena de clusters. No borra carpetas.
bool fat_delete_file(const char *name);

// Escribir POR PARTES (fase 5b): añade len bytes al final de 'name' (en
// la raiz; debe existir -- vale uno de 0 bytes). 'fin' es un cursor al
// ultimo cluster que el llamador conserva entre llamadas ({0,0} =
// desconocido). *escritos = añadidos de verdad; false si falla o se llena.
bool fat_append(const char *name, fat_cursor_t *fin, const void *buf, uint32_t len, uint32_t *escritos);

#endif
