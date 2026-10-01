# Referencia de Nemo Basic

*Todos los comandos del lenguaje, con ejemplos.*

Esta es la referencia **completa**: los 260 comandos y funciones incorporadas
del lenguaje, más sus palabras reservadas. Si buscas una introducción con la
que empezar, esa es `GUIA_NEMO_BASIC.md`; esto es el manual al que se vuelve.

## Cómo leer las tablas

La columna **Firma** dice qué espera cada argumento: `N` un número (entero o
decimal) y `S` una cadena. `NNS` son dos números y una cadena, en ese orden. Un
guion significa que el compilador no comprueba los argumentos de esa función.

La columna **Syscall** es el número de llamada al sistema que usa. Un guion
significa que **no hay llamada al sistema**: se calcula dentro del propio
programa, así que cuesta nanosegundos en vez de los 3,5 µs de una syscall. Las
matemáticas y las cadenas son casi todas de estas.

Un `$` al final del nombre significa que devuelve una cadena. Un `#`, que
trabaja en decimal.

Las funciones que llevan un guion en la columna de firma no esperan ningún
argumento: `MilliSecs`, `PollEvent`, `EventSource`, `NetIp$`, `HttpCode` y las
demás del mismo estilo. Si les pasas argumentos de sobra nadie protesta —
sencillamente se ignoran.

## Las cinco partes

El visor carga el documento entero en el montón de 4 MB de su tarea, así que
una referencia de este tamaño no cabe de una pieza. Va en cinco partes
enlazadas: al final de cada una hay un enlace a la siguiente.

1. **Esta parte** — el lenguaje: tipos, operadores, control de flujo, arrays,
   Types, funciones, Data, Include.
2. [REFERENCIA_NEMO_BASIC_2.md](REFERENCIA_NEMO_BASIC_2.md) — consola, tiempo,
   pantalla, gráficos, imágenes, teclado y ratón.
3. [REFERENCIA_NEMO_BASIC_3.md](REFERENCIA_NEMO_BASIC_3.md) — controles,
   menús, árbol, eventos, archivos, matemáticas, cadenas, GPIO, sonido y red.
4. [REFERENCIA_NEMO_BASIC_4.md](REFERENCIA_NEMO_BASIC_4.md) — el índice
   alfabético de los 260 comandos.
5. [REFERENCIA_NEMO_BASIC_5.md](REFERENCIA_NEMO_BASIC_5.md) — las trampas del
   lenguaje y dónde seguir.

---

# 1. El lenguaje

## 1.1 Líneas, comentarios y números

Una sentencia por línea. El comentario empieza con **`;`** y llega al final de
la línea. **No existe el comentario con apóstrofo** (`'`): el compilador lo
toma por una llamada a una función que no existe y da error.

```basic
Console
; esto es un comentario
Print 42          ; y esto también
Print $FF         ; hexadecimal con $
Print 0xFF        ; hexadecimal con 0x, lo mismo
Print %1010       ; binario, sale 10
```

Salida: `42`, `255`, `255`, `10`.

## 1.2 Variables y tipos

Tres tipos, y el sufijo del nombre es el que manda:

| Sufijo | Tipo | Ejemplo |
|---|---|---|
| ninguno | entero de 64 bits | `x = 5` |
| `#` | decimal | `x# = 1.5` |
| `$` | cadena | `s$ = "hola"` |

```basic
Console
n = 7
d# = 2.5
s$ = "nemo"
Print n
Print d#
Print s$
Print "n vale " + Str$(n)
```

Salida: `7`, `2.500000`, `nemo`, `n vale 7`.

**Los decimales se imprimen con seis cifras**: `2.5` sale como `2.500000`. Si
quieres otro formato, compón la cadena tú.

**Mezclar tipos no se perdona en silencio.** Guardar un número en algo con `$`
es un error de compilación, no una conversión a tus espaldas: usa `Str$()`
para ir a cadena y `Val()` para volver.

## 1.3 Declaraciones de ámbito

```basic
Console
Global marcador          ; visible desde cualquier función
Const MAXIMO = 100       ; no se puede reasignar

Function suma(a, b)
  Local total            ; solo dentro de esta función
  total = a + b
  Return total
End Function

marcador = suma(2, 3)
Print marcador
Print MAXIMO
```

Salida: `5`, `100`.

