# Guía de programación Lua para Nemo OS

Todo lo que necesita saber quien vaya a escribir programas para Nemo OS
en Lua: cómo se ejecuta un script, qué ofrece el lenguaje dentro del
sistema, cómo se habla con el kernel, y la referencia completa de las
241 syscalls — generada directamente de `src/syscall.h`, no de memoria.

---

## 1. Cómo se ejecuta un programa

Un programa Lua es un archivo de texto `.lua`, normalmente en la raíz
de NemoFS (o en cualquier carpeta de tus documentos). Lo ejecuta el
intérprete, que vive en `/PROGRAMAS/LUA.PRO`:

    C:/ run LUA.PRO hola.lua

o directamente, ya que el kernel redirige cualquier `.lua` a
`LUA.PRO` automáticamente (el mismo mecanismo de `run script.lua`
que usa la shell y el doble clic del explorador):

    C:/ run hola.lua

**Estructura de carpetas del sistema** — todo esto lo instala el
propio kernel al arrancar (va embebido en `kernel8.img`, ver
`embedded_lua.c`), no hay que copiar nada a mano:

    /
    ├── PROGRAMAS/
    │   └── LUA.PRO              -- el interprete en si
    ├── SISTEMA/
    │   ├── nemo_gui.lua         -- ventanas, gadgets, eventos, teclado, menus
    │   ├── nemo_archivos.lua    -- archivos y carpetas, las dos unidades
    │   └── nemo_sistema.lua     -- tareas, disco, memoria
    ├── ACCESORIOS/
    │   ├── shell.lua, editor.lua, calculadora.lua, gestor_tareas.lua,
    │   └── monitor_sistema.lua, reloj.lua, notas.lua, visor_imagenes.lua
    └── tu_programa.lua          -- tus propios scripts, donde quieras

`require("nemo_gui")` encuentra la librería sola dentro de `SISTEMA`.
`LUA.PRO` se busca siempre en `PROGRAMAS`. Y un `.lua` que no esté en
la carpeta actual se busca en `ACCESORIOS` — así `run reloj.lua`
funciona desde cualquier sitio, igual que un `.pro` de `PROGRAMAS`.

Como se sobreescriben en cada arranque, **no edites los archivos de
`SISTEMA` ni `ACCESORIOS` desde Nemo OS**: el cambio se perdería al
reiniciar. Edita `lua/ejemplos/*.lua` en el proyecto y recompila el
kernel. Tus propios scripts (fuera de esas dos carpetas) no se tocan.

**Si acabas de mover un módulo a `SISTEMA`** y el primer programa que
lo usa falla: reinicia Nemo OS antes de investigar más — ver la
trampa 9 en la sección 9.

El intérprete compila el script en memoria y lo ejecuta dentro de la
**tarea** que el kernel le ha dado (cada programa tiene una, con su
propia ventana, 16 MB de memoria y planificación cooperativa). Cuando
el script termina (o falla con un error), el intérprete vuelve al
kernel y la tarea desaparece.

Un error de Lua se imprime en la consola con su traza:

    lua: hola.lua:3: attempt to index a nil value (global 'x')
    stack traceback: ...

No hay paso de compilación: editas el `.lua` en el IDE, lo ejecutas,
lo corriges, lo vuelves a ejecutar.

## 2. El modelo mental: cooperación

Nemo OS usa los **cuatro núcleos** de la Raspberry Pi: el núcleo 0 se
dedica al sistema (ventanas, teclado, ratón, red, pantalla) y los
programas corren en los núcleos 1 a 3, varios a la vez de verdad.

Cada programa sigue siendo **cooperativo**: nadie lo interrumpe, y es él
quien cede su turno. La regla:

> **Todo bucle que espere algo (eventos, tiempo, teclado) llama a
> `nemo.pump()` en cada vuelta.**

`nemo.pump()` (syscall 14) **duerme hasta el siguiente latido del reloj**
(100 por segundo). Es el ritmo natural de un programa: la pantalla se
compone justo en cada latido, así que un bucle con `pump` dibuja su
fotograma, lo entrega y duerme hasta que toca el siguiente. Los
envoltorios de `nemo_gui.lua` ya lo hacen por ti; si escribes tu propio
bucle, no lo olvides.

Un bucle que no cede nunca ya no congela el sistema, pero deja **su
núcleo ocupado al cien por cien** y su ventana no se actualiza: lo que
se dibuja solo aparece en pantalla cuando el programa cede el turno.

