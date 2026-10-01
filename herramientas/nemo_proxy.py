#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
nemo_proxy.py — el ayudante que deja navegar a Nemo OS.

QUE HACE. Nemo OS le pide una pagina. El proxy la baja de internet, le
quita todo lo que Nemo OS no sabe leer, convierte sus imagenes al
formato .nimg ya escaladas, y le devuelve UN SOLO archivo .nmz con todo
dentro (ver FORMATO_NMZ.md).

POR QUE EXISTE. Porque sin el, para ver una pagina web harian falta
dentro de Nemo OS tres cosas enormes:

  * TLS. Meses de trabajo --handshake, certificados, cadena de
    confianza, criptografia-- y una responsabilidad de seguridad para
    siempre. Una implementacion propia y a medias es peor que no tener
    red. Aqui el cifrado lo pone Python y Nemo OS habla HTTP plano por
    la red local.
  * Decodificadores de imagen. JPEG, PNG, WebP, GIF, AVIF: cada uno un
    proyecto. Aqui los pone Pillow y Nemo OS recibe pixeles.
  * Un motor de CSS y JavaScript. Eso no se termina nunca.

Lo que queda del otro lado es un visor de HTML sencillo, que Nemo OS ya
tenia escrito antes de que existiera la red.

LO QUE NO HACE, y conviene saberlo:
  * Sin JavaScript. Va bien con Wikipedia, documentacion, blogs, foros y
    prensa. No con aplicaciones web.
  * Sin formularios (todavia). Solo se lee.
  * Sin tablas: el visor no las sabe maquetar, asi que sus celdas se
    convierten en lineas sueltas. Se pierde la rejilla, se conserva el
    texto.

USO
    pip3 install Pillow requests     (requests es opcional)
    python3 nemo_proxy.py            escucha en el 8080

    Y desde Nemo OS:
        bajar <ip del Mac> 8080 /nmz?u=https://es.wikipedia.org/wiki/Nautilus  pagina.nmz
