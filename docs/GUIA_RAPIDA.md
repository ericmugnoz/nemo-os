# Nemo OS

Un sistema operativo ARM64 escrito desde cero para la **Raspberry Pi 4**:
kernel propio ("Nautilus"), sistema de archivos propio (NemoFS), escritorio
gráfico con ventanas y gadgets, programas en EL0 aislados por MMU, Lua 5.5
como lenguaje del sistema, un BASIC propio en camino, red Ethernet con shell
remota, fuentes con antialiasing, y un visor de HTML y Markdown que muestra
su propia documentación. Corre en la placa real y, con la misma pila,
emulado en QEMU.

Esta guía te lleva de cero a tener Nemo OS arrancando —en la Pi o en QEMU—
sin necesitar ningún libro ni conocimiento previo del proyecto.

> **Revisada en septiembre de 2026.** La versión anterior de esta guía
> describía el Nemo OS de antes del port a hardware real, con otra
> estructura de carpetas y otro compilador. Todo lo de aquí corresponde al
> proyecto actual.

---

## Requisitos

- **Un compilador cruzado de C para ARM64 bare-metal**: `aarch64-elf-gcc`
  (con `aarch64-elf-as`, `aarch64-elf-ld`, `aarch64-elf-objcopy`).
- **make** (GNU make; el de macOS vale).
- **QEMU** (`qemu-system-aarch64`) y **qemu-img** — solo para `make run`.
- Opcional: `python3` con Pillow (solo para regenerar fuentes o convertir
  imágenes a `.nimg`).

En macOS con Homebrew:

```bash
brew install aarch64-elf-gcc qemu make
```

En Ubuntu/Debian:

```bash
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu qemu-system-arm make
```

(En Linux el prefijo del compilador es `aarch64-linux-gnu-`; ajusta `CC`,
`AS`, `LD` y `OBJCOPY` al principio del `Makefile`.)

---

## Compilar

Un solo `Makefile` en la raíz compila todo, incluido el intérprete de Lua
y todo lo que va embebido en el kernel:

```bash
make            # Raspberry Pi 4 real -> kernel8.img en la raiz
make run        # QEMU: compila la version emulada y la arranca
make qemu       # QEMU: solo compilar
make clean      # limpiar todo (incluye lua/build/)
```

`make` deja `kernel8.img` (unos 5 MB: dentro van el intérprete de Lua, los
programas del sistema, las librerías y programas en Lua, las páginas y
guías de ejemplo y 2 MB de fuentes). Detalles en `GUIA_MAKEFILE.md`.

---

## Arrancar en la Raspberry Pi 4

1. Formatea una tarjeta SD con una primera partición **FAT32** (la de
   arranque) y deja el resto para NemoFS — Nemo OS formatea esa segunda
   partición él solo la primera vez.
2. Copia a la partición FAT los archivos de firmware de Raspberry Pi:
   `start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`, la carpeta `overlays/`,
   y el `config.txt` del proyecto (raíz del repositorio).
3. Copia `kernel8.img`.
4. Conecta teclado y ratón USB, HDMI, y opcionalmente el cable serie (UART
   a 115200) para ver los mensajes de arranque.
5. Enciende. En unos segundos aparece el escritorio.

Con la UART verás cada paso del arranque: disco, NemoFS, USB, red, fuentes,
instalación de programas. Es la herramienta de diagnóstico principal.

## Arrancar en QEMU

```bash
make run
```

Abre una ventana con Nemo OS emulado (máquina `virt`, Cortex-A53, 512 MB),
crea si hacen falta los discos `disk.img` y `fat.img`, y saca la UART por
la terminal. La red también funciona: `nc -v localhost 2323` desde otra
terminal llega a la shell remota (ver más abajo). QEMU es la red de
seguridad del proyecto — todo se prueba ahí antes de grabar la SD.

---

## Primeros pasos

Nemo OS arranca directamente al escritorio gráfico. Botón **Inicio** abajo
a la izquierda, iconos en el escritorio, ventanas con barra de título.

