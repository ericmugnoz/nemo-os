# Cómo usar el Makefile — guía rápida

Un único `Makefile`, en la raíz del proyecto, compila **las dos versiones**
de Nemo OS: la que corre en una Raspberry Pi 4 real, y la que corre
emulada en QEMU (útil para probar cambios rápido, sin tener que grabar una
SD cada vez). Y, además, **construye la imagen de tarjeta** que se
distribuye. Esta guía explica qué hace cada comando y cómo está montado
por dentro, por si algún día hay que tocarlo.

---

## Los comandos, de un vistazo

| Comando | Qué hace |
| --- | --- |
| `make` | El kernel de la Raspberry Pi 4 → `kernel8.img` |
| `make imagen` | La **imagen de tarjeta SD** completa → `nemo-os-1.0.1.img` |
| `make imagen-xz` | La misma, comprimida → `nemo-os-1.0.1.img.xz` (así se descarga) |
| `make run` | Compila para QEMU **y lo arranca** |
| `make qemu` | Igual, pero sin arrancar (solo comprobar que compila) |
| `make fuentes` | Regenerar `fuentes/FUENTES.NFP` (opcional) |
| `make otros-paquete` | Rehacer el paquete de programas de ejemplo (se hace solo) |
| `make clean` | Borrar lo compilado de las dos plataformas |
| `make distclean` | Lo anterior, y además los discos de prueba de QEMU |

---

### `make` — Raspberry Pi 4 real (el de siempre)

```bash
make
```

Compila el kernel completo para hardware real y deja listo `kernel8.img`
en la raíz del proyecto — el archivo que va a la tarjeta SD, junto a
`config.txt`, `start4.elf`, etc. (ver la guía bare-metal para el resto de
archivos de firmware que hacen falta en la SD).

Es el comportamiento por defecto: escribir `make` sin nada más hace
exactamente esto, igual que siempre lo ha hecho.

**Al terminar bien**, verás:
```
Listo: kernel8.img generado.
```

### `make run` — QEMU (compila y arranca)

```bash
make run
```

Compila la versión de QEMU (si hace falta — si no ha cambiado nada desde
la última vez, salta directo a arrancar) y abre una ventana con Nemo OS
corriendo emulado. Eso incluye el compilador de Nemo Basic (`nbc.pro`) y
su runtime incrustado, que se rehacen solos cuando cambian sus fuentes. Crea también, si no existen ya, dos discos de prueba
(`disk.img` y `fat.img`, 64 MB cada uno) que usa la máquina virtual.

Para salir, cierra la ventana de QEMU como cualquier otra ventana, o
`Ctrl+C` en la terminal donde lanzaste `make run`.

### `make qemu` — QEMU (solo compilar, sin arrancar)

```bash
make qemu
```

Igual que `make run`, pero sin el último paso de abrir la ventana — útil
si solo quieres comprobar que compila limpio (por ejemplo, antes de subir
un cambio) sin distraerte con la máquina virtual abriéndose.

### `make fuentes` — regenerar el paquete de fuentes (opcional)

```bash
make fuentes
```

Vuelve a generar `fuentes/FUENTES.NFP` (las fuentes proporcionales del
sistema) a partir de las TTF de DejaVu con `herramientas/nfnt_pack.py`.
Necesita `python3` con Pillow y las DejaVu instaladas. **No hace falta
para compilar**: el paquete generado se guarda en el repositorio y `make`
lo embebe tal cual. Solo si cambias tamaños, familias o el propio
generador.

### `make clean` — limpiar todo

```bash
make clean
```

Borra los archivos intermedios de **las dos** plataformas a la vez: los
`.o`/`.elf`/`.bin` de la Pi 4 (en `src/`, `src/pi4/`, `programs/`,
`nbc-selfhost/`) y la carpeta `build/` completa de QEMU. Después de un
`make clean`, tanto `make` como `make run` recompilan todo desde cero.

**Cuándo usarlo**: cuando cambies un archivo y el resultado no refleje el
cambio — casi siempre significa que algo se quedó sin recompilar. También
es buena costumbre antes de cualquier prueba importante, para descartar
que el problema sea un `.o` viejo mezclado con fuentes nuevas.

Hay además `make distclean`, que hace lo mismo que `clean` y además borra
los discos de prueba de QEMU (`disk.img`, `fat.img`) — útil si quieres
empezar también con discos vacíos, no solo con el código recompilado.

