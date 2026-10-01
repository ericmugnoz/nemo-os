#!/usr/bin/env python3
# GENERA build_otros/otros.npak -- no editar el paquete a mano.
#
# Empaqueta la carpeta "otros programas" del proyecto en UN solo archivo, que
# el kernel lleva dentro como un blob y desempaqueta en NemoFS al arrancar
# (ver src/otros_programas.c).
#
# Por que un paquete y no un blob por archivo, como con Lua:
#
#   1. Cada programa vive en SU carpeta porque usa Include con nombres
#      relativos ("tetris.nb" incluye "tetris_logica.nb"): sus piezas tienen
#      que acabar juntas, no sueltas en una carpeta comun.
#   2. Los nombres llevan espacios ("otros programas", "nemo plataforma
#      demo") y make parte las dependencias por espacios. Con un paquete, el
#      Makefile solo ve una ruta sin espacios: build_otros/otros.npak.
#   3. Añadir un programa nuevo no toca ni el Makefile ni el C: se deja la
#      carpeta dentro de "otros programas" y se reconstruye.
#
# Formato (todo little-endian, sin relleno):
#
#   "NPAK"            4 bytes
#   version           u32   (1)
#   n_entradas        u32
#   por entrada:
#       len_ruta      u16   ruta relativa con '/' como separador
#       ruta          len_ruta bytes (sin terminador)
#       len_datos     u32
#       datos         len_datos bytes
#
# Las rutas se guardan tal cual, con sus espacios: quien las parte es el
# kernel, que crea las carpetas que hagan falta.

import os
import struct
import sys

# Lo que NemoFS aguanta por nombre (ver NEMOFS_MAX_NAME en src/nemofs.h).
# Se comprueba AQUI, al construir, y no en el arranque: un nombre demasiado
# largo tiene que romper la compilacion en el Mac, no dejar un archivo sin
# instalar en la tarjeta de alguien.
NEMOFS_MAX_NAME = 31

# Basura de Finder y compañia: no tiene sentido llevarla a la Pi.
IGNORAR = {".DS_Store", "Thumbs.db", ".localized"}


def se_ignora(nombre):
    return nombre in IGNORAR or nombre.startswith("._")


def recoger(raiz):
    """Lista de (ruta_relativa, bytes), ordenada para que el paquete sea
    reproducible: el mismo arbol da siempre el mismo archivo."""
    entradas = []
    for base, dirs, ficheros in os.walk(raiz):
        dirs[:] = sorted(d for d in dirs if not se_ignora(d))
        for f in sorted(ficheros):
            if se_ignora(f):
                continue
            completo = os.path.join(base, f)
            rel = os.path.relpath(completo, raiz).replace(os.sep, "/")
            with open(completo, "rb") as fh:
                entradas.append((rel, fh.read()))
    return entradas


def comprobar(entradas):
    problemas = []
    for rel, _ in entradas:
        for parte in rel.split("/"):
            if len(parte.encode("utf-8")) > NEMOFS_MAX_NAME:
                problemas.append(
                    "'%s': el trozo '%s' pasa de %d bytes"
                    % (rel, parte, NEMOFS_MAX_NAME))
    return problemas


def main():
    if len(sys.argv) != 3:
        print("uso: empaquetar_otros.py <carpeta> <salida.npak>", file=sys.stderr)
        return 2
    raiz, salida = sys.argv[1], sys.argv[2]
    if not os.path.isdir(raiz):
        print("empaquetar_otros: no existe la carpeta '%s'" % raiz, file=sys.stderr)
        return 1

    entradas = recoger(raiz)
    problemas = comprobar(entradas)
    if problemas:
        print("empaquetar_otros: nombres que NemoFS no puede guardar:", file=sys.stderr)
        for p in problemas:
            print("  " + p, file=sys.stderr)
        return 1

    trozos = [b"NPAK", struct.pack("<II", 1, len(entradas))]
    for rel, datos in entradas:
        ruta = rel.encode("utf-8")
        trozos.append(struct.pack("<H", len(ruta)))
        trozos.append(ruta)
        trozos.append(struct.pack("<I", len(datos)))
        trozos.append(datos)
    paquete = b"".join(trozos)

    os.makedirs(os.path.dirname(salida) or ".", exist_ok=True)
    # Solo se reescribe si cambia: asi make no vuelve a enlazar el kernel
    # entero cada vez que se toca cualquier otra cosa.
    if os.path.exists(salida):
        with open(salida, "rb") as fh:
            if fh.read() == paquete:
                print("empaquetar_otros: %s sin cambios (%d archivos, %d bytes)"
                      % (salida, len(entradas), len(paquete)))
                return 0
    with open(salida, "wb") as fh:
        fh.write(paquete)
    print("empaquetar_otros: %s (%d archivos, %d bytes)"
          % (salida, len(entradas), len(paquete)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
