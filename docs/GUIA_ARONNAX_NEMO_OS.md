# Aronnax: el diseñador visual de Nemo Basic

Aronnax debe su nombre al profesor Pierre Aronnax de *Veinte mil leguas de viaje submarino*, el científico que observa, dibuja y cataloga todo lo que ve por los ventanales del Nautilus. Con él se **dibuja** la ventana de un programa, colocando botones y demás controles con el ratón, y se escribe lo que hace cada uno. Aronnax genera el programa de Nemo Basic, lo compila y lo ejecuta.

Tiene además dos secciones que no tiene ningún otro diseñador. La primera es **Nautilus**, con controles conectados directamente a los pines GPIO de la Raspberry Pi: un LED en la pantalla que se enciende a la vez que el de verdad, un pulsador físico que avisa a tu programa, un deslizador que regula un PWM.

La segunda es **Telemetría**, para hablar con otras máquinas de la red: mandar una lectura a un servidor por HTTP, o cruzar mensajes con otro ordenador.

Aronnax está en **Accesorios**.

## Las zonas de la ventana

- **La barra de arriba**: Nuevo, Abrir y Guardar; las pestañas **Diseño** y **Código**; "Ver programa" (en la pestaña Código) y **Ejecutar**.
- **La paleta**, a la izquierda: los controles normales arriba, los del **Nautilus** en verde, y los de **Telemetría** en ámbar.
- **El formulario**, en el centro: la ventana que estás diseñando, con su rejilla de puntos.
- **El inspector**, a la derecha: las propiedades de lo que tengas seleccionado.
- **La línea de abajo**: qué acaba de pasar, y los errores.

## Diseñar

- **Colocar un control**: elígelo en la paleta y haz clic en el formulario.
- **Moverlo**: arrástralo. Se ajusta a la rejilla de 8 píxeles. También con las flechas, una vez seleccionado.
- **Cambiar su tamaño**: tira del asa azul de su esquina inferior derecha.
- **El tamaño de la ventana**: tira del asa blanca de la esquina del formulario.
- **Borrarlo**: Supr, o el botón "Quitar" del inspector.

**El inspector** se desplaza con la rueda del ratón cuando no cabe. Muestra las propiedades del control seleccionado; si haces clic en un hueco del formulario, las de la ventana. Haz clic en una propiedad para cambiarla: lo primero que escribas sustituye el valor (Retroceso, en cambio, edita sobre él), Enter acepta y Esc cancela. Las de sí/no, como Maximizar, se alternan con un clic.

El **nombre** de un control es el que usará tu código (`Boton1`, `Etiqueta2`...). Tiene que empezar por una letra, llevar solo letras, cifras o `_`, y no repetirse. Si lo cambias, Aronnax lo cambia también en tu código, funciones incluidas.

El **pin** de los controles del Nautilus cambia con cada clic al siguiente pin válido y **libre**: nunca te dejará dos controles en el mismo pin. El PWM solo existe en los pines 12, 13, 18 y 19, y solo ofrece esos. El canal del analógico va del 0 al 7.

La **caja de texto** tiene dos límites que conviene conocer antes de apoyar nada en ella, y que el inspector recuerda en su fila *Guarda*:

- Guarda **200 líneas de 128 letras**. Al llenarse descarta la más antigua, y parte sola las líneas largas guardando el salto. Si tu programa guarda lo que el usuario escriba ahí, guardará menos de lo que escribió.
- En **todo el sistema** solo hay **cuatro** cajas de texto, repartidas entre todos los programas abiertos: el IDE usa una, Timonel otra. Sin hueco, la caja aparece en pantalla y no guarda nada. Aronnax te avisa desde la primera, y no te deja pasar de cuatro en un mismo programa.

Para un editor de verdad, lo que toca es llevar el texto en tu propio programa y leer el teclado con `ReadChar()`.

## Dos ejemplos en DOCUMENTOS

- **`panel_nautilus.anx`**: un panel de GPIO con LED, PWM, pulsador y sensor.
- **`editor_texto.anx`**: un editor de texto sencillo, con menús (y un submenú de plantillas), barra de herramientas, una caja de texto que se estira con la ventana, y Abrir y Guardar de verdad con los comandos de archivos. Buen sitio para ver cómo encaja todo: la caja de texto se escribe a mano, haz clic dentro y teclea.

Se abren con **Abrir** y se ejecutan con **Ejecutar**.

## Varios controles a la vez