---

## Crear la imagen de instalación

Esto es lo que produce el archivo que un usuario descarga y escribe en una
tarjeta. Son dos órdenes:

```bash
make imagen
make imagen-xz
```

`make imagen` compila primero el kernel si hace falta (depende de
`kernel8.img`, así que no hay forma de empaquetar un kernel viejo) y deja
**`nemo-os-1.0.1.img`** en la raíz. `make imagen-xz` lo comprime y deja
**`nemo-os-1.0.1.img.xz`**, que es el archivo que se publica: unos **3,5 MB**.

Al terminar, el propio Makefile te recuerda cómo se escribe:

```
  Listo: nemo-os-1.0.1.img
  Comprimela con 'make imagen-xz' y escribela con el programa de Raspberry Pi
  (opcion de imagen personalizada; cuando pregunte por la personalizacion, responde que NO).
```

La guía para el usuario final —escribir la tarjeta, el primer arranque, el
reinicio y el Particionador— está en [INSTALACION.md](../INSTALACION.md).

### Qué hace `make imagen`, paso a paso

1. **Comprueba la carpeta de arranque.** Es la que tiene el firmware de la
   Pi (`start4.elf`, `fixup4.dat`, el `.dtb` del modelo y `overlays/`). Por
   defecto es **`arranque/`**. Ese firmware **no viaja en el repositorio**: es
   de la Fundación Raspberry Pi, con su propia licencia, y se baja una vez (el
   README lo explica en «La carpeta de arranque»). Si lo tienes en otro sitio:

   ```bash
   make imagen ARRANQUE=/ruta/a/la/carpeta/con/el/firmware
   ```

   Si la carpeta no existe, o le falta `start4.elf`, se para ahí y te lo
   dice. No genera una imagen a medias.

2. **Copia `kernel8.img` y `config.txt` recién hechos** dentro de esa
   carpeta. Siempre, incluso si ya había copias. Es a propósito: así no
   existe la posibilidad de publicar una imagen con un kernel de ayer
   dentro, que es un error del que nadie se da cuenta hasta que alguien se
   queja de un fallo ya corregido.

3. **Construye la imagen** con `herramientas/imagen/crear_imagen.py`.

4. **La verifica** con `herramientas/imagen/verificar_imagen.py`: vuelve a
   abrir el archivo terminado, lee la tabla de particiones y comprueba que
   dentro está todo lo que tenía que entrar. Un paso separado, y el que
   más disgustos ahorra.

### Qué hay dentro de la imagen

| Zona | Tamaño | Qué es |
| --- | --- | --- |
| Hueco inicial | 1 MB | Para la tabla de particiones y la alineación, como es costumbre |
| Arranque | 1 GB, FAT32 | El firmware de la Pi, `config.txt` y `kernel8.img`. Es la partición que el Imager y el propio ordenador ven al meter la tarjeta |
| NemoFS | 2 GB, **a ceros** | Vacía. **El sistema la formatea él solo en el primer arranque** |

Total declarado: unos **3 GB**. Y el `.xz` mide 3,5 MB. No es magia: la
partición de NemoFS son ceros —se comprimen a nada— y lo que de verdad va
dentro son unas decenas de MB de kernel y firmware.

Que NemoFS viaje vacía y se formatee en la placa es lo que hace que la
imagen sea igual de válida para cualquier tarjeta. Lo que sobra de la
tarjeta se aprovecha después, desde el sistema, con el **Particionador**
(Menú Inicio → Sistema → Particionador), no aquí.

Los dos parámetros de tamaño se pueden cambiar si algún día hace falta:
`crear_imagen.py` acepta `--arranque-mb` (1024 por defecto) y `--nemofs-mb`.
El número de versión que aparece en el nombre del archivo sale de
`VERSION_NEMO` en el Makefile.

### Qué necesitas para esto

Además del compilador cruzado: **`python3`** (sin librerías externas — los
dos scripts van con la biblioteca estándar) y **`xz`** para el paso de
compresión. En Mac, `xz` se instala con `brew install xz`; en Debian o
Ubuntu, `apt install xz-utils`.

---

## Qué necesitas instalado

- El compilador cruzado `aarch64-elf-gcc` (y `-as`, `-ld`, `-objcopy` del
  mismo paquete). En Mac: `brew install aarch64-elf-gcc
  aarch64-elf-binutils`.
