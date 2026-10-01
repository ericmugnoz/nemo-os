# Guía de HTML para Nemo OS

Nemo OS puede mostrar documentos HTML: páginas de ayuda, portadas,
pantallas de información, presentaciones sencillas — cualquier cosa que
se escriba mejor con etiquetas que con llamadas de dibujo. Esta guía
cubre cómo escribir esas páginas, qué parte de HTML y CSS entiende el
visor, cómo abrirlas, y dónde está cada pieza para quien quiera
ampliarlo.

Un aviso antes de nada: **esto no es un navegador**. No hay red por
debajo ni JavaScript. Es un visor de documentos locales, hecho a medida
del sistema, con el mismo espíritu que el resto de Nemo OS: lo que hace
falta, bien, y nada más.

---

## 1. Empezar en dos minutos

Crea un archivo de texto con extensión `.html`, en UTF-8:

```html
<!DOCTYPE html>
<html>
<head>
  <title>Mi primera página</title>
  <style>
    body { background-color: #f4f4f8; color: #202030 }
    h1 { color: navy; text-align: center }
  </style>
</head>
<body>
  <h1>Hola, Nemo OS</h1>
  <p>Esto es un <b>párrafo</b> con <i>cursiva</i>, <u>subrayado</u>
     y <code>código</code>. Con acentos: canción, año, ¿qué?</p>
  <ul>
    <li>Una lista</li>
    <li>con <a href="otra.html">un enlace</a> a otra página</li>
  </ul>
</body>
</html>
```

Cópialo a NemoFS (por ejemplo a `DOCUMENTOS`) y ábrelo de cualquiera
de estas formas:

- **Doble clic** en el archivo desde el explorador.
- Desde la shell: `run visor.lua mipagina.html` (busca en la raíz, en
  `MANUALES` y en `DOCUMENTOS`, por ese orden), o
  `run visor.lua 5:mipagina.html` si sabes el inodo de la carpeta.
- Desde la shell remota por red (`nc -v 169.254.103.188 2323   # la dirección que anuncia el arranque`):
  `run visor.lua mipagina.html`.

Hay dos páginas de ejemplo que llegan a `DOCUMENTOS` en cada arranque:
`bienvenida.html` (muestra todo lo que sabe hacer el visor) y
`ayuda_html.html` (la referencia rápida). Están enlazadas entre sí, y
son buen punto de partida para copiar.

---

## 2. Controles del visor

| Acción | Cómo |
|---|---|
| Desplazarse | Rueda del ratón, o flechas arriba/abajo (3 líneas por paso) |
| Seguir un enlace | Clic sobre él |
| Volver a la página anterior | Retroceso |
| Zoom | `+` para agrandar, `-` para reducir (1x, 2x, 3x; remaqueta todo) |
| Cerrar | Esc, o la X de la ventana |

Al redimensionar la ventana, el texto se vuelve a ajustar al nuevo
ancho. La barra de la derecha indica la posición en el documento.

Si un enlace apunta a un archivo que no existe, el visor lo dice en
rojo y Retroceso vuelve al documento.

---

## 3. Qué HTML entiende

### 3.1 Estructura

`html`, `head`, `title`, `body`, `div`, `section`, `article`, `header`,
`footer`, `nav`, `main`, `aside`, `center`.

El `<title>` se muestra en la barra de la ventana. `body` acepta
`bgcolor` y `text` (colores de fondo y de texto), además de `style`.

### 3.2 Bloques

| Etiqueta | Qué hace |
|---|---|
| `h1` … `h6` | Encabezados, en negrita: 28, 22, 18, 16, 14 y 12 px |
| `p` | Párrafo, con aire arriba y abajo |
| `br` | Salto de línea |
| `hr` | Regla horizontal |
| `ul`, `ol`, `li` | Listas con viñeta (`*`) o numeradas; se pueden anidar; `ol` acepta `start` |
| `blockquote` | Cita: sangrada, con barra lateral gris |
| `pre` | Texto preformateado en monoespaciada: respeta espacios y saltos, no ajusta líneas |
| `table`, `tr`, `td`, `th` | Tablas: anchura de columna según el contenido, texto ajustado dentro de cada celda, cabecera `th` en negrita sobre fondo gris, bordes finos. `thead`/`tbody` se aceptan. Sin `colspan`/`rowspan` |

### 3.3 Inline

