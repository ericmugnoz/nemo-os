/* lua.ld — LUA.PRO para Nemo OS.
 *
 * Programa PIE (enlazado con -pie): el binario plano empieza en _start
 * (enlazado en 0, SIN cabecera ELF delante -- por eso PHDRS explicito
 * sin FILEHDR: el loader de Nemo OS salta al primer byte del blob) y
 * lleva dentro la lista de reubicaciones (.rela.dyn) que _start aplica
 * al arrancar. Asi las tablas de punteros de Lua funcionan cargue el
 * programa donde cargue. .bss lleva el pool de 12 MB y la pila de 1 MB;
 * lo pone a cero el loader. */
ENTRY(_start)
PHDRS { load PT_LOAD FLAGS(7); dyn PT_DYNAMIC FLAGS(6); }
SECTIONS
{
    . = 0;
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
