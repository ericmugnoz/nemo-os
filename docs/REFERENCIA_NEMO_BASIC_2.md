# Referencia de Nemo Basic (2)

*Consola, tiempo, pantalla, gráficos, imágenes, teclado y ratón.*

Viene de [REFERENCIA_NEMO_BASIC.md](REFERENCIA_NEMO_BASIC.md).
# 2. Consola, tiempo y pantalla

## 2.1 Modo de pantalla

Un programa elige **una** de estas cuatro al empezar. Son sentencias del
lenguaje, no funciones.

| | Qué hace |
|---|---|
| `Console` | programa de consola: escribe texto en la ventana de la shell |
| `Graphics ancho, alto` | ventana de dibujo del tamaño que pidas |
| `CreateWindow titulo$, x, y, ancho, alto` | ventana con título y controles |
| `Desktop` | dibuja directamente en el escritorio, sin ventana |

```basic
Graphics 320, 240
Color 0, 0, 255
Rect 10, 10, 100, 50
```

Un programa tiene **una** ventana. `CloseWindow()` la cierra sin terminar el
programa, y devuelve 1 si la había:

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `CloseWindow()` | cierra la ventana del programa | — | 313 |

Sirve para atender el aviso de cerrar (el evento `$803`, 2051) sin morirse en el
acto: se cierra la ventana, se guarda lo que estuviera a medias y el programa
termina cuando le conviene. También cierra sus controles, así que un
`CreateWindow` posterior empieza de cero con una ventana nueva.

## 2.2 Consola

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `Print expr` | escribe y salta de línea | — | 11 |
| `Input$ [aviso$]` | pide una línea y la devuelve | — | 258 |
| `Input [aviso$]` | pide una línea y la devuelve como número | — | 258 |

**Cuidado con los separadores de `Print`.** La coma concatena; el punto y coma
**se come lo que venga detrás, sin avisar**:

```basic
Console
Print "a", 1, "b"      ; sale a1b
Print "a"; 1           ; sale SOLO a -- el 1 se pierde
Print "a" + Str$(1)    ; la forma segura: a1
```

Salida: `a1b`, `a`, `a1`.

Si quieres varias cosas en una línea, concaténalas con `+` y `Str$()`, o usa
comas. El punto y coma es la trampa.

## 2.3 Tiempo

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `MilliSecs()` | milisegundos desde el arranque | — | 2 |
| `MicroSecs()` | microsegundos desde el arranque | — | 272 |
| `Delay ms` | espera (sentencia) | — | 1 |
| `CreateTimer(hz)` | temporizador de ventana | `N` | 127 |
| `TimerTicks(t)` | cuántos latidos van | `N` | 219 |
| `PauseTimer(t)`, `ResumeTimer(t)`, `ResetTimer(t)`, `FreeTimer(t)` | | `N` | 216, 217, 218, 131 |

**`MilliSecs()` avanza a saltos de 10 ms**, porque el reloj del planificador va
a 100 Hz. Para medir algo corto —un fotograma, una función— hace falta
`MicroSecs()`.

```basic
Console
t = MicroSecs()
For i = 1 To 1000
  x = i * 2
Next
Print "el bucle tardo microsegundos:"
Print MicroSecs() - t
```

## 2.4 Ceder el turno

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `Pump()` | cede el procesador al resto del sistema | — | 14 |

**El planificador es cooperativo: esto es imprescindible.** Un bucle que no
llame a `Pump()` ni a `WaitEvent()` se queda el turno y el resto del sistema
se para.

---

# 3. Gráficos

## 3.1 Dibujar

Sentencias del lenguaje. Todas dibujan con el color activo, salvo `ClsColor`,
que fija el color del borrado.

| | Qué hace | Syscall |
|---|---|---|
| `Cls` | borra con el color de `ClsColor` | 30 |
| `Color r, g, b` | color activo para lo que se dibuje | — |
| `ClsColor r, g, b` | color con el que borra `Cls` | — |
| `Plot x, y` | un píxel | 30 |
| `Rect x, y, ancho, alto` | rectángulo relleno | 30 |
| `Oval x, y, ancho, alto` | elipse rellena | 47 |
| `Line x1, y1, x2, y2` | línea | 257 |
| `Text x, y, cadena$` | texto | 31 |

```basic
Graphics 200, 120
ClsColor 0, 0, 40
Cls
Color 255, 200, 0
Rect 20, 20, 60, 30
Oval 100, 20, 60, 30
Color 255, 255, 255
Line 20, 70, 180, 70
Text 20, 85, "Nemo OS"
Plot 100, 100
```

