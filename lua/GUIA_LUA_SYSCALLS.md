# Referencia de syscalls para Lua en Nemo OS

Complemento de la guía de programación Lua (`GUIA_LUA.md`): todas las
syscalls del kernel, con su número y sus argumentos, para llamarlas con
`nemo.syscall(numero, ...)`. Las más habituales ya tienen función con
nombre en `nemo_gui` y `nemo_sistema`; ver la sección 4b de la guía.


Generada de `src/syscall.h`. `a0`..`a4` son los argumentos en orden;
`->` es el valor devuelto. Donde el comentario original del kernel es
escueto se ha completado leyendo `syscall.c`. **Si algo no cuadra, el
header manda.**

### Sistema

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 0 | `SYS_EXIT` | -- Sistema (0-9) -- cierra la ventana del programa; debe terminar |
| 1 | `SYS_SLEEP` | en su siguiente SYS_PUMP/SYS_READ_CHAR_WAIT a0 = ticks a esperar (100 ticks = 1 segundo) |
| 2 | `SYS_GET_TICKS` | sin argumentos; devuelve el contador de ticks del sistema |
| 3 | `SYS_CLIPBOARD_SET` | a0=puntero a texto -> copia al portapapeles del sistema (compartido entre programas). |
| 4 | `SYS_CLIPBOARD_GET` | a0=buffer de destino, a1=tamaño max -> bytes copiados (0 si vacio). |
| 5 | `SYS_LAUNCH_PROGRAM` | a0=puntero al nombre del .pro, a1=puntero a un argumento (o 0). Lanza otro programa en una ventana nueva. |
| 6 | `SYS_GET_LAUNCH_ARG` | Sin argumentos. Devuelve en a0 el buffer de salida (a0=buffer, a1=tamaño max) el argumento con el que se lanzo el programa actual (cadena vacia si no tenia ninguno). Devuelve la longitud copiada. |
| 7 | `SYS_READ_CONSOLE_OUTPUT` | a0=buffer, a1=tamaño max -> bytes. Lee la salida de consola encolada del programa hijo lanzado con 'run'. |
| 8 | `SYS_POLL_EVENT` | Sin argumentos -> id del evento pendiente de la ventana propia (0 si ninguno). Codigos: 0x401 gadget, 0x802 redimension, 0x803 cierre, 0x1001 menu, 0x4001 temporizador. |
| 9 | `SYS_GET_EVENT_INFO` | Sin argumentos -> (fuente<<32 |  datos) del ULTIMO evento leido con POLL_EVENT. fuente = id de gadget/menu que lo disparo. |

### Consola y texto

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 10 | `SYS_WRITE_CHAR` | -- Texto / consola (10-19) -- a0=caracter |
| 11 | `SYS_WRITE_STRING` | a0=puntero a string terminado en \0 |
| 12 | `SYS_READ_CHAR` | sin argumentos; devuelve 0 si no hay tecla pendiente |
| 13 | `SYS_READ_CHAR_WAIT` | espera bloqueando hasta que haya una tecla |
| 14 | `SYS_PUMP` | Sin argumentos. Cede el procesador al resto del sistema un paso; hay que llamarla en cada vuelta de cualquier bucle. Devuelve <0 si la ventana se ha cerrado. |

