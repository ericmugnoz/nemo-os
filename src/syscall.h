// syscall.h — Nemo OS
//
// Numeracion organizada por rangos, al estilo MenuetOS (ver
// sysfuncs.txt de referencia): cada bloque de 10-20 numeros es una
// categoria, con hueco de sobra para ampliar sin tener que renumerar
// nada despues. Los rangos 50-99 quedan reservados para cuando
// construyamos sonido, multitarea real, red y acceso a PCI -- no
// tiene sentido definir esos numeros ya si no hay nada detras
// todavia.
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stdbool.h>

// Bug real corregido: la cola de consola redirigida se indexa por
// ventana y esos indices se reciclan -- sin resetear, una ventana
// nueva hereda bytes sin leer de una tarea anterior distinta.
void console_queue_reset(int32_t window_idx);

// -- Sistema (0-9) --
// Termina la tarea actual: cierra su ventana, libera sus gadgets, y NO
// VUELVE. Desde la Fase 1 de la separacion kernel/programas las tareas
// corren en EL0, y desde EL0 no se puede "volver" al kernel con un
// ret -- asi que el ret final del _start de cualquier programa cae en
// un stub (copiado al final de su area de programa por tasks.c) que
// pide esta syscall. Los programas existentes no cambian nada. (Antes
// de la Fase 1 solo cerraba la ventana y volvia; el programa hacia
// ret despues. nb_runtime_nemo.c ya se protegia con un bucle "por si
// SYS_EXIT no detiene la tarea de inmediato" -- ahora si la detiene.)
#define SYS_EXIT       0
                           // en su siguiente SYS_PUMP/SYS_READ_CHAR_WAIT
#define SYS_SLEEP      1  // a0 = ticks a esperar (100 ticks = 1 segundo)
#define SYS_GET_TICKS  2  // sin argumentos; devuelve el contador de ticks del sistema

// Portapapeles compartido por todo el sistema -- lo que copies en un
// programa se puede pegar en otro, exactamente como esperarias.
#define SYS_CLIPBOARD_SET 3 // a0=puntero a texto, a1=longitud
#define SYS_CLIPBOARD_GET 4 // a0=buffer de destino, a1=tamaño max; devuelve
                             // cuantos bytes copio (0 si el portapapeles esta vacio)

// Pide al kernel que lance otro programa en una ventana nueva -- lo
// mismo que hace un icono de escritorio, pero desde codigo. Util para
// "abrir con": por ejemplo, el explorador pidiendo abrir un .txt con
// el editor. a0=puntero al nombre del .pro, a1=puntero a un argumento
// de texto opcional (o 0 si no aplica), a2=carpeta desde la que
// buscarlo primero (por ejemplo, la carpeta actual de una shell), o
// 0xFFFFFFFF si no aplica -- en ese caso se busca en la raiz y luego
// en PROGRAMAS, como hasta ahora.
#define SYS_LAUNCH_PROGRAM 5

// Sin argumentos. Devuelve en a0 el buffer de salida (a0=buffer,
// a1=tamaño max) el argumento con el que se lanzo el programa actual
// (cadena vacia si no tenia ninguno). Devuelve la longitud copiada.
#define SYS_GET_LAUNCH_ARG 6

// Si el programa que llama fue lanzado con 'run' desde una shell (u
// otro programa con ventana), su SYS_WRITE_STRING no va a la UART --
// se encola aqui para que quien lo lanzo la vaya leyendo con esta
// syscall, caracter a caracter, e integrarla en su propia consola
// (evita que dos programas dibujen a la vez sobre la misma ventana).
// Sin argumentos. Devuelve un caracter (0 si no hay ninguno pendiente).
#define SYS_READ_CONSOLE_OUTPUT 7

// Sistema de eventos estilo BlitzPlus (WaitEvent/EventID/EventSource/
// EventData) -- ver gadgets.h para los codigos de evento ($401, $802,
// $803, $1001...). SYS_POLL_EVENT consume el evento pendiente de la
// ventana propia (0 si no hay ninguno) y cachea su fuente/datos.
// SYS_GET_EVENT_INFO devuelve esos datos cacheados del ULTIMO evento
// consultado: (fuente<<32 | datos), cada mitad de 32 bits.
#define SYS_POLL_EVENT       8
#define SYS_GET_EVENT_INFO   9
#define SYS_GET_EVENT_XY     256 // a0=(sin usar) -> (x<<32)|y del ultimo
                                  // evento CONSUMIDO -- companero de
                                  // SYS_GET_EVENT_INFO, arregla H_EVENTXY

// -- Texto / consola (10-19) --
#define SYS_WRITE_CHAR      10 // a0=caracter
#define SYS_WRITE_STRING    11 // a0=puntero a string terminado en \0
#define SYS_READ_CHAR       12 // sin argumentos; devuelve 0 si no hay tecla pendiente
#define SYS_READ_CHAR_WAIT  13 // espera bloqueando hasta que haya una tecla
                                // (sin congelar el resto del sistema)
#define SYS_PUMP            14 // avanza el resto del sistema un paso sin
                                // esperar teclado. Devuelve -1 si la
                                // ventana del programa se cerro.

// -- Archivos, NemoFS y FAT (20-29) --
//
// Todas estas syscalls aceptan un "volumen" para poder operar sobre
// cualquiera de los dos discos con la MISMA interfaz -- asi cualquier
// programa (el explorador, un futuro gestor de copias...) puede tocar
// ambos discos sin necesitar dos juegos de syscalls distintos.
#define VOLUME_NEMOFS 0
#define VOLUME_FAT    1

// a0=puntero a nombre, a1=inodo padre (0=raiz, ignorado en FAT -- v1
// de FAT solo tiene raiz), a2=volumen. Crea el archivo si no existe
// (solo en NemoFS; en FAT, "abrir" uno que no existe reserva un hueco
// para poder escribirlo despues, pero no lo crea hasta que llegue el
// primer SYS_FILE_WRITE). Devuelve un identificador (inodo o handle
// FAT segun el volumen) para usar en las siguientes llamadas.
#define SYS_FILE_OPEN  20

// a0=identificador (de SYS_FILE_OPEN), a1=buffer, a2=tamaño max,
// a3=volumen
#define SYS_FILE_READ  21

// a0=identificador, a1=buffer, a2=tamaño, a3=volumen.
//
// En FAT sobrescribe reescribiendo el archivo ENTERO (fat_write_file
// borra la entrada vieja y la recrea), asi que el 'size' que se pasa
// pasa a ser el tamaño definitivo del archivo: no es una escritura
// incremental ni se puede escribir a partir de un desplazamiento.
// En NemoFS, igual.
//
// (El comentario anterior decia que solo funcionaba para archivos
// NUEVOS. Eso dejo de ser cierto cuando se añadio fat_write_file y
// nadie actualizo la nota.)
#define SYS_FILE_WRITE 22

// a0=inodo padre (0=raiz; ignorado en FAT), a1=puntero a buffer de
// salida, a2=maximo de entradas que caben en el buffer, a3=volumen.
// Devuelve el numero total de entradas en la carpeta (puede ser mayor
// que a2).
// Cada entrada ocupa 40 bytes:
//   offset 0:  uint32_t inode (o 0 en FAT, que no tiene inodos)
//   offset 4:  uint32_t type   (0=libre, 1=archivo, 2=carpeta)
//   offset 8:  uint32_t size
//   offset 12: char name[28]   (terminado en \0)
#define SYS_FILE_LIST  23

// a0=puntero a nombre, a1=inodo padre, a2=volumen. Crea una carpeta.
// Solo soportado en NemoFS -- FAT v1 no tiene subcarpetas, asi que con
// volumen=VOLUME_FAT esto siempre falla. Devuelve el inodo nuevo, o -1.
#define SYS_DIR_CREATE 24

// a0=puntero a nombre, a1=inodo padre, a2=volumen. Borra un archivo o
// una carpeta VACIA. Solo soportado en NemoFS por ahora -- FAT
// siempre falla (v1 del driver FAT no soporta borrar). Devuelve 0 si
// tuvo exito, -1 si no.
#define SYS_FILE_DELETE 25

// Portapapeles de ARCHIVOS (distinto del de texto de mas arriba) --
// para poder copiar/pegar un archivo entero desde un menu contextual,
// por ejemplo. a0=puntero a nombre, a1=inodo padre, a2=volumen.
#define SYS_FILE_CLIPBOARD_SET 26

// a0=buffer de salida para el nombre, a1=tamaño max. Devuelve
// (inodo_padre<<32 | volumen), o -1 si el portapapeles de archivos
// esta vacio.
#define SYS_FILE_CLIPBOARD_GET 27

// DebugLog(texto$) -- escribe SIEMPRE por UART/terminal, sin pasar
// por la redireccion de consola que usa Print (util para depurar
// aunque el programa este redirigiendo su salida a otro sitio).
#define SYS_DEBUG_LOG 28

// -- Graficos y ventana (30-49) --
#define SYS_DRAW_RECT       30 // a0=x, a1=y, a2=ancho, a3=alto, a4=color
#define SYS_DRAW_TEXT       31 // a0=x, a1=y, a2=puntero a string, a3=color
#define SYS_DRAW_ICON       32 // a0=x, a1=y, a2=id de icono, a3=escala (0 o 1 = tamaño normal 24x24; 2 = doble, etc.)
#define SYS_GET_WINDOW_SIZE 33 // devuelve (ancho<<32 | alto) de la ventana propia

// Devuelve el estado del raton relativo a la ventana del programa,
// solo si el cursor esta dentro de ella y tiene el foco. Si no,
// devuelve -1 (0xFFFFFFFFFFFFFFFF).
// bits[47:32]=x local, bits[31:16]=y local, bit0=boton izq, bit1=boton der.
#define SYS_GET_MOUSE       34

