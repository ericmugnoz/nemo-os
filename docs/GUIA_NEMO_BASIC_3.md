# Guía de Nemo Basic (3)

*Imágenes y ventanas con controles.*

Viene de [GUIA_NEMO_BASIC_2.md](GUIA_NEMO_BASIC_2.md).

---

### 4.5 Imágenes

| | Qué hace | Syscall |
|---|---|---|
| `img = LoadImage(nombre$)` | carga una imagen `.nimg` (−1 si no la encuentra) | 49 |
| `img = CreateImage(ancho, alto)` | imagen vacía | 52 |
| `DrawImage img, x, y` | dibuja la imagen | 50 |
| `ImageWidth(img)`, `ImageHeight(img)` | tamaño | 51 |
| `MaskImage img, color` | ese color pasa a ser transparente | 92 |
| `HandleImage img, x, y` | punto de la imagen que cae en las coordenadas de `DrawImage` | 89 |
| `copia = CopyImage(img)` | duplica una imagen | 93 |
| `SaveImage img, nombre$` | la guarda en disco | 94 |
| `FreeImage img` | la libera | 88 |
| `img = LoadAnimImage(nombre$, ancho, alto, primera, cuantas)` | carga una **hoja de celdas iguales** | 99 |
| `DrawImage img, x, y, fotograma` | dibuja una celda de la hoja | 50 |
| `DrawBlock img, x, y [, fotograma]` | dibuja **opaco**: ignora alfa y máscara | 50 |

Truco de sprites: `MaskImage img, 0xFF00FF` (el magenta, transparente) y `HandleImage img, ImageWidth(img) / 2, ImageHeight(img) / 2` (el centro como punto de agarre).

**Hojas de celdas.** Una hoja es una imagen con los fotogramas en celdas
iguales, en fila. Se carga entera con un solo identificador, y se dibuja la
celda que toque:

```basic
; 32 celdas de 16x16 en una sola imagen
tiles = LoadAnimImage("tiles.nimg", 16, 16, 0, 32)
DrawBlock tiles, x, y, 7          ; la celda 7
```

Importa porque los huecos de imagen son **64 en todo el sistema**: con un
tile por imagen se acaban enseguida. `primera` desplaza el origen, así que
con `primera = 3` el fotograma 0 es la celda 3 — para un tileset, `primera`
a 0 y `cuantas` al total.

**`DrawBlock` frente a `DrawImage`.** `DrawBlock` copia sin mirar el alfa ni
la máscara. Para un fondo es lo que quieres: más rápido y sin mezclar con lo
que hubiera debajo. Para un sprite con partes transparentes, `DrawImage`.

**Dibujar dentro de una imagen.** `SetBuffer ImageBuffer(img)` manda todo el
dibujo a esa imagen en vez de a la ventana, y `SetBuffer BackBuffer()`
vuelve. Lo respetan todos los comandos de dibujo, incluidos `DrawImage` y
`DrawBlock`. Sirve para montar un fondo grande una vez y luego dibujarlo de
una sola llamada:

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

Dentro de un `ImageBuffer`, `Cls` borra **la imagen**, no la ventana.

**Los límites, que deciden el diseño**:

| | |
|---|---|
| imágenes a la vez | 64 |
| tamaño máximo | 1024 x 1024 |
| entre todas | 24 MB |

Un `.nimg` no va comprimido, así que una imagen de 1024x1024 pesa 4 MB en
disco. Si no caben, `LoadImage` y `CreateImage` devuelven −1: el programa se
entera.

**Cuánto cuesta dibujar** (medido en Raspberry Pi 4):

| | |
|---|---|
| una llamada al sistema, dibuje lo que dibuje | 3,5 µs |
| un píxel | 4 ns |

Un tile de 16x16 son 256 píxeles: un microsegundo de píxeles contra 3,5 de
peaje. **Dibujar tile a tile es pagar peajes, no dibujar.** Una pantalla de
640x480 con tiles de 16 son 1200 llamadas, 4,2 ms solo de peaje. Los mismos
píxeles en 8 imágenes grandes son 28 µs. De ahí la costumbre: montar el
fondo en imágenes grandes con `ImageBuffer` y dibujarlo en pocas llamadas.

#### Trabajar con filas de píxeles

La misma cuenta de arriba explica estos cinco comandos. Mirar o pintar
píxeles de uno en uno con `Plot` y `ReadPixel` cuesta 3,5 µs por píxel, casi
900 veces más peaje que trabajo. Estos cinco hacen el recorrido **dentro del
kernel**, a 4 ns el píxel, y son una sola llamada.

Trabajan sobre **imágenes**, no sobre la ventana: para la ventana ya están
`DrawBlock` y compañía.

| | Qué hace | Syscall |
|---|---|---|
| `FillRow img, x, y, cuantos, color` | pinta una fila de un solo color | 291 |
| `RowRun(img, x, y, maximo, color)` | cuántos píxeles seguidos **son** de ese color | 292 |
| `RowSkip(img, x, y, maximo, color)` | cuántos seguidos **no** lo son | 292 |
| `ReadRow img, x, y, cuantos, array` | trae la fila a un array de enteros | 289 |
| `WriteRow img, x, y, cuantos, array` | lleva el array a la fila | 290 |

