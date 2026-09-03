// heap.c — Nemo OS
//
// Primera versión de kmalloc/kfree: un "bump allocator". Es el asignador
// más simple que existe -- un puntero que solo avanza. Cada kmalloc()
// mueve el puntero hacia adelante y devuelve el trozo; kfree() de momento
// no hace nada, porque un bump allocator no sabe reclamar memoria
// individual (solo sabe "todo o nada").
//
// Es una limitación real y consciente: cuando lleguemos a gestión de
// procesos (donde sí necesitamos crear y destruir muchas cosas
// repetidamente) sustituiremos esto por un asignador más completo,
// probablemente basado en listas libres o un buddy allocator.
// Documentar esto aquí es justo el tipo de decisión que vale la pena
// para el libro: empezamos simple a propósito, y explicamos por qué.

#include <stdint.h>
#include "heap.h"

#define HEAP_SIZE (16UL * 1024 * 1024) // 16MB de heap para el kernel

// El heap vive dentro de la propia imagen del kernel (.bss), así que no
// dependemos todavía de saber cuánta RAM física tiene la máquina.
static uint8_t heap[HEAP_SIZE] __attribute__((aligned(16)));
static size_t heap_offset = 0;

void *kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    // Alineamos cada asignación a 16 bytes -- un compromiso razonable
    // para la mayoría de estructuras del kernel.
    size_t aligned_size = (size + 15) & ~((size_t)15);

    if (heap_offset + aligned_size > HEAP_SIZE) {
        return NULL; // Sin memoria disponible
    }

    void *ptr = &heap[heap_offset];
    heap_offset += aligned_size;
    return ptr;
}

void kfree(void *ptr) {
    // Ver el comentario de cabecera: un bump allocator no libera memoria
    // individual. Se deja la función ya definida para no tener que tocar
    // el resto del kernel cuando implementemos un asignador de verdad.
    (void)ptr;
}

size_t kheap_used(void) {
    return heap_offset;
}

size_t kheap_free(void) {
    return HEAP_SIZE - heap_offset;
}