- Para `make run`/`make qemu`: además, `qemu-system-aarch64`. En Mac:
  `brew install qemu`.
- Para `make imagen`: `python3` y `xz`.

Si tu compilador se llama distinto (por ejemplo `aarch64-none-elf-gcc` en
vez de `aarch64-elf-gcc`), cambia la línea `PREFIX = aarch64-elf-` al
principio del Makefile por el prefijo que uses tú.

---

## Qué carpetas espera encontrar

**No hace falta ningún proyecto aparte para QEMU** — todo vive en la misma
carpeta `Nemo OS ARM/` de siempre:

```
Nemo OS ARM/
├── Makefile              ← este archivo
├── config.txt            ← el config.txt de la Pi; 'make imagen' lo mete en la SD
├── lua/                  ← el interprete Lua 5.5 (con su PROPIO Makefile, que este
│   │                        llama con `make -C lua`) y, en lua/ejemplos/, las
│   │                        librerias y programas en Lua que se embeben en el kernel
├── lua/html/             ← paginas HTML de ejemplo que van a /DOCUMENTOS
├── docs/                 ← las guias .md; algunas se embeben en /MANUALES
├── fuentes/FUENTES.NFP   ← paquete de fuentes proporcionales (2 MB), embebido
├── otros programas/      ← los programas en Nemo Basic (tetris, arkanoid, buscaminas,
│                            Timonel, ejemplos/...), con su codigo fuente
├── build_otros/          ← donde aterriza otros.npak, el paquete de los anteriores
├── arranque/             ← el firmware de la Pi 4, que se baja aparte: es la
│                            carpeta que 'make imagen' usa por defecto (ARRANQUE)
├── herramientas/         ← nfnt_pack.py (fuentes), nimg_convert.py (imagenes),
│   │                        pro_sizes.py, empaquetar_otros.py
│   └── imagen/            ← crear_imagen.py y verificar_imagen.py, los de la SD
├── src/                  ← kernel compartido por las dos plataformas...
│   │                        ...y AQUÍ MISMO, sueltos junto a los
│   │                        compartidos, los archivos que SOLO usa QEMU:
│   │                        boot.s, ramfb.c, mmu.c, gic.c, disk.c, power.c,
│   │                        sound.c, input.c, uart.c, fwcfg.c y virtio_net.c
│   │                        -- son los archivos ORIGINALES con los que
│   │                        empezó el proyecto, antes de que existiera el
│   │                        port a la Pi 4; no hay que copiarlos a ningún
│   │                        otro sitio.
│   └── pi4/               ← SOLO para la Pi 4 real (arranque, framebuffer, disco, USB, Ethernet...)
│       └── blobs/          ← aqui aterrizan TODOS los blobs embebidos en el kernel: los .pro
│                              del sistema, lua.bin, cada .lua, las paginas .html, las guias
│                              .md, los iconos, otros.npak y el paquete de fuentes. Los
│                              comparten los DOS builds.
├── programs/              ← shell, explorador, editor, IDE... (compartidos)
└── nbc-selfhost/          ← el compilador de Nemo Basic (el BASIC propio), compartido
    └── tools/              ← nb_elf_extract.py, que arma el runtime que se incrusta
                               en cada programa compilado (ver mas abajo)
```

La idea de fondo: **todo lo de `src/pi4/` es exclusivo de la Pi 4 real**
(arranque de bajo nivel, mailbox, framebuffer por VideoCore, disco EMMC,
USB por xHCI/PCIe, Ethernet por GENET...) — nada de eso existe en QEMU, que
usa en su lugar los archivos "genéricos" que están sueltos en `src/`, junto
a todo lo demás. Todo lo que sí es realmente compartido — el propio kernel
(`kernel.c`, `wm.c`, `syscall.c`, `nemofs.c`, `fat.c`, `heap.c`, `timer.c`,
`rtc.c`, `exceptions.c`, `net.c`, `tcp.c`, `udp.c`, `dhcp.c`,
`bitacora.c`...) y los programas de usuario (`shell.c`, `explorer.c`...) —
es el **mismo archivo, tal cual**, usado por las dos plataformas a la vez:
cualquier mejora o corrección que hagas ahí vale para las dos sin copiar
nada a mano.

