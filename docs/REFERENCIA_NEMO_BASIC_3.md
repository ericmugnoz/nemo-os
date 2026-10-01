# Referencia de Nemo Basic (3)

*Controles, menús, árbol, eventos, archivos, matemáticas, cadenas, GPIO, sonido y red.*

Viene de [REFERENCIA_NEMO_BASIC_2.md](REFERENCIA_NEMO_BASIC_2.md).
# 6. Ventanas con controles

## 6.1 Crear controles

Todos devuelven un identificador, o −1.

| | Firma | Syscall |
|---|---|---|
| `CreateButton(texto$, x, y, ancho, alto)` | `SNNNN` | 100 |
| `CreateCheckBox(texto$, x, y, ancho, alto)` | `SNNNN` | 100 |
| `CreateRadio(texto$, x, y, ancho, alto)` | `SNNNN` | 100 |
| `CreateLabel(texto$, x, y, ancho, alto)` | `SNNNN` | 160 |
| `CreatePanel(x, y, ancho, alto)` | `NNNN` | 101 |
| `CreateTextField(x, y, ancho, alto)` | `NNNN` | 102 |
| `CreateTextArea(x, y, ancho, alto)` | `NNNN` | 125 |
| `CreateListBox(x, y, ancho, alto)` | `NNNN` | 103 |
| `CreateComboBox(x, y, ancho, alto)` | `NNNN` | 167 |
| `CreateTabber(x, y, ancho, alto)` | `NNNN` | 168 |
| `CreateProgBar(x, y, ancho, alto)` | `NNNN` | 161 |
| `CreateSlider(x, y, ancho, alto [, vertical])` | `NNNNN` | 163 |
| `CreateScrollBar(x, y, ancho, alto [, vertical])` | `NNNNN` | 314 |
| `CreateCanvas(x, y, ancho, alto)` | `NNNN` | 188 |
| `CreateTreeView(x, y, ancho, alto)` | `NNNN` | 175 |

El quinto argumento de `CreateSlider` y `CreateScrollBar` es `2` para
vertical; sin él, horizontal.

**El deslizador y la barra de desplazamiento comparten los comandos**
`SetSliderRange`, `SetSliderValue` y `SliderValue`: por dentro son el mismo
recorrido y el mismo valor. La diferencia está en cómo se usan:

- El **deslizador** es un control de ajuste —volumen, brillo—. Pulsar en
  cualquier punto lleva el pomo ahí.
- La **barra** es para recorrer algo más largo que la ventana. Trae flecha en
  cada extremo (avanza de uno en uno), salto de página al pulsar el carril, y
  al agarrar el pomo **no salta**: se queda por donde lo cogiste.

```basic
lista = CreateScrollBar(300, 10, 16, 200, 2)
SetSliderRange lista, 20, 500      ; se ven 20 renglones de 500
```

`SetSliderRange` toma cuánto se ve y cuánto hay en total, y de ahí sale el
tamaño del pomo. `SliderValue` devuelve el primer renglón visible, de 0 a
`total − visible`.

## 6.2 Texto, listas y estado

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `SetGadgetText id, texto$` | cambia su texto | `NS` | 105 |
| `GadgetText$(id)` | lee su texto | `N` | 106 |
| `AddGadgetItem id, texto$` | añade una línea a una lista | `NS` | 114 |
| `InsertGadgetItem id, n, texto$` | la inserta en la posición n | `NNS` | 157 |
| `ModifyGadgetItem id, n, texto$` | cambia la línea n | `NNS` | 159 |
| `RemoveGadgetItem id, n` | quita la línea n | `NN` | 158 |
| `ClearGadgetItems id` | vacía la lista | `N` | 115 |
| `CountGadgetItems(id)` | cuántas líneas tiene | `N` | 118 |
| `SelectedGadgetItem(id)` | cuál está elegida, o −1 | `N` | 116 |
| `SelectGadgetItem id, n` | elige la línea n | `NN` | 117 |
| `SetTextAreaText id, texto$` | pone el texto de un área | `NS` | 126 |
| `AddTextAreaText id, texto$` | añade al final | `NS` | 145 |
| `ButtonState(id)` | 1 o 0: casilla u opción marcada | `N` | 141 |
| `SetButtonState id, valor` | la marca o la desmarca | `NN` | 142 |
| `SliderValue(id)` | valor del deslizador | `N` | 166 |
| `SetSliderValue id, v` | lo mueve | `NN` | 165 |
| `SetSliderRange id, visible, total` | su recorrido | `NNN` | 164 |
| `UpdateProgBar id, milesimas` | avanza la barra (el kernel cuenta por mil) | `NN` | 162 |
| `SetPanelColor id, r, g, b` | color de un panel | `NNNN` | 207 |
| `SetPanelImage id, archivo$` | imagen de fondo de un panel | `NS` | 222 |
| `SetGadgetGroup id, grupo` | mete el control en un grupo de opciones | `NN` | 223 |
| `GadgetGroup(id)` | en qué grupo está, o 0 | `N` | 224 |