## 3. Lo que Lua ofrece dentro de Nemo OS

Es Lua 5.5 completo: enteros de 64 bits, flotantes de doble precisión,
cadenas, tablas, metatablas, clausuras, corrutinas, `goto`,
`string.format`, patrones, `utf8`, `pcall`/`error`, `load`, `require`.

Librerías estándar disponibles: `string`, `table`, `math`, `utf8`,
`coroutine`, `debug`, y las funciones base (`print`, `tostring`,
`tonumber`, `pairs`, `pcall`, `require`, `load`, `collectgarbage`...).

**No están** `io` ni `os`: son librerías POSIX (archivos con `FILE*`,
`os.execute`, variables de entorno) que no tienen sentido aquí. Sus
equivalentes son la librería `nemo` y las syscalls de archivos.

- `print()` escribe en la consola de la shell que lanzó el programa.
- `require("modulo")` busca `modulo.lua` en la raíz de NemoFS.
- Memoria: 4 MB para el script (cada aplicación de Lua ocupa 8 MB en
  total, con el intérprete y la pila). Las imágenes no cuentan: sus
  píxeles viven en el kernel. `collectgarbage("count")` dice cuánto se
  usa; si se agota, da un error limpio `not enough memory`.
- Pila de Lua acotada a 60.000 valores: una recursión infinita falla
  con `stack overflow` limpio en vez de agotar la memoria.

## 4. La librería `nemo`

Es el puente con el kernel. Deliberadamente pequeña en C: casi todo lo
demás se construye en Lua encima.

| Función | Qué hace |
|---|---|
| `nemo.syscall(num, a0, a1, a2, a3, a4)` | Llama a cualquier syscall del kernel. Devuelve el entero de `x0`. |
| `nemo.escribir(s)` | Escribe en la consola sin salto de línea (`print` lo añade). |
| `nemo.ticks()` | **Latidos** de 10 ms desde el arranque (`SYS_GET_TICKS`). Para milisegundos, `sistema.milisegundos()`. |
| `nemo.pump()` | Cede el procesador (`SYS_PUMP`). |
| `nemo.leer_archivo(ruta)` | Devuelve el contenido entero de un archivo como cadena, o `nil`. |
| `nemo.memoria()` | Bytes en uso del pool del intérprete. |
| `nemo.buffer(n)` | Reserva `n` bytes a cero para que una syscall escriba en ellos. |
| `nemo.direccion(buf)` | La dirección de ese búfer, para pasársela a la syscall. |
| `nemo.cadena(buf)` | La cadena C (hasta el primer `\0`) que la syscall dejó en el búfer. |
| `nemo.leer_i32(buf, off)` | Entero de 32 bits little-endian en el desplazamiento `off`. Da error si `off` se sale del búfer. |
| `nemo.escribir_i32(buf, off, v)` | El simétrico: mete un entero de 32 bits little-endian. Para preparar datos que la syscall va a **leer**. |

## 3b. Trabajar con filas de píxeles

Una llamada al sistema cuesta **3,5 µs** medidos en la Raspberry Pi 4, y
un píxel cuesta **4 ns**. Cualquier cosa que trabaje píxel a píxel paga
casi 900 veces más peaje que trabajo. El relleno por inundación del
Pintor era el caso extremo: unos 4,2 millones de llamadas en un lienzo de
1024×1024, cerca de 15 segundos.

Para eso están las cuatro funciones de fila. Trabajan sobre **imágenes**
(no sobre la ventana: ahí ya hay caminos rápidos como `dibujar_trozo`), y
reciben la imagen como argumento, así que no tocan ningún estado global
de dibujo.

| Función | Qué hace |
|---|---|
| `gui.leer_fila(img, x, y, n, buf)` | Lee `n` píxeles desde `(x, y)` en `buf` (pedido con `nemo.buffer(n * 4)`). |
| `gui.escribir_fila(img, x, y, n, buf)` | Escribe en la imagen los `n` píxeles que hay en `buf`. |
| `gui.rellenar_fila(img, x, y, n, color)` | Pinta `n` píxeles de un solo color. Sin búfer. |
| `gui.tira_fila(img, x, y, max, color, distinto)` | Longitud de la tira de píxeles que desde `(x, y)` **son** de ese color (o que **no** lo son, con `distinto = true`). Con `max` negativo, hacia la izquierda. |

