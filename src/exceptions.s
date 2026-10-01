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
// Nos importan "EL1h" (el kernel con su propia pila, donde vivimos tras
// el arranque) y "EL0 AArch64" (las tareas de usuario, desde la Fase 1
// de la separacion kernel/programas).

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
    // Enmascarar IRQ ANTES de tocar spsr_el1/elr_el1: como las
    // excepciones sincronas (syscalls) corren ahora con IRQ habilitada,
    // una interrupcion anidada entre estas escrituras y el eret los
    // machacaria y volveriamos a un sitio equivocado. El eret restaura
    // la mascara real desde el SPSR guardado.
    msr     daifset, #2
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

// ---- Estado de coma flotante del PROGRAMA (SMP) ----
//
// Al entrar al kernel desde un programa se guardan tambien sus 32
// registros de coma flotante/SIMD (q0-q31, 128 bits cada uno) y los de
// control FPCR/FPSR, y se reponen al volver. 528 bytes en la pila de
// kernel de la tarea, justo debajo del marco de los registros enteros.
//
// Por que: los programas hacen sus llamadas con un 'svc' en ensamblador
// en linea que solo declara que toca la memoria, asi que su compilador
// puede dejar decimales vivos en cualquier registro de coma flotante A
// TRAVES de la llamada. Antes el kernel solo guardaba los enteros (y
// d8-d15 al cambiar de tarea): un decimal en, por ejemplo, d16 se perdia
// si la tarea volvia en OTRO nucleo (con lo que dejo alli otro programa),
// si entre medias corria otra tarea en el mismo, o si el propio kernel
// usaba esos registros (el de la Pi se compila sin -mgeneral-regs-only).
// Se vio con Lua, que calcula casi todo con decimales: con varias apps a
// la vez, tamanos de fuente equivocados, maquetaciones absurdas y
// scripts que fallaban -- el visor se cerraba solo.
//
// Asi el estado viaja con la tarea, en su propia pila, a cualquier
// nucleo. x9/x10 se pueden usar: ya estan guardados en el marco entero.
.macro FP_GUARDAR
    sub     sp, sp, #528
    stp     q0,  q1,  [sp, #0]
    stp     q2,  q3,  [sp, #32]
    stp     q4,  q5,  [sp, #64]
    stp     q6,  q7,  [sp, #96]
    stp     q8,  q9,  [sp, #128]
    stp     q10, q11, [sp, #160]
    stp     q12, q13, [sp, #192]
    stp     q14, q15, [sp, #224]
    stp     q16, q17, [sp, #256]
    stp     q18, q19, [sp, #288]
    stp     q20, q21, [sp, #320]
    stp     q22, q23, [sp, #352]
    stp     q24, q25, [sp, #384]
    stp     q26, q27, [sp, #416]
    stp     q28, q29, [sp, #448]
    stp     q30, q31, [sp, #480]
    mrs     x9, fpcr
    mrs     x10, fpsr
    str     x9,  [sp, #512]      // str sueltos: un stp de 64 bits solo llega a #504
    str     x10, [sp, #520]
.endm

.macro FP_RESTAURAR
    ldr     x9,  [sp, #512]
    ldr     x10, [sp, #520]
    msr     fpcr, x9
    msr     fpsr, x10
    ldp     q0,  q1,  [sp, #0]
    ldp     q2,  q3,  [sp, #32]
    ldp     q4,  q5,  [sp, #64]
    ldp     q6,  q7,  [sp, #96]
    ldp     q8,  q9,  [sp, #128]
    ldp     q10, q11, [sp, #160]
    ldp     q12, q13, [sp, #192]
    ldp     q14, q15, [sp, #224]
    ldp     q16, q17, [sp, #256]
    ldp     q18, q19, [sp, #288]
    ldp     q20, q21, [sp, #320]
    ldp     q22, q23, [sp, #352]
    ldp     q24, q25, [sp, #384]
    ldp     q26, q27, [sp, #416]
    ldp     q28, q29, [sp, #448]
    ldp     q30, q31, [sp, #480]
    add     sp, sp, #528
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

    // EL0 (AArch64): las tareas de usuario, desde la Fase 1 de la
    // separacion kernel/programas. Mismos manejadores que EL1h: SAVE_STATE
    // guarda SPSR_EL1 (que dice de que nivel venimos) y el eret de
    // RESTORE_STATE vuelve a ese mismo nivel. Al entrar desde EL0 la CPU
    // cambia sola a SP_EL1 (la pila de kernel de la tarea, la que
    // task_switch deja puesta), asi que la pila de usuario no se toca.
    VECTOR_ENTRY sync_stub_el0
    VECTOR_ENTRY irq_stub_el0
    VECTOR_ENTRY fiq_stub_el0
    VECTOR_ENTRY serror_stub_el0

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
    // Las excepciones sincronas son casi siempre llamadas al sistema
    // (svc). Se ejecutan con IRQ HABILITADA, como en cualquier kernel:
    // al entrar en una excepcion la CPU enmascara las interrupciones, y
    // si una syscall cede el control (SYS_PUMP -> task_yield -> wfe) con
    // IRQ enmascarada, el WFE no se despierta nunca (respeta la mascara
    // y no hay otros nucleos que envien eventos). En la Pi 4 esto
    // colgaba el sistema al primer SYS_PUMP de cualquier programa; en
    // QEMU no se veia porque su WFE apenas duerme.
    msr     daifclr, #2
    mov     x0, sp
    bl      handle_sync
    RESTORE_STATE

irq_stub:
    SAVE_STATE
    // Interrupcion recibida en el KERNEL (EL1). Casi siempre el nucleo
    // ya tiene el candado grande, porque estaba trabajando. La excepcion
    // es cuando el nucleo 0 duerme SIN el (el wfe de task_yield, fase 5a
    // del SMP): ahi este manejador correria kernel sin candado.
    // bkl_tomar_si_falta lo coge solo en ese caso y devuelve 1 si lo
    // hizo. Ese 1 se guarda en x19, que las funciones de C respetan y
    // que SAVE_STATE/RESTORE_STATE ya guardan y reponen, para saber al
    // salir si hay que soltarlo.
    bl      bkl_tomar_si_falta
    mov     x19, x0
    bl      handle_irq
    cbz     x19, 1f
    bl      bkl_soltar
1:
    RESTORE_STATE

fiq_stub:
    SAVE_STATE
    bl      handle_fiq
    RESTORE_STATE

serror_stub:
    SAVE_STATE
    bl      handle_serror
    RESTORE_STATE

// ---- Entradas desde un PROGRAMA (EL0): el candado grande ----
//
// Son los mismos manejadores de siempre, envueltos con el candado grande
// del kernel (bkl.c, fase 4 del SMP): se coge al entrar y se suelta
// justo antes de volver al programa.
//
// Las entradas desde el propio kernel (EL1h, arriba) NO lo tocan: si
// llega una interrupcion mientras el kernel trabaja, ese nucleo ya tiene
// el candado, y pedirlo otra vez lo colgaria esperandose a si mismo.
// Por eso antes compartian manejador y ahora no: el nivel de origen
// decide si hay que coger el candado.
//
// El orden al salir importa: primero enmascarar las interrupciones y
// DESPUES soltar. Al reves, una interrupcion colada entre las dos cosas
// entraria por la entrada EL1 (sin coger candado) y correria codigo del
// kernel sin tenerlo, mientras otro nucleo podria estar dentro.
//
// bkl_tomar y bkl_soltar son funciones de C: pueden machacar x0-x18,
// pero SAVE_STATE ya los guardo todos en el marco y RESTORE_STATE los
// recupera de ahi. El resultado de una syscall tambien viaja en el marco
// (handle_sync lo escribe alli), asi que tampoco se pierde.
sync_stub_el0:
    SAVE_STATE
    FP_GUARDAR
    bl      bkl_tomar
    // Las syscalls corren con IRQ habilitada; ver la nota de sync_stub.
    msr     daifclr, #2
    add     x0, sp, #528        // el marco de enteros esta ENCIMA del de coma flotante
    bl      handle_sync
    msr     daifset, #2
    bl      bkl_soltar
    FP_RESTAURAR
    RESTORE_STATE

irq_stub_el0:
    SAVE_STATE
    FP_GUARDAR
    bl      bkl_tomar
    bl      handle_irq
    msr     daifset, #2
    bl      bkl_soltar
    FP_RESTAURAR
    RESTORE_STATE

fiq_stub_el0:
    SAVE_STATE
    FP_GUARDAR
    bl      bkl_tomar
    bl      handle_fiq
    msr     daifset, #2
    bl      bkl_soltar
    FP_RESTAURAR
    RESTORE_STATE

serror_stub_el0:
    SAVE_STATE
    FP_GUARDAR
    bl      bkl_tomar
    bl      handle_serror
    msr     daifset, #2
    bl      bkl_soltar
    FP_RESTAURAR
    RESTORE_STATE

// Registra nuestra tabla de vectores en VBAR_EL1, el registro que le dice
// a la CPU dónde está la tabla activa.
exceptions_init:
    adr     x0, vector_table
    msr     vbar_el1, x0
    isb
    ret