**Los grupos son para las opciones (`CreateRadio`).** Las opciones del mismo
grupo se apagan entre ellas: marcar una desmarca las demás. Sin grupo, dos
opciones de la misma ventana se quedan las dos encendidas y no hay forma de
saber cuál manda. El número de grupo lo eliges tú; vale cualquiera menos el 0,
que significa «sin grupo».

```basic
facil = CreateRadio("Facil", 10, 10, 100, 20)
normal = CreateRadio("Normal", 10, 35, 100, 20)
dificil = CreateRadio("Dificil", 10, 60, 100, 20)
SetGadgetGroup facil, 1
SetGadgetGroup normal, 1
SetGadgetGroup dificil, 1
SetButtonState normal, 1
```

## 6.3 Mostrar, mover y medir

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `ShowGadget id`, `HideGadget id` | lo muestra u oculta | `N` | 110 |
| `EnableGadget id`, `DisableGadget id` | lo activa o desactiva | `N` | 111 |
| `ActivateGadget id` | le da el foco | `N` | 112 |
| `FreeGadget id` | lo destruye | `N` | 104 |
| `SetGadgetShape id, x, y, ancho, alto` | lo coloca y lo redimensiona | `NNNNN` | 108, 109 |
| `GadgetX(id)`, `GadgetY(id)`, `GadgetWidth(id)`, `GadgetHeight(id)` | dónde y cuánto mide | `N` | 107 |
| `WindowButtons maximizar, minimizar, cerrar` | 1 se ve, 0 oculto | `NNN` | 271 |

```basic
CreateWindow "Ejemplo", 40, 40, 300, 160
b = CreateButton("Pulsa", 20, 20, 100, 28)
l = CreateListBox(20, 60, 260, 70)
AddGadgetItem l, "primera"
AddGadgetItem l, "segunda"
SetGadgetText b, "Ya"
Repeat
  ev = WaitEvent()
Until ev = 0
```

## 6.4 Menús

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `WindowMenu()` | la barra de menús de la ventana | — | 120 |
| `CreateMenu(texto$, etiqueta, padre)` | una entrada; texto `""` es un separador | `SNN` | 121 |
| `CheckMenu id`, `UncheckMenu id` | marca de verificación | `N` | 122 |
| `EnableMenu id`, `DisableMenu id` | activa o desactiva | `N` | 123 |
| `UpdateWindowMenu id` | **no hace nada** | `N` | — |

**`CreateMenu` tiene el padre en el TERCER argumento.** Llamarla al revés no da
error: el kernel recibe un número donde espera un puntero a texto, devuelve −1,
y simplemente no aparece ninguna barra de menús.

**`UpdateWindowMenu` no hace nada** y está así a propósito: en BlitzPlus hacía
falta tras crear los menús, aquí el kernel los redibuja solo, y se acepta para
que el código de BlitzPlus compile tal cual.

La etiqueta que le das a `CreateMenu` es el número que devuelve `EventData()`
cuando el usuario elige esa entrada, con el evento `$1001`.

### Menús flotantes

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `CreateContextMenu()` | un menú flotante vacío | — | 315 |
| `ShowContextMenu m, x, y` | lo abre en ese punto | `NNN` | 316 |

Se crea **una vez**, al arrancar, y se abre tantas veces como haga falta. Las
entradas se le cuelgan con el mismo `CreateMenu` de arriba, pasándole el menú
flotante como padre, y mandan el mismo evento `$1001` con su etiqueta en
`EventData()`. Sirven también las marcas, los separadores y `EnableMenu`.

```basic
Global menu
menu = CreateContextMenu()
CreateMenu "Copiar", 1, menu
CreateMenu "Pegar", 2, menu
CreateMenu "", 0, menu             ; separador
CreateMenu "Borrar", 3, menu

; cuando toque abrirlo, donde se pulsó
ShowContextMenu menu, EventX(), EventY()
```

