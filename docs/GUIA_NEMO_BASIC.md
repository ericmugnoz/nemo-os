# Guía rápida de Nemo Basic

Nemo Basic es el lenguaje de programación de Nemo OS: un BASIC de la familia de BlitzBasic, compilado a código máquina ARM64 de verdad. El compilador (`nbc.pro`) corre **dentro del propio sistema**, así que puedes escribir, compilar y ejecutar programas sin salir de Nemo OS.

Esta guía te lleva de cero a escribir juegos sencillos y ventanas con botones.

---

## Las cinco partes

La guía va en cinco partes enlazadas: el visor de Nemo OS carga el documento
entero en el montón de 4 MB de su tarea, y de una pieza no cabría. Al final de
cada parte hay un enlace a la siguiente.

1. **Esta parte** — empezar, compilar y el lenguaje.
2. [GUIA_NEMO_BASIC_2.md](GUIA_NEMO_BASIC_2.md) — la biblioteca: errores,
   archivos, Include, consola, tiempo, gráficos, teclado y ratón.
3. [GUIA_NEMO_BASIC_3.md](GUIA_NEMO_BASIC_3.md) — imágenes y controles.
4. [GUIA_NEMO_BASIC_4.md](GUIA_NEMO_BASIC_4.md) — menús, árbol, eventos,
   archivos, matemáticas, cadenas y GPIO.
5. [GUIA_NEMO_BASIC_5.md](GUIA_NEMO_BASIC_5.md) — las syscalls por dentro,
   ejemplos, cuidados y la chuleta.

Y la lista **completa** de los 260 comandos, con su firma y su syscall, está en
[REFERENCIA_NEMO_BASIC.md](REFERENCIA_NEMO_BASIC.md).

## Índice de esta parte

