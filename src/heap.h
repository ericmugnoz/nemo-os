// heap.h — Nemo OS
#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

void *kmalloc(size_t size);
void kfree(void *ptr);
size_t kheap_used(void);
size_t kheap_free(void);

#endif
