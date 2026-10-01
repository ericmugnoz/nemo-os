# Lo que falta en Nemo Basic

El lenguaje tiene hoy los **260 comandos y funciones** que lista
[REFERENCIA_NEMO_BASIC_4.md](REFERENCIA_NEMO_BASIC_4.md), y un solo compilador:
`nbc` (`nbc-selfhost`), que corre tanto en el Mac como dentro de Nemo OS como
`nbc.pro`.

Esta lista es lo que **no** está. Cada punto se ha comprobado contra el
compilador de verdad, escribiendo el programa y mirando lo que hace: al final
hay una orden para volver a comprobarla entera.

---

## Límites del lenguaje

Lo que el compilador rechaza, y por qué. Ninguno pasa ya en silencio: los tres
últimos se aceptaban sin decir nada y se han cerrado.

| Qué | Qué pasa |
|---|---|
| Arrays de **tres o más dimensiones** | `Dim c(2,2,2)` da error. Una y dos están; tres quedó fuera del diseño. |
| `Try`, `Catch`, `Throw` | El lexer conoce las palabras, el analizador no las acepta: **no hay excepciones**. Un error en tiempo de ejecución para el programa y dice la línea. |
| Más de **ocho argumentos** | Son los que caben en los registros `x0`–`x7`. Pasarse da error; antes se recortaba y el noveno llegaba como cero. |
| Una `Function` **dentro de otra** | Da error. Antes se aceptaba, no se generaba y no se podía llamar: la función desaparecía sin ningún mensaje. |
| `lista.P(i)\campo` | No se analiza. Los arrays de `Type` funcionan, pero para leer un campo hay que pasar por una variable: `p.P = lista.P(i)` y luego `p\campo`. |

## Falta capacidad de verdad

Aquí no hay atajo: falta una pieza por debajo, en el kernel.

| Nombre | Qué falta |
|---|---|
| `SetWindowModal` | **Un programa tiene UNA ventana**: `CreateWindow` no abre otra, reconfigura la que la tarea ya tiene (`task_ensure_window` en `tasks.c`). Una ventana modal sobre la única ventana propia no significa nada, y modal sobre la de OTRO programa no debería poder hacerse: sería que cualquiera pueda congelar al de al lado. Para que esto tenga sentido primero hacen falta varias ventanas por tarea, que es un cambio en el gestor de ventanas y en el reparto de eventos, no un comando. |
| `StopSound`, `ChannelPlaying`, `ChannelVolume` / `ChannelPan` / `ChannelPitch`, `LoopSound` | Todos piden lo mismo: que el sonido **no sea bloqueante**. Hoy `PlaySound` reproduce de principio a fin en una sola llamada, así que cuando vuelve ya no hay ningún canal sobre el que actuar — por eso el "canal" que devuelve es el propio sonido. Hace falta un búfer circular y un mezclador que sume las voces activas, rellenado desde el latido del reloj; con eso el canal existe de verdad y estos comandos salen casi solos. **Aparcado para Nemo 2.0**, en dos pasos: primero el mezclador y el anillo, que no necesitan DMA; después el DMA como sustitución de pieza — y ese mismo DMA sirve para las copias de pantalla, así que un solo driver paga dos deudas. |

## Escritas en Nemo Basic, no en el compilador

Están en **`otros programas/utiles/utiles.nb`**, y se usan con
`Include "utiles.nb"`:

| Nombre | Qué hace |
|---|---|
| `StripDir$(ruta$)` | El nombre del archivo, sin la carpeta |
| `ExtractDir$(ruta$)` | La carpeta, sin el nombre del archivo |
| `StripExt$(nombre$)` | El nombre sin la extensión |
| `ExtractExt$(nombre$)` | Solo la extensión |
| `IsDigit(c$)`, `IsAlpha(c$)`, `IsSpace(c$)` | Clasificar un carácter |
| `Split(texto$, sep$, tope, partes$())` | Partir una cadena por un separador |
| `Join$(sep$, cuantos, partes$())` | Volver a juntarla |
| `ListDir(carpeta$, tope, nombres$(), tipos())` | Listar una carpeta |
| `GadgetDoubleClicked(id)` | Doble clic: dos avisos del mismo control seguidos |

Ninguna necesita nada del kernel: se hacen con `Len`, `Mid$`, `Instr`, `Asc` y
los comandos de carpetas que ya existen. Meterlas en el compilador sería
hacerlo más grande sin ganar nada, y cada comando incorporado es un nombre que
ya nadie puede usar para una función suya.

Si alguna acaba usándose en cuatro o cinco programas, entonces sí merece ser
comando de verdad.

## Lo que el emulador no sabe ejecutar

No falta en el lenguaje, falta en `herramientas/emu/arm64.py`, y es peor de
localizar porque parece un fallo del programa. El emulador implementa las
instrucciones que hacen falta, no el juego entero de ARM64: cuando aparece una
nueva, el programa muere con **«instrucción desconocida»** y un número.

Si eso pasa, el número es la instrucción: se decodifica y se añade. Así se
añadieron `ushr` y `scvtf` de registro escalar, que son las que construyen el
decimal de `Rnd()` — sin ellas, `Rnd()` sin argumento no se podía probar aquí
y llevaba desde siempre sin ejecutarse ni una vez.

---

## Cómo volver a comprobar esta lista

Una lista de pendientes que miente cuesta más que no tenerla, porque manda a
trabajar donde no hace falta. Conviene comprobarla **contra el compilador**, no
contra el recuerdo.

La forma de hacerlo es escribir el programa. Compila una llamada a cada comando
con el compilador del ordenador de desarrollo y mira lo que pasa: si acepta lo
que esta lista dice que rechaza, o al revés, la lista está vieja. Los tres
límites del lenguaje que se cerraron la última vez —los nueve argumentos, la
función anidada y el `Select` con cadenas— no salieron de leer el compilador,
sino de intentar usarlo.

Los nombres que el compilador conoce salen de su tabla de firmas:

```sh
awk '/static const nb_firma_t nb_firmas\[\] = \{/,/^\};/' nbc-selfhost/nb_codegen.c \
  | grep -o '{ "[^"]*"' | sed 's/{ "//;s/"//' | sort
```

Eso da los que llevan argumentos. Los que no llevan ninguno (`Pump`,
`PollEvent`, `ClientWidth`…) y las palabras del lenguaje (`Print`, `Cls`,
`Color`, `Text`…) no están en esa tabla: para esos, `grep '"Nombre"'
nbc-selfhost/nb_codegen.c`. Los nombres alternativos van en su propia tabla,
`nb_alias`.


