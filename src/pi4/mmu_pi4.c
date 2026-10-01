// mmu_pi4.c -- Nemo OS, Raspberry Pi 4
// MMU con mapeo de identidad (VA = PA), UNA TABLA POR TAREA.
//
// TERCERA VERSION (Fase 2 de la separacion kernel/programas). La
// Fase 1 usaba una unica tabla de nivel 2 con bloques de 2MB, con el
// area de programa de TODAS las tareas abierta para EL0: el kernel
// quedaba protegido, pero una tarea podia leer o pisar la memoria de
// otra. Ahora cada hueco de tarea tiene su propia pareja de tablas
// (L1 + L2), donde SOLO su propia area de 16MB es de usuario y todo
// lo demas -- el kernel, y las areas de las otras tareas -- es solo
// EL1. El kernel tiene su propia pareja, sin nada de usuario.
//
// Las 9 parejas se construyen UNA VEZ (mmu_init para la del kernel,
// mmu_build_task_tables para las 8 de tareas) y no se tocan nunca
// mas: con mapeo de identidad, la tabla del hueco i es la misma sea
// cual sea el programa que corra en el. Cambiar de tarea es cambiar
// TTBR0_EL1 (mmu_switch_context, desde task_yield) -- dos
// instrucciones.
//
// ASID: cada tabla lleva su identificador en TTBR0[55:48] (0 = kernel,
// i+1 = hueco i). El TLB etiqueta cada traduccion con el ASID activo,
// asi que cambiar de tarea NO exige vaciar el TLB: las traducciones de
// cada una se conservan y no se mezclan. Regla clave: NINGUNA entrada
// es "global" (nG=1 en todos los bloques, tambien los del kernel). Si
// el kernel usara entradas globales, la direccion del area de la
// tarea 1 tendria una traduccion global (kernel, desde las otras
// tablas) y otra no-global (usuario, desde la suya) conviviendo en el
// TLB -- ARM lo declara impredecible (conflicto de TLB). Con todo
// etiquetado por ASID no hay conflicto posible; el precio son unas
// entradas de TLB duplicadas para el kernel, irrelevante con 9
// contextos.
//
// Cortex-A72 es ARMv8.0-A: sin PAN, el kernel sigue leyendo los
// punteros que las syscalls reciben desde EL0 sin configuracion
// extra. Y una syscall de la tarea A que carga un programa en el hueco
// B (SYS_LAUNCH_PROGRAM) funciona porque, en la tabla de A, el area de
// B es memoria de kernel: EL1 puede escribirla.

#include <stdint.h>
#include <stdbool.h>
#include "mmu.h"
#include "memoria.h"

#define MM_TYPE_BLOCK         0x1UL
#define MM_TYPE_TABLE         0x3UL
#define MM_ACCESS_FLAG        (1UL << 10)
#define MM_NOT_GLOBAL         (1UL << 11)   // nG: etiquetar por ASID
#define MM_SHAREABLE_INNER    (3UL << 8)
#define MM_AP_EL1_RW_EL0_NONE (0UL << 6)    // AP[2:1] = 00
#define MM_AP_EL1_RW_EL0_RW   (1UL << 6)    // AP[2:1] = 01
#define MM_UXN                (1UL << 54)   // EL0 no puede ejecutar
#define MM_PXN                (1UL << 53)   // EL1 no puede ejecutar

#define MT_DEVICE_IDX 0
#define MT_NORMAL_IDX 1

// MAIR: indice 0 = Device-nGnRE (0x04), indice 1 = Normal WB (0xFF).
#define MAIR_VALUE ( (0x04UL << (MT_DEVICE_IDX * 8)) | \
                     (0xFFUL << (MT_NORMAL_IDX * 8)) )

#define BLOCK_DEVICE (MM_TYPE_BLOCK | (MT_DEVICE_IDX << 2) | MM_ACCESS_FLAG | MM_NOT_GLOBAL | MM_AP_EL1_RW_EL0_NONE | MM_UXN | MM_PXN)
#define BLOCK_KERNEL (MM_TYPE_BLOCK | (MT_NORMAL_IDX << 2) | MM_ACCESS_FLAG | MM_NOT_GLOBAL | MM_SHAREABLE_INNER | MM_AP_EL1_RW_EL0_NONE | MM_UXN)
#define BLOCK_USER   (MM_TYPE_BLOCK | (MT_NORMAL_IDX << 2) | MM_ACCESS_FLAG | MM_NOT_GLOBAL | MM_SHAREABLE_INNER | MM_AP_EL1_RW_EL0_RW | MM_PXN)

#define BLOCK_1GB (1UL << 30)
#define BLOCK_2MB (2UL * 1024 * 1024)

