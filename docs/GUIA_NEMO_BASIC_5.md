# Guía de Nemo Basic (5)

*Las syscalls por dentro, ejemplos, cuidados y la chuleta.*

Viene de [GUIA_NEMO_BASIC_4.md](GUIA_NEMO_BASIC_4.md).

---

## 5. Cómo funcionan las syscalls

Tu programa no toca el hardware directamente. Cuando escribes `Rect 10, 10, 50, 50`, el compilador genera unas pocas instrucciones que colocan el número de syscall y los argumentos en los registros del procesador y ejecutan la instrucción `svc`, que pasa el control al kernel. El kernel dibuja el rectángulo en el lienzo de tu ventana y devuelve el control a tu programa.

Tres cosas que conviene saber:

**1. Cada programa tiene su lienzo, y la pantalla lo recoge al compás del sistema.** Lo que dibujas no aparece en pantalla al instante: se muestra cuando tu programa cede el turno (en `Pump`, `WaitEvent` o `Delay`). Así la pantalla enseña siempre un fotograma completo, nunca uno a medio dibujar. En la práctica: dibuja el fotograma entero y luego llama a `Pump`.

**2. `Pump` marca el ritmo.** Espera hasta el siguiente latido del reloj del sistema, que da 100 latidos por segundo. Un bucle de juego con `Pump` va, por tanto, a unos 100 fotogramas por segundo, al mismo compás con que el sistema pinta la pantalla. **Todo bucle que no espere de otra forma necesita `Pump`**: un bucle sin él acapara su núcleo sin descanso.

**3. Nemo OS usa los cuatro núcleos de la Raspberry Pi.** Varios programas pueden estar ejecutándose a la vez de verdad. Para ti, como programador, es transparente: cada programa tiene su propia memoria, su propio lienzo y su propio estado de dibujo.

**Nemo Basic no permite llamar a una syscall por su número**: se usan siempre a través de sus comandos. Si necesitas una syscall que Nemo Basic no expone, Lua y C sí tienen acceso directo.

---

## 6. Programas de ejemplo

Todos estos programas están comprobados con el compilador.

### 6.1 Adivina el número (consola)

```basic
Console
; adivina.nb -- adivina el numero del 1 al 100

Seed MilliSecs()
secreto = Rnd(100) + 1
intentos = 0

Print "He pensado un numero del 1 al 100."

While 1
  Print "Tu apuesta:"
  n = Val(Input$())
  intentos = intentos + 1

  If n < secreto Then
    Print "Mas alto."
  ElseIf n > secreto Then
    Print "Mas bajo."
  Else
    Print "Acertaste en " + Str$(intentos) + " intentos."
    Exit
  EndIf
Wend
```

### 6.2 Pelota y pala (gráficos y teclado)

```basic
; pelota.nb -- una pelota que rebota y una pala que se mueve con las flechas
; Esc para salir

Const ANCHO = 400
Const ALTO = 300
Const PALA = 60                ; anchura de la pala

Graphics ANCHO, ALTO

bx = 200 : by = 100      ; posicion de la pelota
vx = 3   : vy = 2        ; velocidad de la pelota
px = 170                 ; posicion de la pala
puntos = 0

While Not KeyDown(1)

  ; mover la pala
  If KeyDown(105) Then px = px - 5
  If KeyDown(106) Then px = px + 5
  px = Max(0, Min(px, ANCHO - PALA))

  ; mover la pelota y rebotar en las paredes
  bx = bx + vx
  by = by + vy
  If bx < 0 Or bx > ANCHO - 10 Then vx = -vx
  If by < 0 Then vy = -vy

  ; rebote en la pala
  If by > ALTO - 30 And by < ALTO - 20 And bx > px - 10 And bx < px + PALA Then
    vy = -vy
    puntos = puntos + 1
  EndIf

  ; se escapo por abajo: vuelve arriba
  If by > ALTO Then
    by = 50
    puntos = 0
  EndIf

  ; dibujar
  Cls
  Color 255, 200, 0
  Oval bx, by, 10, 10
  Color 0, 180, 255
  Rect px, ALTO - 20, PALA, 8
  Color 255, 255, 255
  Text 8, 8, "Puntos: " + Str$(puntos)

  Pump        ; cede el turno y espera al siguiente latido (100 por segundo)
Wend
```

La estructura de todo juego: **leer la entrada → mover → dibujar el fotograma entero → `Pump`**.

### 6.3 Contador con botones (ventana con controles)

```basic
; botones.nb -- una ventana con controles que responde a los clics

CreateWindow "Contador", 100, 100, 300, 200

etiqueta = CreateTextField(20, 20, 260, 24)
SetGadgetText etiqueta, "Pulsa un boton"
sumar    = CreateButton("Sumar", 20, 60, 120, 28)
restar   = CreateButton("Restar", 160, 60, 120, 28)
salir    = CreateButton("Salir", 90, 120, 120, 28)

cuenta = 0

While 1
  e = WaitEvent()              ; duerme hasta que pase algo
  If e = 2051 Then Exit        ; 2051 = se pulso la X de la ventana

  quien = EventSource()
  If quien = sumar  Then cuenta = cuenta + 1
  If quien = restar Then cuenta = cuenta - 1
  If quien = salir  Then Exit

  SetGadgetText etiqueta, "Cuenta: " + Str$(cuenta)
Wend
```