// Rueda del raton -- devuelve el acumulado desde la ultima llamada, ya
// sin importar de que ventana venga (a diferencia de SYS_GET_MOUSE, no
// depende del foco -- da igual, un programa solo la consulta cuando
// le interesa saber si hay que hacer scroll). Positivo = arriba,
// negativo = abajo. Se resetea a 0 en cada llamada, asi que hay que
// consultarla una vez por vuelta del bucle principal.
#define SYS_GET_MOUSE_WHEEL 45

// Graphics(ancho, alto) -- modo grafico clasico de BlitzPlus. A
// diferencia de CreateWindow, NO activa el modo de eventos: la X
// cierra la ventana directamente, porque un programa "Graphics"
// tipico no consulta la cola de eventos, solo dibuja y comprueba
// KeyDown/MouseDown en un bucle simple.
#define SYS_GRAPHICS_MODE 46

// Oval(x, y, ancho, alto, color) -- elipse rasterizada por filas,
// sin necesitar ninguna libreria de coma flotante (raiz cuadrada
// entera propia). SIEMPRE dibuja RELLENO: a4 es el COLOR, no un
// indicador de "solido" -- el contorno nunca llego a implementarse.
// Cuidado con pasarle 1 pensando que significa "relleno": pinta el
// color 0x000001, negro sobre negro, y no se ve nada. El color va
// entero.)
#define SYS_DRAW_OVAL 47

// Line(x0, y0, x1, y1, color) -- recta por Bresenham entero (sin
// division ni coma flotante), pixel a pixel con la misma
// wm_content_fill_rect de 1x1 que usa Plot. Mismo recorte y mismo
// desplazamiento de origen/barra de menu que Rect y Oval.
// Cerrar lo que abrio SYS_FILE_OPEN. a0=identificador,
// a1=volumen. En NemoFS no hace falta (el identificador es el inodo, no
// un hueco reservado) y devuelve 0 sin mas; en FAT libera el hueco.
// Hasta ahora NO habia forma de cerrar: los 8 huecos de FAT solo se
// soltaban al morir el programa. El explorador no muere mientras lo
// usas, asi que a la novena copia dejaba de poder abrir archivos y
// decia "el origen ya no existe" -- que era mentira.
#define SYS_FILE_CLOSE 274
#define SYS_KEY_BANK 273 // a0=banco (0..7) -> 64 bits con el estado de las teclas banco*64..banco*64+63
#define SYS_MICROS 272 // microsegundos desde el arranque (contador de ARM, no el de 100 Hz)
#define SYS_DRAW_LINE 257

// ReadLine -- leer una LINEA completa del teclado, con eco y con
// retroceso, y devolverla en el buffer del programa.
// a0=buffer de salida, a1=tamano maximo -> longitud leida, o -1 si la
// tarea no tiene consola donde leer.
//
// Existe como syscall, y no generada en el programa, porque es casi
// todo esperar y hacer eco: un bucle en C aqui es mas corto, mas
// claro, cede el turno correctamente y queda disponible para Lua y
// para cualquier otro programa. La primera version se genero dentro
// del programa y colgo el sistema entero: SYS_READ_CHAR_WAIT devuelve
// -1 cuando no hay ventana de consola, el bucle generado lo tomaba por
// un caracter, lo hacia eco y no terminaba nunca.
#define SYS_READ_LINE 258

// ConsoleBusy -- sin argumentos; devuelve 1 si OTRA tarea tiene el
// teclado tomado (esta dentro de SYS_READ_LINE esperando que el
// usuario escriba), 0 si no.
//
// La usa la shell para ESCONDER su prompt mientras un programa suyo
// pide datos: si no, se ven a la vez el "Tu numero:" del programa y
// el "C:\>" de la shell, y no hay forma de saber quien espera que
// escribas.
#define SYS_CONSOLE_BUSY 259

// KeyDown(codigo) -- codigo es el mismo numero interno que usa el
// driver de teclado (estilo Linux/evdev), no la tabla de scancodes de
// BlitzPlus real -- ver KEY_* en input.c para los valores conocidos.
#define SYS_KEY_DOWN 48

// LoadImage/DrawImage/ImageWidth+Height -- imagenes cargadas desde
// disco en nuestro propio formato "NIMG" (cabecera simple + RGBA en
// crudo, nada de PNG/JPEG de verdad -- ver la nota junto al pool de
// imagenes en syscall.c). El "handle" que devuelve LoadImage es
// sencillamente el indice dentro de ese pool.
#define SYS_LOAD_IMAGE 49 // a0=puntero a nombre -> handle, o -1 si no se pudo cargar
#define SYS_DRAW_IMAGE 50 // a0=handle, a1=x, a2=y
#define SYS_IMAGE_SIZE 51 // a0=handle -> (ancho<<32 | alto), o 0 si el handle no es valido

// CreateImage(ancho, alto) -- lienzo VACIO (transparente) en el mismo
// pool que LoadImage, para dibujar sobre el en vez de cargarlo de un
// archivo. Devuelve un handle igual que LoadImage.
#define SYS_CREATE_IMAGE 52

// Teclado/raton "por flanco" (KeyHit/GetKey/WaitKey/MouseHit) y
// velocidad relativa del raton -- complementan a SYS_KEY_DOWN y
// SYS_GET_MOUSE, que son "por nivel" (estado actual).
#define SYS_KEY_HIT 53        // a0=scancode -> 1 si se pulso desde la ultima vez que se pregunto por ELLA, 0 si no (consume el aviso)
#define SYS_GET_KEY 54        // -> siguiente scancode pulsado en la cola, o 0 si no hay ninguno pendiente
#define SYS_FLUSH_KEYS 55     // vacia todas las colas/banderas de teclado (caracteres, scancodes, KeyHit)
#define SYS_MOUSE_HIT 56      // a0=boton (1=izq,2=der) -> 1 si se pulso desde la ultima vez, 0 si no (consume el aviso)
#define SYS_GET_MOUSE_SPEED 57 // -> (dx<<32 | dy) desde la ultima vez que se llamo, cada uno con su propio acumulado
#define SYS_MOVE_MOUSE 58     // a0=x, a1=y -> fuerza la posicion del cursor (temporal, ver mouse_move_to)

// Bancos de memoria (CreateBank/PeekX/PokeX/ResizeBank/CopyBank) --
// PeekFloat/PokeFloat NO llevan syscall propia: reutilizan
// SYS_PEEK_INT/SYS_POKE_INT (los mismos 4 bytes en crudo), y el
// compilador hace la conversion simple<->doble con FCVT en su propio
// codigo generado, ya que BlitzPlus real guarda floats de 32 bits en
// los bancos, mientras que nuestros flotantes son de 64.
#define SYS_CREATE_BANK 59    // a0=tamaño -> handle, o -1 si no hay hueco/es demasiado grande
#define SYS_FREE_BANK 60      // a0=handle
#define SYS_BANK_SIZE 61      // a0=handle -> tamaño actual, o 0 si el handle no es valido
#define SYS_RESIZE_BANK 62    // a0=handle, a1=nuevo_tamaño -> 0 si ok, -1 si fallo
#define SYS_COPY_BANK 63      // a0=handle_origen, a1=offset_origen, a2=handle_destino, a3=offset_destino, a4=cantidad
#define SYS_PEEK_BYTE 64      // a0=handle, a1=offset -> valor (0 si fuera de rango)
#define SYS_PEEK_SHORT 65
#define SYS_PEEK_INT 66
#define SYS_POKE_BYTE 67      // a0=handle, a1=offset, a2=valor
#define SYS_POKE_SHORT 68
#define SYS_POKE_INT 69

// -- Archivos "generales" (OpenFile/WriteFile/lectura y escritura
// tipada) -- distintos de ReadFile/ReadLine$/Eof/CloseFile (que
// siguen igual, solo lectura, solo texto linea a linea). Estos
// soportan LECTURA Y ESCRITURA, con POSICION explicita (FilePos/
// SeekFile), cargando el archivo entero en un buffer propio al
// abrirlo (igual que ReadFile) y volcandolo a disco al cerrar si se
// modifico algo -- las llamadas a SYS_FILE_READ/WRITE de mas arriba
// no soportan posicion, solo archivo completo desde el principio.
#define SYS_GENFILE_OPEN 70   // a0=puntero a nombre, a1=modo (0=OpenFile: lee+escribe, crea si no existe; 1=WriteFile: crea vacio siempre) -> handle, o -1
#define SYS_GENFILE_READ_BYTES 71  // a0=handle, a1=buffer, a2=cantidad -> bytes leidos de verdad (puede ser menos si llega al final)
#define SYS_GENFILE_WRITE_BYTES 72 // a0=handle, a1=buffer, a2=cantidad -> 0 si ok, -1 si no cabe
#define SYS_GENFILE_POS 73    // a0=handle -> posicion actual
#define SYS_GENFILE_SEEK 74   // a0=handle, a1=nueva_posicion -> 0 si ok, -1 si fuera de rango
#define SYS_GENFILE_SIZE 75   // a0=handle -> tamaño actual del contenido
#define SYS_GENFILE_EOF 76    // a0=handle -> 1 si no queda nada por leer
#define SYS_GENFILE_CLOSE 77  // a0=handle -- vuelca a disco si hizo falta, libera el hueco

