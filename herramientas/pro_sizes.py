#!/usr/bin/env python3
"""pro_sizes.py -- cuanta memoria necesita cada programa del sistema.

Lee el tamaño de cada ELF (codigo + datos + .bss) y escribe una
cabecera C con un #define por programa. El instalador de programas del
kernel (src/loader.c) la usa para escribir cabeceras NEXE version 3,
que declaran cuanta memoria necesita cada programa.

Por que hace falta: sin esto, los programas del sistema no declaraban
su tamaño y cada uno recibia 16MB por defecto. Medido de verdad, la
shell necesita 80KB y el editor medio MB -- ocho tareas del sistema se
comian los 128MB de la reserva entera sin necesitarlo.

El dato solo existe en el ELF: al empaquetar con objcopy se pierde el
.bss (no tiene contenido, son ceros). Por eso se calcula aqui, al
compilar, y no a mano: una tabla escrita a mano se quedaria vieja la
primera vez que cambiara un programa.

Uso:
    python3 pro_sizes.py -o salida.h nombre=ruta.elf [nombre=ruta.elf ...]

Lee las cabeceras de seccion del propio ELF, sin depender de
aarch64-elf-size ni de pyelftools.
"""
import struct
import sys


def mem_size_of(path):
    """Suma de las secciones que ocupan memoria al ejecutarse (SHF_ALLOC):
    codigo, datos de solo lectura, datos y .bss."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF':
        raise ValueError(f"{path} no es un ELF")
    if data[4] != 2:
        raise ValueError(f"{path} no es un ELF de 64 bits")
    e_shoff, = struct.unpack_from('<Q', data, 0x28)
    e_shentsize, e_shnum = struct.unpack_from('<HH', data, 0x3A)
    SHF_ALLOC = 0x2
    total = 0
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type, sh_flags = struct.unpack_from('<IQ', data, off + 4)
        sh_size, = struct.unpack_from('<Q', data, off + 32)
        if sh_flags & SHF_ALLOC:
            total += sh_size
    return total


def main():
    args = sys.argv[1:]
    if '-o' not in args:
        print(__doc__)
        sys.exit(1)
    i = args.index('-o')
    salida = args[i + 1]
    pares = args[:i] + args[i + 2:]

    lineas = [
        "// GENERADO por herramientas/pro_sizes.py -- no editar a mano.",
        "// Memoria que necesita cada programa del sistema (codigo + datos",
        "// + .bss), leida de su ELF al compilar. Ver src/loader.c.",
        "#pragma once",
        "",
    ]
    for par in pares:
        nombre, ruta = par.split('=', 1)
        tam = mem_size_of(ruta)
        macro = "PRO_MEM_" + nombre.upper().replace('.', '_')
        lineas.append(f"#define {macro} {tam}u   // {tam / 1024:.1f} KB")
        print(f"  {nombre:16s} {tam / 1024 / 1024:6.2f} MB")
    with open(salida, 'w') as f:
        f.write("\n".join(lineas) + "\n")
    print(f"Tamaños escritos en {salida}")


if __name__ == '__main__':
    main()
