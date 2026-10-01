# Referencia de Nemo Basic (5)

*Las trampas del lenguaje, y dónde seguir.*

Viene de [REFERENCIA_NEMO_BASIC_4.md](REFERENCIA_NEMO_BASIC_4.md).
# 13. Las trampas

Las que muerden de verdad, y cómo se reconocen.

**El punto y coma de `Print` se come lo que venga detrás.** `Print "a"; 1`
imprime solo `a`, sin avisar. Concatena con `+` y `Str$()`, o usa comas.

**El comentario es `;`, no `'`.** Un apóstrofo se interpreta como una llamada
a una función que no existe y da error de compilación. Al menos avisa.

**Cualquier variable no declarada `Local` es global.** Una función recursiva
que use una variable sin declarar como contador comparte ese contador con
todas sus llamadas y no termina nunca. El compilador avisa cuando una función
usa una variable que también vive fuera; ese aviso hay que leerlo.

**`/` entre enteros trunca.** `7 / 2` es 3, no 3,5.

**`Val` también trunca.** `Val("21.5")` es 21. Para leer un decimal de un
texto —de la red, de un archivo, de un campo— es `Val#`. `Str$` sí escribe los
decimales, así que con `Val` el viaje de ida y vuelta se pierde por un lado.

**`Mid$` cuenta desde 1 y los arrays desde 0.** No es un descuido, es la
herencia de Basic, pero se confunde.

**`Sqr`, `Floor` y `Ceil` devuelven decimal.** `Sqr(16)` es `4.000000`.

**`CreateMenu` lleva el padre el tercero.** Llamarla al revés no da error: el
kernel recibe un número donde espera un puntero, devuelve −1, y no aparece
ninguna barra de menús.

**Un menú no llega por el evento de control**, sino como evento de menú con la
etiqueta en la fuente del evento.

**Solo hay ocho huecos de archivo.** Abrir sin cerrar los agota y a partir de
ahí todo falla. `CloseFile` no es opcional.

**Solo hay 64 huecos de imagen**, de 1024×1024 como mucho. Una carpeta con cien
`.nimg` no cabe si cada miniatura ocupa el suyo: se monta un mosaico en una
sola imagen.

**El planificador es cooperativo.** Un bucle sin `Pump()` ni `WaitEvent()` se
queda el turno y para el sistema entero.

**`MilliSecs()` salta de 10 en 10 ms.** Para medir algo corto, `MicroSecs()`.

**Una tabla estática con punteros a texto no funciona en un `.pro`.** Los
`.pro` son binarios planos que el cargador no reubica, así que una tabla de
punteros conserva direcciones de enlace y leerla da un Data Abort. Se reconoce
porque el `FAR_EL1` del fallo es una dirección pequeña.

**Una `Function` no puede llevar otra dentro**, ni más de ocho parámetros.
Las dos cosas dan error al compilar; antes la función anidada se aceptaba y
desaparecía, y el noveno argumento llegaba como cero.

**`lista.P(i)\campo` no se analiza.** Los arrays de `Type` funcionan, pero para
tocar un campo hay que pasar por una variable: `p.P = lista.P(i)`, y luego
`p\campo`.

**`ImageBuffer` recibe el identificador tal cual.** El `+100000` es cómo el
kernel distingue un lienzo de una imagen, y lo pone `CanvasBuffer`, no tú.

**`UdpFrom$` y `UdpFromPort` hablan del último datagrama LEÍDO**, no de si ha
llegado algo. Después de leer el primero se quedan ahí para siempre, así que
usarlos como condición de un bucle que vacía la cola hace que el bucle no
termine nunca y el programa se cuelgue. Lo que dice si queda algo es
`UdpPending`.

**`UdpSend` devuelve −3 la primera vez que se le habla a una máquina nueva.**
No es un error: falta resolver su MAC por ARP y ese datagrama no ha salido. O
se reintenta, o —en un juego— se ignora y el siguiente sale unos milisegundos
después.

**Un datagrama vacío no es "no ha llegado nada".** `UdpRecv$` devuelve cadena
vacía en los dos casos. El que distingue es `UdpPending`.

**Solo hay UNA petición HTTP en todo el sistema.** `HttpGet` y `HttpPost`
devuelven 0 si hay otra en marcha, y esa otra puede ser de otro programa
abierto. Hay que mirar lo que devuelven.

---

# 14. Dónde sigue

Todas están en esta misma carpeta, y [INDICE_MANUALES.md](INDICE_MANUALES.md) las lista.

- [GUIA_NEMO_BASIC.md](GUIA_NEMO_BASIC.md) — la introducción: primer programa,
  cómo compilar desde el IDE, la shell o el explorador, y qué hacer cuando hay
  un error.
- [GUIA_AMBITO_NEMO_BASIC.md](GUIA_AMBITO_NEMO_BASIC.md) — el ámbito de las variables, y por qué
  `Local` deja de ser opcional en cuanto hay recursión.
- [GUIA_IMAGENES_NIMG.md](GUIA_IMAGENES_NIMG.md) — el formato `.nimg` por dentro: cabecera,
  píxeles, transparencia por color clave, hojas de celdas y el espejo.
- [GUIA_ARONNAX_NEMO_OS.md](GUIA_ARONNAX_NEMO_OS.md) — el diseñador visual: colocar controles
  con el ratón y saltar al código.
- [GUIA_GPIO_NEMO_OS.md](GUIA_GPIO_NEMO_OS.md) — los pines, con esquemas de conexión.
- [GUIA_RED_NEMO_OS.md](GUIA_RED_NEMO_OS.md) — la red por dentro y por fuera: UDP,
  HTTP, difusión, y los programas de ejemplo de `otros programas/red`.

Y hay una biblioteca de utilidades escrita en Nemo Basic, en
`otros programas/utiles/utiles.nb`, que se usa con `Include "utiles.nb"`:
partir y juntar cadenas (`Split`, `Join$`), separar una ruta en carpeta,
nombre y extensión (`StripDir$`, `ExtractDir$`, `StripExt$`, `ExtractExt$`),
clasificar caracteres (`IsDigit`, `IsAlpha`, `IsSpace`), listar una carpeta
(`ListDir`) y detectar el doble clic (`GadgetDoubleClicked`).

Y para ver código de verdad: `arkanoid`, `buscaminas` y `tetris`, y el juego
del capitán Nemo, que es el programa más grande escrito en Nemo Basic.
