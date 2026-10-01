#!/usr/bin/env python3
# crear_iconos.py -- dibuja la biblioteca de iconos de Nemo OS y la guarda en
# formato NIMG: tiras para la barra de herramientas (CreateToolBar) e iconos
# grandes para paneles (SetPanelImage) o LoadImage.
#
#   python3 crear_iconos.py [carpeta_salida]
#
# Escribe, en la carpeta de salida (por defecto, ./salida):
#   tb_<grupo>16.nimg, tb_<grupo>24.nimg   tiras de iconos de 16 y 24 px
#   ico32_<nombre>.nimg                     iconos sueltos de 32 px, opacos sobre el gris de las ventanas
#   muestra_iconos.png                      hoja con todos, numerados
#   iconos_nimg.c                           los mismos archivos, para incrustar
#
# Formato NIMG: "NIMG", ancho (u32), alto (u32), pixeles RGBA fila a fila.
#
# LA TRANSPARENCIA: la barra de herramientas del kernel no mezcla alfa; toma
# el color del PRIMER pixel de la tira como transparente y no dibuja los
# pixeles de ese color exacto. Asi que cada icono se dibuja grande (64 px),
# se reduce con buen suavizado, y luego:
#   - lo casi transparente pasa a MAGENTA (el color transparente);
#   - los bordes suavizados se mezclan con el fondo sobre el que se veran
#     (el de la barra en las tiras, el de las ventanas en los de 32 px).
import os, sys, struct
from PIL import Image, ImageDraw, ImageFont

G = 64                                  # tamaño de dibujo
MAGENTA = (255, 0, 255)
FONDO_BARRA = (0x30, 0x38, 0x40)        # el fondo de un boton de la barra (gadgets.c)
FONDO_VENTANA = (0xD4, 0xD0, 0xC8)      # el de las ventanas

# Colores
NEGRO, BLANCO, GRIS, GRIS_O = (30, 32, 38), (250, 250, 250), (160, 164, 172), (90, 94, 102)
AZUL, AZUL_C, VERDE, VERDE_C = (40, 110, 210), (120, 180, 250), (40, 160, 70), (120, 220, 130)
ROJO, ROJO_C, AMARILLO, NARANJA = (210, 50, 45), (250, 130, 120), (245, 200, 40), (240, 140, 40)
MARRON, MORADO, CIAN = (150, 100, 50), (140, 80, 200), (40, 180, 190)
T = 4                                   # grosor de trazo a 64 px (1 px a 16)

def lienzo():
    im = Image.new("RGBA", (G, G), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)

# ---------------------------------------------------------------------
# Piezas reutilizables
# ---------------------------------------------------------------------
def pagina(d, x0=14, y0=6, x1=50, y1=58, pliegue=12, relleno=BLANCO):
    d.polygon([(x0, y0), (x1 - pliegue, y0), (x1, y0 + pliegue), (x1, y1), (x0, y1)], fill=relleno, outline=NEGRO, width=T)
    d.line([(x1 - pliegue, y0), (x1 - pliegue, y0 + pliegue), (x1, y0 + pliegue)], fill=NEGRO, width=T)

def flecha(d, dx, dy, color=AZUL):
    # una flecha gorda que apunta en (dx, dy): -1, 0, 1
    import math
    ang = math.atan2(dy, dx)
    def rot(px, py):
        return (32 + px * math.cos(ang) - py * math.sin(ang), 32 + px * math.sin(ang) + py * math.cos(ang))
    puntos = [(-24, -8), (4, -8), (4, -20), (26, 0), (4, 20), (4, 8), (-24, 8)]
    d.polygon([rot(*p) for p in puntos], fill=color, outline=NEGRO, width=T)

def lupa(d, signo=None):
    d.ellipse([8, 8, 42, 42], fill=AZUL_C, outline=NEGRO, width=T + 1)
    d.line([(38, 38), (56, 56)], fill=NEGRO, width=T * 2 + 2)
    if signo:
        d.line([(16, 25), (34, 25)], fill=NEGRO, width=T)
        if signo == "+": d.line([(25, 16), (25, 34)], fill=NEGRO, width=T)

