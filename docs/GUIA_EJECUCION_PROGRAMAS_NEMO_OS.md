# Cómo se ejecutan los programas en Nemo OS

Guía de referencia técnica para quien escriba un compilador nuevo, un
programa a mano, o cualquier cosa que termine como `.pro` — y para
entender qué pasa cuando se lanza un `.lua`, un `.html` o un `.md`.

**Revisada en septiembre de 2026** tras la separación kernel/programas
(Semanas 10-11 de la bitácora): los programas corren ahora en EL0, cada
uno con su propia memoria, y varias cosas de la versión anterior de
esta guía dejaron de ser ciertas. Las secciones marcadas con ⚠ son las
que cambiaron.

---

## 1. Qué archivos se pueden ejecutar

| Extensión | Qué es | Quién lo ejecuta |
|---|---|---|
| `.pro` | Código máquina ARM64 con cabecera NEXE (§2) | El kernel, directamente |
| `.lua` | Script de Lua 5.5 | El kernel lo redirige a `PROGRAMAS/LUA.PRO` con el script como argumento (§7) |
| `.html`, `.md` | Documento | `ACCESORIOS/visor.lua` — el explorador lo lanza con doble clic (§7) |
| `.nb` | Fuente de Nemo Basic | No se ejecuta: se abre con el IDE, que lo compila a `.pro` con `nbc.pro` |

Dónde vive cada cosa en NemoFS, instalado por el kernel en cada
arranque desde lo que va embebido en `kernel8.img`:

```
/PROGRAMAS/    los .pro del sistema: shell, explorer, editor, ide, nbc, LUA.PRO...
/SISTEMA/      librerías de Lua (nemo_gui, nemo_archivos, nemo_sistema, nemo_html, nemo_md)
/ACCESORIOS/   programas escritos en Lua (shell.lua, editor.lua, reloj.lua, visor.lua...)
/MANUALES/    las guías del sistema en .md, y AYUDA/ dentro
/DOCUMENTOS/   páginas de ejemplo y los proyectos .anx
```

Lo que hay en esas carpetas se **sobreescribe en cada arranque**: la
fuente de verdad es el código del proyecto, no el disco. Los programas
propios del usuario van en cualquier otra carpeta.

## 2. El formato `.pro` (NEXE)

Cabecera de 16 bytes, seguida del código máquina crudo:

```
offset 0:  magic[4]      = "NEXE"
offset 4:  version (u32)
offset 8:  entry_offset  (u32) -- normalmente 0
offset 12: code_size     (u32)
offset 16: código máquina ARM64 crudo (code_size bytes)
```

El código debe ser **independiente de la posición**: no hay
reubicación al cargar (`loader_load_into` copia los bytes tal cual y
salta). Todo el direccionamiento tiene que resolverse en el propio
código (§5). Tamaño máximo de un `.pro`: 6 MB (búfer del cargador);
NemoFS admite archivos de hasta 8 MB.

## 3. ⚠ Cómo se carga y arranca una tarea

`task_spawn_from_file` (en `tasks.c`):

1. Busca un hueco libre entre las **8 áreas de programa** de 16 MB cada
   una (`task_program_areas[MAX_TASKS]`). Las áreas están alineadas a
   **2 MB** (antes 4 KB): la MMU da permisos por bloques de ese tamaño.
   Cada tarea arranca, por tanto, en una dirección alineada a 2 MB —
   más que suficiente para que `adrp`/`:lo12:` (§5) sea seguro.
2. Limpia el área entera a cero (el `.bss` implícito ya empieza a
   cero sin que el `.pro` lo incluya).
3. Copia el código y calcula `entry = área + entry_offset`.
4. Copia un **stub de salida** de 12 bytes al final del área:
   `mov x8, #0 ; svc #0 ; b .` — es decir, `SYS_EXIT`.
5. La primera vez que el planificador le da turno, `task_trampoline`
   hace un **`eret` a EL0** con `ELR_EL1 = entry`, `SPSR_EL1 = EL0t`, y
   **`SP_EL0` = la dirección del stub**. El programa arranca en modo
   usuario, con la pila creciendo hacia abajo desde el final de su
   área, y con la dirección del stub como "dirección de retorno" de
   `_start`.

Es decir: el punto de entrada **ya no se llama como una función C
desde el kernel**. El kernel le cede la CPU en EL0 y no vuelve a saber
de él hasta que hace una syscall o falla.