// Iteracion de carpetas (ReadDir/NextFile$/CloseDir) -- por encima de
// SYS_FILE_LIST, que ya devuelve el listado completo de una vez; aqui
// solo guardamos ese listado en un hueco propio y vamos devolviendo
// un nombre cada vez que se llama a NextFile$.
#define SYS_DIR_OPEN 78       // a0=inodo padre -> handle, o -1
#define SYS_DIR_NEXT 79       // a0=handle, a1=buffer de salida, a2=tamaño max -> longitud del nombre (0 si no quedan mas)
#define SYS_DIR_CLOSE 80      // a0=handle

// Utilidades por NOMBRE (buscan en NemoFS raiz+DOCUMENTOS y luego
// FAT, sin necesidad de abrir un handle) -- para FileSize/FileType/
// DeleteFile/DeleteDir, y para resolver una carpeta antes de abrirla
// con ReadDir o navegar con ChangeDir.
#define SYS_FILE_SIZE_BY_NAME 81  // a0=puntero a nombre -> tamaño, o -1 si no existe en ningun sitio
#define SYS_FILE_TYPE_BY_NAME 82  // a0=puntero a nombre -> 0=no existe, 1=archivo, 2=carpeta
#define SYS_FIND_CHILD 83         // a0=puntero a nombre, a1=inodo padre -> inodo (solo NemoFS), o -1
#define SYS_DELETE_ANYWHERE 84    // a0=puntero a nombre -> 0 si se borro, -1 si no se encontro en ningun sitio
// (el numero va con los nuevos, 312, porque el 85 lleva anos siendo otra cosa)
#define SYS_CLOSE_WINDOW 313      // cierra la ventana de la tarea que llama (y libera sus controles)
                                  // sin terminar la tarea -> 1 si habia ventana, 0 si no. Un
                                  // CreateWindow posterior crea otra.
#define SYS_RENAME_ANYWHERE 312   // a0=nombre actual, a1=nombre nuevo (sin carpetas) -> 0 exito, -1 fallo.
                                  // El hermano de la 84: busca el archivo donde este (ruta, raiz o
                                  // DOCUMENTOS) en vez de pedir carpeta y volumen como la 234. En FAT
                                  // devuelve -1: fat.c no sabe renombrar.

// Origin(x,y)/Viewport(x,y,w,h) -- desplazamiento aplicado dentro del
// kernel a TODAS las syscalls de dibujo (ver g_origin_x/y en
// syscall.c), asi que Plot/Rect/Line/Oval/Text/DrawImage lo respetan
// automaticamente. GetColor/CopyRect trabajan sobre el buffer de
// contenido de la ventana actual.
#define SYS_SET_ORIGIN 85  // a0=x, a1=y
#define SYS_GET_PIXEL 86   // a0=x, a1=y -> color 0xRRGGBB (0 si fuera de rango) -- respeta el origen actual
#define SYS_COPY_RECT 87   // a0=x1, a1=y1, a2=ancho, a3=alto, a4=(x2<<32|y2) -- respeta el origen actual

// Imagenes extra -- punto de agarre, mascara de transparencia,
// copiar/guardar/capturar. FreeImage/HandleImage/MidHandle etc.
#define SYS_FREE_IMAGE 88          // a0=handle
#define SYS_SET_IMAGE_HANDLE 89    // a0=handle, a1=x, a2=y
#define SYS_GET_IMAGE_HANDLE 90    // a0=handle -> (handle_x<<32 | handle_y)
#define SYS_SET_AUTO_MID_HANDLE 91 // a0=0/1
#define SYS_MASK_IMAGE 92          // a0=handle, a1=color 0xRRGGBB -> ese color pasa a transparente
#define SYS_COPY_IMAGE 93          // a0=handle -> handle nuevo, o -1
#define SYS_SAVE_IMAGE 94          // a0=handle, a1=puntero a nombre -> 0 si ok, -1 si no
#define SYS_GRAB_IMAGE 95          // a0=x, a1=y, a2=ancho, a3=alto -> handle nuevo, o -1

#define SYS_RESIZE_IMAGE 96        // a0=handle, a1=nuevo_ancho, a2=nuevo_alto -> 0/-1 -- EN EL MISMO HUECO
#define SYS_ROTATE_IMAGE 97        // a0=handle, a1=bits del angulo en grados (double) -> 0/-1 -- EN EL MISMO HUECO
#define SYS_DRAW_IMAGE_RECT 98     // a0=handle, a1=x, a2=y, a3=(rx<<16|ry), a4=(rw<<16|rh) -- dibuja solo ese sub-rectangulo
#define SYS_LOAD_ANIM_IMAGE 99     // a0=puntero a nombre, a1=(cell_w<<16|cell_h), a2=first, a3=count -> handle, o -1

// ImageBuffer(handle): redirige Rect/Plot/Line/Cls/Oval hacia una
// imagen en vez de la ventana. handle=-1 vuelve a dibujar en la
// ventana. Text y DrawImage anidado NO se redirigen (limitacion
// documentada).
#define SYS_SET_IMAGE_BUFFER 128   // a0=handle (o -1 para volver a la ventana)

// -- Varios: reloj, titulo de ventana, temporizadores por handle --
#define SYS_RTC_NOW 129        // -> timestamp Unix (segundos desde 1970-01-01 UTC), del PL031
#define SYS_RTC_CIVIL 134      // -> año/mes/dia/hora/minuto/segundo empaquetado: (año<<48 | mes<<40 | dia<<32 | hora<<24 | minuto<<16 | segundo<<8)

// Viewport(x,y,w,h): recorta el dibujo a un rectangulo, SEPARADO de
// Origin (que solo desplaza). w=0 o h=0 desactiva el recorte (vuelve
// a la ventana entera).
#define SYS_SET_VIEWPORT 135   // a0=x, a1=y, a2=w, a3=h

// Lectura a nivel de byte para handles de ReadFile (a0=handle, a1=puntero
// destino, a2=cuantos bytes) -- para que ReadByte/ReadShort/ReadInt/
// ReadFloat/ReadString$ funcionen tambien con handles de ReadFile, no
// solo de OpenFile/WriteFile.
#define SYS_READ_FILE_READ_BYTES 136

// MouseZ() -- posicion ACUMULADA de la rueda desde que arranco el
// programa (nunca se resetea al leerla), a diferencia de
// SYS_GET_MOUSE_WHEEL (que es un delta que SI se consume).
#define SYS_GET_MOUSE_Z 137

// FlushMouse -- comando SEPARADO de FlushKeys en BlitzPlus real, solo
// limpia las pulsaciones de boton en cola.
#define SYS_FLUSH_MOUSE 138
#define SYS_SET_TITLE 130      // a0=puntero a texto -- cambia el titulo de la ventana actual
#define SYS_FREE_TIMER 131     // a0=handle (el mismo que devolvio CreateTimer)
#define SYS_TIMER_READY 132    // a0=handle -> 1 si ya toca el siguiente disparo (consulta pura, sin efectos)
#define SYS_TIMER_CONSUME 133  // a0=handle -- avanza al siguiente disparo (llamar justo despues de que TIMER_READY de 1)

#define SYS_GET_SCREEN_SIZE 35 // sin argumentos; devuelve (ancho<<32 | alto) de toda la pantalla

// Registra (o actualiza) un boton clicable dentro de la ventana del
// programa, y lo dibuja con un aspecto por defecto. El programa
// todavia tiene que dibujar su propia etiqueta encima con SYS_DRAW_TEXT.
// a0=id (elegido por el programa, >0), a1=x, a2=y,
// a3=(ancho<<16 | alto), a4=color
#define SYS_DEFINE_BUTTON   36

// Sin argumentos. Devuelve el id del boton que se acaba de pulsar en
// esta ventana (0 si ninguno). Hay que llamarla una vez por vuelta del
// bucle principal del programa, igual que SYS_READ_CHAR.
#define SYS_GET_BUTTON_ID   37

// Dialogo comun de "Abrir"/"Guardar como" (ver dialog.h) -- el kernel
// dibuja y gestiona toda la navegacion de carpetas dentro de la
// ventana del programa, y bloquea (cediendo el control a otras
// tareas) hasta que el usuario elige un archivo o cancela.
// a0=inodo de la carpeta inicial, a1=puntero a buffer de salida para
// el nombre elegido, a2=tamaño de ese buffer.
// Devuelve el inodo del archivo elegido, o -1 si cancela.
#define SYS_OPEN_FILE_DIALOG 38
#define SYS_SAVE_FILE_DIALOG 39

// CreateWindow() estilo BlitzPlus -- "personaliza" (titulo, posicion,
// tamaño) la ventana que la tarea ya tiene automaticamente, y la pone
// en "modo evento" (ver wm_set_event_mode): a partir de aqui, la X de
// cerrar deja de destruir la ventana directamente y en su lugar
// dispara EVENT_WINDOWCLOSE, para que el programa lo vea con
// WaitEvent(). a0=puntero a titulo, a1=x, a2=y, a3=ancho, a4=alto.
// Devuelve el indice de ventana (util como "handle" para los CreateX
// de gadgets, que ya esperan recibirlo como 'parent').
#define SYS_CREATE_WINDOW 40

// -- ReadFile/ReadLine$/Eof/CloseFile: lectura de archivos de texto
// linea a linea (busca primero en la raiz, luego en DOCUMENTOS) --
#define SYS_READ_FILE_OPEN  41 // a0=puntero a nombre -> handle, o -1 si no existe
#define SYS_READ_FILE_LINE  42 // a0=handle, a1=buffer salida, a2=tamaño max -> longitud
#define SYS_READ_FILE_EOF   43 // a0=handle -> 1 si no queda nada por leer
#define SYS_READ_FILE_CLOSE 44 // a0=handle

