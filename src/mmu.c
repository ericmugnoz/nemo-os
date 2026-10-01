// mmu.c — Nemo OS, QEMU virt
// MMU con mapeo de identidad (VA = PA), UNA TABLA POR TAREA -- la
// misma Fase 2 de la separacion kernel/programas que src/pi4/mmu_pi4.c,
// con el mapa de memoria de la maquina "virt" de QEMU:
//
//   0x00000000-0x3FFFFFFF  perifericos (UART en 0x09000000, GIC en 0x08000000)
//   0x40000000-...         RAM (el kernel arranca en 0x40080000), del tamaño
//                          que diga memoria.c (-m del Makefile, hasta 4 GB)
//
// Antes solo se mapeaba el primer GB de RAM. Ahora cada juego de tablas
// tiene una tabla de nivel 2 por cada GB donde puede haber RAM
// (MMU_GB_TABLAS), y cada bloque de 2 MB se mapea segun lo que es DE
// VERDAD (memoria_es_ram): RAM del kernel, RAM de la tarea, o nada. El GB
// del kernel se sigue mapeando entero, como siempre.
//
// Todo lo demas -- una pareja de tablas (L1+L2) por hueco de tarea mas
// la del kernel, construidas una vez y nunca modificadas, ASID por
// tabla en TTBR0[55:48], y NINGUNA entrada global (nG=1 en todo) para
// que no convivan una traduccion global y una no-global de la misma
// direccion en el TLB -- esta explicado en mmu_pi4.c y no se repite
// aqui. Mantener los dos archivos en paralelo: si cambia la logica de
// uno, cambia la del otro.

#include <stdint.h>
#include <stdbool.h>
#include "mmu.h"
#include "memoria.h"

#define MM_TYPE_BLOCK         0x1UL
#define MM_TYPE_TABLE         0x3UL
#define MM_ACCESS_FLAG        (1UL << 10)
#define MM_NOT_GLOBAL         (1UL << 11)
#define MM_SHAREABLE_INNER    (3UL << 8)
#define MM_AP_EL1_RW_EL0_NONE (0UL << 6)
#define MM_AP_EL1_RW_EL0_RW   (1UL << 6)
#define MM_UXN                (1UL << 54)
#define MM_PXN                (1UL << 53)

#define MT_DEVICE_nGnRnE_IDX 0
#define MT_NORMAL_IDX        1

// MAIR: indice 0 = Device-nGnRnE (0x00, lo que QEMU virt siempre ha
// usado aqui), indice 1 = Normal WB (0xFF).
#define MAIR_VALUE ( (0x00UL << (MT_DEVICE_nGnRnE_IDX * 8)) | \
                     (0xFFUL << (MT_NORMAL_IDX * 8)) )

#define BLOCK_DEVICE (MM_TYPE_BLOCK | (MT_DEVICE_nGnRnE_IDX << 2) | MM_ACCESS_FLAG | MM_NOT_GLOBAL | MM_AP_EL1_RW_EL0_NONE | MM_UXN | MM_PXN)
#define BLOCK_KERNEL (MM_TYPE_BLOCK | (MT_NORMAL_IDX << 2) | MM_ACCESS_FLAG | MM_NOT_GLOBAL | MM_SHAREABLE_INNER | MM_AP_EL1_RW_EL0_NONE | MM_UXN)
#define BLOCK_USER   (MM_TYPE_BLOCK | (MT_NORMAL_IDX << 2) | MM_ACCESS_FLAG | MM_NOT_GLOBAL | MM_SHAREABLE_INNER | MM_AP_EL1_RW_EL0_RW | MM_PXN)

#define RAM_BASE  0x40000000UL
#define BLOCK_1GB (1UL << 30)
#define BLOCK_2MB (2UL * 1024 * 1024)

// La RAM de QEMU virt empieza en el GB 1 (0x40000000). Tablas para 4 GB
// de RAM: GB 1 a 4. Cada juego ocupa 4 KB + 4 x 4 KB; con 17 juegos
// (16 huecos de tarea + el kernel), unos 340 KB.
#define MMU_GB_PRIMERO 1
#define MMU_GB_TABLAS  4

