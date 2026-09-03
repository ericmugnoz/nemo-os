// exceptions.s — Nemo OS
//
// ARM64 exige que la tabla de vectores tenga 16 entradas de 0x80 bytes
// cada una (128 bytes = espacio de sobra para código, aunque cada
// entrada real solo necesita unas pocas instrucciones antes de saltar).
//
// Las 16 entradas cubren la combinación de:
//   - 4 orígenes posibles (EL1t, EL1h, EL0 en AArch64, EL0 en AArch32)
//   - 4 tipos de evento (Síncrono, IRQ, FIQ, SError)
//
// Nosotros solo nos importa "EL1h" (kernel corriendo con su propia pila),
// que es donde vivimos tras el arranque.

.section ".text"
.align 11   // La tabla debe estar alineada a 2048 bytes (2^11)

.global vector_table
.global exceptions_init

.macro VECTOR_ENTRY handler
.align 7    // Cada entrada ocupa 0x80 = 128 bytes
    b       \handler
.endm

// Guarda todos los registros de propósito general en la pila antes de
// llamar a C, y los restaura al volver. Esto es obligatorio: C puede
// usar cualquier registro libremente, así que hay que preservar el
// estado exacto de lo que estábamos haciendo cuando llegó la excepción.
.macro SAVE_STATE
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x30, x0,  [sp, #16 * 15]
    str     x1,       [sp, #16 * 16]
.endm

.macro RESTORE_STATE
    ldr     x1,       [sp, #16 * 16]
    ldp     x30, x0,  [sp, #16 * 15]
    msr     spsr_el1, x1
    msr     elr_el1, x0
    ldp     x28, x29, [sp, #16 * 14]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x0,  x1,  [sp, #16 * 0]
    add     sp, sp, #272
    eret
.endm

.align 11
vector_table:
    // EL1t: no lo usamos (kernel siempre corre en EL1h)
    VECTOR_ENTRY not_used
    VECTOR_ENTRY not_used
    VECTOR_ENTRY not_used
    VECTOR_ENTRY not_used

    // EL1h: aquí es donde vivimos
    VECTOR_ENTRY sync_stub
    VECTOR_ENTRY irq_stub
    VECTOR_ENTRY fiq_stub
    VECTOR_ENTRY serror_stub

    // EL0 (AArch64): para cuando tengamos programas de usuario
    VECTOR_ENTRY not_used
    VECTOR_ENTRY not_used
    VECTOR_ENTRY not_used
    VECTOR_ENTRY not_used

    // EL0 (AArch32): no lo soportamos, Nemo OS es 64-bit puro
    VECTOR_ENTRY not_used
    VECTOR_ENTRY not_used
    VECTOR_ENTRY not_used
    VECTOR_ENTRY not_used

not_used:
    SAVE_STATE
    bl      handle_unexpected
    RESTORE_STATE

sync_stub:
    SAVE_STATE
    mov     x0, sp
    bl      handle_sync
    RESTORE_STATE

irq_stub:
    SAVE_STATE
    bl      handle_irq
    RESTORE_STATE

fiq_stub:
    SAVE_STATE
    bl      handle_fiq
    RESTORE_STATE

serror_stub:
    SAVE_STATE
    bl      handle_serror
    RESTORE_STATE

// Registra nuestra tabla de vectores en VBAR_EL1, el registro que le dice
// a la CPU dónde está la tabla activa.
exceptions_init:
    adr     x0, vector_table
    msr     vbar_el1, x0
    isb
    ret