Las cuatro devuelven **cuántos píxeles se han tratado de verdad**, ya
recortados al borde de la imagen, o `nil` si los argumentos no valen. La
diferencia importa: `0` es «no quedaba nada dentro de la imagen» y `nil`
es «te has equivocado». Cada píxel es un entero `0xRRGGBB`, el mismo
valor que devuelve `ReadPixel`.

Leer una fila y recorrerla entera:

```lua
local gui = require("nemo_gui")
local ancho, alto = gui.tamano_imagen(img)
local buf = nemo.buffer(ancho * 4)
local n = gui.leer_fila(img, 0, y, ancho, buf)
for i = 0, n - 1 do
  local color = nemo.leer_i32(buf, i * 4)
  -- ...
end
```

**Cuidado con recorrer desde Lua.** Medido: una llamada a
`nemo.leer_i32` cuesta unos 210 ns en x86-64, así que en la Pi queda en
el mismo orden que la propia syscall. Traer la fila y recorrerla en Lua
**no arregla nada** si lo que buscas es velocidad: cambia un peaje por
otro. Para buscar o medir tramos usa `gui.tira_fila`, que recorre dentro
del kernel a 4 ns el píxel.

`tira_fila` es la pieza que hace barato un relleno por inundación: los
dos extremos de un tramo son dos llamadas, y localizar los tramos de las
filas de arriba y abajo son dos más por tramo. El coste pasa a crecer con
el **perímetro** en vez de con el área. Medido en el Pintor, lienzo de un
solo color:

| Lienzo | Antes | Ahora | Llamadas por píxel |
|---|---|---|---|
| 64×64 | 20.225 | 446 | 0,1089 |
| 256×256 | 326.657 | 1.790 | 0,0273 |
| 1024×1024 | 4.193.279 | 7.166 | 0,0068 |

En la Pi 4 eso son **~14,7 s antes y ~25 ms ahora**. El código está en
`cubo()`, en `lua/ejemplos/pintor.lua`.

## 4b. Novedades de `nemo_gui` y `nemo_sistema` (septiembre de 2026)

Funciones que el kernel ya tenía y que ahora los módulos exponen con
nombre. Ejemplo que las usa casi todas: `dibujo.lua`, en Accesorios.

**Ratón** (`gui = require("nemo_gui")`)

| Función | Qué hace |
|---|---|
| `gui.raton()` | `x, y, botones` dentro de la ventana (botones: 1 izquierdo, 2 derecho, 4 central, sumados). `nil` si el ratón está fuera o la ventana no tiene el foco. |
| `gui.clic(boton)` | `true` si se pulsó ese botón (1 por defecto) desde la última vez. Consume el aviso. |

**Dibujo**

| Función | Qué hace |
|---|---|
| `gui.linea(x0, y0, x1, y1, color)` | Línea. |
| `gui.ovalo(x, y, ancho, alto, color)` | Elipse rellena. |
| `gui.punto(x, y, color)` | Un píxel. |
| `gui.limpiar(color)` | Rellena la ventana entera (negro si no se indica). |

**Eventos**: `gui.posicion_evento()` devuelve `x, y` del último evento
leído, por ejemplo un clic en un panel.

**Imágenes para sprites** (además de `cargar_imagen`, `dibujar_imagen`...)

| Función | Qué hace |
|---|---|
| `gui.crear_imagen(ancho, alto)` | Imagen vacía; devuelve su identificador. |
| `gui.transparente(img, color)` | Ese color pasa a ser transparente (el magenta `0xFF00FF`, por costumbre). |
| `gui.punto_agarre(img, x, y)` | Qué punto de la imagen cae en las coordenadas de `dibujar_imagen`. Con el centro, el sprite queda centrado. |
| `gui.copiar_imagen(img)` | Duplica una imagen. |
| `gui.guardar_imagen(img, nombre)` | La guarda en disco; `true` si fue bien. |

**Tiempo y consola** (`sistema = require("nemo_sistema")`)

| Función | Qué hace |
|---|---|
| `sistema.esperar(ms)` | Espera durmiendo: no gasta nada y deja el núcleo libre. Redondea hacia arriba a latidos de 10 ms. |
| `sistema.milisegundos()` | Milisegundos desde el arranque, de 10 en 10. |
| `sistema.leer_linea(max)` | Lee una línea del teclado con eco y retroceso, para scripts de consola. Devuelve el texto, o `nil` si el script no tiene consola. |

