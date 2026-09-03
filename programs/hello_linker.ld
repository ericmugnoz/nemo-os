/* hello_linker.ld — programas de prueba para Nemo OS
 * El codigo se coloca en la direccion 0 -- no importa, porque el
 * loader lo copia a donde le convenga en tiempo de ejecucion. Esto
 * es simple pero asume que el codigo no depende de su direccion
 * absoluta (nuestro hello.s no lo hace: solo usa UART0_BASE, un
 * valor fijo, no direcciones relativas a si mismo).
 */
ENTRY(_start)
SECTIONS
{
    . = 0;
    .text : { *(.text.start) *(.text) }
    .rodata : { *(.rodata) }
    .bss : { *(.bss) *(COMMON) }
}
