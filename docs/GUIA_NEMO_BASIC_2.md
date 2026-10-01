# Guía de Nemo Basic (2)

*La biblioteca: errores, archivos, Include, consola, tiempo, gráficos, teclado y ratón.*

Viene de [GUIA_NEMO_BASIC.md](GUIA_NEMO_BASIC.md).

---

## 4. La biblioteca: comandos y funciones

Casi todo lo que un programa hace con el mundo exterior —pantalla, teclado, archivos, ventanas— pasa por una **llamada al sistema** (*syscall*): una petición al kernel de Nemo OS. Las tablas indican el número de syscall que hay detrás de cada comando, por si quieres seguir el código del kernel (`src/syscall.c`).

**Comandos y funciones**: los comandos se escriben sin paréntesis (`Cls`, `Rect 10, 10, 50, 50`); las funciones devuelven un valor y llevan paréntesis (`x = MouseX()`). Una función que *hace* algo puede escribirse también como comando, sin paréntesis: `SetGadgetText boton, "OK"`.

> **La lista completa está en otro sitio.** Esta guía enseña a usar el
> lenguaje y cubre lo que se necesita a diario. Para **los 260 comandos y
> funciones**, cada uno con su firma, su número de syscall y un ejemplo, está
> [REFERENCIA_NEMO_BASIC.md](REFERENCIA_NEMO_BASIC.md), en cinco partes.

### Errores al ejecutar

Si el programa falla mientras se ejecuta, se detiene y dice **en qué línea** y por qué:

```
Error en tiempo de ejecucion, linea 23: indice de array fuera de rango (indice 11, el array tiene 11 elementos)
Error en tiempo de ejecucion, linea 2 de calculos.nb: division entre cero
```

| Error | Cuándo |
|---|---|
| índice de array fuera de rango | `a(i)` con `i` negativo o mayor que lo que se reservó con `Dim` (dice el índice y el tamaño) |
| división entre cero | `/` o `Mod` con un divisor entero que vale 0 (sin esta comprobación, daría 0 y seguiría con un resultado falso) |
| objeto Null | `p\x` con `p` sin `New`, o después de `Delete` |
| array sin Dim | usar un array antes de que se ejecute su `Dim` |

El mensaje aparece en el registro del IDE (que pone el cursor en esa línea), en Aronnax (que te lleva a tu línea) y en la terminal de QEMU. Mientras nada falla, cada comprobación cuesta una sola instrucción.

### Mover y medir controles, y la ventana

| | Qué hace |
|---|---|
| `SetGadgetShape id, x, y, ancho, alto` | lo coloca y le da tamaño |
| `GadgetX(id)` / `GadgetY(id)` / `GadgetWidth(id)` / `GadgetHeight(id)` | dónde está y cuánto mide |
| `ClientWidth()` / `ClientHeight()` | la zona de dibujo de la ventana |

Con esto, un programa puede recolocar sus controles cuando la ventana cambia de tamaño (evento `EVENT_WINDOWSIZE`); Aronnax lo genera solo con sus **anclajes**. El kernel borra donde estaba el control al moverlo, así que no queda ningún rastro.

### Cadenas: dar formato

| | Qué hace |
|---|---|
| `String$(t$, n)` | repite `t$` n veces: `String$("-", 20)` |
| `LSet$(t$, n)` | `t$` a la izquierda, rellenado con espacios hasta el ancho n (y cortado si no cabe) |
| `RSet$(t$, n)` | igual, pero a la derecha: para alinear números en columnas |
| `Hex$(n)` / `Bin$(n)` | el número en hexadecimal o en binario, sin ceros delante: `Hex$(255)` es `"FF"` |

El resultado de `String$`, `LSet$` y `RSet$` se corta a 1024 caracteres.

### Archivos y carpetas

Lo de siempre (leer un archivo línea a línea):

| | Qué hace |
|---|---|
| `f = OpenFile(nombre$)` | abre para leer, o -1 si no existe |
| `ReadLine$(f)` / `Eof(f)` | la siguiente línea / si ya no queda nada |
| `WriteFile nombre$, texto$` | escribe un archivo entero de una vez |
| `CloseFile f` | cierra |

**Rutas con carpetas**. Un nombre de archivo puede decir en qué carpeta
está:

```basic
hoja = LoadAnimImage("JUEGOS/NEMO/tiles.nimg", 16, 16, 0, 32)
f    = OpenFile("/JUEGOS/NEMO/nivel.txt")
```

