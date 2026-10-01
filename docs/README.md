# Las guías de Nemo OS

Esta carpeta es la documentación del sistema, y **viaja dentro de él**: varios
de estos archivos se empotran en el kernel y se instalan en la carpeta
`MANUALES` en el primer arranque, de modo que Nemo OS lleva sus propios
manuales y se leen desde el visor del propio sistema. Si los borras, no
compila.

**[INDICE_MANUALES.md](INDICE_MANUALES.md)** es la puerta de entrada: lista las
guías una por una y dice de qué va cada cual.

Para compilar el proyecto, arrancarlo en QEMU o instalarlo en una Raspberry Pi,
está el [README de la raíz](../README.md), que es el que se mantiene al día con
el Makefile.

## Por dónde empezar

| Si quieres… | Lee |
| --- | --- |
| Entender cómo funciona por dentro | [GUIA_BAREMETAL_RASPBERRY_PI4.md](GUIA_BAREMETAL_RASPBERRY_PI4.md) |
| Programar en el lenguaje del sistema | [GUIA_NEMO_BASIC.md](GUIA_NEMO_BASIC.md), y luego [REFERENCIA_NEMO_BASIC.md](REFERENCIA_NEMO_BASIC.md) |
| Programar en Lua | [../lua/GUIA_PROGRAMACION_LUA_NEMO_OS.md](../lua/GUIA_PROGRAMACION_LUA_NEMO_OS.md) |
| Hacer ventanas con el ratón | [GUIA_ARONNAX_NEMO_OS.md](GUIA_ARONNAX_NEMO_OS.md) |
| Conectar algo a los pines | [GUIA_GPIO_NEMO_OS.md](GUIA_GPIO_NEMO_OS.md) |
| Entender el Makefile | [GUIA_MAKEFILE.md](GUIA_MAKEFILE.md) |
| Instalarlo en una tarjeta | [INSTALACION.md](INSTALACION.md) |

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

## Enlaces

### Libros de la colección

- **Capitán de mi propio sistema** — El diario de Nemo OS → [amzn.eu/d/030HpOwK](https://amzn.eu/d/030HpOwK)
- **El Nautilus, pieza por pieza** — Manual técnico de Nemo OS → [amzn.eu/d/08wkFff8](https://amzn.eu/d/08wkFff8)
- **Cartas de navegación del Nautilus** — Guía de estudios de sistemas operativos, con Nemo OS → [amzn.eu/d/0fZR5FKS](https://amzn.eu/d/0fZR5FKS)
- **40seconds · Crónicas de intrusión — Vol. 1** (Diarios de un hacker) → [amzn.eu/d/08tG6k2Q](https://amzn.eu/d/08tG6k2Q)
- **40seconds · Crónicas de intrusión — Vol. 2** (Diarios de un hacker) → [amzn.eu/d/03FcGv4D](https://amzn.eu/d/03FcGv4D)

### Comunidad

- **r/Astillero** — bitácora del laboratorio, novedades y comunidad → [reddit.com/r/Astillero](https://www.reddit.com/r/Astillero)

## Licencia

Apache 2.0. Úsalo libremente.