| Cómo | Qué hace |
|---|---|
| **Mayúsculas + clic** | añade (o quita) ese control del grupo |
| **arrastrar sobre el fondo** | un recuadro: entra lo que quede dentro del todo |
| arrastrar uno del grupo | los mueve todos |
| flechas | los mueven todos, de 8 en 8 píxeles |
| Supr | los quita todos |

Con dos o más elegidos, el inspector muestra botones para **alinear** (izquierda, derecha, arriba, abajo), **igualar tamaño** (al del último elegido, que es el que lleva el asa) y **repartir** el espacio a lo ancho o a lo alto (con tres o más). Todo se deshace con Ctrl+Z.

## Plantillas

El botón **Plantillas** de la barra abre una lista con seis proyectos que **ya funcionan**: se eligen, se pulsa Ejecutar y se ven en marcha. Luego se cambian a gusto.

| Plantilla | Qué trae |
|---|---|
| Formulario | etiquetas, campos, una casilla y los botones Aceptar y Cancelar anclados a la esquina |
| Lista y detalle | una lista que crece con la ventana, con Añadir y Quitar |
| Menús y barra | menús Archivo y Ayuda, barra de herramientas y una caja de texto |
| Panel del Nautilus | LED, botón de encendido, PWM, pulsador y sensor, con temporizador |
| Telemetría | manda una lectura por HTTP y cruza mensajes con la red por UDP |
| Lienzo animado | una pelota que rebota, con **decimales** y gravedad: pierde fuerza en cada bote |

Si tienes cambios sin guardar, el primer clic en Plantillas avisa, como Nuevo y Abrir.

**Un aviso que vale oro:** dentro de una función, una variable que no declares con **`Local`** es **global**. Si dos funciones tuyas usan `i` en un bucle, se pisan. Declara `Local i` en cada una.

## Telemetría: hablar con otras máquinas

Los comandos de red de Nemo Basic se pueden escribir a mano en el código de cualquier botón. Estos dos controles no existen para eso: existen porque se encargan del **trabajo asíncrono**, que es donde está lo que se hace mal.

Una petición de red no termina cuando la pides: termina más tarde. Y en Nemo OS un programa no puede quedarse esperando —el planificador es cooperativo, así que esperar dentro del sistema para a la máquina entera, no a tu programa—. Así que hay que arrancar la operación, seguir con lo tuyo, y volver a mirar en cada vuelta. Eso es lo que escriben estos dos por ti.

### Sensor: mandar datos por HTTP

Colócalo, y en el inspector pon el **servidor**, el **puerto**, la **ruta** y **cada cuántos milisegundos** quieres mandar. Con el periodo a 0 solo manda cuando tú se lo digas.

En el formulario se ve a dónde manda. En el programa es una etiqueta que dice cómo va: `enviando...`, `ok 200`, o el motivo del fallo.

| Lo que escribes tú | Cuándo |
|---|---|
| `Function Sensor1_Valor$()` | Cuando toca mandar. Devuelve lo que se manda. Hace falta si pones periodo. |
| `Function Sensor1_Respuesta(codigo, cuerpo$)` | Cuando el servidor contesta. `codigo` es 200, 404… |

| Lo que puedes llamar | Qué hace |
|---|---|
| `Sensor1_Manda(datos$)` | Manda ahora. Devuelve 1 si salió, 0 si había otra petición en marcha. |

**En todo Nemo OS solo puede haber una petición HTTP a la vez.** Por eso `Sensor1_Manda` devuelve 0 en vez de fingir que la mandó, y por eso dos sensores se turnan: con periodos cortos se pierden lecturas, y Aronnax te avisa si pones más de uno.

### Mensaje: cruzar mensajes por UDP

Colócalo y pon el **puerto**. En el arranque abre el socket; si no puede, lo dice en su etiqueta —un socket que no abrió y un programa que no recibe nada se parecen demasiado—.

| Lo que escribes tú | Cuándo |
|---|---|
| `Function Mensaje1_Recibido(texto$, de$, puerto)` | Por cada mensaje que llega. `de$` es la dirección de **ese** mensaje. |

| Lo que puedes llamar | Qué hace |
|---|---|
| `Mensaje1_Manda(a$, puerto, texto$)` | A una máquina concreta. Reintenta mientras se resuelve su dirección física. |
| `Mensaje1_Grita(texto$)` | A **toda la red**, sin saber quién hay. Así se encuentran dos máquinas sin teclear ninguna dirección. |
| `Mensaje1_Responde(texto$)` | A quien mandó el mensaje que estás atendiendo. Solo vale dentro de `_Recibido`. |

