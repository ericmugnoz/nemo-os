# Nemo OS

Un sistema operativo ARM64 escrito desde cero, sin usar código de nadie más.

Kernel propio, sistema de archivos propio, gestor de ventanas propio, lenguaje
de programación propio con su compilador —que se compila a sí mismo dentro del
sistema— y un puerto de Lua 5.5. Arranca en una **Raspberry Pi 4 real** y en
**QEMU**, desde el mismo código fuente.

Está escrito para que se pueda **leer y entender**. Es material de estudio de
sistemas operativos tanto como un sistema que funciona: si abres cualquier
archivo de `src/`, encontrarás explicado qué hace y, sobre todo, por qué está
hecho así.

---

## Qué trae

| | |
| --- | --- |
| **Kernel** | ARM64 (AArch64), bare-metal, multiproceso sobre los 4 núcleos |
| **Sistema de archivos** | NemoFS, propio, con inodos y bloques indirectos; lee y escribe FAT32 |
| **Interfaz** | Escritorio, ventanas, menús, iconos, 42 tipografías con suavizado |
| **Lenguaje** | Nemo Basic: 260 comandos, compilador propio que corre **dentro** del sistema |
| **Segundo lenguaje** | Lua 5.5 portado, con 39 programas de ejemplo |
| **Programas** | Shell, explorador, editor, IDE, editor de imágenes, monitor del sistema |
| **Red** | ARP, IPv4, ICMP, UDP y TCP escritos desde cero; cliente DHCP; shell remota por TCP |
| **Hardware de la Pi 4** | USB (xHCI sobre PCIe), tarjeta SD (EMMC2), red (GENET), sonido por el jack, GPIO, I2C, SPI |
| **Documentación** | 32 guías en español, instaladas dentro del propio sistema |

---

## Lo que necesitas

- Un **compilador cruzado de C para ARM64 bare-metal**. El Makefile busca el
  prefijo `aarch64-elf-`.
- **QEMU** (`qemu-system-aarch64`), si quieres probarlo sin una Pi.
- **Python 3**, que usan varias herramientas del build.
- **make**.

En macOS con Homebrew:

```bash
brew tap messense/macos-cross-toolchains
brew install aarch64-elf-gcc aarch64-elf-binutils qemu python3 make
```

En Debian o Ubuntu:

```bash
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
                 qemu-system-arm python3 make
```

> En Debian el prefijo del compilador es `aarch64-linux-gnu-`, no
> `aarch64-elf-`. Cámbialo al compilar:
> `make PREFIX=aarch64-linux-gnu-`

Comprueba que lo tienes:

```bash
aarch64-elf-gcc --version
qemu-system-aarch64 --version
```

---

## Compilar

```bash
git clone https://github.com/ericmugnoz/nemo-os.git
cd nemo-os
make
```

Eso produce **`kernel8.img`**, el kernel de la Raspberry Pi 4. La primera vez
tarda: compila el kernel, los siete programas del sistema, el compilador de
Nemo Basic, el intérprete de Lua, y empotra dentro del kernel las guías, las
tipografías, los iconos y los programas de ejemplo.

Otros objetivos:

| Orden | Qué hace |
| --- | --- |
| `make` | El kernel de la Pi 4 (`kernel8.img`) |
| `make qemu` | La versión para QEMU (`build/kernel.elf`) |
| `make run` | Compila para QEMU **y lo arranca** |
| `make imagen` | La imagen de tarjeta SD completa, lista para escribir |
| `make imagen-xz` | La misma, comprimida (es como se distribuye) |
| `make clean` | Borra lo compilado |

---

## Probarlo en QEMU

```bash
make run
```

Se abre una ventana con el escritorio de Nemo OS. El ratón y el teclado
funcionan; la consola serie sale por el terminal desde el que lo lanzaste, y
ahí es donde se ve el log de arranque —que cuenta bastante de lo que el
sistema está haciendo—.

> La orden `run` del Makefile usa `-display cocoa`, que es de macOS. En Linux,
> cámbialo por `-display gtk` o `-display sdl` en la regla `run:`.

---

## Instalarlo en una Raspberry Pi 4