Con `maximo` negativo, `RowRun` y `RowSkip` avanzan hacia la **izquierda**.
El píxel de partida cuenta si cumple. Todos se paran en el borde de la
imagen, y devuelven cuántos píxeles han tratado de verdad, o **−1** si los
argumentos no valen (una imagen que no existe, por ejemplo). Un 0 es «no
quedaba nada dentro de la imagen»; no es lo mismo que un error.

**`RowRun` es la pieza para colisiones y rellenos.** Medir hasta dónde llega
una pared en una máscara es una llamada en vez de una por píxel:

```basic
mascara = LoadImage("JUEGOS/NEMO/solido.nimg")
; ¿cuántos píxeles de suelo seguidos hay bajo el pie de Nemo?
; el fondo es negro, así que "lo que NO es negro" es suelo
suelo = RowSkip(mascara, px, py + 16, 64, $000000)
If suelo = 0 Then
; no hay nada debajo: a caer
End If
```

Y un relleno por inundación pasa de costar por píxel a costar por tramo: los
dos extremos de un tramo son dos llamadas a `RowRun`, y pintarlo una a
`FillRow`. En el Pintor, en Lua, esa reescritura bajó un lienzo de 1024x1024
de 4.193.279 llamadas a 7.166 — de unos 14,7 s a unos 25 ms.

**Dos cosas de `ReadRow` y `WriteRow` que conviene tener claras.**

El quinto argumento es un **array declarado con `Dim`**, no una expresión. Si
pones un número o una cuenta ahí, el compilador lo dice y no genera nada:

```basic
Console
Dim fila(320)
img = CreateImage(320, 240)
ReadRow img, 0, 100, 320, fila
Print fila(0)
```

Si en vez del array pones un número o una cuenta —`ReadRow img, 0, 100, 320, 42`—
el compilador lo dice y no genera nada.

Y **`cuantos` se acota solo al tamaño del array**. Un `ReadRow` de 500 sobre
un `Dim fila(10)` llena los diez y para; no escribe los otros 490 por encima
del array. Eso no es cortesía: el kernel comprueba que el búfer sea de tu
programa, no que quepa en el array, así que sin ese tope se estarían
machacando las demás variables y la comprobación pasaría igual.

Aquí sí merece la pena traerse los píxeles al programa, al contrario que en
Lua: Nemo Basic compila a ARM64 nativo, así que recorrer el array cuesta
nanosegundos por elemento. En Lua cada lectura del búfer cuesta unos 210 ns,
del mismo orden que la propia llamada al sistema, y por eso allí se usa
`RowRun` y no `ReadRow`.

### 4.6 Ventanas con controles (gadgets)

| | Qué hace | Syscall |
|---|---|---|
| `CreateWindow titulo$, x, y, ancho, alto` | ventana en modo evento | 40 |
| `b = CreateButton(texto$, x, y, ancho, alto)` | botón | 100 |
| `CreatePanel(x, y, ancho, alto)` | panel | 101 |
| `CreateTextField(x, y, ancho, alto)` | campo de texto | 102 |
| `CreateListBox(x, y, ancho, alto)` | lista | 103 |
| `CreateLabel(texto$, x, y, ancho, alto)` | etiqueta | 160 |
| `SetGadgetText id, texto$` | cambia el texto | 105 |
| `t$ = GadgetText$(id)` | lee el texto | 106 |
| `FreeGadget id` | elimina el control | 104 |
| `AddGadgetItem id, texto$` | añade una línea a una lista | 114 |
| `ClearGadgetItems id` | vacía la lista | 115 |
| `SelectedGadgetItem(id)` | línea elegida (−1 si ninguna) | 116 |
| `SelectGadgetItem id, n` | elige una línea | 117 |
| `CountGadgetItems(id)` | cuántas líneas hay | 118 |
| `c = CreateCheckBox(texto$, x, y, ancho, alto)` | casilla de marcar | 100 |
| `r = CreateRadio(texto$, x, y, ancho, alto)` | botón de opción: al marcar uno, se desmarcan los demás | 100 |
| `ButtonState(id)` / `SetButtonState id, 1` | si una casilla u opción está marcada / marcarla | 141 / 142 |
| `s = CreateSlider(x, y, ancho, alto)` | deslizador (con un quinto argumento `1`, vertical) | 163 |
| `SetSliderRange id, visible, total` / `SetSliderValue id, v` / `SliderValue(id)` | su recorrido (de 0 a total − visible: para ir de 0 a 100, `SetSliderRange id, 1, 101`), ponerlo en un valor, leerlo | 164 / 165 / 166 |
| `p = CreateProgBar(x, y, ancho, alto)` / `UpdateProgBar id, valor#` | barra de progreso, de 0,0 a 1,0 | 161 / 162 |
| `k = CreateComboBox(x, y, ancho, alto)` | lista desplegable (sus líneas, con `AddGadgetItem`) | 167 |
| `t = CreateTabber(x, y, ancho, alto)` | fila de pestañas (sus nombres, con `AddGadgetItem`) | 168 |
| `a = CreateTextArea(x, y, ancho, alto)` | caja de texto de varias líneas | 125 |
| `SetTextAreaText id, t$` / `AddTextAreaText id, t$` | poner o añadir texto en ella | 126 / 145 |

