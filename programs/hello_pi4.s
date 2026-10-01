// hello_pi4.s — version para Raspberry Pi 4 de hello.s
//
// Identico en espiritu al hello.s original de QEMU: un programa
// minimo que toca hardware directamente, sin pasar por ninguna
// syscall, como prueba de que el mecanismo de carga y ejecucion
// del loader funciona. La unica diferencia real es la direccion
// del UART -- aqui la de la Pi 4 (PL011 en 0xFE201000, verificada
// en las Fases 1-2 de este mismo port), no la de QEMU (0x09000000).
//
// Nota: como en QEMU, este programa NO puede llamar a uart_init()
// primero -- asume que el UART ya esta inicializado (por el kernel,
// al arrancar), y solo escribe directamente al registro de datos.

.section .text
.global _start

_start:
    movz x0, #0xFE20, lsl #16   // x0 = 0xFE200000
    movk x0, #0x1000             // x0 = 0xFE201000 (UART0_BASE de la Pi 4)

    mov w1, #0x4E                // 'N'
    strb w1, [x0]
    mov w1, #0x65                // 'e'
    strb w1, [x0]
    mov w1, #0x78                // 'x'
    strb w1, [x0]
    mov w1, #0x21                // '!'
    strb w1, [x0]
    mov w1, #0x0A                // '\n'
    strb w1, [x0]

    ret                          // vuelve al loader que nos llamo
