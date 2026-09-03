// mmu.c — Nemo OS
//
// Activamos la MMU con el mapeo MÁS simple posible: "identity mapping"
// (cada dirección virtual = la misma dirección física) usando bloques
// de 1GB a nivel L1. Esto es intencionalmente básico -- cuando lleguemos
// a procesos de usuario, cada uno necesitará su propia tabla con páginas
// de 4KB para poder aislarlos entre sí. Por ahora solo necesitamos que
// la MMU esté activa y el kernel siga funcionando exactamente igual.
//
// Con TCR_EL1.T0SZ = 25, el espacio de direcciones virtuales es de 39
// bits, y la traducción empieza directamente en el nivel 1 (nos
// ahorramos una tabla de nivel 0). Cada entrada de nivel 1 cubre 1GB.

#include <stdint.h>
#include "mmu.h"

// Tabla de nivel 1: 512 entradas de 8 bytes = exactamente 4KB.
// Debe estar alineada a 4KB porque solo los bits altos de la dirección
// se usan como puntero a la tabla (los bits bajos son para flags).
__attribute__((aligned(4096)))
static uint64_t l1_table[512];

#define MM_TYPE_BLOCK       0x1UL
#define MM_ACCESS_FLAG      (1UL << 10)  // AF: marca la página como "accedida"
#define MM_SHAREABLE_INNER  (3UL << 8)   // SH[1:0] = 11

// Índices de atributos de memoria (definidos también en MAIR_EL1 abajo)
#define MT_DEVICE_nGnRnE_IDX 0
#define MT_NORMAL_IDX        1

// MAIR_EL1: una tabla de hasta 8 "perfiles" de memoria, referenciados
// por índice desde cada entrada de la tabla de traducción.
//   - Device-nGnRnE (0x00): para MMIO (UART, GIC...) -- sin reordenar,
//     sin agrupar, sin acceso especulativo. Es obligatorio para hardware.
//   - Normal, Write-Back (0xFF): para RAM normal, con cache.
#define MAIR_VALUE ( (0x00UL << (MT_DEVICE_nGnRnE_IDX * 8)) | \
                     (0xFFUL << (MT_NORMAL_IDX  * 8)) )

#define BLOCK_DEVICE (MM_TYPE_BLOCK | (MT_DEVICE_nGnRnE_IDX << 2) | MM_ACCESS_FLAG)
#define BLOCK_NORMAL (MM_TYPE_BLOCK | (MT_NORMAL_IDX << 2) | MM_ACCESS_FLAG | MM_SHAREABLE_INNER)

void mmu_init(void) {
    // Entrada 0: direcciones 0x00000000 - 0x3FFFFFFF -> dispositivos
    // (aquí viven el UART en 0x09000000 y el GIC en 0x08000000..0x08010000)
    l1_table[0] = 0x00000000UL | BLOCK_DEVICE;

    // Entrada 1: direcciones 0x40000000 - 0x7FFFFFFF -> RAM, donde
    // vive nuestro kernel (arranca en 0x40080000)
    l1_table[1] = 0x40000000UL | BLOCK_NORMAL;

    // IMPORTANTE: barrera de memoria explícita. Sin el clobber "memory",
    // el compilador (sobre todo con -O2) puede reordenar las escrituras
    // de arriba respecto a los bloques asm de abajo, porque no ve
    // ninguna relación entre "l1_table[...]=..." y "msr ttbr0_el1, ...".
    // Si eso pasa, podríamos activar la MMU antes de que la tabla esté
    // realmente escrita en memoria -> tabla vacía -> fallo de traducción
    // inmediato. Este es exactamente el tipo de bug silencioso que solo
    // aparece con optimizaciones activadas.
    __asm__ volatile("" ::: "memory");

    uint64_t mair = MAIR_VALUE;
    __asm__ volatile("msr mair_el1, %0" :: "r"(mair) : "memory");

    // Preguntamos a la propia CPU cuántos bits de dirección física
    // soporta de verdad (PARange), en vez de asumir un número fijo.
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t parange = mmfr0 & 0xF;

    uint64_t tcr = (25UL)            // T0SZ=25 -> VA de 39 bits, empieza en L1
                 | (1UL << 8)        // IRGN0: Write-Back, Read/Write-Allocate
                 | (1UL << 10)       // ORGN0: igual, para el exterior
                 | (3UL << 12)       // SH0: Inner Shareable
                 | (0UL << 14)       // TG0=00: granularidad de 4KB
                 | (parange << 32);  // IPS: rango de direcciones físicas real

    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");

    uint64_t ttbr0 = (uint64_t)l1_table;
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr0) : "memory");

    __asm__ volatile("isb" ::: "memory");

    // Activamos: bit M (MMU), bit C (cache de datos), bit I (cache de
    // instrucciones). Hacemos lectura-modificación-escritura para no
    // pisar otros bits que la CPU ya tuviera puestos.
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);

    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
    __asm__ volatile("isb" ::: "memory");
}
