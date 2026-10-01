# Variables globales y locales en Nemo Basic

Guía breve del ámbito de las variables en Nemo Basic: cuáles son propias de
una función, cuáles se comparten con el resto del programa, por qué `Local`
deja de ser opcional en cuanto hay recursión, y qué avisa el compilador.

---

## La regla, en una línea

> Dentro de una función, **todo lo que no sea parámetro ni `Local` es
> GLOBAL**.

Es distinto de BlitzBasic, y es **la misma regla que Lua**, el otro
lenguaje de Nemo OS. Por eso no va a cambiar: los dos lenguajes del
sistema se comportan igual en esto.

```basic
contador = 0

Function Sumar(n)      ; n es parametro   -> local
  Local paso           ; declarada Local  -> local
  paso = n
  contador = contador + paso    ; contador NO esta declarada -> GLOBAL
  Return contador
End Function
```

---

## Las cuatro formas

| Forma | Dónde vive | Cuándo usarla |
|---|---|---|
| `x = 1` dentro de una función | global | por defecto; cuidado |
| `Local x` | en el marco de la llamada | temporales de la función |
| `Global x` | global, y **declarada** | estado que se comparte a propósito |
| `Const X = 9` | global, no se puede cambiar | números fijos del programa |

`Local` y `Global` se pueden declarar sin valor (`Global x`) o con él
(`Global x = 5`). `Const` **necesita** su valor en la misma línea.

Una `Const` es constante de verdad: cambiarla es un **error de
compilación**, no un aviso.

```basic
Const TOPE = 9
TOPE = 3        ; nbc: error en la linea 2: no se puede cambiar
                ; el valor de una constante
```

---

## Por qué importa: `Local` sobrevive a la recursión

Cada llamada tiene su propio marco, así que cada llamada tiene su propia
copia de las variables `Local`. Una variable global es **una sola para
todo el programa**, y la llamada de dentro pisa la de fuera.

```basic
Function CuentaBien(n)
  Local i
  i = 0
  While i < n
    i = i + 1
    If n > 1
      d = CuentaBien(n - 1)
    EndIf
  Wend
  Return i
End Function
```

`CuentaBien(3)` devuelve **3**, correcto.

La misma función sin `Local`:

```basic
Function CuentaMal(n)
  i = 0                      ; global: UNA para todo el programa
  While i < n
    i = i + 1
    If n > 1
      d = CuentaMal(n - 1)   ; la llamada de dentro pone i = 0 otra vez
    EndIf
  Wend
  Return i
End Function
```

`CuentaMal(3)` **se cuelga**. El bucle de fuera no termina nunca, porque
cada llamada de dentro devuelve el contador a cero. Comprobado en el
emulador: *"demasiados pasos (5000000): ¿un bucle sin fin?"*.

**Cualquier función recursiva necesita `Local` en todo lo que use como
temporal.**

---

## Por qué importa: dos funciones que creen tener cada una su `t`

```basic
Function Doble(k)
  t = k * 2        ; global
  Return t
End Function

Function SumaMal(n)
  t = 0            ; la MISMA t
  For j = 1 To n
    d = Doble(j)   ; aqui dentro, t pasa a valer j * 2
    t = t + d      ; ...y esta suma parte de ese valor, no del suyo
  Next
  Return t
End Function
```

`SumaMal(4)` devuelve **16**. Debería devolver 20 (2+4+6+8). Sin ningún
error, sin ningún aviso al ejecutar: simplemente el número está mal.

Con `Local`, las dos `t` son variables distintas:

```basic
Function SumaBien(n)
  Local t
  Local d
  t = 0
  For j = 1 To n
    d = DobleBien(j)
    t = t + d
  Next
  Return t
End Function
```

`SumaBien(4)` devuelve **20**.

**Lo peligroso de este fallo es que a veces sale bien por casualidad.**
La misma `SumaMal(3)` devuelve 12, que es el resultado correcto: los
números cuadran por accidente. Probar con un solo valor no demuestra
nada.

---

## El aviso del compilador

`nbc` avisa cuando una función usa una variable que
**también se usa fuera de ella** sin declararla:

```
nbc: aviso en la linea 3: la funcion Doble comparte 't' con el resto
del programa sin declararla (Global t)
```

Es un aviso, no un error: el programa compila igual y el binario no
cambia ni un byte. Declarar la variable con `Global` o `Const` lo apaga,
y de paso deja escrito en el programa que ese estado se comparte a
propósito.

**Lo que el aviso NO ve**, y conviene tener presente: una variable que
solo se usa dentro de UNA función no se avisa, porque no choca con
nadie. Pero si esa función es **recursiva**, choca consigo misma — el
caso de `CuentaMal` de arriba no genera ningún aviso y se cuelga igual.

---

## Patrones que funcionan bien

### 1. Temporales de una función: `Local`, o un prefijo

Lo correcto es `Local`. Si por lo que sea no se usa (código antiguo, una
función muy corta), el apaño es un prefijo por función:

```basic
Function MueveX(mx_x, mx_y, mx_dx)
  mx_paso = 1          ; mx_ = solo de MueveX
  ...
End Function
```

Feo, pero evita el choque. Es lo que hace `colision.nb`.

### 2. El contrato de una biblioteca: declararlo arriba

```basic
; colision.nb
; Estas tres las pone quien incluye el archivo; col_toco lo devuelve ella.
Global TILE
Global MAPA_W
Global MAPA_H
Global col_toco
```

Apaga el aviso y, sobre todo, deja el contrato escrito donde se ve: quien
incluya el archivo sabe qué variables tiene que poner sin leerse el código.

### 3. Estado de un módulo entre llamadas

```basic
; sincro.nb -- el ritmo vive entre una llamada y la siguiente
Global sin_paso
Global sin_anterior
Global sin_proximo
```

Aquí la global es lo correcto, no un apaño: `FijaFps` la escribe y
`Sincroniza` la lee, y eso es justo lo que se quiere.

### 4. Números fijos del programa: `Const`

```basic
Const ANCHO = 40
Const ALTO = 30
Const GRAVEDAD = 2
```

Mejor que `Global`, porque el compilador impide cambiarlos por
descuido.

---

## Detalles sueltos

- **Los arrays (`Dim`) son siempre globales** y no generan aviso.
  Compartir un array entre funciones es lo normal.
- Las **etiquetas** de `Goto`/`Gosub` son de nivel superior: `Gosub`
  dentro de una función no está soportado (ahí `Return` significa otra
  cosa).
- `Global` sin valor no genera **ninguna instrucción**: solo declara. Un
  programa con las declaraciones puestas y otro sin ellas producen el
  mismo binario byte a byte.
- Los sufijos de tipo cuentan como parte del nombre: `t`, `t#` y `t$`
  son **tres variables distintas**.

---

## Resumen

1. `Local` para todo lo que sea temporal de una función. Siempre, si la
   función es recursiva.
2. `Global` para el estado que de verdad se comparte, **declarándolo**.
3. `Const` para los números fijos.
4. Si el compilador avisa, o declaras o pones `Local`. Las dos
   respuestas son buenas; ignorarlo es la mala.