Las coordenadas son las del **área de contenido**, las mismas que usan los
controles. El menú se abre donde se le dice: no se recorta contra el borde de
la ventana, así que cerca del borde derecho conviene restarle su ancho.

Se cierra solo al elegir una entrada o al pulsar fuera, y ese clic de fuera
**sigue su camino**: si caía sobre un botón, el botón se pulsa. No hay que dar
dos clics.

Puedes tener varios menús flotantes creados —uno para la lista, otro para el
lienzo— pero solo uno abierto a la vez; abrir uno cierra el que hubiera,
incluido un desplegable de la barra.

## 6.5 Barra de herramientas

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `CreateToolBar(archivo$, x, y, ancho, alto)` | barra desde una tira de iconos | `SNNNN` | 172 |
| `SetToolBarTips id, textos$` | las ayudas, separadas por comas | `NS` | 174 |
| `EnableToolBarItem id, n`, `DisableToolBarItem id, n` | activa o desactiva un botón | `NN` | 173 |

## 6.6 Árbol

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `TreeViewRoot(id)` | el nodo raíz | `N` | 176 |
| `AddTreeViewNode(texto$, padre)` | añade un hijo | `SN` | 177 |
| `InsertTreeViewNode(padre, texto$, n)` | lo inserta en la posición n | `NSN` | 178 |
| `ModifyTreeViewNode nodo, texto$` | cambia su texto | `NS` | 179 |
| `FreeTreeViewNode nodo` | lo quita | `N` | 180 |
| `ExpandTreeViewNode nodo`, `CollapseTreeViewNode nodo` | lo abre o lo cierra | `N` | 181 |
| `CountTreeViewNodes(nodo)` | cuántos hijos tiene | `N` | 182 |
| `SelectedTreeViewNode(id)` | cuál está elegido | `N` | 183 |
| `SelectTreeViewNode nodo` | lo elige | `N` | 184 |

---

# 7. Eventos

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `WaitEvent()` | espera hasta que pase algo (y cede el turno) | — | 8, 14 |
| `PollEvent()` | mira si hay algo, sin esperar | — | 8 |
| `EventSource()` | qué control lo produjo | — | 9 |
| `EventData()` | el dato del evento | — | 9 |
| `EventX()`, `EventY()` | coordenadas del evento | — | 256 |

Los eventos son constantes del sistema: `$401` (1025) un control accionado,
`$1001` (4097) una entrada de menú, `$4001` (16385) un temporizador, `$803`
(2051) cerrar la ventana.

**Un menú no llega por el evento de control**: llega como evento de menú, con
la etiqueta en la fuente del evento.

`WaitEvent()` cede el turno mientras espera, así que un bucle construido sobre
él no necesita `Pump()`. Uno construido sobre `PollEvent()` sí.

```basic
CreateWindow "Eventos", 40, 40, 260, 120
b = CreateButton("Salir", 20, 20, 100, 28)
Repeat
  ev = WaitEvent()
  If ev = $401 And EventSource() = b Then Exit
Until ev = $803
```

---

# 8. Archivos y carpetas

## 8.1 Leer y escribir por líneas

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `OpenFile(nombre$)` | abre para leer líneas, o −1 | `S` | 41 |
| `ReadLine$(h)` | la siguiente línea | `N` | 42 |
| `Eof(h)` | 1 si se acabó | `N` | 43 |
| `WriteLine h, texto$` | escribe una línea | `NS` | 72 |
| `CloseFile h` | cierra | `N` | 44 |
| `WriteFile nombre$, texto$` | crea el archivo con ese contenido | `SS` | 20, 22 |

**Cerrar importa de verdad.** Hay **32 huecos** de archivo, y son de **todo el
sistema**, no 32 por programa: uno que abra y no cierre deja sin huecos también
a los demás, y además retiene la memoria de lo que abrió. Se recupera solo
cuando ese programa termina. El explorador agotaba los ocho que había antes a
la novena copia por esto.

**El tamaño ya no es problema.** `OpenFile` carga el archivo entero, pero pide
un buffer **del tamaño del archivo**: hasta 8 MB por archivo y 24 MB entre
todos los abiertos a la vez. Antes eran 16383 bytes fijos y lo que pasaba de
ahí se abría truncado y sin avisar.