| Etiqueta | Qué hace |
|---|---|
| `b`, `strong` | Negrita (real: hay glifos de negrita) |
| `i`, `em`, `cite`, `var` | Cursiva (real) |
| `u` | Subrayado |
| `code`, `kbd`, `samp`, `tt` | Monoespaciada, en granate |
| `small`, `big` | 80 % / 125 % del tamaño actual |
| `span` | Sin efecto por sí solo; para aplicar `style` o `class` |
| `font` | `color`, `size` (1–7) y `face` (si contiene "mono" o "courier", monoespaciada) |
| `a href="…"` | Enlace: azul y subrayado, clicable |
| `button`, `input type="button"` | Botón con caja y borde; clicable desde un script por su `id` (§11) |
| `img src="…"` | Imagen `.nimg` (ver §5); acepta `width`, `height` y `alt` |

### 3.4 Otros

- **Entidades**: `&amp;` `&lt;` `&gt;` `&quot;` `&apos;` `&nbsp;`
  `&copy;` `&reg;` `&hellip;` `&mdash;` `&ndash;` `&laquo;` `&raquo;`
  `&middot;` `&bull;` `&trade;` `&euro;` `&larr;` `&rarr;`, y numéricas
  `&#241;` / `&#xF1;`. Las que no conoce se dejan tal cual.
- **Comentarios** `<!-- … -->` y `<!DOCTYPE>`: se ignoran.
- **Tolerancia**: etiquetas sin cerrar, `<li>` que abre otro `<li>`,
  `<p>` cerrado implícitamente por un bloque, mayúsculas, atributos con
  o sin comillas — todo se acepta. Un `<script>` se ignora entero.

---

## 4. Estilos (CSS)

Se aceptan en el atributo `style=""` de cualquier etiqueta y en un
bloque `<style>` dentro de `<head>`.

### 4.1 Selectores

Por etiqueta (`p`), por clase (`.aviso`), por id (`#cabecera`), y la
combinación etiqueta.clase (`p.nota`). Varios selectores separados por
coma. **No** hay descendientes (`div p`), pseudoclases (`:hover`) ni
atributos.

```css
h1, h2 { color: navy }
.aviso { background-color: #fff3b0; color: #503000 }
#pie   { text-align: center; font-size: 12px }
```

### 4.2 Propiedades

| Propiedad | Valores |
|---|---|
| `color` | nombre (`red`, `navy`, `steelblue`… unos 60), `#rrggbb`, `#rgb`, `rgb(r,g,b)` |
| `background-color`, `background` | igual; `transparent` lo quita. En un bloque, pinta el bloque entero; en un inline, solo el texto |
| `text-align` | `left`, `center`, `right` |
| `font-size` | `Npx`, `Nem` (1em = 14px), `Npt`. Se usa el tamaño más cercano disponible: 10, 12, 14, 16, 20, 24, 32, 40 px |
| `font-weight` | `bold`, `normal`, o un número (≥600 = negrita) |
| `font-style` | `italic`, `oblique`, `normal` |
| `font-family` | `sans-serif` (por defecto), `serif`, `monospace` (también vale `Courier`, `Consolas`) |
| `text-decoration` | `underline`, `none` |
| `margin`, `padding` | Margen (fuera de la caja) y relleno (dentro, con el fondo), en `px`, `em` o `pt`. Forma corta con 1 a 4 valores —`10px`; `4px 20px` (arriba/abajo, lados); `1px 2px 3px 4px` (arriba, derecha, abajo, izquierda)— o larga: `margin-top`, `padding-left`… No se heredan. Sin `margin`, los párrafos, encabezados y listas llevan su aire de media línea arriba y abajo; `margin: 0` lo quita |
| `display: none` | Oculta el elemento (también el atributo `hidden`) |

Lo que **no** hay: `border`, `width`/`height` en bloques, `float`,
`position`, columnas, `line-height`.

### 4.3 Tablas

Las columnas se reparten según el contenido: cada columna pide como
mínimo el ancho de su palabra más larga y como máximo el de su celda
más larga en una sola línea; si todo cabe, cada una toma su máximo; si
no, el espacio sobrante se reparte en proporción. `text-align` en `td`
o `th` alinea la celda. Las celdas pueden contener listas y párrafos.

Una celda puede abarcar varias columnas (`colspan="2"`) y varias filas
(`rowspan="3"`), o las dos cosas. Si su contenido no cabe en las
columnas que abarca, estas se ensanchan; si no cabe en sus filas, la
última de ellas crece. Un `rowspan` que pase de la última fila se
recorta a la tabla.

