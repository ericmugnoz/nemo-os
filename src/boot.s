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

clear_bss:
    cbz     x1, clear_bss_done
    strb    wzr, [x0], #1
    sub     x1, x1, #1
    b       clear_bss
clear_bss_done:
    ret
