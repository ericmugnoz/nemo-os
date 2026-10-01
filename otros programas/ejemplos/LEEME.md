# Ejemplos de Nemo Basic

Programas cortos, **una idea cada uno**. Están para leerse enteros de una
sentada y copiarse a trozos: ninguno pasa de sesenta líneas.

| Archivo | De qué va |
| --- | --- |
| `lineas.nb` | `Line`: un abanico, un marco y un aspa. Lo más simple que dibuja algo. |
| `trigonometria.nb` | `Sin`, `Cos` y `Tan`: una onda, un reloj y un círculo. |
| `animacion.nb` | El bucle de siempre: borrar, dibujar, esperar. Un reloj que anda, una onda que avanza y algo en órbita. |
| `imagenes.nb` | Cargar un `.nimg`, la transparencia por color clave y el punto de agarre. |
| `ventana.nb` | Una ventana de verdad, con botones y un campo de texto. **No** dibuja en bucle: crea los controles una vez y espera a que pase algo. |
| `sonido.nb` | Los seis comandos de sonido. Solo suena en la Raspberry Pi: el jack va por PWM y QEMU no tiene ese hardware. |

## Cómo ejecutarlos

Desde la shell del propio sistema, que es donde tiene gracia:

```
run nbc.pro lineas.nb
run lineas.pro
```

Desde el ordenador de desarrollo, sin arrancar nada, con el emulador —vale
para los de consola, no para los que abren ventana:

```sh
nbc lineas.nb lineas.pro
python3 herramientas/emu/arm64.py lineas.pro
```

## Y después

- Las carpetas vecinas tienen los **programas completos**: dos juegos, un
  editor de texto con pestañas, el monitor del sistema, red y una biblioteca de
  utilidades. Ahí se ve cómo se organiza un programa que no cabe en un archivo.
- `docs/GUIA_NEMO_BASIC.md` es la introducción al lenguaje, y
  `docs/REFERENCIA_NEMO_BASIC.md` la referencia de los 260 comandos.