def circulo(d, color):
    d.ellipse([6, 6, 58, 58], fill=color, outline=NEGRO, width=T)

def engranaje(d, color=GRIS):
    import math
    pts = []
    for i in range(16):
        a = i * math.pi / 8
        r = 28 if i % 2 == 0 else 21
        for da in (-0.17, 0.17):
            pts.append((32 + r * math.cos(a + da), 32 + r * math.sin(a + da)))
    d.polygon(pts, fill=color, outline=NEGRO)
    d.ellipse([22, 22, 42, 42], fill=(0, 0, 0, 0), outline=NEGRO, width=T)

def carpeta(d, abierta=False):
    d.polygon([(6, 16), (24, 16), (30, 22), (58, 22), (58, 54), (6, 54)], fill=(220, 170, 50), outline=NEGRO, width=T)
    if abierta:
        d.polygon([(6, 54), (16, 30), (62, 30), (52, 54)], fill=AMARILLO, outline=NEGRO, width=T)
    else:
        d.rectangle([6, 28, 58, 54], fill=AMARILLO, outline=NEGRO, width=T)

def texto(d, s, color=BLANCO, tam=40):
    try:
        f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", tam)
    except OSError:
        f = ImageFont.load_default()
    b = d.textbbox((0, 0), s, font=f)
    d.text(((G - (b[2] - b[0])) / 2 - b[0], (G - (b[3] - b[1])) / 2 - b[1]), s, font=f, fill=color)

# ---------------------------------------------------------------------
# Los iconos
# ---------------------------------------------------------------------
def i_nuevo(d):
    pagina(d)
    d.line([(44, 36), (56, 36)], fill=VERDE, width=T + 2); d.line([(50, 30), (50, 42)], fill=VERDE, width=T + 2)
def i_abrir(d): carpeta(d, True)
def i_guardar(d):
    d.rectangle([8, 8, 56, 56], fill=AZUL, outline=NEGRO, width=T)
    d.rectangle([18, 8, 46, 26], fill=BLANCO, outline=NEGRO, width=T)
    d.rectangle([36, 12, 42, 22], fill=NEGRO)
    d.rectangle([16, 36, 48, 56], fill=GRIS, outline=NEGRO, width=T)
def i_imprimir(d):
    d.rectangle([18, 6, 46, 26], fill=BLANCO, outline=NEGRO, width=T)
    d.rectangle([6, 24, 58, 48], fill=GRIS, outline=NEGRO, width=T)
    d.rectangle([16, 40, 48, 58], fill=BLANCO, outline=NEGRO, width=T)
    d.ellipse([46, 30, 52, 36], fill=VERDE)
def i_cerrar(d):
    d.line([(14, 14), (50, 50)], fill=ROJO, width=T * 3); d.line([(50, 14), (14, 50)], fill=ROJO, width=T * 3)
def i_deshacer(d, espejo=False):
    im, dd = lienzo()
    dd.arc([12, 14, 56, 58], 180, 360, fill=AZUL, width=T * 2 + 2)
    dd.arc([12, 14, 56, 58], 0, 90, fill=AZUL, width=T * 2 + 2)
    dd.polygon([(2, 36), (22, 36), (12, 18)], fill=AZUL, outline=NEGRO)
    return im.transpose(Image.FLIP_LEFT_RIGHT) if espejo else im
def i_rehacer(d): return i_deshacer(d, True)
def i_cortar(d):
    d.line([(20, 6), (40, 40)], fill=GRIS_O, width=T + 2); d.line([(44, 6), (24, 40)], fill=GRIS_O, width=T + 2)
    d.ellipse([8, 38, 28, 58], fill=(0, 0, 0, 0), outline=ROJO, width=T + 1)
    d.ellipse([36, 38, 56, 58], fill=(0, 0, 0, 0), outline=ROJO, width=T + 1)
