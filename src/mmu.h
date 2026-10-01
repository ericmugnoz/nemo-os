// mmu.h — Nemo OS
#ifndef MMU_H
#define MMU_H

#include <stdint.h>

// Huecos de tarea con tabla de traduccion propia. tasks.h comprueba
// que MAX_TASKS no lo supere.
#define MMU_MAX_TASK_CONTEXTS 16

// Construye la tabla del kernel y activa la MMU con ella (ASID 0).
void mmu_init(void);

// Construye una tabla por hueco de tarea: en la del hueco i, solo
// [areas_base + i*area_size, +area_size) es memoria de usuario; todo
// lo demas es solo EL1. Llamar una vez, tras mmu_init. Ver mmu_pi4.c.
void mmu_build_task_tables(uint64_t areas_base, uint64_t area_size, int count);

// Activa la tabla del hueco 'slot' (o la del kernel si slot < 0).
// Dos instrucciones, sin vaciar el TLB. La llama task_yield.
void mmu_switch_context(int slot);
// Programa la MMU del nucleo que la llama con las tablas del kernel ya
// construidas. Para los nucleos secundarios al despertar (smp.c).
void mmu_activar_en_este_nucleo(void);
// Rehace la tabla de un hueco para una region nueva e invalida la TLB
// de su ASID. Ver la nota en mmu.c.
void mmu_set_task_area(int slot, uint64_t base, uint64_t size);

// Fase 4: una zona mas de memoria de usuario para una tarea que ya existe.
void mmu_task_marcar_usuario(int slot, uint64_t base, uint64_t size);

#endif