## 8.2 Posición y tamaño

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `SeekFile h, pos` | coloca el cursor | `NN` | 74 |
| `FilePos(h)` | dónde está el cursor | `N` | 73 |
| `FileLength(h)` | tamaño del archivo abierto | `N` | 75 |
| `FileSize(nombre$)` | tamaño, sin abrirlo | `S` | 81 |

## 8.3 Carpetas y existencia

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `FileExists(nombre$)` | 1 si está | `S` | 82 |
| `FileType(nombre$)` | 1 archivo, 2 carpeta, 0 nada | `S` | 82 |
| `DeleteFile nombre$` | lo borra | `S` | 84 |
| `RenameFile(viejo$, nuevo$)` | lo renombra; 1 si pudo, 0 si no | `SS` | 312 |
| `CreateDir nombre$` | crea una carpeta | `S` | 24 |
| `ReadDir(ruta$)` | abre una carpeta para listarla | `S` | 83 |
| `NextFile$(h)` | siguiente nombre, o `""` | `N` | 79 |
| `CloseDir h` | cierra el listado | `N` | 80 |

Las rutas con carpetas funcionan con `/` y con `\` en **todos** los comandos
de esta sección: leer, escribir, crear carpetas, borrar, preguntar el tamaño o
el tipo, y listar. No hay `..` ni `.`, que no tienen
sentido sin carpeta actual. Si la carpeta de la ruta no existe, la llamada
falla: ni se crea el archivo suelto en la raíz ni se borra otro que se llame
igual en otro sitio. Un nombre **sin** separador se busca donde siempre (la
raíz y DOCUMENTOS), y `WriteFile` y `CreateDir` lo crean en la raíz.

```basic
Console
WriteFile "prueba.txt", "una linea"
Print FileExists("prueba.txt")
Print FileSize("prueba.txt")
h = OpenFile("prueba.txt")
Print ReadLine$(h)
CloseFile h
DeleteFile "prueba.txt"
Print FileExists("prueba.txt")
```

**`RenameFile` cambia el nombre, no mueve de carpeta.** El nombre nuevo va sin
separadores; con una barra dentro devuelve 0 y no toca nada. Devuelve 0
también si el archivo no existe, si ya hay otro con ese nombre —no machaca
nada a la callada— o si está en la partición FAT, donde el sistema de archivos
todavía no sabe renombrar. El nombre viejo sí admite ruta, y entonces se
renombra ahí y solo ahí.

```basic
If RenameFile("borrador.txt", "definitivo.txt") = 0 Then
  Print "no se pudo renombrar"
EndIf
```

---

# 9. Matemáticas

Ninguna llama al sistema: se calculan dentro del programa. Las que llevan `#`
trabajan en decimal; las de enteros son unas pocas instrucciones del propio
procesador, sin ramas.

| | Qué hace | Firma |
|---|---|---|
| `Abs(n)`, `Abs#(n)`, `Abs%(n)` | valor absoluto | `N` |
| `Sgn(n)` | −1, 0 o 1 | `N` |
| `Min(a, b)`, `Max(a, b)` | el menor, el mayor | `NN` |
| `Sqr(n)`, `Sqr#(n)` | raíz cuadrada | `N` |
| `Sin(n)`, `Cos(n)`, `Tan(n)`, `ATan(n)` y sus `#` | trigonometría, en **grados** | `N` |
| `Exp(n)`, `Log(n)` y sus `#` | exponencial y logaritmo | `N` |
| `Floor(n)`, `Ceil(n)` y sus `#` | redondeo abajo y arriba | `N` |
| `Int(n)`, `Int%(n)` | decimal a entero, truncando | `N` |
| `Float(n)`, `Float#(n)` | entero a decimal | `N` |
| `Rnd(n)` | entero al azar, de 0 a `n−1` | `N` |
| `Rand(a, b)` | entero al azar, de `a` a `b`, los dos incluidos | `NN` |
| `Seed(n)` | fija la semilla | `N` |

```basic
Console
Print Abs(-3)
Print Min(2, 5)
Print Max(2, 5)
Print Sgn(-9)
Print Int(2.9)
```

Salida: `3`, `2`, `5`, `-1`, `2`.

**`Sqr`, `Floor` y `Ceil` devuelven decimal** incluso con entrada entera:
`Sqr(16)` sale `4.000000`. Si quieres el entero, `Int(Sqr(16))`.

