# El formato NMZ — un paquete con una página dentro

Lo que el proxy le devuelve a Nemo OS cuando se le pide una página: el HTML ya
reducido y todas sus imágenes ya convertidas a `.nimg`, en **un solo archivo**.

## Por qué un paquete y no piezas sueltas

Pedir cada imagen por separado cuesta un ida y vuelta por imagen, y en Nemo OS
cada ida y vuelta cuesta **10 ms como mínimo**, porque la red se sondea cien
veces por segundo. Diez imágenes serían diez conexiones TCP y un centenar de
milisegundos regalados antes de transferir un solo byte útil.

Con un paquete hay una conexión y una descarga. Y encaja con cómo funciona el
resto del sistema: un archivo en el disco y un programa que lo abre.

## Por qué NO va comprimido

Porque no hace falta, y meter un descompresor dentro de Nemo OS para no
necesitarlo sería construir de más.

Las cuentas, con la velocidad **medida** el 28/09/2026 (2570 KB/s):

| Página | Tamaño | Tarda |
| --- | --- | --- |
| Texto y una imagen | ~150 KB | 0,06 s |
| Texto y cuatro imágenes | ~510 KB | 0,2 s |
| Ocho imágenes de 256×256 | ~2 MB | 0,8 s |

Comprimir ahorraría medio segundo en el peor caso, a cambio de un descompresor
que hay que escribir, probar y mantener. El día que se note, se añade; hasta
entonces, no.

## Por qué no `.zip` ni `.tar`

- **`.zip`** ya trae compresión, que es justo lo que no queremos, y su directorio
  central va **al final** del archivo: no se puede empezar a desempaquetar hasta
  haberlo recibido entero.
- **`.tar`** rellena cada archivo hasta un múltiplo de 512 bytes y gasta 512 más
  en cada cabecera. Con veinte imágenes pequeñas eso es bastante desperdicio, y
  su cabecera es un formato de 1979 con campos en octal.

Un formato propio de veinte líneas evita las dos cosas y se lee de un vistazo.

## El formato

Todos los números son enteros **sin signo, little-endian** — que es como está
escrito el resto de Nemo OS y como los lee el ARM sin dar vueltas.

```
  0   "NMZ1"          4 bytes, para saber que es esto y no otra cosa
  4   n_archivos      uint32
  8   tabla de n_archivos entradas, una detrás de otra:
          nombre_len  uint16
          nombre      nombre_len bytes, ASCII, sin barras
          tam         uint32     tamaño del contenido
      y después, los contenidos en el MISMO orden, pegados sin relleno
```

El primer archivo de la tabla es **siempre** `index.html`. Así el visor sabe por
dónde empezar sin tener que buscar nada.

Los nombres no llevan barras a propósito: un paquete se desempaqueta en una sola
carpeta, y un nombre con `../` dentro sería una forma de escribir donde no toca.
Quien desempaqueta **debe** rechazar cualquier nombre con `/`, `\` o `..`.

## Ejemplo

Una página con dos imágenes:

```
"NMZ1"  3
  10 "index.html"      4210
   5 "0.nimg"         76812
   5 "1.nimg"        249868
  <4210 bytes de HTML>
  <76812 bytes de la primera imagen>
  <249868 bytes de la segunda>
```

Total: 8 + (2+10+4) + (2+5+4) + (2+5+4) + 330890 = 330 KB.

## Lo que hay dentro del `index.html`

HTML **ya reducido** al subconjunto que entiende `lua/ejemplos/nemo_html.lua`:
encabezados, párrafos, listas, negritas, cursivas, enlaces, imágenes, `style=`
y bloques `<style>`. Sin scripts, sin tablas, sin formularios.

Las imágenes vienen reescritas a los nombres del paquete:

```html
<img src="0.nimg" width="200" height="150">
```

Y los enlaces, reescritos para que vuelvan a pasar por el proxy, de modo que
pinchar en uno pida la página siguiente igual que la primera.