**Datos del equipo** (septiembre de 2026, syscalls 285-288)

Hasta ahora el sistema sabía cuánta RAM tiene (`memoria.c` la detecta al
arrancar para poder mapearla entera) y qué procesador es (`MIDR_EL1`),
pero no había forma de preguntárselo desde un programa: el monitor sólo
podía enseñar los repartos que hace el propio sistema. Estas cuatro lo
exponen, y son las que usa `monitor_sistema.lua`.

| Función | Qué hace |
|---|---|
| `sistema.cpu_nombre()` | Nombre del procesador, leído de `MIDR_EL1`: `"ARM Cortex-A72 (Raspberry Pi 4)"`, `"ARM Cortex-A53 (QEMU)"`. No está cableado: sale del registro, así que dice la verdad en las dos máquinas. |
| `sistema.cpu_datos()` | Devuelve **núcleos en marcha** y **MHz**. Ojo: `mhz` vale `0` cuando no se puede saber (en QEMU no hay mailbox del firmware al que preguntar), que **no** es lo mismo que ir a 0 MHz — escribe "desconocida", no "0 MHz". |
| `sistema.ram_fisica()` | RAM física de la placa, en bytes. La de verdad; no confundir con la reserva de tareas ni con el montón del núcleo, que son repartos hechos **dentro** de ésta. |
| `sistema.ram_nucleo()` | Lo que ocupa el núcleo cargado en memoria (código + datos + `.bss`), medido entre `_start` y `__bss_end`. Incluye los arrays estáticos grandes: el montón de 64 MB, la reserva de tareas y los lienzos de las ventanas viven en `.bss` y no en el archivo. |
| `sistema.encendido_segundos()` | Segundos desde el arranque. |
| `sistema.tarjeta_bytes()` | Tamaño físico de la tarjeta. |
| `sistema.particiones()` | Las entradas no vacías de la tabla de particiones: `{indice=, tipo=, inicio=, sectores=}`. |
| `sistema.pantalla()` | Resolución activa ahora mismo: ancho, alto. |

Las tres cifras de memoria contestan preguntas distintas y conviene no
mezclarlas: `ram_fisica()` es la RAM que hay; `memoria_de_tareas()` es la
reserva de la que se sirve a los programas; `heap_kernel()` es el montón
del que tira el propio núcleo con `kmalloc`. Sumar las dos últimas y
restarlas de la primera da la RAM que existe y que **nadie está usando
todavía** — que no es "libre" en el sentido de otros sistemas, porque
aquí no hay un asignador general que vaya a repartirla.

**Teclado y ratón, solo con foco.** Todas las funciones de teclado y de
ratón responden "nada pulsado" si la ventana del programa no está en
primer plano, sin consumir la tecla. Con dos programas abiertos, cada
uno recibe solo lo que va dirigido a él.

### Ventanas que cambian de tamaño (septiembre de 2026)

Una ventana se puede maximizar y restaurar. Un programa tiene que
recolocar lo suyo cuando pasa, o lo que quede fuera de su dibujo se verá
con el gris de fondo. El patrón, en cada vuelta del bucle:

```lua
local function colocar(w, h)
  gui.limpiar(gui.COLOR_VENTANA)                -- o el fondo propio
  gui.mover_gadget(lista, 10, 35, w - 20, h - 80)       -- crece con la ventana
  gui.mover_gadget(boton, 10, h - 40, w - 20, 30)       -- anclado abajo
  -- y repintar lo que el programa dibuje a mano, con w y h
end

gui.bucle(function(ev, fuente)
  local cambia, w, h = gui.tamano_cambiado()
  if cambia then colocar(w, h) end
  -- ...el resto del programa
end)
```

- `gui.tamano_ventana()` da el tamaño **actual** de la zona de dibujo,
  ya sin la barra de menús. La barra de título se pinta *por encima* de
  esa zona: no hay que restarla.
- `gui.tamano_cambiado()` es verdadero la primera vez que se llama y
  cada vez que el tamaño cambia; devuelve también el tamaño nuevo. Así
  la disposición inicial y las siguientes salen de la misma función.
- `gui.mover_gadget(id, x, y, [ancho, alto])` mueve un control y, si se
  dan, le cambia el tamaño. Borra su imagen vieja: los controles se
  pintan dentro del dibujo de la ventana, y si no, quedaría un fantasma.
  `gui.rect_gadget(id)` devuelve su posición y tamaño.

