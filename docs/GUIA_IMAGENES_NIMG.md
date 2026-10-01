# Imágenes NIMG en Nemo OS

NIMG es el formato de imagen del sistema. Lo usan `LoadImage`,
`LoadAnimImage`, las tiras de la barra de herramientas, los iconos del
escritorio y los sprites de los juegos.

Para la **biblioteca de iconos** que viene hecha en DOCUMENTOS/IMAGENES,
mira `GUIA_ICONOS.md`. Esta guía es sobre el formato en sí.

## El formato

Cabecera de 12 bytes y después los píxeles en crudo:

```
 0   "NIMG"                    4 bytes
 4   ancho                     uint32, little-endian
 8   alto                      uint32, little-endian
12   píxeles: R, G, B, A       ancho x alto x 4 bytes, fila a fila
```

Eso es todo. **Sin comprimir y sin paleta**: el tamaño en disco es siempre
`12 + ancho * alto * 4`. Una imagen de 256x256 pesa 256 KB; una de 1024x1024,
4 MB. Conviene tenerlo presente antes de hacer una hoja enorme.

## Los límites del sistema

| | |
|---|---|
| imágenes cargadas a la vez | 64 |
| tamaño máximo de una | 1024 x 1024 |
| memoria entre todas | 24 MB |

Los píxeles se reservan en el montón del kernel, así que un hueco vacío no
cuesta nada. Si no cabe lo que pides, `LoadImage` y `CreateImage` devuelven
**−1**: el programa se entera, no hay fallo silencioso.

## Transparencia: es un color, no el alfa

`MaskImage img, 0xFF00FF` hace que **ese color exacto** deje de dibujarse.
Se compara el color entero en cada dibujado; la imagen no se toca. El
magenta es el de costumbre porque casi nunca aparece en un dibujo de verdad.

```basic
nave = LoadImage("nave.nimg")
MaskImage nave, 0xFF00FF
DrawImage nave, x, y
```

Si conviertes desde PNG y el conversor reduce la paleta, **vuelve a fijar el
magenta exacto después**: si la cuantización lo mueve un solo tono, deja de
coincidir y el fondo se dibuja.

`DrawBlock` **ignora la máscara** y el alfa a propósito: es la copia opaca,
para fondos.

## Hojas de celdas

Una hoja es una imagen con los fotogramas en celdas iguales, una al lado de
otra. Se carga entera con un solo identificador:

```basic
; 24 celdas de 16x16 en una imagen de 384x16
bichos = LoadAnimImage("bichos.nimg", 16, 16, 0, 24)
DrawImage bichos, x, y, 9         ; la celda 9
```

`primera` desplaza el origen: con `primera = 8`, el fotograma 0 es la celda
8. Para un tileset, `primera` a 0 y `cuantas` al total.

Las hojas no son un adorno: con 64 huecos de imagen en todo el sistema, un
tile por imagen se acaba enseguida. Un tileset de 128 celdas cabe en uno.

**El espejo va dentro de la hoja.** El kernel no sabe dibujar una imagen del
revés. Si un personaje mira a los dos lados, la hoja lleva sus fotogramas y
los mismos espejados, y cambiar de lado es sumar el número de fotogramas:

```
0-3   cangrejo a la derecha      12-15  el mismo a la izquierda
4-7   moneda                     16-19  moneda espejada
8-11  murciélago a la derecha    20-23  murciélago a la izquierda
```

## Dibujar dentro de una imagen

`SetBuffer ImageBuffer(img)` manda todo el dibujo a esa imagen; `SetBuffer
BackBuffer()` vuelve a la ventana. Dentro, `Cls` borra **la imagen**.

Lo respetan también `DrawImage` y `DrawBlock` — antes solo
`Rect`, `Line` y `Oval`, y `DrawImage` pintaba en la ventana sin avisar.

Sirve para montar algo grande una vez y dibujarlo luego de una sola llamada:

```basic
fondo = CreateImage(256, 240)
SetBuffer ImageBuffer(fondo)
For fy = 0 To 14
  For fx = 0 To 15
    DrawBlock tiles, fx * 16, fy * 16, MiTile(fx, fy)
  Next
Next
SetBuffer BackBuffer()
```

Una imagen no puede dibujarse sobre sí misma: origen y destino se pisarían a
medio camino, y el dibujado se rechaza.

## Cuánto cuesta dibujar

Medido en Raspberry Pi 4:

| | |
|---|---|
| una llamada al sistema, dibuje lo que dibuje | 3,5 µs |
| un píxel | 4 ns |

Un tile de 16x16 son 256 píxeles: **un microsegundo de píxeles contra tres y
medio de peaje**. Dibujar tile a tile es pagar peajes.

Una pantalla de 640x480 con tiles de 16 son 1200 llamadas: 4,2 ms de peaje
antes de mover un píxel, cuando un fotograma a 100 por segundo dura 10 ms.
Los mismos píxeles repartidos en 8 imágenes grandes son 28 µs.

De ahí la costumbre para cualquier cosa con scroll: **montar el fondo en
imágenes grandes con `ImageBuffer` y dibujarlo en pocas llamadas**.

## Dónde se buscan

Un nombre a secas se busca en la **raíz**, en **DOCUMENTOS** y en
**DOCUMENTOS/IMAGENES**, por ese orden.

También se puede decir la ruta:

```basic
hoja = LoadAnimImage("JUEGOS/NEMO/tiles.nimg", 16, 16, 0, 32)
```

Vale `/` y `\`, y una barra inicial que solo significa "desde la raíz". No
se admite `..`.

## Hacer un NIMG desde el Mac

El formato es tan simple que no hace falta herramienta:

```python
import struct
with open('salida.nimg', 'wb') as f:
    f.write(b'NIMG' + struct.pack('<II', ancho, alto))
    for y in range(alto):
        for x in range(ancho):
            r, g, b = imagen.getpixel((x, y))
            f.write(bytes((r, g, b, 255)))
```

Y para verlo al revés, `Image.frombytes('RGBA', (w, h), datos[12:])`.

Hay dos conversores hechos en `herramientas/`: `tiles_nemo.py` dibuja un
tileset, y `nemo_sprites.py` convierte una hoja de poses grande en celdas
iguales.

## Dos avisos de tamaño

**El explorador copia por trozos**, así que ya no hay
límite al copiar: lo que quepa en el destino se copia. Antes se quedaba en
4 MB.

**Un archivo de NemoFS no pasa de 8,25 MB.** Una imagen de 1024x1024 son
4 MB y entra de sobra, pero es el techo.