Programas del sistema (en C, en `PROGRAMAS`): **Explorador** (doble clic
abre cualquier archivo con el programa adecuado), **Editor**, **IDE** (para
programas en Nemo Basic, con botón de compilar), **Shell**.

Programas en Lua (en `ACCESORIOS`, se lanzan desde el explorador o con
`run nombre.lua`): `shell.lua`, `editor.lua`, `calculadora.lua`,
`gestor_tareas.lua`, `monitor_sistema.lua`, `reloj.lua`, `notas.lua`,
`visor_imagenes.lua`, `visor.lua` (HTML y Markdown).

Empieza en **`DOCUMENTOS`**: haz doble clic en `bienvenida.html` — es una
portada que muestra lo que sabe hacer el visor — y luego en
`GUIA_LUA.md`, `GUIA_HTML.md` o `GUIA_RED.md`. La documentación del sistema
se lee dentro del propio sistema.

La shell (`shell.pro` o `shell.lua`) tiene los comandos de siempre:

| Comando | Qué hace |
|---|---|
| `ls`, `cd`, `pwd`, `mkdir`, `del`, `cat` | Archivos y carpetas |
| `C:` / `F:` | Cambiar entre NemoFS y el disco FAT (SD o pendrive) |
| `run programa.pro [arg]` | Ejecutar un programa |
| `run script.lua [args]` | Ejecutar un script de Lua |
| `run visor.lua doc.md` | Abrir un documento |
| `help`, `clear`, `exit` | Lo que dicen |

Detalles en `GUIA_SHELL_NEMO_OS.md`.

---

## Red y shell remota

Con un cable Ethernet directo entre la Pi y tu ordenador no hay nadie que
reparta direcciones, así que Nemo OS usa una del rango *link-local* y la
anuncia al arrancar:

```
net: reserva = 169.254.103.188 (sacada de la MAC b8:27:eb:11:22:33)
```

Con esa dirección:

```bash
ping 169.254.103.188
nc -v 169.254.103.188 2323     # shell remota: ls, cd, cat, run, disco, mem, tareas
```

Enchufada a un router es más sencillo: el DHCP le da una dirección normal
de la red de casa y el arranque también la dice.

Desde ahí puedes lanzar programas en el escritorio de la Pi
(`run reloj.lua`). Configuración del Mac y solución de problemas en
`GUIA_RED_NEMO_OS.md`.

---

## Escribe tu primer programa

**En Lua** (el camino recomendado). Crea `hola.lua` con el editor, o cópialo
al disco:

```lua
local gui = require("nemo_gui")
gui.crear_ventana("Hola", 100, 100, 320, 120)
gui.crear_etiqueta("Hola desde Nemo OS", 20, 30, 280, 24)
gui.bucle(function() return true end)
```

Ejecútalo con `run hola.lua` desde la shell o con doble clic. La librería
`nemo_gui` cubre ventanas, gadgets, eventos, teclado, ratón, imágenes y
fuentes; `nemo_archivos`, los archivos; `nemo_sistema`, el estado del
sistema. Todo en `GUIA_PROGRAMACION_LUA_NEMO_OS.md`.

**En Nemo Basic** (BASIC, compilado dentro de la propia Pi): crea
`hola.bb` en el IDE —

```basic
Print "Hola desde Nemo OS"
```

— y pulsa compilar; el IDE llama a `nbc.pro` y ejecuta el `.pro`
resultante. El lenguaje está evolucionando hacia un BASIC propio,
separado de BlitzPlus; ver `REFERENCIA_NEMO_BASIC.md`.

**En C o ensamblador**, generando un `.pro` en tu ordenador:
`GUIA_EJECUCION_PROGRAMAS_NEMO_OS.md` y `GUIA_SYSCALLS_EVENTOS_GADGETS_NEMO_OS.md`.

---

## Estructura del repositorio