**`Local` no es opcional en cuanto hay recursión.** En Nemo Basic cualquier
variable que no declares `Local` es **global**, así que una función recursiva
que use una variable sin declarar como contador comparte ese contador con
todas sus llamadas y no termina nunca. El compilador avisa cuando una función
usa una variable que también se usa fuera sin `Global` ni `Const`: no es un
error y el binario no cambia, pero ese aviso merece leerse.

## 1.4 Operadores

| | |
|---|---|
| Aritmética | `+` `-` `*` `/` `Mod` |
| Comparación | `=` `<>` `<` `>` `<=` `>=` |
| Lógicos | `And` `Or` `Not` `Xor` |
| Bits | `Shl` `Shr` `Sar` |
| Constantes | `True` `False` `Null` |

```basic
Console
Print 7 / 2            ; división ENTERA entre enteros: 3
Print 7 Mod 3          ; 1
Print 1 Shl 4          ; 16
If 1 And Not 0 Then Print "si"
```

Salida: `3`, `1`, `16`, `si`.

**`/` entre enteros trunca.** Si quieres el decimal, que al menos uno de los
dos lo sea.

## 1.5 Condiciones

En una línea con `Then`, o en bloque:

```basic
Console
x = 5
If x > 3 Then Print "grande"

If x = 5
  Print "exacto"
ElseIf x > 5
  Print "pasado"
Else
  Print "corto"
EndIf
```

Salida: `grande`, `exacto`.

`End If` con espacio también vale.

## 1.6 Bucles

```basic
Console
For i = 1 To 3
  Print i
Next

For i = 3 To 1 Step -1
  Print i
Next

i = 0
While i < 2
  Print i
  i = i + 1
Wend

i = 0
Repeat
  Print i
  i = i + 1
Until i = 2
```

Salida, una por línea: `1`, `2`, `3`, `3`, `2`, `1`, `0`, `1`, `0`, `1`.

`Repeat ... Forever` es un bucle sin fin. **`Exit` sale del bucle** en curso:

```basic
Console
For i = 1 To 9
  If i = 3 Then Exit
  Print i
Next
```

Salida: `1`, `2`.

## 1.7 Select

```basic
Console
x = 2
Select x
  Case 1
    Print "uno"
  Case 2
    Print "dos"
  Default
    Print "otro"
End Select
```

Salida: `dos`.

## 1.8 Funciones

```basic
Console
Function doble(n)
  Return n * 2
End Function

Function saluda$(quien$)
  Return "hola " + quien$
End Function

Print doble(21)
Print saluda$("nemo")
```

Salida: `42`, `hola nemo`.

El sufijo del nombre de la función dice qué devuelve. Hasta **ocho
parámetros**. El orden de aparición no importa: una función puede llamar a
otra declarada más abajo, y a sí misma.

### Pasar un array

Un parámetro escrito `nombre()` recibe un array entero:

```basic
Console
Function Partir(texto$, sep$, trozos$())
  Local n, desde, corte
  desde = 1
  Repeat
    corte = Instr(texto$, sep$, desde)
    If corte = 0 Then
      trozos$(n) = Mid$(texto$, desde, Len(texto$) - desde + 1)
      Return n + 1
    EndIf
    trozos$(n) = Mid$(texto$, desde, corte - desde)
    n = n + 1
    desde = corte + Len(sep$)
  Forever
End Function

Dim partes$(10)
Global i, n
n = Partir("uno,dos,tres", ",", partes$)
For i = 0 To n - 1
  Print partes$(i)
Next
```

Salida: `uno`, `dos`, `tres`.

**Lo que viaja es el array, no una copia.** La función escribe en el del
programa que la llama, y es la única forma de que una función devuelva más de
un valor. Los paréntesis del parámetro van **vacíos**: el tamaño lo lleva el
propio array, así que la comprobación de rango sigue funcionando dentro de la
función y con el tamaño de verdad.

En la llamada se pone el nombre del array a secas, sin paréntesis
(`Partir(t$, ",", partes$)`). Tiene que ser un array declarado con `Dim` y del
mismo tipo que el parámetro: pasar una variable suelta, o un array de números
donde se esperan cadenas, **da error al compilar** y no un programa que se
estropea a la callada.

## 1.9 Arrays