**Decimales donde se espera un entero**: se convierten truncando, como `Int`. Vale para las coordenadas (`DrawImage bola, x# - 8, y# - 8`), los índices de array y los argumentos de cualquier orden (`Mid$(t$, i#, 2)`). Antes se pasaban los bits del decimal y el resultado era absurdo, sin aviso.

**Comparar decimales**: `<`, `>`, `<=`, `>=`, `=` y `<>` funcionan con decimales, con literales (`d# < 0.01`), con negativos y mezclando entero y decimal. Antes las comparaciones pasaban siempre por enteros y daban resultados falsos sin avisar.

**La caja de texto se escribe a mano**: al hacer clic en ella recibe el foco (su borde de arriba se pone amarillo) y admite letras, Intro, Retroceso, flechas, Inicio, Fin, RePág y AvPág, con cursor y desplazamiento automático. `GadgetText$(caja)` devuelve **todo** su contenido, con las líneas separadas por `Chr$(10)`; antes devolvía una cadena vacía. Caben 200 líneas de 127 caracteres, y hasta cuatro cajas grandes a la vez.

| `InsertGadgetItem id, n, t$` / `ModifyGadgetItem id, n, t$` / `RemoveGadgetItem id, n` | insertar, cambiar o quitar la línea `n` | 157 / 159 / 158 |
| `HideGadget id` / `ShowGadget id` | ocultar o mostrar un control | 110 |
| `DisableGadget id` / `EnableGadget id` | desactivarlo (gris, no responde) o activarlo | 111 |
| `ActivateGadget id` | darle el teclado (a un campo de texto) | 112 |

```basic
; Los controles, juntos
CreateWindow "Controles", 100, 100, 420, 330
avisos = CreateCheckBox("Quiero avisos", 20, 20, 160, 20)
rojo = CreateRadio("Rojo", 20, 50, 80, 20)
azul = CreateRadio("Azul", 110, 50, 80, 20)
SetButtonState rojo, 1
volumen = CreateSlider(20, 85, 200, 20)
SetSliderRange volumen, 1, 101
SetSliderValue volumen, 50
barra = CreateProgBar(20, 115, 200, 16)
UpdateProgBar barra, 0.5
lista = CreateComboBox(240, 20, 160, 22)
AddGadgetItem lista, "Uno"
AddGadgetItem lista, "Dos"
notas = CreateTextArea(20, 145, 380, 120)
SetTextAreaText notas, "Prueba los controles."
Repeat
  e = WaitEvent()
  If e = EVENT_GADGETACTION
    fuente = EventSource()
    If fuente = volumen
      UpdateProgBar barra, SliderValue(volumen) / 100.0
    EndIf
    If fuente = avisos
      If ButtonState(avisos)
        AddTextAreaText notas, " Avisos: si."
      Else
        AddTextAreaText notas, " Avisos: no."
      EndIf
    EndIf
    If fuente = rojo Then AddTextAreaText notas, " Color: rojo."
    If fuente = azul Then AddTextAreaText notas, " Color: azul."
    If fuente = lista
      n = SelectedGadgetItem(lista)
      AddTextAreaText notas, " Elegido el " + Str$(n + 1) + "."
    EndIf
  EndIf
Until e = EVENT_WINDOWCLOSE
End
```

**`CreateWindow` no es `Graphics`**: la X de cerrar no destruye la ventana, sino que envía un evento (el 2051), y tu programa decide cuándo terminar.

**Los botones de la barra de título.** `WindowButtons maximizar, minimizar, cerrar` decide cuáles se ven: 1 se ve, 0 no. Un reloj no necesita maximizar (`WindowButtons 0, 1, 1`); un juego a pantalla completa puede quitarlos todos. **Si quitas el de cerrar, tu programa tiene que ofrecer su propia salida**, como la tecla Esc; si se quedara colgado, se cierra desde el gestor de tareas.

```basic
; Un juego sin botones en la barra de titulo: se sale con Esc
Graphics 640, 480
WindowButtons 0, 0, 0
x = 0
While KeyHit(1) = 0          ; 1 = Esc
  Cls
  Color 255, 200, 0
  Oval x, 200, 40, 40
  Color 255, 255, 255
  Text 10, 10, "Pulsa Esc para salir"
  x = (x + 4) Mod 600
  Delay 16
Wend
End
```

---

Continúa en [GUIA_NEMO_BASIC_4.md](GUIA_NEMO_BASIC_4.md): menús, árbol, eventos, archivos, matemáticas, cadenas y GPIO.