### Distribución del área de 16 MB

```
área + 0          código (.text, .rodata, .data, .bss implícito)
...               lo que el programa use como heap (por ejemplo, el pool de 12 MB de Lua)
...               ↑ la pila crece hacia abajo desde aquí
área + 16MB - 16  stub de salida (mov x8,#0 ; svc #0 ; b .)
```

Un programa no puede pedir más memoria que su área. Para un `.pro`
normal es muchísimo; para el intérprete de Lua, que reserva un pool
fijo de 12 MB, es lo justo.

## 4. ⚠ Cómo termina un programa

Las dos formas son correctas ahora:

- **Terminar con `ret`** (lo que hacen los `.pro` de Nemo Basic:
  `ldp x29,x30,[sp],#16 ; ret`). Como `x30` apunta al stub de salida,
  el `ret` ejecuta `SYS_EXIT`. Ningún programa antiguo cambia.
- **Terminar con `mov x8, #0 ; svc #0`** (`SYS_EXIT` explícito).
  `SYS_EXIT` llama a `task_exit_current()` y **no vuelve**: cierra la
  ventana y los gadgets de la tarea, libera el área y cede a la
  siguiente. Ya no es "cooperativo" ni cae en `.rodata` — ese fallo
  de la versión anterior de esta guía desapareció con la Fase 1.

Lo que sigue siendo verdad: Nemo OS es **cooperativo**. Un bucle que
no ceda la CPU (`b .` sin syscalls, un `while(1){}`) **congela el
sistema entero**, no solo el programa. Un programa que necesite
"seguir vivo" (esperando eventos, compilando en varios pasos) tiene
que llamar a `SYS_PUMP` (14) en cada vuelta — es lo que hace
`nb_pump()` en C y `gui.bucle` en Lua.

## 5. Direccionar datos propios (cadenas, tablas...)

**Siempre `adrp`/`add :lo12:`, relativo a PC — nunca punteros
absolutos.**

```asm
adrp x0, mi_dato
add  x0, x0, :lo12:mi_dato
```

`adrp` calcula la página desde el PC en tiempo de ejecución, así que
funciona en cualquier dirección de carga.

**Lección sufrida tres veces en el proyecto** (`screensettings.c`,
`desktoped.c`, el lexer de Bitácora): nunca guardar `const char *` en
una tabla estática indexada — ni un array de struct, ni un `switch`
que "elige" un texto por un número. El compilador puede convertirlo
en una tabla de punteros absolutos que solo valen en la dirección de
enlazado. Síntomas: textos en blanco o cadenas ajenas; un lexer que
clasifica mal las palabras clave. Arreglo: cada cadena en su propia
llamada, nunca detrás de un índice.

## 6. ⚠ Lo que el programa NO puede hacer, y qué pasa si lo intenta

Desde la Fase 2, cada tarea tiene su propia tabla de traducción: **solo
ve su área de 16 MB**. El kernel y las otras tareas son inaccesibles
desde EL0.

- **Leer o escribir fuera del área** → `Data Abort desde EL0`. El
  kernel imprime en la UART el programa, el desplazamiento de la
  instrucción culpable, `FAR_EL1` (la dirección tocada) y los
  registros, **retira solo esa tarea** y el sistema sigue. Antes, esto
  colgaba todo.
- **Pasar a una syscall un puntero fuera del área** → la syscall lo
  rechaza (devuelve error o no hace nada), **sin tocar el puntero**.
  Todas las syscalls que reciben punteros (unas 70) validan que el
  rango entero — o la cadena completa hasta su `\0` — está dentro del
  área de quien llama. Una cadena sin terminador que llega al borde
  del área se rechaza.
- **Instrucciones privilegiadas** (`msr`, `mrs` de registros del
  sistema...) → excepción, misma consecuencia.
- Coma flotante y NEON **sí** están disponibles en EL0 (Lua los usa).

Para depurar un fallo: leer `desplazamiento dentro del programa` en la
UART y buscar esa dirección en el desensamblado del `.pro`
(`aarch64-elf-objdump -D` sobre el `.elf` intermedio).

## 7. Cómo se lanza cada tipo de archivo

### `.pro`