Cuidado con una confusión fácil: que un archivo esté suelto en `src/` **no**
quiere decir que sea de QEMU. La mayoría de los que están ahí son
compartidos; los exclusivos de QEMU son solo los once de la lista de
arriba. Para saberlo de verdad, mira si el archivo aparece en `OBJS` (Pi 4),
en `QEMU_OBJS`, o en las dos.

(La Pi 4 escribe sus `.o` junto a cada fuente, como siempre; QEMU los deja
en una carpeta `build/` que se crea sola la primera vez que compilas —
tampoco hay que tocarla a mano.)

---

## Cómo está montado por dentro (para quien vaya a tocar el Makefile)

El archivo tiene dos mitades bien separadas:

1. **La de la Pi 4**, arriba del todo — es la de siempre, sin cambios de
   fondo. Usa las variables `CFLAGS`, `OBJS`, etc.
2. **La de QEMU**, debajo, con sus propias variables **con el prefijo
   `QEMU_`** (`QEMU_CFLAGS`, `QEMU_BUILD_DIR`, `QEMU_OBJS`...) — para que
   no choquen de nombre con las de la Pi 4. Por ejemplo, las dos
   plataformas necesitan una variable `CFLAGS` con valores *distintos*
   (`-mcpu=cortex-a72` para la Pi 4 real, `-mcpu=cortex-a53
   -mgeneral-regs-only` para QEMU) — si se llamaran igual las dos, la
   segunda definición pisaría a la primera sin avisar, y probablemente
   verías fallos de compilación rarísimos, difíciles de rastrear hasta la
   causa.

Las reglas de la imagen de tarjeta van al final, después de las dos
mitades: usan `kernel8.img`, que es producto de la primera.

`make` (el comando, sin especificar objetivo) siempre usa el **primer
target definido en el archivo** — por eso el target `all` de la Pi 4 está
el primero del todo, y así `make` a secas hace lo de siempre sin que haga
falta escribir `make all` ni `make pi4`.

Ambas plataformas comparten el mismo toolchain (`aarch64-elf-gcc` y
compañía) — solo se define una vez, arriba del todo, y las dos mitades lo
reutilizan.

### El flag que evita el bug de las tablas de punteros

Las dos plataformas compilan con `-fno-jump-tables
-fno-tree-switch-conversion` — dos opciones que le impiden al compilador
convertir un `switch` (o una selección parecida) en una tabla de punteros
interna. Hace falta porque los programas de Nemo OS (`.pro`) son binarios
planos, sin ningún paso de reubicación al cargarlos: una tabla así,
generada por el propio compilador sin que se vea en el código fuente,
puede acabar con direcciones válidas solo en el sitio donde se enlazó el
programa — y esas direcciones dejan de servir en cuanto el programa carga
en otra zona de memoria distinta (algo que pasa con normalidad, cada tarea
tiene la suya). El síntoma real, si este flag faltara: texto que aparece
en blanco, o con cadenas que no tienen nada que ver con tu programa.

**Importante si algún día añades una regla de compilación nueva a mano**:
tiene que pasar por la variable `$(CFLAGS)` (o `$(QEMU_CFLAGS)`, según la
plataforma) — nunca escribir los flags del compilador sueltos, a mano, en
la propia regla. Si lo haces, ese archivo concreto se queda sin esta
protección aunque el resto del proyecto la tenga, y el bug puede volver a
aparecer solo ahí.

### La misma trampa, pero escrita por ti: punteros en datos estáticos

El flag de arriba impide que *el compilador* genere tablas de punteros a
tus espaldas. Pero puedes crear el mismo problema tú mismo, y el flag no
te protege de eso:

```c
/* MAL: el enlazador escribe aqui direcciones ABSOLUTAS */
static const struct { const char *nombre; int valor; } tabla[] = {
    { "uno", 1 }, { "dos", 2 },
};
```

Los punteros de esa tabla se calculan como si el programa fuera a cargarse
en la dirección 0. Como cada tarea de Nemo OS carga en la suya, al
ejecutar apuntan a cualquier sitio. La forma correcta es guardar el texto
**dentro** de la propia tabla, que así no tiene ninguna dirección que
corregir:

```c
/* BIEN: el nombre vive dentro de la tabla */
static const struct { char nombre[16]; int valor; } tabla[] = {
    { "uno", 1 }, { "dos", 2 },
};
```

