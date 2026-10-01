# -*- coding: utf-8 -*-
"""Escribir un sistema de archivos FAT32 a mano, sin herramientas externas.

Hace falta para crear la imagen de tarjeta: la particion de arranque de la Pi
tiene que ser FAT y llevar el firmware, y no queremos depender de que quien
compile tenga mtools ni permisos de administrador.
"""
import struct

SECTOR = 512


def _checksum_corto(nombre11):
    s = 0
    for c in nombre11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def _nombre_corto(nombre, usados):
    """El nombre 8.3 clasico. Si no cabe, se acorta con ~1, ~2..."""
    base, _, ext = nombre.rpartition(".")
    if not base:
        base, ext = nombre, ""
    limpio = "".join(c for c in base.upper() if c.isalnum() or c in "_-")[:8] or "ARCHIVO"
    ext = "".join(c for c in ext.upper() if c.isalnum())[:3]
    corto = limpio
    if len(base) > 8 or any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in base.upper()):
        n = 1
        while True:
            corto = (limpio[:6] + "~" + str(n))[:8]
            if corto + "." + ext not in usados:
                break
            n += 1
    usados.add(corto + "." + ext)
    return (corto.ljust(8) + ext.ljust(3)).encode("ascii")


def _entradas_lfn(nombre, corto11):
    """Las entradas de nombre largo, del ultimo trozo al primero."""
    cs = _checksum_corto(corto11)
    trozos = [nombre[i:i + 13] for i in range(0, len(nombre), 13)]
    salida = []
    for idx in range(len(trozos), 0, -1):
        t = trozos[idx - 1]
        chars = [ord(c) for c in t]
        if len(chars) < 13:
            chars.append(0)                       # el fin de cadena
            while len(chars) < 13:
                chars.append(0xFFFF)              # relleno
        e = bytearray(32)
        e[0] = idx | (0x40 if idx == len(trozos) else 0)
        e[11] = 0x0F
        e[13] = cs
        posiciones = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
        for k, p in enumerate(posiciones):
            e[p] = chars[k] & 0xFF
            e[p + 1] = (chars[k] >> 8) & 0xFF
        salida.append(bytes(e))
    return salida


