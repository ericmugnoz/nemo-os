// heap.c — Nemo OS
//
// SEGUNDA VERSIÓN de kmalloc/kfree. La primera era un "bump
// allocator": un puntero que solo avanza, y kfree() no hacía nada en
// absoluto -- documentado en su momento como una limitación real y
// consciente, para cuando hiciera falta gestión de procesos de
// verdad. Ese momento ha llegado: es el primer prerrequisito del
// roadmap hacia multiprocesador (antes de tocar SMP hace falta poder
// reservar y liberar memoria de verdad para colas y estructuras por
// núcleo, sin agotar el heap fijo del kernel).
//
// El diseño es el MISMO que ya se construyó y se probó a fondo para
// el asignador de Lua (nemo_alloc.c) -- listas libres SEGREGADAS por
// clase de tamaño (potencias de 2), con fusión de bloques vecinos por
// dirección al liberar, y partición del sobrante al reservar. Ese
// diseño ya pasó por su propia primera versión ingenua (first-fit
// recorriendo todos los bloques, O(n) por reserva) y su propio bug
// real (un realloc que se tragaba el vecino entero sin devolver el
// exceso) -- aquí se parte directamente de la versión ya corregida,
// en vez de repetir el mismo camino dos veces.
//
// Nota importante para quien lea esto pensando en el port de Lua: el
// heap del kernel y el pool de memoria de Lua (nemo_alloc.c) son DOS
// cosas completamente separadas -- Lua vive en su propio array
// estático de 12MB dentro del espacio de cada tarea, y nunca pasa por
// kmalloc/kfree. Este heap es solo para estructuras internas del
// kernel.

#include <stdint.h>
#include <stdbool.h>
#include "heap.h"

#define HEAP_SIZE (64UL * 1024 * 1024) // 64MB de heap para el kernel -- ampliado de 16MB (roadmap: el
                                        // mapa de bloques libres de NemoFS ahora se reserva aqui, de
                                        // tamaño proporcional al disco -- ver nemofs.c)

typedef struct block {
    uint32_t size;                // bytes utiles tras la cabecera
    uint32_t free;
    struct block *prev, *next;    // orden de direccion (para fusionar)
    struct block *fprev, *fnext;  // lista libre de su clase (solo si free)
} block_t;

#define HDR_SIZE   ((uint32_t)sizeof(block_t))
#define ALIGN16(n) (((n) + 15u) & ~15u)
#define MIN_SPLIT  (HDR_SIZE + 16u)
#define NUM_CLASSES 32

// El heap sigue viviendo dentro de la propia imagen del kernel
// (.bss), igual que antes -- no depende de detectar RAM física.
static uint8_t heap[HEAP_SIZE] __attribute__((aligned(16)));
static block_t *head = NULL;
static block_t *bins[NUM_CLASSES];

static inline int class_of(uint32_t size) {
    // clase k contiene bloques con 2^k <= size < 2^(k+1) (en unidades
    // de 16 bytes: la clase 0 son bloques de 16-31 bytes)
    int k = 0; uint32_t s = size >> 4;
    while (s > 1 && k < NUM_CLASSES - 1) { s >>= 1; k++; }
    return k;
}
static inline void *block_mem(block_t *b) { return (uint8_t *)b + HDR_SIZE; }
static inline block_t *mem_block(void *p) { return (block_t *)((uint8_t *)p - HDR_SIZE); }

static void bin_push(block_t *b) {
    int k = class_of(b->size);
    b->fprev = NULL; b->fnext = bins[k];
    if (bins[k]) bins[k]->fprev = b;
    bins[k] = b; b->free = 1;
}
static void bin_remove(block_t *b) {
    int k = class_of(b->size);
    if (b->fprev) b->fprev->fnext = b->fnext; else bins[k] = b->fnext;
    if (b->fnext) b->fnext->fprev = b->fprev;
    b->fprev = b->fnext = NULL; b->free = 0;
}

static void init_heap(void) {
    for (int i = 0; i < NUM_CLASSES; i++) bins[i] = NULL;
    head = (block_t *)heap;
    head->size = HEAP_SIZE - HDR_SIZE;
    head->prev = head->next = NULL;
    bin_push(head);
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    if (head == NULL) init_heap();

    uint32_t n = (uint32_t)size;
    if (n < 16) n = 16;
    n = ALIGN16(n);

    // Buscar desde la clase minima que puede contener n; dentro de
    // una clase los bloques pueden ser menores que n (misma potencia
    // de 2), asi que se miran unos pocos y si no encaja, la clase
    // siguiente -- igual que en nemo_alloc.c, mismo razonamiento.
    for (int k = class_of(n); k < NUM_CLASSES; k++) {
        int tries = 0;
        for (block_t *b = bins[k]; b && tries < 8; b = b->fnext, tries++) {
            if (b->size < n) continue;
            bin_remove(b);
            if (b->size - n >= MIN_SPLIT) {
                block_t *rest = (block_t *)((uint8_t *)block_mem(b) + n);
                rest->size = b->size - n - HDR_SIZE;
                rest->prev = b; rest->next = b->next;
                if (b->next) b->next->prev = rest;
                b->next = rest; b->size = n;
                bin_push(rest);
            }
            return block_mem(b);
        }
    }
    return NULL; // heap agotado de verdad, no solo fragmentado
}

void kfree(void *ptr) {
    if (ptr == NULL) return;
    block_t *b = mem_block(ptr);

    if (b->next && b->next->free) {            // fusionar con el siguiente
        block_t *nx = b->next; bin_remove(nx);
        b->size += HDR_SIZE + nx->size; b->next = nx->next;
        if (nx->next) nx->next->prev = b;
    }
    if (b->prev && b->prev->free) {            // fusionar con el anterior
        block_t *pv = b->prev; bin_remove(pv);
        pv->size += HDR_SIZE + b->size; pv->next = b->next;
        if (b->next) b->next->prev = pv;
        b = pv;
    }
    bin_push(b);
}

size_t kheap_used(void) {
    if (head == NULL) return 0;
    size_t used = 0;
    for (block_t *b = head; b; b = b->next) if (!b->free) used += b->size + HDR_SIZE;
    return used;
}

size_t kheap_free(void) {
    return HEAP_SIZE - kheap_used();
}