def i_copiar(d):
    pagina(d, 6, 4, 38, 46, 9); pagina(d, 24, 18, 58, 60, 9)
def i_pegar(d):
    d.rectangle([8, 10, 50, 60], fill=MARRON, outline=NEGRO, width=T)
    d.rectangle([20, 4, 38, 16], fill=GRIS, outline=NEGRO, width=T)
    pagina(d, 24, 24, 58, 62, 9)
def i_borrar(d):
    d.rectangle([14, 20, 50, 60], fill=GRIS, outline=NEGRO, width=T)
    d.rectangle([8, 12, 56, 20], fill=GRIS_O, outline=NEGRO, width=T)
    d.rectangle([26, 6, 38, 12], fill=GRIS_O, outline=NEGRO, width=T)
    for x in (24, 32, 40): d.line([(x, 26), (x, 54)], fill=NEGRO, width=T - 1)
def i_buscar(d): lupa(d)
def i_ejecutar(d): d.polygon([(14, 6), (58, 32), (14, 58)], fill=VERDE, outline=NEGRO, width=T)
def i_pausa(d):
    d.rectangle([12, 8, 26, 56], fill=NARANJA, outline=NEGRO, width=T); d.rectangle([38, 8, 52, 56], fill=NARANJA, outline=NEGRO, width=T)
def i_parar(d): d.rectangle([10, 10, 54, 54], fill=ROJO, outline=NEGRO, width=T)
def i_paso(d):
    d.polygon([(8, 10), (40, 32), (8, 54)], fill=VERDE, outline=NEGRO, width=T); d.rectangle([44, 10, 56, 54], fill=VERDE, outline=NEGRO, width=T)
def i_compilar(d): engranaje(d)
def i_depurar(d):
    d.ellipse([18, 16, 46, 58], fill=VERDE, outline=NEGRO, width=T); d.ellipse([22, 6, 42, 24], fill=VERDE_C, outline=NEGRO, width=T)
    for y in (26, 38, 50):
        d.line([(6, y - 4), (20, y)], fill=NEGRO, width=T); d.line([(58, y - 4), (44, y)], fill=NEGRO, width=T)
def i_atras(d): flecha(d, -1, 0)
def i_adelante(d): flecha(d, 1, 0)
def i_arriba(d): flecha(d, 0, -1)
def i_abajo(d): flecha(d, 0, 1)
def i_inicio(d):
    d.polygon([(32, 4), (60, 30), (4, 30)], fill=ROJO, outline=NEGRO, width=T)
    d.rectangle([12, 28, 52, 58], fill=BLANCO, outline=NEGRO, width=T); d.rectangle([26, 38, 38, 58], fill=MARRON, outline=NEGRO, width=T)
def i_recargar(d):
    d.arc([8, 8, 56, 56], 40, 330, fill=VERDE, width=T * 2 + 2)
    d.polygon([(40, 4), (60, 18), (38, 26)], fill=VERDE, outline=NEGRO)
def i_acercar(d): lupa(d, "+")
def i_alejar(d): lupa(d, "-")
def i_led_on(d):
    for a, b in ((32, 0), (6, 10), (58, 10), (0, 30), (64, 30)):
        d.line([(32, 22), (a, b)], fill=AMARILLO, width=T)
    d.ellipse([18, 10, 46, 38], fill=ROJO, outline=NEGRO, width=T); d.rectangle([18, 24, 46, 44], fill=ROJO, outline=NEGRO, width=T)
    d.rectangle([22, 18, 42, 40], fill=ROJO); d.ellipse([24, 16, 32, 24], fill=ROJO_C)
    d.rectangle([14, 42, 50, 48], fill=ROJO, outline=NEGRO, width=T - 1)
    d.line([(26, 48), (26, 62)], fill=GRIS, width=T); d.line([(38, 48), (38, 58)], fill=GRIS, width=T)
