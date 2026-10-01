// nemo_alloc.c — allocator para el port de Lua a Nemo OS (version 2).
//
// La v1 era first-fit recorriendo TODOS los bloques (ocupados
// incluidos) en cada malloc: O(n) por reserva, O(n^2) en total. Con
// los miles de objetos vivos de un script Lua normal, 3 rondas de GC
// tardaban 4 segundos y una recursion infinita no terminaba nunca.
//
// v2: listas libres SEGREGADAS por clase de tamaño (potencias de 2).
// malloc toma el primer bloque de la primera clase no vacia que cabe:
// O(1) en el caso tipico. free fusiona con los vecinos por direccion
// (para eso cada bloque conserva prev/next por direccion) y encola el
// resultado en su clase. Misma interfaz lua_Alloc de siempre.
//
// Sin libc. Mismo archivo para host y Nemo OS.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef NEMO_ALLOC_POOL_SIZE
#define NEMO_ALLOC_POOL_SIZE (12u * 1024u * 1024u)
#endif

typedef struct block {
    uint32_t size;            // bytes utiles tras la cabecera
    uint32_t free;
    struct block *prev, *next;   // orden de direccion (para fusionar)
    struct block *fprev, *fnext; // lista libre de su clase (solo si free)
} block_t;

#define HDR_SIZE  ((uint32_t)sizeof(block_t))   // 48 bytes
#define ALIGN16(n) (((n) + 15u) & ~15u)
#define MIN_SPLIT (HDR_SIZE + 16u)
#define NUM_CLASSES 32

static uint8_t pool[NEMO_ALLOC_POOL_SIZE] __attribute__((aligned(16)));
static block_t *head = NULL;

// ---- v4: EL MONTON CRECE (fase 4 de la memoria,) ----
//
// 'pool' es solo la zona INICIAL. Cuando no queda sitio, el asignador pide
// otra zona al sistema (nemo_plat_pedir_memoria: SYS_MEM_PEDIR en Nemo
// OS) y sigue. Asi un programa de Lua pequeño se queda pequeño y uno
// grande crece hasta donde haya memoria, en vez de un tamaño fijo para
// todos.
//
// Cada zona tiene SU PROPIA cadena de bloques por direccion (prev/next):
// las zonas no son contiguas entre si, y enlazarlas en una sola cadena
// haria que free() "fusionara" bloques que no estan juntos en memoria.
// Las listas de libres por tamaño (bins) si son comunes a todas.
#define MAX_ZONAS 32                              // las mismas que admite el kernel
#define ZONA_MIN  (8u * 1024u * 1024u)
static block_t *zonas[MAX_ZONAS];                 // primer bloque de cada zona
static int num_zonas = 0;
static uint64_t bytes_zonas = 0;                  // tamaño total del monton

// La da cada plataforma: Nemo OS pide una zona al kernel; el anfitrion
// puede devolver NULL (sin crecimiento).
void *nemo_plat_pedir_memoria(uint64_t bytes);
static block_t *bins[NUM_CLASSES];