// -- Reservado para cuando existan estas piezas de kernel --
// 50-59  Sonido
// 60-69  Hilos / multitarea real
// 70-79  Red
// 80-89  PCI / hardware de bajo nivel
// 90-99  IPC (comunicacion entre procesos)

// -- Gadgets estilo BlitzPlus (100-129) --
//
// A diferencia de las syscalls de dibujo de mas arriba, estos widgets
// se dibujan y gestionan ellos solos: el programa los crea una vez, y
// el kernel se encarga de pintarlos y de procesar clics/teclado en
// cada vuelta del planificador. El objetivo es poder portar programas
// BlitzPlus reales que usen CreateButton/CreateGadget/etc. con el
// menor cambio posible.

// La ventana se deduce automaticamente de quien hace la llamada
// (igual que en las syscalls de dibujo) -- no se pasa como argumento.
//
// a0=puntero al texto del boton, a1=x, a2=y, a3=(ancho<<16|alto).
// Devuelve un id de gadget, o -1 si no hay hueco.
#define SYS_CREATE_BUTTON     100
// a0=x, a1=y, a2=(ancho<<16|alto)
#define SYS_CREATE_PANEL      101
// a0=x, a1=y, a2=(ancho<<16|alto)
#define SYS_CREATE_TEXTFIELD  102
// a0=x, a1=y, a2=(ancho<<16|alto)
#define SYS_CREATE_LISTBOX    103

#define SYS_GADGET_FREE       104 // a0=id
#define SYS_GADGET_SET_TEXT   105 // a0=id, a1=puntero a texto
#define SYS_GADGET_GET_TEXT   106 // a0=id, a1=buffer salida, a2=tamaño max -> longitud
// Devuelve (x<<48 | y<<32 | ancho<<16 | alto), cada campo de 16 bits
#define SYS_GADGET_RECT       107
#define SYS_GADGET_MOVE       108 // a0=id, a1=x, a2=y
#define SYS_GADGET_RESIZE     109 // a0=id, a1=ancho, a2=alto
#define SYS_GADGET_SHOW       110 // a0=id, a1=visible (0/1)
#define SYS_GADGET_ENABLE     111 // a0=id, a1=activo (0/1)
#define SYS_GADGET_ACTIVATE   112 // a0=id -- da el foco de teclado (TextField)

// Sin argumentos ademas de la ventana propia (se deduce igual que en
// las syscalls de dibujo). Devuelve el id del gadget que disparo un
// evento desde la ultima llamada (boton pulsado, seleccion de lista
// cambiada, Enter en un campo de texto, entrada de menu elegida), o 0
// si ninguno.
#define SYS_GADGET_EVENT      113

// -- ListBox --
#define SYS_LISTBOX_ADD_ITEM    114 // a0=id, a1=puntero a texto
#define SYS_LISTBOX_CLEAR       115 // a0=id
#define SYS_LISTBOX_SELECTED    116 // a0=id -> indice seleccionado, -1 si ninguno
#define SYS_LISTBOX_SELECT      117 // a0=id, a1=indice
#define SYS_LISTBOX_ITEM_COUNT  118 // a0=id -> cantidad de elementos
#define SYS_LISTBOX_ITEM_TEXT   119 // a0=id, a1=indice, a2=buffer, a3=tamaño max -> longitud

// -- TextArea (caja multilinea de solo lectura en v1) --
// a0=x, a1=y, a2=(ancho<<16|alto)
#define SYS_CREATE_TEXTAREA     125
// a0=id, a1=puntero a texto -- reemplaza TODO el contenido, partido
// en lineas por '\n'.
#define SYS_TEXTAREA_SET_TEXT   126

// CreateTimer(hertz) -- temporizador de la ventana propia, dispara
// EVENT_TIMERTICK ($4001) mientras el programa siga cediendo el
// control (WaitEvent, PollEvent, o cualquier otra syscall que
// bombee). a0=hertz (veces por segundo).
#define SYS_CREATE_TIMER        127

// -- Menus --
// Devuelve (creando la primera vez) la raiz de la barra de menu de la
// ventana propia -- se usa como 'parent' en SYS_CREATE_MENU para las
// entradas de primer nivel.
#define SYS_WINDOW_MENU        120
// a0=puntero a texto, a1=tag (numero libre para el programa),
// a2=parent (la raiz, u otra entrada de menu para anidar un submenu)
#define SYS_CREATE_MENU        121
#define SYS_MENU_CHECK         122 // a0=id, a1=marcado (0/1)
#define SYS_MENU_ENABLE        123 // a0=id, a1=activo (0/1)
#define SYS_MENU_GET_TAG       124 // a0=id -> el 'tag' que se le puso al crearlo

// PeekEvent()/FlushEvents([id]) -- confirmados contra la
// documentacion real de BlitzPlus.
#define SYS_PEEK_EVENT 139   // -> id del evento pendiente, sin consumirlo (0 si no hay ninguno)
#define SYS_FLUSH_EVENTS 140 // a0=id (0=cualquiera) -- descarta el evento pendiente si coincide

// ButtonState/SetButtonState -- casilla/radio (CreateButton style 2/3)
#define SYS_BUTTON_STATE 141      // a0=id -> 1/0
#define SYS_SET_BUTTON_STATE 142  // a0=id, a1=estado (0/1)

// HotKeyEvent -- a0=(rawkey<<8|modificador), a1=event_id (0=quitar),
// a2=event_data, a3=event_source. (event_x/y/z de BlitzPlus real se
// aceptan en el compilador pero no se conservan -- no tenemos
// EventX/EventY/EventZ implementados; limitacion documentada)
#define SYS_HOTKEY_EVENT 143

// Getter generico de 'enabled' -- MenuEnabled() y similares
// TextArea: leer/anadir/consultar longitud y estructura -- ver la
// nota de "LIMITACION DOCUMENTADA" junto a TextAreaCursor en el
// compilador: no hay cursor de verdad todavia (TextArea no admite
// edicion interactiva por teclado en v1).
#define SYS_GADGET_ENABLED 144 // a0=id -> 1/0
#define SYS_TEXTAREA_ADD_TEXT 145      // a0=id, a1=texto
#define SYS_TEXTAREA_LEN 146           // a0=id, a1=units (1=caracteres,2=lineas) -> longitud
#define SYS_TEXTAREA_LINE_LEN 147      // a0=id, a1=linea -> longitud de esa linea
#define SYS_TEXTAREA_LINE_OF_CHAR 148  // a0=id, a1=indice de caracter -> indice de linea
#define SYS_TEXTAREA_GET_TEXT 149      // a0=id, a1=start, a2=count (-1=hasta el final), a3=buffer salida, a4=tamaño max

// ActivateWindow/ActiveWindow -- foco de ventana
#define SYS_ACTIVATE_WINDOW 150 // a0=indice de ventana
#define SYS_ACTIVE_WINDOW 151   // -> indice de la ventana con foco

// MaximizeWindow/MinimizeWindow/WindowMaximized/WindowMinimized/SetMinWindowSize
#define SYS_MAXIMIZE_WINDOW 152      // a0=indice de ventana
#define SYS_MINIMIZE_WINDOW 153      // a0=indice de ventana
#define SYS_WINDOW_MAXIMIZED 154     // a0=indice -> 1/0
#define SYS_WINDOW_MINIMIZED 155     // a0=indice -> 1/0
#define SYS_SET_MIN_WINDOW_SIZE 156  // a0=indice, a1=ancho (0=actual), a2=alto (0=actual)

// InsertGadgetItem/RemoveGadgetItem/ModifyGadgetItem -- solo aplican
// a ListBox por ahora (ComboBox/Tabber son Fase 3, sin implementar)
#define SYS_GADGET_INSERT_ITEM 157 // a0=id, a1=indice, a2=texto
#define SYS_GADGET_REMOVE_ITEM 158 // a0=id, a1=indice
#define SYS_GADGET_MODIFY_ITEM 159 // a0=id, a1=indice, a2=texto

// Fase 3: tipos de gadget nuevos
#define SYS_CREATE_LABEL 160 // a0=texto, a1=x, a2=y, a3=(ancho<<16|alto), a4=style
#define SYS_CREATE_PROGBAR 161 // a0=x, a1=y, a2=(ancho<<16|alto)
#define SYS_UPDATE_PROGBAR 162 // a0=id, a1=bits crudos de un double (0.0-1.0)

// CreateSlider/SetSliderRange/SetSliderValue/SliderValue
#define SYS_CREATE_SLIDER 163    // a0=x, a1=y, a2=(ancho<<16|alto), a3=style
// Barra de desplazamiento. Misma forma de llamar que el deslizador, y usa
// SYS_SET_SLIDER_RANGE / _VALUE / SYS_SLIDER_VALUE igual que el: comparten
// el recorrido y el valor. Lo que añade son las flechas y el salto de pagina.
#define SYS_CREATE_SCROLLBAR 314 // a0=x, a1=y, a2=(ancho<<16|alto), a3=style (1 horizontal, 2 vertical)
// Menu flotante. Se crea vacio, se le cuelgan entradas con SYS_CREATE_MENU
// pasandole este id como padre, y se abre donde haga falta. Sus entradas
// mandan el mismo evento que las de la barra de menu.
#define SYS_CREATE_CONTEXT_MENU 315 // -> id de la raiz, o -1
#define SYS_SHOW_CONTEXT_MENU 316   // a0=id de la raiz, a1=x, a2=y (dentro del area de contenido)
#define SYS_SET_SLIDER_RANGE 164 // a0=id, a1=visible, a2=total
#define SYS_SET_SLIDER_VALUE 165 // a0=id, a1=valor
#define SYS_SLIDER_VALUE 166     // a0=id -> valor actual