**`Oval` siempre sale relleno.** El comentario de `syscall.h` dice que el
quinto argumento es «sólido», pero la implementación lo usa como color: manda
el código, no el comentario.

## 3.2 A dónde va el dibujo

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `SetBuffer(b)` | fija el destino del dibujo | `N` | 128 |
| `ImageBuffer(img)` | el destino «esta imagen» | `N` | — |
| `FrontBuffer()`, `BackBuffer()` | los de siempre de Blitz | — | — |
| `CreateCanvas(x, y, ancho, alto)` | un lienzo como control | `NNNN` | 188 |
| `CanvasBuffer(c)` | el destino «este lienzo» | `N` | — |
| `FlipCanvas(c)` | vuelca el lienzo a la pantalla | `N` | — |

`ImageBuffer` y `CanvasBuffer` **no llaman al sistema**: solo calculan el
número que `SetBuffer` espera. Un lienzo se distingue de una imagen por un
`+100000` en ese número, que es cómo lo reconoce el kernel.

```basic
Graphics 200, 120
fondo = CreateImage(200, 120)
SetBuffer ImageBuffer(fondo)
Color 0, 60, 90
Rect 0, 0, 200, 120
SetBuffer FrontBuffer()
DrawImage fondo, 0, 0
```

## 3.3 Medidas de la ventana

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `ClientWidth()` | ancho de la zona de dibujo | — | 33 |
| `ClientHeight()` | alto de la zona de dibujo | — | 33 |

---

# 4. Imágenes

## 4.1 Cargar, crear y dibujar

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `LoadImage(nombre$)` | carga un `.nimg`, o −1 | `S` | 49 |
| `CreateImage(ancho, alto)` | imagen vacía, o −1 | `NN` | 52 |
| `LoadAnimImage(nombre$, ancho, alto, primera, cuantas)` | hoja de celdas iguales | `SNNNN` | 99 |
| `DrawImage img, x, y [, fotograma]` | dibuja | `NNN` | 50 |
| `DrawBlock img, x, y [, fotograma]` | dibuja **opaco** | `NNN` | 50 |
| `ImageWidth(img)`, `ImageHeight(img)` | tamaño | `N` | 51 |
| `MaskImage img, color` | ese color pasa a transparente | `NN` | 92 |
| `HandleImage img, x, y` | punto de agarre | `NNN` | 89 |
| `CopyImage(img)` | duplica | `N` | 93 |
| `SaveImage img, nombre$` | guarda en disco | `NS` | 94 |
| `FreeImage img` | libera | `N` | 88 |

Los límites: hasta **64 imágenes**, cada una de hasta **1024×1024**, con un
tope de 4 MB por imagen y 24 MB entre todas.

```basic
Console
img = CreateImage(32, 16)
Print ImageWidth(img)
Print ImageHeight(img)
copia = CopyImage(img)
Print ImageWidth(copia)
FreeImage copia
FreeImage img
```

Salida: `32`, `16`, `32`.

## 4.2 Filas de píxeles

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `FillRow img, x, y, cuantos, color` | pinta una fila de un color | `NNNNN` | 291 |
| `RowRun(img, x, y, maximo, color)` | cuántos seguidos **son** de ese color | `NNNNN` | 292 |
| `RowSkip(img, x, y, maximo, color)` | cuántos seguidos **no** lo son | `NNNNN` | 292 |
| `ReadRow img, x, y, cuantos, array` | trae la fila a un array | `NNNN` | 289 |
| `WriteRow img, x, y, cuantos, array` | lleva el array a la fila | `NNNN` | 290 |

Con `maximo` negativo se avanza hacia la **izquierda**. El píxel de partida
cuenta si cumple. Todos se paran en el borde de la imagen y devuelven cuántos
píxeles han tratado, o **−1** si los argumentos no valen. Un 0 es «no quedaba
nada dentro»; no es lo mismo que un error.

```basic
Console
Dim fila(16)
img = CreateImage(16, 4)
FillRow img, 0, 0, 16, $FF0000
FillRow img, 4, 1, 8, $00FF00
Print RowRun(img, 0, 0, 16, $FF0000)
Print RowRun(img, 4, 1, 16, $00FF00)
Print RowSkip(img, 0, 1, 16, $00FF00)
Print RowRun(img, 11, 1, -16, $00FF00)
ReadRow img, 0, 0, 16, fila
Print fila(0)
```

