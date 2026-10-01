#!/usr/bin/env python3
# nemo_sprites.py -- convierte la hoja de poses del capitan Nemo (un render
# grande, sin rejilla y sin canal alfa) en una hoja de sprites de CELDAS
# IGUALES en formato NIMG, que es lo unico que sabe leer LoadAnimImage.
#
# El problema de partida: la imagen de referencia mide 2752x1536, no tiene
# transparencia (el damero esta PINTADO) y sus figuras no estan alineadas a
# ninguna rejilla -- cada pose ocupa lo que ocupa y esta donde esta.
#
# Lo que hace:
#   1. separa figura y fondo (el fondo es casi blanco o gris muy claro)
#   2. localiza cada pose como una mancha conexa
#   3. recorta cada una a su caja justa
#   4. la reduce con UNA SOLA escala para todas (la de la figura mas alta),
#      y la pega centrada y APOYADA EN EL SUELO de la celda. Las dos cosas
#      importan: con una escala por pose, el capitan ENCOGE al saltar o al
#      golpear, porque esas poses son mas anchas; sin apoyarlo en el suelo,
#      da saltitos al cambiar de fotograma.
#   5. reduce la paleta, para que parezca pixel art y no una foto encogida
#   6. pinta el fondo de magenta, el color que MaskImage tratara como
#      transparente
#
# Uso:  python3 nemo_sprites.py <perfil> hoja.png salida.nimg
#   perfil "nemo"    -> el capitan, celdas de 32x32
#   perfil "bichos"  -> cangrejo y moneda, celdas de 16x16 (un tile)

import sys, struct
from PIL import Image
import numpy as np
from scipy import ndimage

# Cada perfil dice: tamaño de celda, que figuras de la hoja original van
# en cada fotograma, y si ademas se añaden espejadas para el otro sentido.
PERFILES = {
    "nemo":   { "celda": (32, 32),
                # 0-3 quieto | 4-7 andar | 8 saltar | 9 golpear
                "grupos": [[0, 1, 2, 3, 7, 8, 9, 10, 6, 17]],
                "espejo": True },
    "bichos": { "celda": (16, 16),
                # Una escala POR GRUPO: si se comparte entre todos, el
                # murcielago (el mas ancho de la hoja) encoge al cangrejo
                # y a la moneda. Dentro de un grupo si tiene que ser la
                # misma, para que el bicho no cambie de tamaño al animarse.
                "grupos": [[0, 1, 2, 3],        # 0-3  cangrejo
                           [26, 27, 28, 29],    # 4-7  moneda
                           [20, 21, 22, 23]],   # 8-11 murcielago
                "espejo": True, "minimo": 2500 },
}
MASCARA = (255, 0, 255)       # el magenta de MaskImage
COLORES = 24                  # paleta final

# Que figura va en cada fotograma, por su orden de deteccion (fila, luego x).
# 0-3 quieto | 7-10 andando | 6 saltando | 4 corriendo | 17 golpeando
# Los fotogramas espejados van en la MISMA hoja a proposito: el kernel no
# sabe dibujar una imagen del reves, y tener dos imagenes gastaria dos de
# los huecos y obligaria a elegir cual dibujar en cada sitio. Asi solo
# cambia el numero de fotograma: + (cuantos haya) para el otro lado.