static inline int class_of(uint32_t size) {
    // clase k contiene bloques con 2^k <= size < 2^(k+1)
    int k = 0; uint32_t s = size >> 4;   // /16: la clase 0 son bloques de 16-31 bytes
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

static void anadir_zona(void *mem, uint64_t tam) {
    block_t *b = (block_t *)mem;
    b->size = (uint32_t)(tam - HDR_SIZE);
    b->prev = b->next = NULL;                     // cadena propia de esta zona
    zonas[num_zonas++] = b;
    bytes_zonas += tam;
    bin_push(b);
}

static void init_pool(void) {
    for (int i = 0; i < NUM_CLASSES; i++) bins[i] = NULL;
    head = (block_t *)pool;
    anadir_zona(pool, NEMO_ALLOC_POOL_SIZE);
}

// Pide otra zona con sitio para un bloque de n bytes. La zona crece con
// el monton (al menos la mitad de lo que ya hay, y nunca menos de 8 MB),
// para que un programa que necesite mucho no agote las 32 zonas.
static bool crecer(uint32_t n) {
    if (num_zonas >= MAX_ZONAS) return false;
    uint64_t tam = bytes_zonas / 2;
    if (tam < ZONA_MIN) tam = ZONA_MIN;
    uint64_t hace_falta = (uint64_t)n + 2u * HDR_SIZE + 64u;
    if (tam < hace_falta) tam = hace_falta;
    if (tam > 0xF0000000ULL) return false;        // block_t.size es de 32 bits
    tam = (tam + (2u * 1024u * 1024u) - 1u) & ~(uint64_t)((2u * 1024u * 1024u) - 1u);
    void *mem = nemo_plat_pedir_memoria(tam);
    if (mem == NULL) return false;
    anadir_zona(mem, tam);
    return true;
}

static void *nemo_malloc_en_bins(uint32_t n);

static void *nemo_malloc(uint32_t n) {
    if (head == NULL) init_pool();
    if (n < 16) n = 16;
    n = ALIGN16(n);
    void *p = nemo_malloc_en_bins(n);
    if (p == NULL && crecer(n)) p = nemo_malloc_en_bins(n);
    return p;
}

static void *nemo_malloc_en_bins(uint32_t n) {
    // buscar desde la clase minima que puede contener n; dentro de una
    // clase los bloques pueden ser menores que n (misma potencia de 2),
    // asi que se miran unos pocos y si no, la clase siguiente
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
    return NULL;
}

static void nemo_free(void *p) {
    if (p == NULL) return;
    block_t *b = mem_block(p);
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

static void *nemo_realloc(void *p, uint32_t nsize) {
    if (p == NULL) return nemo_malloc(nsize);
    block_t *b = mem_block(p);
    if (b->size >= nsize) return p;
    // intentar crecer sobre el vecino libre sin copiar
    if (b->next && b->next->free && b->size + HDR_SIZE + b->next->size >= nsize) {
        block_t *nx = b->next; bin_remove(nx);
        b->size += HDR_SIZE + nx->size; b->next = nx->next;
        if (nx->next) nx->next->prev = b;
        // BUG v2.0: aqui se absorbia el vecino ENTERO aunque hicieran
        // falta 16 bytes -- un bufer del parser que crecia se tragaba
        // todo el pool y la siguiente reserva daba "not enough memory".
        // Devolver el exceso a la lista libre, igual que hace malloc.
        uint32_t n = ALIGN16(nsize);
        if (b->size - n >= MIN_SPLIT) {
            block_t *rest = (block_t *)((uint8_t *)block_mem(b) + n);
            rest->size = b->size - n - HDR_SIZE;
            rest->prev = b; rest->next = b->next;
            if (b->next) b->next->prev = rest;
            b->next = rest; b->size = n;
            bin_push(rest);
        }
        return p;
    }
    void *q = nemo_malloc(nsize);
    if (q == NULL) return NULL;
    uint32_t copy = b->size < nsize ? b->size : nsize;
    uint64_t *d = (uint64_t *)q; const uint64_t *s = (const uint64_t *)p;
    for (uint32_t i = 0; i < copy / 8; i++) d[i] = s[i];
    nemo_free(p);
    return q;
}

// ---- v3: CAJONES PARA OBJETOS PEQUEÑOS ----
//
// Lua crea sobre todo objetos diminutos -- cadenas cortas, entradas de
// tabla, cierres, de 16 a 64 bytes -- y con la cabecera de 48 bytes de
// cada bloque, un objeto de 16 bytes ocupaba 64: la memoria se
// multiplicaba por 3 o 4. Medido con el visor procesando la guia de Lua
// entera: necesitaba 8 MB de monton.
//
// Los objetos de hasta 256 bytes se sirven ahora desde 16 cajones, uno
// por tamaño (multiplos de 16), SIN cabecera: cada cajon toma paginas de
// 64 KB del asignador de arriba y las reparte en trozos exactos; un
// objeto liberado vuelve a la lista de su cajon para el siguiente del
// mismo tamaño. Reservar y liberar es sacar o meter un elemento de una
// lista.
//
// No hace falta cabecera porque Lua SIEMPRE dice el tamaño de un bloque
// al liberarlo o cambiarlo (osize). OJO: cuando ptr es NULL (bloque
// nuevo), osize NO es un tamaño sino el TIPO de objeto que Lua va a
// crear; solo se usa como tamaño si hay bloque.
//
// Las paginas no se devuelven al asignador grande: la memoria de un
// cajon solo la reutiliza ese cajon. En un programa de Lua normal los
// tamaños pequeños se repiten mucho, y al cerrar el programa se devuelve
// su memoria entera de todos modos.
#define PEQ_MAX      256u                  // hasta aqui, cajones
#define PEQ_CLASES   (PEQ_MAX / 16u)       // 16, 32, ... 256
#define PEQ_PAGINA   (64u * 1024u)

typedef struct peq_libre { struct peq_libre *sig; } peq_libre_t;
static peq_libre_t *peq_libres[PEQ_CLASES];   // objetos liberados, por cajon
static uint8_t *peq_cursor[PEQ_CLASES];       // siguiente trozo sin estrenar
static uint8_t *peq_fin[PEQ_CLASES];          // fin de la pagina en uso
static uint32_t peq_paginas = 0;               // para nemo_alloc_used

static inline uint32_t peq_clase(size_t n) { return (uint32_t)((n + 15u) / 16u) - 1u; }

static void *peq_reservar(size_t n) {
    uint32_t c = peq_clase(n);
    peq_libre_t *l = peq_libres[c];
    if (l) { peq_libres[c] = l->sig; return l; }
    uint32_t tam = (c + 1u) * 16u;
    if (peq_cursor[c] == NULL || peq_cursor[c] + tam > peq_fin[c]) {
        uint8_t *pag = (uint8_t *)nemo_malloc(PEQ_PAGINA);
        if (pag == NULL) return NULL;
        peq_paginas++;
        peq_cursor[c] = pag;
        peq_fin[c] = pag + PEQ_PAGINA;
    }
    void *p = peq_cursor[c];
    peq_cursor[c] += tam;
    return p;
}

static void peq_liberar(void *p, size_t n) {
    uint32_t c = peq_clase(n);
    peq_libre_t *l = (peq_libre_t *)p;
    l->sig = peq_libres[c];
    peq_libres[c] = l;
}

void *nemo_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (ptr == NULL) {                                   // bloque nuevo (osize = tipo)
        if (nsize == 0) return NULL;
        return nsize <= PEQ_MAX ? peq_reservar(nsize) : nemo_malloc((uint32_t)nsize);
    }
    // osize 0 con un bloque de verdad no lo produce Lua nunca (no tiene
    // bloques vacios): solo puede venir de un uso equivocado. Se trata
    // como bloque grande, que lleva su tamaño en la cabecera.
    bool viejo_peq = osize > 0 && osize <= PEQ_MAX;
    if (nsize == 0) {                                    // liberar
        if (viejo_peq) peq_liberar(ptr, osize); else nemo_free(ptr);
        return NULL;
    }
    bool nuevo_peq = nsize <= PEQ_MAX;
    if (viejo_peq && nuevo_peq && peq_clase(osize) == peq_clase(nsize)) return ptr;
    if (!viejo_peq && !nuevo_peq) return nemo_realloc(ptr, (uint32_t)nsize);
    // cambia de mundo (o de cajon): nuevo, copiar lo que quepa, liberar
    void *q = nuevo_peq ? peq_reservar(nsize) : nemo_malloc((uint32_t)nsize);
    if (q == NULL) {
        // Lua exige que REDUCIR un bloque nunca falle (lo hace, por
        // ejemplo, en la recoleccion de emergencia, cuando ya no queda
        // memoria). Si no hay sitio para el nuevo, el bloque se queda
        // donde esta y desde ahora cuenta como del tamaño nuevo: un cajon
        // acepta cualquier trozo de memoria suficientemente grande, venga
        // de donde venga. Se pierde el sobrante, solo en este caso raro.
        // Si lo que falla es AGRANDAR, NULL: Lua conserva el bloque viejo.
        return nsize <= osize ? ptr : NULL;
    }
    size_t copia = osize < nsize ? osize : nsize;
    uint8_t *d = (uint8_t *)q; const uint8_t *o = (const uint8_t *)ptr;
    for (size_t i = 0; i < copia; i++) d[i] = o[i];
    if (viejo_peq) peq_liberar(ptr, osize); else nemo_free(ptr);
    return q;
}

// ---- malloc/realloc/free de C (nemo_stdio.c) ----
//
// La biblioteca C del puerto usa este mismo monton, pero no sabe el
// tamaño de lo que libera (free(p) no lo dice). Por eso va SIEMPRE al
// asignador de bloques grandes, que lo guarda en su cabecera, y nunca a
// los cajones. Antes llamaba a nemo_lua_alloc con osize = 0, lo que daba
// igual mientras nemo_lua_alloc ignoraba osize; con los cajones, cada
// free() de C calculaba el cajon -1 y escribia fuera de la tabla (Data
// Abort en LUA.PRO al arrancar: ninguna app de Lua funcionaba).
void *nemo_c_malloc(size_t n) { return n ? nemo_malloc((uint32_t)n) : NULL; }
void *nemo_c_realloc(void *p, size_t n) {
    if (n == 0) { nemo_free(p); return NULL; }
    return nemo_realloc(p, (uint32_t)n);
}
void nemo_c_free(void *p) { nemo_free(p); }

uint32_t nemo_alloc_used(void) {
    if (head == NULL) return 0;
    uint64_t used = 0;
    for (int z = 0; z < num_zonas; z++)
        for (block_t *b = zonas[z]; b; b = b->next) if (!b->free) used += b->size + HDR_SIZE;
    return used > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)used;
}
