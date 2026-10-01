# Manuales de Nemo OS

*Toda la documentación del sistema. Doble clic en cualquiera para abrirlo.*

Esta carpeta se llama **MANUALES** y está en la raíz del disco. Antes todo esto
vivía en `DOCUMENTOS`, que es tu carpeta: veinte guías no dejaban sitio para tus
cosas.

En `DOCUMENTOS` quedan las páginas de ejemplo (`bienvenida.html`,
`demo_lua.html`, `ayuda_html.html`) y los dos proyectos de Aronnax
(`panel_nautilus.anx`, `editor_texto.anx`).

---

## Empezar

- [AYUDA/indice.html](AYUDA/indice.html) — **la ayuda de usuario**: primeros
  pasos, archivos y discos, los programas, ajustes, teclas y qué hacer cuando
  algo no va. Si no sabes por dónde empezar, empieza aquí.

## Nemo Basic

El lenguaje propio del sistema. Se compila dentro de Nemo OS con `nbc.pro`.

- [GUIA_NEMO_BASIC.md](GUIA_NEMO_BASIC.md) — **la guía**, en cinco partes:
  tu primer programa, cómo compilar desde el IDE o la shell, el lenguaje
  entero y la biblioteca.
- [REFERENCIA_NEMO_BASIC.md](REFERENCIA_NEMO_BASIC.md) — **la referencia**, en
  cinco partes: los 260 comandos y funciones, cada uno con su firma, su número
  de syscall y ejemplos. La lista salió del propio compilador y las syscalls se
  verificaron ejecutando, una por una.
- [GUIA_AMBITO_NEMO_BASIC.md](GUIA_AMBITO_NEMO_BASIC.md) — el ámbito de las variables, y por qué
  `Local` deja de ser opcional en cuanto hay recursión.
- [GUIA_ARONNAX_NEMO_OS.md](GUIA_ARONNAX_NEMO_OS.md) — el diseñador visual: colocar controles
  con el ratón y saltar al código.

## Lua

El lenguaje del sistema. Casi todas las aplicaciones del escritorio están
escritas en él.

- [GUIA_PROGRAMACION_LUA_NEMO_OS.md](GUIA_PROGRAMACION_LUA_NEMO_OS.md) — programar en Lua dentro de Nemo OS.
- [GUIA_LUA_SYSCALLS.md](GUIA_LUA_SYSCALLS.md) — las llamadas al sistema desde
  Lua, una por una.
- [GUIA_LUA_SYSCALLS_2.md](GUIA_LUA_SYSCALLS_2.md) — la continuación:
  controles, menús, árbol, temporizadores y bancos de memoria.

## Formatos y recursos

- [GUIA_IMAGENES_NIMG.md](GUIA_IMAGENES_NIMG.md) — el formato de imagen propio: cabecera,
  píxeles, transparencia por color clave y hojas de celdas.
- [GUIA_ICONOS_NEMO_OS.md](GUIA_ICONOS_NEMO_OS.md) — los iconos del sistema y cómo hacer los
  tuyos.
- [GUIA_HTML_NEMO_OS.md](GUIA_HTML_NEMO_OS.md) — qué HTML y qué CSS entiende el visor, y los
  `<script type="text/lua">`.

## Hardware y red

- [GUIA_GPIO_NEMO_OS.md](GUIA_GPIO_NEMO_OS.md) — las patillas de la Raspberry Pi 4: pines, PWM,
  I2C y SPI, con esquemas de conexión.
- [GUIA_RED_NEMO_OS.md](GUIA_RED_NEMO_OS.md) — la red por dentro: Ethernet, ARP,
  DHCP, UDP y HTTP, los comandos de red de Nemo Basic y la terminal remota por
  el puerto 2323.

---

## Por qué están partidas en varias partes

El visor carga el documento entero en el montón de 4 MB de su tarea, así que
una guía grande no cabe de una pieza. Las grandes van en partes enlazadas: al
final de cada una hay un enlace a la siguiente.

Lo que manda no es el tamaño en bytes, son las **filas de tabla**: maquetar
cada una tiene su coste. Una tabla de 190 filas no cabe; una lista de 190
líneas sí. Por eso el índice alfabético de la referencia es una lista.
