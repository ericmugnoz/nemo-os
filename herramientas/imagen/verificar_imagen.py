# -*- coding: utf-8 -*-
"""Lee una imagen de tarjeta de Nemo OS y comprueba que esta bien hecha.

    python3 herramientas/imagen/verificar_imagen.py nemo-os.img [carpeta_original]

Esta escrito APARTE del creador y leyendo el formato por su cuenta: si los dos
compartieran codigo, compartirian tambien los errores. Comprueba la tabla de
particiones, recorre el FAT32 siguiendo sus cadenas de clusters, y si se le da
la carpeta original, compara cada archivo byte a byte.
"""
import os, struct, sys

SECTOR = 512


def leer_mbr(f):
    f.seek(0)
    s = f.read(SECTOR)
    if s[510] != 0x55 or s[511] != 0xAA:
        raise SystemExit("  MAL: el sector 0 no tiene la firma 55AA")
    salida = []
    for i in range(4):
        e = s[446 + 16 * i:446 + 16 * i + 16]
        tipo = e[4]
        lba, n = struct.unpack("<II", e[8:16])
        if tipo and n:
            salida.append((i, tipo, lba, n))
    return salida


def leer_fat32(f, inicio_sector):
    base = inicio_sector * SECTOR
    f.seek(base)
    bs = f.read(SECTOR)
    if bs[510] != 0x55 or bs[511] != 0xAA:
        raise SystemExit("  MAL: la particion de arranque no parece FAT")
    bytes_por_sector = struct.unpack_from("<H", bs, 11)[0]
    por_cluster = bs[13]
    reservados = struct.unpack_from("<H", bs, 14)[0]
    num_fats = bs[16]
    sect_fat = struct.unpack_from("<I", bs, 36)[0]
    raiz = struct.unpack_from("<I", bs, 44)[0]
    if bs[82:90] != b"FAT32   ":
        raise SystemExit("  MAL: no dice ser FAT32")
    inicio_datos = reservados + num_fats * sect_fat

    f.seek(base + reservados * SECTOR)
    tabla = f.read(sect_fat * SECTOR)

    def cadena(primero):
        c, vistos = primero, []
        while 2 <= c < 0x0FFFFFF8:
            if c in vistos:
                raise SystemExit("  MAL: la cadena de clusters da vueltas")
            vistos.append(c)
            c = struct.unpack_from("<I", tabla, c * 4)[0] & 0x0FFFFFFF
        return vistos

    def datos(primero, tam=None):
        out = b""
        for c in cadena(primero):
            f.seek(base + (inicio_datos + (c - 2) * por_cluster) * SECTOR)
            out += f.read(por_cluster * bytes_por_sector)
        return out[:tam] if tam is not None else out

    def leer_dir(primero, prefijo=""):
        archivos = {}
        crudo = datos(primero)
        largo = []
        for off in range(0, len(crudo), 32):
            e = crudo[off:off + 32]
            if not e or e[0] == 0:
                break
            if e[0] == 0xE5:
                largo = []
                continue
            if e[11] == 0x0F:                       # trozo de nombre largo
                pos = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
                t = ""
                for p in pos:
                    ch = struct.unpack_from("<H", e, p)[0]
                    if ch in (0, 0xFFFF):
                        break
                    t += chr(ch)
                largo.insert(0, t) if (e[0] & 0x40) == 0 else largo.insert(0, t)
                continue
            if e[11] & 0x08:                        # la etiqueta del volumen
                largo = []
                continue
            nombre = "".join(largo) if largo else (e[0:8].decode("ascii").strip() +
                      ("." + e[8:11].decode("ascii").strip() if e[8:11].strip() else ""))
            largo = []
            cl = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
            tam = struct.unpack_from("<I", e, 28)[0]
            if e[11] & 0x10:                        # carpeta
                if nombre in (".", ".."):
                    continue
                archivos.update(leer_dir(cl, prefijo + nombre + "/"))
            else:
                archivos[prefijo + nombre] = datos(cl, tam) if cl else b""
        return archivos

    return leer_dir(raiz), {"por_cluster": por_cluster, "sect_fat": sect_fat, "reservados": reservados}


def main():
    if len(sys.argv) < 2:
        raise SystemExit("uso: verificar_imagen.py imagen.img [carpeta_original]")
    ruta = sys.argv[1]
    original = sys.argv[2] if len(sys.argv) > 2 else None
    malos = 0

    def ok(que, bien):
        nonlocal malos
        print("  %-56s %s" % (que, "bien" if bien else "MAL"))
        if not bien:
            malos += 1

    with open(ruta, "rb") as f:
        tam = os.path.getsize(ruta)
        parts = leer_mbr(f)
        print("  tabla de particiones:")
        for i, tipo, lba, n in parts:
            print("    %d: tipo 0x%02X  desde el sector %d  %d MB" % (i + 1, tipo, lba, n * SECTOR // (1024 * 1024)))
        ok("hay exactamente dos particiones", len(parts) == 2)
        if len(parts) != 2:
            raise SystemExit(1)
        (_, t1, lba1, n1), (_, t2, lba2, n2) = parts
        ok("la primera es FAT32 (tipo 0x0C)", t1 == 0x0C)
        ok("la segunda no es FAT (es de NemoFS)", t2 not in (0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E))
        ok("empiezan alineadas a 1 MB", lba1 % 2048 == 0 and lba2 % 2048 == 0)
        ok("no se solapan", lba1 + n1 <= lba2)
        ok("caben en la imagen", (lba2 + n2) * SECTOR <= tam)
        ok("la de NemoFS esta a ceros (la formatea el sistema)", (lambda: (f.seek(lba2 * SECTOR), f.read(64 * 1024))[1].count(0) == 64 * 1024)())

        archivos, info = leer_fat32(f, lba1)
        print("  archivos en la particion de arranque: %d" % len(archivos))
        for n in sorted(archivos):
            print("    %-30s %8d bytes" % (n, len(archivos[n])))
        ok("esta el kernel", "kernel8.img" in archivos)
        ok("esta config.txt", "config.txt" in archivos)
        ok("esta la nota de version", "VERSION.TXT" in archivos)

        if original:
            iguales = True
            for raiz, _, nombres in os.walk(original):
                rel = os.path.relpath(raiz, original)
                for nombre in nombres:
                    if nombre.startswith("."):
                        continue
                    clave = nombre if rel == "." else rel + "/" + nombre
                    esperado = open(os.path.join(raiz, nombre), "rb").read()
                    if archivos.get(clave) != esperado:
                        print("    difiere o falta: %s" % clave)
                        iguales = False
            ok("todos los archivos originales estan, byte a byte", iguales)

    print("  %s" % ("imagen correcta" if malos == 0 else "HAY FALLOS"))
    return malos


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