Vale `/` y `\`, y una barra inicial que solo significa "desde la raíz", que
es de donde se parte siempre. **No** se admite `..`: no hay carpeta actual
de la que subir. Un nombre **sin** separador se busca en la raíz y en
DOCUMENTOS, y para imágenes también en DOCUMENTOS/IMAGENES.

**Todo acepta rutas, no solo lo de leer.** `WriteFile`, `CreateDir`,
`DeleteFile`, `FileSize`, `FileType`, `FileExists` y `ReadDir` las entienden
igual; un nombre a secas sigue cayendo donde siempre. Si la carpeta de la ruta
no existe, la llamada **falla** en vez de dejar el archivo suelto en la raíz
con la ruta entera por nombre:

```basic
CreateDir "JUEGOS/PARTIDAS"                   ; crea la última; JUEGOS ya tiene que existir
WriteFile "JUEGOS/PARTIDAS/nivel3.dat", datos$
DeleteFile "JUEGOS/PARTIDAS/viejo.dat"
Print FileExists("JUEGOS/PARTIDAS/nivel3.dat")
d = ReadDir("JUEGOS/PARTIDAS")
```

Para escribir, y para manejar carpetas:

| | Qué hace |
|---|---|
| `h = WriteFile(nombre$)` | con **un** argumento: abre para escribir, vacío |
| `WriteLine h, texto$` | escribe el texto y un salto de línea |
| `FilePos(h)` / `SeekFile h, p` / `FileLength(h)` | dónde va / ir a una posición / cuánto mide |
| `FileSize(nombre$)` | su tamaño, o -1 si no existe |
| `FileType(nombre$)` | 0 nada, 1 archivo, 2 carpeta |
| `FileExists(nombre$)` | 1 o 0 |
| `DeleteFile nombre$` | lo borra (0 si pudo) |
| `CreateDir nombre$` | crea una carpeta (en la raíz, o donde diga la ruta) |
| `d = ReadDir(carpeta$)` | abre una carpeta para listarla (`""` es la raíz) |
| `NextFile$(d)` | el siguiente nombre, o `""` cuando no quedan |
| `CloseDir d` | cierra la carpeta |

`CloseFile` sirve para los dos tipos de archivo, el de lectura y el de escritura.

```basic
Console
h = WriteFile("prueba.txt")
WriteLine h, "linea uno"
WriteLine h, "linea dos"
Print FileLength(h)
CloseFile h
Print FileExists("prueba.txt")
Print FileSize("prueba.txt")
Print FileType("prueba.txt")
f = OpenFile("prueba.txt")
While Not Eof(f)
  Print ReadLine$(f)
Wend
CloseFile f
Print CreateDir("MIS_COSAS")
Print FileType("MIS_COSAS")
d = ReadDir("")
n$ = NextFile$(d)
While n$ <> ""
  Print "  " + n$
  n$ = NextFile$(d)
Wend
CloseDir d
Print DeleteFile("prueba.txt")
Print FileExists("prueba.txt")
Print FileSize("no_existe.txt")
```

### Include: el programa en varios archivos

Con `Include "archivo.nb"`, en una línea sola, el contenido de otro archivo se pone en ese sitio antes de compilar. Así se reparte un programa grande en archivos más pequeños, o se reutilizan funciones entre programas: sus `Function`, `Global`, `Type` y `Data` quedan disponibles en todo el programa.

- Un nombre **a secas** se busca junto al programa, y después en la raíz, en DOCUMENTOS y en SISTEMA.
- Un nombre **con carpetas** dice exactamente dónde está, y entonces solo se mira ahí.
- Un archivo que ya se incluyó **no se incluye otra vez**, aunque varios lo pidan.
- Un Include en bucle (un archivo que acaba incluyéndose a sí mismo) es un error, y un archivo que no existe también.
- Los errores dicen **en qué archivo y en qué línea** están: `error en utiles.nb, linea 5: ...`.

`principal.nb`:

```basic
Console
Include "utiles.nb"
Include "colores.nb"      ; ya incluido desde utiles.nb: no se repite

Print Saluda$("Nemo")
Print Doble(21)
Print azul
```

`utiles.nb`:

```basic
; utiles.nb -- funciones de uso comun
Include "colores.nb"

Function Doble(n)
  Return n * 2
End Function

Function Saluda$(nombre$)
  Return "Hola, " + nombre$
