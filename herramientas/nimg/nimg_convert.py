#!/usr/bin/env python3
# nimg_convert.py -- convierte una imagen normal (PNG o JPEG) al
# formato propio "NIMG" que entiende LoadImage() en Nemo OS.
#
# PNG y JPEG los dos funcionan sin ningun cambio especial: Pillow
# detecta el formato leyendo el contenido del archivo, no la
# extension, asi que Image.open() decodifica cualquiera de los dos
# igual de bien. Cualquier otro formato que Pillow sepa abrir
# (BMP, GIF fijo, WEBP...) tambien funcionaria, aunque solo PNG y
# JPEG son los que de verdad hacen falta para fotos normales.
#
# Formato NIMG (todo little-endian):
#   offset 0:  magia "NIMG" (4 bytes, sin terminador nulo)
#   offset 4:  ancho (uint32)
#   offset 8:  alto (uint32)
#   offset 12: pixeles RGBA en crudo, fila a fila (ancho*alto*4 bytes)
#
# Uso:
#   python3 nimg_convert.py entrada.png  salida.nimg [ancho_max alto_max]
#   python3 nimg_convert.py entrada.jpg  salida.nimg [ancho_max alto_max]
#   python3 nimg_convert.py entrada.jpeg salida.nimg [ancho_max alto_max]
#
# Si se dan ancho_max/alto_max, la imagen se reduce (con buen
# remuestreo) para no superar ese tamaño -- el limite real del pool
# de imagenes en el kernel es 256x256, asi que conviene quedarse por
# debajo si el sprite va a tener ese tamaño de sobra.

import sys
import struct
import os

FORMATOS_CONOCIDOS = {".png": "PNG", ".jpg": "JPEG", ".jpeg": "JPEG"}

def main():
    if len(sys.argv) < 3:
        print("uso: python3 nimg_convert.py entrada.(png|jpg|jpeg) salida.nimg [ancho_max alto_max]")
        sys.exit(1)

    src_path = sys.argv[1]
    dst_path = sys.argv[2]
    max_w = int(sys.argv[3]) if len(sys.argv) > 3 else 256
    max_h = int(sys.argv[4]) if len(sys.argv) > 4 else 256

    ext = os.path.splitext(src_path)[1].lower()
    if ext not in FORMATOS_CONOCIDOS:
        print(f"aviso: extension '{ext}' no es .png/.jpg/.jpeg -- se intenta igualmente, "
              f"Pillow detecta el formato por el contenido del archivo")

    try:
        from PIL import Image
    except ImportError:
        print("hace falta Pillow -- instalalo con: pip3 install Pillow")
        sys.exit(1)

    img = Image.open(src_path)
    formato_real = img.format or "desconocido"
    img = img.convert("RGBA")
    w, h = img.size
    print(f"leido {src_path}: formato {formato_real}, {w}x{h}")

    if w > max_w or h > max_h:
        ratio = min(max_w / w, max_h / h)
        new_w = max(1, int(w * ratio))
        new_h = max(1, int(h * ratio))
        img = img.resize((new_w, new_h), Image.LANCZOS)
        w, h = img.size
        print(f"redimensionada a {w}x{h} para no superar {max_w}x{max_h}")

    if w > 256 or h > 256:
        print(f"error: {w}x{h} supera el limite del kernel (256x256)")
        sys.exit(1)

    pixels = img.tobytes("raw", "RGBA")

    with open(dst_path, "wb") as f:
        f.write(b"NIMG")
        f.write(struct.pack("<II", w, h))
        f.write(pixels)

    total = 12 + len(pixels)
    print(f"escrito {dst_path}: {w}x{h}, {total} bytes")

if __name__ == "__main__":
    main()
