#!/usr/bin/env python3
"""red_de_pruebas.py -- el acompanante de pruebared.nb, para el Mac.

Levanta dos cosas y escribe en el terminal todo lo que recibe:

  * un servidor HTTP en el 8099, que atiende GET y POST
  * un eco de UDP en el 7001, que devuelve tal cual lo que le llegue

Sirve para ver LA MISMA CONVERSACION POR LOS DOS LADOS, que es lo que
hace facil saber de que lado esta el fallo. Si Nemo OS dice "no llega
nada" y aqui no aparece ni una linea, el paquete no salio; si aparece la
linea y Nemo OS no ve la respuesta, el problema esta en la vuelta.

Se ejecuta sin instalar nada -- solo biblioteca estandar:

    python3 herramientas/red_de_pruebas.py

Y se para con Control-C.

LAS DIRECCIONES. Desde dentro de QEMU, el Mac es SIEMPRE 10.0.2.2: es la
pasarela que pone la red de usuario de QEMU, y el trafico que va ahi lo
recibe el Mac. Desde una Pi de verdad hay que usar la IP del Mac en la
red local, que la dice 'ifconfig'.

POR QUE EL ECO ESCUCHA EN EL 7001 Y NO EN EL 7000. Nemo OS escucha en el
7000; si los dos usaran el mismo numero no se sabria quien contesta a
quien al leer los registros, y en la Pi --donde los dos estan en la
misma red de verdad-- un mensaje de difusion volveria a quien lo mando y
pareceria un eco. Con dos numeros distintos, cada linea del registro
dice sola de donde viene.
"""

import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PUERTO_HTTP = 8099
PUERTO_ECO = 7001

# Las rutas que se conocen. Cualquier otra devuelve 404 A PROPOSITO:
# pruebared.nb comprueba que un error se cuenta como error, porque un
# programa que toma un 404 por un envio correcto pierde datos en
# silencio.
RUTAS = ("/hola", "/sensor", "/arranque")


def ahora():
    return time.strftime("%H:%M:%S")


def log(canal, texto):
    print(f"[{ahora()}] {canal:4} {texto}", flush=True)


class Manejador(BaseHTTPRequestHandler):
    # El registro propio de BaseHTTPRequestHandler va a stderr con un
    # formato distinto; se apaga para que todo salga por el nuestro y en
    # orden.
    def log_message(self, formato, *args):
        pass

    def responder(self, codigo, cuerpo):
        datos = cuerpo.encode("utf-8")
        self.send_response(codigo)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        # Content-Length SIEMPRE. El cliente de Nemo OS sabe terminar sin
        # el (por el cierre de la conexion), pero mandarlo es lo que
        # permite que compruebe que recibio todo.
        self.send_header("Content-Length", str(len(datos)))
        self.end_headers()
        self.wfile.write(datos)

    def do_GET(self):
        quien = self.client_address[0]
        log("HTTP", f"GET {self.path} desde {quien}")
        if self.path in RUTAS:
            self.responder(200, "hola desde el Mac")
        else:
            log("HTTP", f"     -> 404 (ruta desconocida, a proposito)")
            self.responder(404, "no existe")

    def do_POST(self):
        quien = self.client_address[0]
        largo = int(self.headers.get("Content-Length", 0))
        cuerpo = self.rfile.read(largo) if largo else b""
        log("HTTP", f"POST {self.path} desde {quien} -- {largo} bytes")
        if cuerpo:
            log("HTTP", f"     contenido: {cuerpo.decode('utf-8', 'replace')}")
        # Se comprueba que lo declarado y lo recibido coinciden. Si no,
        # el cliente miente en Content-Length y eso deja al servidor
        # esperando bytes que no van a llegar: es EL fallo de un POST.
        if largo != len(cuerpo):
            log("HTTP", f"     OJO: Content-Length decia {largo} y llegaron {len(cuerpo)}")
        if self.path not in RUTAS:
            log("HTTP", f"     -> 404 (ruta desconocida, a proposito)")
            self.responder(404, "no existe")
            return
        # Se devuelve el NUMERO DE BYTES recibidos: asi el programa de
        # Nemo OS puede comparar y saber que su Content-Length cuadraba.
        self.responder(200, str(len(cuerpo)))


def servidor_http():
    s = ThreadingHTTPServer(("0.0.0.0", PUERTO_HTTP), Manejador)
    log("HTTP", f"escuchando en el {PUERTO_HTTP} (rutas: {', '.join(RUTAS)})")
    s.serve_forever()


def eco_udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", PUERTO_ECO))
    log("UDP", f"eco escuchando en el {PUERTO_ECO}")
    while True:
        datos, origen = s.recvfrom(2048)
        # Un datagrama VACIO es legal, y se devuelve vacio: es como se
        # manda un "estoy aqui" sin datos, y Nemo OS comprueba que no se
        # confunde con "no llego nada".
        if datos:
            log("UDP", f"{len(datos)} bytes de {origen[0]}:{origen[1]} -- "
                       f"{datos.decode('utf-8', 'replace')}")
        else:
            log("UDP", f"datagrama VACIO de {origen[0]}:{origen[1]} (es legal)")
        s.sendto(datos, origen)


def main():
    print("red_de_pruebas -- acompanante de otros programas/red/pruebared.nb")
    print("Desde QEMU, este Mac es 10.0.2.2.  Control-C para parar.")
    print("-" * 66)
    hilo = threading.Thread(target=eco_udp, daemon=True)
    hilo.start()
    try:
        servidor_http()
    except KeyboardInterrupt:
        print("\nparado.")
    except OSError as e:
        # El caso de siempre: el puerto ya esta cogido por otra copia de
        # este guion que quedo abierta en otra ventana.
        print(f"\nno se pudo abrir el {PUERTO_HTTP}: {e}")
        print("Si ya hay otra copia corriendo, cierrala primero.")
        sys.exit(1)


if __name__ == "__main__":
    main()