class Fat32:
    """Un FAT32 vacio al que se le añaden archivos y carpetas."""

    def __init__(self, sectores, etiqueta="NEMOOS"):
        self.sectores = sectores
        self.etiqueta = etiqueta
        self.sectores_por_cluster = 8 if sectores < 16 * 1024 * 1024 else 32   # 4 KB o 16 KB
        self.reservados = 32
        self.num_fats = 2
        # cuantos clusters caben, y cuanto ocupa cada FAT (se resuelve iterando)
        self.sectores_fat = 1
        for _ in range(8):
            datos = sectores - self.reservados - self.num_fats * self.sectores_fat
            clusters = datos // self.sectores_por_cluster
            nuevo = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if nuevo == self.sectores_fat:
                break
            self.sectores_fat = nuevo
        self.inicio_datos = self.reservados + self.num_fats * self.sectores_fat
        self.total_clusters = (sectores - self.inicio_datos) // self.sectores_por_cluster
        self.fat = [0] * (self.total_clusters + 2)
        self.fat[0], self.fat[1] = 0x0FFFFFF8, 0x0FFFFFFF
        self.clusters = {}                        # numero -> bytes
        self.siguiente_libre = 3                  # el 2 es la raiz
        self.fat[2] = 0x0FFFFFFF
        self.raiz = []                            # entradas de directorio

    # ---- reserva de clusters ----
    def _reservar(self, cuantos):
        lista = []
        for _ in range(cuantos):
            if self.siguiente_libre >= self.total_clusters + 2:
                raise RuntimeError("la particion FAT se ha quedado sin sitio")
            lista.append(self.siguiente_libre)
            self.siguiente_libre += 1
        for i, c in enumerate(lista):
            self.fat[c] = 0x0FFFFFFF if i == len(lista) - 1 else lista[i + 1]
        return lista

    def _escribir_datos(self, datos):
        tam_cluster = self.sectores_por_cluster * SECTOR
        n = max(1, (len(datos) + tam_cluster - 1) // tam_cluster)
        lista = self._reservar(n)
        for i, c in enumerate(lista):
            self.clusters[c] = datos[i * tam_cluster:(i + 1) * tam_cluster]
        return lista[0]

    # ---- entradas de directorio ----
    def _entrada(self, nombre, primer_cluster, tamano, atributos, usados):
        corto = _nombre_corto(nombre, usados)
        # El nombre 8.3 va SIEMPRE en mayusculas. Si el de verdad no es
        # exactamente ese (minusculas, mas de 8 letras, varios puntos...), se
        # añaden las entradas de nombre largo para conservarlo tal cual.
        base = corto[:8].decode("ascii").strip()
        ext = corto[8:11].decode("ascii").strip()
        como_queda = base + ("." + ext if ext else "")
        entradas = b""
        if como_queda != nombre:
            for e in _entradas_lfn(nombre, corto):
                entradas += e
        e = bytearray(32)
        e[0:11] = corto
        e[11] = atributos
        e[20:22] = struct.pack("<H", (primer_cluster >> 16) & 0xFFFF)
        e[26:28] = struct.pack("<H", primer_cluster & 0xFFFF)
        e[28:32] = struct.pack("<I", tamano)
        e[22:24] = struct.pack("<H", 0)           # hora
        e[24:26] = struct.pack("<H", 0x5279)      # fecha, da igual
        return entradas + bytes(e)

    def anadir_archivo(self, nombre, datos, carpeta=None):
        primer = self._escribir_datos(datos) if datos else 0
        destino = self.raiz if carpeta is None else carpeta["entradas"]
        usados = self.raiz_usados if carpeta is None else carpeta["usados"]
        destino.append(self._entrada(nombre, primer, len(datos), 0x20, usados))

    def crear_carpeta(self, nombre):
        cluster = self._reservar(1)[0]
        carpeta = {"cluster": cluster, "entradas": [], "usados": set()}
        self.raiz.append(self._entrada(nombre, cluster, 0, 0x10, self.raiz_usados))
        # "." y ".."
        p = bytearray(32); p[0:11] = b".          "; p[11] = 0x10
        p[26:28] = struct.pack("<H", cluster & 0xFFFF); p[20:22] = struct.pack("<H", cluster >> 16)
        pp = bytearray(32); pp[0:11] = b"..         "; pp[11] = 0x10
        carpeta["entradas"] = [bytes(p), bytes(pp)]
        self.carpetas.append(carpeta)
        return carpeta

    raiz_usados = None
    carpetas = None

    def construir(self):
        """Devuelve los bytes de la particion entera."""
        tam_cluster = self.sectores_por_cluster * SECTOR
        # las carpetas, a sus clusters
        for c in self.carpetas:
            datos = b"".join(c["entradas"])
            datos += b"\0" * (tam_cluster - len(datos) % tam_cluster if len(datos) % tam_cluster else 0)
            if len(datos) > tam_cluster:
                raise RuntimeError("carpeta demasiado grande para un cluster")
            self.clusters[c["cluster"]] = datos
        # la raiz (cluster 2, encadenando si hace falta)
        etiqueta = bytearray(32)
        etiqueta[0:11] = self.etiqueta.upper().ljust(11)[:11].encode("ascii")
        etiqueta[11] = 0x08
        datos_raiz = bytes(etiqueta) + b"".join(self.raiz)
        cadena = [2]
        while len(datos_raiz) > len(cadena) * tam_cluster:
            nuevo = self._reservar(1)[0]
            self.fat[cadena[-1]] = nuevo
            self.fat[nuevo] = 0x0FFFFFFF
            cadena.append(nuevo)
        for i, c in enumerate(cadena):
            self.clusters[c] = datos_raiz[i * tam_cluster:(i + 1) * tam_cluster]

        imagen = bytearray(self.sectores * SECTOR)
        # sector de arranque
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"MSWIN4.1"
        struct.pack_into("<H", bs, 11, SECTOR)
        bs[13] = self.sectores_por_cluster
        struct.pack_into("<H", bs, 14, self.reservados)
        bs[16] = self.num_fats
        struct.pack_into("<H", bs, 17, 0)          # raiz fija: 0 en FAT32
        struct.pack_into("<H", bs, 19, 0)
        bs[21] = 0xF8
        struct.pack_into("<H", bs, 22, 0)          # FAT de 16 bits: 0
        struct.pack_into("<H", bs, 24, 63)         # sectores por pista
        struct.pack_into("<H", bs, 26, 255)        # cabezas
        struct.pack_into("<I", bs, 28, 2048)       # sectores ocultos (donde empieza la particion)
        struct.pack_into("<I", bs, 32, self.sectores)
        struct.pack_into("<I", bs, 36, self.sectores_fat)
        struct.pack_into("<H", bs, 40, 0)          # banderas
        struct.pack_into("<H", bs, 42, 0)          # version
        struct.pack_into("<I", bs, 44, 2)          # cluster de la raiz
        struct.pack_into("<H", bs, 48, 1)          # FSInfo
        struct.pack_into("<H", bs, 50, 6)          # copia del sector de arranque
        bs[64] = 0x80
        bs[66] = 0x29
        struct.pack_into("<I", bs, 67, 0x4E454D4F)
        bs[71:82] = self.etiqueta.upper().ljust(11)[:11].encode("ascii")
        bs[82:90] = b"FAT32   "
        bs[510:512] = b"\x55\xAA"
        imagen[0:SECTOR] = bs
        imagen[6 * SECTOR:7 * SECTOR] = bs         # la copia

        # FSInfo
        fs = bytearray(SECTOR)
        struct.pack_into("<I", fs, 0, 0x41615252)
        struct.pack_into("<I", fs, 484, 0x61417272)
        struct.pack_into("<I", fs, 488, self.total_clusters - (self.siguiente_libre - 2))
        struct.pack_into("<I", fs, 492, self.siguiente_libre)
        fs[510:512] = b"\x55\xAA"
        imagen[SECTOR:2 * SECTOR] = fs

        # las dos FAT
        tabla = bytearray()
        for v in self.fat:
            tabla += struct.pack("<I", v & 0x0FFFFFFF)
        tabla += b"\0" * (self.sectores_fat * SECTOR - len(tabla))
        for n in range(self.num_fats):
            off = (self.reservados + n * self.sectores_fat) * SECTOR
            imagen[off:off + len(tabla)] = tabla

        # los datos
        for c, datos in self.clusters.items():
            off = (self.inicio_datos + (c - 2) * self.sectores_por_cluster) * SECTOR
            imagen[off:off + len(datos)] = datos
        return bytes(imagen)


def nuevo(sectores, etiqueta="NEMOOS"):
    f = Fat32(sectores, etiqueta)
    f.raiz_usados = set()
    f.carpetas = []
    return f