Un `const char *p = "hola";` dentro de una función **sí** es correcto: esa
dirección la calcula la propia CPU en tiempo de ejecución (`adrp`), no el
enlazador. El problema es solo con punteros **almacenados en datos
estáticos**.

Esto pasó de verdad, en la tabla de símbolos del runtime del compilador:
los programas que no la consultaban funcionaban y los que sí reventaban
con un `Data Abort`, lo que lo hacía parecer un fallo del programa
compilado y no del compilador. La pista que lo resolvió fue que la
dirección del fallo era **pequeña** (`0xfdf0`), no una del área de la
tarea — señal inconfundible de un desplazamiento usado como dirección.

---

## Problemas típicos

**"No rule to make target..."** — casi siempre significa que falta un
archivo que el Makefile espera encontrar (revisa que la ruta exista de
verdad) o que escribiste mal el nombre de un target (`make run`, no
`make Run` ni `make RUN` — Make distingue mayúsculas de minúsculas).

**Un cambio que no se refleja al compilar** — `make clean` y vuelve a
compilar. Si el problema persiste incluso así, puede que el archivo que
tocaste no sea el que de verdad se está compilando (por ejemplo, hay dos
copias de un mismo `.c`, una en `src/` para QEMU y otra parecida en
`src/pi4/` para la Pi 4 — asegúrate de tocar la que corresponde a la
plataforma que estás probando).

**`make imagen` dice "falta la carpeta de arranque"** — o no existe la
carpeta `arranque/`, o le falta el firmware de la Pi. Ese firmware se baja
aparte (ver el README): comprueba que dentro están `start4.elf`,
`fixup4.dat`, el `.dtb` de tu modelo y la carpeta `overlays/`. Si los tienes
en otro sitio, pásale la ruta: `make imagen ARRANQUE=/ruta`.

**La imagen sale, pero la Pi no arranca con ella** — mira lo que dijo
`verificar_imagen.py` al final de `make imagen`: comprueba la tabla de
particiones y el contenido de la partición de arranque, así que si el
problema está en el empaquetado, se ve ahí. Si la verificación pasó, el
problema es de la tarjeta o de la placa, no de la imagen: la guía de
instalación tiene una sección para eso.

**Un programa compilado con `nbc.pro` falla con `Data Abort` y una
dirección pequeña en `FAR_EL1`** — mira la sección sobre punteros en datos
estáticos, más arriba. Una dirección de fallo que no cae dentro del área
de la tarea (que empieza bien alto, tipo `0x4a600000`) casi siempre
significa que se está usando un desplazamiento como si fuera una
dirección.

**`make run` no encuentra `qemu-system-aarch64`** — falta instalarlo (ver
"Qué necesitas instalado" arriba).

**Los flags de compilación cambiaron mucho de tamaño/orden y algo se rompe
después de tocar el Makefile** — comprueba con `make -n` (o `make -n
run`, o `make -n imagen`) qué comandos *ejecutaría* Make sin llegar a
lanzarlos de verdad; es la forma más rápida de ver si una regla está mal
escrita antes de perder tiempo con un fallo de compilación real.

---

## Las piezas que más cuesta entender

El Makefile es grande porque dentro del kernel viaja mucho más que código.
Lo que conviene saber:

### Todo lo que va dentro del kernel es un blob

`kernel8.img` lleva embebidos, además de los `.pro` del sistema (shell,
explorador, editor, IDE, y `nbc.pro`, el compilador de Nemo Basic):

- `lua.bin` (el intérprete, construido por `lua/Makefile` — este Makefile
  lo invoca con `$(MAKE) -C lua build/nemo/lua.bin`; se pide el `.bin`
  crudo, no `lua.pro`, porque el instalador del kernel ya añade la
  cabecera NEXE y `pro_wrap` la añadiría otra vez),
- cada `.lua` de `lua/ejemplos/` listado en `LUA_LIBS` (→ `/SISTEMA`) y
  `LUA_APPS` (→ `/ACCESORIOS`),
- las páginas de `HTML_PAGES` (`lua/html/`) y las guías de `GUIA_BLOBS`
  (`docs/` y `lua/`) → `/MANUALES`,
- los iconos (`src/iconos_nimg.c`) y el fondo de pantalla,
- `otros.npak`, el paquete con los programas de ejemplo en Nemo Basic,
- `fuentes/FUENTES.NFP`.

