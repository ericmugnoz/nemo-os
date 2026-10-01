/* pi4.ld -- script de enlazado para Raspberry Pi 4
 *
 * Diferencia clave frente a kernel.ld de QEMU (documentado en
 * El Nautilus, pieza por pieza): QEMU carga el kernel en
 * 0x40000000. El firmware real de la Pi 4, en cambio, carga
 * kernel8.img en 0x80000 por convencion -- una direccion fija
 * que el propio firmware GPU respeta siempre que arm_64bit=1.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x80000;

    .text.boot : { KEEP(*(.text.boot)) }
    .text      : { *(.text*) }
    .rodata    : { *(.rodata*) }
    .data      : { *(.data*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.bss*)
        *(COMMON)
    }
    __bss_end = .;

    . = ALIGN(16);
    . += 0x10000;   /* 64 KB de pila inicial, igual que en QEMU */
    stack_top = .;
}
