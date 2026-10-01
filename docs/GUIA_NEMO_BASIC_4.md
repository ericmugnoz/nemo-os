# Guía de Nemo Basic (4)

*Menús, árbol, eventos, archivos, matemáticas, cadenas y GPIO.*

Viene de [GUIA_NEMO_BASIC_3.md](GUIA_NEMO_BASIC_3.md).

---

### Menús, barra de herramientas y temporizador

| | Qué hace | Syscall |
|---|---|---|
| `m = WindowMenu()` | la barra de menús de la ventana | 120 |
| `e = CreateMenu(texto$, etiqueta, padre)` | una entrada; `padre` es la barra o un menú. Si `padre` es **una entrada**, esta abre un **submenú** a su derecha (un nivel). Texto `""`: una línea de separación | 121 |
| `CheckMenu e` / `UncheckMenu e` | marcar o desmarcar una entrada | 122 |
| `EnableMenu e` / `DisableMenu e` | activarla o desactivarla (gris) | 123 |
| `UpdateWindowMenu` | no hace falta aquí (el kernel redibuja solo), pero se acepta | — |
| `b = CreateToolBar(archivo$, x, y, ancho, alto)` | barra de herramientas con una tira de iconos NIMG; ancho y alto 0 = los de la imagen | 172 |
| `SetToolBarTips b, "Uno,Dos,..."` | los textos de ayuda, que salen al dejar el ratón quieto sobre un botón | 174 |
| `EnableToolBarItem b, n` / `DisableToolBarItem b, n` | activar o desactivar el botón `n` (desde 0) | 173 |
| `SetPanelImage panel, archivo$` | una imagen NIMG de fondo para un panel | 222 |
| `t = CreateTimer(veces_por_segundo)` | un temporizador de la ventana | 127 |
| `PauseTimer t` / `ResumeTimer t` / `ResetTimer t` / `FreeTimer t` | pararlo, reanudarlo, volver a empezar, liberarlo | 216 / 217 / 218 / 131 |
| `TimerTicks(t)` | cuántas veces ha sonado | 219 |

Los eventos: **4097** (`$1001`) al elegir un menú, con `EventData()` = su etiqueta; **1025** al pulsar un botón de la barra, con `EventSource()` = la barra y `EventData()` = el número del botón; **16385** (`$4001`) cada vez que suena el temporizador.

En DOCUMENTOS/IMAGENES hay tiras de iconos hechas (`tb_basica24.nimg` y otras siete) e iconos de 32 píxeles: `GUIA_ICONOS.md` dice qué hay en cada una y en qué orden.

```basic
; Menus, barra de herramientas, iconos y temporizador
CreateWindow "Menus y barra", 100, 100, 460, 300

; los menus: la etiqueta de cada uno es lo que da EventData()
m = WindowMenu()
archivo = CreateMenu("Archivo", 0, m)
CreateMenu "Nuevo", 1, archivo
CreateMenu "Abrir", 2, archivo
CreateMenu "", 0, archivo                ; una linea de separacion
CreateMenu "Salir", 3, archivo
ver = CreateMenu("Ver", 0, m)
reloj = CreateMenu("Contar segundos", 4, ver)
ayuda = CreateMenu("Ayuda", 0, m)
CreateMenu "Acerca de", 5, ayuda
UpdateWindowMenu

; la barra: tb_basica24.nimg esta en DOCUMENTOS/IMAGENES (10 iconos de 24 px)
barra = CreateToolBar("tb_basica24.nimg", 0, 0, 0, 0)
SetToolBarTips barra, "Nuevo,Abrir,Guardar,Cortar,Copiar,Pegar,Deshacer,Ejecutar,Parar,Ayuda"
DisableToolBarItem barra, 8              ; Parar, hasta que algo este en marcha

; el nombre de cada boton de tb_basica, en su orden: con el se forma el
; archivo de su icono grande (ico32_cortar.nimg...), para el panel
Dim boton$(9)
Restore nombres
For i = 0 To 9
  Read boton$(i)
Next

icono = CreatePanel(20, 50, 32, 32)
SetPanelImage icono, "ico32_info.nimg"
aviso = CreateLabel("Elige algo en los menus o en la barra.", 64, 58, 360, 20)
cuenta = CreateLabel("", 20, 100, 200, 20)

contando = 0
segundos = 0
t = CreateTimer(1)
PauseTimer t

Repeat
  e = WaitEvent()
  If e = EVENT_MENUACTION                            ; un menu
    Select EventData()
      Case 1
        SetGadgetText aviso, "Nuevo documento."
      Case 2
        SetGadgetText aviso, "Abrir..."
      Case 3
        e = 2051
      Case 4
        contando = 1 - contando
        If contando
          CheckMenu reloj
          ResumeTimer t
          EnableToolBarItem barra, 8
        Else
          UncheckMenu reloj
          PauseTimer t
          DisableToolBarItem barra, 8
        EndIf
      Case 5
        SetPanelImage icono, "ico32_estrella.nimg"
        SetGadgetText aviso, "Nemo OS, a bordo del Nautilus."
    End Select
  ElseIf e = EVENT_GADGETACTION
    If EventSource() = barra
      SetGadgetText aviso, "Boton " + Str$(EventData()) + " de la barra: " + boton$(EventData()) + "."
      SetPanelImage icono, "ico32_" + boton$(EventData()) + ".nimg"
      If EventData() = 8
        contando = 0
        UncheckMenu reloj
        PauseTimer t
        DisableToolBarItem barra, 8
      EndIf
    EndIf
  ElseIf e = EVENT_TIMERTICK                       ; el temporizador
    segundos = segundos + 1
    SetGadgetText cuenta, Str$(segundos) + " segundos"
  EndIf
Until e = EVENT_WINDOWCLOSE
End

.nombres
Data "nuevo", "abrir", "guardar", "cortar", "copiar", "pegar", "deshacer", "ejecutar", "parar", "ayuda"
```