---

## 5. Imágenes

Solo el formato del sistema, **`.nimg`**, hasta 256×256 píxeles. Para
convertir un PNG o JPEG desde tu ordenador:

```bash
python3 herramientas/nimg_convert.py foto.png foto.nimg
```

y copia el `.nimg` junto al `.html`. En la página:

```html
<img src="foto.nimg" alt="Una foto" width="120">
```

La ruta es relativa a la carpeta del documento, y puede bajar a una
subcarpeta (`IMG/foto.nimg`). Si la imagen no se encuentra, se muestra
el `alt` entre corchetes. Con `width` o `height` solos, el otro se
escala en proporción, y el visor dibuja la imagen **a ese tamaño**
(una copia redimensionada la primera vez, que se reutiliza). La
ampliación es de píxel grande, sin suavizar: para fotos, mejor
convertirlas ya al tamaño en que se van a ver.

---

## 6. Enlaces

`<a href="otra.html">` abre otro archivo:

- De la misma carpeta: `href="ayuda.html"`.
- De una subcarpeta: `href="docs/manual.html"`.
- De la carpeta de arriba: `href="../indice.html"` (y `../../`, etc.).
- Un ancla de la misma página: `href="#seccion"` desplaza el documento
  hasta el elemento con `id="seccion"` (o `<a name="seccion">`).
- Otra página y un ancla suya: `href="manual.html#instalar"`.

**No** se soportan rutas absolutas ni URLs (`http://…`). El historial de
navegación es por ventana: Retroceso vuelve por donde has venido. Las
imágenes admiten las mismas rutas.

---

## 7. Tipografía

Desde la Semana 22, Nemo OS tiene fuentes proporcionales de verdad:
las DejaVu (licencia libre), pre-rasterizadas con antialiasing en el
paquete `FUENTES.NFP` que viaja dentro del kernel.

- **Familias**: sans (por defecto), serif, mono.
- **Tamaños**: 10, 12, 14, 16, 20, 24, 32, 40 px (sans); 12–32 (serif);
  10–20 (mono). Si se pide otro, se usa el más cercano.
- **Estilos**: negrita en todas las familias; cursiva en sans (12–20 px).
  Si se pide cursiva en serif o mono, se usa la recta.
- **Caracteres**: todo Latin-1 (acentos, ñ, ç, ¿¡, º ª, ¢ £ ¥…), más
  `– — ‘ ’ “ ” • … € ← → ↑ ↓`. Los archivos deben ir en **UTF-8**; un
  byte suelto se interpreta como Latin-1. Un carácter sin glifo sale
  como `?`.

Los tamaños de encabezado y párrafo están pensados para una ventana de
640 px de ancho a 1x. El zoom (`+`/`-`) multiplica todos los tamaños.

---

## 8. Recetas

**Una caja destacada**
```html
<div style="background-color:#fff3b0; color:#503000">
  <p><b>Aviso:</b> texto del aviso.</p>
</div>
```

**Título centrado con subtítulo**
```html
<h1 style="text-align:center">Nemo OS</h1>
<p style="text-align:center"><i>Sistema operativo para Raspberry Pi 4</i></p>
```

**Bloque de código**
```html
<pre>
  run visor.lua pagina.html
  run reloj.lua
</pre>
```

**Índice de páginas**
```html
<ul>
  <li><a href="capitulo1.html">Capítulo 1 — El arranque</a></li>
  <li><a href="capitulo2.html">Capítulo 2 — La MMU</a></li>
</ul>
```

**Pie de página discreto**
```html
<hr>
<p style="text-align:center; color:gray; font-size:12px">Nemo OS · Laboratorio Astillero</p>
```

---

## 9. Cómo está hecho

Para quien quiera ampliarlo. Tres piezas:

```
lua/ejemplos/nemo_html.lua   Motor: parser + CSS + maquetación. NO dibuja.
                             Instalado en /SISTEMA (require("nemo_html")).
lua/ejemplos/visor.lua       Ventana: carga, pinta, scroll, clics, zoom.
                             Instalado en /ACCESORIOS.
src/fonts.c                  Fuentes proporcionales en el kernel (NFNT/NFNP).
```

### 9.1 El motor