> **Si solo quieres usarlo, no compilarlo**, baja la imagen ya hecha de
> [Releases](https://github.com/ericmugnoz/nemo-os/releases) y sigue la
> **[guía de instalación](INSTALACION.md)**, que lo cuenta paso a paso —
> incluido el reinicio y el Particionador, que hacen falta después del primer
> arranque.

### La carpeta de arranque

Para construir la imagen tú mismo hace falta el **firmware de arranque de la
Raspberry Pi**. No está en este repositorio a propósito: no es mío, es de la
Fundación Raspberry Pi y tiene su propia licencia. Se baja una vez, de su
repositorio oficial:

```bash
mkdir -p arranque/overlays
cd arranque
BASE=https://raw.githubusercontent.com/raspberrypi/firmware/master/boot
curl -LO $BASE/start4.elf
curl -LO $BASE/fixup4.dat
curl -LO $BASE/bcm2711-rpi-4-b.dtb
```

Esos tres archivos son todo lo que necesita una Pi 4; la carpeta `overlays/`
puede quedarse vacía si no usas ninguno. El `kernel8.img` y el `config.txt` los
pone el Makefile, recién hechos, cada vez.

Con la carpeta lista:

```bash
make imagen
make imagen-xz
```

Si la tienes en otro sitio: `make imagen ARRANQUE=/ruta/a/arranque`.

Sale un archivo `nemo-os-1.0.1.img.xz` de unos 3,5 MB. Escríbelo con el
**Raspberry Pi Imager**, eligiendo «imagen personalizada»; cuando pregunte por
la personalización, responde que **no**.

Después del primer arranque hay que **reiniciar** y pasar por **Menú Inicio →
Sistema → Particionador** para que NemoFS ocupe la tarjeta entera. Los detalles,
en la [guía de instalación](INSTALACION.md).

En el primer arranque el sistema formatea su partición él solo. El log te lo
dirá:

```
nemofs: montado correctamente (16384 inodos, ... bloques = ... MB, ... MB en uso)
lua: 34 de 34 archivos instalados (todos).
otros programas: 29 de 29 archivos en /otros programas (todos).
```

Probado en tarjetas de 16, 32, 64 y 128 GB.

> Nemo OS usa **16 GiB** de la tarjeta como mucho, sea cual sea su tamaño. No
> es un fallo: el mapa de bloques vive entero en memoria y cuesta 256 bytes por
> cada MB de tarjeta, así que tiene un techo puesto a propósito. Con una
> instalación de unos 25 MB, sobra de largo.

---

## La red

Si hay un cable de red enchufado, Nemo OS **pide su dirección al router** con su
propio cliente DHCP. Lo cuenta en el log:

```
net: preguntando al router por una IP (DHCP)...
net: el router dio una IP por DHCP
net: listo, IP = 10.0.2.15 mascara 255.255.255.0 pasarela 10.0.2.2 DNS 10.0.2.3
```

Si nadie contesta en ocho segundos, usa una dirección de reserva del rango
link-local (`169.254.x.x`) y lo dice. Eso no es un fallo: es el caso del **cable
directo a otro ordenador**, sin router de por medio, donde no hay a quién
preguntar. Un
Mac en esa situación se autoasigna una dirección del mismo rango, así que los
dos extremos acaban en la misma subred sin configurar nada.

La negociación no bloquea el arranque —ocurre dentro de `net_poll`—, así que el
escritorio está en marcha antes de que la red termine.

Toda la pila está escrita desde cero: ARP, IPv4, ICMP (responde a `ping`), UDP,
TCP y el cliente DHCP. Encima de TCP hay una **shell remota**: se conecta con
`telnet` o `nc` a la dirección que diga el log. Los detalles están en
`docs/GUIA_RED_NEMO_OS.md`.

> Un aviso que ahorra mucho rato: si el enlace sube a **1000 Mbps** y no cruza ni
> una trama, no descartes el cable. La autonegociación de Ethernet viaja por un
> solo par, pero transmitir a Gigabit usa los cuatro; con un par bueno y tres
> malos los dos extremos anuncian Gigabit y no pasa nada por el cable.

---

## Cómo está organizado

```
src/            El kernel. Todo lo que corre en modo privilegiado.
  pi4/          Lo específico de la Raspberry Pi 4: arranque, MMU, USB, SD, red, sonido.
programs/       Los programas del sistema escritos en C (shell, explorador, editor, IDE).
nbc-selfhost/   El compilador de Nemo Basic. El mismo código corre en el Mac y dentro de Nemo OS.
lua/            El puerto de Lua 5.5 y sus programas.
otros programas/  Programas en Nemo Basic, con su código fuente. Viajan dentro de la imagen
                y se compilan desde el propio sistema: dos juegos, un editor de texto con
                pestañas, el monitor del sistema, red, una biblioteca de utilidades, y
                en ejemplos/ programas cortos de una sola idea.
docs/           Las guías. Se empotran en el kernel y se leen desde el propio sistema.
fuentes/        El paquete de tipografías.
herramientas/   Utilidades del build: empaquetadores, conversores de imagen, el creador
                de la imagen de tarjeta, y un emulador de ARM64 para probar programas sin la placa.
```

Un detalle que sorprende al principio: **`docs/` y `lua/` no son documentación
suelta**. Sus archivos se empotran dentro del kernel como datos y se instalan
en el sistema de archivos en el primer arranque, así que Nemo OS lleva sus
propios manuales dentro. Si los borras, no compila.

### El mismo código, dos máquinas

El kernel compartido está en `src/`; lo que cambia de una máquina a otra vive
en archivos separados. `src/mmu.c` es la tabla de páginas de QEMU y
`src/pi4/mmu_pi4.c` la de la Pi 4; `src/ramfb.c` y `src/pi4/ramfb_pi4.c` hacen
lo mismo con la pantalla. Por eso los dos objetivos del Makefile se mantienen
al día en paralelo: QEMU es la red de seguridad del desarrollo, y la Pi es
donde se descubren las cosas de verdad.

### Por dónde empezar a leer

Los archivos están comentados para leerse, y los grandes empiezan con una
cabecera que explica de qué va el archivo y termina con una lista **QUE MIRAR
PARA ENTENDERLO**. Un recorrido que funciona:

1. **`src/kernel.c`** — el arranque, paso a paso, y por qué ese orden y no otro.
   Es el índice natural del resto del kernel.
2. **`src/tasks.c`** — cómo se reparte el tiempo entre los cuatro núcleos, y
   `src/bkl.c` para el candado que los coordina.
3. **`src/wm.c`** — el gestor de ventanas y los rectángulos sucios: por qué
   repintar solo lo que cambió es la diferencia entre un ratón que va y uno que
   no.
4. **`src/nemofs.c`** — un sistema de archivos con inodos, desde el
   superbloque hasta la doble indirección.
5. **`src/pi4/ramfb_pi4.c`** — la pantalla en hardware real, y la trampa que más
   caro ha costado en este proyecto: el framebuffer de la Pi 4 está en memoria
   *cacheada*.
6. **`src/udp.c`** y **`src/dhcp.c`** — dos protocolos completos y cortos, buenos
   para leer de una sentada.

---

## Programar dentro del sistema

Nemo OS trae su propio lenguaje y su propio compilador, y el compilador **corre
dentro del sistema**. Desde la shell:

```
run nbc.pro mi_programa.nb
run mi_programa.pro
```

También está **Timonel**, un entorno de programación al estilo de Visual Studio
Code escrito en el propio Nemo Basic: explorador de archivos, pestañas,
números de línea y coloreado de sintaxis. Su código está en
`otros programas/Timonel/`, y puede abrirse a sí mismo.

La referencia completa del lenguaje —los 260 comandos— está en `docs/`, y
también dentro del sistema, en la carpeta `MANUALES`.

---

## Licencia

Apache 2.0. Ver [LICENSE](LICENSE).

Puedes usarlo, estudiarlo, modificarlo y distribuirlo, también en proyectos
comerciales, mientras mantengas el aviso de copyright y la licencia.

**Todo el código de este repositorio está escrito desde cero**, sin copiar de
nadie. Las dos únicas excepciones, ambas anotadas en [NOTICE](NOTICE):

- **Lua 5.5**, de PUC-Rio, con licencia MIT. Está en `lua/`, portado a Nemo OS
  pero con su código original intacto.
- **El paquete de tipografías** (`fuentes/FUENTES.NFP`) se genera a partir de
  **DejaVu**, cuya licencia permite redistribuir y derivar.

El firmware de arranque de la Raspberry Pi **no se incluye**: es de la
Fundación y se baja aparte (ver «La carpeta de arranque»).

---

## Sobre el proyecto

Nemo OS es uno de los tres pilares de un laboratorio independiente de
tecnología en español: junto a un modelo de lenguaje abierto, y una colección
de libros —narrativa, técnica y académica— que documenta ambos proyectos de
principio a fin.

La idea de fondo es que se puede hacer trabajo técnico serio en español, sin
depender de ninguna gran empresa ni esperar permiso de nadie para empezar.
