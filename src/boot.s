// boot.s
// Punto de entrada del kernel. QEMU/UTM (máquina "virt") carga el kernel
// y salta a la dirección 0x40080000 con el core 0 ejecutando este código.
// Los demás cores (1,2,3) quedan detenidos en un bucle.

.section ".text.boot"

.global _start

_start:
    // Averiguamos qué núcleo de CPU somos (MPIDR_EL1, bits [7:0] = CPU ID)
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, primary_core   // Si somos el core 0, seguimos arrancando
    b       halt_core          // Si no, nos quedamos parados

primary_core:
    // QEMU/UTM a veces arranca el core en EL2 (nivel de hipervisor) en vez
    // de EL1 (nivel de kernel normal). Si estamos en EL2, bajamos a EL1
    // antes de seguir, porque nuestro manejo de excepciones está pensado
    // para EL1.
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    set_stack

    // Configuramos HCR_EL2: RW=1 significa "EL1 corre en AArch64"
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0

    // SCTLR_EL1 en un estado conocido y seguro (MMU y cachés apagadas)
    mov     x0, #0x0
    msr     sctlr_el1, x0

    // Preparamos el "regreso" de excepción para que aterrice en EL1h
    // (EL1 usando su propia pila, SP_EL1) con interrupciones enmascaradas
    mov     x0, #0x3c5
    msr     spsr_el2, x0

    // A qué dirección saltamos al "volver" de la excepción falsa
    adr     x0, set_stack
    msr     elr_el2, x0

    eret

set_stack:
    // Habilitamos el acceso a los registros SIMD/coma flotante --
    // por defecto CPACR_EL1 los deja BLOQUEADOS (cualquier instruccion
    // que los toque dispara una excepcion sincrona con EC=0x07). Con
    // -mgeneral-regs-only en todo el proyecto nunca hizo falta, pero
    // el compilador autohospedado (nbc.pro) ahora usa coma flotante de
    // verdad para su propio lexer/AST, asi que el kernel tiene que
    // dejar via libre ANTES de que corra ningun programa. FPEN=0b11
    // (bits 21:20) = "sin trampa, desde cualquier nivel de excepcion".
    mov     x0, #(0x3 << 20)
    msr     cpacr_el1, x0
    isb

    // Configuramos la pila justo antes del inicio del kernel
    ldr     x0, =_start
    mov     sp, x0

    // Limpiamos la sección .bss (variables globales sin inicializar) a cero
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    sub     x1, x1, x0
    bl      clear_bss

    // Saltamos a nuestro código en C
    bl      kernel_main

halt_core:
    wfe                        // "Wait For Event": duerme la CPU
    b       halt_core

// ---- Entrada de un nucleo SECUNDARIO (SMP, fase 3) ----
//
// Aqui empieza a ejecutar un nucleo 1-3 cuando smp.c lo enciende con
// PSCI. Llega con la MMU y las caches APAGADAS y sin pila. Hace lo
// minimo imprescindible en ensamblador -- nivel de excepcion, coma
// flotante, pila -- y salta a C (secundario_main, en smp.c), que
// enciende su MMU y se presenta.
.global secundario_entrada
secundario_entrada:
    // Si llega en EL2, bajar a EL1: exactamente lo mismo que hace el
    // nucleo 0 en primary_core.
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    1f
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    mov     x0, #0x0
    msr     sctlr_el1, x0
    mov     x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, 1f
    msr     elr_el2, x0
    eret
1:
    // Interrupciones enmascaradas: en la fase 3 este nucleo no atiende
    // ninguna. Todas las de los dispositivos siguen yendo al nucleo 0.
    msr     daifset, #0xf

    // Acceso a la coma flotante, como en set_stack del nucleo 0: es un
    // registro de CADA nucleo, no se hereda.
    mov     x0, #(0x3 << 20)
    msr     cpacr_el1, x0
    isb

    // Pila propia: pila_secundarios + (nucleo+1) * 16384, es decir, el
    // FINAL del trozo de este nucleo (la pila crece hacia abajo). El
    // 16384 (lsl #14) tiene que coincidir con SMP_PILA de smp.h.
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    ldr     x1, =pila_secundarios
    add     x2, x0, #1
    lsl     x2, x2, #14
    add     x1, x1, x2
    mov     sp, x1

    bl      secundario_main    // x0 = numero de nucleo; no vuelve
2:  wfe
    b       2b

clear_bss:
    cbz     x1, clear_bss_done
    strb    wzr, [x0], #1
    sub     x1, x1, #1
    b       clear_bss
clear_bss_done:
    ret