`nemo_html.parsear(html)` devuelve un árbol de nodos
(`{tag=, attrs=, hijos=}` y `{texto=}`). `nemo_html.maquetar(arbol,
opciones)` recorre el árbol y devuelve `{ops=, alto=, titulo=, fondo=}`,
donde `ops` es una lista de órdenes con coordenadas absolutas en un
documento de ancho `opciones.ancho`:

```lua
{ tipo="texto",  x=, y=, texto=, tamano=, negrita=, cursiva=, mono=, serif=, color= }
{ tipo="rect",   x=, y=, w=, h=, color= }
{ tipo="imagen", x=, y=, w=, h=, src=, alt= }
{ tipo="enlace", x=, y=, w=, h=, href= }      -- caja invisible, para el clic
```

Las métricas de texto las pone quien llama, con
`opciones.metrica(estilo)` → `{ancho=function(texto), alto_linea=,
ascent=}`. Así el motor no depende del sistema de fuentes: en el host
se prueba con la métrica fija de la 5x7 (`nemo_html.metrica_fija`); en
la Pi, `visor.lua` le pasa la de las fuentes reales.

La maquetación es flujo de bloques en vertical, y dentro de cada
bloque texto inline partido en líneas al ancho disponible, alineadas
por la base (una palabra grande y una pequeña comparten pie). Cada
línea se cierra emitiendo sus órdenes.

`nemo_html.enlace_en(resultado, x, y)` devuelve el `href` bajo un punto
del documento, o `nil`.

### 9.2 El visor

Carga el archivo con `nemo_archivos.leer_en`, lo parsea y maqueta, y en
cada redibujado recorre las órdenes restando el `scroll` y pintando
solo las que caen en la ventana. Para el texto, elige la fuente del
sistema que corresponde a `(tamano, negrita, cursiva, familia)` con una
caché con desalojo (el kernel tiene 16 huecos de fuente; el visor usa
hasta 14 y libera la menos usada), y mide con la tabla de avances de
cada fuente (`nemo_gui.avances_fuente` → `medir_con_avances`), sin
syscalls por palabra.

### 9.3 Las fuentes

`herramientas/nfnt_pack.py` genera `fuentes/FUENTES.NFP` desde las TTF
de DejaVu con Pillow: cada cara es un NFNT (cabecera, tabla de glifos
ordenada por codepoint, datos alpha de 8 bits), y el paquete NFNP los
agrupa con un directorio. El formato está documentado en el propio
script. El kernel (`src/fonts.c`) lo lee en el sitio, sin copiar.

Desde Lua, `nemo_gui` expone: `cargar_fuente(familia, alto, negrita,
cursiva, subrayado)` → handle, `poner_fuente(handle)` (0 = la de
sistema), `liberar_fuente`, `medir_texto(s)`, `alto_fuente()`,
`avances_fuente(handle)` y `medir_con_avances(tabla, s)`. Cualquier
programa Lua puede usarlas, no solo el visor.

### 9.4 Ampliar

- **Una etiqueta nueva**: en `estilo_de()` (si cambia el estilo) o en
  `maquetar_bloque()` (si es un bloque con comportamiento propio), en
  `nemo_html.lua`. Añadirla también a `BLOQUES` si es de bloque.
- **Una propiedad CSS nueva**: en `aplicar_declaraciones()`.
- **Un comportamiento nuevo del visor** (teclas, gestos): en el
  manejador de `gui.bucle` de `visor.lua`.
- **Probar sin la Pi**: el motor entero corre con `lua_host`; las
  baterías del proyecto construyen HTML, maquetan y comprueban las
  órdenes. Para el visor, un mock de `nemo`/`nemo_gui`/`nemo_archivos`
  con un disco en memoria.

### 9.5 Lo que falta, por orden de utilidad

1. `<select>`, `<textarea>` y mover el cursor dentro de un campo (hoy
   se escribe y se borra siempre al final).
2. `border` y anchos (`width`) en los bloques.
3. Identificadores en los títulos de los documentos Markdown, para
   enlazar a una sección de una guía con `#`.

---

## 10. Markdown

El visor abre también archivos **`.md`** (y `.markdown`): los convierte
a HTML con `nemo_md.lua` (en `SISTEMA`) y los pinta igual. Las guías del
sistema — esta, la de red y la de Lua — llegan a `MANUALES` en cada
arranque como `GUIA_HTML.md`, `GUIA_RED.md` y `GUIA_LUA.md`, y se abren
con doble clic desde el explorador.