1. [Tu primer programa](#1-tu-primer-programa)
2. [Compilar y ejecutar](#2-compilar-y-ejecutar)
3. [El lenguaje](#3-el-lenguaje)

---

## 1. Tu primer programa

Los programas de Nemo Basic son archivos de texto con extensión `.nb`:

```basic
Console
; hola.nb -- el primer programa
Print "Hola desde Nemo Basic"
```

- **`Console`** en la primera línea dice que es un programa de **consola**: se ejecuta dentro de una shell y escribe texto. Sin esa línea (o con `Desktop`), el programa es de **escritorio**: abre su propia ventana gráfica.
- **`;`** empieza un comentario hasta el final de la línea.
- **`Print`** escribe una línea en la consola.

---

## 2. Compilar y ejecutar

### Desde el IDE (lo más cómodo)

1. Abre el **IDE** desde el escritorio.
2. Escribe el programa y guárdalo con extensión `.nb`.
3. Pulsa **Compilar y Ejecutar**.

Si hay un error, el IDE lo muestra con el número de línea. Si no, el programa se ejecuta: los de consola se abren en una shell nueva, los de escritorio en su propia ventana.

### Desde la shell

```
run nbc.pro hola.nb
```

El compilador responde con algo como:

```
nbc: listo -> hola.pro (7760 bytes)
```

El `.pro` es el programa ejecutable, y queda en la misma carpeta que el `.nb`. Para ejecutarlo:

```
run hola.pro
```

### Desde el explorador

Doble clic en un `.pro` lo ejecuta. Si es de consola, el explorador le abre una shell automáticamente.

### Desde el Mac

El mismo compilador existe también como programa del Mac (`nbc_driver`), y genera exactamente los mismos binarios, byte a byte:

```
./nbc_driver hola.nb hola.pro
```

Para llevarlo a la Pi, copia el `.pro` a un pendrive y, en Nemo OS, pásalo al disco del sistema con el explorador.

### Cuando hay un error

El compilador para en el primer error y dice la línea y el motivo:

```
nbc: error en la linea 12: llamada a una funcion que no existe
```

Nemo Basic intenta **no aceptar nunca en silencio algo que no sabe hacer**: si una construcción no está soportada, da un error claro en vez de compilar algo que no hace lo que dice. La sección 7 recoge los pocos puntos a tener en cuenta.

---

## 3. El lenguaje

### 3.1 Líneas y comentarios

- Una instrucción por línea. Con **`:`** puedes poner varias en la misma: `x = 1 : y = 2`.
- **`;`** empieza un comentario.
- Las palabras clave no distinguen mayúsculas de minúsculas en su forma habitual (`While`, `WHILE`).

### 3.2 Variables y tipos

El tipo de una variable lo marca un **sufijo** en el nombre:

| Sufijo | Tipo | Ejemplo |
|---|---|---|
| (ninguno) | entero de 64 bits | `vidas = 3` |
| `#` | número con decimales (doble precisión) | `velocidad# = 2.5` |
| `$` | cadena de texto | `nombre$ = "Eric"` |

Las variables no se declaran: se crean al usarlas por primera vez y empiezan valiendo 0 (o cadena vacía).

**Al guardar un número, se convierte solo al tipo de su destino**: `x# = 5` guarda 5,0, y `n = x#` quita los decimales hacia cero, igual que `Int()`. Vale igual para variables, arrays, campos de un `Type`, argumentos de funciones y `Return`. Cadenas y números no se mezclan: `x$ = 5` es un error de compilación; conviértelos a mano con `Str$()` y `Val()`.

**Verdadero y falso**: falso es `0` y verdadero es `-1` (todos los bits a uno), como en BlitzBasic. Existen `True` y `False`. Cualquier valor distinto de cero cuenta como verdadero en un `If`.

Los números se pueden escribir en hexadecimal de dos formas: `$FF00FF` o `0xFF00FF`.

**Constantes**: un valor con nombre que no cambia.

```basic
Const ANCHO = 400
Const TITULO$ = "Mi juego"
```

Una constante necesita su valor en la misma línea, y el compilador da error si el programa intenta cambiarla después.

**`Global`** declara una variable con su valor inicial: `Global vidas = 3`. Es equivalente a `vidas = 3`, porque toda variable que no sea local ya es global (ver 3.6), pero deja claro en qué variables se apoya el programa.

### 3.3 Operadores

| Tipo | Operadores |
|---|---|
| Aritméticos | `+`  `-`  `*`  `/`  `Mod` |
| Comparación | `=`  `<>`  `<`  `>`  `<=`  `>=` |
| Lógicos y de bits | `And`  `Or`  `Xor`  `Not` |
| Desplazamientos | `Shl`  `Shr`  `Sar` |
| Cadenas | `+` concatena |

Como verdadero es `-1`, `And`, `Or` y `Not` sirven a la vez como operadores lógicos y de bits.

Para unir texto y números, convierte el número con `Str$`:

```basic
Print "Tienes " + Str$(vidas) + " vidas"
```

`Print` también acepta varios valores separados por comas: `Print "x = ", x`.

### 3.4 Condiciones

En una línea:

```basic
If x > 10 Then x = 10
```

En bloque:

```basic
If n < 3 Then
  Print "poco"
ElseIf n < 7 Then
  Print "medio"
Else
  Print "mucho"
EndIf
```

Varios casos:

```basic
Select opcion
  Case 1
    Print "uno"
  Case 2, 3
    Print "dos o tres"
  Default
    Print "otro"
End Select
```

### 3.5 Bucles

```basic
For i = 1 To 10            ; de 1 a 10, ambos incluidos
  Print i
Next

For i = 10 To 0 Step -2    ; hacia atrás, de 2 en 2
  Print i
Next

While vidas > 0
;...
Wend

Repeat
;...
Until terminado

Repeat
;...
  If algo Then Exit
Forever
```

**`Exit`** sale del bucle más interno, sea del tipo que sea (también `For Each`, ver 3.8).

### 3.6 Funciones

```basic
Function Doble(n)
  Return n * 2
End Function

Function Saluda(nombre$)
  Print "Hola " + nombre$
End Function
```

**Cómo llamarlas**:

```basic
y = Doble(5)        ; si devuelve un valor, úsalo en una expresión
Saluda "Eric"       ; si solo HACE algo: como un comando, sin paréntesis...
Saluda("Eric")      ; ...o con ellos: las dos formas son equivalentes
```

**Ámbito de las variables — distinto de BlitzBasic:**

- Los **parámetros** y las variables declaradas con **`Local`** son propias de la función.
- **Cualquier otra variable es global**, también dentro de las funciones. Si una función usa `puntos`, es la misma `puntos` del programa principal.

```basic
Function Calcula(a)
  Local temporal = a * 2    ; solo existe dentro de la función
  puntos = puntos + 1       ; ¡la variable global del programa!
  Return temporal
End Function
```

Las funciones pueden llamarse a sí mismas (recursión) — **y ahí `Local` deja
de ser una comodidad**. Cada llamada tiene su propio marco, así que cada
llamada tiene su propia copia de las variables `Local`; una variable global
es una sola para todo el programa, y la llamada de dentro pisa la de fuera.
Una función recursiva que use una global como contador **no termina nunca**.

**El compilador avisa.** Cuando una función usa una variable que también se
usa fuera de ella sin declararla, `nbc` lo dice:

```
nbc: aviso en la linea 39: la funcion Doble comparte 't' con el resto
del programa sin declararla (Global t)
```

No es un error: el programa compila igual y el binario no cambia ni un byte.
Declarar la variable con `Global` o `Const` lo apaga, y de paso deja escrito
que ese estado se comparte a propósito. Solo avisa de las **compartidas**:
una variable interna de una función no genera ruido.

Lo que el aviso **no** ve: una variable que solo se usa dentro de una
función no se avisa, porque no choca con nadie — pero si esa función es
recursiva, choca consigo misma. Para eso, `Local`.

Hay una guía aparte con esto en detalle y con ejemplos:
[GUIA_AMBITO_NEMO_BASIC.md](GUIA_AMBITO_NEMO_BASIC.md).

### 3.7 Arrays

```basic
Dim tabla(10)          ; índices del 0 al 10, ambos incluidos
tabla(3) = 42

Dim mapa(20, 15)       ; dos dimensiones
mapa(5, 7) = 1
```

- Una o dos dimensiones.
- **Cada acceso se comprueba**: un índice fuera de rango detiene el programa con el mensaje "indice de array fuera de rango", en lugar de escribir en memoria ajena.

### 3.8 Tipos propios

Para agrupar datos, como los enemigos de un juego:

```basic
Type Enemigo
  Field x, y, vida
End Type

e = New Enemigo        ; crea uno y lo añade a la lista de su tipo
e\x = 100
e\vida = 3

For e = Each Enemigo   ; recorre todos, del más viejo al más nuevo
  e\x = e\x + 1
Next

Delete First(Enemigo)  ; borra el más viejo
```

Los campos se escriben con **`\`**: `e\vida`.

Dentro de un `For Each` **puedes borrar el elemento actual**, como en BlitzBasic:

```basic
For e = Each Enemigo
  If e\vida <= 0 Then Delete e
Next
```

Lo que no debes borrar dentro del bucle es **otro** elemento distinto del actual —en concreto el siguiente—, porque el bucle ya lo tiene apuntado para la vuelta siguiente.

### 3.9 Datos, saltos y subrutinas

```basic
Data 10, 20, 30        ; valores guardados en el programa
Read a                 ; a = 10
Read b                 ; b = 20
Restore                ; vuelve a leer desde el principio

Gosub dibujar          ; salta a la subrutina y vuelve con Return
;...
End

.dibujar               ; una etiqueta empieza por punto
  Rect 10, 10, 50, 50
  Return
```

`Goto` y `Gosub` existen, pero en código nuevo son preferibles las funciones. `Gosub` no se puede usar dentro de una `Function`.

**`End`** termina el programa en el acto, desde donde sea.

---

---

Continúa en [GUIA_NEMO_BASIC_2.md](GUIA_NEMO_BASIC_2.md): la biblioteca: errores, archivos, Include, consola, tiempo, gráficos, teclado y ratón.