**`Rnd` y `Rand` no cuentan igual.** `Rnd(6)` da de 0 a 5, y `Rand(1, 6)` da de
1 a 6: un dado es lo segundo. `Rnd()` sin argumento devuelve un decimal entre 0
y 1. Con `b` menor que `a`, `Rand` devuelve `a`.

Las trigonométricas y `Exp`/`Log` viven en el bloque de runtime, que el
compilador incrusta solo si el programa lo necesita.

---

# 10. Cadenas

Tampoco llaman al sistema.

| | Qué hace | Firma |
|---|---|---|
| `Len(s$)` | cuántos caracteres | `S` |
| `Left$(s$, n)`, `Right$(s$, n)` | los n primeros, los n últimos | `SN` |
| `Mid$(s$, desde [, cuantos])` | un trozo, contando desde 1 | `SNN` |
| `Upper$(s$)`, `Lower$(s$)` | mayúsculas, minúsculas | `S` |
| `Trim$(s$)` | quita espacios de los dos lados | `S` |
| `Replace$(s$, busca$, pon$)` | sustituye | `SSS` |
| `Instr(s$, busca$ [, desde])` | posición, o 0 | `SSN` |
| `Str$(n)` | número a cadena | `N` |
| `Val(s$)` | cadena a **entero**: `Val("21.5")` da 21 | `S` |
| `Val#(s$)` | cadena a **decimal**: `Val#("21.5")` da 21.5 | `S` |
| `Chr$(n)`, `Asc(s$)` | código a carácter y al revés | `N`, `S` |
| `Hex$(n)`, `Bin$(n)` | en hexadecimal, en binario | `N` |
| `String$(s$, n)` | repite la cadena n veces | `SN` |
| `LSet$(s$, n)`, `RSet$(s$, n)` | rellena a n caracteres, a un lado o al otro | `SN` |

```basic
Console
Print Len("hola")
Print Left$("hola", 2) + Right$("hola", 2)
Print Mid$("abcdef", 3, 2)
Print Upper$("ab") + Lower$("CD")
Print Instr("hola", "la")
Print Replace$("aaa", "a", "b")
Print Hex$(255)
Print Bin$(5)
Print String$("ab", 2)
Print Chr$(66) + Str$(Asc("A"))
```

Salida: `4`, `hola`, `cd`, `ABcd`, `3`, `bbb`, `FF`, `101`, `abab`, `B65`.

**`Mid$` cuenta desde 1**, no desde 0, al contrario que los arrays.

**Las cadenas temporales se liberan al terminar cada sentencia.** Lo que crea
una expresión (`"a" + b$`, `Mid$(...)`, `Str$(n)`) es texto nuevo; el
compilador lo apunta y lo suelta al acabar la sentencia. Sin eso, un bucle de
juego agotaba la memoria en un minuto y a partir de ahí `Mid$` devolvía vacío
y el programa hacía cosas absurdas.

Y **`text_width` cuenta caracteres, no bytes**: el sistema decodifica UTF-8,
así que `"año"` mide 3.

---

# 11. GPIO, I2C, SPI y sonido

Pines 2 a 27 del conector. Solo en la Raspberry Pi 4.

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `GpioMode pin, modo` | 0 entrada, 1 salida, 2 a positivo, 3 a masa | `NN` | 264 |
| `GpioWrite pin, valor` | pone el pin a 0 o a 1 | `NN` | 265 |
| `GpioRead(pin)` | lo lee: 0 o 1, o error negativo | `N` | 266 |
| `GpioPwm pin, hz, diezmilesimas` | PWM por hardware: pines 12, 13, 18, 19 | `NNN` | 267 |
| `I2cWrite direccion, datos$` | escribe en un dispositivo I2C | `NS` | 268 |
| `I2cRead$(direccion, cuantos)` | lee bytes de un dispositivo I2C | `NN` | 269 |
| `SpiTransfer$(datos$, chip, hz, modo)` | envía y recibe por SPI | `SNNN` | 270 |

El I2C está fijo a **100 kHz**.

```basic
Console
GpioMode 17, 1
GpioWrite 17, 1
Delay 10
GpioWrite 17, 0
GpioMode 27, 2
Print GpioRead(27)
```

## 11.4 Sonido