typedef struct {
    __attribute__((aligned(4096))) uint64_t l1[512];
    __attribute__((aligned(4096))) uint64_t l2[MMU_GB_TABLAS][512];   // un GB cada una, bloques de 2MB
} mmu_table_set_t;

static mmu_table_set_t kernel_tables;
static mmu_table_set_t task_tables[MMU_MAX_TASK_CONTEXTS];

static void build_tables(mmu_table_set_t *t, uint64_t user_start, uint64_t user_size) {
    for (int i = 0; i < 512; i++) t->l1[i] = 0;
    uint64_t user_end = user_start + ((user_size + BLOCK_2MB - 1) & ~(BLOCK_2MB - 1));
    for (int k = 0; k < MMU_GB_TABLAS; k++) {
        uint64_t gb = (uint64_t)(MMU_GB_PRIMERO + k);
        bool alguna = false;
        for (int i = 0; i < 512; i++) {
            uint64_t pa = (gb << 30) + (uint64_t)i * BLOCK_2MB;
            uint64_t e = 0;   // sin mapear: ahi no hay RAM
            // El GB del kernel (k == 0) entero, como siempre; los demas,
            // solo donde memoria.c dice que hay RAM.
            if (k == 0 || memoria_es_ram(pa, BLOCK_2MB)) {
                bool usuario = user_size > 0 && pa >= user_start && pa < user_end;
                e = pa | (usuario ? BLOCK_USER : BLOCK_KERNEL);
                alguna = true;
            }
            t->l2[k][i] = e;
        }
        t->l1[gb] = alguna ? ((uint64_t)t->l2[k] | MM_TYPE_TABLE) : 0;
    }
    t->l1[0] = 0x00000000UL | BLOCK_DEVICE;          // perifericos
}

static inline uint64_t ttbr0_for(mmu_table_set_t *t, uint64_t asid) {
    return (uint64_t)t->l1 | (asid << 48);
}

void mmu_activar_en_este_nucleo(void);

void mmu_init(void) {
    build_tables(&kernel_tables, 0, 0);

    // Barrera explicita: sin el clobber "memory", el compilador podria
    // reordenar las escrituras de la tabla respecto a los msr de abajo.
    __asm__ volatile("" ::: "memory");

    mmu_activar_en_este_nucleo();
}

// Programa la MMU de ESTE nucleo con las tablas del kernel, que ya
// estan construidas. La usa el nucleo 0 desde mmu_init, y cada nucleo
// secundario al despertar (smp.c): las tablas son una sola para todos,
// pero MAIR, TCR, TTBR0 y SCTLR son registros de cada nucleo y cada uno
// tiene que escribir los suyos. Reconstruir las tablas desde un
// secundario seria un error: el nucleo 0 ya las esta usando.
void mmu_activar_en_este_nucleo(void) {
    uint64_t mair = MAIR_VALUE;
    __asm__ volatile("msr mair_el1, %0" :: "r"(mair) : "memory");

    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t parange = mmfr0 & 0xF;

    // T0SZ=25 (VA de 39 bits), tablas cacheables, SH0=Inner, TG0=4KB,
    // IPS segun la CPU. AS=0: ASID de 8 bits. A1=0: ASID de TTBR0.
    uint64_t tcr = (25UL)
                 | (1UL << 8)
                 | (1UL << 10)
                 | (3UL << 12)
                 | (0UL << 14)
                 | (parange << 32);
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");

    uint64_t ttbr0 = ttbr0_for(&kernel_tables, 0);
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr0) : "memory");
    __asm__ volatile("isb" ::: "memory");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);   // M, C, I
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
    __asm__ volatile("isb" ::: "memory");
}