// CreateComboBox -- comparte las syscalls de item (114-119) con
// ListBox, ya que ambos usan el mismo almacenamiento (is_item_gadget)
#define SYS_CREATE_COMBOBOX 167 // a0=x, a1=y, a2=(ancho<<16|alto)
#define SYS_CREATE_TABBER 168 // a0=x, a1=y, a2=(ancho<<16|alto)

// CreateToolBar/LoadIconStrip/FreeIconStrip/SetGadgetIconStrip/
// EnableToolBarItem/DisableToolBarItem/SetToolBarTips
#define SYS_LOAD_ICON_STRIP 169     // a0=nombre de archivo -> handle (mismo pool que LoadImage)
#define SYS_FREE_ICON_STRIP 170     // a0=handle
#define SYS_SET_GADGET_ICON_STRIP 171 // a0=id de gadget, a1=handle de tira
#define SYS_CREATE_TOOLBAR 172      // a0=handle de imagen YA cargada, a1=x, a2=y, a3=(ancho<<16|alto)
#define SYS_ENABLE_TOOLBAR_ITEM 173 // a0=id, a1=indice, a2=activo (0/1)
#define SYS_SET_TOOLBAR_TIPS 174    // a0=id, a1=texto (separado por comas)

// CreateTreeView y companeros -- cada nodo es su propio "gadget" con
// handle propio, no un indice como ListBox.
#define SYS_CREATE_TREEVIEW 175       // a0=x, a1=y, a2=(ancho<<16|alto)
#define SYS_TREEVIEW_ROOT 176         // a0=treeview -> id del nodo raiz
#define SYS_ADD_TREEVIEW_NODE 177     // a0=texto, a1=padre -> id del nodo nuevo
#define SYS_INSERT_TREEVIEW_NODE 178  // a0=indice, a1=texto, a2=padre -> id del nodo nuevo
#define SYS_MODIFY_TREEVIEW_NODE 179  // a0=nodo, a1=texto
#define SYS_FREE_TREEVIEW_NODE 180    // a0=nodo
#define SYS_EXPAND_TREEVIEW_NODE 181  // a0=nodo, a1=expandir (1) o colapsar (0)
#define SYS_COUNT_TREEVIEW_NODES 182  // a0=padre -> cantidad de hijos directos
#define SYS_SELECTED_TREEVIEW_NODE 183 // a0=treeview -> id del nodo seleccionado (-1 si ninguno)
#define SYS_SELECT_TREEVIEW_NODE 184  // a0=nodo

// ReadPixel/WritePixel/CopyPixel -- buffer=0 (o ausente) = ventana
// actual, buffer=N = imagen N-1 (misma convencion que ya usa
// ImageBuffer()/SetBuffer: handle+1). Las variantes "Fast" (que
// exigen LockBuffer) y LockedPixels/LockedPitch/LockedFormat quedan
// diferidas -- no tenemos sistema de bloqueo de buffer todavia.
#define SYS_READ_PIXEL 185  // a0=x, a1=y, a2=buffer -> color 0xRRGGBB
#define SYS_WRITE_PIXEL 186 // a0=x, a1=y, a2=argb, a3=buffer
#define SYS_COPY_PIXEL 187  // a0=(src_x<<16|src_y), a1=src_buffer, a2=(dest_x<<16|dest_y), a3=dest_buffer

// CreateCanvas -- ver la nota junto a CANVAS_BUFFER_OFFSET en
// syscall.c. FlipCanvas/DesktopBuffer/EndGraphics reutilizan otras
// syscalls existentes (Flip/valor 0 fijo/no-op), sin numero propio.
#define SYS_CREATE_CANVAS 188 // a0=x, a1=y, a2=(ancho<<16|alto)

// LoadFont/SetFont/FreeFont y companeros -- ver la nota grande junto
// a font_load en syscall.c sobre la limitacion de no tener un
// renderizador TrueType de verdad.
#define SYS_LOAD_FONT 189   // a0=nombre, a1=alto, a2=negrita, a3=cursiva, a4=subrayado -> handle (0 si sin huecos)
#define SYS_FREE_FONT 190   // a0=handle
#define SYS_SET_FONT 191    // a0=handle
#define SYS_FONT_NAME 192   // a0=handle, a1=buffer, a2=tamaño max
#define SYS_FONT_SIZE 193   // a0=handle -> alto PEDIDO al cargar (metadato, no el real en pantalla)
#define SYS_FONT_STYLE 194  // a0=handle -> 1=normal/negrita, 3=cursiva (asi lo documenta BlitzPlus real)
#define SYS_FONT_WIDTH 195  // -> ancho REAL en pixeles de nuestra fuente fija (5)
#define SYS_FONT_HEIGHT 196 // -> alto REAL en pixeles de nuestra fuente fija (7)

// SetGamma/UpdateGamma/GammaRed/GammaGreen/GammaBlue -- ver la nota
// grande junto a g_gamma_r en syscall.c (limitacion real: "solo en
// pantalla completa" en BlitzPlus real, que no tenemos).
#define SYS_SET_GAMMA 197    // a0=(r<<16|g<<8|b), a1=(dest_r<<16|dest_g<<8|dest_b)
#define SYS_UPDATE_GAMMA 198 // a0=calibrate (se acepta, no-op)
#define SYS_GAMMA_RED 199    // a0=red -> salida
#define SYS_GAMMA_GREEN 200  // a0=green -> salida
#define SYS_GAMMA_BLUE 201   // a0=blue -> salida

// GfxDriverName$/GfxModeFormat/GraphicsFormat/TotalVidMem -- valores
// nominales/sinteticos, dado que solo tenemos UN framebuffer propio,
// sin varias tarjetas/drivers/modos entre los que elegir de verdad.
#define SYS_GFX_DRIVER_NAME 202 // a0=indice, a1=buffer, a2=tamaño max -> longitud
#define SYS_GFX_MODE_FORMAT 203 // a0=modo (ignorado) -> mismo valor que GraphicsFormat
#define SYS_GRAPHICS_FORMAT 204 // -> formato de pixel (4 = 32 bits RGB, byte alto sin usar)
#define SYS_TOTAL_VID_MEM 205   // -> bytes "disponibles" (valor nominal fijo)

// Avance de cursor por caracter de la fuente ACTIVA -- distinto de
// SYS_FONT_WIDTH (que da solo el ancho del glifo, sin el hueco entre
// caracteres). Usado por StringWidth para que respete la escala real
// de SetFont, igual que ya hace Text.
#define SYS_FONT_CHAR_ADVANCE 206 // -> (FONT_WIDTH+1) * escala activa

#define SYS_SET_PANEL_COLOR 207 // a0=id, a1=(r<<16|g<<8|b)

// LockBuffer/UnlockBuffer/LockedPixels/LockedPitch/LockedFormat -- ver
// la nota grande junto a LOCKED_BUFFER_SENTINEL en syscall.c.
#define SYS_LOCK_BUFFER 208     // a0=buffer (0=ventana, N=imagen N-1)
#define SYS_UNLOCK_BUFFER 209   // a0=buffer (se ignora -- modelo de bloqueo unico global)
#define SYS_LOCKED_PIXELS 210   // a0=buffer -> handle centinela (o 0 si no hay bloqueo)
#define SYS_LOCKED_PITCH 211    // a0=buffer -> bytes por fila
#define SYS_LOCKED_FORMAT 212   // a0=buffer -> formato de pixel (4, igual que GraphicsFormat)
// Variantes "Fast" -- MISMA logica que ReadPixel/WritePixel/CopyPixel,
// pero EXIGEN que haya un buffer bloqueado (fallan/no hacen nada si
// no), tal como documenta BlitzPlus real.
#define SYS_READ_PIXEL_FAST 213  // a0=x, a1=y, a2=buffer -> color 0xRRGGBB (0 si no hay bloqueo)
#define SYS_WRITE_PIXEL_FAST 214 // a0=x, a1=y, a2=argb, a3=buffer
#define SYS_COPY_PIXEL_FAST 215  // a0=(src_x<<16|src_y), a1=src_buffer, a2=(dest_x<<16|dest_y), a3=dest_buffer

#define SYS_PAUSE_TIMER 216  // a0=handle
#define SYS_RESUME_TIMER 217 // a0=handle
#define SYS_RESET_TIMER 218  // a0=handle
#define SYS_TIMER_TICKS 219  // a0=handle -> contador de ticks

// ReadBytes/WriteBytes(banco,archivo,offset,cantidad) -- version de
// PeekByte/PokeByte para BLOQUES enteros, leyendo/escribiendo
// directamente entre un banco y un archivo ya abierto (con
// SYS_GENFILE_OPEN). El offset es DENTRO DEL BANCO (donde empezar a
// escribir/leer los bytes), no del archivo (que avanza solo, por su
// propia posicion interna).
#define SYS_READ_BYTES_BANK 220  // a0=banco, a1=handle de archivo, a2=offset en el banco, a3=cantidad -> bytes leidos de verdad
#define SYS_WRITE_BYTES_BANK 221 // a0=banco, a1=handle de archivo, a2=offset en el banco, a3=cantidad -> 0 si ok, -1 si error

#define SYS_SET_PANEL_IMAGE 222 // a0=id de panel, a1=puntero a nombre de archivo (se carga aqui mismo, igual que CreateToolBar)

#define SYS_SET_GADGET_GROUP 223 // a0=id de gadget, a1=handle de grupo
#define SYS_GADGET_GROUP 224     // a0=id de gadget -> handle de grupo (0 si no se asigno)

#define SYS_TFORM_IMAGE 225 // a0=handle de imagen, a1..a4=bits crudos de a#,b#,c#,d# (matriz 2x2)

