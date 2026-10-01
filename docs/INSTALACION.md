# Instalar Nemo OS 1.0 en una Raspberry Pi 4

Guía completa, desde la descarga hasta el sistema configurado. Son diez minutos,
y la parte que más se olvida está al final: **hay que reiniciar y pasar por el
Particionador**.

---

## Qué necesitas

| | |
| --- | --- |
| **Placa** | Raspberry Pi 4 Model B (cualquier cantidad de RAM) |
| **Tarjeta** | microSD de **16 GB** (ver [Qué tarjeta usar](#qué-tarjeta-usar)) |
| **Pantalla** | Un monitor o TV por HDMI, en el puerto micro-HDMI **más cercano al conector de alimentación** (HDMI0) |
| **Teclado y ratón** | USB, por cable. Sirve cualquiera de los normales |
| **Alimentación** | La fuente USB-C de la Pi 4 (5 V, 3 A) |
| **Para escribir la tarjeta** | Un ordenador con **Raspberry Pi Imager** y un lector de tarjetas |

Nemo OS 1.0 es **solo para la Raspberry Pi 4**. No arranca en una Pi 3, en una
Pi 5 ni en una Pi Zero.

No hace falta red para instalarlo ni para usarlo. Si enchufas un cable de red,
el sistema pedirá su dirección al router él solo.

---

## 1. Descargar la imagen

Baja **`nemo-os-1.0.1.img.xz`** de la página de
[Releases](https://github.com/ericmugnoz/nemo-os/releases).

Son unos **3,5 MB**. No es un error: el sistema entero —kernel, ventanas,
compilador, intérprete de Lua, 31 manuales y los programas de ejemplo— ocupa
unas decenas de megas, y comprimido se queda en eso.

**No descomprimas el archivo.** El Raspberry Pi Imager lee el `.xz` tal cual.

---

## 2. Escribir la tarjeta con el Raspberry Pi Imager

Si no lo tienes, se descarga gratis de
[raspberrypi.com/software](https://www.raspberrypi.com/software/) (hay versión
para Windows, macOS y Linux).

1. Mete la microSD en el lector.
2. Abre el **Raspberry Pi Imager**.
3. En **«Dispositivo Raspberry Pi»**, elige **Raspberry Pi 4**.
4. En **«Sistema operativo»**, baja hasta el final de la lista y elige
   **«Usar personalizado»** (*Use custom*). Busca el archivo
   `nemo-os-1.0.1.img.xz` que acabas de descargar.
5. En **«Almacenamiento»**, elige tu microSD. Comprueba dos veces que es la
   tarjeta y no otro disco: lo que haya dentro se borra.
6. Pulsa **«Siguiente»**.
7. **Cuando pregunte si quieres aplicar ajustes de personalización, responde
   que NO** (*«No», o «Editar ajustes» → nada*). Esos ajustes son de Raspberry
   Pi OS —usuario, contraseña, wifi, SSH— y aquí no significan nada. Si los
   aplicas, el Imager escribe archivos que Nemo OS no usa.
8. Confirma que se borre la tarjeta y espera. Tarda muy poco, porque la imagen
   es pequeña.
9. Cuando el Imager diga que ha terminado, saca la tarjeta.

> **¿Y si el Imager no te deja elegir la imagen?** Asegúrate de que estás en
> «Usar personalizado» y no en la lista de sistemas oficiales. Si tu versión del
> Imager es muy antigua y no acepta `.xz`, descomprime el archivo y usa el
> `.img` que sale.

---

## 3. Primer arranque

Con la Pi apagada: mete la tarjeta, enchufa el HDMI, el teclado y el ratón, y
por último la alimentación.

El primer arranque **tarda más que los siguientes**, porque el sistema tiene
trabajo que hacer una sola vez:

- formatea su partición en NemoFS, su propio sistema de archivos;
- se instala a sí mismo dentro: los programas, los 31 manuales, las tipografías,
  los iconos, los 39 programas de Lua y los ejemplos en Nemo Basic.

Cuando termina, sale el escritorio.

> ### El primer arranque sale sin fondo de pantalla
>
> **Es normal en la 1.0, y no es un fallo de tu tarjeta.** El escritorio busca
> el fondo guardado *antes* de que el fondo se haya instalado, así que la
> primera vez aparece con el color de fondo pelado. Al reiniciar ya sale.
>
> No hay que arreglar nada: el paso siguiente es reiniciar de todas formas.

---

## 4. Reiniciar

**Menú Inicio → Reiniciar.**

Este paso no es opcional. Al volver:

- sale el **fondo de pantalla**;
- todo lo que se instaló en el primer arranque ya está en su sitio y el
  sistema arranca a su velocidad normal.

---

## 5. Terminar de configurar la tarjeta: el Particionador

La imagen mide unos 3 GB (1 GB de arranque + 2 GB para NemoFS) **sea cual sea el
tamaño de tu tarjeta**. Al escribirla en una de 16 GB, el resto de la tarjeta
queda sin usar hasta que se lo digas.

**Menú Inicio → Sistema → Particionador.**

La ventana enseña la tarjeta entera en una barra: la partición de arranque, lo
que ocupa NemoFS, lo que tiene libre, y el hueco sin usar del final.

Pulsa **«Usar todo el espacio libre»** y confirma con **«Sí, ampliar»**.

- La partición de arranque **no se toca**.
- **No quites la tarjeta ni apagues la Pi** mientras trabaja.
- Al terminar, NemoFS ocupa hasta el final de la tarjeta y el Particionador lo
  dice.

Ya está. A partir de aquí el sistema está configurado del todo.

---

## Qué tarjeta usar

**Recomendada: 16 GB.** Es la que aprovecha el sistema entero sin dejar nada
fuera.

**Funciona con tarjetas de hasta 128 GB**, probado en 16, 32, 64 y 128 GB. Pero
**Nemo OS 1.0 usa como mucho los primeros 16 GB** de la tarjeta, sea del tamaño
que sea: en una de 128 GB, los otros 112 quedan sin usar.

No es un descuido, es una decisión. El mapa de bloques libres de NemoFS vive
entero en la memoria del kernel y cuesta 256 bytes por cada MB de tarjeta;
dejarlo crecer sin límite se comería la RAM que necesitan los programas. Con una
instalación de unos 25 MB, 16 GB sobran de largo.

En una tarjeta grande, el Particionador te lo dirá con estas palabras:

```
NemoFS ya esta en su tamaño maximo (16 GB). El mapa de bloques libres
vive en la memoria del kernel, y dejarlo crecer mas se comeria la RAM
que necesitan los programas. El resto de la tarjeta queda sin usar.
```

Eso no es un error: es el sistema diciendo que ha llegado a su techo.

> **Una tarjeta gastada da síntomas raros.** Si las ventanas salen vacías, si
> Lua no arranca o si algo no se guarda, prueba con otra tarjeta antes de buscar
> el problema en otro sitio. El sistema avisa por el registro cuando la tarjeta
> rechaza las escrituras: `nemofs: LA TARJETA RECHAZA LA ESCRITURA`.

---

## Si algo va mal

**No sale nada por el HDMI.** Usa el puerto micro-HDMI que está **junto al
conector de alimentación** (HDMI0); el otro no se usa. Enchufa el monitor
**antes** de dar corriente a la Pi. Y si el monitor es antiguo o va por un
adaptador, prueba con otro: la imagen fuerza la salida HDMI, pero no puede
inventarse un modo de vídeo que la pantalla no acepte.

**El teclado o el ratón no responden.** Tienen que ser USB por cable. Si van por
un hub, prueba enchufándolos directamente a la placa.

**Arranca y se queda parado.** Nemo OS deja un registro de todo el arranque en la
tarjeta, en el archivo **`NEMO.LOG`** de la partición de arranque: apaga, mete la
tarjeta en el ordenador y ábrelo con cualquier editor de texto. Ahí está paso a
paso lo que hizo el sistema y dónde se quedó.

**Quieres verlo en directo.** La imagen trae la consola serie activada
(`enable_uart=1` en `config.txt`): con un adaptador USB-serie en los pines GPIO
14 y 15 se ve el arranque entero mientras ocurre, incluso antes de que haya
pantalla.

---

## Volver a Raspberry Pi OS

Nada de esto es permanente: la tarjeta se vuelve a escribir con el Raspberry Pi
Imager como cualquier otra, y la Pi arranca lo que haya en ella. Nemo OS no toca
la placa, solo la tarjeta.

---

## Y ahora qué

- El **Manual** está dentro del sistema: Menú Inicio → Ayuda → Manual. Son las
  32 guías, instaladas en la propia tarjeta.
- Para escribir programas, **Timonel** y el **IDE** están en Menú Inicio →
  Programación. Nemo OS trae su propio lenguaje con su compilador, y el
  compilador corre dentro del sistema.
- Si quieres compilarlo tú mismo desde el código, eso está en el
  [README](README.md).