End Function
```

`colores.nb`:

```basic
; colores.nb -- colores con nombre
Global rojo, verde, azul
rojo = 1
verde = 2
azul = 3
```

**Incluir desde otra carpeta.** Si el nombre lleva separadores, es una ruta y
dice exactamente dónde está el archivo:

```basic
Console
Include "JUEGOS/NEMO/utiles.nb"
Include "COMUN\dibujo.nb"        ; vale '/' y vale '\'
Include "/BIBLIOTECA/mates.nb"   ; la barra inicial: desde la raíz
```

Las reglas son las mismas que en el resto del sistema:

- Valen `/` y `\`, y da igual mezclarlos.
- Una barra inicial solo significa **desde la raíz**, que es de donde se parte
  siempre. No hay carpeta actual.
- **No se admite `..`**: no hay de dónde subir.
- Si el nombre lleva carpetas, **solo se mira ahí**. No se busca además en la
  raíz ni en DOCUMENTOS: si el archivo no está en esa ruta, es un error, y no
  otro archivo con el mismo nombre en otro sitio.
- Un componente intermedio tiene que ser una carpeta, y el último un archivo:
  `Include "JUEGOS/"` es un error, no un archivo vacío.

Funciona igual dentro de Nemo OS y con el compilador del Mac, así que un
programa repartido en carpetas se compila igual en los dos sitios. Y el
programa principal admite lo mismo: `run nbc.pro JUEGOS/juego.nb` compila ese
archivo y deja el `.pro` **en esa misma carpeta**.

### 4.1 Consola

| Comando / función | Qué hace | Syscall |
|---|---|---|
| `Print valor, ...` | escribe una línea | 11 |
| `s$ = Input$()` | lee una línea del teclado (con eco y retroceso) | 258 |

### 4.2 Tiempo

| | Qué hace | Syscall |
|---|---|---|
| `Delay ms` | espera esos milisegundos | 1 |
| `t = MilliSecs()` | milisegundos desde el arranque (resolución de 10 ms) | 2 |
| `t = MicroSecs()` | microsegundos desde el arranque | 272 |

`MilliSecs` sale del reloj del planificador, que late 100 veces por segundo:
**solo da saltos de 10 ms**. Con él no se puede medir un fotograma ni hacer
física por tiempo transcurrido. `MicroSecs` sale del contador del propio
procesador, no depende de interrupciones, y es el que hay que usar para
medir.

### 4.3 Gráficos

| | Qué hace | Syscall |
|---|---|---|
| `Graphics ancho, alto` | abre la ventana del programa | 46 |
| `Cls` | borra la ventana | 33 + 30 |
| `ClsColor r, v, a` | el color con el que borra `Cls` (sin él, negro, como siempre) |
| `Color r, g, b` o `Color 0xRRGGBB` | fija el color de lo que se dibuje después | — |
| `Plot x, y` | un punto | 30 |
| `Rect x, y, ancho, alto` | rectángulo relleno | 30 |
| `Line x1, y1, x2, y2` | línea | 257 |
| `Oval x, y, ancho, alto` | elipse rellena | 47 |
| `Text x, y, texto$` | texto | 31 |

El color funciona como en BlitzBasic: `Color` lo fija y los demás comandos lo usan hasta el siguiente `Color`.

### 4.4 Teclado y ratón

| | Qué hace | Syscall |
|---|---|---|
| `KeyDown(código)` | verdadero mientras la tecla está pulsada | 48 |
| `KeyHit(código)` | verdadero si se pulsó desde la última consulta | 53 |
| `k = GetKey()` | siguiente tecla de la cola (0 si no hay) | 54 |
| `MouseX()`, `MouseY()` | posición del ratón dentro de la ventana | 34 |
| `MouseDown()` | verdadero si el botón está pulsado | 34 |
| `KeyBank(n)` | el estado de 64 teclas de golpe, en un entero | 273 |
| `Pump` | cede el turno al sistema hasta el siguiente latido | 14 |

`KeyDown` es **una llamada al sistema por tecla**, y una llamada cuesta unos
3,5 µs en la Raspberry Pi 4 haga lo que haga. En un bucle de juego, correr,
saltar y disparar son tres llamadas cada fotograma. `KeyBank(n)` trae 64
teclas en un entero: el **banco 0** lleva Esc, Espacio, números y letras, y
el **banco 1** las flechas, así que con dos llamadas se tiene todo. A partir
de ahí, mirar una tecla es un `And`:

```basic
b0 = KeyBank(0)
b1 = KeyBank(1)
If (b1 Shr (106 - 64)) And 1     ; flecha derecha, sin salir al kernel
```

Usa `Shr`, no una división: los enteros son de 64 bits **con signo** y el
bit 63 haría negativo el número.

**El teclado y el ratón son solo para la ventana activa**: un programa cuya ventana no tiene el foco recibe "nada pulsado".

**Códigos de tecla** — los del sistema, **no** los de BlitzPlus:

| Tecla | Código |
|---|---|
| Esc | 1 |
| Enter | 28 |
| Espacio | 57 |
| Flecha arriba | 103 |
| Flecha izquierda | 105 |
| Flecha derecha | 106 |
| Flecha abajo | 108 |

---

Continúa en [GUIA_NEMO_BASIC_3.md](GUIA_NEMO_BASIC_3.md): imágenes y ventanas con controles.