### Árbol y lienzo

| | Qué hace | Syscall |
|---|---|---|
| `a = CreateTreeView(x, y, ancho, alto)` | un árbol | 175 |
| `r = TreeViewRoot(a)` | su raíz, el padre de los nodos de primer nivel | 176 |
| `n = AddTreeViewNode(texto$, padre)` / `InsertTreeViewNode(indice, texto$, padre)` | añadir un nodo, al final o en una posición | 177 / 178 |
| `ModifyTreeViewNode n, texto$` / `FreeTreeViewNode n` | cambiar su texto / quitarlo | 179 / 180 |
| `ExpandTreeViewNode n` / `CollapseTreeViewNode n` | abrir o cerrar una rama | 181 |
| `CountTreeViewNodes(padre)` | cuántos hijos tiene | 182 |
| `SelectedTreeViewNode(a)` / `SelectTreeViewNode n` | el nodo elegido / elegirlo | 183 / 184 |
| `l = CreateCanvas(x, y, ancho, alto)` | un lienzo para dibujar | 188 |
| `SetBuffer CanvasBuffer(l)` | desde aquí, todo el dibujo va al lienzo, con sus coordenadas y recortado a él | 128 |
| `SetBuffer BackBuffer()` | volver a dibujar en la ventana | 128 |
| `SetBuffer ImageBuffer(img)` | dibujar dentro de una imagen | 128 |

Al elegir un nodo llega el evento **1025** con el árbol como `EventSource()`. El texto de un nodo se lee con `GadgetText$(nodo)`: cada nodo es un control. `FlipCanvas` se acepta por compatibilidad con BlitzPlus, pero aquí no hace falta: el dibujo va directo al lienzo.

```basic
; Un arbol y un lienzo: al elegir un nodo, el lienzo dibuja su color
CreateWindow "Arbol y lienzo", 100, 100, 460, 260
arbol = CreateTreeView(10, 10, 180, 200)
lienzo = CreateCanvas(210, 10, 230, 200)
raiz = TreeViewRoot(arbol)
calidos = AddTreeViewNode("Calidos", raiz)
AddTreeViewNode "Rojo", calidos
AddTreeViewNode "Naranja", calidos
frios = AddTreeViewNode("Frios", raiz)
AddTreeViewNode "Azul", frios
AddTreeViewNode "Verde", frios
ExpandTreeViewNode calidos
ExpandTreeViewNode frios
Repeat
  e = WaitEvent()
  If e = EVENT_GADGETACTION And EventSource() = arbol
    nombre$ = GadgetText$(SelectedTreeViewNode(arbol))
    SetBuffer CanvasBuffer(lienzo)
    Color 250, 250, 246
    Rect 0, 0, 230, 200, 1
    If nombre$ = "Rojo" Then Color 210, 50, 45
    If nombre$ = "Naranja" Then Color 240, 140, 40
    If nombre$ = "Azul" Then Color 40, 110, 210
    If nombre$ = "Verde" Then Color 40, 160, 70
    Oval 65, 50, 100, 100, 1
    Color 30, 30, 30
    Text 10, 10, nombre$
    SetBuffer BackBuffer()
  EndIf
Until e = EVENT_WINDOWCLOSE
End
```

### 4.7 Eventos

| | Qué hace | Syscall |
|---|---|---|
| `e = WaitEvent()` | **espera** hasta que pase algo | 8 |
| `e = PollEvent()` | mira si ha pasado algo, **sin esperar** (0 si nada) | 8 |
| `EventSource()` | qué control lo produjo | 9 |
| `EventData()` | dato extra del evento | 9 |
| `EventX()`, `EventY()` | posición asociada al evento | 256 |

Evento **2051** (`$803`): se pulsó la X de cerrar.

**La regla de los dos bucles**: `WaitEvent` para menús y formularios (el programa no gasta nada mientras espera); `PollEvent` + `Pump` para juegos (hay que seguir moviendo cosas aunque nadie toque nada). Un programa con menú **y** juego usa dos bucles distintos.

**Constantes de los eventos**. En vez de los números, sus nombres, los mismos que usan el kernel y `nemo_gui` en Lua:

