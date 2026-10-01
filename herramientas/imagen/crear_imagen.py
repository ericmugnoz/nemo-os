# -*- coding: utf-8 -*-
"""Crea la imagen de tarjeta de Nemo OS, lista para el programa de Raspberry Pi.

    python3 herramientas/imagen/crear_imagen.py --arranque carpeta_con_el_firmware

Reparto (decidido para el 1.0):
    - 1 MB sin usar al principio (lo normal, para la tabla y la alineacion)
    - 1 GB de arranque en FAT32: firmware, config.txt, kernel8.img
    - 2 GB de NemoFS, a ceros: el sistema lo formatea SOLO en el primer arranque,
      y ahi reserva su mapa de bloques para la tarjeta ENTERA, de modo que luego
      se pueda estirar hasta el final desde el Particionador.

La imagen se escribe como archivo disperso, asi que ocupa en disco solo lo que
de verdad lleva dentro (unas decenas de MB), aunque mida 3 GB.
"""
import argparse, os, struct, sys, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fat32

SECTOR = 512
MB = 1024 * 1024
TIPO_FAT32_LBA = 0x0C
TIPO_NEMOFS = 0x7F


def mbr(particiones):
    """La tabla de particiones. particiones = [(tipo, primer_sector, sectores)]"""
    s = bytearray(SECTOR)
    for i, (tipo, lba, n) in enumerate(particiones):
        e = 446 + 16 * i
        s[e + 0] = 0x00                       # no arrancable: la Pi no lo mira
        s[e + 1] = 0xFE; s[e + 2] = 0xFF; s[e + 3] = 0xFF     # CHS: relleno
        s[e + 4] = tipo
        s[e + 5] = 0xFE; s[e + 6] = 0xFF; s[e + 7] = 0xFF
        struct.pack_into("<I", s, e + 8, lba)
        struct.pack_into("<I", s, e + 12, n)
    s[510:512] = b"\x55\xAA"
    return bytes(s)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--arranque", required=True, help="carpeta con el firmware, config.txt y kernel8.img")
    p.add_argument("--salida", default="nemo-os.img")
    p.add_argument("--arranque-mb", type=int, default=1024)
    p.add_argument("--nemofs-mb", type=int, default=2048)
    p.add_argument("--version", default="1.0.1")
    a = p.parse_args()

    inicio_arranque = 2048                                   # 1 MB
    sect_arranque = a.arranque_mb * MB // SECTOR
    inicio_nemofs = inicio_arranque + sect_arranque
    sect_nemofs = a.nemofs_mb * MB // SECTOR
    total = inicio_nemofs + sect_nemofs

    # ---- la particion de arranque ----
    fat = fat32.nuevo(sect_arranque, "NEMO-BOOT")
    metidos = []
    for raiz, _, archivos in os.walk(a.arranque):
        rel = os.path.relpath(raiz, a.arranque)
        carpeta = None
        if rel != ".":
            if os.sep in rel:
                print("  aviso: %s se salta (solo una carpeta de profundidad)" % rel)
                continue
            carpeta = fat.crear_carpeta(rel)
        for nombre in sorted(archivos):
            if nombre.startswith("."):
                continue
            datos = open(os.path.join(raiz, nombre), "rb").read()
            fat.anadir_archivo(nombre, datos, carpeta)
            metidos.append((os.path.join(rel, nombre) if rel != "." else nombre, len(datos)))

    # una nota con la version, para saber que tarjeta tiene cada uno
    nota = ("Nemo OS %s\nimagen: arranque %d MB + NemoFS %d MB\n" % (a.version, a.arranque_mb, a.nemofs_mb))
    fat.anadir_archivo("VERSION.TXT", nota.encode("ascii"))
    metidos.append(("VERSION.TXT", len(nota)))

    datos_fat = fat.construir()

    # ---- la imagen, dispersa ----
    TROZO = 1 * MB
    with open(a.salida, "wb") as f:
        f.write(mbr([(TIPO_FAT32_LBA, inicio_arranque, sect_arranque),
                     (TIPO_NEMOFS, inicio_nemofs, sect_nemofs)]))
        # Se escriben SOLO los trozos que llevan algo: los de ceros se saltan y
        # el sistema de archivos del Mac no les da sitio (archivo disperso). Asi
        # una imagen de 3 GB ocupa en disco lo que de verdad lleva dentro.
        base = inicio_arranque * SECTOR
        for off in range(0, len(datos_fat), TROZO):
            trozo = datos_fat[off:off + TROZO]
            if trozo.count(0) == len(trozo):
                continue
            f.seek(base + off)
            f.write(trozo)
        f.truncate(total * SECTOR)               # NemoFS queda a ceros

    real = os.stat(a.salida).st_blocks * 512 if hasattr(os.stat(a.salida), "st_blocks") else 0
    print("  %s" % a.salida)
    print("    tamaño declarado: %d MB" % (total * SECTOR // MB))
    print("    ocupado de verdad: %.1f MB" % (real / MB) if real else "")
    print("    arranque: %d MB en FAT32, con %d archivos" % (a.arranque_mb, len(metidos)))
    print("    NemoFS:   %d MB a ceros (se formatea en el primer arranque)" % a.nemofs_mb)
    for n, t in metidos:
        print("      %-28s %8d bytes" % (n, t))


if __name__ == "__main__":
    main()
