# Syscalls, eventos y gadgets en Nemo OS

Guía de referencia para escribir un programa normal (con ventana,
botones, campos de texto...) en Nemo OS. Complementa a
`GUIA_EJECUCION_PROGRAMAS_NEMO_OS.md` (esa guía cubre cómo arranca y
termina un `.pro`, el formato NEXE y el direccionamiento — esta guía
cubre la API que usa un programa MIENTRAS corre).

---

## 1. Convención de llamada a syscalls (recordatorio)

`x0`-`x4` para hasta 5 argumentos, `x8` = número de syscall, `svc #0`.
El valor de retorno vuelve en `x0`. Todos los ejemplos de esta guía
están en C (con el wrapper `syscall5` que ya usan todos los
programas), pero la misma convención aplica igual en ensamblador
puro.

```c
static inline uint64_t syscall5(uint64_t num, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4) {
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    register uint64_t x4 __asm__("x4") = a4;
    register uint64_t x8 __asm__("x8") = num;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
                     : "memory");
    return x0;
}
```

Catálogo completo en `src/syscall.h` — 255 syscalls (texto, archivos,
gráficos, sonido, gadgets, bancos de memoria, fuentes, sistema...).
Esta guía cubre las que hacen falta para ventanas/eventos/gadgets, y
desde septiembre de 2026 también las de **fuentes** (§8), las de
**sistema** (§9) y lo que cambió con la separación kernel/programas
(§10).

**Punteros: desde la Fase 2 (Semana 14 de la bitácora), toda syscall
que recibe un puntero comprueba que apunta a memoria de la tarea que
llama.** Un puntero fuera de tu área de 16 MB —a memoria del kernel,
de otra tarea, o simplemente basura— hace que la syscall devuelva
error (o no haga nada) sin tocarlo. Antes podía tumbar el sistema.

## 2. Ventanas: cada tarea ya tiene una

Toda tarea lanzada con ventana (no "modo consola", ver la otra guía)
YA TIENE una ventana en blanco automáticamente. `SYS_CREATE_WINDOW`
no crea nada nuevo — **personaliza** esa ventana (título, posición,
tamaño) y la pone en **"modo evento"**:

```c
#define SYS_CREATE_WINDOW 40
// a0=puntero a titulo, a1=x, a2=y, a3=ancho, a4=alto
// devuelve el indice de ventana (sirve de "parent" para gadgets)
int32_t win = syscall5(SYS_CREATE_WINDOW, (uint64_t)"MI PROGRAMA",
                        100, 100, 400, 300, 0);
```

**Importante — "modo evento" cambia el comportamiento de la X de
cerrar.** Antes de llamar a `SYS_CREATE_WINDOW`, la X cierra la
ventana directamente. Después, la X deja de hacerlo — en su lugar
dispara `EVENT_WINDOWCLOSE` (ver sección 4), y el programa tiene que
comprobarlo explícitamente y cerrar él mismo (normalmente saliendo de
su bucle principal). **Un programa que llama a `SYS_CREATE_WINDOW` y
no comprueba `EVENT_WINDOWCLOSE` no se puede cerrar con la X.**

## 3. Dos generaciones de gadgets (botones, campos de texto...)

### 3a. La sencilla — `SYS_DEFINE_BUTTON` + sondeo directo

```c
#define SYS_DEFINE_BUTTON 36  // a0=id (>0, lo eliges tu), a1=x, a2=y,
                               // a3=(ancho<<16|alto), a4=color
#define SYS_GET_BUTTON_ID 37  // sin argumentos -> id del boton pulsado
                               // esta vuelta, o 0 si ninguno
```

El programa tiene que dibujar su propia etiqueta encima con
`SYS_DRAW_TEXT` — `SYS_DEFINE_BUTTON` solo registra la zona clicable y
un aspecto por defecto. Hay que llamar a `SYS_GET_BUTTON_ID` una vez
por vuelta del bucle principal, igual que `SYS_READ_CHAR`.

**Límite real: 8 botones por ventana** (`MAX_BUTTONS_PER_WINDOW` en
`wm.c`). Ya se ha topado con este límite antes (ver
`programas-nemo-os`, mejoras de `explorer.c`) — el arreglo que
funcionó fue sustituir botones extra por detección de clic por
coordenadas a mano (leer `SYS_GET_MOUSE` + `SYS_MOUSE_HIT` y comparar
contra tus propios rectángulos), no pelear contra el límite.

### 3b. La completa — gadgets con eventos

