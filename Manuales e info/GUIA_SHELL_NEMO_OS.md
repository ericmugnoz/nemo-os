# Nemo OS — Guía de la Shell

La Shell es una consola de comandos interactiva que corre como un programa
`.pro` normal, usando únicamente syscalls del kernel — igual que cualquier
programa que puedas escribir tú mismo con `nbc.pro`.

---

## El prompt

```
C:\> 
```

- `C:` es **NemoFS**, el disco de sistema.
- `F:` es el disco **FAT** actualmente montado — la partición de arranque de
  la SD, o un pendrive si hay uno conectado (se cambia solo al conectar o
  desconectar).
- El resto del prompt muestra la carpeta actual, estilo DOS:
  `C:\DOCUMENTOS\PROYECTOS>`.

Cambiar de disco directamente escribiendo la letra:

```
C:\> F:
F:\>
```

---

## Aspecto e interacción

- **Cursor intermitente** al final de la línea de entrada, parpadeando cada
  medio segundo — igual que en Linux o MS-DOS.
- **Ajuste de línea automático**: si lo que escribes o la salida de un
  comando no cabe en el ancho actual de la ventana, continúa sola en la
  fila de abajo. No hay límite de columnas fijo.
- **Scrollback**: se guardan hasta 300 líneas de historial. Cuantas se ven
  a la vez depende del tamaño de la ventana en cada momento — si la
  agrandas, se muestra más historial sin que se haya perdido nada.
- La ventana se abre con un tamaño amplio (560×380) para que la mayoría de
  líneas quepan sin partirse.

---

## Comandos de navegación

| Comando | Qué hace |
|---|---|
| `ls` | Lista el contenido de la carpeta actual: nombre y tamaño (o `<DIR>` si es una carpeta). |
| `cd <carpeta>` | Entra en una subcarpeta. |
| `cd ..` | Sube un nivel. |
| `pwd` | Muestra la ruta completa actual. |
| `C:` | Cambia a NemoFS. |
| `F:` | Cambia al disco FAT (SD o pendrive). |
| `disco nemofs` | Igual que `C:`. |
| `disco fat` | Igual que `F:`. |

---

## Comandos de archivos

| Comando | Qué hace |
|---|---|
| `mkdir <nombre>` | Crea una carpeta en la ubicación actual. *(Solo en NemoFS — FAT todavía no admite subcarpetas desde aquí.)* |
| `del <nombre>` | Borra un archivo. |
| `rm <nombre>` | Alias de `del`. |
| `cat <archivo>` | Muestra el contenido de un archivo de texto. |
| `type <archivo>` | Alias de `cat`. |
| `echo <texto>` | Imprime el texto tal cual — útil para probar el ajuste de línea o dejar notas rápidas en pantalla. |

---

## Ejecutar programas

| Comando | Qué hace |
|---|---|
| `run <archivo.pro>` | Lanza un programa. Lo busca primero en la carpeta actual y, si no está, en `PROGRAMAS`. No hace falta que esté instalado: basta con que exista como archivo. |
| `run <archivo.pro> <arg>` | Lanza el programa pasándole `<arg>` como argumento de lanzamiento — por ejemplo, `run nbc.pro ejemplo.bb` compila `ejemplo.bb`. |

La salida de cualquier programa lanzado con `run` se integra en el
historial de la shell, línea a línea, como si fuera el resultado de un
comando propio.

---

## Otros comandos

| Comando | Qué hace |
|---|---|
| `help` | Muestra un resumen de todos los comandos. |
| `about` | Información sobre la Shell. |
| `clear` | Borra la pantalla. |
| `cls` | Alias de `clear` (para quien venga de MS-DOS). |
| `exit` | Cierra la Shell. |

---

## Notas técnicas

- Los nombres de archivo y carpeta no distinguen mayúsculas de minúsculas
  al buscarlos (aunque en pantalla todo se muestra en mayúsculas, por la
  fuente del sistema).
- `cat`/`type` comprueban que el archivo existe antes de abrirlo — no crean
  uno nuevo por error si escribes mal el nombre.
- Si lanzas la Shell con un comando de arranque (por ejemplo, el IDE la abre
  así tras compilar: `run miprograma.pro`), lo ejecuta automáticamente al
  iniciar, como si lo hubieras escrito tú.