// TODA LA RAM. Antes solo se mapeaba el primer GB: en una
// Pi 4 de 4 GB, Nemo OS veia la cuarta parte. Ahora cada juego de tablas
// tiene una tabla de nivel 2 por cada GB (hasta 8, las Pi 4 de 8 GB), y
// cada bloque de 2 MB se mapea segun lo que es DE VERDAD (memoria.c):
//   - el primer GB, entero y como siempre (kernel, y la memoria de la GPU
//     con el framebuffer, que el firmware coloca al final de ese GB);
//   - el resto de la RAM que diga memoria_es_ram();
//   - los perifericos: SOLO los ultimos 64 MB antes de 4 GB (0xFC000000).
//     Antes se mapeaba como perifericos TODO el cuarto GB, 0xC0000000-
//     0xFFFFFFFF, de un bloque; en una Pi de 4 GB casi todo eso es RAM;
//   - nada en lo demas.
// Cada juego ocupa 4 KB + 8 x 4 KB; con 17 juegos, unos 612 KB.
#define MMU_GB_PRIMERO 0
#define MMU_GB_TABLAS  8
#define PERIFERICOS_INICIO 0xFC000000ULL
#define PERIFERICOS_FIN    0x100000000ULL

typedef struct {
    __attribute__((aligned(4096))) uint64_t l1[512];
    __attribute__((aligned(4096))) uint64_t l2[MMU_GB_TABLAS][512];   // un GB cada una, bloques de 2MB
} mmu_table_set_t;

static mmu_table_set_t kernel_tables;
static mmu_table_set_t task_tables[MMU_MAX_TASK_CONTEXTS];

// Rellena una pareja de tablas: todo el primer GB como kernel, salvo
// [user_start, user_start+user_size) como usuario (user_size=0: nada de
// usuario, la tabla del kernel). Perifericos y ventana PCIe iguales en
// todas.
static void build_tables(mmu_table_set_t *t, uint64_t user_start, uint64_t user_size) {
    for (int i = 0; i < 512; i++) t->l1[i] = 0;
    uint64_t user_end = user_start + ((user_size + BLOCK_2MB - 1) & ~(BLOCK_2MB - 1));
    for (int k = 0; k < MMU_GB_TABLAS; k++) {
        uint64_t gb = (uint64_t)(MMU_GB_PRIMERO + k);
        bool alguna = false;
        for (int i = 0; i < 512; i++) {
            uint64_t pa = (gb << 30) + (uint64_t)i * BLOCK_2MB;
            uint64_t e = 0;   // sin mapear
            if (k == 0 || memoria_es_ram(pa, BLOCK_2MB)) {
                bool usuario = user_size > 0 && pa >= user_start && pa < user_end;
                e = pa | (usuario ? BLOCK_USER : BLOCK_KERNEL);
            } else if (pa >= PERIFERICOS_INICIO && pa < PERIFERICOS_FIN) {
                // mailbox/UART/GPIO desde 0xFE000000, GIC en 0xFF841000,
                // PCIe en 0xFD500000
                e = pa | BLOCK_DEVICE;
            }
            if (e) alguna = true;
            t->l2[k][i] = e;
        }
        t->l1[gb] = alguna ? ((uint64_t)t->l2[k] | MM_TYPE_TABLE) : 0;
    }
    // Entrada 24: 0x600000000-0x63FFFFFFF -> ventana PCIe (BAR0 del
    // VL805, registros xHCI).
    t->l1[24] = 0x600000000UL | BLOCK_DEVICE;
}

static inline uint64_t ttbr0_for(mmu_table_set_t *t, uint64_t asid) {
    return (uint64_t)t->l1 | (asid << 48);
}

void mmu_activar_en_este_nucleo(void);

void mmu_init(void) {
    build_tables(&kernel_tables, 0, 0);

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

    // T0SZ=25 (VA de 39 bits), tablas cacheables (IRGN0/ORGN0=WB),
    // SH0=Inner, TG0=4KB, IPS segun la CPU. AS=0: ASID de 8 bits
    // (solo hacen falta 9 valores). A1=0: el ASID viene de TTBR0.
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
    // Las tablas se escriben con la MMU ya activa y memoria normal
    // cacheable; el recorrido de tablas de la CPU tambien va por cache
    // (IRGN0/ORGN0=WB), asi que basta una barrera para el orden.
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
    // Sin TLBI: las traducciones quedan etiquetadas por ASID y las
    // tablas nunca cambian. Si llega una IRQ entre el msr y el isb no
    // pasa nada -- el manejador es codigo y datos de kernel, presentes
    // e identicos en todas las tablas.
    __asm__ volatile("msr ttbr0_el1, %0\n\tisb" :: "r"(ttbr0) : "memory");
}