Por el jack de 3,5 mm. Solo en la Raspberry Pi 4. Los archivos son `.wav`:
PCM de 16 bits.

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `LoadSound(nombre$)` | carga un `.wav`, o −1 | `S` | 226 |
| `PlaySound sonido` | suena, **y espera a que acabe** | `N` | 228 |
| `SoundVolume sonido, v#` | volumen, de `0.0` a `1.0` | `NN` | 229 |
| `SoundPan sonido, p#` | de `-1.0` (izquierda) a `1.0` (derecha) | `NN` | 230 |
| `SoundPitch sonido, hercios` | frecuencia de muestreo | `NN` | 231 |
| `FreeSound sonido` | libera | `N` | 227 |

Dos cosas que hay que tener claras antes de usarlo:

**`PlaySound` espera.** Bloquea hasta que el sonido termina de sonar, así que
mientras suena el programa no avanza. El resto del sistema sí sigue vivo —
la pantalla se redibuja y el ratón responde —, pero el programa que llamó se
queda ahí. Un efecto corto va bien; una música de fondo, no.

**Un sonido a la vez.** Si un programa llama a `PlaySound` mientras ya está
sonando otro, el segundo se descarta en silencio. No hay mezcla todavía.

**El volumen, el pan y la frecuencia se ponen ANTES de `PlaySound`.** Se
guardan en el sonido, no en un canal: cuando `PlaySound` vuelve ya no hay
nada que ajustar, así que cambiarlos después no se oye.

```basic
Console
s = LoadSound("TONO.WAV")
If s < 0 Then
  Print "no se pudo cargar"
Else
  SoundVolume s, 0.75
  PlaySound s
  FreeSound s
EndIf
```

## 11.5 Datos del sistema

Qué máquina es ésta y en qué se le está yendo la memoria, el disco y los
turnos. Todos devuelven un número, salvo los dos que acaban en `$`.

Varias de estas syscalls devuelven **dos datos metidos en un solo número**, y
el compilador ya los separa: por eso hay `CpuCores()` y `CpuMHz()` en vez de
un `CpuInfo()` que haya que partir a mano.

**El equipo**

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `CpuName$()` | el nombre del procesador | — | 285 |
| `CpuCores()` | núcleos en marcha | — | 286 |
| `CpuMHz()` | frecuencia; **0 = no se pudo saber**, no "parado" | — | 286 |
| `TotalRam()` | RAM física de la placa, en bytes | — | 287 |
| `ScreenWidth()`, `ScreenHeight()` | la pantalla entera (no la ventana: eso es `ClientWidth()`) | — | 35 |

**La memoria**

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `KernelRam()` | lo que ocupa el núcleo cargado, en bytes | — | 288 |
| `TaskRamUsed()`, `TaskRamTotal()` | la reserva de memoria de programas, en bytes | — | 260 |
| `KernelHeapUsed()`, `KernelHeapTotal()` | el montón del núcleo, en bytes | — | 252 |

**El disco**

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `DiskTotalBlocks()`, `DiskUsedBlocks()` | NemoFS, en bloques de 512 bytes | — | 250 |
| `CardSectors()` | sectores físicos de la tarjeta | — | 279 |
| `PartitionType(i)` | tipo de la partición `i` (0 a 3) | `N` | 280 |
| `PartitionStart(i)` | su primer sector | `N` | 280 |
| `PartitionSectors(i)` | cuántos sectores ocupa; **0 = vacía** | `N` | 280 |

**Las tareas**

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `TaskCount()` | cuántas tareas hay vivas | — | 251 |
| `TaskSlots()` | cuántas caben como máximo | — | 251 |
| `TaskName$(i)` | el programa de la tarea `i` (0 a `TaskCount()-1`) | `N` | 295 |
| `TaskSlot(i)` | su hueco, que es lo que pide matarla | `N` | 294 |
| `TaskTurns(i)` | turnos del planificador que ha recibido | `N` | 294 |
| `TaskWindow(i)` | su ventana, o **−1** si no tiene | `N` | 294 |

**Ojo con `TaskTurns`**: son turnos del planificador, **no** tiempo de
procesador. Una tarea que sólo espera acumula turnos igual que una que
trabaja.

**El reloj**

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `Year()`, `Month()`, `Day()` | la fecha | — | 134 |
| `Hour()`, `Minute()`, `Second()` | la hora | — | 134 |

```basic
Console
Print CpuName$() + ", " + Str$(CpuCores()) + " nucleos"
Print "RAM: " + Str$(TotalRam() / 1048576) + " MB"
Print "Disco: " + Str$(DiskUsedBlocks() * 512 / 1048576) + " MB usados"
For i = 0 To TaskCount() - 1
  Print "[" + Str$(TaskSlot(i)) + "] " + TaskName$(i) + ": " + Str$(TaskTurns(i)) + " turnos"
Next
```