```c
#define SYS_CREATE_BUTTON     100
#define SYS_CREATE_PANEL      101
#define SYS_CREATE_TEXTFIELD  102
#define SYS_CREATE_LISTBOX    103
#define SYS_CREATE_TEXTAREA   125
#define SYS_CREATE_MENU       121
#define SYS_CREATE_TIMER      127

#define SYS_GADGET_SET_TEXT   105  // a0=id, a1=puntero a texto
#define SYS_GADGET_GET_TEXT   106  // a0=id, a1=buffer, a2=tam max -> longitud
#define SYS_GADGET_EVENT      113  // sin argumentos -> id del gadget que
                                    // disparo algo esta vuelta, o 0
#define SYS_LISTBOX_ADD_ITEM  114
#define SYS_GADGET_FREE       104  // a0=id
#define SYS_GADGET_MOVE       108  // a0=id, a1=x, a2=y
#define SYS_GADGET_RESIZE     109  // a0=id, a1=ancho, a2=alto
#define SYS_GADGET_SHOW       110  // a0=id, a1=visible (0/1)
#define SYS_GADGET_ENABLE     111  // a0=id, a1=activo (0/1)
#define SYS_GADGET_ACTIVATE   112  // a0=id -- da el foco de teclado (TextField)
```

Sin el límite de 8 — este es el sistema que usan `shell.c`,
`explorer.c`, `ide.c` y `gadgetdemo.c`. `SYS_GADGET_EVENT` es un atajo
que YA filtra por ti "¿saltó algo, y qué id fue?" — más simple que
usar `SYS_POLL_EVENT`/`SYS_GET_EVENT_INFO` a mano cuando lo único que
te importa son los gadgets.

**Ejemplo real completo** (`programs/gadgetdemo.c`, simplificado):

```c
uint64_t wh = ((uint64_t)90 << 16) | 24; // ancho<<16 | alto
int32_t btn_id = syscall5(SYS_CREATE_BUTTON, (uint64_t)"PULSAME",
                           10, 30, wh, 0);

while (running) {
    if ((int64_t)syscall5(SYS_PUMP, 0,0,0,0,0) < 0) break; // ver seccion 5

    int32_t ev = (int32_t)syscall5(SYS_GADGET_EVENT, 0,0,0,0,0);
    if (ev == btn_id) {
        // el boton se pulso esta vuelta
    }
}
```

## 4. Eventos de bajo nivel — `SYS_POLL_EVENT` / `SYS_GET_EVENT_INFO`

Para lo que NO es un gadget (redimensionar, cerrar, menú, temporizador):

```c
#define SYS_POLL_EVENT     8  // -> tipo de evento (0 si ninguno)
#define SYS_GET_EVENT_INFO 9  // -> (fuente<<32 | datos) del ULTIMO
                                // evento consultado con POLL_EVENT
```

Catálogo real de tipos de evento (`src/gadgets.h`):

| Constante            | Valor  | Significado                              |
|-----------------------|--------|-------------------------------------------|
| `EVENT_GADGETACTION` | 0x401  | Un gadget disparó algo (equivalente de bajo nivel a `SYS_GADGET_EVENT`) |
| `EVENT_WINDOWSIZE`   | 0x802  | La ventana cambió de tamaño               |
| `EVENT_WINDOWCLOSE`  | 0x803  | Se pulsó la X de cerrar (solo en "modo evento", ver sección 2) |
| `EVENT_MENUACTION`   | 0x1001 | Se eligió una entrada de menú             |
| `EVENT_TIMERTICK`    | 0x4001 | Disparo de un temporizador (`SYS_CREATE_TIMER`) |

Patrón típico (visto en `shell.c`):

```c
int32_t ev = (int32_t)syscall5(SYS_POLL_EVENT, 0,0,0,0,0);
if (ev == EVENT_WINDOWCLOSE) {
    running = false; // el programa cierra su propio bucle
}
```

## 5. El bucle principal — la forma real de "esperar sin bloquear"

Nemo OS es **cooperativo**: nada cede la CPU a menos que llame a algo
que lo haga explícitamente. El patrón universal de todo programa con
ventana:

```c
while (running) {
    // 1. Cede la CPU al resto del sistema un paso.
    if ((int64_t)syscall5(SYS_PUMP, 0,0,0,0,0) < 0) break;

    // 2. Comprueba gadgets/eventos (no bloquea).
    int32_t ev_gadget = (int32_t)syscall5(SYS_GADGET_EVENT, 0,0,0,0,0);
    int32_t ev_bajo = (int32_t)syscall5(SYS_POLL_EVENT, 0,0,0,0,0);

    // 3. Comprueba teclado (no bloquea).
    char c = (char)syscall5(SYS_READ_CHAR, 0,0,0,0,0);

    // 4. Redibuja SOLO si algo cambio (no en cada vuelta a ciegas).
}
```