**`Mensaje1_Responde` solo vale ahí dentro**, y no es un capricho: el remitente es el del último mensaje leído, y en cuanto se lee otro cambia. Guardarte la dirección "para luego" es como se acaba contestando a quien no era — y eso no da ningún error, solo hace que el otro vea cosas raras.

Un mensaje a toda la red **te vuelve a ti también**. Si no quieres atenderte a ti mismo, compara con `NetIp$()`.

### Un detalle de UDP que conviene saber

UDP no garantiza nada: un mensaje puede perderse y el siguiente llegar igual. Eso es lo que lo hace bueno para un juego —una posición perdida no importa, la siguiente llega enseguida— y malo para algo que tiene que llegar sí o sí. Para eso está el Sensor, que va por HTTP.

La plantilla **Telemetría** junta los dos y funciona tal cual.

## Ayuda mientras escribes

En la pestaña **Código**, al escribir dos letras o más aparece una lista con los nombres que encajan: tus **controles**, tus **funciones** y las **órdenes de Nemo Basic** (170, con sus argumentos), incluidas las de teclado, ratón e imágenes.

| Tecla | Qué hace |
|---|---|
| ↑ / ↓ | elegir en la lista |
| Tab o Intro | completar el nombre elegido |
| Esc | cerrar la lista |

Al completar, y mientras escribes una orden, la línea de abajo enseña **su firma**: `SetGadgetText id, texto$`. Así no hace falta ir a la guía a cada momento.

## Anclajes: ventanas que se adaptan

En el inspector, **Anclaje** dice qué hace cada control cuando la ventana cambia de tamaño. Cada clic pasa al siguiente:

| Anclaje | Qué hace |
|---|---|
| `fijo` | nada (lo de siempre) |
| `derecha` / `abajo` / `derecha-abajo` | se mueve con ese borde: para un botón de Aceptar en la esquina |
| `ancho` / `alto` / `ancho-alto` | se estira con la ventana: para una lista o una caja de texto |

Aronnax genera una función `Aronnax_Recolocar()` que los aplica al abrirse la ventana y cada vez que cambia de tamaño. Los controles fijos no aparecen en ella.

## Deshacer, copiar y pegar

| Atajo (Ctrl, o Cmd en el Mac) | Qué hace |
|---|---|
| Ctrl+Z / Ctrl+Y | deshacer / rehacer (también con los botones de la barra); hasta 100 pasos |
| Ctrl+C / Ctrl+X | copiar / cortar el control elegido |
| Ctrl+V | pegarlo, un poco desplazado y con nombre nuevo |
| Ctrl+D | duplicarlo |

Deshacer vale para todo: colocar, mover, cambiar el tamaño, borrar, el inspector, los menús y el código. Arrastrar un control cuenta como un solo paso, y escribir seguido en una línea del código, también. Un control del Nautilus pegado recibe el siguiente pin libre, porque dos controles no pueden compartir pin.

**Cambios sin guardar.** Si pulsas Nuevo, Abrir o la X de la ventana con cambios sin guardar, Aronnax avisa en la línea de abajo; un segundo clic confirma que quieres descartarlos.

## Escribir el código

**Haz doble clic en un control** (o clic en su fila "Evento" del inspector): Aronnax cambia a la pestaña Código, con el cursor dentro de la función de ese control, que crea si no existía. Esa función se llama sola cuando pasa algo con el control:

| Control | Su función | Cuándo se llama, y con qué |
|---|---|---|
| Botón | `Boton1_Click()` | al pulsarlo |
| Casilla, Opción | `Casilla1_Click()` | al marcarla o desmarcarla (`ButtonState(Casilla1)` dice cómo quedó) |
| Campo de texto | `Campo1_Cambia(valor)` | al pulsar Enter en él (`GadgetText$(Campo1)` da su texto) |
| Lista, Desplegable, Pestañas | `Lista1_Cambia(valor)` | al elegir una línea: `valor` es cuál, desde 0 |
| Deslizador | `Deslizador1_Cambia(valor)` | al moverlo: `valor` va de 0 a 100 |
| Interruptor | `Interruptor1_Click()` | al pulsarlo; su pin ya se ha puesto a 1 o a 0 |
| Pulsador | `Pulsador1_Cambia(valor)` | al pulsar (1) o soltar (0) el botón de verdad |
| PWM | `Pwm1_Cambia(valor)` | al moverlo; el PWM de su pin ya ha cambiado |
| Analógico | `Analogico1_Cambia(valor)` | cuando cambia la lectura: de 0 a 1023 |
| Árbol | `Arbol1_Cambia(valor)` | al elegir un nodo: `valor` es el nodo (su texto: `GadgetText$(valor)`) |
| Lienzo | `Lienzo1_Dibuja()` | lo que se pinta en él, al abrirse la ventana; `Lienzo1_Redibuja()` lo vuelve a pintar |
| Sensor | `Sensor1_Respuesta(codigo, cuerpo$)` | cuando el servidor contesta; y `Sensor1_Valor$()`, cuando toca mandar |
| Mensaje | `Mensaje1_Recibido(texto$, de$, puerto)` | por cada mensaje que llega de la red |

