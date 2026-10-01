#!/usr/bin/env python3
# tiles_nemo.py -- genera el tileset del juego del capitan Nemo en NIMG.
#
# Formato NIMG (confirmado en src/syscall.c): cabecera de 12 bytes,
# "NIMG" + ancho + alto como uint32 little-endian, y despues los pixeles
# RGBA en crudo, fila a fila.
#
# La hoja sale de 256x32: 16 columnas x 2 filas = 32 celdas de 16x16.
# El limite del kernel es 256 en cada lado (IMAGE_MAX_DIM), asi que
# caben hasta 16x16 = 256 celdas -- pero una hoja de 256x256 ocuparia
# 262.156 bytes y el explorador de Nemo OS trunca las copias en 262.144.
# Con 256x128 (128 celdas) sigue habiendo sitio de sobra y el archivo
# pesa 131 KB.
#
# La paleta esta muestreada del arte de referencia.

import struct

TILE = 16
COLS = 16
FILAS = 2

# -- paleta, sacada del arte de referencia --
CIELO        = (0x73, 0xCB, 0xE1)
HIERBA_CLARA = (0x7B, 0xC4, 0x4C)
HIERBA       = (0x39, 0x8E, 0x65)
HIERBA_OSC   = (0x27, 0x6B, 0x4A)
TIERRA       = (0x8B, 0x5E, 0x3C)
TIERRA_OSC   = (0x5C, 0x3B, 0x26)
TIERRA_MUY   = (0x3A, 0x1B, 0x21)
ROCA         = (0x6E, 0x6A, 0x78)
ROCA_OSC     = (0x4A, 0x38, 0x60)
LADRILLO     = (0x9A, 0x6B, 0x45)
LADRILLO_OSC = (0x6B, 0x45, 0x2B)
MADERA       = (0x8A, 0x5A, 0x3A)
MADERA_OSC   = (0x5A, 0x38, 0x24)
MADERA_CLARA = (0xA8, 0x74, 0x4C)
ARENA        = (0xEB, 0xD5, 0xA3)
AGUA         = (0x68, 0xD3, 0xB5)
AGUA_CLARA   = (0xB8, 0xEA, 0xC7)
PROFUNDO     = (0x15, 0x20, 0x40)
ALGA         = (0x2E, 0x8B, 0x57)
ALGA_CLARA   = (0x4E, 0xB0, 0x72)
CORAL        = (0xE0, 0x6C, 0x3E)
CORAL_OSC    = (0xA8, 0x45, 0x28)
CRISTAL      = (0x5B, 0xC8, 0xF5)
CRISTAL_OSC  = (0x3A, 0x7A, 0xC8)
LATON        = (0xC9, 0x9A, 0x4E)
LATON_OSC    = (0x8A, 0x66, 0x2E)
NEGRO        = (0x0E, 0x12, 0x1C)

hoja = [[CIELO for _ in range(COLS * TILE)] for _ in range(FILAS * TILE)]