def i_led_off(d):
    d.ellipse([18, 10, 46, 38], fill=GRIS_O, outline=NEGRO, width=T); d.rectangle([18, 24, 46, 44], fill=GRIS_O, outline=NEGRO, width=T)
    d.rectangle([22, 18, 42, 40], fill=GRIS_O)
    d.rectangle([14, 42, 50, 48], fill=GRIS_O, outline=NEGRO, width=T - 1)
    d.line([(26, 48), (26, 62)], fill=GRIS, width=T); d.line([(38, 48), (38, 58)], fill=GRIS, width=T)
def i_pulsador(d):
    d.rectangle([8, 18, 56, 56], fill=NEGRO, outline=GRIS_O, width=T)
    d.ellipse([18, 8, 46, 36], fill=ROJO, outline=NEGRO, width=T)
    for x in (14, 50): d.line([(x, 56), (x, 64)], fill=GRIS, width=T)
def i_pwm(d):
    d.line([(4, 50), (14, 50), (14, 14), (26, 14), (26, 50), (38, 50), (38, 14), (50, 14), (50, 50), (60, 50)], fill=CIAN, width=T + 2)
def i_sensor(d):
    d.rounded_rectangle([24, 4, 40, 44], radius=8, fill=BLANCO, outline=NEGRO, width=T)
    d.ellipse([16, 36, 48, 62], fill=ROJO, outline=NEGRO, width=T); d.rectangle([29, 20, 35, 44], fill=ROJO)
def i_chip(d):
    d.rectangle([14, 8, 50, 56], fill=NEGRO, outline=GRIS_O, width=T - 1)
    for y in (14, 24, 34, 44):
        d.line([(4, y + 3), (14, y + 3)], fill=GRIS, width=T); d.line([(50, y + 3), (60, y + 3)], fill=GRIS, width=T)
    d.ellipse([20, 12, 28, 20], fill=GRIS_O)
def i_enchufe(d):
    d.rounded_rectangle([12, 18, 52, 46], radius=6, fill=GRIS, outline=NEGRO, width=T)
    d.line([(24, 4), (24, 18)], fill=NEGRO, width=T + 2); d.line([(40, 4), (40, 18)], fill=NEGRO, width=T + 2)
    d.line([(32, 46), (32, 62)], fill=NEGRO, width=T + 2)
def i_rayo(d): d.polygon([(36, 2), (12, 36), (30, 36), (24, 62), (52, 24), (34, 24)], fill=AMARILLO, outline=NEGRO, width=T)
def i_info(d): circulo(d, AZUL); texto(d, "i")
def i_aviso(d):
    d.polygon([(32, 4), (62, 58), (2, 58)], fill=AMARILLO, outline=NEGRO, width=T); texto(d, "!", NEGRO, 34)
def i_error(d):
    circulo(d, ROJO); d.line([(22, 22), (42, 42)], fill=BLANCO, width=T + 3); d.line([(42, 22), (22, 42)], fill=BLANCO, width=T + 3)
def i_ok(d):
    circulo(d, VERDE); d.line([(18, 32), (28, 44), (46, 20)], fill=BLANCO, width=T + 3)
def i_ayuda(d): circulo(d, AZUL); texto(d, "?")
def i_ajustes(d):
    for y, x in ((14, 40), (32, 20), (50, 34)):
        d.line([(6, y), (58, y)], fill=GRIS, width=T); d.rectangle([x - 6, y - 8, x + 6, y + 8], fill=AZUL, outline=NEGRO, width=T - 1)
def i_anadir(d):
    d.rectangle([26, 6, 38, 58], fill=VERDE, outline=NEGRO, width=T); d.rectangle([6, 26, 58, 38], fill=VERDE, outline=NEGRO, width=T)
    d.rectangle([30, 10, 34, 54], fill=VERDE)
def i_quitar(d): d.rectangle([6, 24, 58, 40], fill=ROJO, outline=NEGRO, width=T)
def i_carpeta(d): carpeta(d, False)
def i_archivo(d):
    pagina(d)
    for y in (26, 36, 46): d.line([(20, y), (44, y)], fill=GRIS_O, width=T - 1)