Aquí no hay `Pump` ni `Cls`: el programa no dibuja en bucle, sino que espera con `WaitEvent`. El sistema se encarga de pintar los controles.

### 6.4 Fuente de partículas (tipos propios)

```basic
; fuente.nb -- tipos propios: una fuente de particulas
; Esc para salir

Type Particula
  Field x, y, vx, vy, vida
End Type

Graphics 400, 300
Seed MilliSecs()

While Not KeyDown(1)

  ; nacen dos particulas por fotograma, en el centro
  For i = 1 To 2
    p = New Particula
    p\x = 200 : p\y = 150
    p\vx = Rnd(7) - 3          ; de -3 a 3
    p\vy = Rnd(5) - 7          ; de -7 a -3: hacia arriba
    p\vida = 80
  Next

  Cls
  For p = Each Particula
    p\x = p\x + p\vx
    p\y = p\y + p\vy
    If p\vida Mod 3 = 0 Then p\vy = p\vy + 1     ; gravedad
    p\vida = p\vida - 1

    If p\vida > 0 Then
      Color 255, p\vida * 3, 0
      Rect p\x, p\y, 3, 3
    Else
      Delete p                 ; se puede borrar la particula ACTUAL dentro del bucle
    EndIf
  Next

  Pump
Wend
```

Cada partícula se borra a sí misma al morir, dentro del propio `For Each`.

### 6.5 Guardar y leer notas (archivos)

```basic
Console
; notas.nb -- escribir y leer un archivo

Print "Escribe notas. Linea vacia para terminar."

todo$ = ""
n = 0
While 1
  linea$ = Input$()
  If linea$ = "" Then Exit
  n = n + 1
  todo$ = todo$ + linea$ + Chr$(10)
Wend

r = WriteFile("NOTAS.TXT", todo$)
If r < 0 Then
  Print "No se pudo guardar"
  End
EndIf

Print "Releyendo el archivo:"
h = OpenFile("NOTAS.TXT")
While Eof(h) = 0
  Print "  " + ReadLine$(h)
Wend
CloseFile h
```

---

## 7. Cuidado: lo que conviene saber

**1. Todo bucle necesita `Pump`, `WaitEvent` o `Delay`.** Un `While` que no cede nunca el turno deja su núcleo ocupado al cien por cien.

**2. Dentro de un `For Each`, borra solo el elemento actual.** Borrar otro —sobre todo el siguiente— deja al bucle apuntando a algo que ya no existe.

**3. Con `Read`, escribe en `Data` cada valor con el tipo de la variable donde vas a leerlo**: `Data 2.0` para leerlo en una variable decimal, `Data 2` para una entera. Es el único sitio donde un número no se convierte solo, porque qué valor toca leer solo se sabe al ejecutar.

**4. El aleatorio con decimales es `Rnd()`**, sin argumentos. `Rnd#()` no existe.

**5. Los códigos de tecla son los de Nemo OS**, no los de BlitzPlus. Usa la tabla de la sección 4.4.

**6. Las variables son globales salvo parámetros y `Local`**, también dentro de las funciones. Si una función necesita una variable propia, decláralo con `Local`.

**7. Programas de consola guardados en una carpeta desde el IDE**: la shell que abre el IDE empieza en la raíz y puede no encontrarlos. Guarda los programas de consola en la raíz, o ejecútalos desde una shell situada en su carpeta.

---

## 8. Chuleta

> No es codigo para copiar: es un resumen. El `|` significa «o», y los `...`
> son lo que va en medio. Los ejemplos que si compilan estan en las partes
> anteriores y en [REFERENCIA_NEMO_BASIC.md](REFERENCIA_NEMO_BASIC.md).

```text
Console                         ; programa de consola (sin esto: de escritorio)
; comentario
x = 5 : y# = 2.5 : n$ = "hola"  ; entero, decimal, cadena
Const ANCHO = 400               ; constante (no se puede cambiar)
Global vidas = 3                ; variable global con valor inicial

If x > 3 Then ... | If ... ElseIf ... Else ... EndIf
Select x : Case 1 ... : Default ... : End Select
For i = 1 To 10 Step 2 ... Next
While ... Wend | Repeat ... Until ... | Repeat ... Forever | Exit

Function F(a, b$) ... Return valor ... End Function
F 1, "x"  |  F(1, "x")  |  r = F(1, "x")
Local temporal = 0              ; variable propia de la función

Dim a(10) | Dim m(10, 10)
Type T : Field x, y : End Type | t = New T | t\x = 1 | Delete First(T)
For t = Each T ... If t\x < 0 Then Delete t ... Next     ; borrar el ACTUAL, si

Graphics 400, 300 | Cls | Color r, g, b | Rect x, y, w, h | Oval x, y, w, h
Line x1, y1, x2, y2 | Text x, y, t$ | Plot x, y

KeyDown(1) | KeyHit(57) | MouseX() | MouseY() | MouseDown()
Pump                            ; en cada vuelta de un bucle de juego

CreateWindow t$, x, y, w, h | b = CreateButton(t$, x, y, w, h)
e = WaitEvent() | EventSource() | 2051 = cerrar

h = OpenFile(n$) | ReadLine$(h) | Eof(h) | CloseFile h | WriteFile(n$, todo$)

Seed MilliSecs() | Rnd(6) + 1 | Rnd()
End                             ; terminar
```

Compilar: `run nbc.pro programa.nb` — Ejecutar: `run programa.pro`