// -- Fase 4: Sonido --
// LoadSound/FreeSound/PlaySound: reales, ver sound_load/sound_play en
// syscall.c. SoundVolume/SoundPan/SoundPitch: reales tambien.
// PauseChannel/ResumeChannel/StopChannel/ChannelPlaying/ChannelVolume/
// ChannelPan/ChannelPitch/LoopSound/PlayMusic/PlayCDTrack:
// LIMITACION DOCUMENTADA -- V1 reproduce de forma SINCRONA (bloqueante
// hasta que termina de sonar), asi que no existe un "canal en curso"
// real sobre el que actuar estos comandos despues de que PlaySound ya
// ha vuelto. Se aceptan (evaluando argumentos por si tienen efectos
// secundarios) pero no hacen nada real -- ver la nota grande en
// sound.c sobre que haria falta para polifonia de verdad.
#define SYS_LOAD_SOUND 226   // a0=puntero a nombre de archivo -> handle (-1 si fallo)
#define SYS_FREE_SOUND 227   // a0=handle
#define SYS_PLAY_SOUND 228   // a0=handle (bloquea hasta que termina de sonar)
#define SYS_SOUND_VOLUME 229 // a0=handle, a1=bits crudos de un double (0.0-1.0)
#define SYS_SOUND_PAN 230    // a0=handle, a1=bits crudos de un double (-1.0 a 1.0)
#define SYS_SOUND_PITCH 231  // a0=handle, a1=hercios (entero)

// -- ExecFile/CreateProcess/CallDLL -- ver la nota grande junto a
// exec_program() en syscall.c.
#define SYS_EXEC_FILE 232      // a0=puntero a nombre de archivo -> 1 si arranco, 0 si no
#define SYS_CREATE_PROCESS 233 // a0=puntero a "programa argumentos" -> id de tarea (o -1), usado como "stream"
#define SYS_FILE_RENAME 234 // a0=nombre actual, a1=nombre nuevo, a2=carpeta padre, a3=volumen -> 0 exito, -1 fallo (solo NemoFS por ahora)
// -- Iconos de escritorio: para el programa "editor de escritorio" --
#define SYS_ICON_CATALOG_COUNT   235 // -> cuantos iconos hay en el catalogo (0..N-1 validos con SYS_DRAW_ICON)
#define SYS_DESKTOP_ICON_COUNT   236 // -> cuantos iconos hay en el escritorio ahora mismo
#define SYS_DESKTOP_ICON_GET     237 // a0=indice, a1=buffer de 16 (etiqueta), a2=buffer de 32 (target .pro), a3=puntero a 3 int32 (icon_id, x, y) -> 1 exito, 0 no existe
#define SYS_DESKTOP_ICON_ADD     238 // a0=etiqueta, a1=target .pro, a2=icon_id, a3=x, a4=y -> indice nuevo, o -1
#define SYS_DESKTOP_ICON_REMOVE  239 // a0=indice -> 0 exito, -1 fallo
#define SYS_DESKTOP_ICON_MOVE    240 // a0=indice, a1=x, a2=y -> 0 exito, -1 fallo
// Dibuja un .nimg ESCALADO dentro de otra imagen, leyendo el archivo
// fila a fila y sin cargarlo entero: asi funciona con imagenes mas
// grandes que el maximo del sistema (un fondo de 1920x1080) y no reserva
// megas para pintar unos pocos pixeles.
//   a0 = puntero al nombre o ruta
//   a1 = imagen destino
//   a2 = (x<<16 | y) dentro de esa imagen
//   a3 = (ancho<<16 | alto) del hueco
//   a4 = modo: 0 ENCAJAR (proporcion respetada, centrado, resto en
//        blanco) | 1 MINIATURA (lo muy alargado se recorta al primer
//        cuadrado antes de encajar) | 2 ESTIRAR
// -> 0 exito, -1 fallo
#define SYS_MENU_RELOAD 278 // vuelve a leer MENU.CFG -> 0. Para el editor del menu.

// ---- El reparto de la tarjeta, para el particionador ----
#define SYS_DISK_FISICO 279     // -> sectores fisicos de la tarjeta (512 bytes cada uno)
#define SYS_DISK_PARTICION 280  // a0=indice 0..3 -> (tipo<<56 | inicio<<28 | sectores), 0 si vacia
#define SYS_DISK_NEMOFS 281     // -> (de_tabla<<63 | inicio<<28 | sectores): donde vive NemoFS
#define SYS_NEMOFS_USO 282      // a0=puntero a dos uint32 (bloques totales y usados) -> 1 si va bien
#define SYS_DISK_ESTIRAR 283    // estira NemoFS al final de la tarjeta -> 0 hecho, o codigo de error

// Las DOS miniaturas de un mismo.nimg de una sola
// pasada, en vez de leer el archivo dos veces. Los dos huecos son
// cuadrados, que es como los usa el Navegante, y asi todo cabe en
// cinco argumentos sin pasar punteros a estructuras.
//   a0 = puntero al nombre o ruta del .nimg
//   a1 = (imagen_grande << 16) | imagen_pequena     (dos handles)
//   a2 = (dx_grande << 16) | dy_grande
//   a3 = (dx_pequena << 16) | dy_pequena
//   a4 = (lado_grande << 16) | lado_pequena
// -> 0 exito, -1 fallo. El encuadre es siempre el de MINIATURA.
#define SYS_MINIATURA_DOBLE 284
#define SYS_DRAW_FILE_SCALED 277
#define SYS_SET_WALLPAPER 276 // a0=puntero al nombre o ruta de un .nimg (0 = quitar el fondo),
                              // a1=modo (0 estirar, 1 mosaico, 2 centrar) -> 0 exito, -1 fallo
#define SYS_DESKTOP_ICON_SET_LABEL 275 // a0=indice, a1=puntero a la etiqueta nueva -> 0 exito, -1 fallo
#define SYS_DESKTOP_ICON_SET_GRAPHIC 241 // a0=indice, a1=icon_id -> 0 exito, -1 fallo
#define SYS_DESKTOP_SAVE         242 // sin argumentos -- persiste el escritorio actual en disco
#define SYS_DESKTOP_ICON_SCALE_GET 243 // -> 1 (24x24) o 2 (48x48), la escala actual de todo el sistema
#define SYS_DESKTOP_ICON_SCALE_SET 244 // a0=nueva escala (1 o 2) -- la aplica, la persiste, y la comparten escritorio y explorador
#define SYS_SCREEN_RES_GET_PENDING 245 // -> (ancho<<32 | alto) guardado en SCREEN.CFG (puede no ser el activo ahora mismo, si aun no se ha reiniciado)
#define SYS_SCREEN_RES_SET_PENDING 246 // a0=ancho, a1=alto -- guarda la preferencia; hace falta REINICIAR para que se aplique de verdad

// a0=inodo -> NEMOFS_TYPE_FILE (1), NEMOFS_TYPE_DIR (2), o -1 si el
// inodo no existe/esta libre. Para saber si un hijo devuelto por
// SYS_FIND_CHILD (83) es un archivo o una carpeta, sin pasar por el
// formato binario mixto de SYS_FILE_LIST.
#define SYS_NEMOFS_TYPE_BY_INODE 247

// -- Gestor de tareas y monitor de sistema --
//
// Sin argumentos -> cuenta de tareas vivas escritas en el buffer.
// a0=buffer de salida, a1=maximo de entradas. Cada entrada, 48 bytes:
// slot(4) + window_idx(4) + turnos de planificador(4) + reservado(4)
// + nombre del programa(32, con \0).
#define SYS_TASK_LIST 248
// UN dato de UNA tarea, por indice del mismo listado que da SYS_TASK_LIST.
// Existen porque no todos los lenguajes pueden leer ese volcado binario:
// Nemo Basic no tiene punteros ni estructuras, asi que el kernel lo parte.
// a0=indice (0..vivas-1), a1=campo: 0=slot, 1=ventana, 2=turnos del
// planificador -> el valor, o -1 si el indice o el campo no existen. El
// campo 1 puede ser -1 de verdad: una tarea sin ventana.
#define SYS_TASK_FIELD 294
// a0=indice, a1=buffer, a2=tamaño maximo -> letras escritas (sin el \0), o
// -1. El nombre del programa de esa tarea.
#define SYS_TASK_NAME 295

// ---------------------------------------------------------------------
// RED: bajar algo por HTTP y abrir un paquete.nmz
//
// Hasta ahora el cliente HTTP del kernel solo lo alcanzaba el comando
// 'bajar' de la shell de red. Esto lo abre a los programas, que es lo
// que hace falta para que el navegador --escrito en Lua-- pueda pedir
// una pagina.
//
// UNA DESCARGA A LA VEZ en todo el sistema: debajo hay un solo cliente
// HTTP y un solo cliente TCP. Pedir una segunda mientras la primera
// sigue devuelve -1 en vez de fingir que caben dos.
// ---------------------------------------------------------------------

// a0=ruta ("/nmz?u=..."), a1=IP empaquetada (a.b.c.d -> a|b<<8|c<<16|d<<24),
// a2=puerto, a3=archivo donde guardarla, admitiendo "CARPETA/archivo"
// (a3=0 descarga sin guardar nada, que solo sirve para comprobar que el
// servidor contesta).
// -> 0 si empezo, -1 si no.
#define SYS_NET_BAJAR 296

// -> estado | (codigo HTTP << 8).
// estado: 0 parada, 1 en marcha, 2 lista, 3 fallo.
#define SYS_NET_ESTADO 297