Etiqueta, Progreso, Caja de texto, Panel, Imagen y LED no tienen función propia (el Lienzo tiene `Dibuja`, que no es un evento: dentro de ella, todo lo que dibujes va al lienzo, con sus coordenadas): se manejan desde las de los demás. Un doble clic en uno de ellos te lleva a la primera línea de tu código que lo usa; si todavía no lo usa, Aronnax te dice cómo hacerlo. Para un **LED**, Aronnax escribe una función de ayuda: `Led1_Pon(1)` lo enciende (el pin y el de la pantalla) y `Led1_Pon(0)` lo apaga.

Para los dos de **Telemetría**, Aronnax escribe varias funciones de ayuda —`Sensor1_Manda`, `Mensaje1_Manda`, `Mensaje1_Grita`, `Mensaje1_Responde`—; la fila *Funciones* del inspector las recuerda, y la sección Telemetría explica cada una.

Dos funciones más, si las escribes:

- `Function Ventana_Abre()`: se llama una vez, al abrirse la ventana, con todos los controles ya creados. Es el sitio para rellenar listas o poner valores iniciales.
- Las variables que deban recordar algo entre llamadas se declaran con `Global`, fuera de las funciones: `Global encendido`. Aronnax las sube al principio del programa.
- Aronnax ya declara con `Global` **cada control que colocas**, además de `Barra`, `Temporizador` y las variables de los anclajes. Por eso los programas que genera no producen el aviso de "variable compartida sin declarar" que `nbc` da  (ver `GUIA_AMBITO.md`): si te sale uno, es de tu código, no de la parte generada.
- `Include "utiles.nb"` en tu código pone a tu disposición las funciones de otro archivo (en la carpeta del proyecto, la raíz, DOCUMENTOS o SISTEMA). Aronnax también lo sube al principio. Si hay un error en ese archivo, Aronnax te dice cuál y en qué línea.

**El editor** tiene números de línea y colores. Enter conserva la sangría, Tab mete dos espacios, y se mueve con las flechas, Inicio, Fin, RePág y AvPág, o haciendo clic.

**"Ver programa"** muestra el programa entero que genera Aronnax: en gris, la parte que escribe él (crear la ventana y los controles, preparar los pines y el bucle que reparte los eventos), y después tu código. No es magia: es Nemo Basic normal, y se puede leer para aprender cómo funciona.

## Imágenes

El control **Imagen** muestra una imagen NIMG. Nace con `ico32_imagen.nimg`; en el inspector, **Archivo** abre el diálogo para elegir otra, y el control toma su tamaño. Si lo haces más grande que la imagen, se repite en mosaico.

