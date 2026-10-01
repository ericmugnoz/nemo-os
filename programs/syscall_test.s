// syscall_test.s — programa de prueba para las syscalls de Nemo OS
//
// Convencion de llamada: numero de syscall en x8, argumentos en
// x0-x4, resultado devuelto en x0. Se invoca con 'svc #0'.
//
// A diferencia de hello.s (que toca el hardware del UART
// directamente), este programa NO sabe nada de direcciones de
// hardware -- todo lo pide al kernel via syscalls, exactamente como
// lo hara la shell mas adelante.

.section .text
.global _start

_start:
    // SYS_WRITE_STRING(msg) -- numero de syscall 11
    adr x0, msg
    mov x8, #11
    svc #0

    // SYS_DRAW_RECT(x=10, y=10, w=150, h=50, color=0x00CC3333) -- numero 30
    mov x0, #10
    mov x1, #10
    mov x2, #150
    mov x3, #50
    movz x4, #0x3333
    movk x4, #0x00CC, lsl #16
    mov x8, #30
    svc #0

    // SYS_DRAW_TEXT(x=20, y=28, str=label, color blanco) -- numero 31
    mov x0, #20
    mov x1, #28
    adr x2, label
    movz x3, #0xFFFF
    movk x3, #0x00FF, lsl #16
    mov x8, #31
    svc #0

    // Nos quedamos vivos, cediendo el control con SYS_PUMP (14) hasta
    // que el usuario cierre la ventana -- si no, con el planificador
    // de tareas, la ventana desapareceria en cuanto _start terminara.
loop:
    mov x8, #14
    svc #0
    cmp x0, #0
    b.lt done      // SYS_PUMP devuelve negativo si la ventana se cerro
    b loop
done:

    ret

.section .rodata
msg:
    .asciz "syscall_test: mensaje enviado via SYS_WRITE_STRING\n"
label:
    .asciz "VIA SYSCALL"