// -> (total declarado << 32) | bytes recibidos. El total viene a 0 si el
// servidor no dijo cuanto ocupaba.
#define SYS_NET_BYTES 298

// a0=buffer, a1=tamano max -> largo escrito. Por que fallo, en texto.
#define SYS_NET_MOTIVO 299

// Desempaqueta un .nmz ya descargado (ver herramientas/FORMATO_NMZ.md).
// a0=archivo del paquete, a1=carpeta donde dejarlo (se crea si no existe),
// a2=buffer donde dejar el nombre del primer archivo (0 si no interesa),
// a3=tamano de ese buffer.
// -> cuantos archivos escribio, o -1.
#define SYS_NMZ_ABRIR 300

// ---------------------------------------------------------------------
// RED: sockets UDP
//
// Lo pidio un juego multijugador, y para eso UDP es lo correcto y no el
// pariente pobre de TCP -- ver la explicacion larga en udp_sock.h. Aqui
// SI pueden convivir varios a la vez (cuatro), al contrario que la
// descarga HTTP de arriba: no hay nada que mantener por conexion.
//
// Ninguna de estas cinco espera. El planificador es cooperativo, asi que
// un syscall que se quedara esperando con el candado grande cogido
// pararia la MAQUINA ENTERA, no solo el programa: el raton, el reloj y
// las demas ventanas. Quien quiera esperar, lo hace con un bucle y Delay
// desde Nemo Basic o Lua, donde esperar no le cuesta nada a los demas.
// ---------------------------------------------------------------------

// a0=puerto (0 = elige uno libre de los altos, para quien solo manda y
// escucha la contestacion).
// -> identificador 0..3, o negativo: -1 no quedan sockets, -2 ese puerto
// ya esta abierto, -3 es un puerto del sistema (el 68, del DHCP).
#define SYS_UDP_ABRIR 301

// a0=identificador. Sin resultado.
#define SYS_UDP_CERRAR 302

// a0=identificador, a1=IP EN TEXTO ("192.168.1.40"), a2=puerto destino,
// a3=buffer, a4=cuantos bytes.
// -> los bytes mandados, o negativo: -1 el socket no vale, -2 el mensaje
// es mas grande que el tope, -3 no salio todavia (falta resolver la MAC
// por ARP: hay que volver a llamar), -4 eso no es una IP.
//
// La IP va en TEXTO y no empaquetada como en la 296, a proposito. Quien
// llama desde Nemo Basic no puede partir "192.168.1.40" con comodidad, y
// el kernel si; y sobre todo, una IP en texto SE PUEDE RECHAZAR cuando
// esta mal ("999.1.1.1", "1.2.3") mientras que un entero empaquetado no
// tiene forma de parecer equivocado. Pasar la IP como entero obliga
// ademas a acertar el orden de los bytes en los dos lados (ver
// ip_texto.h).
//
// La IP puede ser de DIFUSION (255.255.255.255, o la de la propia red con
// los bits de host a uno) para hablarle a toda la red sin conocer a nadie.
#define SYS_UDP_ENVIAR 303

// a0=identificador, a1=buffer, a2=tamano max.
// -> bytes copiados, o -1 si no hay nada. Un datagrama VACIO devuelve 0,
// que por eso NO significa "no hay nada". Si no cabe, se copia lo que
// quepa y el resto se pierde: el datagrama se consume igual.
#define SYS_UDP_RECIBIR 304

// a0=identificador, a1=buffer donde escribir la IP en texto (0 si solo se
// quiere el puerto), a2=tamano de ese buffer (16 como minimo).
// -> letras escritas | (puerto << 32).
//
// De quien era EL DATAGRAMA QUE ACABA DE DEVOLVER la 304, no el ultimo
// que llego. Con varios jugadores mandando a la vez, "el ultimo que
// llego" te hace contestarle al que no era, y eso no da ningun error.
#define SYS_UDP_ORIGEN 305

// a0=identificador -> cuantos datagramas se han tirado por cola llena.
// Se expone a proposito: si el juego va a tirones, esta es la cifra que
// dice si es que el programa no vacia lo bastante rapido o es otra cosa.
#define SYS_UDP_PERDIDOS 306

// a0=identificador -> cuantos datagramas esperan. 0 = nada que leer.
//
// Es la que permite escribir el bucle "saca todo lo que haya", y no es
// un lujo: sin ella no se puede. La 304 devuelve una CADENA a quien la
// llama desde Nemo Basic, y un datagrama vacio --que es legal-- da la
// misma cadena vacia que "no hay nada"; y la 305 se queda con el ultimo
// remitente entregado, asi que despues de leer uno ya nunca vuelve a
// cero. El primer bucle escrito sin esto no terminaba nunca.
#define SYS_UDP_PENDIENTES 311
// El puerto local de un socket. Con UdpOpen(0) el kernel elige uno libre, y
// sin esto el programa no sabia cual: no podia decirle al otro extremo a
// donde contestar, asi que abrir con 0 solo servia para hablar primero.
#define SYS_UDP_PUERTO 317        // a0=socket -> puerto, o 0 si el socket no vale

// ---------------------------------------------------------------------
// RED: pedir algo por HTTP y recibirlo EN MEMORIA
//
// La 296 baja a un ARCHIVO, que es lo que quiere un navegador. Esto es
// para lo contrario: MANDAR un dato --la lectura de un sensor del GPIO--
// y recibir una contestacion corta que no merece crear un archivo para
// tirarlo acto seguido.
//
// El estado, el codigo y el motivo se consultan con las de arriba: la
// 297 y la 299. Debajo hay UN solo cliente HTTP, asi que esto y la 296
// son la misma descarga y no pueden ir a la vez.
// ---------------------------------------------------------------------

// a0=ruta ("/sensor"), a1=IP EN TEXTO ("192.168.1.40"), a2=puerto,
// a3=cuerpo a mandar (0 = un GET, sin cuerpo), a4=cuantos bytes.
// -> 0 si empezo, o negativo: -1 ya hay una descarga en marcha o la
// peticion no cabe, -4 eso no es una IP.
//
// La IP en texto por lo mismo que en la 303, y no como en la 296: se
// puede rechazar cuando esta mal.
#define SYS_NET_PEDIR 307

// a0=buffer, a1=tamano max -> bytes copiados. La respuesta recibida.
// Caben DESCARGA_VISTA (2048) bytes; lo que pase de ahi se descarta,
// pero la 298 sigue contando el total que llego, para que se vea que
// vino mas de lo que se guardo.
#define SYS_NET_CUERPO 308

// ---------------------------------------------------------------------
// RED: la configuracion propia
//
// No habia forma de preguntarle al sistema su propia direccion. Hacia
// falta en cuanto hay programas de red: el primero que la necesito fue
// el esqueleto de multijugador, para reconocer su propio mensaje entre
// los que le vuelven por difusion (un mensaje a toda la red te lo
// entregan a ti tambien). Y hace falta para cualquier cosa que se
// quiera ensenar en pantalla o escribir en un archivo.
// ---------------------------------------------------------------------

// a0=buffer, a1=tamano max (16 minimo), a2=cual: 0 la IP, 1 la mascara,
// 2 la pasarela, 3 el DNS. -> letras escritas.
//
// La mascara, la pasarela y el DNS vienen a "0.0.0.0" cuando no hay:
// con la IP de reserva (cable directo, sin router) no existen y no hacen
// falta. Es un dato, no un error.
#define SYS_NET_LOCAL 309

// Sin argumentos -> 1 si la red esta lista (hay enlace Y hay direccion),
// 0 si no.
//
// Merece un numero propio porque es la pregunta que hay que hacer ANTES
// de cualquier otra: pedir un socket o mandar algo mientras el DHCP
// sigue negociando falla, y falla de la forma mas confusa posible -- sin
// error del programa, simplemente nada llega a ninguna parte.
#define SYS_NET_LISTA 310
// a0=slot (el mismo indice que da SYS_TASK_LIST) -> 1 si se mato,
// 0 si el slot no existe o ya no estaba vivo. Cierra tambien su
// ventana y libera sus gadgets, igual que la X.
#define SYS_TASK_KILL 249
// Sin argumentos -> (bloques_totales<<32 | bloques_usados) de
// NemoFS. Cada bloque son 512 bytes (SECTOR_SIZE).
#define SYS_DISK_USAGE 250
// Sin argumentos -> (tareas_en_uso<<32 | tareas_total). NO es RAM del
// sistema -- Nemo OS no detecta memoria fisica; esto es cuantos de
// los bloques de 16MB reservados para tareas estan ocupados.
#define SYS_RAM_TASKS 251  // -> (tareas vivas << 32) | huecos totales