Al ejecutarse, el programa busca las imágenes **en la raíz, en DOCUMENTOS y en DOCUMENTOS/IMAGENES**: si eliges una de otra carpeta, Aronnax te lo dice. También se puede dar la ruta entera desde el código — `SetPanelImage Imagen1, "JUEGOS/NEMO/nave.nimg"` — con `/` o `\` y una barra inicial opcional que solo significa "desde la raíz". En DOCUMENTOS hay 49 iconos de 32 píxeles listos para usar (`GUIA_ICONOS.md`), y `herramientas/nimg/nimg_convert.py` convierte tus PNG y JPEG. Para cambiar la imagen desde el código: `SetPanelImage Imagen1, "otra.nimg"`.

## Menús, barra de herramientas y temporizador

Con la ventana elegida en el inspector (clic en un hueco del formulario):

- **Barra**: cada clic pasa a la siguiente tira de iconos de DOCUMENTOS/IMAGENES (la básica, archivo, edición, ejecutar, navegar, Nautilus, avisos, varios) o a ninguna. En el formulario se ve con sus iconos. Su función es `Barra_Click(boton)`, donde `boton` es el número del botón pulsado, desde 0; la lista de cada tira está en `GUIA_ICONOS.md`. Doble clic en la barra del formulario, o en la fila "Evento barra", lleva a ella. La barra ocupa lo alto de la ventana: coloca los controles por debajo.
- **Temporizador**: cuántas veces por segundo (0, ninguno). Su función es `Temporizador_Tick()`.
- **Menús**: con "+ Menú" se añade un menú a la barra, y con "+ Entrada" o "+ Separador", una entrada al menú elegido. Un clic en una fila la elige; otro clic, para cambiarle el texto. Cada entrada tiene un nombre que sale de su texto ("Guardar como" es `MenuGuardarcomo`) y una función, `MenuGuardarcomo_Click()`, a la que lleva un doble clic. Con ese nombre, tu código puede marcarla o desactivarla: `CheckMenu MenuGuardarcomo`.
- **Submenús**: con una entrada elegida, "+ Subentrada" le pone un submenú (se abre a su derecha al pasar el ratón). Con una subentrada elegida, "+ Entrada" y "+ Separador" van a ese mismo submenú. Una entrada con submenú no tiene función propia: pulsarla lo abre. Un nivel de submenú.

## Ejecutar

Si el programa falla al ejecutarse (una división entre cero, un índice fuera de rango...), Aronnax también te lleva a tu línea.


**Ejecutar** guarda el proyecto, compila el programa y lo lanza. Si hay un error, Aronnax te lleva a la pestaña Código, al sitio exacto de **tu** código donde está, marca la línea en rojo y explica el error abajo. La marca desaparece en cuanto la editas.

Al guardar se escriben dos archivos: el proyecto (`.anx`) y, a su lado, el programa (`.nb`), que también se puede abrir y compilar con el IDE.

## Un ejemplo: el Panel del Nautilus

En DOCUMENTOS está `panel_nautilus.anx`. Ábrelo con Abrir y pulsa Ejecutar. Tiene:

- un **LED** (GPIO 17) y un botón que lo enciende y lo apaga;
- un **PWM** (GPIO 18) que regula el brillo de otro LED, y lo dice en una etiqueta;
- un **pulsador** (GPIO 4) que hace lo mismo que el botón;
- un **sensor analógico** en el canal 0 de un MCP3008, cuya lectura se muestra.

Su código entero:

```basic
Global encendido

Function Ventana_Abre()
  SetSliderValue Pwm1, 50
  GpioPwm 18, 1000, 50 * 100     ; el LED del 18, a la mitad desde el principio
End Function

Function Boton1_Click()
  encendido = 1 - encendido
  Led1_Pon(encendido)
  If encendido
    SetGadgetText Boton1, "Apagar"
  Else
    SetGadgetText Boton1, "Encender"
  EndIf
End Function

Function Pwm1_Cambia(valor)
  SetGadgetText Etiqueta2, "Brillo: " + Str$(valor) + " %"
End Function

Function Pulsador1_Cambia(valor)
  If valor = 1 Then Boton1_Click()
End Function

Function Analogico1_Cambia(valor)
  SetGadgetText Etiqueta4, "Sensor: " + Str$(valor)
End Function
```

**En QEMU** funciona sin placa: el LED de la pantalla se enciende y en la terminal se ven los pines cambiar. El pulsador no se puede pulsar (no hay botón de verdad), y el sensor lee el MCP3008 simulado.

**En la Raspberry Pi**, las conexiones (numeración BCM):

- **LED en el GPIO 17** y **LED en el GPIO 18**: cada uno con una resistencia de unos 330 Ω, del pin a la pata larga del LED, y la pata corta a masa (GND).
- **Pulsador en el GPIO 4**: entre el pin y masa. No hace falta resistencia: Aronnax activa la interna a positivo, y pulsado lee 0 (tu función recibe 1).
- **MCP3008**: al SPI0 (CE0), con VDD y VREF a 3,3 V. La guía de GPIO lo explica en detalle.

## Los archivos de un proyecto

Un proyecto `.anx` es texto que se puede leer: una línea para la ventana, una por control, y después tu código tal cual.

```
; Aronnax 1
ventana titulo="Mi programa" x=100 y=100 ancho=400 alto=300 maximizar=1 minimizar=1 cerrar=1
control tipo="Boton" nombre="Boton1" x=40 y=40 ancho=90 alto=26 texto="Aceptar"
---- codigo ----
Function Boton1_Click()
  ...
End Function
```

## Lo que todavía no hace

- Cada proyecto es **una ventana**.
- Los submenús tienen un nivel: no hay submenús dentro de un submenú.
- Los **controles de un panel** no se mueven con él: el panel es solo un rectángulo de fondo.