def i_imagen(d):
    d.rectangle([4, 10, 60, 54], fill=AZUL_C, outline=NEGRO, width=T)
    d.polygon([(8, 50), (24, 28), (36, 42), (44, 34), (56, 50)], fill=VERDE)
    d.ellipse([40, 14, 50, 24], fill=AMARILLO)
def i_musica(d):
    d.ellipse([6, 40, 26, 56], fill=MORADO, outline=NEGRO, width=T - 1); d.ellipse([36, 34, 56, 50], fill=MORADO, outline=NEGRO, width=T - 1)
    d.line([(24, 48), (24, 10)], fill=NEGRO, width=T); d.line([(54, 42), (54, 4)], fill=NEGRO, width=T)
    d.polygon([(24, 10), (54, 4), (54, 14), (24, 20)], fill=NEGRO)
def i_reloj(d):
    circulo(d, BLANCO); d.line([(32, 32), (32, 14)], fill=NEGRO, width=T); d.line([(32, 32), (46, 38)], fill=NEGRO, width=T)
def i_correo(d):
    d.rectangle([4, 14, 60, 52], fill=BLANCO, outline=NEGRO, width=T); d.line([(4, 14), (32, 36), (60, 14)], fill=NEGRO, width=T)
def i_estrella(d):
    import math
    pts = [(32 + (28 if i % 2 == 0 else 12) * math.sin(i * math.pi / 5), 34 - (28 if i % 2 == 0 else 12) * math.cos(i * math.pi / 5)) for i in range(10)]
    d.polygon(pts, fill=AMARILLO, outline=NEGRO, width=T)

# ---- para el Buscaminas ----
def i_mina(d):
    d.ellipse([14, 14, 50, 50], fill=(40, 42, 50), outline=NEGRO, width=T)      # el cuerpo
    for a, b in ((32, 4), (32, 60), (4, 32), (60, 32)):                          # las puas
        d.line([(32, 32), (a, b)], fill=(40, 42, 50), width=T + 2)
    for a, b in ((12, 12), (52, 12), (12, 52), (52, 52)):
        d.line([(32, 32), (a, b)], fill=(40, 42, 50), width=T + 1)
    d.ellipse([14, 14, 50, 50], fill=(40, 42, 50), outline=NEGRO, width=T)
    d.ellipse([22, 22, 30, 30], fill=(150, 154, 165))                            # el brillo
    d.line([(32, 14), (38, 4)], fill=(120, 90, 40), width=T)                     # la mecha

def i_explosion(d):
    puntas = [(32, 2), (39, 20), (56, 10), (48, 28), (62, 34), (46, 40),
              (56, 58), (38, 48), (32, 62), (26, 48), (10, 58), (18, 40),
              (2, 34), (16, 28), (8, 10), (25, 20)]
    d.polygon(puntas, fill=NARANJA, outline=ROJO)
    d.polygon([(32, 14), (42, 30), (32, 50), (22, 30)], fill=AMARILLO)
    d.ellipse([27, 27, 37, 37], fill=BLANCO)

def i_bandera(d):
    d.line([(24, 8), (24, 54)], fill=(60, 62, 70), width=T)                      # el mastil
    d.rectangle([14, 50, 44, 56], fill=(60, 62, 70))                             # la base
    d.polygon([(26, 10), (50, 20), (26, 30)], fill=ROJO, outline=NEGRO)          # la tela

# ---- para el Arkanoid ----
def i_bola(d):
    d.ellipse([6, 6, 58, 58], fill=(120, 126, 140))               # el borde
    d.ellipse([9, 9, 55, 55], fill=(232, 236, 244))               # el cuerpo
    d.ellipse([30, 30, 55, 55], fill=(196, 202, 214))             # la sombra, abajo a la derecha
    d.ellipse([9, 9, 55, 55], outline=(150, 156, 170), width=2)
    d.ellipse([18, 17, 32, 31], fill=BLANCO)                      # el brillo

