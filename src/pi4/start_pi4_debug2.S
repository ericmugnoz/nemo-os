// start_pi4_debug2.S -- añade desactivacion explicita de MMU/cache
//
// Hipotesis nueva: si el firmware deja la MMU o las cachés activas
// de alguna forma antes de saltar a nuestro kernel, nuestras
// escrituras a direcciones fisicas de periféricos (como el UART)
// podrian no llegar realmente al hardware -- quedarian atrapadas
// en cache, sin ningun error visible, simplemente sin efecto real.
//
// Esta version desactiva explicitamente MMU y cachés en SCTLR_EL2
// (si venimos de ahi) y en SCTLR_EL1, ANTES de tocar ningun
// registro de periferico, para partir de un estado conocido.

.section ".text.boot"
.global _start

.equ UART0_DR,  0xFE201000
.equ UART0_FR,  0xFE201018
.equ GPFSEL1,   0xFE200004

_start:
    mrs     x19, mpidr_el1
    and     x19, x19, #0xFF
    cbz     x19, core0_continua
nucleo_secundario_detenido:
    wfe
    b       nucleo_secundario_detenido

core0_continua:
    mov     x20, x0

    // --- NUEVO: comprobar EL actual y desactivar MMU/cache en
    //     ese nivel ANTES de hacer nada mas, por si el firmware
    //     las dejo activas ---
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.eq    limpiar_el2
    b       limpiar_el1_directo

limpiar_el2:
    mrs     x1, sctlr_el2
    mov     x2, #0x1        // bit M (MMU enable)
    bic     x1, x1, x2
    mov     x2, #0x4         // bit C (data cache enable)
    bic     x1, x1, x2
    mov     x2, #0x1000      // bit I (instruction cache enable)
    bic     x1, x1, x2
    msr     sctlr_el2, x1
    isb
    b       continuar_tras_limpiar

limpiar_el1_directo:
    mrs     x1, sctlr_el1
    mov     x2, #0x1
    bic     x1, x1, x2
    mov     x2, #0x4
    bic     x1, x1, x2
    mov     x2, #0x1000
    bic     x1, x1, x2
    msr     sctlr_el1, x1
    isb

continuar_tras_limpiar:
    // --- Configurar GPIO y UART, ya con MMU/cache garantizado
    //     desactivadas en el nivel actual ---
    ldr     x1, =GPFSEL1
    ldr     w0, [x1]
    mov     w5, #0xF000
    movk    w5, #0x3, lsl #16
    bic     w0, w0, w5
    mov     w5, #0x4000
    movk    w5, #0x2, lsl #16
    orr     w0, w0, w5
    str     w0, [x1]

    ldr     x1, =0xFE201030
    mov     w0, #0
    str     w0, [x1]
    ldr     x1, =0xFE201024
    mov     w0, #26
    str     w0, [x1]
    ldr     x1, =0xFE201028
    mov     w0, #3
    str     w0, [x1]
    ldr     x1, =0xFE20102C
    mov     w0, #0x70
    str     w0, [x1]
    ldr     x1, =0xFE201030
    mov     w0, #0x301
    str     w0, [x1]

    // --- Punto de control 1 ---
    mov     w2, #'1'
    bl      debug_putc

    // Comprobar nivel de excepcion (otra vez, para la bajada)
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.eq    bajar_desde_el2
    cmp     x0, #1
    b.eq    ya_en_el1
    mov     w2, #'?'
    bl      debug_putc
    b       nucleo_secundario_detenido

bajar_desde_el2:
    mov     x0, #0x80000000
    msr     hcr_el2, x0
    mov     x0, #0x3C5
    msr     spsr_el2, x0
    adr     x0, ya_en_el1
    msr     elr_el2, x0
    eret

ya_en_el1:
    mov     w2, #'2'
    bl      debug_putc

    // Activar acceso a punto flotante/SIMD (CPACR_EL1.FPEN=0b11).
    // Sin esto, cualquier instruccion de punto flotante o NEON que
    // el compilador genere (incluso para algo tan simple como una
    // copia de memoria optimizada con -O2) provoca una excepcion
    // sincrona con EC=0x07 -- exactamente el crash real que
    // encontramos al llegar hasta aqui con el kernel completo.
    mrs     x0, cpacr_el1
    mov     x1, #0x300000
    orr     x0, x0, x1
    msr     cpacr_el1, x0
    isb

    ldr     x0, =stack_top
    mov     sp, x0

    mov     w2, #'3'
    bl      debug_putc

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
limpiar_bss:
    cmp     x0, x1
    b.ge    bss_limpio
    str     xzr, [x0], #8
    b       limpiar_bss
bss_limpio:

    mov     w2, #'4'
    bl      debug_putc
    mov     w2, #10
    bl      debug_putc

    mov     x0, x20
    bl      kernel_main

colgar_si_kernel_main_vuelve:
    mov     w2, #'R'
    bl      debug_putc
    wfe
    b       colgar_si_kernel_main_vuelve

debug_putc:
    ldr     x3, =UART0_FR
espera_fifo:
    ldr     w4, [x3]
    tst     w4, #(1 << 5)
    b.ne    espera_fifo
    ldr     x3, =UART0_DR
    str     w2, [x3]
    ret