Subconjunto (el Markdown "de GitHub" que usan las guías): encabezados
`#`…`######` y setext, párrafos, `---` reglas, `>` citas anidables,
listas `-`/`*`/`+` y `1.` anidadas por sangría, bloques de código
cercados con ``` y sangrados con 4 espacios, tablas `| a | b |` con
fila separadora y alineación `:--:`, `**negrita**`, `*cursiva*`,
`` `código` ``, `~~tachado~~` (como cursiva), `[texto](enlace)`,
`![alt](imagen.nimg)`, autoenlaces `<http://…>`, escapes `\*`, dos
espacios al final = salto de línea, y etiquetas HTML inline sueltas.
No hay notas al pie, listas de tareas ni enlaces por referencia.

Un enlace `[x](otra.md)` abre el otro archivo Markdown; `[x](p.html)`
abre HTML. Retroceso vuelve, como siempre. Desde Lua:
`require("nemo_md").documento(texto)` devuelve una página HTML completa
con título y una hoja de estilos sobria; `.a_html(texto)` solo el cuerpo.

---

## 11. Páginas con comportamiento: `<script type="text/lua">`

Una página puede llevar código, y el código es **Lua** — el mismo del
sistema — dentro de `<script type="text/lua">` en el `<head>` o el
`<body>`. Nada de JavaScript. Los `<script>` sin ese `type` se ignoran.

```html
<p>Valor: <span id="n">0</span></p>
<p><button id="mas">+ 1</button> <button id="cero">Cero</button></p>
<script type="text/lua">
  local n = 0
  doc.al_pulsar("mas",  function() n = n + 1; doc.get("n").texto = n end)
  doc.al_pulsar("cero", function() n = 0;     doc.get("n").texto = n end)
  doc.cada(1000, function() doc.get("hora").texto = doc.hora() end)
</script>
```

El código corre en el intérprete del visor pero dentro de un **entorno
aislado**: dispone de `string`, `math`, `table`, `utf8`, `tostring`,
`tonumber`, `pairs`, `ipairs`, `pcall`, `print` (sale por la UART) y del
objeto `doc`. No tiene `require`, ni `nemo`, ni acceso a archivos, red o
syscalls. Una página no puede hacer nada fuera de sí misma.

### 11.1 El objeto `doc`

| Función | Qué hace |
|---|---|
| `doc.get(id)` | El elemento con ese `id`, o `nil` (ver 11.3: qué se puede hacer con él) |
| `doc.ir("#id")` | Desplaza la página hasta ese ancla, como un enlace `href="#id"` |
| `doc.al_cambiar(id, fn)` | `fn(valor)` cada vez que cambia un campo de texto; `fn(true/false)` al marcar una casilla |
| `doc.al_enviar(id, fn)` | `fn(valores)` al enviar el `<form>` con ese `id` (ver 11.4) |
| `doc.foco(id)` | Da el foco del teclado a ese campo (`nil`: a ninguno) |
| `doc.al_pulsar(id, fn)` | Llama a `fn()` al hacer clic en el elemento con ese `id` — un `<button>`, o cualquier `span`, `p`, `div`… con `id` |
| `doc.cada(ms, fn)` | Llama a `fn()` cada `ms` milisegundos (resolución de 10 ms). Devuelve un identificador para `doc.parar(id)` |
| `doc.tecla(fn)` | Llama a `fn(caracter, codigo)` con cada tecla imprimible. Las de control (flechas, Retroceso, Esc, `+`/`-`) se las queda el visor |
| `doc.abrir(href)` | Navega a otro archivo, como un enlace (con historial) |
| `doc.titulo(texto)` | Cambia el título de la ventana |
| `doc.hora()`, `doc.fecha()` | `"HH:MM:SS"`, o `{anio, mes, dia, hora, minuto, segundo}` |
| `doc.ticks()` | Milisegundos desde el arranque |
| `doc.log(...)` | Escribe por la UART, para depurar |

Cualquier cambio en un elemento marca la página: al terminar el evento en
curso, el visor la remaqueta y la redibuja. Varios cambios en un mismo
manejador cuestan un solo redibujado.

### 11.2 Botones y zonas de clic