Cada uno es una regla explícita con `objcopy -I binary` — el símbolo sale
del **nombre del archivo** (`nemo_gui.lua` → `_binary_nemo_gui_lua_start`),
por eso las reglas hacen `cd` a la carpeta antes de llamar a `objcopy`,
para que el nombre no arrastre la ruta. El kernel los escribe en NemoFS al
arrancar (`src/embedded_lua.c`), sobreescribiendo: lo que hay en disco es
siempre lo que se compiló.

**Para añadir un programa en Lua**: su nombre en `LUA_APPS`, una regla de
blob copiada de cualquiera de las existentes, y una línea en la tabla de
`src/embedded_lua.c`. Lo mismo para una página o una guía.

### `otros programas/` se empaqueta en un solo archivo

Los ejemplos grandes en Nemo Basic no son un blob cada uno: se meten todos
en **`build_otros/otros.npak`** con `herramientas/empaquetar_otros.py`, y
ese paquete es el que se embebe. El motivo es que son muchos archivos y
están en carpetas, con espacios en los nombres — imposible de poner como
dependencias de Make de forma limpia.

Como la carpeta no se puede declarar como dependencia, el paquete se
rehace en **cada** `make` (target `otros-paquete`). No cuesta nada: el
script no toca el archivo si nada ha cambiado, así que Make no recompila
por gusto. El desempaquetado, dentro del sistema, lo hace
`src/otros_programas.c`.

El script también **comprueba los nombres al construir**: NemoFS admite 31
caracteres, y un nombre más largo se detectaría al arrancar la placa y no
al compilar, que es el peor momento posible.

### La memoria que declara cada programa: `pro_sizes.py`

Antes de empaquetar los `.pro`, el Makefile ejecuta
`herramientas/pro_sizes.py` sobre los `.elf` de los programas del sistema y
genera una cabecera (`src/pi4/pro_sizes_pi4.h`, y su equivalente en
`build/` para QEMU) con la memoria que necesita cada uno.

Existe porque el dato **solo está en el ELF**: al empaquetar con `objcopy`
se pierde el `.bss`, que no tiene contenido. Sin esta cabecera, cada
programa recibía 16 MB por defecto y ocho tareas del sistema se comían los
128 MB de la reserva entera. Medido de verdad, la shell necesita 80 KB y el
editor medio MB.

Es una cabecera **generada**: no está en el repositorio y no se edita. Si
tu editor se queja de que no la encuentra, compila una vez y aparece.

### `NEMO_QEMU`, la única macro que distingue los dos builds

`QEMU_CFLAGS` lleva `-DNEMO_QEMU`. Es la primera y única macro del proyecto
que distingue plataformas dentro de un archivo compartido, y existe porque
`kernel.c`, `tasks.c` y `net.c` son los mismos para las dos y la red no:
`src/nic.h` elige el driver (`genet_pi4.c` en la Pi, `virtio_net.c` en
QEMU) y la dirección de reserva según esa macro. Todo
lo demás que es exclusivo de una plataforma sigue separado por archivo
(`src/pi4/` frente a los `.c` genéricos de `src/`), como siempre.

### `make run` arranca con red

El comando de QEMU lleva ahora `-netdev user,id=net0,hostfwd=tcp::2323-:2323`
y `-device virtio-net-device,netdev=net0`: la shell remota de Nemo OS
responde en `nc -v localhost 2323` desde el Mac mientras QEMU corre. (El
`ping` al invitado no funciona con la red de usuario de QEMU; TCP sí.)

Como añadido útil: la red de usuario de QEMU **trae servidor DHCP**, y
reparte `10.0.2.15`. Así que `make run` también sirve para probar el cliente
DHCP contra un servidor de verdad, sin tocar la placa.

### El compilador de Nemo Basic (`nbc.pro`) y su runtime

Desde la reescritura del compilador, `nbc-selfhost/` ya no contiene el
Nemo-Blitz antiguo (con su ensamblador de texto) sino **Nemo Basic**: un
BASIC propio cuyo compilador emite código máquina ARM64 directo, sin
ningún paso de ensamblador intermedio. Corre **dentro** de Nemo OS: desde
la shell, `run nbc.pro miprograma.nb` compila y deja el `.pro` al lado del
fuente. La extensión del lenguaje es `.nb` (antes era `.bb`).

