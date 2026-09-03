// hello.s — programa de prueba para el loader de Nemo OS
//
// Este NO es parte del kernel -- se compila por separado y se empaqueta
// como un archivo .pro independiente. No puede llamar a ninguna función
// del kernel (uart_puts, etc.) porque no está enlazado con él; solo
// puede tocar hardware directamente, como hace aquí con el UART.

.section .text
.global _start

_start:
    movz x0, #0x0900, lsl #16   // x0 = 0x09000000 (UART0_BASE)

    mov w1, #0x4E               // 'N'
    strb w1, [x0]
    mov w1, #0x65               // 'e'
    strb w1, [x0]
    mov w1, #0x78                // 'x'
    strb w1, [x0]
    mov w1, #0x21               // '!'
    strb w1, [x0]
    mov w1, #0x0A                // '\n'
    strb w1, [x0]

    ret                          // vuelve al loader que nos llamo