El ejemplo completo, con barras de ocupación y refresco cada segundo, está en
`monitor.nb`.

## 11.6 Leer un `TextArea` y medir texto

Para escribir un editor hacen falta dos cosas que no estaban: poder **leer de
vuelta** lo que el usuario ha escrito en un `TextArea` (antes sólo se podía
escribir en él, así que un programa podía abrir un archivo y no podía
guardarlo), y poder **medir** el texto para colocar un cursor.

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `TextAreaText$(id)` | todo el contenido, como cadena | `N` | 146, 149 |
| `TextAreaLen(id, unidades)` | longitud: `1` en caracteres, `2` en líneas | `NN` | 146 |
| `TextAreaLineLen(id, línea)` | longitud de esa línea | `NN` | 147 |
| `TextAreaLineOfChar(id, i)` | en qué línea cae ese carácter | `NN` | 148 |
| `TextWidth(texto$)` | lo que ocupa en píxeles, con la fuente activa | `S` | 254 |
| `FontHeight()` | alto de la fuente, en píxeles | — | 196 |
| `FontCharWidth()` | lo que avanza un carácter | — | 206 |

`TextAreaText$` pide el buffer **del tamaño exacto** que hace falta, así que
no corta los archivos grandes.

```basic
texto$ = TextAreaText$(editor)
WriteFile "MIPROG.nb", texto$
```

El ejemplo completo es `timonel.nb`, el entorno de programación.

## 11.7 Red

Nemo Basic habla con otras máquinas de dos formas, y cada una sirve para algo
distinto.

**UDP** manda mensajes sueltos a una máquina o a toda la red. No garantiza que
lleguen ni en qué orden, y eso es justo lo que lo hace bueno para un juego: una
posición perdida no importa porque la siguiente llega en milisegundos, mientras
que esperar a reenviarla daría un dato viejo. Caben **cuatro sockets** a la vez.

**HTTP** pide o manda datos a un servidor. Sí garantiza la entrega, y por eso
sirve para lo contrario: una lectura de un sensor que tiene que llegar. Solo
puede haber **una petición a la vez en todo el sistema**.

**Ninguno de estos comandos espera.** Arrancan la operación y vuelven en el
acto; después se consulta el estado. No es incomodidad gratuita: el
planificador de Nemo OS es cooperativo, así que un comando que esperase dentro
del sistema pararía la máquina entera y no solo el programa. Quien quiera
esperar lo hace con un bucle y `Delay`, que cede el turno. La biblioteca
`otros programas/red/red.nb` trae esas funciones ya escritas.

### La configuración de esta máquina

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `NetReady()` | 1 si hay red lista, 0 si no | — | 310 |
| `NetIp$()` | la dirección de esta máquina | — | 309 |
| `NetMask$()` | la máscara | — | 309 |
| `NetGateway$()` | la pasarela | — | 309 |
| `NetDns$()` | el servidor de nombres | — | 309 |

**`NetReady()` es la primera línea de cualquier programa de red.** Pedir un
socket o mandar algo mientras el sistema aún negocia su dirección no da ningún
error: simplemente nada llega a ninguna parte, que es la forma más confusa
posible de fallar.

La máscara, la pasarela y el DNS vienen a `"0.0.0.0"` cuando no hay router. No
es un error: con un cable directo entre dos máquinas no existen y no hacen
falta para hablar con la del otro extremo.

### UDP

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `UdpOpen(puerto)` | abre un socket, o negativo | `N` | 301 |
| `UdpSend(h, ip$, puerto, datos$)` | manda; devuelve los bytes, o negativo | `NSNS` | 303 |
| `UdpPending(h)` | cuántos mensajes esperan | `N` | 311 |
| `UdpPort(h)` | el puerto que tiene de verdad | `N` | 317 |
| `UdpRecv$(h)` | saca el siguiente | `N` | 304 |
| `UdpFrom$(h)` | de quién era el que acaba de sacar | `N` | 305 |
| `UdpFromPort(h)` | y su puerto | `N` | 305 |
| `UdpLost(h)` | mensajes tirados por cola llena | `N` | 306 |
| `UdpClose h` | cierra el socket | `N` | 302 |

