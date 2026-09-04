[GUIA_RAPIDA.md](https://github.com/user-attachments/files/31822869/GUIA_RAPIDA.md)
# Nemo OS

Un sistema operativo ARM64 escrito desde cero — kernel, ensamblador propio, lenguaje de programación propio (Nemo-Blitz), y dos compiladores que se verifican byte a byte entre sí. Corre sobre QEMU, sin dependencias de hardware físico real.

Esta guía te lleva de cero a tener Nemo OS arrancando en tu máquina en unos minutos, sin necesitar ningún libro ni conocimiento previo del proyecto.

---

## Requisitos

Antes de nada, instala:

- **Un compilador cruzado de C para ARM64 bare-metal** (`aarch64-none-elf-gcc` o equivalente)
- **QEMU** (`qemu-system-aarch64`)
- **mtools** y **dosfstools** (para crear la imagen de disco)
- **make**

En macOS con Homebrew:

```bash
brew install aarch64-elf-gcc qemu mtools dosfstools make
```

En Ubuntu/Debian:

```bash
sudo apt install gcc-aarch64-linux-gnu qemu-system-arm mtools dosfstools make
```

Comprueba que todo está instalado:

```bash
aarch64-none-elf-gcc --version
qemu-system-aarch64 --version
```

---

## Compilar todo el proyecto

```bash
git clone https://github.com/ericmugnoz/nemo-os.git
cd nemo-os
```

### 1. El ensamblador propio (`nemoas`)

```bash
cd nemoas
make
cd ..
```

### 2. El kernel

```bash
cd kernel
make
cd ..
```

Esto produce `kernel/kernel.img`, el binario que QEMU carga directamente.

### 3. El compilador de Nemo-Blitz

```bash
cd nbc-host
make
cd ..
```

### 4. Los programas de sistema

```bash
cd programas
for prog in shell explorer editor ide gadgetdemo; do
  ../nbc-host/build/nbc $prog.bb -o $prog.pro
done
cd ..
```

### 5. La imagen de disco

```bash
dd if=/dev/zero of=disco.img bs=1M count=64
mkfs.fat -F 32 disco.img
mcopy -i disco.img programas/*.pro ::/
```

---

## Arrancar Nemo OS

```bash
qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a72 \
    -m 512M \
    -kernel kernel/kernel.img \
    -drive file=disco.img,if=none,id=hd0,format=raw \
    -device virtio-blk-device,drive=hd0 \
    -device virtio-keyboard-device \
    -device virtio-tablet-device \
    -device virtio-sound-device \
    -device ramfb \
    -serial stdio \
    -display default
```

Si todo fue bien, verás los mensajes de arranque por la terminal (gracias a `-serial stdio`) y, después, el escritorio de Nemo OS.

> **Consejo:** guarda este comando en un script `run.sh` para no tener que teclearlo cada vez.

---

## Primeros pasos

Nemo OS arranca directamente al escritorio gráfico, no a una shell de texto — el ratón (a través de `virtio-tablet-device`) y el teclado (`virtio-keyboard-device`) ya funcionan desde el primer instante.

Desde el escritorio, abre el **explorador de archivos** para navegar y lanzar cualquier programa con doble clic:

- `explorer.pro` — el explorador de archivos, con menú contextual de clic derecho
- `editor.pro` — editor de texto simple
- `ide.pro` — entorno de desarrollo, con pestañas y un botón de compilar
- `gadgetdemo.pro` — una galería con un ejemplo de cada control de interfaz

Si prefieres una línea de comandos, `shell.pro` abre una consola de texto dentro de su propia ventana, con estos comandos:

| Comando | Qué hace |
|---|---|
| `DIR` | Lista el contenido de la carpeta actual |
| `CD carpeta` | Cambia de carpeta |
| `RUN programa.pro` | Ejecuta un programa |
| `DEL archivo` | Borra un archivo |
| `MKDIR nombre` | Crea una carpeta |
| `CLS` | Limpia la pantalla |
| `HELP` | Muestra esta misma lista |
| `EXIT` | Sale de la shell |

---

## Escribe tu primer programa

Crea un archivo `hola.bb`:

```
Print "Hola desde Nemo OS"
```

Compílalo con `nbc-host`:

```bash
./nbc-host/build/nbc hola.bb -o hola.pro
```

Copia `hola.pro` a la imagen de disco (`mcopy -i disco.img hola.pro ::/`), arranca Nemo OS, y ábrelo desde el explorador con doble clic — o escribe `RUN hola.pro` en `shell.pro`, si prefieres la línea de comandos.

---

## Estructura del repositorio

```
nemo-os/
├── kernel/          # el nucleo: arranque, excepciones, memoria,
│                     # planificador, sistema de archivos, gráficos, drivers
├── nemoas/           # el ensamblador ARM64 propio
├── nbc-host/         # compilador de Nemo-Blitz (corre en tu máquina)
├── nbc-selfhost/      # el mismo compilador, escrito en su propio lenguaje
├── programas/         # shell, explorador, editor, IDE, galería de gadgets
└── kernel/kernel.ld   # script de enlazado
```

---

## ¿Quieres entender cómo funciona por dentro?

Este README te lleva hasta tener Nemo OS corriendo, pero no explica *por qué* está construido como está. Si quieres ese nivel de detalle, existe una colección de tres libros escrita específicamente para eso:

- **Capitán de mi propio sistema** — el relato completo de cómo se construyó, con los bugs y las dudas incluidas
- **El Nautilus, pieza por pieza** — la referencia técnica completa: cada archivo de código fuente explicado, las 357 funciones del lenguaje documentadas, y esta misma guía de compilación con mucho más detalle
- **Cartas de navegación del Nautilus** — un curso completo de sistemas operativos, con Nemo OS como caso práctico en cada capítulo

Ninguno es necesario para usar o modificar el código de este repositorio — todo lo que hace falta para eso está aquí, en este README y en los propios comentarios del código.

---

## Contribuir

El código está abierto para leerse, usarse, modificarse, y ampliarse — no solo para consultarse. Si quieres:

- Añadir soporte para hardware real (empezando por Raspberry Pi)
- Implementar journaling en NemoFS
- Añadir memoria virtual real
- Simplemente arreglar un bug

Abre un pull request. Si tienes dudas sobre por dónde empezar, abre un issue.

## Apoyar el proyecto

Nemo OS es uno de los tres pilares de un laboratorio independiente de tecnología en español que estoy construyendo: un modelo de lenguaje (LLM) abierto, este sistema operativo, y la colección de libros que lo documenta. Los tres nacen de la misma idea — que se puede hacer trabajo técnico serio, en español, sin depender de ninguna gran empresa ni esperar permiso de nadie para empezar.

Todo lo recaudado, tanto de la venta de los libros de la colección como de cualquier donación o colaboración directa, se destina íntegramente a sostener ese laboratorio: el tiempo dedicado a programarlo, y a que los próximos proyectos —empezando por el LLM abierto— puedan llegar a existir con la misma dedicación que este.

Si quieres colaborar, dos formas directas de hacerlo:

- **Comprando alguno de los libros de la colección** (mencionados más arriba) — no hace falta ninguno para usar este código, pero cada compra ayuda a que el laboratorio siga adelante.
- **Una donación directa**, del tamaño que sea: [buymeacoffee.com/ericmunoz](https://buymeacoffee.com/ericmunoz)

No hay ninguna recompensa exclusiva a cambio, ni ninguna función de Nemo OS reservada para quien colabore — el código es y seguirá siendo abierto para todos, colabores o no. Es, simplemente, la forma más directa de decir "esto merece existir" con algo más que palabras.

## Licencia

Apache 2.0. Úsalo libremente.
