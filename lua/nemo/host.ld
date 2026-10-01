/* host.ld — PIE estatico SIN libc para Linux x86-64, con la lista de
 * reubicaciones dentro del binario para que _start la aplique. */
ENTRY(_start)
PHDRS { load PT_LOAD FILEHDR PHDRS FLAGS(7); dyn PT_DYNAMIC FLAGS(6); }
SECTIONS
{
    . = SIZEOF_HEADERS;
    .text     : { *(.text.start) *(.text .text.*) } :load
    .rodata   : { *(.rodata .rodata.*) } :load
    .rela.dyn : { __rela_start = .; *(.rela.dyn) *(.rela.*) __rela_end = .; } :load
    .dynamic  : { *(.dynamic) } :load :dyn
    .dynsym   : { *(.dynsym) } :load
    .dynstr   : { *(.dynstr) } :load
    .hash     : { *(.hash) } :load
    .gnu.hash : { *(.gnu.hash) } :load
    .got      : { *(.got .got.*) } :load
    .data     : { *(.data .data.*) } :load
    .bss      : { *(.bss .bss.*) *(COMMON) } :load
    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame*) *(.interp) }
}