Si el programa dibuja cosas que no se deben perder al redimensionar
(como el cuaderno de dibujo), en lugar de limpiar todo pinta solo la
zona nueva.

### Botones de la ventana y fuentes (septiembre de 2026)

```lua
-- un reloj: sin maximizar ni minimizar
gui.crear_ventana("Reloj", 200, 150, 260, 130, { maximizar = false, minimizar = false })
-- un juego: sin ningun boton (y su propia salida, con Esc)
gui.crear_ventana("Juego", 0, 0, 640, 480, { maximizar = false, minimizar = false, cerrar = false })
gui.botones_ventana(true, true, true)   -- (maximizar, minimizar, cerrar) volver a ponerlos

gui.usar_fuente("sans", 12)       -- la de la interfaz
gui.usar_fuente("mono", 10)       -- para lo que se edita o va en columnas
```

Quien quite el botón de cerrar debe ofrecer su propia salida; si el
programa se colgara, se cierra desde el gestor de tareas.

Sin elegir fuente, `gui.texto` escribe con la 5×7 del sistema, en
mayúsculas. `gui.usar_fuente` carga la fuente la primera vez, la
recuerda y la activa. La **mono de 10** avanza 6 píxeles por carácter,
como la 5×7, así que un programa que cuente columnas no tiene que
cambiar sus cálculos; el alto de línea sí cambia: pídelo con
`gui.alto_fuente()` en lugar de suponerlo. Con la **sans**, cada letra
mide lo suyo: para saber lo que ocupa un texto, `gui.medir_texto(t)`.

### Todos los controles, menús, barra de herramientas y temporizador (septiembre de 2026)

`nemo_gui` tiene todos los controles del kernel, igual que Nemo Basic:

```lua
local c = gui.crear_casilla("Avisos", 20, 20, 140, 20)     -- gui.marcada(c), gui.marcar(c, true)
local o = gui.crear_opcion("Rojo", 20, 44, 80, 20)          -- al marcar una opcion, se desmarcan las demas
local d = gui.crear_deslizador(20, 70, 200, 20)             -- de 0 a 100; gui.valor_deslizador(d)
local p = gui.crear_progreso(20, 96, 200, 16)               -- gui.poner_progreso(p, 0.5)
local k = gui.crear_desplegable(240, 20, 150, 22)           -- sus lineas: gui.lista_anadir(k, "Uno")
local pe = gui.crear_pestanas(20, 120, 300, 24)              -- sus nombres, igual
local ct = gui.crear_caja_texto(20, 150, 300, 80)           -- gui.poner_caja_texto / gui.anadir_caja_texto
gui.mostrar(c, false); gui.activar(o, false); gui.dar_foco(campo)
gui.color_panel(panel, 40, 120, 200); gui.imagen_panel(panel, "ico32_info.nimg")

-- menus: EVENT_MENUACTION, con el tag de la entrada como dato
local barra_menus = gui.menu_raiz()
local archivo = gui.crear_menu("Archivo", 0, barra_menus)
local nuevo = gui.crear_menu("Nuevo", 1, archivo)
gui.crear_menu("", 0, archivo)                               -- una linea de separacion
local exportar = gui.crear_menu("Exportar", 2, archivo)
gui.crear_menu("Como PNG", 10, exportar)                     -- con una entrada de padre: un submenu
gui.marcar_menu(nuevo, true); gui.activar_menu(nuevo, false)

-- barra de herramientas: EVENT_GADGETACTION, con el numero del boton como dato
local barra = gui.crear_barra("tb_basica24.nimg")
gui.ayudas_barra(barra, { "Nuevo", "Abrir", "Guardar" })
gui.activar_boton_barra(barra, 8, false)

-- temporizador: EVENT_TIMERTICK
local t = gui.crear_temporizador(2)
gui.pausar_temporizador(t); gui.reanudar_temporizador(t)
```

Las tiras de iconos de DOCUMENTOS/IMAGENES y su orden están en `GUIA_ICONOS.md`.

Imágenes con fondo transparente, como las del Buscaminas:

```lua
local mina = gui.cargar_imagen("tr_mina.nimg")   -- en DOCUMENTOS/IMAGENES
gui.mascara_imagen(mina)                          -- el magenta pasa a transparente
gui.dibujar_imagen(mina, x, y)
gui.centro_imagen(mina, 16, 16)                   -- su punto de referencia, si quieres centrarla
```