`<button id="x">Texto</button>` (o `<input type="button" value="Texto">`)
dibuja una caja con borde y fondo, en línea con el texto. Pero **cualquier
elemento con `id` es clicable**: un `<p id="semaforo">` entero, un `<span
id="palabra">`, un `<div id="tarjeta">`. Si hay varios anidados, gana el
más interior. Los enlaces siguen funcionando igual y tienen prioridad
sobre la zona en la que estén.

### 11.3 Cambiar la página: elementos

Lo que devuelve `doc.get(id)` —o `anadir`, abajo— es un elemento, con:

| Qué | Para qué |
|---|---|
| `el.texto` | Leer su texto, o sustituir todo su contenido por un texto |
| `el.color`, `el.fondo` | Color del texto y del fondo: nombre, `#rrggbb` o número |
| `el.visible` | `true`/`false`: mostrar u ocultar (`display: none`), sin perder sus demás estilos |
| `el.valor` | El texto de un campo (`<input>`): leer y cambiar |
| `el.marcado` | Si una casilla está marcada: `true`/`false`, leer y cambiar |
| `el.id`, `el.etiqueta` | Su `id` y su etiqueta (`"p"`, `"li"`…), solo lectura |
| `el:anadir(etiqueta, texto, atributos)` | Crea un hijo al final y lo devuelve. `texto` y `atributos` son opcionales: `lista:anadir("li", "leche", { id = "l1" })` |
| `el:borrar()` | Lo quita de la página |
| `el:vaciar()` | Le quita todo el contenido (hijos y texto) |

Los elementos creados son como los demás: se pueden colorear, ocultar,
borrar, y encontrarlos después por su `id`. Y se pueden anidar:
`caja:anadir("p", "Total: "):anadir("b", "42")`.

En HTML, `hidden` o `style="display: none"` ocultan un elemento desde el
principio; un script lo muestra con `el.visible = true`.

### 11.4 Campos de texto y formularios

```html
<form id="alta">
  Nombre: <input type="text" name="nombre" size="18" placeholder="tu nombre">
  Clave:  <input type="password" name="clave" maxlength="8">
  <input type="checkbox" name="avisos"> Quiero avisos
  <button>Enviar</button>
</form>
<p id="respuesta"></p>
<script type="text/lua">
  doc.al_enviar("alta", function(v)
    doc.get("respuesta").texto = "Hola, " .. v.nombre
  end)
</script>
```

- **Campos**: `<input>` sin tipo o de tipo `text`, `password` (se ve
  con asteriscos), `email`, `number`, `search`, `tel` o `url`. Admiten
  `value`, `size` (ancho en caracteres, 20 por defecto), `placeholder`
  (texto de ayuda en gris mientras está vacío) y `maxlength`.
- **Casillas**: `<input type="checkbox">`, con `checked` para empezar
  marcadas. Un clic las marca o las desmarca.
- **El teclado**: un clic en un campo le da el foco (borde azul y
  cursor). Las letras se escriben en él, **Retroceso** borra, **Tab**
  pasa al campo siguiente, **Enter** envía su formulario y **Esc**
  suelta el teclado (sin cerrar el visor). Las flechas siguen
  desplazando la página. Un clic fuera de los campos también suelta el
  foco.
- **Enviar**: Enter en uno de sus campos, o un clic en su botón de envío
  —un `<button>` sin `type` o con `type="submit"`, o un
  `<input type="submit" value="…">`—. Si hay `doc.al_enviar` para el
  `id` del formulario, recibe una tabla con los valores de todos sus
  campos, por `name` (o por `id` si no tienen `name`): las casillas como
  `true`/`false`, los demás como texto. No hay red: enviar es llamar a
  tu función.

### 11.5 Errores y límites

- Un error en un script —al cargar o dentro de un manejador— no tumba el
  visor: aparece una barra roja arriba con el mensaje, y la página sigue
  usable. Los scripts se ejecutan en orden al cargar la página, y se
  detienen en el primero que falle.
- Nemo OS es cooperativo: un bucle infinito en un script congela el visor
  (y el sistema). Los manejadores deben ser cortos; lo periódico va en
  `doc.cada`, nunca en un `while`.
- No se pueden crear ni borrar elementos (todavía): se cambia el texto y
  el estilo de los que ya existen. Con eso se hacen contadores, relojes,
  calculadoras, cuestionarios, paneles de estado.
- Al navegar a otra página, los temporizadores y manejadores de la
  anterior se descartan.

`DOCUMENTOS/demo_lua.html` es una página completa de ejemplo: un
contador, un reloj, un semáforo y un panel de teclado.