Son **once archivos** que se compilan y enlazan juntos (la lista viva está
en `NBC_SRCS`, en el Makefile):

```
nbc_main.c     ← lee el .nb y escribe el .pro, con syscalls de Nemo OS
nb_lexer.c  nb_ast.c  nb_parser.c        ← análisis
nb_codegen.c  nb_symtab.c  nb_codebuf.c  nb_encode.c   ← generación ARM64
nb_string.c  nb_alloc.c                  ← cadenas y memoria
nb_include.c                             ← el Include de otros .nb
```

**El montón ampliado no es opcional.** El `nbc.pro` se compila con
`-DNB_ALLOC_POOL_SIZE='(8u*1024u*1024u)'`, y el Makefile lo hace en las
dos plataformas. El motivo: `nb_alloc.c` cumple **dos papeles a la vez**
— es la memoria de trabajo del propio compilador (el árbol, el búfer de
código, la tabla de símbolos) **y** es el código que se incrusta como
runtime dentro de cada `.pro` que genera. Los 2 MB por defecto son los
buenos para lo segundo (viajan dentro de cada programa compilado), pero se
le quedan cortos al compilador: al compilar algo con cadenas o gráficos
mete el bloque de runtime entero dentro de su propio búfer.

### `nb_runtime_blob.h` se regenera solo

Cada `.pro` que produce el compilador lleva incrustado un pequeño runtime
(reserva de memoria, cadenas, conversiones y trigonometría). Ese runtime
son cuatro archivos en C —`nb_alloc.c`, `nb_string.c`, `nb_convert.c`,
`nb_math.c`— compilados aparte y combinados en un solo bloque ya resuelto
por `tools/nb_elf_extract.py`, que hace de **enlazador en miniatura**:
coloca `.text`, `.rodata` y `.bss`, resuelve las reubicaciones (incluidas
las llamadas entre archivos distintos) y escribe `nb_runtime_blob.h`.

El Makefile lo hace solo:

```
nbc-selfhost/rt_%.o          ← los cuatro .o del runtime (monton POR DEFECTO, 2 MB)
nbc-selfhost/nb_runtime_blob.h  ← depende de esos .o y del propio script
$(NBC_OBJS), $(QEMU_NBC_OBJS)   ← dependen de la cabecera
```

Tocas cualquiera de los cuatro fuentes del runtime y se regenera la
cabecera y se recompila el compilador entero. **Esto antes era un paso
manual** y dio un problema real difícil de rastrear: `make clean` borraba
los `.o`, al rehacerlos era fácil quedarse con una cabecera vieja
compilada dentro del kernel, y el compilador fallaba por dentro sin que
nada apuntara a la causa.

Fíjate en el prefijo `rt_` de esos objetos: son **distintos** de los `.o`
del propio compilador, aunque salgan de los mismos `.c`. Los del runtime
llevan el montón por defecto; los del compilador, el ampliado. Mezclarlos
sería un error silencioso.

Es un archivo **generado**, como `pro_sizes_pi4.h`: no está en el
repositorio, y el texto de su cabecera lo escribe el propio script. Si
alguna vez quieres forzar la regeneración:

```bash
rm -f nbc-selfhost/nb_runtime_blob.h
make run
```

### `make clean` limpia también `lua/build/`

Llama al sub-make de `lua/` con `clean`, así que la primera compilación
después recompila el intérprete entero (unos 30 archivos). Es normal que
tarde algo más esa vez.

### Cuándo hace falta `make clean` de verdad

Cuando se añade una librería nueva a `SISTEMA`, un blob nuevo, o cambia
`embedded_lua.c` (el instalador tiene una tabla con los destinos). Para
cambios normales en `.c` o `.lua`, `make` a secas basta.

Para el runtime del compilador (`nb_alloc.c`, `nb_string.c`,
`nb_convert.c`, `nb_math.c`) **no hace falta**: sus dependencias están
declaradas, así que `make` a secas regenera la cabecera y recompila lo que
toque. Y un `make clean` tampoco rompe nada — borra los `rt_*.o`, y al
volver a construir se rehacen solos.

Para la imagen de tarjeta tampoco: `make imagen` depende de `kernel8.img`,
así que recompila lo que haga falta antes de empaquetar.
