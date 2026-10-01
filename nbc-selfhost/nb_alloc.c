// nb_alloc.c — asignador de memoria dinámica del runtime de
// Nemo-Blitz 2.0. Vive DENTRO del espacio de memoria de cada tarea
// (los 16MB que ya reserva task_spawn_from_file), nunca pasa por
// kmalloc/kfree del kernel -- ese heap es solo para estructuras
// internas del propio kernel (ver la nota en src/heap.c). Mismo
// motivo por el que Lua tiene su propio nemo_alloc.c aparte: cada
// tarea necesita su propio montón privado, aislado de las demás y
// del kernel.
//
// Diseño: listas libres SEGREGADAS por clase de tamaño (potencias de
// 2), con fusión de bloques vecinos por dirección al liberar, y
// partición del sobrante al reservar. Es EXACTAMENTE el mismo diseño
// que src/heap.c (el heap del kernel) y lua/nemo/nemo_alloc.c (el
// heap de Lua) -- los tres comparten el mismo linaje, ya probado dos
// veces en dos contextos distintos, con su propio bug real ya
// encontrado y corregido en esa historia (ver la nota en
// nb_realloc): un realloc que crece se tragaba el bloque vecino
// ENTERO en vez de solo lo que hacía falta, y la siguiente reserva
// se quedaba sin sitio aunque hubiera espacio de sobra.
//
// Sin libc -- este archivo se compila junto al resto del compilador
// autohospedado, en el mismo entorno sin runtime de C que el resto
// de Nemo OS.
//
// Es la base de la sección 2/4 del diseño (DISENO_NEMO_BLITZ_2.md):
// cadenas e instancias de Type reservan aquí, con conteo de
// referencias por encima (en nb_string.c / el codegen de Type, no en
// este archivo -- este archivo solo sabe reservar y liberar bytes,
// no sabe nada de cadenas ni de tipos).

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Tamaño del montón privado de cada tarea. Punto de partida: la
// tarea tiene 16MB en total (código compilado + .bss + pila +
// este montón) -- un programa Nemo-Blitz típico compilado ocupa
// unos pocos KB de código y .bss, así que 8MB deja margen de sobra
// sin acercarse al límite. Ajustable con -D si algún programa real
// necesita más una vez que haya programas reales que probar.
#ifndef NB_ALLOC_POOL_SIZE
// 2MB, no 8MB: el cargador real de Nemo OS (src/loader.c) copia
// exactamente 'code_size' bytes desde un bufer de lectura de solo
// 6MB+4096 (LOADER_FILE_BUF_SIZE) -- no existe ningun mecanismo tipo
// "reservar memoria sin ocupar espacio en el archivo" (como el
// p_memsz > p_filesz de un ELF real): el bucle de copia de
// loader_load_into() lee 'code_size' bytes del archivo SIN
// distincion, asi que cualquier .bss embebido en el .pro cuenta como
// espacio real en disco Y en ese bufer de lectura. Un monton de 8MB
// haria que NINGUN programa con cadenas, arrays o Type pudiera
// cargar jamas (superaria el bufer de lectura el solo). 2MB deja de
// sobra espacio para un programa BASIC normal, sin acercarse al
// limite.
#define NB_ALLOC_POOL_SIZE (2u * 1024u * 1024u)
#endif

typedef struct nb_block {
    uint32_t size;                   // bytes utiles tras la cabecera
    uint32_t free;
    struct nb_block *prev, *next;    // orden de direccion (para fusionar)
    struct nb_block *fprev, *fnext;  // lista libre de su clase (solo si free)
} nb_block_t;

#define NB_HDR_SIZE   ((uint32_t)sizeof(nb_block_t))
#define NB_ALIGN16(n) (((n) + 15u) & ~15u)
#define NB_MIN_SPLIT  (NB_HDR_SIZE + 16u)
#define NB_NUM_CLASSES 32

static uint8_t nb_pool[NB_ALLOC_POOL_SIZE] __attribute__((aligned(16)));
static nb_block_t *nb_head = NULL;
static nb_block_t *nb_bins[NB_NUM_CLASSES];

static inline int nb_class_of(uint32_t size) {
    // clase k contiene bloques con 2^k <= size < 2^(k+1) (en unidades
    // de 16 bytes: la clase 0 son bloques de 16-31 bytes)
    int k = 0; uint32_t s = size >> 4;
    while (s > 1 && k < NB_NUM_CLASSES - 1) { s >>= 1; k++; }
    return k;
}
static inline void *nb_block_mem(nb_block_t *b) { return (uint8_t *)b + NB_HDR_SIZE; }
static inline nb_block_t *nb_mem_block(void *p) { return (nb_block_t *)((uint8_t *)p - NB_HDR_SIZE); }