`SYS_LAUNCH_PROGRAM` (5): `a0` = nombre, `a1` = argumento (o `""`),
`a2` = carpeta donde buscar (inodo). El kernel busca el archivo en esa
carpeta y, si no está, en `PROGRAMAS`. La shell hace lo mismo con
`run programa.pro [arg]`: carpeta actual, luego `PROGRAMAS`.

### `.lua`

El mismo `SYS_LAUNCH_PROGRAM` con un nombre acabado en `.lua`. El
kernel (`task_spawn_from_file`) lo **redirige**: carga
`PROGRAMAS/LUA.PRO` y le pasa como argumento `"INODO:script.lua args"`,
donde `INODO` es la carpeta donde encontró el script — la carpeta
pedida o, si no estaba ahí, **`ACCESORIOS`**. Así `run reloj.lua`
funciona desde cualquier sitio. El intérprete lee el script de esa
carpeta y expone `arg[0]` (el script) y `arg[1..]` (las palabras del
argumento), como Lua estándar.

### `.html` y `.md`

El explorador, al hacer doble clic, lanza `visor.lua` con el argumento
`"carpeta:archivo"` (mismo formato que usa para abrir un `.txt` con el
editor). El visor convierte los `.md` a HTML y pinta. Desde la shell:
`run visor.lua archivo.html` (busca en la raíz, en `MANUALES` y en
`DOCUMENTOS`, por ese orden).

### El argumento de lanzamiento

`SYS_GET_LAUNCH_ARG` (6): `a0` = búfer, `a1` = tamaño. Devuelve la
cadena que pasó quien lanzó el programa. Convenciones en uso:

- `"parent:nombre"` — inodo de carpeta y nombre de archivo (editor, IDE,
  visor).
- `"nombre.bb"` — lo que compila `nbc.pro`.
- Palabras separadas por espacio — lo que reciben los scripts Lua.

## 8. Sacar texto por consola

`SYS_WRITE_STRING` (11): `x0` = cadena terminada en NUL. **El texto no
aparece en la shell hasta que llega un `\n`**: la shell acumula y solo
vuelca la línea al ver el salto. Cada línea son dos escrituras (texto y
`"\n"`), o una sola que ya lo incluya.

A dónde va depende de cómo se lanzó el programa:

- Con `run` desde la shell: en "modo consola", sin ventana propia; el
  texto llega a la ventana de la shell que lo lanzó.
- Sin padre (icono, doble clic en el explorador, menú Inicio): se le
  crea una ventana y `SYS_WRITE_STRING` va a la **UART**, no a ninguna
  ventana. Un programa puramente de consola no muestra nada si se
  lanza así.
- Desde la **shell remota** por red (`nc -v <la dirección que anuncia el arranque> 2323`, ver
  `GUIA_RED_NEMO_OS.md`): `run` lanza el programa en el escritorio de
  la Pi, como si fuera desde un icono — su salida de consola va a la
  UART, no a la conexión de red.

## 9. Convención de llamada a syscalls

`x0`-`x4` para hasta 5 argumentos, `x8` = número, `svc #0`, retorno en
`x0`. Catálogo en `src/syscall.h`; wrappers en C en `nb_syscalls.h`.
Las syscalls nuevas de esta ronda: 247-253 (tipo de inodo, lista y
cierre de tareas, uso de disco y memoria, imagen desde carpeta) y
254-255 (anchura de texto y avances de fuente). Ver
`GUIA_SYSCALLS_EVENTOS_GADGETS_NEMO_OS.md`.

## 10. Checklist para un compilador o un `.pro` nuevo

- [ ] ¿Todo el direccionamiento de datos es `adrp`/`:lo12:` relativo a PC?
- [ ] ¿Ninguna tabla estática indexada de punteros a texto?
- [ ] ¿El programa termina con `ret` o con `SYS_EXIT`? (los dos valen)
- [ ] ¿Cualquier espera cede la CPU con `SYS_PUMP`? (un bucle cerrado congela el sistema)
- [ ] ¿Cada línea de consola lleva su `\n`?
- [ ] ¿Los punteros que pasa a syscalls apuntan a memoria propia? (si no, la syscall los rechaza)
- [ ] ¿Cabe todo — código, datos, heap, pila — en 16 MB?
- [ ] Si es de consola: ¿se prueba con `run` desde la shell, no con doble clic?
- [ ] Si falla: ¿se ha leído `FAR_EL1` y el desplazamiento en la UART antes de teorizar?