### Archivos (NemoFS/FAT)

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 20 | `SYS_FILE_OPEN` | a0=puntero a nombre, a1=inodo padre (0=raiz, ignorado en FAT -- v1 de FAT solo tiene raiz), a2=volumen. Crea el archivo si no existe (solo en NemoFS; en FAT, "abrir" uno que no existe reserva un hueco para poder escribirlo despues, pero no lo crea hasta que... |
| 21 | `SYS_FILE_READ` | a0=identificador (de SYS_FILE_OPEN), a1=buffer, a2=tamaño max, a3=volumen |
| 22 | `SYS_FILE_WRITE` | a0=identificador, a1=buffer, a2=tamaño, a3=volumen. NOTA: en FAT v1 esto solo funciona para archivos NUEVOS (los que SYS_FILE_OPEN no encontro) -- el driver FAT todavia no soporta sobreescribir un archivo existente. |
| 23 | `SYS_FILE_LIST` | a0=inodo padre (0=raiz), a1=buffer de salida, a2=maximo de entradas, a3=volumen -> cantidad. Cada entrada: 32 bytes de nombre + 4 tipo + 4 inodo + 4 tamaño (ver nemofs.h). |
| 24 | `SYS_DIR_CREATE` | a0=puntero a nombre, a1=inodo padre, a2=volumen. Crea una carpeta. Solo soportado en NemoFS -- FAT v1 no tiene subcarpetas, asi que con volumen=VOLUME_FAT esto siempre falla. Devuelve el inodo nuevo, o -1. |
| 25 | `SYS_FILE_DELETE` | a0=puntero a nombre, a1=inodo padre, a2=volumen. Borra un archivo o una carpeta VACIA. Solo soportado en NemoFS por ahora -- FAT siempre falla (v1 del driver FAT no soporta borrar). Devuelve 0 si tuvo exito, -1 si no. |
| 26 | `SYS_FILE_CLIPBOARD_SET` | a0=puntero a nombre, a1=inodo padre, a2=volumen. Portapapeles de ARCHIVOS (copiar/pegar entre carpetas). |
| 27 | `SYS_FILE_CLIPBOARD_GET` | a0=buffer de salida para el nombre, a1=tamaño max. Devuelve (inodo_padre<<32 |  volumen), o -1 si el portapapeles de archivos esta vacio. |
| 28 | `SYS_DEBUG_LOG` | DebugLog(texto$) -- escribe SIEMPRE por UART/terminal, sin pasar por la redireccion de consola que usa Print (util para depurar aunque el programa este redirigiendo su salida a otro sitio). |

### Archivos generales, carpetas, utilidades por nombre

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 70 | `SYS_GENFILE_OPEN` | a0=puntero a nombre, a1=modo (0 leer, 1 escribir, 2 leer+escribir) -> handle, o -1. |
| 71 | `SYS_GENFILE_READ_BYTES` | a0=handle, a1=buffer, a2=cantidad -> bytes leidos de verdad (puede ser menos si llega al final) |
| 72 | `SYS_GENFILE_WRITE_BYTES` | a0=handle, a1=buffer, a2=cantidad -> 0 si ok, -1 si no cabe |
| 73 | `SYS_GENFILE_POS` | a0=handle -> posicion actual |
| 74 | `SYS_GENFILE_SEEK` | a0=handle, a1=nueva_posicion -> 0 si ok, -1 si fuera de rango |
| 75 | `SYS_GENFILE_SIZE` | a0=handle -> tamaño actual del contenido |
| 76 | `SYS_GENFILE_EOF` | a0=handle -> 1 si no queda nada por leer |
| 77 | `SYS_GENFILE_CLOSE` | a0=handle -- vuelca a disco si hizo falta, libera el hueco |
| 78 | `SYS_DIR_OPEN` | a0=puntero a nombre de carpeta -> handle de iteracion, o -1. |
| 79 | `SYS_DIR_NEXT` | a0=handle, a1=buffer de salida, a2=tamaño max -> longitud del nombre (0 si no quedan mas) |
| 80 | `SYS_DIR_CLOSE` | a0=handle |
| 81 | `SYS_FILE_SIZE_BY_NAME` | a0=puntero a nombre -> tamaño en bytes, o -1 si no existe (busca en NemoFS raiz, DOCUMENTOS y FAT). |
| 82 | `SYS_FILE_TYPE_BY_NAME` | a0=puntero a nombre -> 0=no existe, 1=archivo, 2=carpeta |
| 83 | `SYS_FIND_CHILD` | a0=puntero a nombre, a1=inodo padre -> inodo (solo NemoFS), o -1 |
| 84 | `SYS_DELETE_ANYWHERE` | a0=puntero a nombre -> 0 si se borro, -1 si no se encontro en ningun sitio |

### Archivos (lectura de bytes)

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 136 | `SYS_READ_FILE_READ_BYTES` | Lectura a nivel de byte para handles de ReadFile (a0=handle, a1=puntero destino, a2=cuantos bytes) -- para que ReadByte/ReadShort/ReadInt/ ReadFloat/ReadString$ funcionen tambien con handles de ReadFile, no solo de OpenFile/WriteFile. |

### Ventana, dibujo y raton

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 30 | `SYS_DRAW_RECT` | -- Graficos y ventana (30-49) -- a0=x, a1=y, a2=ancho, a3=alto, a4=color |
| 31 | `SYS_DRAW_TEXT` | a0=x, a1=y, a2=puntero a string, a3=color |
| 32 | `SYS_DRAW_ICON` | a0=x, a1=y, a2=id de icono, a3=escala (0 o 1 = tamaño normal 24x24; 2 = doble, etc.) |
| 33 | `SYS_GET_WINDOW_SIZE` | devuelve (ancho<<32 |  alto) de la ventana propia |
| 34 | `SYS_GET_MOUSE` | Sin argumentos -> (x<<32 |  y<<8 |  botones) relativo a la ventana propia; -1 si el cursor esta fuera o la ventana no tiene el foco. |
| 35 | `SYS_GET_SCREEN_SIZE` | sin argumentos; devuelve (ancho<<32 |  alto) de toda la pantalla |
| 36 | `SYS_DEFINE_BUTTON` | a0=id de boton (numero libre), a1=x, a2=y, a3=(ancho<<16| alto), a4=puntero a texto. Boton 'antiguo' dibujado por el programa; maximo 8 por ventana. Preferir CREATE_BUTTON (100). |
| 37 | `SYS_GET_BUTTON_ID` | Sin argumentos. Devuelve el id del boton que se acaba de pulsar en esta ventana (0 si ninguno). Hay que llamarla una vez por vuelta del bucle principal del programa, igual que SYS_READ_CHAR. |
| 38 | `SYS_OPEN_FILE_DIALOG` | a0=carpeta de inicio (inodo), a1=buffer de salida para el nombre elegido, a2=tamaño max -> (carpeta<<32 \| inodo), o -1 si se cancelo. **a2 es el tamaño del buffer, no un filtro de extension**: no existe tal argumento. El kernel dibuja y gestiona el dialogo entero, y BLOQUEA hasta que el usuario elige o cancela. Tiene el aspecto de los dialogos clasicos: barra de titulo, "Buscar en:" con la ruta desplegable, barra de lugares, lista con iconos y tamaños, filtro de tipo y botones Abrir/Cancelar. Ocupa un rectangulo centrado y devuelve el lienzo tal y como estaba al salir, asi que repintar despues es opcional. Se elige con doble clic, con Enter, o con el boton Abrir; tambien se puede teclear el nombre. |
| 39 | `SYS_SAVE_FILE_DIALOG` | Misma firma que OPEN_FILE_DIALOG, para 'Guardar como'. |
| 285 | `SYS_CPU_NOMBRE` | a0=buffer, a1=tamaño maximo. Escribe el nombre del procesador leido de MIDR_EL1 ("ARM Cortex-A72 (Raspberry Pi 4)") y devuelve cuantos caracteres escribio. No esta cableado: sale del registro de identificacion, asi que acierta tambien en QEMU (Cortex-A53). |
| 286 | `SYS_CPU_DATOS` | sin argumentos -> (nucleos en marcha<<32 \| MHz del procesador). **MHz = 0 significa "no se puede saber"**, no "va a 0": en QEMU no existe el mailbox del firmware al que preguntarselo. |
| 287 | `SYS_RAM_FISICA` | sin argumentos -> RAM **fisica** de la placa, en bytes, la que detecta memoria.c al arrancar. No confundir con SYS_RAM_POOL (la reserva de tareas) ni con SYS_RAM_HEAP (el monton del nucleo): esos dos son repartos hechos DENTRO de esta. |
| 288 | `SYS_RAM_NUCLEO` | sin argumentos -> bytes que ocupa el nucleo cargado en memoria (codigo + datos + .bss), medido entre _start y __bss_end. Sale mucho mayor que el kernel8.img del disco porque los arrays estaticos grandes (monton, reserva de tareas, lienzos de ventanas) viven en .bss, que no viaja en el archivo. |
| 40 | `SYS_CREATE_WINDOW` | a0=puntero a titulo, a1=x, a2=y, a3=ancho, a4=alto -> indice de ventana. Personaliza la ventana que la tarea ya tiene y la pone en modo evento (la X dispara 0x803 en vez de cerrar). |
| 41 | `SYS_READ_FILE_OPEN` | -- ReadFile/ReadLine$/Eof/CloseFile: lectura de archivos de texto linea a linea (busca primero en la raiz, luego en DOCUMENTOS) -- a0=puntero a nombre -> handle, o -1 si no existe |
| 42 | `SYS_READ_FILE_LINE` | a0=handle, a1=buffer salida, a2=tamaño max -> longitud |
| 43 | `SYS_READ_FILE_EOF` | a0=handle -> 1 si no queda nada por leer |
| 44 | `SYS_READ_FILE_CLOSE` | a0=handle |
| 45 | `SYS_GET_MOUSE_WHEEL` | Sin argumentos -> acumulado de la rueda desde la ultima llamada (se resetea al leer). |
| 46 | `SYS_GRAPHICS_MODE` | a0=ancho, a1=alto. Modo grafico clasico: NO activa el modo evento, la X cierra la ventana directamente. |
| 47 | `SYS_DRAW_OVAL` | Oval(x, y, ancho, alto, solido) -- elipse rasterizada por filas, sin necesitar ninguna libreria de coma flotante (raiz cuadrada entera propia). a4=1 rellena, a4=0 solo el contorno. |
| 48 | `SYS_KEY_DOWN` | a0=codigo de tecla (estilo evdev) -> 1 si esta pulsada ahora mismo. |
| 49 | `SYS_LOAD_IMAGE` | a0=puntero a nombre -> handle de imagen NIMG, o -1. |
| 85 | `SYS_SET_ORIGIN` | a0=x, a1=y. Desplaza el origen de TODO el dibujo posterior. |
| 86 | `SYS_GET_PIXEL` | a0=x, a1=y -> color 0xRRGGBB (0 si fuera de rango) -- respeta el origen actual |
| 87 | `SYS_COPY_RECT` | a0=x1, a1=y1, a2=ancho, a3=alto, a4=(x2<<32| y2) -- respeta el origen actual |

### Ventanas (foco, maximizar)

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 130 | `SYS_SET_TITLE` | a0=puntero a texto -- cambia el titulo de la ventana actual |
| 150 | `SYS_ACTIVATE_WINDOW` | a0=indice de ventana. Da el foco a esa ventana. |
| 151 | `SYS_ACTIVE_WINDOW` | -> indice de la ventana con foco |
| 152 | `SYS_MAXIMIZE_WINDOW` | a0=indice de ventana. Maximiza. |
| 153 | `SYS_MINIMIZE_WINDOW` | a0=indice de ventana |
| 154 | `SYS_WINDOW_MAXIMIZED` | a0=indice -> 1/0 |
| 155 | `SYS_WINDOW_MINIMIZED` | a0=indice -> 1/0 |
| 156 | `SYS_SET_MIN_WINDOW_SIZE` | a0=indice, a1=ancho (0=actual), a2=alto (0=actual) |

### Dibujo avanzado, pixeles, canvas

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 128 | `SYS_SET_IMAGE_BUFFER` | a0=handle de imagen (-1 = volver a la ventana). Redirige el dibujo a esa imagen. |
| 135 | `SYS_SET_VIEWPORT` | a0=x, a1=y, a2=ancho, a3=alto. Recorta el dibujo a ese rectangulo; ancho o alto 0 lo desactiva. |
| 185 | `SYS_READ_PIXEL` | a0=x, a1=y, a2=buffer (0=ventana, N=imagen N-1) -> color. |
| 186 | `SYS_WRITE_PIXEL` | a0=x, a1=y, a2=argb, a3=buffer |
| 187 | `SYS_COPY_PIXEL` | a0=(src_x<<16| src_y), a1=src_buffer, a2=(dest_x<<16| dest_y), a3=dest_buffer |
| 188 | `SYS_CREATE_CANVAS` | a0=x, a1=y, a2=(ancho<<16| alto) -> id de canvas. |

### Imagenes

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 50 | `SYS_DRAW_IMAGE` | a0=handle, a1=x, a2=y |
| 51 | `SYS_IMAGE_SIZE` | a0=handle -> (ancho<<32 |  alto), o 0 si el handle no es valido |
| 52 | `SYS_CREATE_IMAGE` | CreateImage(ancho, alto) -- lienzo VACIO (transparente) en el mismo pool que LoadImage, para dibujar sobre el en vez de cargarlo de un archivo. Devuelve un handle igual que LoadImage. |
| 88 | `SYS_FREE_IMAGE` | a0=handle. Libera la imagen. |
| 89 | `SYS_SET_IMAGE_HANDLE` | a0=handle, a1=x, a2=y |
| 90 | `SYS_GET_IMAGE_HANDLE` | a0=handle -> (handle_x<<32 |  handle_y) |
| 91 | `SYS_SET_AUTO_MID_HANDLE` | a0=0/1 |
| 92 | `SYS_MASK_IMAGE` | a0=handle, a1=color 0xRRGGBB -> ese color pasa a transparente |
| 93 | `SYS_COPY_IMAGE` | a0=handle -> handle nuevo, o -1 |
| 94 | `SYS_SAVE_IMAGE` | a0=handle, a1=puntero a nombre -> 0 si ok, -1 si no |
| 95 | `SYS_GRAB_IMAGE` | a0=x, a1=y, a2=ancho, a3=alto -> handle nuevo, o -1 |
| 96 | `SYS_RESIZE_IMAGE` | a0=handle, a1=nuevo_ancho, a2=nuevo_alto -> 0/-1 -- EN EL MISMO HUECO |
| 97 | `SYS_ROTATE_IMAGE` | a0=handle, a1=bits del angulo en grados (double) -> 0/-1 -- EN EL MISMO HUECO |
| 98 | `SYS_DRAW_IMAGE_RECT` | a0=handle, a1=x, a2=y, a3=(rx<<16| ry), a4=(rw<<16| rh) -- dibuja solo ese sub-rectangulo |
| 99 | `SYS_LOAD_ANIM_IMAGE` | a0=puntero a nombre, a1=(cell_w<<16| cell_h), a2=first, a3=count -> handle, o -1 |

### Fuentes

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 189 | `SYS_LOAD_FONT` | a0=puntero a nombre, a1=alto -> handle. Ver limitacion de fuente unica en syscall.c. |
| 190 | `SYS_FREE_FONT` | a0=handle |
| 191 | `SYS_SET_FONT` | a0=handle |
| 192 | `SYS_FONT_NAME` | a0=handle, a1=buffer, a2=tamaño max |
| 193 | `SYS_FONT_SIZE` | a0=handle -> alto PEDIDO al cargar (metadato, no el real en pantalla) |
| 194 | `SYS_FONT_STYLE` | a0=handle -> 1=negrita, 2=cursiva, 3=las dos, 0=ninguna (bits independientes; arreglado en la auditoria de Nemo-Blitz -- antes nunca miraba la negrita) |
| 195 | `SYS_FONT_WIDTH` | Sin argumentos -> ancho del GLIFO de la fuente fija (5). Para calcular columnas usar FONT_CHAR_ADVANCE (206). |
| 196 | `SYS_FONT_HEIGHT` | Sin argumentos -> alto real de la fuente fija (7). |
| 206 | `SYS_FONT_CHAR_ADVANCE` | Sin argumentos -> avance de cursor por caracter de la fuente activa (el ancho REAL entre letras). |

### Gamma y modo grafico

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 197 | `SYS_SET_GAMMA` | a0=canal, a1=indice, a2=r, a3=g, a4=b. Tabla gamma (limitada, ver syscall.c). |
| 198 | `SYS_UPDATE_GAMMA` | a0=calibrate (se acepta, no-op) |
| 199 | `SYS_GAMMA_RED` | a0=red -> salida |
| 200 | `SYS_GAMMA_GREEN` | a0=green -> salida |
| 201 | `SYS_GAMMA_BLUE` | a0=blue -> salida |
| 202 | `SYS_GFX_DRIVER_NAME` | a0=buffer, a1=tamaño -> nombre del driver grafico (valor nominal). |
| 203 | `SYS_GFX_MODE_FORMAT` | a0=modo (ignorado) -> mismo valor que GraphicsFormat |
| 204 | `SYS_GRAPHICS_FORMAT` | -> formato de pixel (4 = 32 bits RGB, byte alto sin usar) |
| 205 | `SYS_TOTAL_VID_MEM` | -> bytes "disponibles" (valor nominal fijo) |

### Teclado y raton (por flanco)

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 53 | `SYS_KEY_HIT` | a0=codigo -> cuantas veces se pulso desde la ultima llamada (consume). |
| 54 | `SYS_GET_KEY` | -> siguiente scancode pulsado en la cola, o 0 si no hay ninguno pendiente |
| 55 | `SYS_FLUSH_KEYS` | vacia todas las colas/banderas de teclado (caracteres, scancodes, KeyHit) |
| 56 | `SYS_MOUSE_HIT` | a0=boton (1=izq,2=der) -> 1 si se pulso desde la ultima vez, 0 si no (consume el aviso) |
| 57 | `SYS_GET_MOUSE_SPEED` | -> (dx<<32 |  dy) desde la ultima vez que se llamo, cada uno con su propio acumulado |
| 58 | `SYS_MOVE_MOUSE` | a0=x, a1=y -> fuerza la posicion del cursor (temporal, ver mouse_move_to) |

### Raton, eventos y atajos

| Nº | Syscall | Argumentos y valor devuelto |
|---|---|---|
| 137 | `SYS_GET_MOUSE_Z` | MouseZ() -- posicion ACUMULADA de la rueda desde que arranco el programa (nunca se resetea al leerla), a diferencia de SYS_GET_MOUSE_WHEEL (que es un delta que SI se consume). |
| 138 | `SYS_FLUSH_MOUSE` | FlushMouse -- comando SEPARADO de FlushKeys en BlitzPlus real, solo limpia las pulsaciones de boton en cola. |
| 139 | `SYS_PEEK_EVENT` | PeekEvent()/FlushEvents([id]) -- confirmados contra la documentacion real de BlitzPlus. -> id del evento pendiente, sin consumirlo (0 si no hay ninguno) |
| 140 | `SYS_FLUSH_EVENTS` | a0=id (0=cualquiera) -- descarta el evento pendiente si coincide |
| 143 | `SYS_HOTKEY_EVENT` | a0=(tecla<<8| modificador), a1=id de evento a disparar (0=quitar), a2=datos, a3=fuente. |


Continúa en [GUIA_LUA_SYSCALLS_2.md](GUIA_LUA_SYSCALLS_2.md): gadgets,
menús, áreas de texto, barras de herramientas, árboles, temporizadores,
reloj y bancos de memoria.