Salida: `16`, `8`, `4`, `8`, `16711680`.

**El quinto argumento de `ReadRow`/`WriteRow` es un array declarado con
`Dim`**, no una expresión; si pones otra cosa, el compilador lo dice. Y
`cuantos` se acota solo al tamaño del array: un `ReadRow` de 500 sobre un
`Dim a(10)` llena los diez y para, no escribe los otros 490 encima de tus
demás variables.

## 4.3 Cuánto cuesta dibujar

Medido en la Raspberry Pi 4:

| | |
|---|---|
| una llamada al sistema, dibuje lo que dibuje | **3,5 µs** |
| un píxel | **4 ns** |

Un tile de 16×16 son 256 píxeles: un microsegundo de píxeles contra tres y
medio de peaje. **Dibujar tile a tile es pagar peajes, no dibujar.** Una
pantalla de 640×480 con tiles de 16 son 1200 llamadas, 4,2 ms solo de peaje;
los mismos píxeles en 8 imágenes grandes son 28 µs.

De ahí las dos costumbres del proyecto: montar el fondo en imágenes grandes
con `ImageBuffer` y volcarlo en pocas llamadas, y usar `RowRun`/`FillRow` en
vez de `Plot` y `ReadPixel` cuando hay que mirar o pintar muchos píxeles.

---

# 5. Teclado y ratón

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `KeyDown(codigo)` | 1 si esa tecla está pulsada ahora | `N` | 48 |
| `KeyHit(codigo)` | 1 si se pulsó desde la última vez | `N` | 53 |
| `KeyBank(n)` | 64 teclas de golpe en un entero | `N` | 273 |
| `GetKey()` | siguiente código de la cola, o 0 | — | 54 |
| `ReadChar()` | siguiente **letra** escrita, o 0 | — | 12 |
| `MouseX()`, `MouseY()` | posición en la ventana | — | 34 |
| `MouseDown()` | botones pulsados, sumados | — | 34 |
| `MouseHit(boton)` | un clic, una sola vez | `N` | 56 |

**Los códigos de tecla son los del driver, estilo evdev, no los de
BlitzPlus**: 1 Esc, 28 Enter, 57 Espacio, 103 Arriba, 105 Izquierda, 106
Derecha, 108 Abajo.

`KeyBank(n)` existe por el peaje: preguntar por 64 teclas con `KeyDown` son 64
llamadas y 224 µs. Con `KeyBank` es una.

**`GetKey` y `ReadChar` no son lo mismo.** `GetKey` da el código de la tecla,
el del driver; `ReadChar` da la **letra ya traducida**, con las mayúsculas
resueltas, lista para pegar a una cadena con `Chr$`. Para moverse por un juego,
`KeyDown`; para escribir texto, `ReadChar`.

Lo que `ReadChar` entrega es **un byte**. La `ñ`, la `ç` y los ordinales `ª`
`º` salen con su código de Latin-1 (241, 231, 170, 186), que es lo que dibuja
la fuente del sistema. Y hay dos cosas que no da:

- **Las vocales acentuadas no se pueden teclear.** Las teclas de acento
  (`´ ¨ \` ^`) son muertas de verdad: esperan a la letra siguiente para
  combinarse, y eso necesita recordar la pulsación anterior. Devuelven 0.
- **`¡` y `¿` dan `!` y `?`**, porque no están en ASCII.

`ReadChar` **no espera**: devuelve 0 si no hay nada escrito. Por eso se puede
llamar desde el bucle principal de un programa con ventana sin congelar nada.
Es lo que permite que un programa lleve su propio texto en vez de depender de
una caja de texto del sistema, que guarda 200 líneas de 128 letras y descarta
lo que pase de ahí:

```basic
c = ReadChar()
If c = 8 Then                      ; retroceso
  linea$ = Left$(linea$, Len(linea$) - 1)
Else
  If c > 0 Then linea$ = linea$ + Chr$(c)
EndIf
```

```basic
Graphics 320, 240
Repeat
  Cls
  Color 255, 255, 255
  Text 10, 10, "Raton: " + Str$(MouseX()) + "," + Str$(MouseY())
  If KeyDown(103) Then Text 10, 30, "arriba"
  Pump()
Until KeyHit(1)
```

---

Continúa en [REFERENCIA_NEMO_BASIC_3.md](REFERENCIA_NEMO_BASIC_3.md):
controles, menús, árbol, eventos, archivos, matemáticas, cadenas, GPIO, sonido
y red.