Al mover un control con `gui.mover_gadget` ya no hay que borrar antes: lo hace el kernel.

El árbol, el lienzo y la rueda del ratón:

```lua
local a = gui.crear_arbol(10, 10, 180, 200)
local rama = gui.arbol_anadir("Colores", gui.arbol_raiz(a))
gui.arbol_anadir("Rojo", rama); gui.arbol_abrir(rama, true)
-- al elegir un nodo (EVENT_GADGETACTION del arbol): gui.leer_texto(gui.arbol_elegido(a))

local l = gui.crear_lienzo(210, 10, 230, 200)
gui.dibujar_en_lienzo(l)                 -- todo el dibujo va al lienzo, con sus coordenadas
gui.ovalo(65, 50, 100, 100, gui.rgb(210, 50, 45))
gui.dibujar_en_ventana()

local r = gui.rueda()                    -- cuanto se ha girado la rueda (positivo: hacia arriba)
```

### Pines GPIO: `nemo_gpio` (septiembre de 2026)

```lua
local gpio = require("nemo_gpio")
gpio.modo(17, gpio.SALIDA)      -- tambien gpio.ENTRADA, gpio.ARRIBA, gpio.ABAJO
gpio.escribir(17, 1)
local v = gpio.leer(4)          -- 1 o 0; si falla, nil y un mensaje
gpio.alternar(17)
gpio.pwm(18, 1000, 25)          -- PWM por hardware (pines 12, 13, 18, 19): 1 kHz al 25 %
gpio.servo(18, 90)              -- un servo, a 90 grados
gpio.i2c_buscar()               -- I2C (GPIO 2 y 3): que direcciones responden
gpio.i2c_leer_registro(0x76, 0xD0)   -- escribir el registro, leer su valor
gpio.mcp3008(0)                 -- SPI: una entrada analogica de un MCP3008, 0..1023
```

Pines del 2 al 27 (numeración BCM); el 14 y el 15 son la terminal. Todo
lo demás —patillas, montajes, límites eléctricos, el panel `gpio.lua`
de Accesorios— está en **GUIA_GPIO.md**, en DOCUMENTOS.

## 5. Cómo se llama a una syscall

Las syscalls reciben hasta cinco argumentos enteros de 64 bits
(`a0`..`a4`, en los registros `x0`..`x4`) y devuelven uno en `x0`. Desde
Lua, `nemo.syscall` traduce así:

- **Un número** se pasa tal cual.
- **Una cadena** se pasa como puntero a su texto (terminado en `\0`).
  Es válido mientras dura la llamada, que es lo que el kernel necesita.
- **`true`/`false`** valen 1/0. Un argumento omitido vale 0.

Convenciones que se repiten en toda la tabla:

**Valores empaquetados.** Muchas syscalls meten dos o más números en un
argumento o en el valor devuelto, con desplazamientos de bits:

```lua
-- ancho y alto de un gadget van juntos: (ancho<<16 | alto)
local id = nemo.syscall(100, "Pulsa", 20, 60, (120 << 16) | 32, 0)

-- la posicion del raton viene junta: (x<<32 | y<<8 | botones)
local v = nemo.syscall(34)
local x, y, botones = v >> 32, (v >> 8) & 0xFFFFFF, v & 0xFF

-- la fecha entera en un solo entero
local t = nemo.syscall(134)
local año, mes, dia = t >> 48, (t >> 40) & 0xFF, (t >> 32) & 0xFF
```

**Números con decimales.** El kernel no tiene coma flotante en su
interfaz: cuando una syscall pide un `double` (volumen de sonido,
progreso de una barra, ángulo de rotación), se pasan sus **bits
crudos** como entero. En Lua:

```lua
local function bits(x) return string.unpack("<i8", string.pack("<d", x)) end
nemo.syscall(162, barra, bits(0.75))   -- UPDATE_PROGBAR al 75%
```

**Búferes de salida.** Cuando el kernel tiene que devolver texto (el
nombre de un archivo, el contenido de un campo), le pasas un búfer y su
tamaño:

```lua
local buf = nemo.buffer(256)
nemo.syscall(106, campo, nemo.direccion(buf), 256)   -- GADGET_GET_TEXT
local texto = nemo.cadena(buf)
```

**Volúmenes.** Las syscalls de archivo llevan un argumento `volumen`:
`0` = NemoFS (el disco del sistema), `1` = FAT (la tarjeta SD). El
`inodo padre` `0` es la raíz.