| Constante | Valor | Cuándo llega |
|---|---|---|
| `EVENT_GADGETACTION` | 1025 | un control hizo algo (botón, lista, deslizador, árbol...) |
| `EVENT_WINDOWSIZE` | 2050 | la ventana cambió de tamaño |
| `EVENT_WINDOWCLOSE` | 2051 | se pulsó la X de cerrar |
| `EVENT_MENUACTION` | 4097 | se eligió una entrada de menú |
| `EVENT_TIMERTICK` | 16385 | saltó un temporizador |

```basic
Repeat
  e = WaitEvent()
  If e = EVENT_GADGETACTION And EventSource() = boton Then Print "pulsado"
Until e = EVENT_WINDOWCLOSE
```

Son constantes del sistema, como `Pi`: darles un valor es un error de compilación. Antes, escribir `EVENT_MENUACTION` compilaba como una variable vacía y valía 0 **en silencio**, así que la comparación no se cumplía nunca.


### 4.8 Archivos

| | Qué hace | Syscall |
|---|---|---|
| `h = OpenFile(nombre$)` | abre para leer (−1 si no existe) | 41 |
| `l$ = ReadLine$(h)` | lee la siguiente línea | 42 |
| `Eof(h)` | verdadero si no queda nada | 43 |
| `CloseFile h` | cierra | 44 |
| `r = WriteFile(nombre$, contenido$)` | escribe el archivo **entero** de una vez (0 bien, −1 mal) | 20 + 22 |

`OpenFile` busca primero en la raíz y luego en `DOCUMENTOS`. Para ir añadiendo texto, construye la cadena completa y escríbela al final; `Chr$(10)` es el salto de línea.

### 4.9 Matemáticas (sin syscall: se calculan dentro del programa)

| Función | Notas |
|---|---|
| `Abs`, `Sgn`, `Min(a, b)`, `Max(a, b)` | con decimales, el resultado es decimal |
| `Pi` | la constante π (3,14159…) |
| `Sqr(x#)` | raíz cuadrada |
| `Sin`, `Cos`, `Tan`, `ATan` | **ángulos en grados**, como en BlitzBasic |
| `Exp`, `Log` | exponencial y logaritmo natural |
| `Int(x#)` | quita los decimales, **hacia cero**: `Int(-2.5) = -2` |
| `Floor(x#)`, `Ceil(x#)` | redondea hacia abajo / hacia arriba: `Floor(-2.5) = -3` |
| `Float(n)` | entero a decimal |
| `Rnd(n)` | entero al azar de 0 a n−1 |
| `Rnd()` | decimal al azar entre 0 y 1 |
| `Seed n` | fija el punto de partida del azar |

Para que cada partida sea distinta: `Seed MilliSecs()` al empezar.

### 4.10 Cadenas (sin syscall)

| Función | Qué hace |
|---|---|
| `Len(s$)` | longitud |
| `Left$(s$, n)`, `Right$(s$, n)` | primeros / últimos n caracteres |
| `Mid$(s$, desde, n)` | trozo; la primera posición es la 1 |
| `Instr(s$, buscar$)` | posición de `buscar$` (0 si no está) |
| `Upper$(s$)`, `Lower$(s$)` | mayúsculas / minúsculas |
| `Trim$(s$)` | quita espacios de los extremos |
| `Replace$(s$, de$, a$)` | sustituye todas las apariciones |
| `Str$(n)`, `Val(s$)` | número a texto / texto a número |
| `Chr$(n)`, `Asc(s$)` | código a carácter / carácter a código |

---

### 4.11 Pines GPIO (Raspberry Pi 4)

| Orden | Qué hace |
|---|---|
| `GpioMode pin, modo` | Configura un pin del 2 al 27: 0 entrada, 1 salida, 2 entrada a positivo, 3 entrada a masa |
| `GpioWrite pin, valor` | Pone una salida a 1 o a 0 |
| `GpioRead(pin)` | Lee un pin: 1 o 0 |
| `GpioPwm pin, hz, ciclo` | PWM por hardware en los pines 12, 13, 18 o 19: brillo de un LED, servos. Ciclo en diezmilésimas (0 a 10000) |
| `I2cWrite dir, datos$` | Envía por I²C los bytes de la cadena (`Chr$` para construirla); devuelve cuántos |
| `I2cRead$(dir, n)` | Lee `n` bytes por I²C, en una cadena (`Asc(Mid$(...))` para sacarlos); vacía si falla |
| `SpiTransfer$(datos$, chip, hz, modo)` | Transferencia SPI (aparato 0 o 1): devuelve los bytes recibidos; vacía si falla |

Devuelven un número negativo si algo falla. Antes de conectar nada, lee
**GUIA_GPIO.md** (en MANUALES): qué patilla es cada pin, cómo montar
un LED y un pulsador, y los límites eléctricos que no hay que pasar. En
QEMU los pines se simulan y cada cambio aparece en la terminal.

---

Continúa en [GUIA_NEMO_BASIC_5.md](GUIA_NEMO_BASIC_5.md): las syscalls por dentro, ejemplos, cuidados y la chuleta.