def celda(idx):
    """Devuelve (x0, y0) de la celda idx dentro de la hoja."""
    return (idx % COLS) * TILE, (idx // COLS) * TILE

def pon(idx, x, y, c):
    if 0 <= x < TILE and 0 <= y < TILE:
        x0, y0 = celda(idx)
        hoja[y0 + y][x0 + x] = c

def relleno(idx, c):
    for y in range(TILE):
        for x in range(TILE):
            pon(idx, x, y, c)

def franja(idx, y0, y1, c):
    for y in range(y0, y1):
        for x in range(TILE):
            pon(idx, x, y, c)

def moteado(idx, c, paso=5, desfase=0):
    for y in range(TILE):
        for x in range(TILE):
            if (x * 3 + y * 7 + desfase) % paso == 0:
                pon(idx, x, y, c)

# ---------------------------------------------------------------
# Fila 0
# ---------------------------------------------------------------

# 0 -- cielo
relleno(0, CIELO)

# 1 -- hierba sobre tierra (el suelo de siempre)
relleno(1, TIERRA)
franja(1, 0, 1, HIERBA_CLARA)
franja(1, 1, 3, HIERBA)
franja(1, 3, 4, HIERBA_OSC)
moteado(1, TIERRA_OSC, 6, 2)
for y in range(4, TILE):
    pon(1, 0, y, TIERRA_OSC)

# 2 -- tierra
relleno(2, TIERRA)
moteado(2, TIERRA_OSC, 5, 1)
for y in range(TILE):
    pon(2, 0, y, TIERRA_OSC)

# 3 -- tierra oscura (relleno profundo)
relleno(3, TIERRA_OSC)
moteado(3, TIERRA_MUY, 4, 3)

# 4 -- roca
relleno(4, ROCA)
moteado(4, ROCA_OSC, 4, 0)
for x in range(TILE):
    pon(4, x, 0, ROCA_OSC)
    pon(4, x, TILE - 1, ROCA_OSC)
for y in range(TILE):
    pon(4, 0, y, ROCA_OSC)
    pon(4, TILE - 1, y, ROCA_OSC)

# 5 -- ladrillo
relleno(5, LADRILLO)
for y in (0, 7, 8, 15):
    for x in range(TILE):
        pon(5, x, y, LADRILLO_OSC)
for y in range(1, 7):
    pon(5, 7, y, LADRILLO_OSC)
for y in range(9, 15):
    pon(5, 0, y, LADRILLO_OSC)
    pon(5, 15, y, LADRILLO_OSC)

# 6, 7, 8 -- plataforma de madera: izquierda, centro, derecha
for idx in (6, 7, 8):
    relleno(idx, CIELO)
    for y in range(4, 12):
        for x in range(TILE):
            pon(idx, x, y, MADERA)
    for x in range(TILE):
        pon(idx, x, 4, MADERA_CLARA)
        pon(idx, x, 11, MADERA_OSC)
    for x in range(0, TILE, 5):
        for y in range(5, 11):
            pon(idx, x, y, MADERA_OSC)
for y in range(4, 12):
    pon(6, 0, y, MADERA_OSC)
    pon(8, 15, y, MADERA_OSC)

# 9 -- arena
relleno(9, ARENA)
moteado(9, (0xD0, 0xB8, 0x86), 7, 1)

# 10 -- agua
relleno(10, AGUA)
for x in range(TILE):
    if (x // 3) % 2 == 0:
        pon(10, x, 5, AGUA_CLARA)
        pon(10, x, 12, AGUA_CLARA)

# 11 -- alga (sobre cielo)
relleno(11, CIELO)
for y in range(2, TILE):
    dx = (y * 3) % 5 - 2
    pon(11, 7 + dx, y, ALGA)
    pon(11, 8 + dx, y, ALGA_CLARA)
    if y % 3 == 0:
        pon(11, 5 + dx, y, ALGA)
        pon(11, 10 + dx, y, ALGA)

# 12 -- coral
relleno(12, CIELO)
for y in range(6, TILE):
    for x in range(5, 11):
        pon(12, x, y, CORAL)
for y in range(3, 10):
    pon(12, 4, y, CORAL_OSC); pon(12, 11, y, CORAL_OSC)
    pon(12, 3, y + 2, CORAL); pon(12, 12, y + 2, CORAL)
for x in range(5, 11):
    pon(12, x, 6, CORAL_OSC)

# 13 -- cristal
relleno(13, CIELO)
for y in range(4, 14):
    ancho = (y - 3) // 2
    for x in range(8 - ancho, 8 + ancho):
        pon(13, x, y, CRISTAL)
    pon(13, 8 - ancho, y, CRISTAL_OSC)
    pon(13, 8 + ancho - 1, y, CRISTAL_OSC)
pon(13, 7, 3, CRISTAL); pon(13, 8, 3, CRISTAL)

# 14 -- tubo de laton (vertical)
relleno(14, CIELO)
for y in range(TILE):
    for x in range(3, 13):
        pon(14, x, y, LATON)
    pon(14, 3, y, LATON_OSC)
    pon(14, 12, y, LATON_OSC)
    pon(14, 5, y, (0xE8, 0xC4, 0x7A))
for y in (2, 3, 12, 13):
    for x in range(2, 14):
        pon(14, x, y, LATON_OSC)

# 15 -- ojo de buey
relleno(15, PROFUNDO)
for y in range(TILE):
    for x in range(TILE):
        d = (x - 7.5) ** 2 + (y - 7.5) ** 2
        if d <= 30:
            pon(15, x, y, AGUA_CLARA)
        elif d <= 49:
            pon(15, x, y, LATON)
        elif d <= 60:
            pon(15, x, y, LATON_OSC)

# ---------------------------------------------------------------
# Fila 1
# ---------------------------------------------------------------

# 16 -- fondo profundo
relleno(16, PROFUNDO)
moteado(16, (0x1E, 0x2C, 0x52), 9, 4)

# 17, 18 -- borde de hierba izquierdo y derecho
for idx, borde in ((17, 0), (18, 15)):
    relleno(idx, TIERRA)
    franja(idx, 0, 1, HIERBA_CLARA)
    franja(idx, 1, 3, HIERBA)
    franja(idx, 3, 4, HIERBA_OSC)
    moteado(idx, TIERRA_OSC, 6, 2)
    for y in range(TILE):
        pon(idx, borde, y, TIERRA_MUY)
    for y in range(0, 4):
        pon(idx, borde, y, HIERBA_OSC)

# 19 -- tierra con piedras
relleno(19, TIERRA)
moteado(19, TIERRA_OSC, 5, 1)
for cx, cy in ((4, 5), (11, 9), (7, 12)):
    for y in range(cy, cy + 2):
        for x in range(cx, cx + 3):
            pon(19, x, y, ROCA)

# 20 -- pared de ladrillo oscuro (fondo de ruina)
relleno(20, LADRILLO_OSC)
for y in (0, 8):
    for x in range(TILE):
        pon(20, x, y, TIERRA_MUY)
for y in range(1, 8):
    pon(20, 5, y, TIERRA_MUY)
for y in range(9, 16):
    pon(20, 11, y, TIERRA_MUY)

# 21 -- ancla del Nautilus (adorno)
relleno(21, CIELO)
for y in range(3, 13):
    pon(21, 8, y, ROCA)
for x in range(5, 12):
    pon(21, x, 5, ROCA)
for x in range(4, 13):
    pon(21, x, 12, ROCA)
pon(21, 4, 11, ROCA); pon(21, 12, 11, ROCA)
pon(21, 8, 2, ROCA); pon(21, 7, 2, ROCA); pon(21, 9, 2, ROCA)

# 22-31: cielo, reservadas
for i in range(22, 32):
    relleno(i, CIELO)

# ---------------------------------------------------------------
with open('tiles.nimg', 'wb') as f:
    f.write(b'NIMG' + struct.pack('<II', COLS * TILE, FILAS * TILE))
    for fila in hoja:
        for c in fila:
            f.write(bytes((c[0], c[1], c[2], 255)))

import os
print('tiles.nimg', os.path.getsize(f.name), 'bytes,', COLS * TILE, 'x', FILAS * TILE,
      '=', COLS * FILAS, 'celdas de', TILE)