**La ventana propia.** Las syscalls de dibujo y de gadgets NO reciben
la ventana: el kernel deduce cuál es por la tarea que llama. Cada
programa tiene exactamente una.

## 6. Eventos

Cuando has llamado a `SYS_CREATE_WINDOW` (40), la ventana entra en
"modo evento": los clics en gadgets, las entradas de menú, el
temporizador y la X de cerrar se encolan y el programa los lee. Solo
existen **cinco** tipos de evento (`src/gadgets.h`):

| Código | Nombre | Qué lo dispara | `fuente` | `datos` |
|---|---|---|---|---|
| `0x401` | `EVENT_GADGETACTION` | clic en botón, cambio en lista/campo | id del gadget | 0 (o valor del slider) |
| `0x802` | `EVENT_WINDOWSIZE` | la ventana cambió de tamaño | — | — |
| `0x803` | `EVENT_WINDOWCLOSE` | se pulsó la X | — | — |
| `0x1001` | `EVENT_MENUACTION` | se eligió una entrada de menú | id del menú | tag del menú |
| `0x4001` | `EVENT_TIMERTICK` | temporizador de `CREATE_TIMER` | — | — |

**No hay evento de teclado.** Las teclas se leen aparte, sondeando:
`SYS_READ_CHAR` (12) devuelve el siguiente carácter ASCII o 0;
`SYS_GET_KEY` (54) el siguiente código de tecla; `SYS_KEY_DOWN` (48)
dice si una tecla está pulsada ahora.

El patrón completo:

```lua
while true do
  nemo.pump()
  local ev = nemo.syscall(8)              -- POLL_EVENT
  if ev == 0x803 then break end           -- la X
  if ev ~= 0 then
    local info = nemo.syscall(9)          -- GET_EVENT_INFO: (fuente<<32 | datos)
    local fuente, datos = info >> 32, info & 0xFFFFFFFF
    -- ...
  end
  local c = nemo.syscall(12)              -- READ_CHAR: teclado, aparte
  if c ~= 0 then --[[ tecla c ]] end
end
```

`nemo_gui.bucle(manejador)` empaqueta exactamente esto.

## 7. Recetas

**Ventana con un botón y una etiqueta** (`ejemplos/contador.lua`):

```lua
local gui = require("nemo_gui")
gui.crear_ventana("Contador", 100, 100, 300, 160)
local etiqueta = gui.crear_etiqueta("Pulsaciones: 0", 20, 20, 260, 24)
local boton    = gui.crear_boton("Pulsa aqui", 20, 60, 120, 32)
local n = 0
gui.bucle(function(ev, fuente)
  if ev == gui.EVENT_GADGETACTION and fuente == boton then
    n = n + 1
    gui.poner_texto(etiqueta, "Pulsaciones: " .. n)
  end
end)
```

**Leer un archivo entero:**

```lua
local texto = nemo.leer_archivo("notas.txt")
if not texto then print("no existe") return end
for linea in texto:gmatch("[^\n]+") do print(linea) end
```

**Listar la carpeta raíz** (`SYS_FILE_LIST`, 23: cada entrada son 44
bytes — 32 de nombre, 4 de tipo, 4 de inodo, 4 de tamaño):

```lua
local MAX = 64
local buf = nemo.buffer(MAX * 44)
local n = nemo.syscall(23, 0, nemo.direccion(buf), MAX, 0)
-- las entradas se leen del bufer; nemo.cadena solo da la primera, asi
-- que para recorrerlas usa un envoltorio que copie con string.unpack
-- (ver nemo_archivos.lua cuando exista) -- o SYS_DIR_OPEN/DIR_NEXT (78/79):
local h = nemo.syscall(78, "/")
if h >= 0 then
  local nombre = nemo.buffer(64)
  while nemo.syscall(79, h, nemo.direccion(nombre), 64) > 0 do
    print(nemo.cadena(nombre))
  end
  nemo.syscall(80, h)
end
```

**Dibujar directamente** en la ventana propia:

```lua
local gui = require("nemo_gui")
gui.crear_ventana("Dibujo", 50, 50, 400, 300)
gui.rect(10, 10, 100, 60, gui.rgb(200, 40, 40))     -- SYS_DRAW_RECT
gui.texto(20, 30, "hola", gui.rgb(255, 255, 255))   -- SYS_DRAW_TEXT
gui.bucle(function() end)                           -- esperar a la X
```