void mmu_build_task_tables(uint64_t areas_base, uint64_t area_size, int count) {
    if (count > MMU_MAX_TASK_CONTEXTS) count = MMU_MAX_TASK_CONTEXTS;
    for (int i = 0; i < count; i++) {
        build_tables(&task_tables[i], areas_base + (uint64_t)i * area_size, area_size);
    }
    __asm__ volatile("dsb ishst" ::: "memory");
}

// Rehace la tabla de UN hueco para que su memoria de usuario sea
// [base, base+size). Se llama al lanzar cada tarea, porque ahora cada
// una recibe una region de su propio tamaño.
//
// ESTO ROMPE UNA SUPOSICION DEL DISEÑO ORIGINAL, y por eso lleva la
// invalidacion de TLB de abajo: hasta ahora las tablas se construian
// una sola vez y no se tocaban nunca, asi que la TLB nunca podia
// quedarse con traducciones viejas. Al rehacer la tabla de un hueco
// para otra tarea con otra region, la TLB aun puede tener cacheadas
// las traducciones de la tarea ANTERIOR de ese hueco (mismo ASID). Si
// no se invalidan, la tarea nueva podria leer y escribir en la region
// de la anterior -- un fallo silencioso de los peores.
//
// El ASID de cada hueco es hueco+1 (ver mmu_switch_context).
void mmu_set_task_area(int slot, uint64_t base, uint64_t size) {
    if (slot < 0 || slot >= MMU_MAX_TASK_CONTEXTS) return;
    build_tables(&task_tables[slot], base, size);

    uint64_t asid = (uint64_t)slot + 1;
    // 1) que las escrituras en la tabla sean visibles para quien
    //    recorre las tablas en hardware, ANTES de invalidar
    __asm__ volatile("dsb ishst" ::: "memory");
    // 2) tirar todo lo cacheado para ese ASID (en todos los nucleos:
    //    el sufijo 'is' es inner shareable)
    __asm__ volatile("tlbi aside1is, %0" :: "r"(asid << 48) : "memory");
    // 3) esperar a que la invalidacion termine y resincronizar
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

// Fase 4: da a una tarea que YA existe una zona mas de memoria (pedida con
// SYS_MEM_PEDIR, ver tasks.c): marca como de usuario los bloques de 2 MB
// de [base, base+size) en SUS tablas. Esos bloques ya estaban mapeados
// como del kernel (toda la RAM lo esta), y los dos tipos solo difieren en
// los permisos (ninguno es global), asi que basta con invalidar la TLB de
// esa tarea, como en mmu_set_task_area.
void mmu_task_marcar_usuario(int slot, uint64_t base, uint64_t size) {
    if (slot < 0 || slot >= MMU_MAX_TASK_CONTEXTS || size == 0) return;
    mmu_table_set_t *t = &task_tables[slot];
    uint64_t fin = base + ((size + BLOCK_2MB - 1) & ~(BLOCK_2MB - 1));
    for (uint64_t pa = base & ~(BLOCK_2MB - 1); pa < fin; pa += BLOCK_2MB) {
        int64_t k = (int64_t)(pa >> 30) - MMU_GB_PRIMERO;
        if (k < 0 || k >= MMU_GB_TABLAS) continue;
        uint64_t i = (pa >> 21) & 511;
        if (t->l2[k][i] == 0) continue;     // ahi no hay RAM: no se toca
        t->l2[k][i] = pa | BLOCK_USER;
    }
    uint64_t asid = (uint64_t)slot + 1;
    __asm__ volatile("dsb ishst" ::: "memory");
    __asm__ volatile("tlbi aside1is, %0" :: "r"(asid << 48) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

void mmu_switch_context(int slot) {
    uint64_t ttbr0;
    if (slot < 0 || slot >= MMU_MAX_TASK_CONTEXTS) ttbr0 = ttbr0_for(&kernel_tables, 0);
    else                                           ttbr0 = ttbr0_for(&task_tables[slot], (uint64_t)slot + 1);
    __asm__ volatile("msr ttbr0_el1, %0\n\tisb" :: "r"(ttbr0) : "memory");
}
