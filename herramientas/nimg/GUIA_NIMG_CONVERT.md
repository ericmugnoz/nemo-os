# Guía — convertir imágenes a NIMG para Nemo OS

Nemo OS no tiene decodificador de PNG ni JPEG de verdad — solo
entiende un formato propio, "NIMG". Para usar una foto normal, hay
que convertirla primero **en tu Mac**, fuera de Nemo OS, con
`nimg_convert.py`.

## 1. Requisito, una sola vez

```bash
pip3 install Pillow
```

## 2. Convertir una imagen

```bash
python3 nimg_convert.py entrada.png  salida.nimg
python3 nimg_convert.py entrada.jpg  salida.nimg
python3 nimg_convert.py entrada.jpeg salida.nimg
```

PNG y JPEG funcionan igual de bien — el script detecta el formato
por el contenido del archivo, no por la extensión.

## 3. Si la imagen es grande

El pool de imágenes del kernel tiene un límite real de **256×256
píxeles**. Si tu foto es más grande, dale un ancho y alto máximo
como tercer y cuarto argumento:

```bash
python3 nimg_convert.py foto.jpg salida.nimg 200 150
```

El script la reduce sola (con buen remuestreo) para que quepa sin
pasarse, y avisa en pantalla del tamaño final:

```
leido foto.jpg: formato JPEG, 1920x1080
redimensionada a 200x112 para no superar 200x150
escrito salida.nimg: 200x112, 89612 bytes
```

Si no le das ningún tamaño, usa 256×256 por defecto. Si la imagen ya
es más pequeña que el máximo, no la toca.

## 4. Copiar a Nemo OS

El visor de imágenes (`visor_imagenes.lua`) busca en
`DOCUMENTOS/IMAGENES`. Copia ahí el `.nimg` resultante (crea la
carpeta si no existe todavía), y ejecuta:

```
run visor_imagenes.lua
```

## Formato NIMG, por si hace falta el detalle

Todo en little-endian:

| Offset | Contenido |
|---|---|
| 0 | magia `"NIMG"` (4 bytes, sin terminador nulo) |
| 4 | ancho (uint32) |
| 8 | alto (uint32) |
| 12 | píxeles RGBA en crudo, fila a fila (ancho × alto × 4 bytes) |

Sin compresión — un archivo de 256×256 ocupa como máximo 262.156
bytes (256×256×4 + 12 de cabecera).

## Errores más comunes

- **"hace falta Pillow"** → te falta el paso 1 (`pip3 install Pillow`).
- **"supera el límite del kernel (256x256)"** → la imagen sigue
  siendo más grande que 256×256 incluso después de reducirla; baja
  el ancho/alto máximo que le pasas.
- La imagen no aparece en el visor → confirma que la copiaste a
  `DOCUMENTOS/IMAGENES` exactamente, no a la raíz ni a otra carpeta.