def i_corazon(d):
    d.ellipse([10, 12, 34, 36], fill=ROJO)
    d.ellipse([30, 12, 54, 36], fill=ROJO)
    d.polygon([(11, 28), (53, 28), (32, 56)], fill=ROJO)
    d.ellipse([18, 18, 26, 26], fill=ROJO_C)                      # el brillo

def i_capsula(d):
    d.rounded_rectangle([6, 22, 58, 42], radius=10, fill=VERDE, outline=NEGRO, width=T)
    d.rounded_rectangle([10, 26, 30, 32], radius=4, fill=VERDE_C)
    d.line([(32, 26), (32, 38)], fill=BLANCO, width=T)
    d.line([(26, 32), (38, 32)], fill=BLANCO, width=T)

ICONOS = {n[2:]: f for n, f in globals().items() if n.startswith("i_")}

# Las tiras: como mucho 10 iconos, para que la de 24 px quepa en 256 de ancho
GRUPOS = [
    ("basica",    ["nuevo", "abrir", "guardar", "cortar", "copiar", "pegar", "deshacer", "ejecutar", "parar", "ayuda"]),
    ("archivo",   ["nuevo", "abrir", "guardar", "imprimir", "cerrar"]),
    ("edicion",   ["deshacer", "rehacer", "cortar", "copiar", "pegar", "borrar", "buscar"]),
    ("ejecutar",  ["ejecutar", "pausa", "parar", "paso", "compilar", "depurar"]),
    ("navegar",   ["atras", "adelante", "arriba", "abajo", "inicio", "recargar", "acercar", "alejar"]),
    ("nautilus",  ["led_on", "led_off", "pulsador", "pwm", "sensor", "chip", "enchufe", "rayo"]),
    ("avisos",    ["info", "aviso", "error", "ok", "ayuda"]),
    ("varios",    ["ajustes", "anadir", "quitar", "carpeta", "archivo", "imagen", "musica", "reloj", "correo", "estrella"]),
]
# Todos, con el mismo nombre que en las tiras: asi un programa
# puede mostrar en grande el icono del boton que se acaba de pulsar.
SUELTOS_32 = sorted(ICONOS)

def dibujar(nombre):
    im, d = lienzo()
    r = ICONOS[nombre](d)
    return r if isinstance(r, Image.Image) else im

def reducir(im64, tam, fondo, opaco=False):
    """64 px con alfa -> tam px opaco sobre 'fondo'. Lo transparente queda en
    magenta (las tiras: la barra de herramientas salta ese color) o, con
    opaco=True, del color del fondo (los de 32 px: un panel dibuja su imagen
    tal cual, sin transparencias, y el magenta se veria)."""
    p = im64.resize((tam, tam), Image.LANCZOS)
    out = Image.new("RGB", (tam, tam), fondo if opaco else MAGENTA)
    for y in range(tam):
        for x in range(tam):
            r, g, b, a = p.getpixel((x, y))
            if a < 60 and not opaco:
                continue                               # transparente
            k = a / 255
            c = (round(r * k + fondo[0] * (1 - k)), round(g * k + fondo[1] * (1 - k)), round(b * k + fondo[2] * (1 - k)))
            if c == MAGENTA and not opaco: c = (254, 0, 254)   # nunca el color transparente por casualidad
            out.putpixel((x, y), c)
    return out

def a_nimg(im):
    w, h = im.size
    datos = bytearray(b"NIMG" + struct.pack("<II", w, h))
    for y in range(h):
        for x in range(w):
            r, g, b = im.getpixel((x, y))[:3]
            datos += bytes((r, g, b, 255))
    return bytes(datos)