def figuras(img, minimo=4000):
    a = np.asarray(img.convert('RGB')).astype(int)
    fondo = (a.min(axis=2) > 200) & (a.max(axis=2) - a.min(axis=2) < 25)
    obj = ndimage.binary_closing(~fondo, np.ones((9, 9)))
    lab, n = ndimage.label(obj)
    tam = ndimage.sum(obj, lab, range(1, n + 1))
    cajas = ndimage.find_objects(lab)
    out = [(int(t), c, i + 1) for i, (t, c) in enumerate(zip(tam, cajas)) if t > minimo]
    # orden de lectura: por bandas horizontales y luego de izquierda a derecha
    out.sort(key=lambda x: (x[1][0].start // 160, x[1][1].start))
    return out, lab

def recortar(img, lab, caja, etiqueta):
    y0, y1 = caja[0].start, caja[0].stop
    x0, x1 = caja[1].start, caja[1].stop
    trozo = img.crop((x0, y0, x1, y1)).convert('RGB')
    # solo los pixeles DE ESTA figura: si dos poses se rozan, la caja de una
    # puede pillar un brazo de la otra
    mia = (lab[y0:y1, x0:x1] == etiqueta)
    px = np.asarray(trozo).copy()
    px[~mia] = MASCARA
    return Image.fromarray(px.astype('uint8')), mia

def a_celda(trozo, mia, escala):
    nw = max(1, int(round(trozo.width * escala)))
    nh = max(1, int(round(trozo.height * escala)))
    # BOX (media de area) en vez de NEAREST: encoger 1/13 con NEAREST se come
    # los detalles finos -- la galoneadura de la gorra desaparece entera
    peq = trozo.resize((nw, nh), Image.BOX)
    msk = Image.fromarray((mia * 255).astype('uint8')).resize((nw, nh), Image.BOX)
    celda = Image.new('RGB', (CELDA_W, CELDA_H), MASCARA)
    ox = (CELDA_W - nw) // 2
    oy = CELDA_H - nh                      # apoyado en el suelo de la celda
    celda.paste(peq, (ox, oy), Image.fromarray((np.asarray(msk) > 110).astype('uint8') * 255, 'L'))
    return celda

def main():
    global CELDA_W, CELDA_H, COLUMNAS, ORDEN
    perfil, entrada, salida = sys.argv[1], sys.argv[2], sys.argv[3]
    cfg = PERFILES[perfil]
    CELDA_W, CELDA_H = cfg["celda"]
    grupos = cfg["grupos"]
    ORDEN = [i for g in grupos for i in g]
    COLUMNAS = len(ORDEN) * (2 if cfg["espejo"] else 1)
    espejo = cfg["espejo"]
    img = Image.open(entrada)
    figs, lab = figuras(img, cfg.get('minimo', 4000))
    print(f"{len(figs)} figuras en la hoja")

    # Una escala por grupo: la que hace que la pose mas grande del grupo
    # llene la celda. Todas las poses de un mismo bicho comparten escala.
    escala_de = {}
    for g in grupos:
        alto_max = max(figs[o][1][0].stop - figs[o][1][0].start for o in g)
        ancho_max = max(figs[o][1][1].stop - figs[o][1][1].start for o in g)
        e = min(CELDA_H / alto_max, CELDA_W / ancho_max)
        for o in g: escala_de[o] = e
        print(f"grupo de {len(g)}: mas alta {alto_max}px, mas ancha {ancho_max}px -> escala {e:.4f}")

    hoja = Image.new('RGB', (CELDA_W * COLUMNAS, CELDA_H), MASCARA)
    for destino, origen in enumerate(ORDEN):
        t, caja, etiqueta = figs[origen]
        trozo, mia = recortar(img, lab, caja, etiqueta)
        celda = a_celda(trozo, mia, escala_de[origen])
        hoja.paste(celda, (destino * CELDA_W, 0))
        if espejo:
            hoja.paste(celda.transpose(Image.FLIP_LEFT_RIGHT), ((destino + len(ORDEN)) * CELDA_W, 0))

    # Paleta corta: asi parece dibujado a mano y no una foto encogida.
    # El magenta se conserva porque es de los colores mas repetidos.
    hoja = hoja.quantize(colors=COLORES, method=Image.MEDIANCUT, dither=Image.NONE).convert('RGB')
    px = np.asarray(hoja).copy()
    # tras la cuantizacion el magenta puede haberse movido un pelo: se vuelve
    # a fijar EXACTO, porque MaskImage compara el color entero, no parecido
    cerca = (np.abs(px.astype(int) - np.array(MASCARA)).sum(axis=2) < 60)
    px[cerca] = MASCARA
    hoja = Image.fromarray(px)

    with open(salida, 'wb') as f:
        f.write(b'NIMG' + struct.pack('<II', hoja.width, hoja.height))
        for y in range(hoja.height):
            for x in range(hoja.width):
                r, g, b = hoja.getpixel((x, y))
                f.write(bytes((r, g, b, 255)))
    print(f"{salida}: {hoja.width}x{hoja.height}, {COLUMNAS} celdas de {CELDA_W}x{CELDA_H}")

if __name__ == '__main__':
    main()