"""

import html.parser
import http.server
import io
import re
import socketserver
import ssl
import struct
import sys
import urllib.parse
import urllib.request

PUERTO = 8080

# Tope del pool de imagenes del kernel de Nemo OS. Una imagen mas grande
# no se puede cargar, asi que se reduce aqui: de nada sirve mandar
# pixeles que luego no caben.
MAX_IMG = 256

# Ancho al que se maqueta la pagina en el visor. Las imagenes se escalan
# para no pasarse de aqui aunque quepan en 256.
ANCHO_PAGINA = 600

# Cuantas imagenes se bajan como mucho. Una pagina de prensa trae
# cincuenta iconos de un pixel; bajarlos todos multiplica el tiempo sin
# aportar nada.
MAX_IMAGENES = 12

AGENTE = "Mozilla/5.0 (compatible; NemoOS-proxy)"


# ---------------------------------------------------------------------
# Los certificados raiz.
#
# Python NO usa los del sistema: trae los suyos, y en macOS no se
# instalan al instalar Python. Resultado: cualquier HTTPS falla con
# "CERTIFICATE_VERIFY_FAILED: unable to get local issuer certificate",
# que parece un problema del sitio y es de la instalacion.
#
# Se busca el paquete 'certifi', que es de donde los saca casi todo el
# mundo. Si no esta, se usa lo que haya por defecto -- que en Linux
# suele bastar.
#
# Lo que NO se hace es apagar la verificacion. Seria una linea y
# arreglaria el sintoma, pero un proxy que acepta cualquier certificado
# es un proxy al que se le puede colar cualquiera: justo lo que los
# certificados existen para impedir. Y quien lo use no se enteraria.
def _contexto_ssl():
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except Exception:
        return ssl.create_default_context()


CTX = _contexto_ssl()


# ---------------------------------------------------------------------
# .nimg: cabecera de 12 bytes y pixeles RGBA en crudo. Sin comprimir,
# que es lo que hace que Nemo OS no necesite ningun decodificador.
# ---------------------------------------------------------------------
def a_nimg(datos, ancho_max=MAX_IMG, alto_max=MAX_IMG):
    from PIL import Image
    img = Image.open(io.BytesIO(datos))
    # Un GIF animado o un PNG con paleta: se pasa a RGBA y se queda el
    # primer fotograma, que es lo unico que el visor puede ensenar.
    img = img.convert("RGBA")
    w, h = img.size
    if w > ancho_max or h > alto_max:
        r = min(ancho_max / w, alto_max / h)
        w, h = max(1, int(w * r)), max(1, int(h * r))
        img = img.resize((w, h), Image.LANCZOS)
    return struct.pack("<4sII", b"NIMG", w, h) + img.tobytes("raw", "RGBA"), w, h


# ---------------------------------------------------------------------
# El paquete. Ver FORMATO_NMZ.md.
# ---------------------------------------------------------------------
def empaquetar(archivos):
    """archivos: lista de (nombre, bytes). El primero debe ser index.html."""
    tabla = bytearray()
    cuerpo = bytearray()
    for nombre, datos in archivos:
        n = nombre.encode("ascii")
        assert b"/" not in n and b"\\" not in n and b".." not in n, nombre
        tabla += struct.pack("<H", len(n)) + n + struct.pack("<I", len(datos))
        cuerpo += datos
    return struct.pack("<4sI", b"NMZ1", len(archivos)) + bytes(tabla) + bytes(cuerpo)


# ---------------------------------------------------------------------
# El reductor de HTML.
#
# La lista de etiquetas NO es una eleccion de estilo: es exactamente lo
# que nemo_html.lua sabe maquetar. Todo lo que no esta aqui, o se tira o
# se convierte en algo que si.
# ---------------------------------------------------------------------
# Ojo: html, head y body NO estan aqui. El documento que se entrega los
# pone de nuevo alrededor (con el titulo ya limpio), asi que conservar
# los originales dejaria dos juegos anidados. Un visor tolerante lo
# aguanta; no es razon para entregarlo mal.
SE_QUEDAN = {
    "div", "section", "article", "header",
    "footer", "nav", "main", "aside", "center",
    "h1", "h2", "h3", "h4", "h5", "h6", "p", "br", "hr", "ul", "ol", "li",
    "blockquote", "pre",
    "b", "strong", "i", "em", "u", "code", "span", "small", "big", "a", "img",
}

# Se tira la etiqueta Y SU CONTENIDO. Un <script> sin su contenido
# dejaria el codigo JavaScript suelto como texto en medio de la pagina.
SE_TIRAN_CON_CONTENIDO = {"script", "style", "noscript", "iframe", "svg",
                          "canvas", "video", "audio", "object", "embed",
                          "form", "select", "textarea", "button"}

# Se tira la etiqueta pero se conserva lo de dentro.
SIN_MARCA = {"html", "body",
             "table", "thead", "tbody", "tfoot", "figure", "figcaption",
             "font", "label", "picture", "source", "abbr", "cite", "time",
             "mark", "sup", "sub", "dl", "dd", "dt", "details", "summary"}

# El visor no maqueta rejillas. Cada fila y cada celda pasan a ser
# lineas: se pierde la tabla, se conserva el texto, que es lo que se iba
# a leer de todas formas.
COMO_LINEA = {"tr", "th", "td"}

ATRIBUTOS_UTILES = {"href", "src", "alt", "style", "width", "height"}


class Reductor(html.parser.HTMLParser):
    def __init__(self, base, cola_imagenes):
        super().__init__(convert_charrefs=True)
        self.base = base
        self.salida = []
        self.profundidad_tirada = 0
        self.imagenes = cola_imagenes      # lista de urls a bajar, en orden
        self.en_titulo = False
        self.titulo = ""
        self.en_head = False

    # -- utilidades --
    def escribir(self, t):
        if self.profundidad_tirada == 0:
            self.salida.append(t)

    def absoluta(self, u):
        return urllib.parse.urljoin(self.base, u)

    # -- ganchos del analizador --
    def handle_starttag(self, tag, attrs):
        # El titulo se mira ANTES que nada: vive dentro de <head>, que
        # se descarta entero, asi que si se comprobara despues no se
        # llegaria a leer nunca.
        if tag == "title":
            self.en_titulo = True
            return
        if tag == "head":
            self.en_head = True
            return
        if tag in SE_TIRAN_CON_CONTENIDO:
            self.profundidad_tirada += 1
            return
        if self.profundidad_tirada:
            return
        if tag in COMO_LINEA:
            self.escribir("<br>")
            return
        if tag in SIN_MARCA:
            return
        if tag not in SE_QUEDAN:
            return

        d = dict(attrs)

        if tag == "img":
            src = d.get("src") or d.get("data-src")
            if not src or len(self.imagenes) >= MAX_IMAGENES:
                # Sin imagen, al menos queda el texto alternativo: una
                # pagina llena de huecos mudos se lee mucho peor.
                alt = d.get("alt")
                if alt:
                    self.escribir("[%s]" % escapar(alt))
                return
            u = self.absoluta(src)
            if u in self.imagenes:
                idx = self.imagenes.index(u)
            else:
                idx = len(self.imagenes)
                self.imagenes.append(u)
            self.escribir('<img src="%d.nimg">' % idx)
            return

        if tag == "a":
            href = d.get("href")
            if not href or href.startswith("#"):
                self.escribir("<a>")
                return
            # El enlace vuelve a pasar por el proxy, de modo que pinchar
            # en uno pida la siguiente pagina igual que la primera.
            self.escribir('<a href="/nmz?u=%s">' %
                          escapar(urllib.parse.quote(self.absoluta(href), safe="")))
            return

        trozos = [tag]
        for k, v in attrs:
            if k in ATRIBUTOS_UTILES and v is not None:
                trozos.append('%s="%s"' % (k, escapar(v)))
        self.escribir("<%s>" % " ".join(trozos))

    def handle_endtag(self, tag):
        if tag == "title":
            self.en_titulo = False
            return
        if tag == "head":
            self.en_head = False
            return
        if tag in SE_TIRAN_CON_CONTENIDO:
            if self.profundidad_tirada:
                self.profundidad_tirada -= 1
            return
        if self.profundidad_tirada:
            return
        if tag in COMO_LINEA or tag in SIN_MARCA or tag not in SE_QUEDAN:
            return
        if tag in ("img", "br", "hr"):
            return
        self.escribir("</%s>" % tag)

    def handle_data(self, datos):
        if self.en_titulo:
            self.titulo += datos
            return
        # Lo que hay en <head> y no es el titulo no pinta nada en la
        # pagina: seria texto suelto antes del primer parrafo.
        if self.en_head or self.profundidad_tirada:
            return
        self.escribir(escapar(datos))


def escapar(t):
    return (t.replace("&", "&amp;").replace("<", "&lt;")
             .replace(">", "&gt;").replace('"', "&quot;"))


# ---------------------------------------------------------------------
def bajar(url, tope=4 * 1024 * 1024):
    pet = urllib.request.Request(url, headers={"User-Agent": AGENTE})
    with urllib.request.urlopen(pet, timeout=15, context=CTX) as r:
        return r.read(tope), r.headers.get_content_charset() or "utf-8", r.geturl()


def construir_paquete(url, registro):
    crudo, juego, url_final = bajar(url)
    texto = crudo.decode(juego, errors="replace")

    urls_img = []
    red = Reductor(url_final, urls_img)
    red.feed(texto)

    titulo = red.titulo.strip() or url_final
    cuerpo = "".join(red.salida)
    doc = ("<html><head><title>%s</title></head><body>%s</body></html>"
           % (escapar(titulo), cuerpo))

    archivos = [("index.html", doc.encode("utf-8", errors="replace"))]

    for i, u in enumerate(urls_img):
        try:
            datos, _, _ = bajar(u, tope=6 * 1024 * 1024)
            nimg, w, h = a_nimg(datos)
            archivos.append(("%d.nimg" % i, nimg))
            registro("  imagen %d: %dx%d, %d bytes  (%s)" % (i, w, h, len(nimg), u[:60]))
        except Exception as e:
            # Una imagen que falla NO puede tumbar la pagina. Se anota y
            # se sigue; el visor vera un archivo que no esta y lo dejara
            # en blanco, que es mucho mejor que no ver nada.
            registro("  imagen %d FALLA (%s): %s" % (i, type(e).__name__, u[:60]))

    return empaquetar(archivos), titulo, len(archivos) - 1


# ---------------------------------------------------------------------
class Servidor(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass   # el registro lo llevamos nosotros, mas legible

    def anotar(self, t):
        print(t, flush=True)

    def do_GET(self):
        partes = urllib.parse.urlparse(self.path)
        if partes.path in ("/", "/ayuda"):
            self.contestar(200, b"text/plain; charset=utf-8", AYUDA.encode())
            return
        if partes.path != "/nmz":
            self.contestar(404, b"text/plain", b"solo /nmz\n")
            return

        q = urllib.parse.parse_qs(partes.query)
        url = (q.get("u") or q.get("url") or [None])[0]
        if not url:
            self.contestar(400, b"text/plain", b"falta ?u=<url>\n")
            return
        if "://" not in url:
            url = "https://" + url

        self.anotar("pidiendo %s" % url)
        try:
            paquete, titulo, n_img = construir_paquete(url, self.anotar)
        except Exception as e:
            msg = "no se pudo: %s: %s\n" % (type(e).__name__, e)
            self.anotar(msg.strip())
            self.contestar(502, b"text/plain; charset=utf-8", msg.encode())
            return

        self.anotar("  -> '%s': %d imagenes, %d bytes de paquete"
                    % (titulo[:50], n_img, len(paquete)))
        self.contestar(200, b"application/x-nemo-nmz", paquete)

    def contestar(self, codigo, tipo, cuerpo):
        self.send_response(codigo)
        self.send_header("Content-Type", tipo.decode())
        self.send_header("Content-Length", str(len(cuerpo)))
        # Nemo OS pide "Connection: close" y usa el cierre como senal de
        # fin. Aqui se manda Content-Length igualmente, que es mejor:
        # asi sabe cuanto falta y puede ensenar el progreso.
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(cuerpo)


AYUDA = """proxy de Nemo OS

  /nmz?u=<url>   devuelve la pagina empaquetada en .nmz

desde Nemo OS:
  bajar <ip de este ordenador> %d /nmz?u=https://ejemplo.com  pagina.nmz
""" % PUERTO


class Hilos(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    try:
        import PIL  # noqa: F401
    except ImportError:
        print("Falta Pillow. Instalalo con:  pip3 install Pillow")
        sys.exit(1)

    try:
        import certifi  # noqa: F401
    except ImportError:
        print("Aviso: falta el paquete 'certifi'. Si los sitios con https fallan con")
        print("       CERTIFICATE_VERIFY_FAILED, instalalo con:  pip3 install certifi")

    puerto = int(sys.argv[1]) if len(sys.argv) > 1 else PUERTO
    with Hilos(("0.0.0.0", puerto), Servidor) as s:
        print("proxy de Nemo OS escuchando en el puerto %d" % puerto)
        print("prueba:  curl -s 'http://localhost:%d/nmz?u=example.com' | head -c 64 | xxd | head -3"
              % puerto)
        try:
            s.serve_forever()
        except KeyboardInterrupt:
            print("\nadios")


if __name__ == "__main__":
    main()