**`SYS_PUMP` devuelve -1 si la ventana del programa se cerró
externamente** — es la forma normal de detectar "me han cerrado,
tengo que terminar" sin tener que comprobar `EVENT_WINDOWCLOSE` en
cada programa. Comprobar el valor de retorno de `SYS_PUMP` es más
fiable que confiar solo en el evento.

## 6. E/S de archivos — recordatorio de volúmenes

Dos volúmenes distintos, casi todas las syscalls de archivo llevan un
parámetro `volume`:

- `VOLUME_NEMOFS` (0) — el sistema de archivos propio de Nemo OS.
- `VOLUME_FAT` (otro valor) — la partición FAT32 de la SD, y también
  cualquier pendrive USB conectado (se exponen como discos FAT
  aparte) — el mecanismo real de intercambio de archivos con el Mac.

Syscalls básicas: `SYS_FILE_OPEN` (20), `SYS_FILE_READ` (21),
`SYS_FILE_WRITE` (22), `SYS_FILE_LIST` (23), `SYS_FILE_DELETE` (25).
Un archivo en el disco FAT NO es lo mismo que un archivo en NemoFS
— hay que copiarlo explícitamente de un volumen a otro (con el
Explorer, por ejemplo) antes de que un programa que solo mira NemoFS
pueda encontrarlo.

## 7. Trampas, y cómo se reconocen

- **`SYS_FONT_WIDTH` no es el avance real entre caracteres** — da el
  ancho del glifo, no `FONT_WIDTH+1`. Para calcular columnas de texto,
  usar `SYS_FONT_CHAR_ADVANCE`.
- **`SYS_CREATE_WINDOW` cambia el comportamiento de la X** — ver
  sección 2. Se te olvida comprobar `EVENT_WINDOWCLOSE` (o el retorno
  de `SYS_PUMP`) y tu programa se queda sin forma de cerrarse con el
  ratón.
- **8 botones máximo con `SYS_DEFINE_BUTTON`** (`MAX_BUTTONS_PER_WINDOW`
  en el kernel, no algo que puedas ampliar desde tu programa) — pasado
  ese límite, el kernel los ignora en silencio. Usa el sistema de
  gadgets moderno (sección 3b) o detección de clic por coordenadas.
- **`SYS_DRAW_ICON` no admite parámetro de tamaño** — los iconos son
  de tamaño fijo del kernel; para escalar hace falta
  `fb_blit_icon_scaled`/`wm_content_blit_icon_scaled` a nivel de
  kernel, no algo disponible por syscall directa.
- **Programas de consola (sin `SYS_CREATE_WINDOW`) lanzados sin `run`
  desde la shell no muestran su `SYS_WRITE_STRING`** — ver
  `GUIA_EJECUCION_PROGRAMAS_NEMO_OS.md`, sección 5, para el porqué
  (redirección de consola según cómo se lanzó el programa).

## 8. Fuentes proporcionales (desde la Semana 22)

Hasta septiembre de 2026 solo existía la fuente de sistema, 5x7
píxeles, **solo mayúsculas** y escalable por enteros. Ahora hay
fuentes reales (DejaVu pre-rasterizadas, con antialiasing) en tres
familias: `sans`, `serif`, `mono`; tamaños 10-40 px; negrita en todas,
cursiva en sans. Cobertura Latin-1 completa (acentos, ñ, ¿¡) y UTF-8.

```c
#define SYS_LOAD_FONT     189  // a0=familia, a1=alto px, a2=negrita, a3=cursiva, a4=subrayado -> handle (0 si no hay hueco)
#define SYS_FREE_FONT     190  // a0=handle
#define SYS_SET_FONT      191  // a0=handle (0 = volver a la de sistema)
#define SYS_FONT_WIDTH    195  // -> avance de la 'n' con la fuente activa (5 con la de sistema)
#define SYS_FONT_HEIGHT   196  // -> alto de linea de la fuente activa (7 con la de sistema)
#define SYS_TEXT_WIDTH    254  // a0=cadena UTF-8 -> ancho en pixeles con la fuente activa
#define SYS_FONT_ADVANCES 255  // a0=handle, a1=buffer de 256 bytes -> avance de cada codepoint 0..255;
                               // devuelve (ascent << 16) | alto_linea
```

- `SYS_LOAD_FONT("sans", 12, ...)` elige la cara **más cercana** que
  exista (familia → estilo → tamaño). `"sistema"`, `"system"`,
  `"fixed"` o `"5x7"` piden la 5x7 de siempre. Una familia desconocida
  cae a `sans`.
