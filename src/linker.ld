/* linker.ld
 * Define dónde vive cada sección del kernel en memoria.
 * 0x40080000 es la dirección donde QEMU/UTM (máquina "virt") carga
 * el kernel cuando se usa la opción -kernel.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text.boot : { *(.text.boot) }
    .text      : { *(.text) }
    .rodata    : { *(.rodata) }
    .data      : { *(.data) }

    . = ALIGN(8);
    __bss_start = .;
    .bss : { *(.bss) *(COMMON) }
    __bss_end = .;
}