// Memoria de la reserva de tareas: (bytes en uso << 32) | bytes totales.
// Distinta de SYS_RAM_TASKS, que cuenta TAREAS: desde que cada tarea
// recibe una region de su propio tamaño, el numero de tareas ya no dice
// nada de la memoria (ocho tareas pequeñas pueden ocupar 16MB, y dos
// grandes 32MB).
#define SYS_RAM_POOL 260
// Fase 4 de la memoria: otra zona de memoria para el programa, mientras
// corre. a0 = bytes (se redondea a 2 MB) -> direccion (puesta a cero), o
// 0 si no hay sitio. Se libera sola al terminar el programa.
#define SYS_MEM_PEDIR 261
// Fase 5: leer por partes. a0 = identificador (SYS_FILE_OPEN), a1 = buffer,
// a2 = bytes (maximo 256 KB por llamada), a3 = posicion, a4 = volumen
// (0 NemoFS, 1 FAT) -> bytes leidos (0 en el final del archivo), o -1.
#define SYS_FILE_READ_AT 262
// Fase 5b: escribir por partes. a0 = identificador (SYS_FILE_OPEN), a1 = buffer,
// a2 = bytes (maximo 64 KB por llamada), a3 = volumen (solo 1, FAT) ->
// bytes AÑADIDOS al final del archivo, o -1. Lo crea vacio si no existia.
#define SYS_FILE_APPEND 263
// GPIO (pines 2 a 27 del conector; 14 y 15 reservados: la terminal). Ver
// src/gpio.h. Resultados negativos: -1 pin o modo invalido, -2 reservado,
// -3 de otro programa (o sin configurar, al escribir), -4 no es salida.
#define SYS_GPIO_MODO     264   // a0=pin, a1=modo (0 entrada, 1 salida, 2 entrada a positivo, 3 a masa) -> 0
#define SYS_GPIO_ESCRIBIR 265   // a0=pin, a1=valor (0/1) -> 0
#define SYS_GPIO_LEER     266   // a0=pin -> 0/1
// PWM por hardware: solo pines 12, 13, 18, 19 (12 y 18 comparten canal; 13 y 19
// tambien). -5 si el otro pin del canal esta en uso.
#define SYS_GPIO_PWM      267   // a0=pin, a1=hz (10..100000), a2=ciclo en diezmilesimas (0..10000) -> 0
// I2C 1 (GPIO 2 = SDA, 3 = SCL), 100 kHz. -> bytes transferidos, o error: -1 argumentos,
// -2/-3 los pines 2/3 no estan libres, -6 nadie responde, -7 el bus no termino. Ver src/i2c.h.
#define SYS_I2C_ESCRIBIR  268   // a0=direccion (7 bits), a1=buffer, a2=bytes (1..256)
#define SYS_I2C_LEER      269   // a0=direccion (7 bits), a1=buffer, a2=bytes (1..256)
// SPI0 (GPIO 11 reloj, 10 MOSI, 9 MISO, 8 CE0, 7 CE1). Full duplex. -> bytes, o error:
// -1 argumentos, -2/-3 pines no libres, -7 no termino. Ver src/spi.h.
// Botones de la barra de titulo de la ventana del programa:
// a0 = maximizar, a1 = minimizar, a2 = cerrar; 1 = se ve, 0 = oculto.
// Quien quite cerrar debe ofrecer su propia salida (Esc, por ejemplo); si
// se cuelga, se cierra desde el gestor de tareas.
#define SYS_WINDOW_BOTONES 271
#define SYS_SPI           270   // a0=aparato (0/1), a1=bytes a enviar, a2=cuantos (1..4096),
                                // a3=hz (100 kHz..50 MHz), a4=donde recibir (0: descartar), a5=modo (0..3)
// Sin argumentos -> (heap_usado<<32 | heap_total) del heap del
// KERNEL (16MB fijos, ver heap.c) -- distinto de la memoria de las
// tareas, y tambien distinto de la memoria propia del interprete Lua.
#define SYS_RAM_HEAP 252

// ---- Datos del equipo ----
// Hasta ahora el sistema sabia cuanta RAM tiene (memoria.c la detecta al
// arrancar, para mapearla toda) y que procesador es (MIDR_EL1), pero no
// habia forma de preguntarselo desde un programa: el monitor solo podia
// enseñar sus propios repartos. Estas cuatro lo exponen.
#define SYS_CPU_NOMBRE 285      // a0=buffer, a1=tamaño max -> escribe el nombre del
                                // procesador ("ARM Cortex-A72 (Raspberry Pi 4)") y
                                // devuelve cuantos caracteres escribio
#define SYS_CPU_DATOS 286       // -> (nucleos en marcha << 32) | MHz del procesador.
                                // MHz = 0 si no se puede saber (en QEMU no hay a quien
                                // preguntarselo: no es que vaya a 0)
#define SYS_RAM_FISICA 287      // -> RAM FISICA de la placa, en bytes. La de verdad, la
                                // que detecta memoria.c -- no confundir con la reserva
                                // de tareas (SYS_RAM_POOL) ni con el heap (SYS_RAM_HEAP)
#define SYS_RAM_NUCLEO 288      // -> bytes que ocupa el nucleo cargado en memoria
                                // (codigo + datos + bss), medido entre _start y __bss_end

// -- Filas de pixeles --------------------------------
//
// Una llamada al sistema cuesta 3,5 us medidos en la Pi 4; un pixel, 4 ns.
// Trabajar pixel a pixel desde un programa es pagar casi 900 veces mas
// peaje que trabajo: el relleno del Pintor eran ~4,2 millones de llamadas
// en 1024x1024, unos 15 segundos. Estas cuatro trabajan por filas.
//
// El 'buffer' sigue la convencion de ImageBuffer: N = imagen N-1. La
// VENTANA (buffer 0) NO esta soportada y devuelve -1; ahi ya hay caminos
// rapidos (DrawBlock, el blit de wm.c). Ver la nota junto a fila_imagen()
// en syscall.c.
//
// Todas devuelven cuantos pixeles se han tratado de verdad, ya recortados
// a la imagen, o -1 si los argumentos no valen. 0 es "no quedaba nada
// dentro"; -1 es "te has equivocado". No es lo mismo.
//
// El formato de los pixeles del buffer del programa es un entero
// little-endian por pixel, 0x00RRGGBB -- el mismo valor que devuelve
// ReadPixel, y el que leen nemo.leer_i32 en Lua y un array de enteros en
// Nemo Basic.
//
// a5 dice CUANTOS BYTES ocupa cada pixel en el buffer del programa: 4 (o 0,
// que se admite como 4) u 8. Hace falta porque nemo.buffer() en Lua se lee
// de 4 en 4, pero un array de Nemo Basic tiene elementos de 8 BYTES. Sin
// ese dato, un ReadRow sobre un array de Basic dejaria los pixeles
// apelotonados de 4 en 4, y el programa leeria dos metidos en un elemento
// y basura en el siguiente, sin ningun aviso. Otro valor devuelve -1.

// a0=buffer, a1=x, a2=y, a3=cuantos pixeles, a4=puntero del programa,
// a5=bytes por pixel en ese buffer (0/4 u 8) -> pixeles leidos
#define SYS_FILA_LEER 289
// igual, pero leyendo del buffer del programa y escribiendo en la imagen
#define SYS_FILA_ESCRIBIR 290
// a0=buffer, a1=x, a2=y, a3=cuantos, a4=color 0xRRGGBB -> escritos
#define SYS_FILA_RELLENAR 291
// a0=buffer, a1=x, a2=y, a3=maximo de pixeles (NEGATIVO = hacia la
// izquierda), a4=color, a5=modo (0 = mientras SEA ese color, 1 = mientras
// NO lo sea) -> longitud de la tira, contando el pixel de partida si
// cumple. Es la pieza que hace barato el relleno por inundacion.
#define SYS_FILA_TIRA 292

// -- Imagenes de la TARJETA --------------------------
//
// El camino de lectura de .nimg atiende los DOS volumenes. Cuando iba solo
// por NemoFS, en la tarjeta no aparecian miniaturas: ni en el explorador ni
// en el Navegante. Ver la nota de nimg_fuente_abrir() en syscall.c.
//
// SYS_DRAW_FILE_SCALED (277) y SYS_MINIATURA_DOBLE (284) admiten un
// ORIGEN en a5: los 32 bits bajos, la carpeta (inodo en NemoFS, cluster
// desplazado en FAT, 0 = la busqueda de siempre); los 32 altos, el
// volumen (VOLUME_NEMOFS o VOLUME_FAT). Un a5 de 0 es EXACTAMENTE el
// comportamiento anterior, asi que quien no lo pase sigue igual.
//
// Esta va aparte y no como argumento de SYS_LOAD_IMAGE (49) porque la 49
// se llama desde Nemo Basic, donde el generador solo pone x0: x1 llegaria
// con lo que hubiera en el registro. Mismo motivo por el que existe la
// 253 en vez de haberle añadido un argumento a la 49.
//
// a0=puntero a nombre, a1=carpeta, a2=volumen -> handle, o -1
#define SYS_LOAD_IMAGE_VOL 293
// a0=puntero a nombre, a1=inodo de la carpeta donde buscar -> handle
// (o -1). Como SYS_LOAD_IMAGE (49), pero para una carpeta EXPLICITA
// en vez de la busqueda fija de siempre (raiz, luego DOCUMENTOS) --
// para imagenes en subcarpetas como DOCUMENTOS/IMAGENES.
#define SYS_LOAD_IMAGE_EN 253

// Fuentes proporcionales (fonts.c, paquete FUENTES.NFP embebido). Con
// SetFont sobre una fuente cargada con LoadFont("sans"|"serif"|"mono",
// alto, negrita, cursiva), DrawText dibuja con ella (antialiasing, UTF-8,
// Latin-1). LoadFont("sistema") es la 5x7 de siempre.
#define SYS_TEXT_WIDTH 254     // a0=puntero a cadena UTF-8 -> ancho en pixeles con la fuente activa
#define SYS_FONT_ADVANCES 255  // a0=handle de fuente, a1=buffer de 256 bytes -> rellena el avance de cada
                               // codepoint 0..255 (0 = sin glifo); devuelve (ascent<<16)|alto_linea. Para que
                               // un programa (el motor HTML en Lua) mida texto sin una syscall por palabra.

// Llamada desde exceptions.c al capturar un SVC
uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

// Accesor publico para que gadgets.c pueda leer una imagen ya
// cargada (usado por CreateToolBar) -- ver la nota junto a su
// definicion en syscall.c.
bool image_get_info(int32_t handle, uint32_t *width, uint32_t *height, const uint8_t **pixels);

#endif
