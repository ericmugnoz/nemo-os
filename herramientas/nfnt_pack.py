#!/usr/bin/env python3
"""
nfnt_pack.py -- Nemo OS
Genera el paquete de fuentes del sistema (FUENTES.NFP) a partir de
fuentes TrueType, pre-rasterizadas con antialiasing.

Por que pre-rasterizar en vez de portar un rasterizador TrueType al
kernel: es la misma decision que NIMG frente a PNG -- el trabajo caro y
complejo (interpretar contornos, hinting, coma flotante) se hace una
vez, aqui, con una herramienta madura (FreeType a traves de Pillow); el
kernel solo copia glifos con mezcla alpha, que es trivial y rapido.

Formato NFNT (una cara), little-endian:
  0   "NFNT"
  4   u16 version = 1
  6   char[8] familia ("sans", "serif", "mono")
  14  u16 tamano (px pedido al rasterizar)
  16  u16 alto_linea (px, distancia recomendada entre lineas)
  18  i16 ascent (px desde la parte de arriba de la linea hasta la base)
  20  u8  negrita, u8 cursiva
  22  u16 num_glifos
  24  u32 offset_datos (desde el inicio del archivo)
  28  u32 reservado
  32  tabla de glifos, num_glifos entradas de 16 bytes, ORDENADA por codepoint:
        u32 codepoint, u8 avance, u8 ancho, u8 alto, i8 dx, i8 dy, u8 pad[3], u32 offset
      (dx, dy: donde se pinta el bitmap respecto al origen del glifo, que
       esta en la esquina superior izquierda de la linea; offset: relativo
       a offset_datos)
  datos: alpha 8 bits por pixel, ancho*alto por glifo, fila a fila

Formato NFNP (paquete):
  0   "NFNP"
  4   u32 num_caras
  8   u32 reservado[2]
  16  directorio: num_caras entradas de 8 bytes: u32 offset, u32 longitud
  luego cada cara, un NFNT completo, alineado a 4 bytes

Cobertura: U+0020..U+00FF (ASCII + Latin-1: acentos, n con tilde, ¿¡ º ª...)
mas unos pocos signos tipograficos habituales en HTML.
"""
import struct, sys, os
from PIL import Image, ImageFont, ImageDraw

EXTRAS = [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AC, 0x2190, 0x2192, 0x2191, 0x2193]
CODEPOINTS = list(range(0x20, 0x100)) + EXTRAS

def rasterizar_cara(ruta_ttf, familia, tamano, negrita, cursiva):
    font = ImageFont.truetype(ruta_ttf, tamano)
    ascent, descent = font.getmetrics()
    alto_linea = ascent + descent
    glifos = []
    datos = bytearray()
    for cp in CODEPOINTS:
        ch = chr(cp)
        # caja del glifo relativa al origen (0,0) = esquina superior izquierda, base en 'ascent'
        try:
            bbox = font.getbbox(ch)  # (x0, y0, x1, y1) con y desde arriba de la linea
        except Exception:
            bbox = None
        avance = int(round(font.getlength(ch)))
        if bbox is None or bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            glifos.append((cp, avance, 0, 0, 0, 0, len(datos)))
            continue
        x0, y0, x1, y1 = bbox
        w, h = x1 - x0, y1 - y0
        img = Image.new("L", (w, h), 0)
        d = ImageDraw.Draw(img)
        d.text((-x0, -y0), ch, font=font, fill=255)
        glifos.append((cp, avance, w, h, x0, y0, len(datos)))
        datos += img.tobytes()
    # cabecera + tabla
    num = len(glifos)
    offset_datos = 32 + 16 * num
    cab = struct.pack("<4sH8sHHhBBHII", b"NFNT", 1, familia.encode("ascii").ljust(8, b"\0"),
                      tamano, alto_linea, ascent, 1 if negrita else 0, 1 if cursiva else 0, num, offset_datos, 0)
    tabla = b"".join(struct.pack("<IBBBbb3xI", cp, min(av,255), min(w,255), min(h,255),
                                 max(-128, min(127, dx)), max(-128, min(127, dy)), off)
                     for (cp, av, w, h, dx, dy, off) in glifos)
    return cab + tabla + bytes(datos)

def empaquetar(caras):
    blobs = [rasterizar_cara(*c) for c in caras]
    dir_off = 16
    cuerpo_off = dir_off + 8 * len(blobs)
    partes, directorio, off = [], [], cuerpo_off
    for b in blobs:
        pad = (-off) % 4
        if pad: partes.append(b"\0" * pad); off += pad
        directorio.append(struct.pack("<II", off, len(b)))
        partes.append(b); off += len(b)
    return struct.pack("<4sIII", b"NFNP", len(blobs), 0, 0) + b"".join(directorio) + b"".join(partes), blobs

if __name__ == "__main__":
    salida = sys.argv[1] if len(sys.argv) > 1 else "FUENTES.NFP"
    dv = "/usr/share/fonts/truetype/dejavu/"
    caras = []
    for t in (10, 12, 14, 16, 20, 24, 32, 40):
        caras.append((dv + "DejaVuSans.ttf", "sans", t, False, False))
        caras.append((dv + "DejaVuSans-Bold.ttf", "sans", t, True, False))
    for t in (12, 14, 16, 20):
        caras.append((dv + "DejaVuSans-Oblique.ttf", "sans", t, False, True))
        caras.append((dv + "DejaVuSans-BoldOblique.ttf", "sans", t, True, True))
    for t in (12, 16, 24, 32):
        caras.append((dv + "DejaVuSerif.ttf", "serif", t, False, False))
        caras.append((dv + "DejaVuSerif-Bold.ttf", "serif", t, True, False))
    for t in (10, 12, 14, 16, 20):
        caras.append((dv + "DejaVuSansMono.ttf", "mono", t, False, False))
        caras.append((dv + "DejaVuSansMono-Bold.ttf", "mono", t, True, False))
    paquete, blobs = empaquetar(caras)
    with open(salida, "wb") as f: f.write(paquete)
    print(f"{salida}: {len(blobs)} caras, {len(paquete)} bytes ({len(paquete)//1024} KB)")
    for c, b in zip(caras, blobs):
        print(f"  {c[1]:5s} {c[2]:3d}px {'negrita ' if c[3] else '        '}{'cursiva' if c[4] else ''}  {len(b)//1024:4d} KB")