```
Nemo OS ARM/
├── Makefile              un solo Makefile para la Pi 4 y para QEMU
├── config.txt            configuracion de arranque para la SD
├── src/                  el kernel: arranque, excepciones, MMU, tareas (EL0),
│   │                     NemoFS, FAT, gestor de ventanas, gadgets, syscalls,
│   │                     red (ARP/IP/ICMP/TCP, shell remota), fuentes
│   └── pi4/              solo Raspberry Pi 4: arranque, mailbox, framebuffer,
│                         EMMC, PCIe, xHCI/USB, Ethernet (GENET), MMU de la Pi
├── programs/             programas del sistema en C (shell, explorer, editor, ide...)
├── lua/                  Lua 5.5 portado, y en lua/ejemplos/ las librerias
│                         (nemo_gui, nemo_archivos, nemo_sistema, nemo_html,
│                         nemo_md) y los programas en Lua
├── lua/html/             paginas HTML de ejemplo
├── docs/                 las guias, en Markdown
├── fuentes/FUENTES.NFP   fuentes proporcionales pre-rasterizadas (DejaVu)
├── herramientas/         nfnt_pack.py (fuentes), nimg_convert.py (imagenes),
│                         crear_imagen.py (la imagen de tarjeta), emu/ (un
│                         emulador de ARM64 para probar programas sin la placa)
├── nbc-selfhost/         el compilador de Nemo Basic: corre dentro de Nemo OS
│                         (como nbc.pro) y tambien en el ordenador de desarrollo
└── otros programas/      programas en Nemo Basic, con su fuente; en ejemplos/,
                          programas cortos de una idea cada uno
```

---

## ¿Quieres entender cómo funciona por dentro?

Las guías de `docs/` cubren cada parte —bare-metal de la Pi 4 (dos niveles:
referencia y explicada desde cero), el Makefile, la ejecución de programas,
las syscalls, la shell, la red, HTML y Markdown, Lua, Nemo Basic— y existe
una colección de libros escrita específicamente para eso:

- **Capitán de mi propio sistema** — el relato completo de cómo se construyó,
  con los bugs y las dudas incluidas
- **El Nautilus, pieza por pieza** — la referencia técnica completa
- **Cartas de navegación del Nautilus** — un curso de sistemas operativos,
  con Nemo OS como caso práctico en cada capítulo

Ninguno es necesario para usar o modificar el código — todo lo que hace
falta está aquí y en los comentarios del propio código.

---

## Contribuir

El código está abierto para leerse, usarse, modificarse y ampliarse. Si
quieres:

- Soporte multiprocesador (los núcleos 1-3 siguen dormidos)
- GPIO desde Lua
- Tablas con `colspan`, `<script type="text/lua">` o formularios en el visor
- El explorador y los programas en C con las fuentes nuevas
- Sonido real en la Pi 4
- Simplemente arreglar un bug

Abre un pull request. Si tienes dudas sobre por dónde empezar, abre un issue.

## Apoyar el proyecto

Nemo OS es uno de los tres pilares de un laboratorio independiente de
tecnología en español que estoy construyendo: un modelo de lenguaje (LLM)
abierto, este sistema operativo, y la colección de libros que lo documenta.
Los tres nacen de la misma idea — que se puede hacer trabajo técnico serio,
en español, sin depender de ninguna gran empresa ni esperar permiso de
nadie para empezar.

Todo lo recaudado, tanto de la venta de los libros de la colección como de
cualquier donación o colaboración directa, se destina íntegramente a
sostener ese laboratorio: el tiempo dedicado a programarlo, y a que los
próximos proyectos —empezando por el LLM abierto— puedan llegar a existir
con la misma dedicación que este.

Si quieres colaborar, dos formas directas de hacerlo:

- **Comprando alguno de los libros de la colección** (mencionados más
  arriba) — no hace falta ninguno para usar este código, pero cada compra
  ayuda a que el laboratorio siga adelante.
- **Una donación directa**, del tamaño que sea:
  [buymeacoffee.com/ericmunoz](https://buymeacoffee.com/ericmunoz)

No hay ninguna recompensa exclusiva a cambio, ni ninguna función de Nemo OS
reservada para quien colabore — el código es y seguirá siendo abierto para
todos, colabores o no. Es, simplemente, la forma más directa de decir "esto
merece existir" con algo más que palabras.

## Licencia

Apache 2.0. Úsalo libremente.