- Con una fuente activa, `SYS_DRAW_TEXT` dibuja con ella: `(x, y)` es
  la esquina superior izquierda de la línea; la cadena es UTF-8 (un
  byte suelto se acepta como Latin-1). El subrayado, si se pidió al
  cargar, lo dibuja el kernel.
- **Sin fuente activa, `SYS_DRAW_TEXT` sigue siendo la 5x7 en
  mayúsculas** — a propósito: `editor.pro`, `ide.pro` y `shell.pro`
  colocan el cursor a columna×6 px contando con ella. Un programa que
  quiera texto real debe pedir su fuente.
- Hay **16 huecos** de fuente en el kernel para todos los programas a
  la vez. Libera las que no uses (`SYS_FREE_FONT`); un visor que
  necesite muchas combinaciones debe cachearlas con desalojo (así lo
  hace `visor.lua`).
- Para maquetar sin una syscall por palabra: `SYS_FONT_ADVANCES` una
  vez por fuente, y sumar avances en el propio programa. Desde Lua:
  `gui.avances_fuente(h)` y `gui.medir_con_avances(t, s)`.
- El escritorio (títulos, gadgets, menús, diálogos) usa sans 12 por su
  cuenta; no depende de la fuente activa del programa.

## 9. Syscalls de sistema (desde la Semana 8)

Para monitores, gestores de tareas y utilidades:

```c
#define SYS_NEMOFS_TYPE_BY_INODE 247 // a0=inodo -> 1 archivo, 2 carpeta, -1 no existe
#define SYS_TASK_LIST            248 // a0=buffer, a1=max entradas -> n; 48 bytes por entrada:
                                     // slot(u32) ventana(u32) turnos(u32) nombre(36 bytes)
#define SYS_TASK_KILL            249 // a0=slot -> 0 ok / -1 (cierra su ventana y gadgets)
#define SYS_DISK_USAGE           250 // -> (total_bloques << 32) | usados, bloques de 512 bytes (NemoFS)
#define SYS_RAM_TASKS            251 // -> (en_uso << 32) | total, bloques de tarea de 16 MB
#define SYS_RAM_HEAP             252 // -> (usado << 32) | total, heap del kernel (64 MB)
#define SYS_LOAD_IMAGE_EN        253 // a0=nombre, a1=inodo de carpeta -> handle de imagen .nimg
```

`SYS_LOAD_IMAGE` (49) solo busca en la raíz y en `DOCUMENTOS`; para una
imagen en cualquier otra carpeta, `SYS_LOAD_IMAGE_EN`.

## 10. Lo que cambió con la separación kernel/programas

Desde las Semanas 10-11 los programas corren en **EL0**, cada uno con
su propia tabla de traducción. Para quien escribe programas:

- Tocar memoria fuera de tu área → la tarea se retira sola con un
  volcado en la UART (`Data Abort desde EL0`, con `FAR_EL1` y el
  desplazamiento). El sistema sigue.
- `SYS_EXIT` (0) termina la tarea de verdad y **no vuelve**. Terminar
  con `ret` también vale (ver `GUIA_EJECUCION_PROGRAMAS_NEMO_OS.md`).
- Punteros a syscalls: validados (ver el aviso de la sección 1).
- Nada de esto exige cambiar programas existentes: los `.pro` de
  Nemo Basic de antes de la Fase 1 siguen funcionando igual.

## 11. Más gotchas recogidos en la Semana 8 (programando en Lua)

- **`SYS_GADGET_EVENT` (105+) y `SYS_POLL_EVENT` (8) comparten la misma
  cola de un solo hueco.** En una ventana en modo evento, léelo todo
  con `SYS_POLL_EVENT` y reparte por código; nunca los dos en la misma
  vuelta, o el primero se come el evento del segundo.
- **`TECLA_ENTER` es 10**, no 13 (así lo entrega el driver de teclado).
- **`SYS_FILE_OPEN` (20) crea el archivo si no existe.** Para comprobar
  existencia sin crear, `SYS_FIND_CHILD` (o `SYS_FILE_LIST`).
- **`SYS_DIR_OPEN`/`SYS_FIND_CHILD` solo entienden NemoFS**; para saber
  si un inodo es carpeta, `SYS_NEMOFS_TYPE_BY_INODE`.
- **`SYS_DRAW_IMAGE` es la 50** (no la 128 que decía una versión
  antigua de la librería Lua).
- El glifo `/` faltaba en la 5x7 hasta la Semana 8; ya está.