`Dim` los crea, con índices desde 0. Una y dos dimensiones; **tres o más no
están soportadas** a propósito.

```basic
Console
Dim a(4)
Dim tablero(2, 2)
For i = 0 To 3
  a(i) = i * i
Next
tablero(1, 1) = 9
Print a(3)
Print tablero(1, 1)
```

Salida: `9`, `9`.

Usar un array antes de su `Dim`, o salirse del rango, da un **error en
tiempo de ejecución con su número de línea**, no basura silenciosa.

## 1.10 Tipos propios (Type)

Se declaran con `Type ... Field ... End Type`, se crean con `New`, se borran
con `Delete`, se recorren con `For... = Each Tipo` y se accede a los campos
con `\`. Tienen también `First`, `Last`, `Insert... Before/After`.

```basic
Console
Type Enemigo
  Field x, y, vida
End Type

e = New Enemigo
e\x = 100
e\vida = 3
Print e\x

f = New Enemigo
f\x = 200
For o = Each Enemigo
  Print o\x
Next

Delete First(Enemigo)
For o = Each Enemigo
  Print o\x
Next
```

Salida, una por línea: `100`, `100`, `200`, `200`.

Dentro de un `For Each` **puedes borrar el elemento actual**. Lo que no debes
borrar es otro distinto —en concreto el siguiente—, porque el bucle ya lo
tiene apuntado para la vuelta siguiente.

Leer un campo de un objeto `Null` da un error en tiempo de ejecución con su
línea:

```basic
Console
Type Punto
  Field x
End Type
p = Null
If p = Null Then Print "todavia no hay punto"
```

Salida: `todavia no hay punto`.

### El sufijo `.Tipo` es una anotación

Nemo Basic acepta la forma de BlitzPlus, con el tipo detrás del nombre:

```basic
Console
Type C
  Field x
End Type
c.C = New C
c.C\x = 7
Print c\x
```

Salida: `7`.

El sufijo es **decorativo**: se descarta al leer el programa, igual que el `%`
de `contador%`. El nombre de la variable es lo de delante del punto, así que
`c.C` y `c` son **la misma variable** y se pueden mezclar sin cuidado:

```basic
Console
Type C
  Field x
End Type
c.C = New C
c\x = 7
Print c.C\x
If c = Null Then Print "vacia" Else Print "vale"
```

Salida, una por línea: `7`, `vale`.

El sufijo no le dice nada al compilador —no comprueba el tipo—, así que lo
más limpio es no escribirlo: `e = New Enemigo`, `e\x`,
`For o = Each Enemigo`. Pero si aparece, no cambia nada.

## 1.11 Data, Read y Restore

```basic
Console
Data 10, 20, 30
Read a
Read b
Print a + b
Restore
Read c
Print c
```

Salida: `30`, `10`.

## 1.12 Goto, Gosub y etiquetas

Las etiquetas llevan un punto delante.

```basic
Console
Gosub saludo
Goto final
.saludo
Print "dentro"
Return
.final
Print "fuera"
```

Salida: `dentro`, `fuera`.

## 1.13 Include

`Include "archivo.nb"` mete otro archivo en el sitio donde aparece. El
compilador lleva la cuenta de qué línea es de qué archivo, así que un error
en un archivo incluido dice su nombre y su línea.

Un nombre a secas se busca junto al programa, en la raíz, en DOCUMENTOS y en
SISTEMA. Un nombre **con carpetas** es una ruta y manda: `Include
"JUEGOS/NEMO/utiles.nb"`. Valen `/` y `\`, una barra inicial significa desde
la raíz, no se admite `..`, y si el archivo no está en esa ruta es un error —
no se busca en ningún otro sitio.

## 1.14 Palabras que el lexer conoce y el lenguaje no tiene

`Try`, `Catch` y `Throw` están en el lexer pero **el analizador no las
acepta**: `Try` da «sentencia no reconocida». No hay manejo de excepciones en
Nemo Basic. Los errores en tiempo de ejecución (índice fuera de rango,
división entre cero, objeto `Null`, array sin `Dim`) paran el programa con un
mensaje y su línea.

---

Continúa en [REFERENCIA_NEMO_BASIC_2.md](REFERENCIA_NEMO_BASIC_2.md):
consola, tiempo, pantalla, gráficos, imágenes, teclado y ratón.