**Esperar un tiempo:**

```lua
local sistema = require("nemo_sistema")
sistema.esperar(500)   -- medio segundo, durmiendo: no gasta nada
```

(Esta receta sumaba antes milisegundos a `nemo.ticks()`, que cuenta en
latidos de 10 ms: esperaba diez veces más de lo pedido, y además daba
vueltas en vez de dormir.)

**Lanzar otro programa:**

```lua
nemo.syscall(5, "EXPLORER.PRO", 0)     -- SYS_LAUNCH_PROGRAM
```

## 8. Errores y depuración

- `print()` va a la consola de la shell. `nemo.syscall(28, "texto")`
  (`SYS_DEBUG_LOG`) va SIEMPRE al puerto serie/UART, aunque el programa
  no tenga consola — útil desde un programa con ventana.
- Envuelve lo arriesgado en `pcall` y muestra el error en un gadget:
  `local ok, err = pcall(f); if not ok then gui.poner_texto(etiqueta, err) end`.
- Una syscall que devuelve `-1` (o un valor negativo como entero de 64
  bits) casi siempre significa "no existe" / "sin hueco". Compruébalo.
- Si el programa "no hace nada", lo más probable es un bucle sin
  `nemo.pump()`.

## 9. Trampas conocidas (todas reales, todas costaron horas)

1. **`SYS_FILE_OPEN` (20) crea el archivo si no existe.** Para leer,
   comprueba antes con `SYS_FILE_SIZE_BY_NAME` (81) — o usa
   `nemo.leer_archivo`, que ya lo hace.
2. **`SYS_CREATE_WINDOW` cambia la X.** Antes de llamarla, la X cierra
   la ventana sola; después, solo dispara `0x803` y el programa tiene
   que terminar. Si no lo compruebas, la ventana no se cierra.
3. **Máximo 8 botones "antiguos"** con `SYS_DEFINE_BUTTON` (36). Usa
   `SYS_CREATE_BUTTON` (100), que no tiene ese tope.
4. **`SYS_FONT_WIDTH` (195) da el ancho del glifo, no el avance.** Para
   calcular columnas de texto usa `SYS_FONT_CHAR_ADVANCE` (206).
5. **`SYS_DRAW_ICON` (32) escala solo por enteros** (1 = 24x24,
   2 = 48x48).
6. **`SYS_PLAY_SOUND` (228) bloquea** hasta que termina el sonido.
7. **`SYS_SCREEN_RES_SET_PENDING` (246) no aplica nada**: guarda la
   preferencia; hace falta reiniciar.
8. **Las cadenas son punteros efímeros**: no guardes el resultado de
   `nemo.direccion()` de una cadena Lua entre llamadas — el recolector
   puede moverla o liberarla. Para memoria estable usa `nemo.buffer`.
9. **Un módulo recién movido a `/SISTEMA` puede no encontrarse hasta
   reiniciar Nemo OS** — y de paso puede aparecer un archivo de 0
   bytes con su nombre en la raíz. Investigado a fondo en hardware:
   **no es un bug de lógica** — probando cada uno de los cinco
   "buscadores" de `require()` por separado, el que mira en `SISTEMA`
   siempre encuentra el módulo correctamente y devuelve una función
   válida. El fallo solo aparece a través del flujo completo de
   `require()`, y desaparece él solo después de un reinicio completo
   del sistema — todo apunta a algún estado de NemoFS en memoria (una
   caché de directorio, probablemente) que no se refresca solo la
   primera vez que se consulta una carpeta recién modificada. **La
   causa mecánica exacta sigue sin confirmarse** — esto documenta el
   síntoma y el arreglo que funciona, no una explicación completa.

   Si acabas de mover o crear un archivo dentro de `SISTEMA` y el
   primer programa que lo requiere falla (o aparece un `.lua` de 0
   bytes en la raíz que no recuerdas haber creado): borra ese archivo
   de 0 bytes y **reinicia Nemo OS**. Con eso, en todas las pruebas
   hechas hasta ahora, se resuelve.

---

## 10. Referencia completa de syscalls

La tabla de todas las syscalls del kernel, con sus números y argumentos,
está en un documento aparte, para que cada uno quepa holgado en la
memoria del visor: [GUIA_LUA_SYSCALLS.md](GUIA_LUA_SYSCALLS.md).
