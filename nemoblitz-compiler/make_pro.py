#!/usr/bin/env python3
# make_pro.py — envuelve un binario plano (el resultado de
# `aarch64-elf-objcopy -O binary ...`) con la cabecera NEXE que espera
# el loader de Nemo OS, produciendo un .pro listo para copiar al
# disco FAT.
#
# Formato NEXE (16 bytes de cabecera + codigo):
#   offset 0:  magic[4]      = "NEXE"
#   offset 4:  version       (u32, little-endian)
#   offset 8:  entry_offset  (u32) -- 0, porque _start esta al principio
#   offset 12: code_size     (u32)
#   offset 16: ... codigo maquina puro ...
#
# Uso: python3 make_pro.py entrada.bin salida.pro

import sys
import struct

if len(sys.argv) != 3:
    print("Uso: python3 make_pro.py entrada.bin salida.pro")
    sys.exit(1)

with open(sys.argv[1], "rb") as f:
    code = f.read()

header = b"NEXE" + struct.pack("<III", 1, 0, len(code))

with open(sys.argv[2], "wb") as f:
    f.write(header)
    f.write(code)

print(f"Escrito {sys.argv[2]}: {len(code)} bytes de codigo + 16 de cabecera")