def main():
    salida = sys.argv[1] if len(sys.argv) > 1 else "salida"
    os.makedirs(salida, exist_ok=True)
    archivos = []                                      # (nombre, bytes)
    cache = {}
    def icono64(n):
        if n not in cache: cache[n] = dibujar(n)
        return cache[n]
    for grupo, nombres in GRUPOS:
        for tam in (16, 24):
            tira = Image.new("RGB", (tam * len(nombres), tam), MAGENTA)
            for i, n in enumerate(nombres):
                tira.paste(reducir(icono64(n), tam, FONDO_BARRA), (i * tam, 0))
            assert tira.getpixel((0, 0)) == MAGENTA, grupo   # el primer pixel es el transparente
            assert tira.size[0] <= 256
            archivos.append((f"tb_{grupo}{tam}.nimg", a_nimg(tira)))
    for n in SUELTOS_32:
        im = reducir(icono64(n), 32, FONDO_VENTANA, opaco=True)
        if n == "bola":
            # la bola, tambien a 16 px: a 32 era mas del doble de
            # grande que la bola del juego y se metia dentro de la pala
            archivos.append((f"tr_bola16.nimg", a_nimg(reducir(icono64(n), 16, FONDO_VENTANA))))
        if n in ("mina", "explosion", "bandera", "bola", "corazon", "capsula"):
            # para los juegos: los mismos, pero con el fondo
            # TRANSPARENTE (magenta), para dibujarlos sobre lo que sea con
            # DrawImage + MaskImage
            archivos.append((f"tr_{n}.nimg", a_nimg(reducir(icono64(n), 32, FONDO_VENTANA))))
        archivos.append((f"ico32_{n}.nimg", a_nimg(im)))
    for nombre, datos in archivos:
        with open(os.path.join(salida, nombre), "wb") as f: f.write(datos)

    # La hoja de muestra: cada tira, a 24 px y ampliada x2, con sus numeros
    fila_h, esc = 24 * 2 + 26, 2
    hoja = Image.new("RGB", (10 * 24 * esc + 200, len(GRUPOS) * fila_h + 20), (236, 234, 228))
    d = ImageDraw.Draw(hoja)
    f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 12)
    for k, (grupo, nombres) in enumerate(GRUPOS):
        y = 10 + k * fila_h
        d.text((8, y + 16), f"tb_{grupo}", font=f, fill=(20, 20, 20))
        for i, n in enumerate(nombres):
            x = 150 + i * 24 * esc
            fondo = Image.new("RGB", (24, 24), FONDO_BARRA)
            ico = reducir(icono64(n), 24, FONDO_BARRA)
            m = Image.new("L", (24, 24), 0)
            for yy in range(24):
                for xx in range(24):
                    if ico.getpixel((xx, yy)) != MAGENTA: m.putpixel((xx, yy), 255)
            fondo.paste(ico, (0, 0), m)
            hoja.paste(fondo.resize((24 * esc, 24 * esc), Image.NEAREST), (x, y))
            d.text((x + 2, y + 24 * esc + 2), f"{i} {n}", font=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 9), fill=(40, 40, 40))
    hoja.save(os.path.join(salida, "muestra_iconos.png"))

    # Para incrustar en el sistema
    with open(os.path.join(salida, "iconos_nimg.c"), "w") as c:
        c.write("// iconos_nimg.c -- GENERADO por herramientas/iconos/crear_iconos.py. No editar a mano.\n")
        c.write("// La biblioteca de iconos NIMG; embedded_lua.c la instala en DOCUMENTOS.\n")
        c.write("#include \"iconos_nimg.h\"\n\n")
        for i, (nombre, datos) in enumerate(archivos):
            c.write(f"static const uint8_t icono_{i}[{len(datos)}] = {{")
            for j, b in enumerate(datos):
                if j % 32 == 0: c.write("\n    ")
                c.write(f"{b},")
            c.write("\n};\n")
        c.write("\nconst icono_nimg_t ICONOS_NIMG[] = {\n")
        for i, (nombre, datos) in enumerate(archivos):
            c.write(f"    {{ icono_{i}, {len(datos)}u, \"{nombre}\" }},\n")
        c.write("};\n")
        c.write(f"const uint32_t ICONOS_NIMG_N = {len(archivos)}u;\n")
    total = sum(len(d) for _, d in archivos)
    print(f"{len(archivos)} archivos NIMG, {total // 1024} KB en total -> {salida}/")

if __name__ == "__main__":
    main()