static void nb_bin_push(nb_block_t *b) {
    int k = nb_class_of(b->size);
    b->fprev = NULL; b->fnext = nb_bins[k];
    if (nb_bins[k]) nb_bins[k]->fprev = b;
    nb_bins[k] = b; b->free = 1;
}
static void nb_bin_remove(nb_block_t *b) {
    int k = nb_class_of(b->size);
    if (b->fprev) b->fprev->fnext = b->fnext; else nb_bins[k] = b->fnext;
    if (b->fnext) b->fnext->fprev = b->fprev;
    b->fprev = b->fnext = NULL; b->free = 0;
}

static void nb_init_pool(void) {
    for (int i = 0; i < NB_NUM_CLASSES; i++) nb_bins[i] = NULL;
    nb_head = (nb_block_t *)nb_pool;
    nb_head->size = NB_ALLOC_POOL_SIZE - NB_HDR_SIZE;
    nb_head->prev = nb_head->next = NULL;
    nb_bin_push(nb_head);
}

// nb_alloc: reserva al menos 'n' bytes utiles. Devuelve NULL si el
// montón privado de la tarea está agotado -- el llamador (nb_string.c,
// el codegen de New) decide qué hacer (para cadenas/Type, esto se
// traduce en un error de "memoria agotada" en tiempo de ejecución,
// nunca en escribir fuera de los límites del montón).
void *nb_alloc(uint32_t n) {
    if (nb_head == NULL) nb_init_pool();
    if (n < 16) n = 16;
    n = NB_ALIGN16(n);
    for (int k = nb_class_of(n); k < NB_NUM_CLASSES; k++) {
        int tries = 0;
        for (nb_block_t *b = nb_bins[k]; b && tries < 8; b = b->fnext, tries++) {
            if (b->size < n) continue;
            nb_bin_remove(b);
            if (b->size - n >= NB_MIN_SPLIT) {
                nb_block_t *rest = (nb_block_t *)((uint8_t *)nb_block_mem(b) + n);
                rest->size = b->size - n - NB_HDR_SIZE;
                rest->prev = b; rest->next = b->next;
                if (b->next) b->next->prev = rest;
                b->next = rest; b->size = n;
                nb_bin_push(rest);
            }
            return nb_block_mem(b);
        }
    }
    return NULL;
}

void nb_free(void *p) {
    if (p == NULL) return;
    nb_block_t *b = nb_mem_block(p);
    if (b->next && b->next->free) {            // fusionar con el siguiente
        nb_block_t *nx = b->next; nb_bin_remove(nx);
        b->size += NB_HDR_SIZE + nx->size; b->next = nx->next;
        if (nx->next) nx->next->prev = b;
    }
    if (b->prev && b->prev->free) {            // fusionar con el anterior
        nb_block_t *pv = b->prev; nb_bin_remove(pv);
        pv->size += NB_HDR_SIZE + b->size; pv->next = b->next;
        if (b->next) b->next->prev = pv;
        b = pv;
    }
    nb_bin_push(b);
}

void *nb_realloc(void *p, uint32_t nsize) {
    if (p == NULL) return nb_alloc(nsize);
    nb_block_t *b = nb_mem_block(p);
    if (b->size >= nsize) return p;
    // intentar crecer sobre el vecino libre sin copiar
    if (b->next && b->next->free && b->size + NB_HDR_SIZE + b->next->size >= nsize) {
        nb_block_t *nx = b->next; nb_bin_remove(nx);
        b->size += NB_HDR_SIZE + nx->size; b->next = nx->next;
        if (nx->next) nx->next->prev = b;
        // Mismo bug que ya se encontró y corrigió en nemo_alloc.c (el
        // asignador de Lua, mismo linaje): absorber el vecino ENTERO
        // aunque solo hicieran falta unos pocos bytes se tragaba el
        // montón entero con el tiempo. Devolver el exceso a la lista
        // libre, igual que hace nb_alloc.
        uint32_t n = NB_ALIGN16(nsize);
        if (b->size - n >= NB_MIN_SPLIT) {
            nb_block_t *rest = (nb_block_t *)((uint8_t *)nb_block_mem(b) + n);
            rest->size = b->size - n - NB_HDR_SIZE;
            rest->prev = b; rest->next = b->next;
            if (b->next) b->next->prev = rest;
            b->next = rest; b->size = n;
            nb_bin_push(rest);
        }
        return p;
    }
    void *q = nb_alloc(nsize);
    if (q == NULL) return NULL;
    uint32_t copy = b->size < nsize ? b->size : nsize;
    uint64_t *d = (uint64_t *)q; const uint64_t *s = (const uint64_t *)p;
    for (uint32_t i = 0; i < copy / 8; i++) d[i] = s[i];
    nb_free(p);
    return q;
}

// Para diagnóstico (una futura MemoryUsed() del lenguaje, o para
// depurar durante el propio desarrollo del runtime).
uint32_t nb_alloc_used(void) {
    if (nb_head == NULL) return 0;
    uint32_t used = 0;
    for (nb_block_t *b = nb_head; b; b = b->next) if (!b->free) used += b->size + NB_HDR_SIZE;
    return used;
}