`UdpOpen` con puerto **0** elige uno libre, que es lo que quiere quien solo va
a mandar y escuchar la contestación; `UdpPort(h)` dice cuál le tocó, que es lo
que hay que ponerle al otro extremo para que conteste ahí. Los valores negativos: **−1** no quedan
sockets, **−2** ese puerto ya está abierto, **−3** es un puerto del sistema.

`UdpSend` devuelve **−3** la primera vez que se le habla a una máquina nueva:
no es un error, es que todavía no se ha averiguado su dirección física y el
mensaje no ha salido. Hay que reintentar. Para tráfico continuo da igual —se
pierde el primero de muchos y los demás salen—; para un mensaje que tiene que
llegar, se reintenta.

Una dirección de **difusión** —`"255.255.255.255"`, o la de la propia red con
los últimos números a 255— manda a todas las máquinas a la vez. Es lo que
permite que dos programas se encuentren sin que nadie teclee una dirección.
Un mensaje por difusión **vuelve también a quien lo mandó**: para no atenderse
a sí mismo, se compara con `NetIp$()`.

**Se vacía la cola entera en cada vuelta, con `UdpPending`:**

```basic
While UdpPending(h) > 0
  m$ = UdpRecv$(h)
  quien$ = UdpFrom$(h)
  ...
Wend
```

Las dos cosas de ese bucle son las que se hacen mal:

**La condición tiene que ser `UdpPending`.** `UdpRecv$` devuelve una cadena, y
un mensaje vacío —que es legal— da la misma cadena vacía que «no hay nada».
`UdpFromPort` tampoco sirve: se queda con el último mensaje leído y después de
leer uno ya nunca vuelve a cero, así que usarlo como señal de «ya no hay más»
da un bucle que no termina.

**`UdpFrom$` se pregunta pegado al `UdpRecv$` que trajo el mensaje.** Se refiere
al último leído, así que en cuanto se lee otro cambia. Guardarse la dirección
«para luego» es como se acaba contestando a quien no era — y eso no da ningún
error, solo hace que el otro vea cosas raras.

Y leer uno por vuelta en vez de vaciar la cola tampoco da error: con varias
máquinas hablando, la cola crece más rápido de lo que se vacía, al llenarse se
tiran los que llegan y los mensajes se ven con retraso. `UdpLost` es la cifra
que lo dice.

### HTTP

| | Qué hace | Firma | Syscall |
|---|---|---|---|
| `HttpGet(ip$, puerto, ruta$)` | pide; 1 si arrancó, 0 si no | `SNS` | 307 |
| `HttpPost(ip$, puerto, ruta$, cuerpo$)` | manda; 1 si arrancó, 0 si no | `SNSS` | 307 |
| `HttpState()` | 0 parada, 1 en marcha, 2 lista, 3 fallo | — | 297 |
| `HttpCode()` | 200, 404… 0 si aún no se sabe | — | 297 |
| `HttpBody$()` | la respuesta recibida | — | 308 |
| `HttpFail$()` | por qué falló, en una línea | — | 299 |

Los dos devuelven **0 cuando no se pudo ni empezar**: ya hay otra petición en
marcha, la dirección no es una dirección, o la petición no cabe. Merece la pena
mirarlo, porque si no el programa cree que mandó algo que no salió.

La respuesta cabe en **2048 bytes**; lo que pase de ahí se descarta. Es de sobra
para lo que contesta un servidor —un «ok», unos valores, un error— y no es para
traerse una página web.

Solo se habla **HTTP**, nunca HTTPS: el cifrado lo pone un ayudante en la red
local. Y no hay redirecciones, ni cookies, ni autenticación.

```basic
Console
If NetReady() = 0 Then
  Print "la red no esta lista"
  End
EndIf

If HttpPost("192.168.1.40", 8099, "/sensor", "temp=21.5") = 0 Then
  Print "no se pudo empezar"
  End
EndIf

While HttpState() = 1
  Delay 1
Wend

If HttpCode() = 200
  Print "enviado; el servidor dice: " + HttpBody$()
Else
  Print "fallo: " + HttpFail$()
EndIf
```

Los ejemplos completos están en `otros programas/red`: `sensor.nb` manda
lecturas del GPIO por HTTP, `partida.nb` es el esqueleto de un multijugador
por UDP, y `pruebared.nb` los ejercita todos y dice cuáles funcionan.

---

Continúa en [REFERENCIA_NEMO_BASIC_4.md](REFERENCIA_NEMO_BASIC_4.md):
el índice alfabético de los 260 comandos.
