// syscall.c — Nemo OS
//
// Despachador de llamadas al sistema. Como todavia no hay aislamiento
// de procesos (todo corre en EL1, el mismo espacio de direcciones que
// el kernel), los punteros que nos pasan los programas se pueden usar
// directamente sin ninguna validacion.
//
// Desde que existe el planificador de tareas (tasks.c), "cual es la
// ventana del programa que llama" ya no es una variable global que
// alguien tiene que acordarse de actualizar -- se deduce directamente
// de que tarea esta corriendo en este momento (task_get_current_window),
// que siempre es correcto sin importar cuantos programas esten vivos
// a la vez.

#include "syscall.h"
#include "uart.h"
#include "nemofs.h"
#include "descarga.h"
#include "nmz.h"
#include "udp_sock.h"
#include "ip_texto.h"
#include "net.h"
#include "disk.h"   // el reparto de la tarjeta, para el particionador
#include "fat.h"
#include "sound.h"
#include "bkl.h"    // soltar el candado grande mientras suena (ver sound_play)
#include "heap.h"
#include "fonts.h"

// ---- Dueño de cada recurso del kernel ----
//
// Fuentes, imagenes, sonidos, bancos, archivos abiertos y carpetas en
// recorrido viven en tablas FIJAS del kernel, compartidas por todos los
// programas. Antes, un programa que terminaba sin liberar lo suyo -- al
// cerrar su ventana, al fallar su script, o simplemente porque no lo
// hacia -- dejaba esos huecos ocupados PARA SIEMPRE. Tras abrir y cerrar
// unos cuantos programas, la tabla de fuentes (16 huecos) se llenaba y
// ningun programa podia cargar una mas: el visor de Lua caia a la letra
// 5x7 del sistema y las demas ventanas perdian sus fuentes.
//
// Ahora cada hueco apunta a su dueño como "hueco de tarea + generacion"
// (la generacion cambia cada vez que el hueco de tarea se reutiliza, ver
// tasks.c), y un hueco cuyo dueño ya no existe se trata como libre al
// buscar sitio. Lo que reserva el propio kernel (fuera de una tarea) no
// tiene dueño y nunca se recupera asi.
#include "spi.h"
#include "i2c.h"
#include "gpio.h"
#include "tasks.h"
typedef struct { int32_t tarea; uint32_t gen; } duena_t;
static duena_t duena_actual(void) {
    int32_t t = task_get_current_slot();
    duena_t d = { -1, 0 };
    if (t >= 0 && t < MAX_TASKS) { d.tarea = t; d.gen = task_generacion(t); }
    return d;
}
// ¿es este hueco de la tarea que esta llamando? Para que un programa no
// pueda cerrar el archivo que abrio otro.
static bool duena_es_actual(const duena_t *d) {
    duena_t a = duena_actual();
    return d->tarea == a.tarea && d->gen == a.gen;
}
static bool hueco_disponible(bool usado, const duena_t *d) {
    if (!usado) return true;
    return d->tarea >= 0 && !task_viva(d->tarea, d->gen);   // su programa ya no existe
}
#include "wm.h"

// Tabla de "handles" para ARCHIVOS FAT -- el driver FAT no tiene
// inodos persistentes como NemoFS (localiza archivos por nombre cada
// vez), asi que aqui les damos un numero pequeño y estable para que
// los programas los puedan usar igual que un inodo de NemoFS.
// Los huecos de FAT solo guardan una entrada de directorio (un par de
// cientos de bytes): subirlos de 8 a 32 cuesta unos pocos KB, y eran ocho
// para dieciseis tareas, igual que los de ReadFile.
#define MAX_FAT_HANDLES 32
typedef struct {
    bool used;
    bool has_entry;              // true si ya existe en disco
    fat_dirent_t entry;          // valido solo si has_entry
    char pending_name[FAT_NAME_LEN]; // usado si !has_entry, para crearlo al escribir
    fat_cursor_t cursor;         // para SYS_FILE_READ_AT: donde se quedo la ultima lectura
    fat_cursor_t fin;            // para SYS_FILE_APPEND: el ultimo cluster del archivo
} fat_handle_t;
static fat_handle_t fat_handles[MAX_FAT_HANDLES];
static duena_t duena_fat[MAX_FAT_HANDLES];   // dueño de cada hueco, ver hueco_disponible

// BUG REAL CORREGIDO: cuando SYS_FILE_OPEN abre una CARPETA en FAT (no
// un archivo), 'fat_handle_open' devolvia un INDICE DE HANDLE (0-7,
// el mismo espacio de numeros que los archivos) -- pero el explorador
// usa ESE MISMO VALOR DEVUELTO como 'parent' en la SIGUIENTE llamada
// a SYS_FILE_LIST/SYS_FILE_OPEN para navegar DENTRO de esa carpeta.
// Un indice de handle pequeño (0-7) NO ES un numero de cluster real
// -- al reinterpretarlo como tal, el driver leia clusters
// COMPLETAMENTE EQUIVOCADOS del disco (contenido vacio o basura, y en
// el peor caso una cadena de punteros corrupta que colgaba el
// sistema -- el sintoma real reportado: "como bloqueado"). La
// solucion: cuando la entrada es una carpeta, devolvemos su CLUSTER
// real, con un DESPLAZAMIENTO grande para que nunca colisione con los
// indices de handle (0-7) ni con -1 (error) -- mismo patron ya usado
// en el codigo para ReadFile/OpenFile (+100).
#define FAT_DIR_CLUSTER_OFFSET 1000000

// Raiz cuadrada entera (metodo de Newton) -- no tenemos libm enlazada,
// y la necesitamos para rasterizar el ovalo fila a fila.
static uint32_t isqrt_u32(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

static int32_t fat_handle_open(uint32_t parent_cluster, const char *name) {
    // 'parent_cluster' puede venir con el desplazamiento (si el
    // programa esta navegando DENTRO de una carpeta abierta antes) o
    // ser 0 (raiz) -- convertimos al cluster real ANTES de buscar.
    uint32_t real_parent = (parent_cluster >= FAT_DIR_CLUSTER_OFFSET) ? (parent_cluster - FAT_DIR_CLUSTER_OFFSET) : 0;

    fat_dirent_t entry;
    bool exists = fat_find_in_dir(real_parent, name, &entry);

    if (exists && entry.is_dir) {
        // Las CARPETAS no necesitan un handle de lectura/escritura --
        // devolvemos directamente su cluster (desplazado) para que el
        // explorador pueda usarlo como 'parent' al navegar dentro.
        return (int32_t)(entry.first_cluster + FAT_DIR_CLUSTER_OFFSET);
    }

    for (int i = 0; i < MAX_FAT_HANDLES; i++) {
        if (hueco_disponible(fat_handles[i].used, &duena_fat[i])) {
            duena_fat[i] = duena_actual();
            fat_handles[i].used = true;
            fat_handles[i].has_entry = exists;
            fat_handles[i].cursor.cluster = 0;       // cursor a cero: se coloca solo
            fat_handles[i].cursor.indice = 0;
            fat_handles[i].fin.cluster = 0;
            fat_handles[i].fin.indice = 0;
            if (exists) {
                fat_handles[i].entry = entry;
            } else {
                int j = 0;
                while (name[j] && j < FAT_NAME_LEN - 1) { fat_handles[i].pending_name[j] = name[j]; j++; }
                fat_handles[i].pending_name[j] = '\0';
            }
            return i;
        }
    }
    return -1; // sin huecos
}

// -- ReadFile/ReadLine$/Eof/CloseFile estilo BlitzPlus --
//
// Lectura de archivos de texto linea a linea. Cargamos el archivo
// ENTERO en un buffer propio al abrirlo (no hay lectura perezosa por
// trozos) y vamos avanzando un puntero de posicion -- de sobra para
// archivos de texto normales, y mucho mas simple que llevar la cuenta
// de bloques sueltos de NemoFS.
// ---- UN BUFFER POR ARCHIVO, DEL TAMAÑO DEL ARCHIVO ----
//
// Antes: ocho huecos, cada uno con un array estatico de 16 KB. Dos
// problemas, y el segundo es grave:
//
//   - 128 KB del kernel reservados SIEMPRE, se usaran o no.
//   - un archivo de mas de 16383 bytes se abria TRUNCADO y SIN DECIR NADA:
//     ReadLine$ daba lineas hasta el corte y Eof decia que se habia acabado,
//     igual que si el archivo terminara ahi. Para leer es una molestia; para
//     un editor que despues guarda, es perder lo que habia detras del corte.
//     El propio syscall.c no cabia.
//
// Ahora el buffer se pide con kmalloc del tamaño que el archivo mide de
// verdad (nemofs_file_size) y se suelta al cerrar. Es el mismo cambio que ya
// se hizo con las imagenes -- ver la nota junto a image_alloc_slot -- y con
// leer_en en la biblioteca de Lua.
//
// Y como los huecos ya no cuestan memoria por existir, pasan de 8 a 32:
// eran ocho para DIECISEIS tareas, y son de todo el sistema, no de cada
// programa, asi que un programa que abriera unos cuantos dejaba sin ninguno
// a los demas.
//
// LOS DOS TOPES. Uno por archivo y otro para TODOS JUNTOS. El segundo es el
// que importa, y es la leccion que ya dejaron escritas las imagenes: sin el,
// un programa con un bucle de OpenFile se comeria los 64 MB del heap del
// kernel, y lo que fallaria despues seria cualquier otra cosa, en cualquier
// otro sitio.
#define MAX_READ_FILES 32
#define READ_FILE_MAX       (8u * 1024u * 1024u)    // 8 MB el archivo mas grande
#define READ_FILE_TOTAL_MAX (24u * 1024u * 1024u)   // 24 MB entre todos los abiertos
static bool rf_used[MAX_READ_FILES];
static duena_t duena_rf[MAX_READ_FILES];   // dueño de cada hueco, ver hueco_disponible
static uint32_t rf_pos[MAX_READ_FILES];
static uint32_t rf_len[MAX_READ_FILES];
static char *rf_buf[MAX_READ_FILES];       // del heap: 0 cuando el hueco no tiene nada
static uint32_t rf_bytes[MAX_READ_FILES];  // lo que se pidio, para poder descontarlo
static uint32_t rf_bytes_total;            // suma de los anteriores

// Suelta el buffer de un hueco y lo deja limpio. Se llama al cerrar Y al
// reciclar el hueco de un programa que murio sin cerrar: sin eso, su memoria
// se quedaria reservada hasta el siguiente arranque, que es justo el goteo
// que estos cambios vienen a quitar.
static void readfile_soltar(int32_t slot) {
    if (slot < 0 || slot >= MAX_READ_FILES) return;
    if (rf_buf[slot]) {
        rf_bytes_total -= rf_bytes[slot];
        kfree(rf_buf[slot]);
        rf_buf[slot] = 0;
    }
    rf_bytes[slot] = 0;
    rf_used[slot] = false;
    rf_pos[slot] = 0;
    rf_len[slot] = 0;
}

// Busca el archivo primero en la raiz, y si no esta ahi, dentro de
// DOCUMENTOS -- los dos sitios donde suele vivir un archivo de texto.
// RUTAS CON CARPETAS.
// Hasta ahora un nombre de archivo era un nombre a secas y el kernel lo
// buscaba en unos pocos sitios fijos (la raiz, DOCUMENTOS, y para las
// imagenes tambien DOCUMENTOS/IMAGENES). Un juego quiere sus cosas en su
// sitio: "JUEGOS/NEMO/tiles.nimg".
//
// Estas dos funciones caminan una ruta componente a componente con
// nemofs_find_child. Aceptan '/' y '\\' como separador y una barra inicial
// ("/JUEGOS/...") que solo significa "desde la raiz", que es de donde se
// parte siempre. NO se admite ".." -- no hay carpeta actual de la que
// subir, y permitirlo solo daria formas nuevas de salirse.
//
// Si la ruta NO lleva separador, ruta_resolver_inode devuelve -2: la
// senal de "esto no es una ruta, sigue con tu busqueda de siempre". Asi
// los sitios que ya buscaban en varias carpetas no cambian de conducta.
#define RUTA_NO_ES_RUTA (-2)

static bool ruta_tiene_carpeta(const char *ruta) {
    for (const char *p = ruta; *p; p++) if (*p == '/' || *p == '\\') return true;
    return false;
}

// Devuelve el inodo de la CARPETA que contiene el ultimo componente, y
// deja ese ultimo componente en 'hoja' (hasta NEMOFS_MAX_NAME). Sirve
// tanto para leer como para crear un archivo nuevo.
static int32_t ruta_resolver_padre(const char *ruta, char *hoja, uint32_t hoja_max) {
    if (!ruta || !hoja || hoja_max == 0) return -1;
    if (!ruta_tiene_carpeta(ruta)) return RUTA_NO_ES_RUTA;

    // Una ruta que ACABA en separador nombra una carpeta, no
    // un archivo. El bucle de abajo no lo veia: en "JUEGOS/" se comia el
    // separador final y devolvia "la hoja JUEGOS dentro de la raiz", o
    // sea, el propio directorio tratado como archivo. Para leer daba
    // igual (ruta_resolver_inode rechaza lo que no sea archivo), pero
    // para CREAR -- WriteFile, SaveImage -- eso acababa escribiendo
    // encima del inodo de una carpeta.
    uint32_t largo = 0;
    while (ruta[largo]) largo++;
    if (largo > 0 && (ruta[largo - 1] == '/' || ruta[largo - 1] == '\\')) return -1;

    const char *p = ruta;
    while (*p == '/' || *p == '\\') p++;          // barra inicial: desde la raiz
    int32_t actual = (int32_t)NEMOFS_ROOT_INODE;
    char tramo[NEMOFS_MAX_NAME + 1];

    for (;;) {
        uint32_t n = 0;
        while (p[n] && p[n] != '/' && p[n] != '\\') {
            if (n >= NEMOFS_MAX_NAME) return -1;   // componente demasiado largo
            tramo[n] = p[n];
            n++;
        }
        tramo[n] = '\0';
        const char *sig = p + n;
        while (*sig == '/' || *sig == '\\') sig++;

        if (*sig == '\0') {
            // 'tramo' es el ultimo componente: el archivo
            if (n == 0) return -1;                 // la ruta acaba en separador
            uint32_t i = 0;
            while (i < n && i + 1 < hoja_max) { hoja[i] = tramo[i]; i++; }
            hoja[i] = '\0';
            return actual;
        }

        if (n == 0) return -1;
        if (tramo[0] == '.' && (tramo[1] == '\0' || (tramo[1] == '.' && tramo[2] == '\0'))) return -1;
        int32_t hijo = nemofs_find_child((uint32_t)actual, tramo);
        if (hijo < 0) return -1;                   // esa carpeta no existe
        if (nemofs_type_by_inode((uint32_t)hijo) != NEMOFS_TYPE_DIR) return -1;  // hay un ARCHIVO donde se esperaba carpeta
        actual = hijo;
        p = sig;
    }
}

static int32_t ruta_resolver_inode(const char *ruta) {
    char hoja[NEMOFS_MAX_NAME + 1];
    int32_t padre = ruta_resolver_padre(ruta, hoja, sizeof(hoja));
    if (padre == RUTA_NO_ES_RUTA) return RUTA_NO_ES_RUTA;
    if (padre < 0) return -1;
    int32_t hijo = nemofs_find_child((uint32_t)padre, hoja);
    if (hijo < 0) return -1;
    // Una CARPETA no es un archivo. Sin esto, "JUEGOS/NEMO/" devolvia el
    // inodo del directorio y LoadImage/OpenFile habrian leido un
    // directorio creyendo que era un archivo.
    if (nemofs_type_by_inode((uint32_t)hijo) != NEMOFS_TYPE_FILE) return -1;
    return hijo;
}

// Las syscalls que reciben (nombre, carpeta, volumen) -- abrir/crear un
// archivo, crear una carpeta, borrar -- comparten esta forma. Si el nombre
// trae carpetas, la ruta manda y el argumento 'carpeta' se ignora; si no,
// no cambia nada. Devuelve:
//    1  era una ruta y se resolvio: *padre y *nombre ya apuntan a lo bueno
//    0  no era una ruta: seguir con la busqueda de siempre
//   -1  era una ruta pero esa carpeta no existe: fallar, no crear nada
//       suelto en la raiz por las buenas
//
// ---- INTERRUPTOR ----
// En 0, ruta_o_nombre() y buscar_con_padre() se comportan EXACTAMENTE como
// antes de que existieran: las syscalls 20, 24, 25, 81, 82 y 83 y el borrado
// vuelven a tratar cualquier nombre como un nombre a secas, y las rutas con
// carpetas dejan de funcionar en ellas. Lo que ya caminaba rutas desde antes
// (OpenFile, WriteFile, LoadImage, SaveImage, via genfile_open y
// readfile_resolve_inode) NO se toca: eso es anterior y sigue igual.
//
// Sirve para partir en dos el problema en una sola compilacion: si con esto
// en 0 el sistema va bien, el fallo esta en estas funciones; si sigue igual,
// no es esto. Se deja puesto aposta: es barato y la proxima vez que algo
// raro pase por aqui, ahorra una tarde.
#define RUTAS_EN_SYSCALLS 1

static int ruta_o_nombre(const char **nombre, uint32_t *padre, char *hoja, uint32_t hoja_max) {
#if !RUTAS_EN_SYSCALLS
    (void)nombre; (void)padre; (void)hoja; (void)hoja_max;
    return 0;                      // "no es una ruta": la busqueda de siempre
#else
    int32_t por_ruta = ruta_resolver_padre(*nombre, hoja, hoja_max);
    if (por_ruta == RUTA_NO_ES_RUTA) return 0;
    if (por_ruta < 0) return -1;
    *padre = (uint32_t)por_ruta;
    *nombre = hoja;
    return 1;
#endif
}

static int32_t readfile_resolve_inode(const char *filename) {
    int32_t por_ruta = ruta_resolver_inode(filename);
    if (por_ruta != RUTA_NO_ES_RUTA) return por_ruta;   // lleva carpetas: manda la ruta
    int32_t inode = nemofs_find_child(NEMOFS_ROOT_INODE, filename);
    if (inode >= 0) return inode;
    int32_t docs = nemofs_find_child(NEMOFS_ROOT_INODE, "DOCUMENTOS");
    if (docs >= 0) {
        inode = nemofs_find_child((uint32_t)docs, filename);
        if (inode >= 0) return inode;
    }
    return -1;
}

static int32_t readfile_open(const char *filename) {
    int32_t slot = -1;
    for (int i = 0; i < MAX_READ_FILES; i++) if (hueco_disponible(rf_used[i], &duena_rf[i])) { slot = i; break; }
    if (slot < 0) return -1;
    // Puede venir del programa que murio sin cerrar: soltar lo suyo primero.
    readfile_soltar(slot);
    duena_rf[slot] = duena_actual();

    int32_t inode = readfile_resolve_inode(filename);
    if (inode < 0) return -1;

    int32_t tam = nemofs_file_size((uint32_t)inode);
    if (tam < 0) return -1;
    if ((uint32_t)tam > READ_FILE_MAX) return -1;
    if (rf_bytes_total + (uint32_t)tam + 1u > READ_FILE_TOTAL_MAX) return -1;

    // +1 para el terminador: readfile_line y readfile_read_bytes recorren el
    // buffer por posicion, pero quien lo lea como cadena espera el cero.
    uint32_t pedidos = (uint32_t)tam + 1u;
    char *buf = (char *)kmalloc(pedidos);
    if (!buf) return -1;

    int32_t bytes = nemofs_read_file((uint32_t)inode, buf, (uint32_t)tam);
    if (bytes < 0) { kfree(buf); return -1; }

    buf[bytes] = '\0';
    rf_buf[slot] = buf;
    rf_bytes[slot] = pedidos;
    rf_bytes_total += pedidos;
    rf_pos[slot] = 0;
    rf_len[slot] = (uint32_t)bytes;
    rf_used[slot] = true;
    return slot;
}

// Devuelve la siguiente linea (sin el salto de linea, y quitando un
// posible '\r' de un archivo con finales de linea estilo Windows), y
// avanza el puntero de lectura. Cadena vacia si ya no queda nada.
static uint32_t readfile_line(int32_t handle, char *out, uint32_t max_len) {
    if (handle < 0 || handle >= MAX_READ_FILES || !rf_used[handle] || max_len == 0) {
        if (max_len) out[0] = '\0';
        return 0;
    }
    uint32_t p = rf_pos[handle];
    uint32_t len = rf_len[handle];
    uint32_t i = 0;
    while (p < len && rf_buf[handle][p] != '\n' && i < max_len - 1) {
        out[i++] = rf_buf[handle][p++];
    }
    while (p < len && rf_buf[handle][p] != '\n') p++; // por si la linea era mas larga que el buffer de salida
    if (p < len && rf_buf[handle][p] == '\n') p++;
    if (i > 0 && out[i - 1] == '\r') i--;
    out[i] = '\0';
    rf_pos[handle] = p;
    return i;
}

static bool readfile_eof(int32_t handle) {
    if (handle < 0 || handle >= MAX_READ_FILES || !rf_used[handle]) return true;
    return rf_pos[handle] >= rf_len[handle];
}

static void readfile_close(int32_t handle) {
    readfile_soltar(handle);   // cerrar devuelve el buffer, no solo el hueco
}

// Lectura a nivel de byte para el espacio de handles de ReadFile --
// igual que genfile_read_bytes, pero sobre rf_buf/rf_pos. Hace que
// ReadByte/ReadShort/ReadInt/ReadFloat/ReadString$ funcionen tambien
// con un handle de ReadFile, no solo de OpenFile/WriteFile (confirmado
// en el manual: "una variable valida establecida con OpenFile, ReadFile
// o OpenTCPStream").
static uint32_t readfile_read_bytes(int32_t handle, uint8_t *out, uint32_t count) {
    if (handle < 0 || handle >= MAX_READ_FILES || !rf_used[handle]) return 0;
    if (rf_pos[handle] >= rf_len[handle]) return 0;
    uint32_t avail = rf_len[handle] - rf_pos[handle];
    uint32_t n = count < avail ? count : avail;
    for (uint32_t i = 0; i < n; i++) out[i] = (uint8_t)rf_buf[handle][rf_pos[handle] + i];
    rf_pos[handle] += n;
    return n;
}

// -- Archivos "generales" (OpenFile/WriteFile) --
//
// A diferencia de ReadFile (solo lectura, solo texto), estos soportan
// lectura Y escritura, con posicion explicita (FilePos/SeekFile).
// Igual que ReadFile, cargamos el archivo entero en un buffer propio
// al abrirlo -- las syscalls de bajo nivel (SYS_FILE_READ/WRITE) no
// soportan posicion, solo archivo completo desde el principio, asi
// que hacemos todo el trabajo de posicionamiento aqui en memoria, y
// volcamos el resultado a disco de una vez al cerrar (si se modifico
// algo).
#define MAX_GEN_FILES 6
#define GEN_FILE_BUF_SIZE 65536
typedef struct {
    bool used;
    bool dirty;
    uint32_t pos;
    uint32_t len;
    uint32_t volume;         // VOLUME_NEMOFS o VOLUME_FAT
    int32_t nemofs_inode;    // valido si volume==NEMOFS; -1 si el archivo aun no existia (se crea al cerrar)
    uint32_t nemofs_parent;  // inodo padre donde vive/se creara, si volume==NEMOFS
    char name[64];
} genfile_t;
static genfile_t genfiles[MAX_GEN_FILES];
static duena_t duena_gen[MAX_GEN_FILES];   // dueño de cada hueco, ver hueco_disponible
static uint8_t genfile_buf[MAX_GEN_FILES][GEN_FILE_BUF_SIZE];

// mode: 0=OpenFile (busca en NemoFS raiz/DOCUMENTOS y luego FAT; si
// no existe en ningun sitio, FALLA -- BlitzPlus real no crea archivos
// con OpenFile), 1=WriteFile (siempre vacio, sin importar si ya
// existia -- pero respeta DONDE vivia, para no duplicarlo en otro
// volumen; SI crea el archivo si no existia en ningun sitio).
static int32_t genfile_open(const char *name, uint32_t mode) {
    // RUTAS CON CARPETAS. LoadImage, LoadAnimImage, ReadFile
    // y SaveImage ya caminan la ruta desde; esta se quedo
    // fuera, y con ella OpenFile Y WriteFile, que comparten funcion. El
    // efecto era el mismo que tenia SaveImage antes de arreglarlo:
    // WriteFile("JUEGOS/PARTIDA.DAT") creaba PARTIDA.DAT suelto en la
    // raiz, sin avisar de nada, y OpenFile de esa misma ruta no lo
    // encontraba nunca.
    //
    // Se resuelve ANTES de coger hueco, para no ocupar uno y soltarlo.
    char hoja[NEMOFS_MAX_NAME + 1];
    bool es_ruta = false;
    uint32_t padre_ruta = NEMOFS_ROOT_INODE;
    int32_t por_ruta = ruta_resolver_padre(name, hoja, sizeof(hoja));
    if (por_ruta >= 0) {
        padre_ruta = (uint32_t)por_ruta;
        name = hoja;                       // a partir de aqui, solo el nombre final
        es_ruta = true;
    } else if (por_ruta != RUTA_NO_ES_RUTA) {
        return -1;                         // lleva carpetas que no existen
    }

    int32_t slot = -1;
    for (int i = 0; i < MAX_GEN_FILES; i++) if (hueco_disponible(genfiles[i].used, &duena_gen[i])) { slot = i; break; }
    if (slot >= 0) { genfiles[slot].used = false; duena_gen[slot] = duena_actual(); }
    if (slot < 0) return -1;

    genfiles[slot].pos = 0;
    genfiles[slot].dirty = false;
    int n = 0;
    while (name[n] != '\0' && n < 63) { genfiles[slot].name[n] = name[n]; n++; }
    genfiles[slot].name[n] = '\0';

    int32_t idx;
    uint32_t parent_used;
    if (es_ruta) {
        // La ruta manda: ni busqueda en DOCUMENTOS ni en FAT (que no
        // tiene subcarpetas). Si no esta ahi, no esta.
        parent_used = padre_ruta;
        idx = nemofs_find_child(padre_ruta, name);
    } else {
        idx = nemofs_find_child(NEMOFS_ROOT_INODE, name);
        parent_used = NEMOFS_ROOT_INODE;
        if (idx < 0) {
            int32_t docs = nemofs_find_child(NEMOFS_ROOT_INODE, "DOCUMENTOS");
            if (docs >= 0) {
                int32_t idx2 = nemofs_find_child((uint32_t)docs, name);
                if (idx2 >= 0) { idx = idx2; parent_used = (uint32_t)docs; }
            }
        }
    }

    // Y si lo que hay con ese nombre es una CARPETA, no se
    // toca. Sin esto, WriteFile("DOCUMENTOS") encontraba el inodo del
    // directorio, lo vaciaba y al cerrar escribia datos de archivo
    // encima: la carpeta y todo lo que colgara de ella, perdidos, sin un
    // solo aviso. Estaba ahi desde antes de las rutas; con rutas solo
    // era mas facil de encontrar.
    if (idx >= 0 && nemofs_type_by_inode((uint32_t)idx) != NEMOFS_TYPE_FILE) return -1;

    if (idx >= 0) {
        // Ya existia en NemoFS.
        genfiles[slot].volume = VOLUME_NEMOFS;
        genfiles[slot].nemofs_inode = idx;
        genfiles[slot].nemofs_parent = parent_used;
        if (mode == 1) {
            genfiles[slot].len = 0; // WriteFile lo vacia, aunque siga viviendo en el mismo sitio
            genfiles[slot].dirty = true;
        } else {
            int32_t bytes = nemofs_read_file((uint32_t)idx, genfile_buf[slot], GEN_FILE_BUF_SIZE);
            genfiles[slot].len = bytes >= 0 ? (uint32_t)bytes : 0;
        }
        genfiles[slot].used = true;
        return slot;
    }

    fat_dirent_t entry;
    if (!es_ruta && fat_find_root(name, &entry)) {
        // Ya existia en FAT. (Solo si el nombre venia suelto: una ruta
        // con carpetas no puede referirse a FAT, que no las tiene.)
        genfiles[slot].volume = VOLUME_FAT;
        if (mode == 1) {
            genfiles[slot].len = 0;
            genfiles[slot].dirty = true;
        } else {
            uint32_t out_size = 0;
            fat_read_file(&entry, genfile_buf[slot], GEN_FILE_BUF_SIZE, &out_size);
            genfiles[slot].len = out_size;
        }
        genfiles[slot].used = true;
        return slot;
    }

    if (mode == 0) {
        // OpenFile: el manual es explicito -- "el archivo debe
        // existir porque esta funcion no creara uno nuevo... el
        // handle seria igual a 0" -- si no aparecio en NemoFS ni en
        // FAT, fallamos en vez de crearlo vacio (eso es cosa de
        // WriteFile).
        return -1;
    }

    // No existia en ningun sitio -- WriteFile SI lo crea vacio, y desde
    // lo crea en la CARPETA QUE DIGA LA RUTA, no siempre
    // en la raiz.
    genfiles[slot].volume = VOLUME_NEMOFS;
    genfiles[slot].nemofs_inode = -1;
    genfiles[slot].nemofs_parent = es_ruta ? padre_ruta : NEMOFS_ROOT_INODE;
    genfiles[slot].len = 0;
    genfiles[slot].dirty = true; // para que se cree de verdad al cerrar, aunque no se escriba nada
    genfiles[slot].used = true;
    return slot;
}

static uint32_t genfile_read_bytes(int32_t handle, uint8_t *out, uint32_t count) {
    if (handle < 0 || handle >= MAX_GEN_FILES || !genfiles[handle].used) return 0;
    if (genfiles[handle].pos >= genfiles[handle].len) return 0;
    uint32_t avail = genfiles[handle].len - genfiles[handle].pos;
    uint32_t n = count < avail ? count : avail;
    for (uint32_t i = 0; i < n; i++) out[i] = genfile_buf[handle][genfiles[handle].pos + i];
    genfiles[handle].pos += n;
    return n;
}

static bool genfile_write_bytes(int32_t handle, const uint8_t *data, uint32_t count) {
    if (handle < 0 || handle >= MAX_GEN_FILES || !genfiles[handle].used) return false;
    if ((uint64_t)genfiles[handle].pos + count > GEN_FILE_BUF_SIZE) return false;
    for (uint32_t i = 0; i < count; i++) genfile_buf[handle][genfiles[handle].pos + i] = data[i];
    genfiles[handle].pos += count;
    if (genfiles[handle].pos > genfiles[handle].len) genfiles[handle].len = genfiles[handle].pos;
    genfiles[handle].dirty = true;
    return true;
}

static uint32_t genfile_pos(int32_t handle) {
    if (handle < 0 || handle >= MAX_GEN_FILES || !genfiles[handle].used) return 0;
    return genfiles[handle].pos;
}

static bool genfile_seek(int32_t handle, uint32_t new_pos) {
    if (handle < 0 || handle >= MAX_GEN_FILES || !genfiles[handle].used) return false;
    if (new_pos > genfiles[handle].len) return false;
    genfiles[handle].pos = new_pos;
    return true;
}

static uint32_t genfile_size(int32_t handle) {
    if (handle < 0 || handle >= MAX_GEN_FILES || !genfiles[handle].used) return 0;
    return genfiles[handle].len;
}

static bool genfile_eof(int32_t handle) {
    if (handle < 0 || handle >= MAX_GEN_FILES || !genfiles[handle].used) return true;
    return genfiles[handle].pos >= genfiles[handle].len;
}

static void genfile_close(int32_t handle) {
    if (handle < 0 || handle >= MAX_GEN_FILES || !genfiles[handle].used) return;
    if (genfiles[handle].dirty) {
        if (genfiles[handle].volume == VOLUME_FAT) {
            fat_write_file(genfiles[handle].name, genfile_buf[handle], genfiles[handle].len);
        } else {
            if (genfiles[handle].nemofs_inode < 0) {
                genfiles[handle].nemofs_inode = nemofs_create(genfiles[handle].nemofs_parent, genfiles[handle].name, NEMOFS_TYPE_FILE);
            }
            if (genfiles[handle].nemofs_inode >= 0) {
                nemofs_write_file((uint32_t)genfiles[handle].nemofs_inode, genfile_buf[handle], genfiles[handle].len);
            }
        }
    }
    genfiles[handle].used = false;
}

// -- Iteracion de carpetas (ReadDir/NextFile$/CloseDir) --
//
// Por encima de nemofs_list_dir (la misma funcion que ya usa
// SYS_FILE_LIST): pedimos el listado UNA vez al abrir, lo guardamos
// en un hueco propio, y vamos devolviendo un nombre cada vez que se
// llama a NextFile$. Solo NemoFS -- FAT v1 no tiene subcarpetas, asi
// que no hay "directorios" que iterar ahi aparte de la raiz (que ya
// cubre el Explorador via SYS_FILE_LIST directamente).
#define MAX_DIR_HANDLES 4
#define DIR_MAX_ENTRIES 64
typedef struct {
    bool used;
    uint32_t count;
    uint32_t pos;
    char names[DIR_MAX_ENTRIES][28];
} dir_iter_t;
static dir_iter_t dir_iters[MAX_DIR_HANDLES];
static duena_t duena_dir[MAX_DIR_HANDLES];   // dueño de cada hueco, ver hueco_disponible

static int32_t dir_open(uint32_t parent) {
    int32_t slot = -1;
    for (int i = 0; i < MAX_DIR_HANDLES; i++) if (hueco_disponible(dir_iters[i].used, &duena_dir[i])) { slot = i; break; }
    if (slot >= 0) { dir_iters[slot].used = false; duena_dir[slot] = duena_actual(); }
    if (slot < 0) return -1;

    static nemofs_dirent_t tmp[DIR_MAX_ENTRIES];
    uint32_t total = nemofs_list_dir(parent, tmp, DIR_MAX_ENTRIES);
    uint32_t n = total < DIR_MAX_ENTRIES ? total : DIR_MAX_ENTRIES;
    for (uint32_t i = 0; i < n; i++) {
        int j = 0;
        while (tmp[i].name[j] != '\0' && j < 27) { dir_iters[slot].names[i][j] = tmp[i].name[j]; j++; }
        dir_iters[slot].names[i][j] = '\0';
    }
    dir_iters[slot].count = n;
    dir_iters[slot].pos = 0;
    dir_iters[slot].used = true;
    return slot;
}

static uint32_t dir_next(int32_t handle, char *out, uint32_t max_len) {
    if (handle < 0 || handle >= MAX_DIR_HANDLES || !dir_iters[handle].used || max_len == 0) {
        if (max_len) out[0] = '\0';
        return 0;
    }
    if (dir_iters[handle].pos >= dir_iters[handle].count) { out[0] = '\0'; return 0; }
    const char *name = dir_iters[handle].names[dir_iters[handle].pos];
    dir_iters[handle].pos++;
    uint32_t i = 0;
    while (name[i] != '\0' && i < max_len - 1) { out[i] = name[i]; i++; }
    out[i] = '\0';
    return i;
}

static void dir_close(int32_t handle) {
    if (handle < 0 || handle >= MAX_DIR_HANDLES) return;
    dir_iters[handle].used = false;
}

// -- Utilidades por NOMBRE (buscan en NemoFS raiz+DOCUMENTOS y luego
// FAT) -- para FileSize/FileType/DeleteFile/DeleteDir, sin necesidad
// de abrir un handle primero.
// Busca un nombre en NemoFS y deja TAMBIEN su carpeta, que hace falta para
// sacar el tamaño o el tipo (que salen del listado del padre, no del inodo).
// Si el nombre trae carpetas, la ruta manda y 'era_ruta' se pone a true: el
// que llama no debe entonces probar suerte en FAT con la ruta entera, que
// alli no significa nada. Sin carpetas, la busqueda de siempre: raiz y
// DOCUMENTOS.
static int32_t buscar_con_padre(const char *name, uint32_t *parent, bool *era_ruta) {
    char hoja[NEMOFS_MAX_NAME + 1];
    const char *hijo = name;
    uint32_t padre = NEMOFS_ROOT_INODE;
    int es_ruta = ruta_o_nombre(&hijo, &padre, hoja, sizeof(hoja));
    if (es_ruta != 0) {
        *era_ruta = true;
        *parent = padre;
        return (es_ruta > 0) ? nemofs_find_child(padre, hijo) : -1;
    }
    *era_ruta = false;
    *parent = NEMOFS_ROOT_INODE;
    int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, name);
    if (idx < 0) {
        int32_t docs = nemofs_find_child(NEMOFS_ROOT_INODE, "DOCUMENTOS");
        if (docs >= 0) {
            int32_t idx2 = nemofs_find_child((uint32_t)docs, name);
            if (idx2 >= 0) { idx = idx2; *parent = (uint32_t)docs; }
        }
    }
    return idx;
}

static int32_t file_size_by_name(const char *name) {
    bool era_ruta = false;
    uint32_t parent = NEMOFS_ROOT_INODE;
    int32_t idx = buscar_con_padre(name, &parent, &era_ruta);
    if (idx >= 0) {
        // No tenemos una consulta directa "dame el tamaño de este
        // inodo" -- reutilizamos el listado de su carpeta padre y
        // buscamos la entrada por inodo, igual que en file_type_by_name.
        static nemofs_dirent_t tmp[512];   // mismo tope corregido que SYS_FILE_LIST
        uint32_t total = nemofs_list_dir(parent, tmp, 512);
        uint32_t n = total < 512 ? total : 512;
        for (uint32_t i = 0; i < n; i++) {
            if ((int32_t)tmp[i].inode == idx) return (int32_t)tmp[i].size;
        }
        return -1;
    }
    if (era_ruta) return -1;      // una ruta no se busca en FAT: alli no hay carpetas
    fat_dirent_t entry;
    if (fat_find_root(name, &entry)) return (int32_t)entry.size;
    return -1;
}

static int32_t file_type_by_name(const char *name) {
    bool era_ruta = false;
    uint32_t parent = NEMOFS_ROOT_INODE;
    int32_t idx = buscar_con_padre(name, &parent, &era_ruta);
    if (idx >= 0) {
        static nemofs_dirent_t tmp[512];   // mismo tope corregido que SYS_FILE_LIST
        uint32_t total = nemofs_list_dir(parent, tmp, 512);
        uint32_t n = total < 512 ? total : 512;
        for (uint32_t i = 0; i < n; i++) {
            if ((int32_t)tmp[i].inode == idx) return (int32_t)tmp[i].type;
        }
        return 1; // por si acaso, asumimos archivo
    }
    if (era_ruta) return 0;       // una ruta no se busca en FAT
    fat_dirent_t entry;
    if (fat_find_root(name, &entry)) return entry.is_dir ? NEMOFS_TYPE_DIR : NEMOFS_TYPE_FILE;
    return 0;
}

static void delete_anywhere(const char *name, int32_t *out_ok) {
    // Con carpetas, la ruta manda y solo se borra AHI. Buscar ademas en
    // DOCUMENTOS y en FAT convertiria un "JUEGOS/viejo.dat" que no existe en
    // el borrado de otro "viejo.dat" que si -- borrar el archivo equivocado
    // es de las pocas cosas que no tienen vuelta atras.
    {
        char hoja[NEMOFS_MAX_NAME + 1];
        uint32_t padre = NEMOFS_ROOT_INODE;
        const char *n = name;
        int es_ruta = ruta_o_nombre(&n, &padre, hoja, sizeof(hoja));
        if (es_ruta != 0) {   // era una ruta, con o sin suerte
            *out_ok = (es_ruta > 0 && nemofs_delete(padre, n)) ? 0 : -1;
            return;
        }
    }
    int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, name);
    if (idx >= 0) { *out_ok = nemofs_delete(NEMOFS_ROOT_INODE, name) ? 0 : -1; return; }
    int32_t docs = nemofs_find_child(NEMOFS_ROOT_INODE, "DOCUMENTOS");
    if (docs >= 0) {
        int32_t idx2 = nemofs_find_child((uint32_t)docs, name);
        if (idx2 >= 0) { *out_ok = nemofs_delete((uint32_t)docs, name) ? 0 : -1; return; }
    }
    *out_ok = fat_delete_file(name) ? 0 : -1;
}

// Renombrar buscando donde esta, igual que delete_anywhere y por los mismos
// motivos: los comandos de archivo de Nemo Basic reciben un nombre a secas y
// no tienen forma de decir "en la carpeta tal del volumen cual".
//
// Con carpetas, la ruta manda y solo se renombra AHI: buscar ademas en
// DOCUMENTOS convertiria un "JUEGOS/viejo.dat" que no existe en el renombrado
// de otro "viejo.dat" que si.
//
// El nombre NUEVO se toma tal cual, sin ruta: renombrar no mueve de carpeta.
// Un "a/b.txt" como destino se quedaria como un nombre con barra dentro, que
// ningun otro comando sabria abrir despues, asi que se rechaza.
static void rename_anywhere(const char *viejo, const char *nuevo, int32_t *out_ok) {
    for (const char *c = nuevo; *c; c++)
        if (*c == '/' || *c == '\\') { *out_ok = -1; return; }

    {
        char hoja[NEMOFS_MAX_NAME + 1];
        uint32_t padre = NEMOFS_ROOT_INODE;
        const char *n = viejo;
        int es_ruta = ruta_o_nombre(&n, &padre, hoja, sizeof(hoja));
        if (es_ruta != 0) {
            *out_ok = (es_ruta > 0 && nemofs_rename(padre, n, nuevo)) ? 0 : -1;
            return;
        }
    }
    if (nemofs_find_child(NEMOFS_ROOT_INODE, viejo) >= 0) {
        *out_ok = nemofs_rename(NEMOFS_ROOT_INODE, viejo, nuevo) ? 0 : -1;
        return;
    }
    int32_t docs = nemofs_find_child(NEMOFS_ROOT_INODE, "DOCUMENTOS");
    if (docs >= 0 && nemofs_find_child((uint32_t)docs, viejo) >= 0) {
        *out_ok = nemofs_rename((uint32_t)docs, viejo, nuevo) ? 0 : -1;
        return;
    }
    // En FAT no se puede: tocaria la entrada de directorio a mano, y fat.c
    // no ofrece renombrar como operacion propia. Se dice que no en vez de
    // fingir que se hizo.
    *out_ok = -1;
}

// -- LoadImage/DrawImage/ImageWidth+Height --
//
// Formato propio "NIMG": nada de PNG/JPEG de verdad (no tenemos
// decodificador, y escribir uno es un proyecto aparte). Cabecera de
// 12 bytes (magic "NIMG", ancho, alto, los dos como uint32 little-
// endian) seguida de los pixeles RGBA en crudo, fila a fila -- el
// mismo formato que ya usamos para los iconos embebidos, solo que
// aqui vive en un archivo de verdad en vez de compilado en el kernel.
// Un script en el host (nimg_convert.py) convierte un PNG normal a
// este formato.
#define MAX_IMAGES 64   // eran 16, con 256KB estaticos cada uno. Ahora los\n // pixeles van al heap, asi que un hueco vacio no cuesta nada.

// Origen de dibujo (Origin/Viewport) -- desplazamiento que se suma a
// TODAS las coordenadas de las syscalls de dibujo (Rect/Text/Ovalo/
// Imagen), asi que Plot/Rect/Line/Oval/Text/DrawImage lo respetan
// automaticamente sin que el compilador tenga que tocar cada una por
// separado -- Line en particular ya dibuja llamando repetidamente a
// SYS_DRAW_RECT, asi que tambien queda cubierta gratis.
#include "tasks.h"   // MAX_TASKS y task_get_current_slot, para el estado por tarea

// ---- Estado de dibujo POR TAREA (SMP, fase 5b) ----
//
// Origin, SetBuffer, el lienzo activo, Viewport, la fuente y LockBuffer
// eran variables GLOBALES: "el estado de quien este dibujando". Con un
// solo nucleo funcionaba por casualidad, porque una tarea solo cede el
// turno en puntos concretos (Pump, WaitEvent...), asi que su secuencia
// "fijar destino -> dibujar -> dibujar -> ceder" nunca se mezclaba con la
// de otra.
//
// Con varios nucleos, entre dos llamadas al sistema de la misma tarea el
// candado grande queda libre, y otra tarea en otro nucleo puede cambiar
// ese estado. Si el explorador fijaba su destino y en medio el reloj de
// Lua fijaba el suyo, parte de los dibujos del explorador acababan en el
// sitio del reloj: la ventana del explorador se redibujaba mal en la Pi.
// El candado no protege esto -- impide que dos nucleos esten A LA VEZ en
// el kernel, no que las secuencias de llamadas de dos tareas se mezclen.
//
// Ahora cada tarea tiene su propia copia. Las macros conservan los
// nombres de siempre (g_origin_x...), asi que el codigo que los usa no
// cambia: cada acceso va a la copia de la tarea del nucleo que lo
// ejecuta. La ultima entrada es para el kernel, que no es ninguna tarea.
//
// El gamma de la pantalla y el dueño del teclado (g_input_owner) SI son
// globales de verdad -- son del sistema, no de una tarea -- y se quedan
// como estaban.
typedef struct {
    int32_t origin_x, origin_y;
    int32_t draw_target_image;
    int32_t draw_target_canvas;   // id de gadget Canvas activo, -1 = ninguno
    bool viewport_active;
    int32_t viewport_x, viewport_y;
    uint32_t viewport_w, viewport_h;
    bool auto_mid_handle;
    int32_t current_font;         // indice (no handle) de SetFont, -1 = ninguna
    // Cara proporcional activa (NULL = la 5x7). Se quedo fuera la primera
    // vez y seguia siendo GLOBAL: con dos programas usando fuentes en
    // nucleos distintos, uno escribia -- y MEDIA -- su texto con la fuente
    // que acababa de elegir el otro. El visor de Lua, con dos documentos
    // abiertos, veia letras equivocadas y maquetaba mal hasta que su
    // script fallaba y se cerraba la ventana.
    const nfnt_cara_t *cara;
    uint32_t font_scale;
    bool font_bold;
    bool buffer_locked;
    int32_t locked_buffer_id;     // 0=ventana, N=imagen N-1 (convencion de SetBuffer)
    uint32_t locked_width, locked_height;
} estado_dibujo_t;

// Los mismos valores iniciales que tenian las globales.
#define ESTADO_DIBUJO_INICIAL { \
    .origin_x = 0, .origin_y = 0, .draw_target_image = -1, .draw_target_canvas = -1, \
    .viewport_active = false, .viewport_x = 0, .viewport_y = 0, .viewport_w = 0, .viewport_h = 0, \
    .auto_mid_handle = false, .current_font = -1, .cara = 0, .font_scale = 1, .font_bold = false, \
    .buffer_locked = false, .locked_buffer_id = 0, .locked_width = 0, .locked_height = 0 }

static estado_dibujo_t estado_dibujo[MAX_TASKS + 1] = { [0 ... MAX_TASKS] = ESTADO_DIBUJO_INICIAL };

static inline estado_dibujo_t *dibujo(void) {
    int32_t t = task_get_current_slot();
    return &estado_dibujo[(t >= 0 && t < MAX_TASKS) ? t : MAX_TASKS];
}

// Una tarea NUEVA empieza con el estado de dibujo limpio. Sin esto
// heredaria el de la tarea que ocupo antes su hueco: un SetBuffer o un
// Viewport de un programa ya cerrado.
void syscall_reiniciar_dibujo(int32_t tarea) {
    static const estado_dibujo_t inicial = ESTADO_DIBUJO_INICIAL;
    if (tarea >= 0 && tarea < MAX_TASKS) estado_dibujo[tarea] = inicial;
}

#define g_origin_x           (dibujo()->origin_x)
#define g_origin_y           (dibujo()->origin_y)
#define g_draw_target_image  (dibujo()->draw_target_image)
#define g_draw_target_canvas (dibujo()->draw_target_canvas)
#define g_viewport_active    (dibujo()->viewport_active)
#define g_viewport_x         (dibujo()->viewport_x)
#define g_viewport_y         (dibujo()->viewport_y)
#define g_viewport_w         (dibujo()->viewport_w)
#define g_viewport_h         (dibujo()->viewport_h)
#define g_auto_mid_handle    (dibujo()->auto_mid_handle)
#define g_current_font       (dibujo()->current_font)
#define g_cara               (dibujo()->cara)
#define g_font_scale         (dibujo()->font_scale)
#define g_font_bold          (dibujo()->font_bold)
#define g_buffer_locked      (dibujo()->buffer_locked)
#define g_locked_buffer_id   (dibujo()->locked_buffer_id)
#define g_locked_width       (dibujo()->locked_width)
#define g_locked_height      (dibujo()->locked_height)


// ImageBuffer(handle): redirige el dibujo hacia una imagen en vez de
// la ventana -- -1 significa "dibujar en la ventana", como siempre.
// Cubre Rect/Plot/Line/Cls (todos pasan por SYS_DRAW_RECT) y Oval.
// Text y DrawImage anidado siguen yendo SIEMPRE a la ventana --
// limitacion documentada, cubrir esos tambien pediria duplicar mucho
// mas codigo de bajo nivel.

// Quien tiene el teclado en EXCLUSIVA mientras lee una linea completa.
//
// Hace falta porque la shell consulta SYS_READ_CHAR en cada vuelta de
// su bucle, sin condiciones: mientras un programa lanzado por ella
// esperaba en SYS_READ_LINE, la shell le robaba las teclas y el
// programa no recibia nada -- el usuario escribia y le respondia el
// prompt de la shell.
//
// Con esto, mientras una tarea esta dentro de SYS_READ_LINE, las
// lecturas de caracter de las DEMAS tareas devuelven "no hay tecla",
// que es la verdad desde su punto de vista.
//
// -1 = nadie. Se pone al entrar y se quita al salir por TODOS los
// caminos, incluidos los de error: si se quedara puesto, la shell no
// volveria a responder nunca.
static int32_t g_input_owner = -1;

// CanvasBuffer(canvas): en vez de un buffer de pixeles aparte,
// redirige el dibujo aplicando Origin+Viewport automaticamente al
// rectangulo del gadget Canvas dentro de su ventana -- asi el dibujo
// normal (Rect/Plot/Line/Oval/Text/DrawImage, TODOS respetan
// Origin/Viewport ya) queda recortado y desplazado correctamente sin
// necesitar un camino de dibujo aparte. Usa un rango numerico bien
// separado del de ImageBuffer (que va de 0 a MAX_IMAGES-1) para que
// SetBuffer pueda distinguir "es un canvas" de "es una imagen" sin
// ambiguedad. LIMITACION: si el programa tenia su propio Origin o
// Viewport activo ANTES de entrar en el canvas, se pierde al salir
// (no se guarda/restaura) -- simplificacion razonable, dado que
// mezclar Canvas con Origin/Viewport manuales a la vez es un uso poco
// habitual.
#define CANVAS_BUFFER_OFFSET 100000

// Viewport(x,y,w,h): recorta el dibujo a un rectangulo, SEPARADO de
// Origin (que solo desplaza, no recorta) -- coordenadas LOCALES de
// la ventana, antes de sumar menu_off. g_viewport_active=false
// significa "sin recorte", toda la ventana como siempre.

// Calcula los limites de recorte actuales para SYS_DRAW_* -- si
// Viewport esta activo, la interseccion de su rectangulo con la
// ventana; si no, la ventana entera (el comportamiento de siempre).
static void get_clip_bounds(uint32_t ww, uint32_t wh, uint32_t menu_off, int32_t *cx0, int32_t *cy0, int32_t *cx1, int32_t *cy1) {
    if (g_viewport_active) {
        *cx0 = g_viewport_x;
        *cy0 = g_viewport_y + (int32_t)menu_off;
        *cx1 = g_viewport_x + (int32_t)g_viewport_w;
        *cy1 = g_viewport_y + (int32_t)menu_off + (int32_t)g_viewport_h;
        if (*cx0 < 0) *cx0 = 0;
        if (*cy0 < (int32_t)menu_off) *cy0 = (int32_t)menu_off;
        if (*cx1 > (int32_t)ww) *cx1 = (int32_t)ww;
        if (*cy1 > (int32_t)wh) *cy1 = (int32_t)wh;
    } else {
        *cx0 = 0;
        *cy0 = (int32_t)menu_off;
        *cx1 = (int32_t)ww;
        *cy1 = (int32_t)wh;
    }
}

// Calcula el recorte de un blit de w x h en (x,y) contra los limites
// de recorte actuales -- false si queda TOTALMENTE fuera (nada que
// dibujar). src_dx/src_dy son cuanto hay que desplazarse dentro del
// ORIGEN (la imagen) para compensar lo recortado por la
// izquierda/arriba, reutilizando wm_content_blit_image_rect (que ya
// admite un origen a mitad de camino de una imagen mas ancha).
static bool clip_blit(int32_t x, int32_t y, uint32_t w, uint32_t h,
                       int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1,
                       int32_t *out_x, int32_t *out_y, uint32_t *out_w, uint32_t *out_h,
                       uint32_t *src_dx, uint32_t *src_dy) {
    int32_t x2 = x + (int32_t)w, y2 = y + (int32_t)h;
    int32_t nx0 = x < cx0 ? cx0 : x;
    int32_t ny0 = y < cy0 ? cy0 : y;
    int32_t nx1 = x2 > cx1 ? cx1 : x2;
    int32_t ny1 = y2 > cy1 ? cy1 : y2;
    if (nx0 >= nx1 || ny0 >= ny1) return false;
    *src_dx = (uint32_t)(nx0 - x);
    *src_dy = (uint32_t)(ny0 - y);
    *out_x = nx0;
    *out_y = ny0;
    *out_w = (uint32_t)(nx1 - nx0);
    *out_h = (uint32_t)(ny1 - ny0);
    return true;
}
// Los pixeles de cada imagen se reservan con kmalloc, no en
// un array estatico. Antes eran 16 huecos FIJOS de 256x256 (image_pixels
// ocupaba 4MB de kernel estuviera vacio o lleno), y un juego de tiles se
// quedaba sin imagenes enseguida: solo el fondo pre-compuesto gasta 8.
// Ahora el hueco solo cuesta memoria cuando de verdad hay una imagen
// dentro, y ni el numero ni el tamaño estan atados al estatico.
//
// Dos topes: uno por imagen (IMAGE_BYTES_MAX) y otro para TODAS juntas
// (IMAGE_BYTES_TOTAL_MAX). El segundo importa: sin el, un programa con un
// bucle de LoadImage se comeria los 64MB del heap del kernel y lo que
// fallaria despues seria cualquier otra cosa, en cualquier otro sitio.
#define IMAGE_MAX_DIM 1024
#define IMAGE_BYTES_MAX (IMAGE_MAX_DIM * IMAGE_MAX_DIM * 4u)      // 4MB: la imagen mas grande posible
#define IMAGE_BYTES_TOTAL_MAX (24u * 1024u * 1024u)               // 24MB entre todas
typedef struct {
    bool used;
    uint32_t width, height;
    int32_t handle_x, handle_y; // punto de "agarre" para DrawImage/MidHandle -- (0,0) por defecto, la esquina superior izquierda
    uint32_t cell_width, cell_height; // >0 si es un sprite sheet cargado con LoadAnimImage -- DrawImage(...,frame) recorta esa celda
    uint32_t anim_first; // primera celda de la hoja que corresponde al fotograma 0 de la animacion (LoadAnimImage: parametro 'first')
    uint32_t anim_count; // cuantos fotogramas validos tiene la animacion, para recortar el indice y no leer fuera de la hoja
    bool has_mask;
    uint32_t mask_color; // MaskImage: color "clave" que se trata como transparente al dibujar -- NO se borra el pixel, se comprueba en cada blit (igual que BlitzPlus real)
} image_slot_t;
static image_slot_t images[MAX_IMAGES];
static duena_t duena_imagen[MAX_IMAGES];   // dueño de cada hueco, ver hueco_disponible
// Un hueco de imagen recien reservado: sin dueño anterior ni datos
// auxiliares heredados. Antes, al reutilizar un hueco no se borraban la
// transparencia ni las celdas de animacion, y una imagen nueva podia
// heredar el color transparente de la anterior. Queda sin usar hasta que
// quien la reserva la rellene y la marque.
static void imagen_soltar_pixeles(int32_t slot);   // adelantada: ver mas abajo

static void imagen_reservar(int32_t slot) {
    // Soltar ya los pixeles del dueño anterior. Si se dejaran para
    // cuando alguien vuelva a usar el hueco, la memoria de un programa
    // muerto seguiria reservada sin que nadie la reclamara.
    imagen_soltar_pixeles(slot);
    images[slot].used = false;
    images[slot].has_mask = false;
    images[slot].mask_color = 0;
    images[slot].cell_width = images[slot].cell_height = 0;
    images[slot].anim_first = images[slot].anim_count = 0;
    images[slot].handle_x = images[slot].handle_y = 0;
    duena_imagen[slot] = duena_actual();
}
static uint8_t *image_pixels[MAX_IMAGES];   // NULL si el hueco no tiene pixeles
static uint32_t image_bytes[MAX_IMAGES];    // cuanto se reservo para cada uno
static uint32_t image_bytes_total;          // suma de los anteriores

static void imagen_soltar_pixeles(int32_t slot) {
    if (slot < 0 || slot >= MAX_IMAGES || !image_pixels[slot]) return;
    kfree(image_pixels[slot]);
    image_pixels[slot] = 0;
    image_bytes_total -= image_bytes[slot];
    image_bytes[slot] = 0;
}

// Devuelve la memoria de las imagenes cuyo programa ya no existe. Los
// huecos se reciclaban solos (hueco_disponible), pero sus PIXELES se
// quedaban reservados hasta que alguien pedia ese hueco concreto: con
// memoria dinamica eso seria una fuga de hasta 4MB por imagen huerfana.
static void imagenes_barrer_muertas(void) {
    for (int i = 0; i < MAX_IMAGES; i++) {
        if (!image_pixels[i]) continue;
        if (hueco_disponible(images[i].used, &duena_imagen[i])) {
            images[i].used = false;
            imagen_soltar_pixeles(i);
        }
    }
}

static bool imagen_reservar_pixeles(int32_t slot, uint32_t bytes) {
    if (slot < 0 || slot >= MAX_IMAGES) return false;
    imagen_soltar_pixeles(slot);
    if (bytes == 0 || bytes > IMAGE_BYTES_MAX) return false;
    if (image_bytes_total + bytes > IMAGE_BYTES_TOTAL_MAX) {
        imagenes_barrer_muertas();                                  // segunda oportunidad
        if (image_bytes_total + bytes > IMAGE_BYTES_TOTAL_MAX) return false;
    }
    uint8_t *p = (uint8_t *)kmalloc(bytes);
    if (!p) return false;
    image_pixels[slot] = p;
    image_bytes[slot] = bytes;
    image_bytes_total += bytes;
    return true;
}

// AutoMidHandle(true) hace que LoadImage/CreateImage centren el punto
// de agarre automaticamente (ancho/2, alto/2) segun se crean, sin
// tener que llamar a MidHandle a mano cada vez.

// Ya no hay buffer de carga: se lee la cabecera de 12 bytes con
// nemofs_read_at, se reserva justo lo que dice, y los pixeles se leen
// DIRECTAMENTE encima. Antes se copiaba el archivo entero a un estatico
// y de ahi al hueco -- el doble de memoria y una copia de mas.

// -- LoadSound / almacen de sonidos --
//
// Cargamos archivos WAV reales (formato PCM sin comprimir), no un
// formato propio -- BlitzPlus real espera .wav. El WAV de origen
// puede venir en CUALQUIER frecuencia/profundidad/numero de canales
// soportado por el formato (8/16/24/32 bits, mono o estereo); lo
// convertimos SIEMPRE al formato fijo que espera nuestro driver de
// audio (44100Hz, 16 bits, estereo) al cargarlo, guardando ya el
// resultado convertido -- asi PlaySound no necesita volver a tocar
// los datos.
//
// Limite: 5 segundos por sonido (generoso para efectos de sonido
// tipicos; musica larga NO cabria, pero eso es un problema aparte de
// "PlayMusic" que no cubrimos aqui todavia).
#define MAX_SOUNDS 16
#define SOUND_MAX_FRAMES (44100u * 5u) // 5 segundos, en FRAMES estereo (L+R)
#define WAV_LOAD_BUF_BYTES (2u * 1024u * 1024u) // 2MB para el archivo WAV de origen sin convertir

static uint8_t wav_load_buf[WAV_LOAD_BUF_BYTES];

typedef struct {
    bool used;
    uint32_t frame_count; // frames estereo REALES guardados (<= SOUND_MAX_FRAMES)
    int16_t samples[SOUND_MAX_FRAMES * 2]; // entrelazado L,R,L,R,...
} sound_slot_t;

static sound_slot_t sounds[MAX_SOUNDS];
static duena_t duena_sonido[MAX_SOUNDS];   // dueño de cada hueco, ver hueco_disponible

// SoundVolume/SoundPan/SoundPitch -- guardados por sonido (declarados
// aqui, antes de sound_load, porque este ya los inicializa al cargar
// cada sonido).
//
// LIMITACION IMPORTANTE DEL KERNEL: -mgeneral-regs-only prohibe usar
// coma flotante en CODIGO C DEL KERNEL (el manejador de interrupciones
// en exceptions.s NO guarda los registros de coma flotante -- si el
// temporizador interrumpe a mitad de un calculo en punto flotante,
// SU ESTADO SE CORROMPERIA). Por eso, volumen/pan se guardan como
// ENTEROS de punto fijo "por mil" (0-1000 = 0.0-1.0 para volumen,
// -1000 a 1000 = -1.0 a 1.0 para pan) -- la CONVERSION desde el
// double que escribe el programa BlitzPlus se hace en el COMPILADOR
// (codegen.c), que SI puede usar coma flotante de verdad porque
// genera ensamblado de USUARIO (compilado aparte con nemoas, sin
// esta restriccion), no codigo C del kernel.
static int32_t sound_volume_permil[MAX_SOUNDS]; // 0-1000
static int32_t sound_pan_permil[MAX_SOUNDS];    // -1000 a 1000
static uint32_t sound_pitch_hz[MAX_SOUNDS];

// Lee UNA muestra (de 'bytes_per_sample' bytes, en little-endian) y
// la normaliza a rango de int16 con signo, sea cual sea la
// profundidad de bits original.
static int32_t wav_read_one_sample(const uint8_t *p, uint32_t bytes_per_sample) {
    if (bytes_per_sample == 1) {
        // WAV de 8 bits es SIN SIGNO, centrado en 128
        return ((int32_t)p[0] - 128) * 256;
    } else if (bytes_per_sample == 2) {
        int16_t v = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        return (int32_t)v;
    } else if (bytes_per_sample == 3) {
        int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
        if (v & 0x800000) v |= (int32_t)0xFF000000u; // extension de signo de 24 bits
        return v >> 8;
    } else {
        int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
        return v >> 16;
    }
}

// Busca los chunks 'fmt ' y 'data' dentro de un WAV (sin asumir un
// orden fijo entre ellos, saltando cualquier otro chunk que haya en
// medio como 'LIST') -- solo aceptamos PCM sin comprimir (audioFormat
// == 1).
static bool wav_parse(const uint8_t *buf, uint32_t len,
                       uint16_t *out_channels, uint32_t *out_rate, uint16_t *out_bits,
                       uint32_t *out_data_off, uint32_t *out_data_len) {
    if (len < 12) return false;
    if (buf[0] != 'R' || buf[1] != 'I' || buf[2] != 'F' || buf[3] != 'F') return false;
    if (buf[8] != 'W' || buf[9] != 'A' || buf[10] != 'V' || buf[11] != 'E') return false;

    bool have_fmt = false, have_data = false;
    uint32_t pos = 12;
    while (pos + 8 <= len) {
        uint8_t id0 = buf[pos], id1 = buf[pos + 1], id2 = buf[pos + 2], id3 = buf[pos + 3];
        uint32_t chunk_size = (uint32_t)buf[pos + 4] | ((uint32_t)buf[pos + 5] << 8) |
                               ((uint32_t)buf[pos + 6] << 16) | ((uint32_t)buf[pos + 7] << 24);
        uint32_t chunk_data_pos = pos + 8;

        if (id0 == 'f' && id1 == 'm' && id2 == 't' && id3 == ' ') {
            if (chunk_data_pos + 16 > len) return false;
            uint16_t audio_format = (uint16_t)buf[chunk_data_pos] | ((uint16_t)buf[chunk_data_pos + 1] << 8);
            if (audio_format != 1) return false; // solo PCM sin comprimir
            *out_channels = (uint16_t)buf[chunk_data_pos + 2] | ((uint16_t)buf[chunk_data_pos + 3] << 8);
            *out_rate = (uint32_t)buf[chunk_data_pos + 4] | ((uint32_t)buf[chunk_data_pos + 5] << 8) |
                        ((uint32_t)buf[chunk_data_pos + 6] << 16) | ((uint32_t)buf[chunk_data_pos + 7] << 24);
            *out_bits = (uint16_t)buf[chunk_data_pos + 14] | ((uint16_t)buf[chunk_data_pos + 15] << 8);
            have_fmt = true;
        } else if (id0 == 'd' && id1 == 'a' && id2 == 't' && id3 == 'a') {
            uint32_t avail = (chunk_data_pos < len) ? (len - chunk_data_pos) : 0;
            if (chunk_size > avail) chunk_size = avail; // recortamos si el archivo viene truncado
            *out_data_off = chunk_data_pos;
            *out_data_len = chunk_size;
            have_data = true;
        }

        uint32_t advance = chunk_size + (chunk_size & 1); // los chunks van alineados a 2 bytes
        if (chunk_data_pos + advance <= pos) break; // proteccion ante un chunk_size corrupto/cero que no avance
        pos = chunk_data_pos + advance;
        if (have_fmt && have_data) break;
    }
    return have_fmt && have_data;
}

// Convierte el PCM de origen (cualquier frecuencia/profundidad/canales)
// al formato fijo 44100Hz/16 bits/estereo, con remuestreo lineal si
// hace falta cambiar de frecuencia. Devuelve el numero de frames de
// salida (<= dst_max_frames).
static uint32_t wav_convert(uint16_t src_channels, uint32_t src_rate, uint16_t src_bits,
                             const uint8_t *src_data, uint32_t src_data_len,
                             int16_t *dst, uint32_t dst_max_frames) {
    if (src_channels == 0 || src_rate == 0) return 0;
    uint32_t bytes_per_sample = src_bits / 8;
    if (bytes_per_sample == 0 || bytes_per_sample > 4) return 0;
    uint32_t frame_bytes = bytes_per_sample * src_channels;
    if (frame_bytes == 0) return 0;
    uint32_t src_frame_count = src_data_len / frame_bytes;
    if (src_frame_count == 0) return 0;

    uint32_t out_frame_count = (uint32_t)(((uint64_t)src_frame_count * 44100u) / src_rate);
    if (out_frame_count > dst_max_frames) out_frame_count = dst_max_frames;
    if (out_frame_count == 0) return 0;

    for (uint32_t of = 0; of < out_frame_count; of++) {
        // posicion (en frames de origen) con 8 bits de fraccion, para
        // interpolar linealmente entre dos frames de origen vecinos.
        uint64_t src_pos_fixed = ((uint64_t)of * src_rate * 256u) / 44100u;
        uint32_t src_idx = (uint32_t)(src_pos_fixed / 256u);
        uint32_t frac = (uint32_t)(src_pos_fixed % 256u);
        if (src_idx >= src_frame_count) src_idx = src_frame_count - 1;
        uint32_t src_idx2 = (src_idx + 1 < src_frame_count) ? src_idx + 1 : src_idx;

        const uint8_t *f0 = src_data + (uint64_t)src_idx * frame_bytes;
        const uint8_t *f1 = src_data + (uint64_t)src_idx2 * frame_bytes;

        int32_t l0 = wav_read_one_sample(f0, bytes_per_sample);
        int32_t r0 = (src_channels >= 2) ? wav_read_one_sample(f0 + bytes_per_sample, bytes_per_sample) : l0;
        int32_t l1 = wav_read_one_sample(f1, bytes_per_sample);
        int32_t r1 = (src_channels >= 2) ? wav_read_one_sample(f1 + bytes_per_sample, bytes_per_sample) : l1;

        int32_t l = l0 + (int32_t)(((int64_t)(l1 - l0) * frac) / 256);
        int32_t r = r0 + (int32_t)(((int64_t)(r1 - r0) * frac) / 256);

        dst[of * 2 + 0] = (int16_t)l;
        dst[of * 2 + 1] = (int16_t)r;
    }
    return out_frame_count;
}

static int32_t sound_load(const char *filename) {
    int32_t slot = -1;
    for (int i = 0; i < MAX_SOUNDS; i++) if (hueco_disponible(sounds[i].used, &duena_sonido[i])) { slot = i; break; }
    if (slot >= 0) { sounds[slot].used = false; duena_sonido[slot] = duena_actual(); }
    if (slot < 0) return -1; // sin huecos libres

    int32_t inode = readfile_resolve_inode(filename);
    if (inode < 0) return -1;

    int32_t bytes = nemofs_read_file((uint32_t)inode, wav_load_buf, sizeof(wav_load_buf));
    if (bytes < 44) return -1; // ni siquiera cabe una cabecera WAV minima

    uint16_t channels, bits;
    uint32_t rate, data_off, data_len;
    if (!wav_parse(wav_load_buf, (uint32_t)bytes, &channels, &rate, &bits, &data_off, &data_len)) {
        return -1; // no es un WAV PCM valido
    }

    uint32_t frames = wav_convert(channels, rate, bits, wav_load_buf + data_off, data_len,
                                   sounds[slot].samples, SOUND_MAX_FRAMES);
    if (frames == 0) return -1;

    sounds[slot].frame_count = frames;
    sounds[slot].used = true;
    // volumen por defecto = 1.0 (a todo volumen) -- SIN esto, el
    // valor por defecto de un array estatico (0.0) dejaria CUALQUIER
    // sonido en silencio hasta que se llamara a SoundVolume.
    sound_volume_permil[slot] = 1000; // 1.0 = a todo volumen
    sound_pan_permil[slot] = 0;
    sound_pitch_hz[slot] = 0; // 0 = sin override de tono (usa la frecuencia real guardada)
    return slot;
}

static void sound_free(int32_t handle) {
    if (handle < 0 || handle >= MAX_SOUNDS) return;
    sounds[handle].used = false;
}

// SoundVolume/SoundPan/SoundPitch -- aplicados en el momento de
// reproducir (no modifican los datos ORIGINALES guardados, asi que se
// pueden cambiar entre una reproduccion y la siguiente). Volumen y
// pan tienen efecto real; el tono (pitch) tambien -- reutiliza la
// MISMA tecnica de remuestreo lineal del cargador WAV, tratando los
// samples ya guardados a 44100Hz como si su frecuencia "nativa" fuera
// la de 'pitch_hz', lo que cambia a la vez velocidad y tono (igual
// que en los sistemas de sonido clasicos). Los arrays en si se
// declaran mas arriba, junto a 'sounds[]' (sound_load ya los
// inicializa al cargar).

// Buffers de trabajo reutilizados en cada reproduccion (validos
// porque V1 es sincrono -- una reproduccion termina antes de que
// pueda empezar la siguiente, asi que no hace falta uno por sonido).
static int16_t play_scratch_a[SOUND_MAX_FRAMES * 2];
static int16_t play_scratch_b[SOUND_MAX_FRAMES * 2];

// Remuestreo lineal simple de estereo 16 bits YA cargado (a
// diferencia de wav_convert, aqui el origen ya esta en nuestro
// formato interno -- solo cambia la frecuencia "aparente").
static uint32_t resample_stereo16(const int16_t *src, uint32_t src_frames, uint32_t src_rate,
                                   int16_t *dst, uint32_t dst_max_frames) {
    if (src_rate == 0 || src_frames == 0) return 0;
    uint32_t out_frames = (uint32_t)(((uint64_t)src_frames * 44100u) / src_rate);
    if (out_frames > dst_max_frames) out_frames = dst_max_frames;
    for (uint32_t of = 0; of < out_frames; of++) {
        uint64_t src_pos_fixed = ((uint64_t)of * src_rate * 256u) / 44100u;
        uint32_t idx = (uint32_t)(src_pos_fixed / 256u);
        uint32_t frac = (uint32_t)(src_pos_fixed % 256u);
        if (idx >= src_frames) idx = src_frames - 1;
        uint32_t idx2 = (idx + 1 < src_frames) ? idx + 1 : idx;
        int32_t l = src[idx * 2 + 0] + (int32_t)(((int64_t)(src[idx2 * 2 + 0] - src[idx * 2 + 0]) * frac) / 256);
        int32_t r = src[idx * 2 + 1] + (int32_t)(((int64_t)(src[idx2 * 2 + 1] - src[idx * 2 + 1]) * frac) / 256);
        dst[of * 2 + 0] = (int16_t)l;
        dst[of * 2 + 1] = (int16_t)r;
    }
    return out_frames;
}

static int16_t clamp_s16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// ---- Sonar SIN congelar la maquina ----
//
// BUG REAL CORREGIDO. sound_play se ejecuta dentro de una llamada al
// sistema, o sea CON EL CANDADO GRANDE COGIDO, y la reproduccion bloquea
// hasta que el sonido acaba. Resultado: mientras sonaba, ningun otro nucleo
// podia entrar en el kernel -- ni componer la pantalla, ni leer el raton, ni
// atender la red. La maquina entera se quedaba parada, no solo el programa
// que llamo. Con un sonido de un segundo, un segundo de sistema congelado.
//
// Se suelta el candado SOLO alrededor de la reproduccion, que es la parte
// larga y la unica que no toca estado compartido del kernel: lee
// play_scratch_b y escribe los registros del PWM (o la cola de virtio en
// QEMU), nada mas. Los pasos de antes -- remuestreo y volumen/pan -- se
// quedan dentro del candado, porque escriben los dos buffers estaticos y
// leen la tabla de sonidos.
//
// Es el mismo patron que ya usa wm.c para componer sin retener el candado, y
// bkl_tomar/bkl_soltar se encargan solos de la mascara de interrupciones.
//
// Por que basta con esto: el nucleo 0 NO ejecuta programas cuando hay
// secundarios (ver ctx_is_ready en tasks.c), asi que quien llama a PlaySound
// siempre es un nucleo 1-3. Suelta el candado, se queda ese nucleo ocupado
// dando muestras, y el 0 recupera el candado y sigue con la pantalla y la
// entrada como si nada. Si algun dia no arrancara ningun secundario, el 0 lo
// ejecutaria todo y la pantalla si se pararia mientras suena: es el caso
// degenerado de una sola CPU, y no tiene mejor arreglo sin DMA.
//
// El guardia 'sonando': con el candado soltado, otra tarea podria entrar en
// sound_play y pisar play_scratch_a/b a mitad de la reproduccion. Se lee y se
// escribe siempre con el candado cogido, asi que sirve de candado propio del
// sonido. Un segundo PlaySound mientras suena el primero no se encola: se
// descarta (V1 no tiene polifonia; ver la nota de sound.c).
//
// Liberar el sonido a mitad NO es un problema: la reproduccion ya trabaja
// sobre la copia de play_scratch_b, no sobre sounds[handle].samples.
static bool sonando = false;   // protegido por el candado grande

static void sound_play(int32_t handle) {
    if (handle < 0 || handle >= MAX_SOUNDS || !sounds[handle].used) return;
    if (sonando) return;       // ya hay uno en curso: no se pisan los buffers

    const int16_t *src = sounds[handle].samples;
    uint32_t frames = sounds[handle].frame_count;

    // Paso 1: tono (remuestreo), solo si se fijo un valor distinto de 44100
    uint32_t pitch = sound_pitch_hz[handle];
    if (pitch != 0 && pitch != 44100) {
        frames = resample_stereo16(src, frames, pitch, play_scratch_a, SOUND_MAX_FRAMES);
        src = play_scratch_a;
    }

    // Paso 2: volumen + pan (escala cada canal por separado) -- todo
    // en enteros de punto fijo "por mil" (ver la nota junto a
    // sound_volume_permil), sin ningun float/double: el kernel no
    // puede usar coma flotante de verdad (ver la nota grande junto a
    // esas variables).
    int32_t vol = sound_volume_permil[handle];       // 0-1000
    int32_t pan = sound_pan_permil[handle];           // -1000 a 1000
    int32_t vol_l = (pan <= 0) ? vol : (vol * (1000 - pan)) / 1000;
    int32_t vol_r = (pan >= 0) ? vol : (vol * (1000 + pan)) / 1000;
    for (uint32_t f = 0; f < frames; f++) {
        play_scratch_b[f * 2 + 0] = clamp_s16(((int32_t)src[f * 2 + 0] * vol_l) / 1000);
        play_scratch_b[f * 2 + 1] = clamp_s16(((int32_t)src[f * 2 + 1] * vol_r) / 1000);
    }

    // La parte larga, fuera del candado (ver la nota de arriba).
    sonando = true;
    bkl_soltar();
    sound_play_blocking(play_scratch_b, frames);
    bkl_tomar();
    sonando = false;
}

// ---- Un dato de UNA tarea (SYS_TASK_FIELD, SYS_TASK_NAME) ----
//
// SYS_TASK_LIST vuelca la tabla entera en un buffer, 48 bytes por tarea:
// slot(4) + ventana(4) + turnos(4) + reservado(4) + nombre(32). Eso sirve a
// Lua, que sabe leer bytes de un buffer, pero no a Nemo Basic, que no tiene
// punteros ni estructuras. Estas dos syscalls parten el volcado aqui.
//
// Se vuelca ENTERO en cada llamada, a proposito: la tabla de tareas cambia
// entre una llamada y la siguiente, y un volcado guardado de la vez anterior
// daria el nombre de una tarea con los turnos de otra. Son 16 tareas como
// maximo, asi que el volcado cuesta nada.
#define TASK_ENTRADA 48
#define TASK_OFF_NOMBRE 16
static uint8_t task_volcado[MAX_TASKS * TASK_ENTRADA];

static uint32_t task_volcar(void) { return task_list_dump(task_volcado, MAX_TASKS); }

// Nucleo compartido de image_load() -- todo lo que pasa DESPUES de
// resolver el inodo del archivo, igual sea por la busqueda fija de
// siempre (raiz+DOCUMENTOS) o por una carpeta explicita (ver
// image_load_en, para SYS_LOAD_IMAGE_EN).
// ---------------------------------------------------------------
// LEER UN.nimg DE CUALQUIERA DE LOS DOS VOLUMENES
// ---------------------------------------------------------------
// El camino de lectura de .nimg tiene que atender los DOS volumenes.
// Cuando iba SOLO por NemoFS --LoadImage por image_resolve_inode, y las
// miniaturas por nemofs_read_at-- la consecuencia visible era que en la
// tarjeta no aparecian miniaturas de .nimg, ni en el explorador ni en el
// Navegante. El explorador lo tenia escrito a la cara, en su propio codigo:
//
//     if (current_volume != VOLUME_NEMOFS) return;   // LoadImage no lee de FAT
//
// (De paso: el roadmap culpaba a LoadImage, y las miniaturas no pasan
// por ahi -- van por SYS_DRAW_FILE_SCALED y SYS_MINIATURA_DOBLE, que
// leen el archivo fila a fila sin cargarlo. Hacian falta las dos cosas.)
//
// La 'fuente' es lo unico que esas funciones necesitan de un archivo:
// poder pedir 'len' bytes desde un desplazamiento. Con esto, el resto
// del codigo de escalado y encuadre no sabe en que volumen esta, y no
// hay dos versiones de las reglas de encuadre que se desincronicen.
//
// EL CURSOR DE FAT NO ES UN ADORNO: sin el, cada fila volveria a
// recorrer la cadena de clusters desde el principio y leer una imagen
// entera seria cuadratico en vez de lineal. fat_read_at lo mantiene;
// aqui solo hay que conservarlo entre filas, y por eso la fuente se
// abre UNA vez por imagen y no una por fila.
//
// Medido contando los sectores leidos al recorrer la imagen fila a
// fila, con cursor y sin el:
//
//     32x32    41 contra 72     1,8x
//     64x64    97 contra 160    1,6x
//   128x128   259 contra 526    2,0x
//   256x256   774 contra 1780   2,3x
//
// La proporcion crece con el tamaño, que es lo que se espera de cambiar
// cuadratico por lineal. En la Pi 4 cada uno de esos sectores es un
// comando a la tarjeta.
typedef struct {
    uint32_t volumen;         // VOLUME_NEMOFS o VOLUME_FAT
    int32_t inode;            // NemoFS
    fat_dirent_t entrada;     // FAT
    fat_cursor_t cursor;      // FAT: para no recorrer los clusters en cada fila
} nimg_fuente_t;

// Exige los 'len' bytes completos: una imagen a medias dejaria basura
// dentro del hueco, y eso no se ve venir mirando la pantalla.
static bool nimg_fuente_leer(nimg_fuente_t *f, uint32_t off, void *buf, uint32_t len) {
    if (f->volumen == VOLUME_FAT) {
        uint32_t leidos = 0;
        if (!fat_read_at(&f->entrada, &f->cursor, off, buf, len, &leidos)) return false;
        return leidos >= len;
    }
    return nemofs_read_at((uint32_t)f->inode, off, buf, len) >= (int32_t)len;
}

// Adelantada: abrir una fuente necesita image_resolve_inode(), que se
// define mas abajo, y cargar una imagen necesita abrir la fuente.
static bool nimg_fuente_abrir(const char *nombre, uint64_t origen, nimg_fuente_t *f);

// Carga la imagen ENTERA en un hueco, desde una fuente ya abierta (ver
// nimg_fuente_t): asi vale igual para NemoFS y para la tarjeta.
static int32_t image_load_desde_fuente(nimg_fuente_t *f) {
    int32_t slot = -1;
    for (int i = 0; i < MAX_IMAGES; i++) if (hueco_disponible(images[i].used, &duena_imagen[i])) { slot = i; break; }
    if (slot >= 0) imagen_reservar(slot);
    if (slot < 0) return -1; // sin huecos libres

    uint8_t cab[12];
    if (!nimg_fuente_leer(f, 0, cab, 12)) return -1; // ni siquiera cabe la cabecera

    if (cab[0] != 'N' || cab[1] != 'I' || cab[2] != 'M' || cab[3] != 'G') {
        return -1; // no es nuestro formato
    }
    uint32_t width = (uint32_t)cab[4] | ((uint32_t)cab[5] << 8) |
                      ((uint32_t)cab[6] << 16) | ((uint32_t)cab[7] << 24);
    uint32_t height = (uint32_t)cab[8] | ((uint32_t)cab[9] << 8) |
                       ((uint32_t)cab[10] << 16) | ((uint32_t)cab[11] << 24);

    if (width == 0 || height == 0 || width > IMAGE_MAX_DIM || height > IMAGE_MAX_DIM) return -1;
    uint32_t needed = width * height * 4;
    if (!imagen_reservar_pixeles(slot, needed)) return -1;

    // El archivo tiene que traer TODOS los pixeles que anuncia su
    // cabecera. Si se queda corto, el hueco quedaria con basura dentro.
    if (!nimg_fuente_leer(f, 12, image_pixels[slot], needed)) {
        imagen_soltar_pixeles(slot);
        return -1;
    }

    images[slot].width = width;
    images[slot].height = height;
    images[slot].handle_x = g_auto_mid_handle ? (int32_t)(width / 2) : 0;
    images[slot].handle_y = g_auto_mid_handle ? (int32_t)(height / 2) : 0;
    images[slot].used = true;
    return slot;
}

// Las imagenes se buscan en la raiz, en DOCUMENTOS y en
// DOCUMENTOS/IMAGENES -- ahi viven los iconos del sistema y es la carpeta del
// visor de imagenes. Vale para LoadImage, CreateToolBar y SetPanelImage.
static int32_t image_resolve_inode(const char *filename) {
    if (ruta_tiene_carpeta(filename)) return ruta_resolver_inode(filename);
    int32_t inode = readfile_resolve_inode(filename);          // raiz y DOCUMENTOS
    if (inode >= 0) return inode;
    int32_t docs = nemofs_find_child(NEMOFS_ROOT_INODE, "DOCUMENTOS");
    if (docs < 0) return -1;
    int32_t imgs = nemofs_find_child((uint32_t)docs, "IMAGENES");
    if (imgs < 0) return -1;
    return nemofs_find_child((uint32_t)imgs, filename);
}

// Carga desde un 'origen' como el de las syscalls: 32 bits bajos la
// carpeta, 32 altos el volumen. Un origen de 0 es la busqueda de
// siempre en NemoFS. Ver nimg_fuente_abrir().
// 'origen' es como lo pasan las syscalls: los 32 bits bajos, la carpeta
// (inodo en NemoFS, cluster desplazado en FAT, 0 = la de siempre); los
// 32 altos, el volumen. Un origen de 0 es EXACTAMENTE lo de antes:
// NemoFS con la busqueda de siempre (raiz, DOCUMENTOS, DOCUMENTOS/IMAGENES).
static bool nimg_fuente_abrir(const char *nombre, uint64_t origen, nimg_fuente_t *f) {
    uint32_t carpeta = (uint32_t)(origen & 0xFFFFFFFFu);
    f->volumen = (uint32_t)(origen >> 32);
    f->inode = -1;
    f->cursor.cluster = 0;
    f->cursor.indice = 0;
    if (f->volumen == VOLUME_FAT) {
        // Misma convencion que fat_handle_open: una carpeta llega con el
        // desplazamiento sumado, y 0 es la raiz.
        uint32_t real = (carpeta >= FAT_DIR_CLUSTER_OFFSET) ? (carpeta - FAT_DIR_CLUSTER_OFFSET) : 0;
        if (!fat_find_in_dir(real, nombre, &f->entrada)) return false;
        return !f->entrada.is_dir;
    }
    f->volumen = VOLUME_NEMOFS;
    if (carpeta != 0) {
        f->inode = nemofs_find_child(carpeta, nombre);
    } else {
        f->inode = image_resolve_inode(nombre);
    }
    return f->inode >= 0;
}


static int32_t image_load_origen(const char *filename, uint64_t origen) {
    nimg_fuente_t f;
    if (!nimg_fuente_abrir(filename, origen, &f)) return -1;
    return image_load_desde_fuente(&f);
}

static int32_t image_load(const char *filename) {
    return image_load_origen(filename, 0);
}

// Como image_load(), pero buscando el archivo dentro de una carpeta
// EXPLICITA por su inodo (parent) -- para imagenes que no viven en la
// raiz ni en DOCUMENTOS, como DOCUMENTOS/IMAGENES. Ver SYS_LOAD_IMAGE_EN.
static int32_t image_load_en(const char *filename, uint32_t parent) {
    return image_load_origen(filename, (uint64_t)parent);
}

// LoadAnimImage: carga el archivo NIMG entero (reutilizando
// image_load tal cual) y ademas guarda el tamaño de celda, para que
// DrawImage(...,frame) sepa recortar solo esa parte de la hoja de
// sprites.
// image_get_info: accesor publico (declarado en syscall.h) para que
// gadgets.c pueda leer las dimensiones y los pixeles de una imagen ya
// cargada -- lo usa CreateToolBar para recortar sus botones de la
// tira de iconos, reutilizando el MISMO almacenamiento que LoadImage
// (LoadIconStrip/CreateToolBar no son mas que "cargar una imagen" con
// nuestro formato NIMG propio, ver la nota grande de mas arriba).
bool image_get_info(int32_t handle, uint32_t *width, uint32_t *height, const uint8_t **pixels) {
    if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return false;
    if (width) *width = images[handle].width;
    if (height) *height = images[handle].height;
    if (pixels) *pixels = image_pixels[handle];
    return true;
}
// TFormImage(image,a#,b#,c#,d#) -- transforma la imagen IN PLACE
// segun la matriz 2x2 (a b; c d), centrada en el medio de la imagen
// (mismo tamaño de salida que de entrada). Usamos MAPEO INVERSO (para
// cada pixel DESTINO, calculamos de que pixel ORIGEN viene, con la
// matriz invertida) en vez de mapeo directo, para no dejar huecos sin
// escribir en el resultado -- remuestreo al vecino mas cercano (sin
// interpolacion bilineal, mas simple y suficiente aqui). Los pixeles
// que caerian fuera de los limites de la imagen origen se dejan
// transparentes.
//
// TODO EN PUNTO FIJO Q16.16 (entero escalado x65536), SIN NINGUN
// float/double: el kernel no puede usar coma flotante de verdad
// (-mgeneral-regs-only -- ver la nota grande junto a
// sound_volume_permil, un poco mas arriba). Los valores a#,b#,c#,d#
// llegan YA convertidos a Q16.16 desde el COMPILADOR (que si tiene
// coma flotante real, siendo ensamblado de usuario sin esa
// restriccion).
#define FP_SHIFT 16
#define FP_ONE (1 << FP_SHIFT)
static int32_t fp_mul(int32_t x, int32_t y) {
    return (int32_t)(((int64_t)x * (int64_t)y) >> FP_SHIFT);
}
static int32_t fp_div(int32_t x, int32_t y) {
    if (y == 0) return 0;
    return (int32_t)(((int64_t)x << FP_SHIFT) / y);
}
static void tform_image(int32_t handle, int32_t a, int32_t b, int32_t c, int32_t d) {
    if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return;
    uint32_t w = images[handle].width, h = images[handle].height;
    // Buffer temporal del heap: una imagen puede llegar a 4MB, demasiado
    // para la pila y un desperdicio como estatico.
    uint8_t *tform_tmp_buf = (uint8_t *)kmalloc(w * h * 4);
    if (!tform_tmp_buf) return;
    int32_t det = fp_mul(a, d) - fp_mul(b, c);
    if (det == 0) return; // matriz no invertible -- no se puede deshacer, no hacemos nada
    int32_t inv_a = fp_div(d, det), inv_b = fp_div(-b, det);
    int32_t inv_c = fp_div(-c, det), inv_d = fp_div(a, det);
    int32_t cx = ((int32_t)w << FP_SHIFT) / 2, cy = ((int32_t)h << FP_SHIFT) / 2;
    uint8_t *src = image_pixels[handle];

    for (uint32_t dy = 0; dy < h; dy++) {
        for (uint32_t dx = 0; dx < w; dx++) {
            int32_t rx = ((int32_t)dx << FP_SHIFT) - cx, ry = ((int32_t)dy << FP_SHIFT) - cy;
            int32_t sx = fp_mul(inv_a, rx) + fp_mul(inv_b, ry) + cx;
            int32_t sy = fp_mul(inv_c, rx) + fp_mul(inv_d, ry) + cy;
            // vecino mas cercano: redondeamos sumando media unidad
            // (en Q16.16) antes de truncar via desplazamiento,
            // tratando el signo aparte para redondear hacia el
            // vecino correcto tambien con valores negativos.
            int32_t isx = (sx >= 0) ? ((sx + FP_ONE / 2) >> FP_SHIFT) : -(((-sx) + FP_ONE / 2) >> FP_SHIFT);
            int32_t isy = (sy >= 0) ? ((sy + FP_ONE / 2) >> FP_SHIFT) : -(((-sy) + FP_ONE / 2) >> FP_SHIFT);
            uint8_t *dst_px = &tform_tmp_buf[(dy * w + dx) * 4];
            if (isx < 0 || isy < 0 || (uint32_t)isx >= w || (uint32_t)isy >= h) {
                dst_px[0] = 0; dst_px[1] = 0; dst_px[2] = 0; dst_px[3] = 0; // transparente
            } else {
                const uint8_t *src_px = &src[((uint32_t)isy * w + (uint32_t)isx) * 4];
                dst_px[0] = src_px[0]; dst_px[1] = src_px[1]; dst_px[2] = src_px[2]; dst_px[3] = src_px[3];
            }
        }
    }
    uint32_t total = w * h * 4;
    for (uint32_t i = 0; i < total; i++) src[i] = tform_tmp_buf[i];
    kfree(tform_tmp_buf);
}
// LoadAnimImage: carga el archivo NIMG entero (reutilizando
// image_load tal cual) y ademas guarda el tamaño de celda y el rango
// de fotogramas validos, para que DrawImage(...,frame) sepa recortar
// solo esa parte de la hoja de sprites. 'first' es la celda de la
// hoja (contando por filas) que corresponde al fotograma 0 de la
// animacion; 'count' cuantos fotogramas validos hay a partir de ahi
// (si es 0, se trata como "todos los que quepan", igual que antes).
static int32_t image_load_anim(const char *filename, uint32_t cell_w, uint32_t cell_h, uint32_t first, uint32_t count) {
    int32_t handle = image_load(filename);
    if (handle < 0) return -1;
    images[handle].cell_width = cell_w;
    images[handle].cell_height = cell_h;
    images[handle].anim_first = first;
    if (count == 0 && cell_w > 0 && cell_h > 0) {
        uint32_t cols = images[handle].width / cell_w;
        uint32_t rows = images[handle].height / cell_h;
        uint32_t total = cols * rows;
        count = total > first ? total - first : 1;
    }
    images[handle].anim_count = count;
    return handle;
}

// -- LoadFont/SetFont/FreeFont y companeros --
//
// LIMITACION REAL: no tenemos un renderizador de fuentes TrueType
// (eso es un proyecto aparte, comparable o mayor que el decodificador
// de imagenes que ya decidimos NO construir), asi que la FORMA de las
// letras siempre es la de nuestro bitmap fijo 5x7 (font5x7.h/.c) --
// eso no cambia. PERO el TAMAÑO y la NEGRITA de Text (no de la UI del
// propio SO) SI son reales: wm_content_draw_string ya admitia un
// factor de escala entero, asi que SetFont calcula una escala a
// partir del alto pedido (redondeada al entero mas cercano), y la
// negrita se consigue dibujando el texto dos veces con 1 pixel de
// desplazamiento -- sin necesitar autoria de glifos nuevos. La
// CURSIVA si queda sin implementar (inclinar un bitmap de verdad
// pediria deformar cada fila, mas trabajo del que compensa aqui).
//
// FontName$/FontSize/FontStyle devuelven lo que el programa PIDIO al
// cargar la fuente (metadatos). FontWidth()/FontHeight() devuelven
// las dimensiones REALES en pantalla de la fuente ACTIVA (5*escala,
// 7*escala) -- ya coinciden con lo que de verdad se ve.
// Huecos de fuente para TODO el sistema. Eran 16, y cada visor de Lua se
// reserva hasta 14: con varios documentos abiertos a la vez la tabla se
// llenaba y los visores caian a la letra del sistema. Cada hueco son unos
// 50 bytes (las fuentes en si viven en el paquete compartido, fonts.c),
// asi que 64 cuestan unos 3 KB.
#define MAX_FONTS 64
typedef struct {
    bool used;
    char name[32];
    int32_t height;
    bool bold, italic, underlined;
    const nfnt_cara_t *cara;   // fuente proporcional (fonts.c), o NULL = la 5x7 "sistema" escalada
} font_slot_t;
static font_slot_t fonts[MAX_FONTS];
static duena_t duena_fuente[MAX_FONTS];   // dueño de cada hueco, ver hueco_disponible

// Aunque no rasterizamos TrueType de verdad, SI podemos dar un tamaño
// y negrita REALES: wm_content_draw_string ya admite un factor de
// escala entero (cada pixel de la fuente 5x7 se dibuja como un
// bloque escala x escala), y la negrita se consigue dibujando el
// texto DOS VECES con un desplazamiento de 1 pixel -- sin necesitar
// autoria manual de glifos nuevos. Solo afecta al comando Text (lo
// que dibuja el PROGRAMA), no a los botones/menus/etc del propio
// SO, que siguen usando la fuente de sistema a escala 1 siempre.

static bool nombre_es_sistema(const char *n) {
    // "sistema", "system", "fixed", "5x7", o vacio -> la fuente de siempre
    if (!n || !n[0]) return true;
    const char *opciones[] = { "sistema", "system", "fixed", "5x7" };
    for (int k = 0; k < 4; k++) {
        const char *o = opciones[k]; int i = 0;
        while (o[i] && n[i]) {
            char a = n[i], b = o[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (!o[i] && !n[i]) return true;
    }
    return false;
}

// Callbacks de pixel para fonts_dibujar sobre el bufer de una ventana,
// con recorte al area de cliente visible.
typedef struct { int32_t win; int32_t cx0, cy0, cx1, cy1; } dibujo_ctx_t;
static uint32_t ctx_get_pixel(void *c, int32_t x, int32_t y) {
    dibujo_ctx_t *d = (dibujo_ctx_t *)c;
    return wm_content_get_pixel(d->win, (uint32_t)x, (uint32_t)y);
}
static void ctx_put_pixel(void *c, int32_t x, int32_t y, uint32_t color) {
    dibujo_ctx_t *d = (dibujo_ctx_t *)c;
    if (x < d->cx0 || y < d->cy0 || x >= d->cx1 || y >= d->cy1) return;
    wm_content_put_pixel(d->win, (uint32_t)x, (uint32_t)y, color);
}

static int32_t font_load(const char *name, int32_t height, bool bold, bool italic, bool underlined) {
    for (int i = 0; i < MAX_FONTS; i++) {
        if (hueco_disponible(fonts[i].used, &duena_fuente[i])) {
            duena_fuente[i] = duena_actual();
            fonts[i].used = true;
            int j = 0;
            while (name[j] != '\0' && j < 31) { fonts[i].name[j] = name[j]; j++; }
            fonts[i].name[j] = '\0';
            fonts[i].height = height;
            fonts[i].bold = bold;
            fonts[i].italic = italic;
            fonts[i].underlined = underlined;
            // Proporcional si se pide una familia y hay paquete cargado;
            // si no (o si se pide "sistema"), la 5x7 escalada de siempre.
            fonts[i].cara = nombre_es_sistema(name) ? 0 : fonts_find(name, (uint32_t)(height > 0 ? height : 12), bold, italic);
            return i + 1;
        }
    }
    return 0; // sin huecos libres -- BlitzPlus real devuelve 0 tambien si LoadFont falla
}
static void font_free(int32_t handle) {
    int32_t idx = handle - 1;
    if (idx < 0 || idx >= MAX_FONTS) return;
    fonts[idx].used = false;
    if (g_current_font == idx) {
        g_current_font = -1;
        g_font_scale = 1;
        g_font_bold = false;
        g_cara = 0;
    }
}

// -- SetGamma/UpdateGamma/GammaRed/GammaGreen/GammaBlue --
//
// LIMITACION REAL: BlitzPlus documenta que "Gamma can ONLY be used in
// fullscreen mode" -- nuestro sistema es exclusivamente en ventana,
// asi que ni siquiera en BlitzPlus real aplicaria aqui de verdad. Y
// aunque quisieramos, no tenemos acceso a tablas de gamma de hardware
// (QEMU con framebuffer simple, sin ese control). Aun asi, SI
// implementamos una tabla de consulta REAL (no un no-op ciego): SI
// guarda lo que se le pida, para que GammaRed/Green/Blue puedan
// devolver un valor coherente con lo que el programa configuro,
// aunque no se vea reflejado visualmente en pantalla.
static uint8_t g_gamma_r[256], g_gamma_g[256], g_gamma_b[256];
static bool g_gamma_init = false;
static void gamma_ensure_init(void) {
    if (g_gamma_init) return;
    for (int i = 0; i < 256; i++) { g_gamma_r[i] = (uint8_t)i; g_gamma_g[i] = (uint8_t)i; g_gamma_b[i] = (uint8_t)i; }
    g_gamma_init = true;
}

// CreateImage(ancho,alto) -- lienzo VACIO (transparente del todo, ya
// que la memoria del pool empieza a cero), sin cargar nada de disco.
static int32_t image_create(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > IMAGE_MAX_DIM || height > IMAGE_MAX_DIM) return -1;
    int32_t slot = -1;
    for (int i = 0; i < MAX_IMAGES; i++) if (hueco_disponible(images[i].used, &duena_imagen[i])) { slot = i; break; }
    if (slot >= 0) imagen_reservar(slot);
    if (slot < 0) return -1;

    uint32_t needed = width * height * 4;
    if (!imagen_reservar_pixeles(slot, needed)) return -1;
    for (uint32_t i = 0; i < needed; i++) image_pixels[slot][i] = 0;

    images[slot].width = width;
    images[slot].height = height;
    images[slot].handle_x = g_auto_mid_handle ? (int32_t)(width / 2) : 0;
    images[slot].handle_y = g_auto_mid_handle ? (int32_t)(height / 2) : 0;
    images[slot].used = true;
    return slot;
}

static void image_free(int32_t handle) {
    if (handle < 0 || handle >= MAX_IMAGES) return;
    // Si era el destino de dibujo activo, soltarlo: si no, el siguiente
    // dibujo iria a un hueco liberado -- o, peor, al que lo reutilice.
    if (g_draw_target_image == handle) g_draw_target_image = -1;
    images[handle].used = false;
    imagen_soltar_pixeles(handle);
}

static void image_set_handle(int32_t handle, int32_t x, int32_t y) {
    if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return;
    images[handle].handle_x = x;
    images[handle].handle_y = y;
}

// MaskImage: cualquier pixel que coincida EXACTAMENTE con el color
// dado pasa a tener alfa=0 (transparente) -- el truco clasico de
// "color clave" de los sprites sin canal alfa de verdad.
// MaskImage NO-DESTRUCTIVO: guarda el color como metadato del hueco,
// se comprueba en CADA dibujado (blit_with_mask_check mas abajo) --
// nunca se toca la imagen en si. Igual que BlitzPlus real
// (gxCanvas::setMask), a diferencia de una version que borrara el
// canal alfa de los pixeles que coincidan de una vez.
static void image_mask(int32_t handle, uint32_t rgb) {
    if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return;
    images[handle].has_mask = true;
    images[handle].mask_color = rgb & 0xFFFFFF;
}

// Reescala una imagen EN SU MISMO HUECO (no crea una nueva), por
// vecino mas cercano -- ni ResizeImage ni ScaleImage necesitan mas
// precision que eso para uso tipico de juego.
static bool image_resize(int32_t handle, uint32_t new_w, uint32_t new_h) {
    if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return false;
    if (new_w == 0 || new_h == 0 || new_w > IMAGE_MAX_DIM || new_h > IMAGE_MAX_DIM) return false;
    uint32_t old_w = images[handle].width, old_h = images[handle].height;
    uint32_t needed_new = new_w * new_h * 4;
    if (needed_new > IMAGE_BYTES_MAX) return false;
    // Se escribe en un buffer NUEVO y solo al final se suelta el viejo:
    // el tamaño puede crecer, asi que no se puede reescribir en el sitio.
    uint8_t *resize_temp = (uint8_t *)kmalloc(needed_new);
    if (!resize_temp) return false;
    for (uint32_t y = 0; y < new_h; y++) {
        uint32_t sy = (y * old_h) / new_h;
        for (uint32_t x = 0; x < new_w; x++) {
            uint32_t sx = (x * old_w) / new_w;
            uint32_t src_idx = (sy * old_w + sx) * 4;
            uint32_t dst_idx = (y * new_w + x) * 4;
            resize_temp[dst_idx + 0] = image_pixels[handle][src_idx + 0];
            resize_temp[dst_idx + 1] = image_pixels[handle][src_idx + 1];
            resize_temp[dst_idx + 2] = image_pixels[handle][src_idx + 2];
            resize_temp[dst_idx + 3] = image_pixels[handle][src_idx + 3];
        }
    }
    if (!imagen_reservar_pixeles(handle, needed_new)) { kfree(resize_temp); return false; }
    for (uint32_t i = 0; i < needed_new; i++) image_pixels[handle][i] = resize_temp[i];
    kfree(resize_temp);
    images[handle].width = new_w;
    images[handle].height = new_h;
    return true;
}

// Seno/coseno propios, en PUNTO FIJO Q16.16 (para uso interno del
// kernel, en RotateImage -- distintos de rt_sin/rt_cos en ensamblado
// que usa el compilador, esos SI tienen coma flotante real porque son
// codigo de usuario). El kernel no puede usar coma flotante de verdad
// (-mgeneral-regs-only -- ver la nota grande junto a
// sound_volume_permil).
//
// BUG REAL ENCONTRADO Y CORREGIDO: la primera version reducia solo a
// un SEMIPERIODO [-180,180) antes de aplicar una serie de Taylor de 4
// terminos -- verificado NUMERICAMENTE (comparando contra math.sin/
// cos reales en Python, replicando exactamente esta misma aritmetica
// entera) que el error crecia hasta 0.21 cerca de los bordes del
// rango (angulos cercanos a ±180°), demasiado impreciso para
// resultados visualmente correctos. La serie de Taylor converge mucho
// peor para argumentos grandes (cercanos a π) que para argumentos
// pequeños -- la solucion estandar es reducir al CUADRANTE (rango
// [0°,90°]) usando las identidades trigonometricas de reflexion, no
// solo al semiperiodo. Reduciendo asi, el error maximo verificado
// baja a ~0.001 -- de sobra para redondear al pixel mas cercano.
#define FP_DEG_TO_RAD 1144 // pi/180 en Q16.16 (0.017453292519943295 * 65536, redondeado)
#define FP_90  5898240     // 90.0 en Q16.16
#define FP_180 11796480    // 180.0 en Q16.16
#define FP_270 17694720    // 270.0 en Q16.16
#define FP_360 23592960    // 360.0 en Q16.16

// Reduce un angulo (Q16.16, cualquier valor) al rango [0,360), y
// devuelve el "angulo de referencia" en [0,90] junto con el cuadrante
// (0-3) en el que caia el angulo original -- para que el llamador
// aplique el signo/identidad correcta segun sin/cos y el cuadrante.
static int32_t fp_reduce_to_quadrant(int32_t deg, int *out_quadrant) {
    int32_t n = deg / FP_360;
    int32_t norm = deg - n * FP_360;
    if (norm < 0) norm += FP_360;
    if (norm < FP_90) { *out_quadrant = 0; return norm; }
    if (norm < FP_180) { *out_quadrant = 1; return FP_180 - norm; }
    if (norm < FP_270) { *out_quadrant = 2; return norm - FP_180; }
    *out_quadrant = 3; return FP_360 - norm;
}

// Serie de Taylor de 4 terminos, evaluada SOLO para rad en [0, pi/2]
// aprox (tras la reduccion de cuadrante) -- con este rango mas
// pequeño, 4 terminos ya dan precision de sobra.
static int32_t fp_taylor_sin_rad(int32_t rad) {
    int32_t x2 = fp_mul(rad, rad);
    int32_t acc = -13;                // -1/5040
    acc = 546 + fp_mul(x2, acc);      // 1/120
    acc = -10923 + fp_mul(x2, acc);   // -1/6
    acc = FP_ONE + fp_mul(x2, acc);   // 1.0
    return fp_mul(rad, acc);
}
static int32_t fp_taylor_cos_rad(int32_t rad) {
    int32_t x2 = fp_mul(rad, rad);
    int32_t acc = -91;                // -1/720
    acc = 2731 + fp_mul(x2, acc);     // 1/24
    acc = -32768 + fp_mul(x2, acc);   // -1/2
    acc = FP_ONE + fp_mul(x2, acc);   // 1.0
    return acc;
}

static int32_t fp_sin(int32_t deg) {
    int quadrant;
    int32_t ref = fp_reduce_to_quadrant(deg, &quadrant);
    int32_t rad = fp_mul(ref, FP_DEG_TO_RAD);
    int32_t s = fp_taylor_sin_rad(rad);
    return (quadrant == 2 || quadrant == 3) ? -s : s;
}

static int32_t fp_cos(int32_t deg) {
    int quadrant;
    int32_t ref = fp_reduce_to_quadrant(deg, &quadrant);
    int32_t rad = fp_mul(ref, FP_DEG_TO_RAD);
    int32_t c = fp_taylor_cos_rad(rad);
    return (quadrant == 1 || quadrant == 2) ? -c : c;
}

// floor/ceil de un valor Q16.16, devolviendo un ENTERO normal (no
// Q16.16) -- el desplazamiento aritmetico a la derecha de un entero
// CON SIGNO en complemento a 2 YA redondea hacia menos infinito por
// definicion, asi que floor() no necesita ningun caso especial (a
// diferencia de la version en punto flotante, que si comprobaba
// explicitamente si habia parte fraccionaria).
static int32_t fp_floor_to_int(int32_t v) {
    return v >> FP_SHIFT;
}
static int32_t fp_ceil_to_int(int32_t v) {
    return -((-v) >> FP_SHIFT);
}

// Rota una imagen EN SU MISMO HUECO, pero EXPANDIENDO el ancho/alto
// para que quepa toda la imagen rotada sin recortar esquinas (igual
// que BlitzPlus real: calcula el rectangulo delimitador de las
// cuatro esquinas rotadas). Si el resultado excede IMAGE_MAX_DIM, se
// recorta a ese limite -- una restriccion real de nuestro pool de
// huecos de tamaño fijo que el original (memoria dinamica) no tiene.
// Mapeo INVERSO para el relleno: para cada pixel DESTINO calculamos
// que pixel ORIGEN le corresponde, rotando hacia atras.
//
// TODO EN PUNTO FIJO Q16.16, SIN NINGUN float/double (ver la nota
// grande junto a fp_sin/fp_cos, un poco mas arriba). 'angle_deg'
// llega YA convertido a Q16.16 desde el COMPILADOR. LIMITACION DE
// BORDE: angulos mayores de ±32767 grados podrian desbordar (Q16.16
// solo tiene 16 bits de parte entera) -- irrelevante en la practica
// (nadie gira una imagen 32767 grados), pero real frente a la version
// en punto flotante anterior, que no tenia este limite.
static bool image_rotate(int32_t handle, int32_t angle_deg) {
    if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return false;
    uint32_t w = images[handle].width, h = images[handle].height;
    int32_t w_fp = (int32_t)w << FP_SHIFT, h_fp = (int32_t)h << FP_SHIFT;
    int32_t cx = w_fp / 2, cy = h_fp / 2;

    // Rotamos las cuatro esquinas HACIA ADELANTE para saber cuanto
    // espacio hace falta.
    int32_t fs = fp_sin(angle_deg), fc = fp_cos(angle_deg);
    int32_t corners_x[4] = { -cx, w_fp - cx, w_fp - cx, -cx };
    int32_t corners_y[4] = { -cy, -cy, h_fp - cy, h_fp - cy };
    int32_t minx = fp_mul(corners_x[0], fc) - fp_mul(corners_y[0], fs);
    int32_t maxx = minx, miny = fp_mul(corners_x[0], fs) + fp_mul(corners_y[0], fc), maxy = miny;
    for (int k = 1; k < 4; k++) {
        int32_t rx = fp_mul(corners_x[k], fc) - fp_mul(corners_y[k], fs);
        int32_t ry = fp_mul(corners_x[k], fs) + fp_mul(corners_y[k], fc);
        if (rx < minx) minx = rx;
        if (rx > maxx) maxx = rx;
        if (ry < miny) miny = ry;
        if (ry > maxy) maxy = ry;
    }
    int32_t ominx = fp_floor_to_int(minx), omaxx = fp_ceil_to_int(maxx);
    int32_t ominy = fp_floor_to_int(miny), omaxy = fp_ceil_to_int(maxy);
    uint32_t new_w = (uint32_t)(omaxx - ominx);
    uint32_t new_h = (uint32_t)(omaxy - ominy);
    if (new_w == 0) new_w = 1;
    if (new_h == 0) new_h = 1;
    if (new_w > IMAGE_MAX_DIM) new_w = IMAGE_MAX_DIM;
    if (new_h > IMAGE_MAX_DIM) new_h = IMAGE_MAX_DIM;

    // Mapeo inverso para rellenar: cada destino busca su origen
    // rotando hacia atras (-angle_deg).
    int32_t is_ = fp_sin(-angle_deg), ic = fp_cos(-angle_deg);
    int32_t new_cx = ((int32_t)new_w << FP_SHIFT) / 2, new_cy = ((int32_t)new_h << FP_SHIFT) / 2;

    uint32_t needed_new = new_w * new_h * 4;
    if (needed_new > IMAGE_BYTES_MAX) return false;
    uint8_t *rotate_temp = (uint8_t *)kmalloc(needed_new);
    if (!rotate_temp) return false;
    for (uint32_t i = 0; i < needed_new; i++) rotate_temp[i] = 0; // transparente por defecto

    for (uint32_t dy = 0; dy < new_h; dy++) {
        for (uint32_t dx = 0; dx < new_w; dx++) {
            int32_t rx = ((int32_t)dx << FP_SHIFT) - new_cx;
            int32_t ry = ((int32_t)dy << FP_SHIFT) - new_cy;
            int32_t sx = fp_mul(rx, ic) - fp_mul(ry, is_) + cx;
            int32_t sy = fp_mul(rx, is_) + fp_mul(ry, ic) + cy;
            int32_t isx = (sx >= 0) ? ((sx + FP_ONE / 2) >> FP_SHIFT) : -(((-sx) + FP_ONE / 2) >> FP_SHIFT);
            int32_t isy = (sy >= 0) ? ((sy + FP_ONE / 2) >> FP_SHIFT) : -(((-sy) + FP_ONE / 2) >> FP_SHIFT);
            if (isx >= 0 && isx < (int32_t)w && isy >= 0 && isy < (int32_t)h) {
                uint32_t src_idx = ((uint32_t)isy * w + (uint32_t)isx) * 4;
                uint32_t dst_idx = (dy * new_w + dx) * 4;
                rotate_temp[dst_idx + 0] = image_pixels[handle][src_idx + 0];
                rotate_temp[dst_idx + 1] = image_pixels[handle][src_idx + 1];
                rotate_temp[dst_idx + 2] = image_pixels[handle][src_idx + 2];
                rotate_temp[dst_idx + 3] = image_pixels[handle][src_idx + 3];
            }
        }
    }
    if (!imagen_reservar_pixeles(handle, needed_new)) { kfree(rotate_temp); return false; }
    for (uint32_t i = 0; i < needed_new; i++) image_pixels[handle][i] = rotate_temp[i];
    kfree(rotate_temp);

    // El punto de agarre se recentra en el nuevo tamaño -- una
    // simplificacion razonable frente a la formula exacta de
    // BlitzPlus real (que reposiciona segun el punto de agarre
    // ANTERIOR), pero cubre bien el caso tipico (girar centrado).
    images[handle].handle_x = (int32_t)(new_w / 2);
    images[handle].handle_y = (int32_t)(new_h / 2);
    images[handle].width = new_w;
    images[handle].height = new_h;
    return true;
}

static int32_t image_copy(int32_t handle) {
    if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return -1;
    int32_t slot = -1;
    for (int i = 0; i < MAX_IMAGES; i++) if (hueco_disponible(images[i].used, &duena_imagen[i])) { slot = i; break; }
    if (slot >= 0) imagen_reservar(slot);
    if (slot < 0) return -1;
    uint32_t needed = images[handle].width * images[handle].height * 4;
    if (!imagen_reservar_pixeles(slot, needed)) return -1;
    for (uint32_t i = 0; i < needed; i++) image_pixels[slot][i] = image_pixels[handle][i];
    images[slot].width = images[handle].width;
    images[slot].height = images[handle].height;
    images[slot].handle_x = images[handle].handle_x;
    images[slot].handle_y = images[handle].handle_y;
    images[slot].used = true;
    return slot;
}

// Guarda una imagen en NemoFS raiz, en nuestro propio formato NIMG --
// el mismo que ya lee LoadImage.
static bool image_save(int32_t handle, const char *filename) {
    if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return false;
    uint32_t bytes_totales = 12 + images[handle].width * images[handle].height * 4;
    uint8_t *save_buf = (uint8_t *)kmalloc(bytes_totales);
    if (!save_buf) return false;
    save_buf[0] = 'N'; save_buf[1] = 'I'; save_buf[2] = 'M'; save_buf[3] = 'G';
    uint32_t w = images[handle].width, h = images[handle].height;
    save_buf[4] = (uint8_t)w; save_buf[5] = (uint8_t)(w >> 8); save_buf[6] = (uint8_t)(w >> 16); save_buf[7] = (uint8_t)(w >> 24);
    save_buf[8] = (uint8_t)h; save_buf[9] = (uint8_t)(h >> 8); save_buf[10] = (uint8_t)(h >> 16); save_buf[11] = (uint8_t)(h >> 24);
    uint32_t needed = w * h * 4;
    for (uint32_t i = 0; i < needed; i++) save_buf[12 + i] = image_pixels[handle][i];

    // Guardar EN UNA CARPETA. Hasta ahora SaveImage escribia
    // siempre en la raiz, asi que editar un icono de DOCUMENTOS/IMAGENES y
    // guardarlo dejaba el original intacto y una copia suelta en la raiz:
    // parecia que habia guardado, y no donde tocaba.
    //
    // ruta_resolver_padre devuelve la carpeta y el nombre final, y ya
    // rechaza ".." y los tramos que no son carpetas.
    uint32_t padre = NEMOFS_ROOT_INODE;
    char hoja[NEMOFS_MAX_NAME + 1];
    int32_t por_ruta = ruta_resolver_padre(filename, hoja, sizeof(hoja));
    if (por_ruta >= 0) {
        padre = (uint32_t)por_ruta;
        filename = hoja;
    } else if (por_ruta != RUTA_NO_ES_RUTA) {
        kfree(save_buf);
        return false;                       // la ruta lleva carpetas que no existen
    }

    int32_t idx = nemofs_find_child(padre, filename);
    if (idx < 0) idx = nemofs_create(padre, filename, NEMOFS_TYPE_FILE);
    if (idx < 0) { kfree(save_buf); return false; }
    bool bien = nemofs_write_file((uint32_t)idx, save_buf, 12 + needed);
    kfree(save_buf);
    return bien;
}

// -- Bancos de memoria (CreateBank/PeekX/PokeX/ResizeBank/CopyBank) --
//
// Buffers de bytes con tamaño variable (hasta BANK_MAX_SIZE), el
// equivalente a memoria reservada a mano en BlitzPlus real. Como no
// tenemos heap dinamico, usamos el mismo patron de pool de huecos
// fijos que ya usamos para ventanas/gadgets/imagenes.
#define MAX_BANKS 8
#define BANK_MAX_SIZE 65536
typedef struct {
    bool used;
    uint32_t size; // tamaño ACTUAL (puede ser menor que BANK_MAX_SIZE)
} bank_slot_t;
static bank_slot_t banks[MAX_BANKS];
static duena_t duena_banco[MAX_BANKS];   // dueño de cada hueco, ver hueco_disponible
__attribute__((aligned(16)))
static uint8_t bank_data[MAX_BANKS][BANK_MAX_SIZE];

static int32_t bank_create(uint32_t size) {
    if (size > BANK_MAX_SIZE) return -1;
    int32_t slot = -1;
    for (int i = 0; i < MAX_BANKS; i++) if (hueco_disponible(banks[i].used, &duena_banco[i])) { slot = i; break; }
    if (slot >= 0) { banks[slot].used = false; duena_banco[slot] = duena_actual(); }
    if (slot < 0) return -1;
    for (uint32_t i = 0; i < size; i++) bank_data[slot][i] = 0;
    banks[slot].used = true;
    banks[slot].size = size;
    return slot;
}

static void bank_free(int32_t handle) {
    if (handle < 0 || handle >= MAX_BANKS) return;
    banks[handle].used = false;
    banks[handle].size = 0;
}

static uint32_t bank_size(int32_t handle) {
    if (handle < 0 || handle >= MAX_BANKS || !banks[handle].used) return 0;
    return banks[handle].size;
}

static bool bank_resize(int32_t handle, uint32_t new_size) {
    if (handle < 0 || handle >= MAX_BANKS || !banks[handle].used) return false;
    if (new_size > BANK_MAX_SIZE) return false;
    uint32_t old_size = banks[handle].size;
    if (new_size > old_size) {
        for (uint32_t i = old_size; i < new_size; i++) bank_data[handle][i] = 0;
    }
    banks[handle].size = new_size;
    return true;
}

// Copia bytes entre bancos (o dentro del mismo banco) -- si el origen
// y destino son el MISMO banco y los rangos se solapan con el
// destino por delante del origen, hace falta copiar de atras hacia
// adelante (como memmove), o se pisaria a si mismo a medio copiar.
static bool bank_copy(int32_t src, uint32_t src_off, int32_t dst, uint32_t dst_off, uint32_t count) {
    if (src < 0 || src >= MAX_BANKS || !banks[src].used) return false;
    if (dst < 0 || dst >= MAX_BANKS || !banks[dst].used) return false;
    if ((uint64_t)src_off + count > banks[src].size) return false;
    if ((uint64_t)dst_off + count > banks[dst].size) return false;
    if (src == dst && dst_off > src_off) {
        for (int32_t i = (int32_t)count - 1; i >= 0; i--) {
            bank_data[dst][dst_off + (uint32_t)i] = bank_data[src][src_off + (uint32_t)i];
        }
    } else {
        for (uint32_t i = 0; i < count; i++) {
            bank_data[dst][dst_off + i] = bank_data[src][src_off + i];
        }
    }
    return true;
}

// -- LockBuffer/UnlockBuffer/LockedPixels/LockedPitch/LockedFormat --
//
// LockedPixels() debe devolver un "banco" que representa los pixeles
// del buffer bloqueado. En vez de COPIAR todo el buffer a un banco
// normal (los bancos son de como mucho 64KB, muy poco para una
// ventana de 1400x900x4 = 5MB), usamos un handle CENTINELA especial
// que PeekByte/PokeByte reconocen y redirigen DIRECTAMENTE a la
// memoria real del buffer (ventana o imagen), sin copia intermedia --
// mas fiel al espiritu de "acceso directo" del comando real, y sin
// gastar memoria de mas.
//
// Las funciones locked_buffer_peek/poke_one viven MAS ABAJO en este
// archivo (necesitan wm_content_get_pixel/fill_rect e image_pixels[],
// que se declaran/definen despues de los includes de wm.h etc.) --
// de ahi las declaraciones adelantadas aqui.
#define LOCKED_BUFFER_SENTINEL (-100)
static uint32_t locked_buffer_peek(uint32_t offset);
static void locked_buffer_poke_one(uint32_t offset, uint8_t byte_value);

// Lectura/escritura generica de 1/2/4 bytes, en little-endian --
// Peek*/Poke* de 1, 2 y 4 bytes son todos variaciones de esto mismo.
static uint32_t bank_peek_bytes(int32_t handle, uint32_t offset, uint32_t nbytes) {
    if (handle == LOCKED_BUFFER_SENTINEL) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < nbytes; i++) v |= locked_buffer_peek(offset + i) << (8 * i);
        return v;
    }
    if (handle < 0 || handle >= MAX_BANKS || !banks[handle].used) return 0;
    if ((uint64_t)offset + nbytes > banks[handle].size) return 0;
    uint32_t v = 0;
    for (uint32_t i = 0; i < nbytes; i++) v |= ((uint32_t)bank_data[handle][offset + i]) << (8 * i);
    return v;
}

static void bank_poke_bytes(int32_t handle, uint32_t offset, uint32_t nbytes, uint32_t value) {
    if (handle == LOCKED_BUFFER_SENTINEL) {
        for (uint32_t i = 0; i < nbytes; i++) locked_buffer_poke_one(offset + i, (uint8_t)(value >> (8 * i)));
        return;
    }
    if (handle < 0 || handle >= MAX_BANKS || !banks[handle].used) return;
    if ((uint64_t)offset + nbytes > banks[handle].size) return;
    for (uint32_t i = 0; i < nbytes; i++) bank_data[handle][offset + i] = (uint8_t)(value >> (8 * i));
}

#include "input.h"
#include "wm.h"
#include "font5x7.h" // FONT_WIDTH/FONT_HEIGHT, para FontWidth()/FontHeight()
#include "ramfb.h"
#include "text.h"
#include "icons_data.h"
#include "timer.h"
#include "tasks.h"
#include "dialog.h"
#include "gadgets.h"
#include "rtc.h"
#include "memoria.h"   // RAM fisica detectada, para SYS_RAM_FISICA
#include "smp.h"       // nucleos en marcha, para SYS_CPU_DATOS
#ifndef NEMO_QEMU
#include "pi4/mailbox_pi4.h"   // la frecuencia del procesador, solo en la Pi
#endif
#include "tasks.h"

// Captura una region de la pantalla (el buffer de contenido de la
// ventana actual) y la convierte en una imagen nueva -- lo contrario
// de DrawImage. El canal alfa se pone siempre a opaco, ya que lo
// capturado del framebuffer no lleva transparencia propia. Va aqui
// (y no junto al resto de funciones de imagen) porque necesita
// wm_content_get_pixel, declarada en wm.h, que se incluye justo
// arriba -- antes de este punto del archivo wm.h todavia no esta
// disponible.
static int32_t image_grab(int32_t win, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || w > IMAGE_MAX_DIM || h > IMAGE_MAX_DIM) return -1;
    int32_t slot = -1;
    for (int i = 0; i < MAX_IMAGES; i++) if (hueco_disponible(images[i].used, &duena_imagen[i])) { slot = i; break; }
    if (slot >= 0) imagen_reservar(slot);
    if (slot < 0) return -1;
    if (!imagen_reservar_pixeles(slot, w * h * 4)) return -1;
    for (uint32_t j = 0; j < h; j++) {
        for (uint32_t i = 0; i < w; i++) {
            uint32_t color = wm_content_get_pixel(win, x + i, y + j);
            uint32_t idx = (j * w + i) * 4;
            image_pixels[slot][idx + 0] = (uint8_t)(color >> 16);
            image_pixels[slot][idx + 1] = (uint8_t)(color >> 8);
            image_pixels[slot][idx + 2] = (uint8_t)(color);
            image_pixels[slot][idx + 3] = 255;
        }
    }
    images[slot].width = w;
    images[slot].height = h;
    images[slot].handle_x = 0;
    images[slot].handle_y = 0;
    images[slot].used = true;
    return slot;
}

// CopyRect entre CUALQUIER combinacion de ventana/imagen (BlitzPlus
// real admite buffers origen/destino opcionales) -- src_img/dst_img
// = -1 significa "la ventana actual". Para ventana->ventana
// reutilizamos wm_content_copy_rect (que ya maneja solapamiento como
// memmove); para el resto, pixel a pixel -- mas lento, pero no hay
// solapamiento posible entre buffers distintos.
static bool copy_rect_generic(int32_t win, int32_t src_img, uint32_t sx, uint32_t sy,
                               int32_t dst_img, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
    if (src_img < 0 && dst_img < 0) {
        wm_content_copy_rect(win, sx, sy, w, h, dx, dy);
        return true;
    }
    if (src_img >= 0 && (src_img >= MAX_IMAGES || !images[src_img].used)) return false;
    if (dst_img >= 0 && (dst_img >= MAX_IMAGES || !images[dst_img].used)) return false;

    for (uint32_t j = 0; j < h; j++) {
        for (uint32_t i = 0; i < w; i++) {
            uint32_t color;
            if (src_img >= 0) {
                if (sx + i >= images[src_img].width || sy + j >= images[src_img].height) continue;
                uint32_t idx = ((sy + j) * images[src_img].width + (sx + i)) * 4;
                color = ((uint32_t)image_pixels[src_img][idx] << 16) |
                        ((uint32_t)image_pixels[src_img][idx + 1] << 8) | image_pixels[src_img][idx + 2];
            } else {
                color = wm_content_get_pixel(win, sx + i, sy + j);
            }

            if (dst_img >= 0) {
                if (dx + i >= images[dst_img].width || dy + j >= images[dst_img].height) continue;
                uint32_t idx = ((dy + j) * images[dst_img].width + (dx + i)) * 4;
                image_pixels[dst_img][idx + 0] = (uint8_t)(color >> 16);
                image_pixels[dst_img][idx + 1] = (uint8_t)(color >> 8);
                image_pixels[dst_img][idx + 2] = (uint8_t)color;
                image_pixels[dst_img][idx + 3] = 255;
            } else {
                wm_content_fill_rect(win, dx + i, dy + j, 1, 1, color);
            }
        }
    }
    return true;
}

// -- Dibujo directo sobre una imagen, para ImageBuffer() --
//
// Version simplificada (sin barra de menu, sin buscar ventana) de lo
// que wm.c ya hace para ventanas -- escriben directamente en
// image_pixels[] en vez de en window_content[]. Los colores de dibujo
// (Color r,g,b) son siempre opacos, asi que aqui tambien escribimos
// alfa=255 siempre.
static void imgbuf_fill_rect(int32_t handle, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return;
    int32_t iw = (int32_t)images[handle].width, ih = (int32_t)images[handle].height;
    uint8_t r = (uint8_t)(color >> 16), g = (uint8_t)(color >> 8), b = (uint8_t)color;
    for (int32_t j = 0; j < h; j++) {
        int32_t py = y + j;
        if (py < 0 || py >= ih) continue;
        for (int32_t i = 0; i < w; i++) {
            int32_t px = x + i;
            if (px < 0 || px >= iw) continue;
            uint32_t idx = ((uint32_t)py * (uint32_t)iw + (uint32_t)px) * 4;
            image_pixels[handle][idx + 0] = r;
            image_pixels[handle][idx + 1] = g;
            image_pixels[handle][idx + 2] = b;
            image_pixels[handle][idx + 3] = 255;
        }
    }
}

// Ovalo relleno, EXACTAMENTE el mismo algoritmo fila-a-fila que
// SYS_DRAW_OVAL (misma formula, misma isqrt_u32), pero reutilizando
// imgbuf_fill_rect como "pintar una fila de 1 pixel de alto" en vez
// de wm_content_fill_rect.
static void imgbuf_fill_oval(int32_t handle, int32_t bx, int32_t by, int32_t bw, int32_t bh, uint32_t color) {
    if (bw <= 0 || bh <= 0) return;
    int32_t rx = bw / 2, ry = bh / 2;
    if (rx == 0 || ry == 0) return;
    int32_t cx = bx + rx, cy = by + ry;
    for (int32_t dy = -ry; dy <= ry; dy++) {
        uint32_t inner = (uint32_t)(ry * ry - dy * dy);
        int32_t dx = (int32_t)(((uint64_t)rx * isqrt_u32(inner)) / (uint32_t)ry);
        imgbuf_fill_rect(handle, cx - dx, cy + dy, dx * 2 + 1, 1, color);
    }
}

// Blit de imagen a imagen -- lo que le faltaba a SetBuffer ImageBuffer(...).
// Misma semantica que wm_content_blit_image_rect (mascara por color clave,
// mezcla alfa, o copia opaca si viene de DrawBlock), pero escribiendo en los
// pixeles de otra imagen en vez de en el lienzo de una ventana. Recorta
// contra el DESTINO, no contra la ventana: una imagen no tiene barra de
// menu ni Viewport.
static void imgbuf_blit_image(int32_t dst, int32_t src, int32_t x, int32_t y,
                              uint32_t frame, bool solid) {
    if (dst < 0 || dst >= MAX_IMAGES || !images[dst].used) return;
    if (src < 0 || src >= MAX_IMAGES || !images[src].used) return;
    // Dibujarse sobre si misma: origen y destino se pisarian a medio
    // camino y el resultado no estaria definido. Se rechaza.
    if (dst == src) return;

    uint32_t stride = images[src].width;
    uint32_t sw = images[src].width, sh = images[src].height;
    uint32_t sx0 = 0, sy0 = 0;

    if (images[src].cell_width > 0 && images[src].cell_height > 0) {
        // Hoja de sprites: misma cuenta de celdas que SYS_DRAW_IMAGE.
        uint32_t count = images[src].anim_count > 0 ? images[src].anim_count : 1;
        uint32_t cell_idx = images[src].anim_first + (frame % count);
        uint32_t cols = images[src].width / images[src].cell_width;
        if (cols == 0) cols = 1;
        uint32_t rows = images[src].height / images[src].cell_height;
        if (rows == 0) rows = 1;
        uint32_t total_cells = cols * rows;
        if (cell_idx >= total_cells) cell_idx = cell_idx % total_cells;
        sx0 = (cell_idx % cols) * images[src].cell_width;
        sy0 = (cell_idx / cols) * images[src].cell_height;
        sw = images[src].cell_width;
        sh = images[src].cell_height;
    }

    int32_t bx, by; uint32_t bw, bh, sdx, sdy;
    if (!clip_blit(x, y, sw, sh, 0, 0, (int32_t)images[dst].width, (int32_t)images[dst].height,
                   &bx, &by, &bw, &bh, &sdx, &sdy)) return;

    bool has_mask = images[src].has_mask;
    uint32_t mask_color = images[src].mask_color;
    uint32_t dst_w = images[dst].width;

    for (uint32_t iy = 0; iy < bh; iy++) {
        const uint8_t *srow = &image_pixels[src][(((sy0 + sdy + iy) * stride) + (sx0 + sdx)) * 4];
        uint8_t *drow = &image_pixels[dst][(((uint32_t)by + iy) * dst_w + (uint32_t)bx) * 4];
        for (uint32_t ix = 0; ix < bw; ix++) {
            const uint8_t *px = &srow[ix * 4];
            uint32_t px_rgb = ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
            if (has_mask && px_rgb == mask_color) continue;
            uint8_t a = solid ? 255 : px[3];
            if (a == 0) continue;
            uint8_t *d = &drow[ix * 4];
            if (a == 255) {
                d[0] = px[0]; d[1] = px[1]; d[2] = px[2]; d[3] = 255;
                continue;
            }
            d[0] = (uint8_t)((px[0] * a + d[0] * (255 - a)) / 255);
            d[1] = (uint8_t)((px[1] * a + d[1] * (255 - a)) / 255);
            d[2] = (uint8_t)((px[2] * a + d[2] * (255 - a)) / 255);
            d[3] = 255;
        }
    }
}

// Blit de un SUB-RECTANGULO de una imagen a otra imagen. Lo mismo que
// imgbuf_blit_image pero eligiendo que trozo del origen se copia; hace
// falta para cualquier programa que muestre una parte de una imagen
// grande -- un editor con scroll, por ejemplo.
static void imgbuf_blit_rect(int32_t dst, int32_t src, int32_t x, int32_t y,
                             uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh, bool solid) {
    if (dst < 0 || dst >= MAX_IMAGES || !images[dst].used) return;
    if (src < 0 || src >= MAX_IMAGES || !images[src].used) return;
    if (dst == src) return;
    if (rx + rw > images[src].width || ry + rh > images[src].height) return;

    int32_t bx, by; uint32_t bw, bh, sdx, sdy;
    if (!clip_blit(x, y, rw, rh, 0, 0, (int32_t)images[dst].width, (int32_t)images[dst].height,
                   &bx, &by, &bw, &bh, &sdx, &sdy)) return;

    uint32_t stride = images[src].width;
    bool has_mask = images[src].has_mask;
    uint32_t mask_color = images[src].mask_color;
    uint32_t dst_w = images[dst].width;

    for (uint32_t iy = 0; iy < bh; iy++) {
        const uint8_t *srow = &image_pixels[src][(((ry + sdy + iy) * stride) + (rx + sdx)) * 4];
        uint8_t *drow = &image_pixels[dst][(((uint32_t)by + iy) * dst_w + (uint32_t)bx) * 4];
        for (uint32_t ix = 0; ix < bw; ix++) {
            const uint8_t *px = &srow[ix * 4];
            uint32_t px_rgb = ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
            if (has_mask && px_rgb == mask_color) continue;
            uint8_t a = solid ? 255 : px[3];
            if (a == 0) continue;
            uint8_t *d = &drow[ix * 4];
            if (a == 255) { d[0] = px[0]; d[1] = px[1]; d[2] = px[2]; d[3] = 255; continue; }
            d[0] = (uint8_t)((px[0] * a + d[0] * (255 - a)) / 255);
            d[1] = (uint8_t)((px[1] * a + d[1] * (255 - a)) / 255);
            d[2] = (uint8_t)((px[2] * a + d[2] * (255 - a)) / 255);
            d[3] = 255;
        }
    }
}

// ---------------------------------------------------------------
// MINIATURAS
//
// Se hace la miniatura LEYENDO EL ARCHIVO fila a fila, sin cargar la
// imagen en la tabla del sistema. Dos motivos:
//
//   1. Esa tabla no admite imagenes de mas de 1024x1024, asi que un
//      fondo de pantalla de 1920x1080 no tenia miniatura -- el
//      explorador caia al icono generico sin poder explicar por que.
//   2. Cargar 8 MB para pintar un cuadrado de 24 pixeles es absurdo.
//      Aqui solo se reserva UNA fila del original.
//
// Se lee solo una fila de cada 'lado', no la imagen entera: para una
// miniatura de 24 px de un original de 1080 de alto, 24 lecturas.
#define ESCALA_ENCAJAR   0
#define ESCALA_MINIATURA 1
#define ESCALA_ESTIRAR   2

// El encuadre: a partir del tamaño del original y del hueco donde va,
// que trozo se usa, de que tamaño queda y cuanto margen lleva para
// quedar centrado. Esta aparte, sin tocar memoria ni disco, para
// poder comprobarlo con numeros inventados -- las reglas de encuadre
// son faciles de romper sin que se note en una miniatura de 20 px.
typedef struct {
    uint32_t rx, ry, rw, rh;   // el trozo del original que se usa
    uint32_t mw, mh;           // el tamaño que ocupa dentro del hueco
    uint32_t ox, oy;           // margen para centrarlo en el hueco
} encuadre_t;

static void calcular_encuadre(uint32_t iw, uint32_t ih,
                              uint32_t dw, uint32_t dh,
                              uint32_t modo, encuadre_t *e) {
    e->rx = 0; e->ry = 0; e->rw = iw; e->rh = ih;
    if (modo == ESCALA_MINIATURA && (iw > ih * 3 || ih > iw * 3)) {
        // Muy alargada: una hoja de sprites de 640x32 encajada en un
        // cuadrado queda en una tira de dos pixeles que no dice nada.
        // Se recorta el primer cuadrado, que es el primer fotograma.
        e->rw = e->rh = (iw < ih) ? iw : ih;
    }
    if (modo == ESCALA_ESTIRAR) {
        e->mw = dw; e->mh = dh;
    } else if (e->rw * dh >= e->rh * dw) {   // sin decimales: compara proporciones
        e->mw = dw; e->mh = e->rh * dw / e->rw;
    } else {
        e->mw = e->rw * dh / e->rh; e->mh = dh;
    }
    if (e->mw == 0) e->mw = 1;
    if (e->mh == 0) e->mh = 1;
    if (e->mw > dw) e->mw = dw;
    if (e->mh > dh) e->mh = dh;
    e->ox = (dw - e->mw) / 2;
    e->oy = (dh - e->mh) / 2;
}

// El hueco en blanco: lo que la imagen no llega a llenar. Sin esto se
// veria lo que hubiera antes en esa zona del destino.
static void blanquear_hueco(int32_t destino, uint32_t dx, uint32_t dy,
                            uint32_t dw, uint32_t dh) {
    uint8_t *dpx = image_pixels[destino];
    uint32_t dstride = images[destino].width;
    for (uint32_t y = 0; y < dh; y++) {
        uint8_t *p = &dpx[(((uint64_t)(dy + y) * dstride) + dx) * 4];
        for (uint32_t x = 0; x < dw; x++) { p[x*4] = 255; p[x*4+1] = 255; p[x*4+2] = 255; p[x*4+3] = 255; }
    }
}

// Pinta UNA fila ya leida del original en su sitio del destino,
// encogiendola a lo ancho por vecino mas cercano.
static void pintar_fila(int32_t destino, uint32_t dx, uint32_t dy,
                        const encuadre_t *e, uint32_t y, const uint8_t *fila) {
    uint8_t *dpx = image_pixels[destino];
    uint32_t dstride = images[destino].width;
    uint8_t *p = &dpx[(((uint64_t)(dy + e->oy + y) * dstride) + dx + e->ox) * 4];
    for (uint32_t x = 0; x < e->mw; x++) {
        uint32_t sx = e->rx + (uint32_t)(((uint64_t)x * e->rw) / e->mw);
        const uint8_t *s = &fila[(uint64_t)sx * 4];
        p[x*4] = s[0]; p[x*4+1] = s[1]; p[x*4+2] = s[2]; p[x*4+3] = 255;
    }
}

// Comprobaciones comunes de un destino y su hueco.
static bool hueco_valido(int32_t destino, uint32_t dx, uint32_t dy,
                         uint32_t dw, uint32_t dh) {
    if (destino < 0 || destino >= MAX_IMAGES || !images[destino].used) return false;
    if (dw == 0 || dh == 0) return false;
    if (dx + dw > images[destino].width || dy + dh > images[destino].height) return false;
    return true;
}

// Lee la cabecera de un .nimg y devuelve su inodo y su tamaño.

static bool nimg_cabecera(const char *nombre, uint64_t origen, nimg_fuente_t *f,
                          uint32_t *iw_out, uint32_t *ih_out) {
    if (!nimg_fuente_abrir(nombre, origen, f)) return false;
    uint8_t cab[12];
    if (!nimg_fuente_leer(f, 0, cab, 12)) return false;
    if (cab[0] != 'N' || cab[1] != 'I' || cab[2] != 'M' || cab[3] != 'G') return false;
    uint32_t iw = (uint32_t)cab[4] | ((uint32_t)cab[5] << 8) | ((uint32_t)cab[6] << 16) | ((uint32_t)cab[7] << 24);
    uint32_t ih = (uint32_t)cab[8] | ((uint32_t)cab[9] << 8) | ((uint32_t)cab[10] << 16) | ((uint32_t)cab[11] << 24);
    if (iw == 0 || ih == 0 || iw > 8192 || ih > 8192) return false;
    *iw_out = iw; *ih_out = ih;
    return true;
}

static bool archivo_escalado(const char *nombre, int32_t destino,
                             uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh,
                             uint32_t modo, uint64_t origen) {
    if (!hueco_valido(destino, dx, dy, dw, dh)) return false;

    nimg_fuente_t f; uint32_t iw, ih;
    if (!nimg_cabecera(nombre, origen, &f, &iw, &ih)) return false;

    // Las reglas de encuadre viven en calcular_encuadre() y no en cada
    // programa, para que el explorador, el Navegante y el visor
    // enseñen lo mismo.
    encuadre_t e;
    calcular_encuadre(iw, ih, dw, dh, modo, &e);

    uint8_t *fila = (uint8_t *)kmalloc(iw * 4);
    if (!fila) return false;

    blanquear_hueco(destino, dx, dy, dw, dh);

    for (uint32_t y = 0; y < e.mh; y++) {
        uint32_t sy = e.ry + (uint32_t)(((uint64_t)y * e.rh) / e.mh);
        uint64_t off = 12 + ((uint64_t)sy * iw) * 4;
        if (!nimg_fuente_leer(&f, (uint32_t)off, fila, iw * 4)) { kfree(fila); return false; }
        pintar_fila(destino, dx, dy, &e, y, fila);
    }
    kfree(fila);
    return true;
}

// ---------------------------------------------------------------
// DOS MINIATURAS DEL MISMO ARCHIVO, EN UNA SOLA PASADA
//
// El Navegante monta dos mosaicos, uno de 64 px para la cuadricula y
// otro de 20 para la lista. Hasta ahora llamaba dos veces, y cada
// llamada recorria el archivo entero por su cuenta: cada imagen se
// leia dos veces del disco.
//
// Aqui se recorre UNA vez. Se leen las filas que necesita la grande,
// que son mas, y la pequeña se sirve de esas mismas, quedandose con
// la mas cercana a la que le tocaba. El resultado en pantalla es el
// mismo: los dos escalados son por vecino mas cercano de todas
// formas, asi que "la fila mas cercana" es exactamente lo que el
// escalado habria elegido.
//
// Medido con las carpetas de ejemplo: 1.027 comandos a la tarjeta
// contra 799, un 22% menos.
static bool archivo_escalado_doble(const char *nombre,
                                   int32_t dest_g, uint32_t dx_g, uint32_t dy_g, uint32_t lado_g,
                                   int32_t dest_p, uint32_t dx_p, uint32_t dy_p, uint32_t lado_p,
                                   uint64_t origen) {
    if (!hueco_valido(dest_g, dx_g, dy_g, lado_g, lado_g)) return false;
    if (!hueco_valido(dest_p, dx_p, dy_p, lado_p, lado_p)) return false;

    nimg_fuente_t f; uint32_t iw, ih;
    if (!nimg_cabecera(nombre, origen, &f, &iw, &ih)) return false;

    encuadre_t eg, ep;
    calcular_encuadre(iw, ih, lado_g, lado_g, ESCALA_MINIATURA, &eg);
    calcular_encuadre(iw, ih, lado_p, lado_p, ESCALA_MINIATURA, &ep);
    if (eg.mh == 0 || ep.mh == 0) return false;

    uint8_t *fila = (uint8_t *)kmalloc(iw * 4);
    if (!fila) return false;

    blanquear_hueco(dest_g, dx_g, dy_g, lado_g, lado_g);
    blanquear_hueco(dest_p, dx_p, dy_p, lado_p, lado_p);

    uint32_t siguiente_p = 0;
    for (uint32_t y = 0; y < eg.mh; y++) {
        uint32_t sy = eg.ry + (uint32_t)(((uint64_t)y * eg.rh) / eg.mh);
        uint64_t off = 12 + ((uint64_t)sy * iw) * 4;
        if (!nimg_fuente_leer(&f, (uint32_t)off, fila, iw * 4)) { kfree(fila); return false; }
        pintar_fila(dest_g, dx_g, dy_g, &eg, y, fila);

        // Las filas de la pequeña que le tocan a esta del original.
        // La cuenta va al reves que la de arriba: para la fila 'k' de
        // la pequeña, la de la grande que le corresponde es
        // k * mh_g / mh_p. Se pintan todas las que ya han llegado.
        while (siguiente_p < ep.mh &&
               (uint32_t)(((uint64_t)siguiente_p * eg.mh) / ep.mh) <= y) {
            pintar_fila(dest_p, dx_p, dy_p, &ep, siguiente_p, fila);
            siguiente_p++;
        }
    }
    // Por si el redondeo dejo alguna sin pintar al final: se pintan con
    // la ultima fila leida, que es la que mas se le parece. Sin esto
    // quedarian en blanco, y en blanco se nota.
    while (siguiente_p < ep.mh) {
        pintar_fila(dest_p, dx_p, dy_p, &ep, siguiente_p, fila);
        siguiente_p++;
    }

    kfree(fila);
    return true;
}

// ---------------------------------------------------------------
// FONDO DE PANTALLA
//
// Se lee el .nimg y se ESCALA UNA VEZ al tamaño exacto de la pantalla,
// en un bufer propio que se le entrega al gestor de ventanas. No pasa
// por la tabla de imagenes a proposito: aquellas estan limitadas a
// 1024x1024 y una pantalla de 1920 no cabria.
//
// El escalado es de vecino mas cercano, en enteros. Para una foto no es
// lo ideal, pero evita reservar un segundo bufer y hacer cuentas con
// decimales en el kernel, y a tamaño de pantalla apenas se nota.
#define FONDO_ESTIRAR 0
#define FONDO_MOSAICO 1
#define FONDO_CENTRAR 2
#define FONDO_CFG "FONDO.CFG"

static bool fondo_poner(const char *nombre, uint32_t modo) {
    int32_t inode = image_resolve_inode(nombre);
    if (inode < 0) return false;

    uint8_t cab[12];
    if (nemofs_read_at((uint32_t)inode, 0, cab, 12) < 12) return false;
    if (cab[0] != 'N' || cab[1] != 'I' || cab[2] != 'M' || cab[3] != 'G') return false;
    uint32_t iw = (uint32_t)cab[4] | ((uint32_t)cab[5] << 8) | ((uint32_t)cab[6] << 16) | ((uint32_t)cab[7] << 24);
    uint32_t ih = (uint32_t)cab[8] | ((uint32_t)cab[9] << 8) | ((uint32_t)cab[10] << 16) | ((uint32_t)cab[11] << 24);
    if (iw == 0 || ih == 0 || iw > 4096 || ih > 4096) return false;

    uint32_t pw = fb_width(), ph = fb_height();
    if (pw == 0 || ph == 0) return false;

    // El original y el resultado a la vez: dos buferes grandes durante un
    // momento. Se pide primero el del original; si no hay sitio, se sale
    // sin haber tocado el fondo que hubiera.
    uint32_t bytes_src = iw * ih * 4;
    uint8_t *src = (uint8_t *)kmalloc(bytes_src);
    if (!src) return false;
    if (nemofs_read_at((uint32_t)inode, 12, src, bytes_src) < (int32_t)bytes_src) { kfree(src); return false; }

    uint32_t *dst = (uint32_t *)kmalloc((uint64_t)pw * ph * 4);
    if (!dst) { kfree(src); return false; }

    for (uint32_t y = 0; y < ph; y++) {
        for (uint32_t x = 0; x < pw; x++) {
            uint32_t sx, sy;
            bool dentro = true;
            if (modo == FONDO_MOSAICO) {
                sx = x % iw; sy = y % ih;
            } else if (modo == FONDO_CENTRAR) {
                int32_t ox = ((int32_t)pw - (int32_t)iw) / 2;
                int32_t oy = ((int32_t)ph - (int32_t)ih) / 2;
                int32_t rx = (int32_t)x - ox, ry = (int32_t)y - oy;
                if (rx < 0 || ry < 0 || (uint32_t)rx >= iw || (uint32_t)ry >= ih) dentro = false;
                sx = (uint32_t)rx; sy = (uint32_t)ry;
            } else {
                sx = (uint32_t)(((uint64_t)x * iw) / pw);
                sy = (uint32_t)(((uint64_t)y * ih) / ph);
            }
            if (!dentro) { dst[(uint64_t)y * pw + x] = 0x00204060; continue; }  // el azul del escritorio
            const uint8_t *px = &src[((uint64_t)sy * iw + sx) * 4];
            dst[(uint64_t)y * pw + x] = ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
        }
    }
    kfree(src);
    wm_fondo_set(dst, pw, ph);
    return true;
}

// Se recuerda entre arranques en un archivo suyo, no en DESKTOP.CFG: son
// cosas distintas y asi borrar uno no se lleva el otro por delante.
static void fondo_recordar(const char *nombre, uint32_t modo) {
    uint8_t buf[132];
    uint32_t n = 0;
    while (nombre[n] && n < 127) { buf[n + 1] = (uint8_t)nombre[n]; n++; }
    buf[0] = (uint8_t)modo;
    buf[n + 1] = 0;
    int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, FONDO_CFG);
    if (idx < 0) idx = nemofs_create(NEMOFS_ROOT_INODE, FONDO_CFG, NEMOFS_TYPE_FILE);
    if (idx >= 0) nemofs_write_file_if_changed((uint32_t)idx, buf, n + 2);
}

static void fondo_olvidar(void) {
    nemofs_delete(NEMOFS_ROOT_INODE, FONDO_CFG);
}

// Al arrancar: si hay fondo guardado, se pone. Que falle no es grave.
void fondo_cargar_guardado(void) {
    int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, FONDO_CFG);
    if (idx < 0) return;
    uint8_t buf[132];
    int32_t n = nemofs_read_file((uint32_t)idx, buf, sizeof(buf) - 1);
    if (n < 3) return;
    buf[n] = 0;
    fondo_poner((const char *)&buf[1], buf[0]);
}

// Portapapeles: un buffer compartido a nivel de kernel, para que
// cualquier programa pueda leer lo que copio otro.
#define CLIPBOARD_MAX 4096
static char clipboard_buf[CLIPBOARD_MAX];
static uint32_t clipboard_len = 0;

// Portapapeles de archivos -- guarda una referencia (nombre + carpeta
// + volumen), no el contenido. Lo llena SYS_FILE_CLIPBOARD_SET, lo
// consulta SYS_FILE_CLIPBOARD_GET.
static bool file_clip_used = false;
static char file_clip_name[28] = "";
static uint32_t file_clip_parent = 0;
static uint32_t file_clip_volume = 0;

// Cola de salida de consola, una por ventana -- cuando un programa
// lanzado con 'run' llama a SYS_WRITE_STRING, el texto se encola aqui
// (en vez de ir a la UART) para que quien lo lanzo lo vaya leyendo
// caracter a caracter con SYS_READ_CONSOLE_OUTPUT, e integrarlo en su
// propia consola sin que los dos programas dibujen a la vez sobre la
// misma ventana.
#define CONSOLE_QUEUE_SIZE 512
#define MAX_CONSOLE_WINDOWS MAX_WINDOWS // la de wm.h: una sola definicion
typedef struct {
    char buf[CONSOLE_QUEUE_SIZE];
    uint32_t read_pos, write_pos;
} console_queue_t;
static console_queue_t console_queues[MAX_CONSOLE_WINDOWS];

// Se llama al crear/reutilizar una ventana -- ver el bug real
// documentado junto a wm_create_window(): sin esto, una ventana nueva
// que reutiliza el indice de una ya cerrada hereda los bytes sin leer
// de la tarea ANTERIOR, y aparecen solos en una consola que no ha
// ejecutado nada.
void console_queue_reset(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_CONSOLE_WINDOWS) return;
    console_queues[window_idx].read_pos = 0;
    console_queues[window_idx].write_pos = 0;
}

static void console_queue_write(int32_t window_idx, const char *text) {
    if (window_idx < 0 || window_idx >= MAX_CONSOLE_WINDOWS) return;
    console_queue_t *q = &console_queues[window_idx];
    for (uint32_t i = 0; text[i] != '\0'; i++) {
        uint32_t next = (q->write_pos + 1) % CONSOLE_QUEUE_SIZE;
        if (next == q->read_pos) break; // cola llena -- descartamos el resto antes que bloquear
        q->buf[q->write_pos] = text[i];
        q->write_pos = next;
    }
}

static char console_queue_read(int32_t window_idx) {
    if (window_idx < 0 || window_idx >= MAX_CONSOLE_WINDOWS) return 0;
    console_queue_t *q = &console_queues[window_idx];
    if (q->read_pos == q->write_pos) return 0; // vacia
    char c = q->buf[q->read_pos];
    q->read_pos = (q->read_pos + 1) % CONSOLE_QUEUE_SIZE;
    return c;
}

// Definiciones REALES de locked_buffer_peek/poke_one (declaradas
// arriba, junto al resto del sistema LockBuffer) -- van aqui porque
// necesitan wm_content_get_pixel/fill_rect (de wm.h) e
// images[]/image_pixels[] (ya visibles desde antes en este archivo).
// offset/4 = indice de pixel (x,y en orden de fila), offset%4 = canal
// (0=R,1=G,2=B,3=A).
static uint32_t locked_buffer_peek(uint32_t offset) {
    if (!g_buffer_locked || g_locked_width == 0) return 0;
    uint32_t pixel_idx = offset / 4;
    uint32_t channel = offset % 4;
    uint32_t x = pixel_idx % g_locked_width;
    uint32_t y = pixel_idx / g_locked_width;
    if (y >= g_locked_height) return 0;
    if (g_locked_buffer_id == 0) {
        int32_t win = task_ensure_window();
        if (win < 0) return 0;
        uint32_t menu_off = gadgets_menubar_height(win);
        uint32_t rgb = wm_content_get_pixel(win, x, y + menu_off);
        if (channel == 0) return (rgb >> 16) & 0xFF;
        if (channel == 1) return (rgb >> 8) & 0xFF;
        if (channel == 2) return rgb & 0xFF;
        return 255; // alfa: el contenido de ventana ya renderizado se trata como opaco
    }
    int32_t handle = g_locked_buffer_id - 1;
    if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return 0;
    if (x >= images[handle].width || y >= images[handle].height) return 0;
    return image_pixels[handle][(y * images[handle].width + x) * 4 + channel];
}
static void locked_buffer_poke_one(uint32_t offset, uint8_t byte_value) {
    if (!g_buffer_locked || g_locked_width == 0) return;
    uint32_t pixel_idx = offset / 4;
    uint32_t channel = offset % 4;
    uint32_t x = pixel_idx % g_locked_width;
    uint32_t y = pixel_idx / g_locked_width;
    if (y >= g_locked_height) return;
    if (g_locked_buffer_id == 0) {
        int32_t win = task_ensure_window();
        if (win < 0) return;
        uint32_t menu_off = gadgets_menubar_height(win);
        // lectura-modificacion-escritura: hace falta el pixel COMPLETO
        // para reconstruirlo con un solo canal cambiado (channel==3,
        // alfa, se ignora -- el contenido de ventana no tiene canal
        // alfa real que modificar).
        if (channel == 3) return;
        uint32_t rgb = wm_content_get_pixel(win, x, y + menu_off);
        uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
        if (channel == 0) r = byte_value;
        else if (channel == 1) g = byte_value;
        else b = byte_value;
        uint32_t new_rgb = (r << 16) | (g << 8) | b;
        wm_content_fill_rect(win, x, y + menu_off, 1, 1, new_rgb);
        return;
    }
    int32_t handle = g_locked_buffer_id - 1;
    if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return;
    if (x >= images[handle].width || y >= images[handle].height) return;
    image_pixels[handle][(y * images[handle].width + x) * 4 + channel] = byte_value;
}

// -- ExecFile/CreateProcess --
//
// LIMITACION DOCUMENTADA: las versiones reales de estos comandos son
// especificas de Windows (ExecFile usa ShellExecute para abrir
// CUALQUIER archivo con su programa asociado; CreateProcess lanza un
// .exe de consola y ofrece una tuberia bidireccional real via
// ReadLine/WriteLine sobre su stdin/stdout). Nemo OS no tiene
// concepto de "programa asociado por tipo de archivo" ni una tuberia
// entre tareas -- REINTERPRETAMOS ambos comandos de la forma mas util
// posible dentro de nuestro propio modelo: lanzan un programa .pro
// real de Nemo OS como una tarea nueva (usando el mismo mecanismo que
// ya usa la shell con 'run'), con su salida (Print) redirigida a la
// ventana de quien lo lanza. CreateProcess "funciona" en el sentido
// de que SI arranca el programa, pero el stream que devuelve no tiene
// una tuberia real detras -- ReadLine/WriteLine/Eof sobre el actuan
// como si estuviera vacio y ya cerrado, en vez de fallar de forma
// confusa.
static int32_t exec_program(const char *filename, const char *arg) {
    int32_t parent = nemofs_find_child(NEMOFS_ROOT_INODE, filename);
    if (parent >= 0) {
        parent = (int32_t)NEMOFS_ROOT_INODE;
    } else {
        int32_t programas = nemofs_find_child(NEMOFS_ROOT_INODE, "PROGRAMAS");
        parent = (programas >= 0) ? programas : (int32_t)NEMOFS_ROOT_INODE;
    }
    int32_t caller_window = task_get_current_window();
    return task_spawn_from_file(filename, (uint32_t)parent, -1, arg, caller_window);
}

// -- Validacion de punteros de usuario -- ver la nota grande junto a
// task_owns_range() en tasks.c para el porque.
//
// Cadena de un maximo razonable (4096 -- de sobra para cualquier
// nombre de archivo o texto de un gadget) terminada en '\0', SIN
// poder salirse nunca del area de la tarea que llama: se comprueba
// byte a byte a medida que se busca el terminador, no se calcula
// primero un strlen() sobre memoria sin validar (eso ya seria leer
// fuera de los limites si la cadena esta mal formada a proposito).
// Devuelve NULL si no es valida (fuera del area, o sin '\0' dentro
// del limite) -- el llamador decide que error devolver.
static const char *validate_str(uint64_t addr, uint32_t max_len) {
    int32_t ctx = task_get_current_slot();
    const char *p = (const char *)addr;
    for (uint32_t i = 0; i < max_len; i++) {
        if (!task_owns_range(ctx, addr + i, 1)) return 0;
        if (p[i] == '\0') return p;
    }
    return 0; // no cupo un terminador dentro del limite
}

// Rango de bytes (lectura o escritura) que la tarea que llama tiene
// que poseer entero, de un tiron -- para buffers de tamaño conocido
// (a1=buffer, a2=tamaño, patron mas comun de toda la tabla de
// syscalls). true si el rango completo [addr, addr+len) es suyo.
static bool validate_buf(uint64_t addr, uint64_t len) {
    return task_owns_range(task_get_current_slot(), addr, len);
}

// ¿Tiene el foco la ventana de la tarea que hace la llamada? Para un
// programa de consola sin ventana propia, cuenta la ventana de la shell
// donde escribe.
static bool tarea_tiene_foco(void) {
    int32_t w = task_get_current_window();
    if (w < 0) w = task_get_console_window();
    return w >= 0 && w == wm_get_focused_window();
}

// ---------------------------------------------------------------
// Datos del equipo -- para SYS_CPU_NOMBRE / SYS_CPU_DATOS
// ---------------------------------------------------------------
// El nombre del procesador NO se inventa ni se cablea: sale de
// MIDR_EL1, el registro de identificacion que todo ARM64 lleva. Asi
// esto dice la verdad tanto en la Pi 4 (Cortex-A72) como en QEMU
// (Cortex-A53, que es con lo que se lanza), sin una linea distinta
// para cada uno.
static uint32_t cpu_nombre(char *out, uint32_t max) {
    if (!out || max == 0) return 0;
    uint64_t midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    uint32_t implementador = (uint32_t)((midr >> 24) & 0xFF);
    uint32_t pieza         = (uint32_t)((midr >> 4) & 0xFFF);

    const char *nombre = 0;
    if (implementador == 0x41) {   // 0x41 = 'A' = ARM Ltd.
        switch (pieza) {
            case 0xD03: nombre = "ARM Cortex-A53"; break;
            case 0xD04: nombre = "ARM Cortex-A35"; break;
            case 0xD05: nombre = "ARM Cortex-A55"; break;
            case 0xD07: nombre = "ARM Cortex-A57"; break;
            case 0xD08: nombre = "ARM Cortex-A72"; break;   // la Raspberry Pi 4
            case 0xD09: nombre = "ARM Cortex-A73"; break;
            case 0xD0B: nombre = "ARM Cortex-A76"; break;   // la Raspberry Pi 5
            case 0xD41: nombre = "ARM Cortex-A78"; break;
            default: break;
        }
    }

    uint32_t n = 0;
    if (nombre) {
        while (nombre[n] && n < max - 1) { out[n] = nombre[n]; n++; }
    } else {
        // Uno que no esta en la tabla: mejor decir el numero crudo que
        // mentir con un nombre parecido.
        const char *pre = "ARM64 (MIDR 0x";
        while (*pre && n < max - 1) out[n++] = *pre++;
        for (int d = 7; d >= 0 && n < max - 1; d--) {
            uint32_t nib = (uint32_t)((midr >> (d * 4)) & 0xF);
            out[n++] = (char)(nib < 10 ? ('0' + nib) : ('A' + nib - 10));
        }
        if (n < max - 1) out[n++] = ')';
    }

    // De donde esta corriendo: se sabe al compilar, no hay que
    // preguntarselo a nadie.
#ifdef NEMO_QEMU
    const char *donde = " (QEMU)";
#else
    const char *donde = " (Raspberry Pi 4)";
#endif
    while (*donde && n < max - 1) out[n++] = *donde++;
    out[n] = '\0';
    return n;
}

// Frecuencia del procesador en MHz. En la Pi se la pedimos al
// firmware por mailbox (reloj 3 = ARM); en QEMU no hay a quien
// preguntar, y devolver 0 es mas honesto que inventar una cifra.
static uint32_t cpu_mhz(void) {
#ifdef NEMO_QEMU
    return 0;
#else
    uint32_t hz = mailbox_reloj_pi4(3);
    return hz / 1000000u;
#endif
}

// ---------------------------------------------------------------
// Filas de pixeles -- SYS_FILA_LEER / ESCRIBIR / RELLENAR / TIRA
// ---------------------------------------------------------------
// Por que existen: una llamada al sistema cuesta 3,5 us MEDIDOS en la Pi 4
// y un pixel cuesta 4 ns. Cualquier cosa que trabaje pixel a pixel desde
// un programa paga casi 900 veces mas peaje que trabajo. El relleno por
// inundacion del Pintor era el caso extremo: ~4,2 millones de llamadas en
// un lienzo de 1024x1024, unos 15 segundos.
//
// La tentacion era resolverlo en el programa, leyendo la fila de golpe y
// recorriendola desde Lua. Medido con el Lua 5.5 de este proyecto: una
// llamada a nemo.leer_i32 cuesta ~210 ns en x86-64, asi que en la Pi
// quedaria en el mismo orden que la propia syscall. No arregla nada:
// cambia un peaje por otro. EL TRABAJO POR PIXEL TIENE QUE QUEDARSE EN EL
// KERNEL, y es de lo que van estas cuatro.
//
// Trabajan por FILAS, que es como estan guardados los pixeles, y reciben
// la imagen destino COMO ARGUMENTO, igual que WritePixel. NO usan el
// destino global de ImageBuffer a proposito: ese estado global es justo el
// que se mezclaba entre nucleos al llegar el SMP (ver la nota del estado
// de dibujo por tarea mas arriba). Una syscall que recibe su destino no
// tiene ese problema.
//
// Todas devuelven cuantos pixeles han tratado de verdad, ya recortados a
// la imagen, o -1 si los argumentos no valen. La diferencia importa: un 0
// es "no quedaba nada dentro de la imagen" y un -1 es "te has equivocado".
// Sin distinguirlas se repetiria el patron de fallos silenciosos que se
// cazo en el generador de Nemo Basic.

// Resuelve un 'buffer' de los de las syscalls de pixeles (misma
// convencion que ImageBuffer: N = imagen N-1) a los pixeles de una imagen.
//
// Solo IMAGENES. La ventana actual (buffer 0) devuelve false A PROPOSITO:
// ahi ya hay caminos rapidos de verdad (DrawBlock, el blit de wm.c) y una
// fila tendria que arrastrar Origin, Viewport y el alto de la barra de
// menus, que es exactamente donde viven los errores de coordenadas. Si
// alguna vez hace falta, se añade aparte sin tocar esto.
static bool fila_imagen(int32_t buffer, uint8_t **px, uint32_t *ancho, uint32_t *alto) {
    if (buffer <= 0) return false;
    int32_t handle = buffer - 1;
    if (handle >= MAX_IMAGES || !images[handle].used || !image_pixels[handle]) return false;
    *px = image_pixels[handle];
    *ancho = images[handle].width;
    *alto = images[handle].height;
    return true;
}

// Cuantos bytes ocupa cada pixel en el buffer DEL PROGRAMA (no en la
// imagen, que siempre son 4).
//
// Existe porque los dos lenguajes guardan un entero de distinta forma:
// nemo.buffer() en Lua es un array de bytes que se lee de 4 en 4, pero un
// array de Nemo Basic tiene elementos de 8 BYTES (el generador indexa con
// lsl #3). Sin esto, un ReadRow sobre un array de Basic dejaria los
// pixeles apelotonados de 4 en 4 y el programa leeria dos pixeles metidos
// en un elemento y basura en el siguiente -- sin ningun aviso.
//
// 0 se admite como 4 para que lo que ya llamaba con cinco argumentos siga
// funcionando igual. Cualquier otro valor es un error: mas vale un -1 que
// adivinar.
static uint32_t fila_paso(uint64_t a5) {
    if (a5 == 0 || a5 == 4) return 4;
    if (a5 == 8) return 8;
    return 0;
}

// Recorta la fila [x, x+n) a los bordes de la imagen.
//
// Devuelve false si no queda ni un pixel dentro. 'x_ok' y 'n_ok' son el
// tramo que si esta dentro, y 'salto' cuantos pixeles del buffer del
// usuario hay que saltarse por el recorte de la IZQUIERDA -- sin eso, una
// fila que empieza en x=-5 escribiria en el buffer del usuario desde el
// principio y saldria corrida cinco pixeles.
//
// Las cuentas van en int64_t: x y n vienen del programa y 'x + n' con dos
// int32_t grandes se desborda, que es UB y encima silencioso.
static bool fila_recortar(int32_t x, int32_t y, int32_t n, uint32_t ancho, uint32_t alto,
                          uint32_t *x_ok, uint32_t *n_ok, uint32_t *salto) {
    if (n <= 0) return false;
    if (y < 0 || (uint32_t)y >= alto) return false;
    int64_t ini = (int64_t)x, fin = (int64_t)x + (int64_t)n;   // [ini, fin)
    int64_t s = 0;
    if (ini < 0) { s = -ini; ini = 0; }
    if (fin > (int64_t)ancho) fin = (int64_t)ancho;
    if (ini >= fin) return false;
    *x_ok  = (uint32_t)ini;
    *n_ok  = (uint32_t)(fin - ini);
    *salto = (uint32_t)s;
    return true;
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    // el sexto argumento (x5): SYS_SPI y SYS_FILA_TIRA
    int32_t current_window = task_get_current_window();

    switch (num) {

        // -- Sistema --

        case SYS_EXIT:
            // Ventana y gadgets los cierra task_exit_current(); no vuelve.
            task_exit_current();

        case SYS_SLEEP: {
            // Mismo resultado que antes (esperar a0 latidos), pero durmiendo
            // de verdad en vez de ceder el turno en bucle: ver
            // task_dormir_hasta. Con 0 latidos, al menos cede una vez.
            uint64_t ticks_to_wait = a0;
            if (ticks_to_wait == 0) { task_yield(); return 0; }
            task_dormir_hasta(timer_get_ticks() + ticks_to_wait);
            return 0;
        }

        case SYS_GET_TICKS:
            return timer_get_ticks();

        case SYS_MICROS:
            return timer_micros();

        case SYS_LAUNCH_PROGRAM: {
            const char *name = validate_str(a0, 256);
            if (!name) return 0;
            const char *arg = (a1 != 0) ? validate_str(a1, 256) : "";
            if (!arg) return 0;
            uint32_t search_dir = (uint32_t)a2;
            wm_request_launch(name, arg, current_window, search_dir);
            return 0;
        }

        case SYS_GET_LAUNCH_ARG: {
            uint32_t max_len = (uint32_t)a1;
            if (max_len == 0 || !validate_buf(a0, max_len)) return 0;
            char *out = (char *)a0;
            const char *arg = task_get_launch_arg();
            uint32_t i = 0;
            while (arg[i] != '\0' && i < max_len - 1) { out[i] = arg[i]; i++; }
            out[i] = '\0';
            return i;
        }

        case SYS_CLIPBOARD_SET: {
            uint32_t len = (uint32_t)a1;
            if (len > sizeof(clipboard_buf)) len = sizeof(clipboard_buf);
            if (!validate_buf(a0, len)) return 0;
            const char *src = (const char *)a0;
            for (uint32_t i = 0; i < len; i++) clipboard_buf[i] = src[i];
            clipboard_len = len;
            return 0;
        }

        case SYS_CLIPBOARD_GET: {
            uint32_t max_len = (uint32_t)a1;
            if (!validate_buf(a0, max_len)) return 0;
            char *dst = (char *)a0;
            uint32_t len = (clipboard_len < max_len) ? clipboard_len : max_len;
            for (uint32_t i = 0; i < len; i++) dst[i] = clipboard_buf[i];
            return len;
        }

        // -- Texto / consola --

        case SYS_WRITE_CHAR:
            uart_putc((char)a0);
            return 0;

        case SYS_WRITE_STRING: {
            const char *msg = validate_str(a0, 4096);
            if (!msg) return 0;
            int32_t console_target = task_get_console_window();
            if (console_target >= 0) {
                console_queue_write(console_target, msg);
            } else {
                uart_puts(msg);
            }
            return 0;
        }

        case SYS_READ_CONSOLE_OUTPUT:
            if (current_window < 0) return 0;
            return (uint64_t)(uint8_t)console_queue_read(current_window);

        case SYS_POLL_EVENT: {
            if (current_window < 0) return 0;
            return (uint64_t)(int64_t)gadgets_poll_raw_event(current_window);
        }

        case SYS_GET_EVENT_INFO: {
            if (current_window < 0) return 0;
            int32_t src = gadgets_get_last_event_source(current_window);
            int32_t data = gadgets_get_last_event_data(current_window);
            return ((uint64_t)(uint32_t)src << 32) | (uint32_t)data;
        }

        case SYS_GET_EVENT_XY: {
            if (current_window < 0) return 0;
            int32_t x = gadgets_get_last_event_x(current_window);
            int32_t y = gadgets_get_last_event_y(current_window);
            return ((uint64_t)(uint32_t)x << 32) | (uint32_t)y;
        }

        case SYS_READ_CHAR: {
            // Si otra tarea esta leyendo una linea entera, para esta
            // no hay teclas: son suyas. Sin esto la shell le robaria
            // la entrada a los programas que lanza.
            if (g_input_owner >= 0 && g_input_owner != task_get_current_slot()) return 0;
            if (current_window != wm_get_focused_window()) return 0;
            char c;
            if (input_read_char(&c)) return (uint64_t)(uint8_t)c;
            return 0;
        }

        case SYS_READ_CHAR_WAIT: {
            char c;
            int32_t wx, wy;
            uint32_t ww, wh;
            if (!wm_get_window_client_rect(current_window, &wx, &wy, &ww, &wh)) {
                return (uint64_t)-1;
            }
            uint32_t last_ww = ww, last_wh = wh;

            while (1) {
                if (!wm_get_window_client_rect(current_window, &wx, &wy, &ww, &wh)) {
                    return (uint64_t)-1;
                }
                if (ww != last_ww || wh != last_wh) {
                    return (uint64_t)-2;
                }
                // Mismo respeto al propietario del teclado que en
                // SYS_READ_CHAR: si otra tarea esta leyendo una linea
                // entera, aqui no hay teclas que coger.
                bool puedo_leer = (g_input_owner < 0 || g_input_owner == task_get_current_slot());
                if (puedo_leer && current_window == wm_get_focused_window() && input_read_char(&c)) {
                    return (uint64_t)(uint8_t)c;
                }
                // Esperar al siguiente latido: el teclado solo se lee una vez por
                // latido (turno del nucleo 0), asi que mirar antes no sirve de nada
                // y solo gasta vueltas. Ver task_dormir_hasta.
                task_dormir_hasta(timer_get_ticks() + 1);
            }
        }

        case SYS_PUMP: {
            // Antes, si la tarea no tenia ventana (modo consola, como
            // nuestro propio compilador autohospedado), esto fallaba
            // ANTES de llegar a task_yield() -- convirtiendo
            // cualquier "for(;;) pump();" en un bucle ocupado de
            // verdad, sin ceder el control nunca. Ahora cedemos
            // SIEMPRE; el rectangulo de la ventana solo se consulta
            // si de verdad existe una.
            if (current_window >= 0) {
                int32_t wx, wy;
                uint32_t ww, wh;
                if (!wm_get_window_client_rect(current_window, &wx, &wy, &ww, &wh)) {
                    task_yield(); // la ventana se cerro externamente -- aun asi cedemos
                    return (uint64_t)-1;
                }
            }
            // Dormir hasta el SIGUIENTE LATIDO del reloj (100 por segundo).
            //
            // Con un solo nucleo, ceder el turno ya significaba esperar la
            // vuelta del kernel, que duerme hasta el latido: en la practica
            // Pump esperaba un latido, y un programa con un bucle de Pump
            // iba a unas 100 vueltas por segundo. Con varios nucleos, ceder
            // en un secundario es casi instantaneo: el explorador en reposo
            // daba cientos de vueltas por segundo peleando por el candado,
            // y los juegos pasaron de ~100 a mas de 600 fotogramas.
            //
            // Ahora Pump vuelve a esperar un latido, y ademas va al mismo
            // compas que la pantalla: el nucleo 0 compone en cada latido.
            // El programa dibuja, publica su fotograma al ceder (task_yield)
            // y duerme; el latido lo pinta y lo despierta para el siguiente.
            task_dormir_hasta(timer_get_ticks() + 1);
            return 0;
        }

        // -- Archivos --

        case SYS_FILE_OPEN: {
            const char *name = validate_str(a0, 256);
            if (!name) return (uint64_t)-1;
            uint32_t parent = (uint32_t)a1;
            uint32_t volume = (uint32_t)a2;
            // RUTAS CON CARPETAS. Esta es la que usa WriteFile(nombre$,
            // contenido$), que pasa SIEMPRE la raiz como carpeta: sin esto,
            // WriteFile("JUEGOS/PARTIDA.DAT") creaba un archivo llamado
            // literalmente "JUEGOS/PARTIDA.DAT" colgando de la raiz, y
            // OpenFile de esa misma ruta no lo encontraba jamas.
            char hoja[NEMOFS_MAX_NAME + 1];
            int es_ruta = ruta_o_nombre(&name, &parent, hoja, sizeof(hoja));
            if (es_ruta < 0) return (uint64_t)-1;          // esa carpeta no existe
            if (volume == VOLUME_FAT) {
                // FAT escribe solo en la raiz (fat_write_file la busca
                // ahi), asi que aceptar una ruta seria prometer algo que
                // no se cumple. Mejor decir que no.
                if (es_ruta) return (uint64_t)-1;
                return (uint64_t)(int64_t)fat_handle_open(parent, name);
            }
            int32_t idx = nemofs_find_child(parent, name);
            if (idx < 0) idx = nemofs_create(parent, name, NEMOFS_TYPE_FILE);
            return (uint64_t)(int64_t)idx;
        }

        case SYS_FILE_READ: {
            uint32_t id = (uint32_t)a0;
            uint32_t max_size = (uint32_t)a2;
            if (!validate_buf(a1, max_size)) return (uint64_t)-1;
            void *buf = (void *)a1;
            uint32_t volume = (uint32_t)a3;
            if (volume == VOLUME_FAT) {
                if (id >= MAX_FAT_HANDLES || !fat_handles[id].used || !fat_handles[id].has_entry) return (uint64_t)-1;
                uint32_t out_size = 0;
                if (!fat_read_file(&fat_handles[id].entry, buf, max_size, &out_size)) return (uint64_t)-1;
                return (uint64_t)out_size;
            }
            return (uint64_t)(int64_t)nemofs_read_file(id, buf, max_size);
        }

        case SYS_FILE_WRITE: {
            uint32_t id = (uint32_t)a0;
            uint32_t size = (uint32_t)a2;
            if (!validate_buf(a1, size)) return (uint64_t)-1;
            const void *buf = (const void *)a1;
            uint32_t volume = (uint32_t)a3;
            if (volume == VOLUME_FAT) {
                if (id >= MAX_FAT_HANDLES || !fat_handles[id].used) return (uint64_t)-1;
                // Si el handle se abrio sobre un archivo YA existente,
                // el nombre vive en su 'entry'; si no existia todavia,
                // vive en 'pending_name' -- en cualquiera de los dos
                // casos, fat_write_file ya sabe sobrescribir o crear
                // segun haga falta.
                const char *target_name = fat_handles[id].has_entry
                    ? fat_handles[id].entry.name
                    : fat_handles[id].pending_name;
                if (!fat_write_file(target_name, buf, size)) return (uint64_t)-1;
                fat_find_root(target_name, &fat_handles[id].entry);
                fat_handles[id].has_entry = true;
                fat_handles[id].cursor.cluster = 0;     // su cadena de clusters puede haber cambiado
                fat_handles[id].cursor.indice = 0;
                fat_handles[id].fin.cluster = 0;
                fat_handles[id].fin.indice = 0;
                return 0;
            }
            return nemofs_write_file(id, buf, size) ? 0 : (uint64_t)-1;
        }

        case SYS_FILE_APPEND: {
            // Escribir POR PARTES (fase 5b): añade un trozo al FINAL de un
            // archivo del volumen FAT. a0 = identificador (SYS_FILE_OPEN),
            // a1 = buffer, a2 = bytes (como mucho 64 KB por llamada),
            // a3 = volumen -> bytes añadidos, o -1. Antes solo se podia
            // escribir un archivo ENTERO de una vez, en una sola llamada con
            // el candado tomado: el sistema se paraba mientras tanto, y el
            // archivo tenia que caber entero en la memoria del programa.
            // Escribir en la tarjeta es mas lento que leer: de ahi los 64 KB.
            // Si el archivo aun no existe (se abrio para crearlo), se crea
            // vacio en la primera llamada.
            uint32_t id = (uint32_t)a0;
            uint32_t len = a2 > (64u * 1024u) ? (64u * 1024u) : (uint32_t)a2;

            // Tambien en NemoFS. Antes solo valia para FAT,
            // con el argumento de que en NemoFS nada pasa de ~8 MB -- pero
            // el problema no era el tamaño del ARCHIVO sino el de la
            // MEMORIA: escribir de una vez obliga a tener el archivo
            // entero en la tarea, y una tarea tiene 16 MB. Copiar un
            // .nimg de 4 MB no cabia comodamente en ningun sitio.
            if ((uint32_t)a3 == VOLUME_NEMOFS) {
                if (len > 0 && !validate_buf(a1, len)) return (uint64_t)-1;
                if (len == 0) return 0;
                if (!nemofs_append(id, (const void *)(uintptr_t)a1, len)) return (uint64_t)-1;
                return len;
            }

            if ((uint32_t)a3 != VOLUME_FAT) return (uint64_t)-1;
            if (id >= MAX_FAT_HANDLES || !fat_handles[id].used) return (uint64_t)-1;
            if (len > 0 && !validate_buf(a1, len)) return (uint64_t)-1;
            fat_handle_t *fh = &fat_handles[id];
            if (!fh->has_entry) {
                uint8_t nada = 0;
                if (!fat_create_file(fh->pending_name, &nada, 0)) return (uint64_t)-1;
                if (!fat_find_root(fh->pending_name, &fh->entry)) return (uint64_t)-1;
                fh->has_entry = true;
                fh->fin.cluster = 0; fh->fin.indice = 0;
            }
            uint32_t escritos = 0;
            bool ok = fat_append(fh->entry.name, &fh->fin, (const void *)(uintptr_t)a1, len, &escritos);
            fat_find_root(fh->entry.name, &fh->entry);   // tamaño nuevo, para SYS_FILE_READ_AT
            if (!ok && escritos == 0) return (uint64_t)-1;
            return escritos;
        }

        case SYS_FILE_CLOSE: {
            // Solo FAT tiene huecos que soltar. Se comprueba el dueño: un
            // programa no puede cerrar el archivo de otro.
            if ((uint32_t)a1 != VOLUME_FAT) return 0;
            uint32_t id = (uint32_t)a0;
            if (id >= MAX_FAT_HANDLES || !fat_handles[id].used) return (uint64_t)-1;
            if (!duena_es_actual(&duena_fat[id])) return (uint64_t)-1;
            fat_handles[id].used = false;
            return 0;
        }

        case SYS_FILE_READ_AT: {
            // Leer POR PARTES desde cualquier posicion (fase 5 de la
            // memoria): a0 = identificador de SYS_FILE_OPEN, a1 = buffer,
            // a2 = bytes, a3 = posicion, a4 = volumen -> bytes leidos (0 en
            // el final), o -1. Como mucho 256 KB por llamada: leer de la
            // tarjeta retiene el candado grande, y con trozos pequeños
            // ninguna llamada congela la interfaz de forma apreciable. El
            // programa pide trozo tras trozo; la velocidad total es la
            // misma. En FAT, el manejador guarda un cursor: leer seguido
            // nunca vuelve a recorrer la cadena de clusters desde el
            // principio.
            uint32_t id = (uint32_t)a0;
            uint32_t len = a2 > (256u * 1024u) ? (256u * 1024u) : (uint32_t)a2;
            if (len == 0) return 0;
            if (!validate_buf(a1, len)) return (uint64_t)-1;
            if (a3 > 0xFFFFFFFFull) return 0;               // mas alla de 4 GB: ningun archivo llega
            if ((uint32_t)a4 == VOLUME_FAT) {
                if (id >= MAX_FAT_HANDLES || !fat_handles[id].used || !fat_handles[id].has_entry) return (uint64_t)-1;
                uint32_t leidos = 0;
                if (!fat_read_at(&fat_handles[id].entry, &fat_handles[id].cursor, (uint32_t)a3,
                                 (void *)(uintptr_t)a1, len, &leidos)) return (uint64_t)-1;
                return leidos;
            }
            return (uint64_t)(int64_t)nemofs_read_at(id, (uint32_t)a3, (void *)(uintptr_t)a1, len);
        }

        case SYS_FILE_LIST: {
            uint32_t parent = (uint32_t)a0;
            uint32_t max_entries = (uint32_t)a2;
            if (!validate_buf(a1, (uint64_t)max_entries * 40)) return 0; // 40 bytes por entrada, ver la nota junto a SYS_FILE_LIST
            uint8_t *out = (uint8_t *)a1;
            uint32_t volume = (uint32_t)a3;

            if (volume == VOLUME_FAT) {
                // 'parent' puede venir con el desplazamiento (ver
                // FAT_DIR_CLUSTER_OFFSET) si el programa esta
                // navegando dentro de una carpeta abierta antes, o
                // ser 0 (raiz).
                uint32_t real_parent = (parent >= FAT_DIR_CLUSTER_OFFSET) ? (parent - FAT_DIR_CLUSTER_OFFSET) : 0;
                // BUG REAL CORREGIDO: este tope de 64 (fijado aqui, no en
                // NemoFS ni en FAT) hacia que una carpeta con mas
                // archivos que eso los perdiera en silencio -- no solo
                // al listar, sino en cualquier operacion que dependiera
                // del listado, como copiar una carpeta entera desde el
                // explorador (solo se copiaban los primeros 64).
                static fat_dirent_t tmp[512];
                uint32_t cap = (max_entries < 512) ? max_entries : 512;
                uint32_t total = fat_list_dir(real_parent, tmp, cap);
                uint32_t to_write = (total < cap) ? total : cap;
                for (uint32_t i = 0; i < to_write; i++) {
                    uint8_t *entry = out + i * 40;
                    uint32_t type_val = tmp[i].is_dir ? NEMOFS_TYPE_DIR : NEMOFS_TYPE_FILE;
                    // El campo "inodo" en FAT lleva el CLUSTER de la
                    // entrada CON el desplazamiento (coherente con lo
                    // que devuelve SYS_FILE_OPEN para la misma
                    // entrada) -- asi el explorador puede pasarlo de
                    // vuelta como 'parent' para navegar DENTRO de una
                    // subcarpeta. Para archivos (no carpetas) esto no
                    // se usa para navegar, pero no hace daño llevarlo
                    // igualmente.
                    uint32_t inode_val = tmp[i].first_cluster + FAT_DIR_CLUSTER_OFFSET;
                    for (int b = 0; b < 4; b++) entry[0 + b] = (uint8_t)(inode_val >> (b * 8));
                    for (int b = 0; b < 4; b++) entry[4 + b] = (uint8_t)(type_val >> (b * 8));
                    for (int b = 0; b < 4; b++) entry[8 + b] = (uint8_t)(tmp[i].size >> (b * 8));
                    int j = 0;
                    while (tmp[i].name[j] != '\0' && j < 27) { entry[12 + j] = (uint8_t)tmp[i].name[j]; j++; }
                    entry[12 + j] = 0;
                }
                return (uint64_t)total;
            }

            static nemofs_dirent_t tmp[512];
            uint32_t cap = (max_entries < 512) ? max_entries : 512;
            uint32_t total = nemofs_list_dir(parent, tmp, cap);

            uint32_t to_write = (total < cap) ? total : cap;
            for (uint32_t i = 0; i < to_write; i++) {
                uint8_t *entry = out + i * 40;
                uint32_t inode_val = tmp[i].inode;
                uint32_t type_val = (uint32_t)tmp[i].type;
                uint32_t size_val = tmp[i].size;
                for (int b = 0; b < 4; b++) entry[0 + b] = (uint8_t)(inode_val >> (b * 8));
                for (int b = 0; b < 4; b++) entry[4 + b] = (uint8_t)(type_val >> (b * 8));
                for (int b = 0; b < 4; b++) entry[8 + b] = (uint8_t)(size_val >> (b * 8));
                int j = 0;
                while (tmp[i].name[j] != '\0' && j < 27) { entry[12 + j] = (uint8_t)tmp[i].name[j]; j++; }
                entry[12 + j] = 0;
            }

            return (uint64_t)total;
        }

        case SYS_DIR_CREATE: {
            const char *name = validate_str(a0, 256);
            if (!name) return (uint64_t)-1;
            uint32_t parent = (uint32_t)a1;
            uint32_t volume = (uint32_t)a2;
            if (volume == VOLUME_FAT) return (uint64_t)-1; // FAT v1 no tiene subcarpetas
            // Con rutas se pueden anidar: CreateDir "JUEGOS/NIVELES". Solo
            // crea la ULTIMA; las de en medio tienen que existir ya.
            char hoja[NEMOFS_MAX_NAME + 1];
            if (ruta_o_nombre(&name, &parent, hoja, sizeof(hoja)) < 0) return (uint64_t)-1;
            int32_t idx = nemofs_create(parent, name, NEMOFS_TYPE_DIR);
            return (uint64_t)(int64_t)idx;
        }

        case SYS_FILE_DELETE: {
            const char *name = validate_str(a0, 256);
            if (!name) return (uint64_t)-1;
            uint32_t parent = (uint32_t)a1;
            uint32_t volume = (uint32_t)a2;
            char hoja[NEMOFS_MAX_NAME + 1];
            int es_ruta = ruta_o_nombre(&name, &parent, hoja, sizeof(hoja));
            if (es_ruta < 0) return (uint64_t)-1;
            // Borrar por ruta en FAT no vale: fat_delete_file solo mira la
            // raiz, asi que "JUEGOS/x.dat" borraria otra cosa o nada.
            if (volume == VOLUME_FAT) return (!es_ruta && fat_delete_file(name)) ? 0 : (uint64_t)-1;
            return nemofs_delete(parent, name) ? 0 : (uint64_t)-1;
        }

        case SYS_FILE_RENAME: {
            // Solo NemoFS por ahora -- renombrar en FAT tocaria la
            // entrada de directorio directamente, algo que fat.c
            // todavia no ofrece como operacion propia.
            const char *old_name = validate_str(a0, 256);
            const char *new_name = old_name ? validate_str(a1, 256) : 0;
            if (!old_name || !new_name) return (uint64_t)-1;
            uint32_t parent = (uint32_t)a2;
            uint32_t volume = (uint32_t)a3;
            if (volume == VOLUME_FAT) return (uint64_t)-1;
            return nemofs_rename(parent, old_name, new_name) ? 0 : (uint64_t)-1;
        }

        case SYS_ICON_CATALOG_COUNT:
            return (uint64_t)icon_catalog_count();

        case SYS_DESKTOP_ICON_COUNT:
            return (uint64_t)(int64_t)wm_desktop_icon_count();

        case SYS_DESKTOP_ICON_GET: {
            int32_t index = (int32_t)a0;
            if (!validate_buf(a1, 16) || !validate_buf(a2, 32) || (a3 != 0 && !validate_buf(a3, 3 * sizeof(int32_t)))) return 0;
            char *out_label = (char *)a1;
            char *out_target = (char *)a2;
            int32_t *out_ints = (int32_t *)a3; // [icon_id, x, y]
            int32_t icon_id = 0, x = 0, y = 0;
            bool ok = wm_get_desktop_icon(index, out_label, 16, out_target, 32, &icon_id, &x, &y);
            if (ok && out_ints) { out_ints[0] = icon_id; out_ints[1] = x; out_ints[2] = y; }
            return ok ? 1 : 0;
        }

        case SYS_DESKTOP_ICON_ADD: {
            const char *label = validate_str(a0, 256);
            const char *target = label ? validate_str(a1, 256) : 0;
            if (!label || !target) return (uint64_t)-1;
            int32_t icon_id = (int32_t)a2;
            int32_t x = (int32_t)a3;
            int32_t y = (int32_t)a4;
            int32_t before = wm_desktop_icon_count();
            wm_add_desktop_icon_ex(label, target, icon_id, x, y);
            int32_t after = wm_desktop_icon_count();
            return (after > before) ? (uint64_t)(int64_t)before : (uint64_t)-1;
        }

        case SYS_DESKTOP_ICON_REMOVE:
            return wm_remove_desktop_icon((int32_t)a0) ? 0 : (uint64_t)-1;

        case SYS_DESKTOP_ICON_MOVE:
            return wm_move_desktop_icon((int32_t)a0, (int32_t)a1, (int32_t)a2) ? 0 : (uint64_t)-1;

        case SYS_DESKTOP_ICON_SET_GRAPHIC:
            return wm_set_desktop_icon_graphic((int32_t)a0, (int32_t)a1) ? 0 : (uint64_t)-1;

        case SYS_MENU_RELOAD:
            wm_menu_cargar();
            return 0;

        // a5 = ORIGEN: 32 bits bajos la carpeta, 32 altos el
        // volumen. a5 = 0 es exactamente lo de antes, NemoFS con la
        // busqueda de siempre, asi que quien no lo pase sigue igual. Ver
        // la nota de nimg_fuente_abrir().
        case SYS_DRAW_FILE_SCALED: {
            const char *nombre = validate_str(a0, 256);
            if (!nombre) return (uint64_t)-1;
            return archivo_escalado(nombre, (int32_t)a1,
                                    (uint32_t)(a2 >> 16), (uint32_t)(a2 & 0xFFFF),
                                    (uint32_t)(a3 >> 16), (uint32_t)(a3 & 0xFFFF),
                                    (uint32_t)a4, a5) ? 0 : (uint64_t)-1;
        }

        case SYS_MINIATURA_DOBLE: {
            const char *nombre = validate_str(a0, 256);
            if (!nombre) return (uint64_t)-1;
            return archivo_escalado_doble(nombre,
                        (int32_t)((a1 >> 16) & 0xFFFF), (uint32_t)(a2 >> 16), (uint32_t)(a2 & 0xFFFF), (uint32_t)(a4 >> 16),
                        (int32_t)(a1 & 0xFFFF),         (uint32_t)(a3 >> 16), (uint32_t)(a3 & 0xFFFF), (uint32_t)(a4 & 0xFFFF),
                        a5)
                   ? 0 : (uint64_t)-1;
        }

        // LoadImage con VOLUMEN y carpeta explicitos.
        //
        // Existe aparte y no como argumento de la 49 porque la 49 se llama
        // desde Nemo Basic, y ahi el generador solo pone x0: x1 llegaria
        // con lo que hubiera, y una imagen se cargaria del volumen que
        // tocara por casualidad. Mismo motivo por el que existe la 253
        // (SYS_LOAD_IMAGE_EN) en vez de haberle añadido un argumento.
        // ---- Red: bajar y desempaquetar ----------------
        case SYS_NET_BAJAR: {
            const char *ruta = validate_str(a0, 512);
            if (!ruta) return (uint64_t)-1;
            uint8_t ip[4] = { (uint8_t)a1, (uint8_t)(a1 >> 8),
                              (uint8_t)(a1 >> 16), (uint8_t)(a1 >> 24) };
            uint32_t carpeta = NEMOFS_ROOT_INODE;
            const char *nombre = 0;
            char hoja[NEMOFS_MAX_NAME + 1];
            if (a3) {
                nombre = validate_str(a3, 256);
                if (!nombre) return (uint64_t)-1;
                // Admite "DOCUMENTOS/pagina.nmz" -- si no, el archivo se
                // llamaria literalmente asi, colgando de la raiz, y
                // nadie lo encontraria despues.
                if (ruta_o_nombre(&nombre, &carpeta, hoja, sizeof hoja) < 0) return (uint64_t)-1;
            }
            return descarga_empezar(ip, (uint16_t)a2, ruta, carpeta, nombre) ? 0 : (uint64_t)-1;
        }

        case SYS_NET_ESTADO:
            return (uint64_t)descarga_estado() | ((uint64_t)descarga_codigo() << 8);

        case SYS_NET_BYTES:
            return (uint64_t)descarga_bytes() | ((uint64_t)descarga_total() << 32);

        case SYS_NET_MOTIVO: {
            uint32_t max = (uint32_t)a1;
            if (max == 0 || !validate_buf(a0, max)) return 0;
            const char *m = descarga_motivo();
            char *d = (char *)a0;
            uint32_t i = 0;
            while (m[i] && i + 1 < max) { d[i] = m[i]; i++; }
            d[i] = 0;
            return i;
        }

        case SYS_NMZ_ABRIR: {
            const char *paquete = validate_str(a0, 256);
            const char *carpeta = validate_str(a1, 256);
            if (!paquete || !carpeta) return (uint64_t)-1;

            uint32_t dir_paq = NEMOFS_ROOT_INODE;
            char hoja[NEMOFS_MAX_NAME + 1];
            if (ruta_o_nombre(&paquete, &dir_paq, hoja, sizeof hoja) < 0) return (uint64_t)-1;
            int32_t inodo = nemofs_find_child(dir_paq, paquete);
            if (inodo < 0) return (uint64_t)-1;
            int32_t tam = nemofs_file_size((uint32_t)inodo);
            if (tam <= 0) return (uint64_t)-1;

            // La carpeta de destino se crea si no existe: quien abre un
            // paquete quiere el resultado, no una lista de pasos
            // previos.
            uint32_t dir_dst = NEMOFS_ROOT_INODE;
            const char *nombre_dst = carpeta;
            if (ruta_o_nombre(&nombre_dst, &dir_dst, hoja, sizeof hoja) < 0) return (uint64_t)-1;
            int32_t destino = nemofs_find_child(dir_dst, nombre_dst);
            if (destino < 0) destino = nemofs_create(dir_dst, nombre_dst, NEMOFS_TYPE_DIR);
            else if (nemofs_type_by_inode((uint32_t)destino) != NEMOFS_TYPE_DIR) return (uint64_t)-1;
            if (destino < 0) return (uint64_t)-1;

            nmz_ctx_t ctx = { (uint32_t)inodo, (uint32_t)destino };
            nmz_io_t io = { nmz_leer_fs, nmz_crear_fs, nmz_escribir_fs, &ctx };

            char primero[NEMOFS_MAX_NAME + 1];
            int32_t n = nmz_desempaquetar(&io, (uint32_t)tam, primero, sizeof primero);
            if (n < 0) {
                uart_puts("nmz: "); uart_puts(nmz_motivo()); uart_puts("\n");
                return (uint64_t)-1;
            }
            if (a2 && a3 && validate_buf(a2, (uint32_t)a3)) {
                char *d = (char *)a2;
                uint32_t i = 0, max = (uint32_t)a3;
                while (primero[i] && i + 1 < max) { d[i] = primero[i]; i++; }
                d[i] = 0;
            }
            return (uint64_t)(int64_t)n;
        }

        // ---- Red: sockets UDP -------------------------
        case SYS_UDP_ABRIR:
            return (uint64_t)(int64_t)udps_abrir((uint16_t)a0);

        case SYS_UDP_CERRAR:
            udps_cerrar((int32_t)a0);
            return 0;

        case SYS_UDP_ENVIAR: {
            uint32_t len = (uint32_t)a4;
            if (udps_puerto((int32_t)a0) == 0) return (uint64_t)-1;
            if (len > UDPS_MAX_DATAGRAMA) return (uint64_t)-2;
            // Un datagrama vacio es legal, y entonces no hay buffer que
            // validar: exigirlo convertiria un "estoy aqui" sin datos en
            // un error.
            if (len > 0 && !validate_buf(a3, len)) return (uint64_t)-1;
            const char *texto = validate_str(a1, 64);
            if (!texto) return (uint64_t)-4;
            uint8_t ip[4];
            if (!ip_desde_texto(texto, ip)) return (uint64_t)-4;
            if (!net_enviar_udp(ip, (uint16_t)a2, udps_puerto((int32_t)a0),
                                (const uint8_t *)a3, len))
                return (uint64_t)-3;   // aun no salio: falta el ARP
            return (uint64_t)len;
        }

        case SYS_UDP_RECIBIR: {
            uint32_t max = (uint32_t)a2;
            if (max == 0 || !validate_buf(a1, max)) return (uint64_t)-1;
            return (uint64_t)(int64_t)udps_recibir((int32_t)a0, (uint8_t *)a1, max);
        }

        case SYS_UDP_ORIGEN: {
            uint8_t ip[4]; uint16_t puerto;
            udps_origen((int32_t)a0, ip, &puerto);
            uint32_t escrito = 0;
            // Con a1 a cero solo se pregunta por el puerto, que es lo que
            // hace UdpFromPort: asi no hay que reservar un buffer para
            // tirarlo.
            if (a1 && (uint32_t)a2 >= IP_TEXTO_MAX && validate_buf(a1, (uint32_t)a2))
                escrito = ip_a_texto(ip, (char *)a1, (uint32_t)a2);
            return (uint64_t)escrito | ((uint64_t)puerto << 32);
        }

        case SYS_UDP_PERDIDOS:
            return (uint64_t)udps_perdidos((int32_t)a0);

        case SYS_UDP_PENDIENTES:
            return (uint64_t)udps_pendientes((int32_t)a0);

        case SYS_UDP_PUERTO:
            return (uint64_t)udps_puerto((int32_t)a0);

        // ---- Red: pedir por HTTP a memoria ------------
        case SYS_NET_PEDIR: {
            const char *ruta = validate_str(a0, 512);
            if (!ruta) return (uint64_t)-1;
            const char *texto = validate_str(a1, 64);
            if (!texto) return (uint64_t)-4;
            uint8_t ip[4];
            if (!ip_desde_texto(texto, ip)) return (uint64_t)-4;
            const uint8_t *cuerpo = 0;
            uint32_t cuerpo_len = 0;
            if (a3) {
                cuerpo_len = (uint32_t)a4;
                // Un POST con cuerpo vacio es legal, y entonces no hay
                // buffer que validar.
                if (cuerpo_len > 0 && !validate_buf(a3, cuerpo_len)) return (uint64_t)-1;
                cuerpo = (const uint8_t *)a3;
            }
            return descarga_pedir(ip, (uint16_t)a2, ruta, cuerpo, cuerpo_len)
                   ? 0 : (uint64_t)-1;
        }

        case SYS_NET_CUERPO: {
            uint32_t max = (uint32_t)a1;
            if (max == 0 || !validate_buf(a0, max)) return 0;
            return (uint64_t)descarga_vista((uint8_t *)a0, max);
        }

        case SYS_NET_LOCAL: {
            uint32_t max = (uint32_t)a1;
            if (max < IP_TEXTO_MAX || !validate_buf(a0, max)) return 0;
            uint8_t ip[4];
            switch ((uint32_t)a2) {
                case 1:  net_get_mascara(ip); break;
                case 2:  net_get_pasarela(ip); break;
                case 3:  net_get_dns(ip);      break;
                default: net_get_ip(ip);       break;
            }
            return (uint64_t)ip_a_texto(ip, (char *)a0, max);
        }

        case SYS_NET_LISTA:
            return net_ready() ? 1 : 0;

        case SYS_LOAD_IMAGE_VOL: {
            const char *n = validate_str(a0, 256);
            if (!n) return (uint64_t)-1;
            return (uint64_t)(int64_t)image_load_origen(n, ((uint64_t)a2 << 32) | (uint32_t)a1);
        }

        case SYS_SET_WALLPAPER: {
            if (a0 == 0) { wm_fondo_set(0, 0, 0); fondo_olvidar(); return 0; }
            const char *nombre = validate_str(a0, 256);
            if (!nombre) return (uint64_t)-1;
            if (!fondo_poner(nombre, (uint32_t)a1)) return (uint64_t)-1;
            fondo_recordar(nombre, (uint32_t)a1);
            return 0;
        }

        case SYS_DESKTOP_ICON_SET_LABEL: {
            const char *l = validate_str(a1, 16);
            if (!l) return (uint64_t)-1;
            return wm_set_desktop_icon_label((int32_t)a0, l) ? 0 : (uint64_t)-1;
        }

        case SYS_DESKTOP_SAVE:
            wm_save_desktop_icons();
            return 0;

        case SYS_DESKTOP_ICON_SCALE_GET:
            return (uint64_t)(int64_t)wm_get_icon_scale();

        case SYS_DESKTOP_ICON_SCALE_SET:
            wm_set_icon_scale((int32_t)a0);
            wm_save_desktop_icons();
            return 0;

        case SYS_SCREEN_RES_GET_PENDING: {
            int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, "SCREEN.CFG");
            if (idx < 0) return 0;
            uint8_t buf[8];
            if (nemofs_read_file((uint32_t)idx, buf, sizeof(buf)) != 8) return 0;
            uint32_t w = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
            uint32_t h = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
            return ((uint64_t)w << 32) | h;
        }

        case SYS_SCREEN_RES_SET_PENDING: {
            uint32_t w = (uint32_t)a0, h = (uint32_t)a1;
            if (w == 0 || h == 0) return (uint64_t)-1;
            int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, "SCREEN.CFG");
            if (idx < 0) idx = nemofs_create(NEMOFS_ROOT_INODE, "SCREEN.CFG", NEMOFS_TYPE_FILE);
            if (idx < 0) return (uint64_t)-1;
            uint8_t buf[8];
            for (int b = 0; b < 4; b++) buf[b] = (uint8_t)(w >> (b * 8));
            for (int b = 0; b < 4; b++) buf[4 + b] = (uint8_t)(h >> (b * 8));
            return nemofs_write_file((uint32_t)idx, buf, sizeof(buf)) ? 0 : (uint64_t)-1;
        }

        case SYS_NEMOFS_TYPE_BY_INODE:
            return (uint64_t)(int64_t)nemofs_type_by_inode((uint32_t)a0);

        case SYS_TASK_LIST:
            if (!validate_buf(a0, (uint64_t)(uint32_t)a1 * 48)) return 0; // 48 bytes por entrada, ver task_list_dump
            return (uint64_t)task_list_dump((uint8_t *)a0, (uint32_t)a1);

        case SYS_TASK_FIELD: {
            uint32_t vivas = task_volcar();
            uint32_t i = (uint32_t)a0;
            if (i >= vivas) return (uint64_t)-1;
            uint32_t off;
            switch ((uint32_t)a1) {
                case 0: off = 0; break;    // slot
                case 1: off = 4; break;    // ventana (puede ser -1 de verdad)
                case 2: off = 8; break;    // turnos del planificador
                default: return (uint64_t)-1;
            }
            int32_t v;
            __builtin_memcpy(&v, task_volcado + i * TASK_ENTRADA + off, sizeof v);
            return (uint64_t)(int64_t)v;   // con signo: la ventana puede ser -1
        }

        case SYS_TASK_NAME: {
            uint32_t max = (uint32_t)a2;
            if (max == 0 || !validate_buf(a1, max)) return (uint64_t)-1;
            uint32_t vivas = task_volcar();
            uint32_t i = (uint32_t)a0;
            if (i >= vivas) return (uint64_t)-1;
            const char *nombre = (const char *)(task_volcado + i * TASK_ENTRADA + TASK_OFF_NOMBRE);
            char *out = (char *)(uintptr_t)a1;
            uint32_t k = 0;
            // Tope doble: lo que cabe en el buffer del programa, y los 32
            // bytes del campo -- que pueden venir sin \0 si el nombre los
            // llena justos.
            while (k + 1 < max && k < 32 && nombre[k] != '\0') { out[k] = nombre[k]; k++; }
            out[k] = '\0';
            return (uint64_t)k;
        }

        case SYS_TASK_KILL:
            return task_kill_by_slot((int32_t)a0) ? 1 : 0;

        case SYS_DISK_USAGE: {
            uint32_t total, used;
            nemofs_disk_usage(&total, &used);
            return ((uint64_t)total << 32) | (uint64_t)used;
        }

        case SYS_RAM_TASKS: {
            uint32_t en_uso = task_count_used();
            return ((uint64_t)en_uso << 32) | (uint64_t)MAX_TASKS;
        }

        case SYS_GPIO_MODO:     return (uint64_t)(int64_t)gpio_modo((int32_t)a0, (int32_t)a1);
        case SYS_GPIO_ESCRIBIR: return (uint64_t)(int64_t)gpio_escribir((int32_t)a0, (int32_t)a1);
        case SYS_GPIO_LEER:     return (uint64_t)(int64_t)gpio_leer((int32_t)a0);
        case SYS_GPIO_PWM:      return (uint64_t)(int64_t)gpio_pwm((int32_t)a0, (int32_t)a1, (int32_t)a2);
        case SYS_SPI: {
            if (a2 == 0 || a2 > SPI_MAX || !validate_buf(a1, a2)) return (uint64_t)(int64_t)SPI_ERR_ARG;
            if (a4 != 0 && !validate_buf(a4, a2)) return (uint64_t)(int64_t)SPI_ERR_ARG;
            return (uint64_t)(int64_t)spi_transferir((uint32_t)a0, (uint32_t)a5, (uint32_t)a3,
                                                     (const uint8_t *)(uintptr_t)a1, (uint8_t *)(uintptr_t)a4, (uint32_t)a2);
        }
        case SYS_I2C_ESCRIBIR:
        case SYS_I2C_LEER: {
            if (a2 == 0 || a2 > I2C_MAX || !validate_buf(a1, a2)) return (uint64_t)(int64_t)I2C_ERR_ARG;
            uint8_t *buf = (uint8_t *)(uintptr_t)a1;
            int32_t r = (num == SYS_I2C_LEER) ? i2c_leer((uint32_t)a0, buf, (uint32_t)a2)
                                              : i2c_escribir((uint32_t)a0, buf, (uint32_t)a2);
            return (uint64_t)(int64_t)r;
        }

        case SYS_MEM_PEDIR:
            return task_pedir_memoria(a0);

        case SYS_RAM_POOL: {
            // En KB (antes bytes: con mas de 4 GB de reserva no cabian en
            // 32 bits). nemo_sistema.lua lo devuelve en bytes, como antes.
            uint32_t usado, total;
            task_pool_usage_kb(&usado, &total);
            return ((uint64_t)usado << 32) | (uint64_t)total;
        }

        case SYS_RAM_HEAP: {
            uint64_t usado = (uint64_t)kheap_used();
            uint64_t total = usado + (uint64_t)kheap_free();
            return (usado << 32) | total;
        }

        case SYS_CPU_NOMBRE: {
            if (!validate_buf(a0, a1)) return 0;
            return (uint64_t)cpu_nombre((char *)a0, (uint32_t)a1);
        }

        case SYS_CPU_DATOS: {
            uint64_t nucleos = (uint64_t)smp_nucleos_en_marcha();
            return (nucleos << 32) | (uint64_t)cpu_mhz();
        }

        case SYS_RAM_FISICA:
            return memoria_total();

        case SYS_RAM_NUCLEO: {
            // Entre la entrada del kernel y el final de su .bss: el sitio
            // que ocupa de verdad en RAM, incluidos los arrays estaticos
            // grandes (el heap de 64MB, la reserva de tareas, los lienzos
            // de las ventanas...), que viven en .bss y no en el archivo.
            extern char _start[], __bss_end[];
            return (uint64_t)(__bss_end - _start);
        }

        case SYS_FILE_CLIPBOARD_SET: {
            const char *name = validate_str(a0, 256);
            if (!name) return 0;
            uint32_t parent = (uint32_t)a1;
            uint32_t volume = (uint32_t)a2;
            int i = 0;
            while (name[i] != '\0' && i < 27) { file_clip_name[i] = name[i]; i++; }
            file_clip_name[i] = '\0';
            file_clip_parent = parent;
            file_clip_volume = volume;
            file_clip_used = true;
            return 0;
        }

        case SYS_FILE_CLIPBOARD_GET: {
            if (!file_clip_used) return (uint64_t)-1;
            uint32_t max_len = (uint32_t)a1;
            if (max_len == 0 || !validate_buf(a0, max_len)) return (uint64_t)-1;
            char *out = (char *)a0;
            uint32_t i = 0;
            while (file_clip_name[i] != '\0' && i < max_len - 1) { out[i] = file_clip_name[i]; i++; }
            out[i] = '\0';
            return ((uint64_t)file_clip_parent << 32) | file_clip_volume;
        }

        case SYS_DEBUG_LOG: {
            const char *m = validate_str(a0, 4096);
            uart_puts("DEBUG: ");
            if (m) uart_puts(m);
            uart_puts("\n");
            return 0;
        }

        // -- Graficos y ventana --
        //
        // NOTA SOBRE EL REDIBUJADO.
        //
        // Ninguna de las syscalls que dibujan EN EL LIENZO de una ventana pide
        // redibujado, y es a proposito: no hace falta. content_put_pixel marca
        // la ventana como cambiada, y wm_publicar_contenido —-que corre en cada
        // vuelta del kernel-— publica el lienzo y ensucia EL RECTANGULO DE ESA
        // VENTANA, que es exactamente lo que ha cambiado.
        //
        // Antes cada una de ellas llamaba a wm_request_redraw(), que ensucia la
        // PANTALLA ENTERA: cientos de veces por repintado de un programa. Con
        // eso, el rectangulo sucio acababa siendo siempre toda la pantalla y
        // todo el trabajo de recortar no servia para nada. Se vio en las
        // medidas: fb_present se quedaba clavado en 3,07 ms —-una pantalla
        // completa-— hiciera el programa lo que hiciera, incluso repintando
        // una ventana pequeña.
        //
        // Las que SI piden redibujado completo son las que cambian el MARCO o
        // la geometria (titulo, botones, crear ventana, modo grafico): eso lo
        // pinta el compositor, no el lienzo, y ademas pasa muy de vez en cuando.

        case SYS_DRAW_RECT: {
            if (g_draw_target_image >= 0) {
                // Sin menu_off ni busqueda de ventana -- las imagenes
                // no tienen barra de menu ni son "ventanas".
                imgbuf_fill_rect(g_draw_target_image, (int32_t)a0 + g_origin_x, (int32_t)a1 + g_origin_y,
                                  (int32_t)a2, (int32_t)a3, (uint32_t)a4);
                return 0;
            }
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            int32_t wx, wy;
            uint32_t ww, wh;
            if (!wm_get_window_client_rect(current_window, &wx, &wy, &ww, &wh)) return (uint64_t)-1;
            (void)wx; (void)wy;

            uint32_t menu_off = gadgets_menubar_height(current_window);

            int32_t x = (int32_t)a0 + g_origin_x, y = (int32_t)a1 + g_origin_y + (int32_t)menu_off;
            uint32_t w = (uint32_t)a2, h = (uint32_t)a3;
            uint32_t color = (uint32_t)a4;

            int32_t cx0, cy0, cx1, cy1;
            get_clip_bounds(ww, wh, menu_off, &cx0, &cy0, &cx1, &cy1);
            if (x < cx0) { int32_t d = cx0 - x; w = (w > (uint32_t)d) ? w - (uint32_t)d : 0; x = cx0; }
            if (y < cy0) { int32_t d = cy0 - y; h = (h > (uint32_t)d) ? h - (uint32_t)d : 0; y = cy0; }
            if (x + (int32_t)w > cx1) w = (cx1 > x) ? (uint32_t)(cx1 - x) : 0;
            if (y + (int32_t)h > cy1) h = (cy1 > y) ? (uint32_t)(cy1 - y) : 0;

            wm_content_fill_rect(current_window, (uint32_t)x, (uint32_t)y, w, h, color);
            // (no se pide nada: el lienzo se publica solo -- ver la nota de arriba)
            return 0;
        }

        case SYS_DRAW_TEXT: {
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            int32_t wx, wy;
            uint32_t ww, wh;
            if (!wm_get_window_client_rect(current_window, &wx, &wy, &ww, &wh)) return (uint64_t)-1;
            (void)wx; (void)wy;

            uint32_t menu_off = gadgets_menubar_height(current_window);

            int32_t x = (int32_t)a0 + g_origin_x, y = (int32_t)a1 + g_origin_y + (int32_t)menu_off;
            const char *str = validate_str(a2, 4096);
            if (!str) return (uint64_t)-1;
            uint32_t color = (uint32_t)a3;

            int32_t cx0, cy0, cx1, cy1;
            get_clip_bounds(ww, wh, menu_off, &cx0, &cy0, &cx1, &cy1);
            if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1) return (uint64_t)-1;

            if (g_cara) {
                // Fuente proporcional (fonts.c): antialiasing, UTF-8, negrita y
                // cursiva de verdad. (x, y) = esquina superior izquierda de la linea.
                dibujo_ctx_t d = { current_window, cx0, cy0, cx1, cy1 };
                uint32_t ancho = fonts_dibujar(g_cara, x, y, str, color, ctx_get_pixel, ctx_put_pixel, &d);
                if (g_current_font >= 0 && fonts[g_current_font].underlined) {
                    int32_t uy = y + fonts_ascent(g_cara) + 1;
                    uint32_t grosor = fonts_alto_linea(g_cara) / 14; if (grosor < 1) grosor = 1;
                    for (uint32_t k = 0; k < grosor; k++)
                        for (int32_t px = x; px < x + (int32_t)ancho; px++) ctx_put_pixel(&d, px, uy + (int32_t)k, color);
                }
                // (no se pide nada: el lienzo se publica solo -- ver la nota de arriba)
                return 0;
            }
            wm_content_draw_string(current_window, (uint32_t)x, (uint32_t)y, str, color, g_font_scale);
            if (g_font_bold) {
                // Negrita "falsa": se dibuja una segunda vez desplazada
                // 1 pixel a la derecha -- efecto real de engrosado,
                // sin necesitar un juego de glifos en negrita aparte.
                wm_content_draw_string(current_window, (uint32_t)x + 1, (uint32_t)y, str, color, g_font_scale);
            }
            // (no se pide nada: el lienzo se publica solo -- ver la nota de arriba)
            return 0;
        }

        case SYS_TEXT_WIDTH: {
            const char *t = validate_str(a0, 4096);
            if (!t) return 0;
            if (g_cara) return fonts_ancho_texto(g_cara, t);
            return text_width(t, g_font_scale);
        }

        case SYS_FONT_ADVANCES: {
            int32_t idx = (int32_t)a0 - 1;
            if (!validate_buf(a1, 256)) return 0;
            uint8_t *out = (uint8_t *)a1;
            const nfnt_cara_t *cara = (idx >= 0 && idx < MAX_FONTS && fonts[idx].used) ? fonts[idx].cara : 0;
            uint32_t escala = 1;
            if (!cara && idx >= 0 && idx < MAX_FONTS && fonts[idx].used) {
                int32_t sc = (fonts[idx].height + FONT_HEIGHT / 2) / FONT_HEIGHT; escala = (uint32_t)(sc < 1 ? 1 : sc);
            }
            for (uint32_t cp = 0; cp < 256; cp++) {
                if (cara) { uint32_t av = fonts_avance(cara, cp); out[cp] = (uint8_t)(av > 255 ? 255 : av); }
                else out[cp] = (cp >= 32 && cp < 127) ? (uint8_t)((FONT_WIDTH + 1) * escala) : 0;
            }
            // (ascent << 16) | alto_linea -- las dos metricas que hacen falta
            // para maquetar: donde esta la base y cuanto ocupa la linea.
            if (cara) return ((uint64_t)(uint32_t)fonts_ascent(cara) << 16) | fonts_alto_linea(cara);
            return ((uint64_t)(FONT_HEIGHT * escala) << 16) | (FONT_HEIGHT * escala + 3 * escala);
        }

        case SYS_DRAW_ICON: {
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            uint32_t menu_off = gadgets_menubar_height(current_window);
            int32_t x = (int32_t)a0, y = (int32_t)a1 + (int32_t)menu_off;
            int icon_id = (int)a2;
            // a3=0 (lo que mandaban TODOS los programas antes de que
            // esto existiera) se trata igual que a3=1 -- tamaño normal,
            // compatible con cualquier llamada ya escrita.
            uint32_t scale = (uint32_t)a3;
            if (scale < 1) scale = 1;
            const uint8_t *rgba = icon_get_rgba(icon_id);
            if (!rgba || x < 0 || y < (int32_t)menu_off) return (uint64_t)-1;

            wm_content_blit_icon_scaled(current_window, (uint32_t)x, (uint32_t)y, ICON_SIZE, scale, rgba);
            // (no se pide nada: el lienzo se publica solo -- ver la nota de arriba)
            return 0;
        }

        case SYS_GET_WINDOW_SIZE: {
            // Con un ImageBuffer activo, "el tamaño" es el de la imagen
            // destino. Si no, Cls (que pide el tamaño y luego rellena)
            // creeria tener 640x480 dentro de una imagen de 256x256.
            if (g_draw_target_image >= 0)
                return ((uint64_t)images[g_draw_target_image].width << 32) | images[g_draw_target_image].height;
            if (current_window < 0) return 0;
            int32_t wx, wy;
            uint32_t ww, wh;
            if (!wm_get_window_client_rect(current_window, &wx, &wy, &ww, &wh)) return 0;
            (void)wx; (void)wy;
            uint32_t menu_off = gadgets_menubar_height(current_window);
            uint32_t usable_h = (wh > menu_off) ? wh - menu_off : 0;
            return ((uint64_t)ww << 32) | usable_h;
        }

        case SYS_GET_MOUSE: {
            if (current_window != wm_get_focused_window()) return (uint64_t)-1;
            int32_t wx, wy;
            uint32_t ww, wh;
            if (!wm_get_window_client_rect(current_window, &wx, &wy, &ww, &wh)) return (uint64_t)-1;
            uint32_t menu_off = gadgets_menubar_height(current_window);

            int32_t lx = mouse_x() - wx;
            int32_t ly = mouse_y() - wy - (int32_t)menu_off;
            uint32_t usable_h = (wh > menu_off) ? wh - menu_off : 0;
            if (lx < 0 || ly < 0 || (uint32_t)lx >= ww || (uint32_t)ly >= usable_h) return (uint64_t)-1;

            uint64_t buttons = (mouse_left_down() ? 1u : 0u) | (mouse_right_down() ? 2u : 0u) | (mouse_middle_down() ? 4u : 0u);
            return ((uint64_t)(lx & 0xFFFF) << 32) | ((uint64_t)(ly & 0xFFFF) << 16) | buttons;
        }

        case SYS_GET_MOUSE_WHEEL:
            // La rueda se CONSUME al leerla: sin el foco, un programa de
            // fondo le robaba el desplazamiento al que el usuario tiene
            // delante (con dos visores abiertos, el de delante no se movia).
            if (!tarea_tiene_foco()) return 0;
            return (uint64_t)(int64_t)mouse_wheel_delta();

        case SYS_GRAPHICS_MODE: {
            // Graphics(ancho, alto) -- estilo clasico: sin modo de
            // eventos, la X cierra la ventana directamente.
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            uint32_t w = (uint32_t)a0, h = (uint32_t)a1;
            const char *name; uint64_t base;
            task_get_debug_info(&name, &base);
            (void)base;
            wm_configure_window(current_window, name, 60, 60, w, h);
            wm_request_redraw();
            return (uint64_t)(int64_t)current_window;
        }

        case SYS_CONSOLE_BUSY:
            // "Otra" tarea: si el propietario soy yo mismo no cuenta.
            return (uint64_t)((g_input_owner >= 0 && g_input_owner != task_get_current_slot()) ? 1 : 0);

        case SYS_READ_LINE: {
            // Leer una linea completa: espera teclas, las hace eco,
            // gestiona el retroceso, y termina con Enter.
            //
            // Cede el turno en cada vuelta de espera (task_yield). El
            // planificador es cooperativo: sin eso, esperar a que el
            // usuario escriba congelaria el sistema entero.
            uint32_t max_len = (uint32_t)a1;
            if (max_len == 0 || !validate_buf(a0, max_len)) return (uint64_t)-1;
            char *out = (char *)a0;

            // La ventana donde leer y hacer eco es la de CONSOLA, no
            // 'current_window': un programa lanzado desde la shell no
            // tiene ventana propia, escribe en la de la shell. Es la
            // misma que usa SYS_WRITE_STRING.
            //
            // Con current_window la primera version devolvia -1
            // siempre en programas de consola: Input$ no esperaba
            // nada y el bucle del programa daba diez vueltas seguidas
            // con la cadena vacia.
            int32_t consola = task_get_console_window();
            if (consola < 0) return (uint64_t)-1;

            // Tomar el teclado en exclusiva mientras dure la lectura
            g_input_owner = task_get_current_slot();

            uint32_t len = 0;
            for (;;) {
                int32_t wx, wy;
                uint32_t ww, wh;
                if (!wm_get_window_client_rect(consola, &wx, &wy, &ww, &wh)) {
                    g_input_owner = -1;    // soltar SIEMPRE, tambien al fallar
                    return (uint64_t)-1;   // la ventana se cerro mientras escribia
                }

                char c;
                if (consola == wm_get_focused_window() && input_read_char(&c)) {
                    if (c == '\n' || c == '\r') {
                        console_queue_write(consola, "\n");
                        break;
                    }
                    if (c == 8 || c == 127) {          // retroceso
                        if (len > 0) {
                            len--;
                            // Un solo "\b": quien dibuja la consola lo
                            // entiende como "quita el ultimo caracter".
                            // El truco clasico "\b \b" (retroceder,
                            // pintar un espacio, retroceder otra vez)
                            // es para terminales que solo saben mover
                            // el cursor; aqui sobra y ademas haria
                            // parpadear un espacio.
                            console_queue_write(consola, "\b");
                        }
                        continue;
                    }
                    if (c >= 32 && len + 1 < max_len) {  // imprimible y cabe
                        out[len++] = c;
                        char eco[2] = { c, '\0' };
                        console_queue_write(consola, eco);
                    }
                    continue;
                }
                // Esperar al siguiente latido: el teclado solo se lee una vez por
                // latido (turno del nucleo 0), asi que mirar antes no sirve de nada
                // y solo gasta vueltas. Ver task_dormir_hasta.
                task_dormir_hasta(timer_get_ticks() + 1);
            }
            g_input_owner = -1;    // soltar el teclado
            out[len] = '\0';
            return (uint64_t)len;
        }

        case SYS_DRAW_LINE: {
            // Recta por Bresenham ENTERO: sin division, sin coma
            // flotante y sin raiz cuadrada -- solo sumas, restas y
            // comparaciones. Cada pixel se pinta como un rectangulo de
            // 1x1, igual que Plot, asi que no hace falta ninguna
            // rutina nueva en wm.c.
            //
            // Antes esto no existia y el compilador de Nemo-Blitz
            // generaba el bucle entero en el propio programa: una
            // syscall POR PIXEL (una diagonal de 300 pixeles eran 300
            // llamadas al sistema). Aqui es una sola llamada, y ademas
            // queda disponible para Lua y para cualquier otro
            // programa.
            int32_t lx0 = (int32_t)a0, ly0 = (int32_t)a1;
            int32_t lx1 = (int32_t)a2, ly1 = (int32_t)a3;
            uint32_t color = (uint32_t)a4;

            if (g_draw_target_image >= 0) {
                int32_t x = lx0 + g_origin_x, y = ly0 + g_origin_y;
                int32_t xt = lx1 + g_origin_x, yt = ly1 + g_origin_y;
                int32_t dx = xt > x ? xt - x : x - xt;
                int32_t dy = yt > y ? -(yt - y) : -(y - yt);
                int32_t sx = x < xt ? 1 : -1, sy = y < yt ? 1 : -1;
                int32_t err = dx + dy;
                for (;;) {
                    imgbuf_fill_rect(g_draw_target_image, x, y, 1, 1, color);
                    if (x == xt && y == yt) break;
                    int32_t e2 = err * 2;
                    if (e2 >= dy) { err += dy; x += sx; }
                    if (e2 <= dx) { err += dx; y += sy; }
                }
                return 0;
            }

            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            int32_t wx, wy;
            uint32_t ww, wh;
            if (!wm_get_window_client_rect(current_window, &wx, &wy, &ww, &wh)) return (uint64_t)-1;
            (void)wx; (void)wy;
            uint32_t menu_off = gadgets_menubar_height(current_window);

            int32_t x = lx0 + g_origin_x, y = ly0 + g_origin_y + (int32_t)menu_off;
            int32_t xt = lx1 + g_origin_x, yt = ly1 + g_origin_y + (int32_t)menu_off;

            int32_t cx0, cy0, cx1, cy1;
            get_clip_bounds(ww, wh, menu_off, &cx0, &cy0, &cx1, &cy1);

            int32_t dx = xt > x ? xt - x : x - xt;
            int32_t dy = yt > y ? -(yt - y) : -(y - yt);
            int32_t sx = x < xt ? 1 : -1, sy = y < yt ? 1 : -1;
            int32_t err = dx + dy;
            for (;;) {
                if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1) {
                    wm_content_fill_rect(current_window, (uint32_t)x, (uint32_t)y, 1, 1, color);
                }
                if (x == xt && y == yt) break;
                int32_t e2 = err * 2;
                if (e2 >= dy) { err += dy; x += sx; }
                if (e2 <= dx) { err += dx; y += sy; }
            }
            // (no se pide nada: el lienzo se publica solo -- ver la nota de arriba)
            return 0;
        }

        case SYS_DRAW_OVAL: {
            // Ovalo RELLENO, rasterizado fila a fila: para cada fila,
            // (dx/rx)^2 + (dy/ry)^2 = 1  =>  dx = rx*sqrt(ry^2-dy^2)/ry,
            // y se dibuja como un rectangulo de 1 pixel de alto -- asi
            // reutilizamos wm_content_fill_rect, sin necesitar ninguna
            // rutina nueva de "pintar un pixel" en wm.c.
            if (g_draw_target_image >= 0) {
                imgbuf_fill_oval(g_draw_target_image, (int32_t)a0 + g_origin_x, (int32_t)a1 + g_origin_y,
                                  (int32_t)a2, (int32_t)a3, (uint32_t)a4);
                return 0;
            }
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            int32_t wx, wy;
            uint32_t ww, wh;
            if (!wm_get_window_client_rect(current_window, &wx, &wy, &ww, &wh)) return (uint64_t)-1;
            (void)wx; (void)wy;
            uint32_t menu_off = gadgets_menubar_height(current_window);

            int32_t bx = (int32_t)a0 + g_origin_x;
            int32_t by = (int32_t)a1 + g_origin_y + (int32_t)menu_off;
            int32_t bw = (int32_t)a2, bh = (int32_t)a3;
            uint32_t color = (uint32_t)a4;

            if (bw <= 0 || bh <= 0) return 0;
            int32_t rx = bw / 2, ry = bh / 2;
            if (rx == 0 || ry == 0) return 0;
            int32_t cx = bx + rx, cy = by + ry;

            int32_t cx0, cy0, cx1, cy1;
            get_clip_bounds(ww, wh, menu_off, &cx0, &cy0, &cx1, &cy1);

            for (int32_t dy = -ry; dy <= ry; dy++) {
                uint32_t inner = (uint32_t)(ry * ry - dy * dy);
                int32_t dx = (int32_t)(((uint64_t)rx * isqrt_u32(inner)) / (uint32_t)ry);
                int32_t row_y = cy + dy;
                if (row_y < cy0 || row_y >= cy1) continue;
                int32_t left = cx - dx;
                uint32_t span_w = (uint32_t)(dx * 2 + 1);
                if (left < cx0) { int32_t d = cx0 - left; span_w = (span_w > (uint32_t)d) ? span_w - (uint32_t)d : 0; left = cx0; }
                if (left + (int32_t)span_w > cx1) span_w = (cx1 > left) ? (uint32_t)(cx1 - left) : 0;
                if (span_w > 0) wm_content_fill_rect(current_window, (uint32_t)left, (uint32_t)row_y, span_w, 1, color);
            }
            // (no se pide nada: el lienzo se publica solo -- ver la nota de arriba)
            return 0;
        }

        case SYS_SET_IMAGE_BUFFER: {
            // Si el handle no es una imagen VALIDA, ni un Canvas
            // valido, volvemos a dibujar en la ventana (sin Origin ni
            // Viewport aplicados) -- asi ImageBuffer(BackBuffer())/
            // CanvasBuffer(cualquier_cosa_invalida) funcionan para
            // "restaurar", igual que antes.
            int32_t val = (int32_t)a0;
            g_draw_target_image = -1;
            g_draw_target_canvas = -1;
            if (val >= CANVAS_BUFFER_OFFSET) {
                int32_t canvas_id = val - CANVAS_BUFFER_OFFSET;
                int32_t cx, cy; uint32_t cw, ch;
                if (gadget_is_canvas(canvas_id) && gadget_get_rect(canvas_id, &cx, &cy, &cw, &ch)) {
                    g_draw_target_canvas = canvas_id;
                    g_origin_x = cx;
                    g_origin_y = cy;
                    g_viewport_active = true;
                    g_viewport_x = cx;
                    g_viewport_y = cy;
                    g_viewport_w = cw;
                    g_viewport_h = ch;
                } else {
                    g_origin_x = 0; g_origin_y = 0; g_viewport_active = false;
                }
            } else if (val >= 0 && val < MAX_IMAGES && images[val].used) {
                g_draw_target_image = val;
            } else {
                g_origin_x = 0; g_origin_y = 0; g_viewport_active = false;
            }
            return 0;
        }

        case SYS_RTC_NOW:
            return rtc_unix_timestamp();

        case SYS_RTC_CIVIL: {
            int y, mo, d, h, mi, s;
            rtc_to_civil(rtc_unix_timestamp(), &y, &mo, &d, &h, &mi, &s);
            return ((uint64_t)(uint16_t)y << 48) | ((uint64_t)(uint8_t)mo << 40) |
                   ((uint64_t)(uint8_t)d << 32) | ((uint64_t)(uint8_t)h << 24) |
                   ((uint64_t)(uint8_t)mi << 16) | ((uint64_t)(uint8_t)s << 8);
        }

        case SYS_SET_VIEWPORT: {
            uint32_t w = (uint32_t)a2, h = (uint32_t)a3;
            if (w == 0 || h == 0) {
                g_viewport_active = false;
            } else {
                g_viewport_active = true;
                g_viewport_x = (int32_t)a0;
                g_viewport_y = (int32_t)a1;
                g_viewport_w = w;
                g_viewport_h = h;
            }
            return 0;
        }

        case SYS_READ_FILE_READ_BYTES:
            if (!validate_buf(a1, a2)) return 0;
            return readfile_read_bytes((int32_t)a0, (uint8_t *)a1, (uint32_t)a2);

        case SYS_GET_MOUSE_Z:
            return (uint64_t)(int64_t)mouse_wheel_total();

        case SYS_FLUSH_MOUSE:
            if (!tarea_tiene_foco()) return 0;   // ver SYS_FLUSH_KEYS
            input_flush_mouse();
            return 0;

        case SYS_WINDOW_BOTONES: {
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            wm_set_window_buttons(current_window, a0 == 0, a1 == 0, a2 == 0);
            return 0;
        }

        case SYS_SET_TITLE: {
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            {
                const char *t = validate_str(a0, 256);
                if (t) wm_set_title(current_window, t);
            }
            wm_request_redraw();
            return 0;
        }

        case SYS_FREE_TIMER:
            gadget_free_timer((int32_t)a0);
            return 0;

        case SYS_TIMER_READY:
            return gadget_timer_ready((int32_t)a0) ? 1 : 0;

        case SYS_TIMER_CONSUME:
            gadget_timer_consume((int32_t)a0);
            return 0;

        // Teclado y botones del raton: SOLO para la tarea cuya ventana
        // tiene el foco.
        //
        // Antes cualquier programa veia cualquier tecla, tuviera o no el
        // foco: KeyDown leia el estado global del teclado. Con dos juegos
        // abiertos, las flechas movian la pelota de los dos. Ya pasaba con
        // un solo nucleo, pero con cada juego corriendo en su nucleo a la
        // vez salta a la vista.
        //
        // Para las tareas sin foco se responde "no pulsada" SIN consumir
        // nada: KeyHit y GetKey sacan la tecla de una cola, y si un
        // programa sin foco se la llevara, el que tiene el foco no la
        // veria nunca.
        case SYS_KEY_DOWN:
            if (!tarea_tiene_foco()) return 0;
            return key_is_down((uint16_t)a0) ? 1 : 0;

        // El estado de 64 teclas de golpe, en un entero.
        // Un bucle de juego preguntaba una tecla por llamada al sistema:
        // correr, saltar y disparar eran tres, cada fotograma. Con esto
        // son DOS llamadas en total -- el banco 0 trae Esc, Espacio y las
        // letras, y el banco 1 las flechas -- y a partir de ahi mirar una
        // tecla es un And, sin salir al kernel.
        case SYS_KEY_BANK: {
            if (!tarea_tiene_foco()) return 0;
            uint32_t banco = (uint32_t)a0;
            if (banco >= 8) return 0;   // key_state solo tiene 512 teclas
            uint64_t mascara = 0;
            for (uint32_t i = 0; i < 64; i++) {
                if (key_is_down((uint16_t)(banco * 64 + i))) mascara |= (1ULL << i);
            }
            return mascara;
        }

        case SYS_KEY_HIT:
            if (!tarea_tiene_foco()) return 0;
            return key_was_hit((uint16_t)a0);

        case SYS_GET_KEY: {
            if (!tarea_tiene_foco()) return 0;
            uint16_t code;
            return input_read_scancode(&code) ? (uint64_t)code : 0;
        }

        case SYS_FLUSH_KEYS:
            // Vaciar el teclado solo si la ventana tiene el foco: si no,
            // una app de fondo que vaciara el suyo borraria las teclas
            // pendientes de la app que el usuario tiene delante. Ver la
            // nota de SYS_KEY_DOWN.
            if (!tarea_tiene_foco()) return 0;
            input_flush_keys();
            return 0;

        case SYS_MOUSE_HIT:
            if (!tarea_tiene_foco()) return 0;   // ver la nota de SYS_KEY_DOWN
            return mouse_button_was_hit((int)a0);

        case SYS_GET_MOUSE_SPEED: {
            if (!tarea_tiene_foco()) return 0;   // tambien se consume al leerla
            int32_t dx = mouse_x_speed();
            int32_t dy = mouse_y_speed();
            return ((uint64_t)(uint32_t)dx << 32) | (uint32_t)dy;
        }

        case SYS_MOVE_MOUSE:
            mouse_move_to((int32_t)a0, (int32_t)a1);
            return 0;

        case SYS_CREATE_BANK:
            return (uint64_t)(int64_t)bank_create((uint32_t)a0);

        case SYS_FREE_BANK:
            bank_free((int32_t)a0);
            return 0;

        case SYS_BANK_SIZE:
            return bank_size((int32_t)a0);

        case SYS_RESIZE_BANK:
            return bank_resize((int32_t)a0, (uint32_t)a1) ? 0 : (uint64_t)-1;

        case SYS_COPY_BANK:
            return bank_copy((int32_t)a0, (uint32_t)a1, (int32_t)a2, (uint32_t)a3, (uint32_t)a4) ? 0 : (uint64_t)-1;

        case SYS_PEEK_BYTE:
            return bank_peek_bytes((int32_t)a0, (uint32_t)a1, 1);

        case SYS_PEEK_SHORT:
            return bank_peek_bytes((int32_t)a0, (uint32_t)a1, 2);

        case SYS_PEEK_INT:
            return bank_peek_bytes((int32_t)a0, (uint32_t)a1, 4);

        case SYS_POKE_BYTE:
            bank_poke_bytes((int32_t)a0, (uint32_t)a1, 1, (uint32_t)a2);
            return 0;

        case SYS_POKE_SHORT:
            bank_poke_bytes((int32_t)a0, (uint32_t)a1, 2, (uint32_t)a2);
            return 0;

        case SYS_POKE_INT:
            bank_poke_bytes((int32_t)a0, (uint32_t)a1, 4, (uint32_t)a2);
            return 0;

        case SYS_GENFILE_OPEN: {
            const char *n = validate_str(a0, 256);
            return n ? (uint64_t)(int64_t)genfile_open(n, (uint32_t)a1) : (uint64_t)-1;
        }

        case SYS_GENFILE_READ_BYTES:
            if (!validate_buf(a1, a2)) return (uint64_t)-1;
            return genfile_read_bytes((int32_t)a0, (uint8_t *)a1, (uint32_t)a2);

        case SYS_GENFILE_WRITE_BYTES:
            if (!validate_buf(a1, a2)) return (uint64_t)-1;
            return genfile_write_bytes((int32_t)a0, (const uint8_t *)a1, (uint32_t)a2) ? 0 : (uint64_t)-1;

        case SYS_GENFILE_POS:
            return genfile_pos((int32_t)a0);

        case SYS_GENFILE_SEEK:
            return genfile_seek((int32_t)a0, (uint32_t)a1) ? 0 : (uint64_t)-1;

        case SYS_GENFILE_SIZE:
            return genfile_size((int32_t)a0);

        case SYS_GENFILE_EOF:
            return genfile_eof((int32_t)a0) ? 1 : 0;

        case SYS_GENFILE_CLOSE:
            genfile_close((int32_t)a0);
            return 0;

        case SYS_DIR_OPEN:
            return (uint64_t)(int64_t)dir_open((uint32_t)a0);

        case SYS_DIR_NEXT:
            if (!validate_buf(a1, a2)) return 0;
            return dir_next((int32_t)a0, (char *)a1, (uint32_t)a2);

        case SYS_DIR_CLOSE:
            dir_close((int32_t)a0);
            return 0;

        case SYS_FILE_SIZE_BY_NAME: {
            const char *n = validate_str(a0, 256);
            return n ? (uint64_t)(int64_t)file_size_by_name(n) : (uint64_t)-1;
        }

        case SYS_FILE_TYPE_BY_NAME: {
            const char *n = validate_str(a0, 256);
            return n ? (uint64_t)(int64_t)file_type_by_name(n) : 0;
        }

        case SYS_FIND_CHILD: {
            const char *n = validate_str(a0, 256);
            if (!n) return (uint64_t)-1;
            // Con carpetas, la ruta manda y el argumento 'padre' se ignora
            // (una ruta siempre parte de la raiz). Esta es la unica forma
            // que tiene un programa de usuario de resolver una ruta SIN
            // crear nada: SYS_FILE_OPEN crea el archivo si no existe, y eso
            // convierte un nombre mal escrito en un archivo vacio.
            // nbc.pro la usa para los Include.
            char hoja[NEMOFS_MAX_NAME + 1];
            uint32_t padre = (uint32_t)a1;
            int es_ruta = ruta_o_nombre(&n, &padre, hoja, sizeof(hoja));
            if (es_ruta < 0) return (uint64_t)-1;
            return (uint64_t)(int64_t)nemofs_find_child(padre, n);
        }

        case SYS_DELETE_ANYWHERE: {
            const char *n = validate_str(a0, 256);
            if (!n) return (uint64_t)-1;
            int32_t ok = -1;
            delete_anywhere(n, &ok);
            return (uint64_t)(int64_t)ok;
        }

        case SYS_CLOSE_WINDOW:
            // No hace falta validar nada: solo puede cerrar la SUYA. No
            // recibe un indice de ventana a proposito -- con uno, un
            // programa podria cerrar la ventana de otro escribiendo un
            // numero cualquiera.
            return (uint64_t)(int64_t)task_close_window();

        case SYS_RENAME_ANYWHERE: {
            // Los dos nombres se validan ANTES de tocar nada: validate_str
            // del segundo puede fallar, y para entonces con un solo nombre
            // validado ya se habria renombrado a medias.
            const char *viejo = validate_str(a0, 256);
            const char *nuevo = viejo ? validate_str(a1, 256) : 0;
            if (!viejo || !nuevo || !nuevo[0]) return (uint64_t)-1;
            int32_t ok = -1;
            rename_anywhere(viejo, nuevo, &ok);
            return (uint64_t)(int64_t)ok;
        }

        case SYS_SET_ORIGIN:
            g_origin_x = (int32_t)a0;
            g_origin_y = (int32_t)a1;
            return 0;

        case SYS_GET_PIXEL: {
            current_window = task_ensure_window();
            if (current_window < 0) return 0;
            uint32_t menu_off = gadgets_menubar_height(current_window);
            int32_t x = (int32_t)a0 + g_origin_x;
            int32_t y = (int32_t)a1 + g_origin_y + (int32_t)menu_off;
            if (x < 0 || y < 0) return 0;
            return wm_content_get_pixel(current_window, (uint32_t)x, (uint32_t)y);
        }

        case SYS_READ_PIXEL: {
            // buffer=0 (ventana actual) o buffer=N (imagen N-1, misma
            // convencion que ImageBuffer()).
            int32_t buffer = (int32_t)a2;
            if (buffer == 0) {
                current_window = task_ensure_window();
                if (current_window < 0) return 0;
                uint32_t menu_off = gadgets_menubar_height(current_window);
                int32_t x = (int32_t)a0 + g_origin_x;
                int32_t y = (int32_t)a1 + g_origin_y + (int32_t)menu_off;
                if (x < 0 || y < 0) return 0;
                return wm_content_get_pixel(current_window, (uint32_t)x, (uint32_t)y);
            }
            int32_t handle = buffer - 1;
            if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return 0;
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            if (x < 0 || y < 0 || (uint32_t)x >= images[handle].width || (uint32_t)y >= images[handle].height) return 0;
            const uint8_t *px = &image_pixels[handle][((uint32_t)y * images[handle].width + (uint32_t)x) * 4];
            return ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
        }

        case SYS_WRITE_PIXEL: {
            int32_t buffer = (int32_t)a3;
            uint32_t rgb = (uint32_t)a2 & 0xFFFFFF;
            if (buffer == 0) {
                current_window = task_ensure_window();
                if (current_window < 0) return 0;
                uint32_t menu_off = gadgets_menubar_height(current_window);
                int32_t x = (int32_t)a0 + g_origin_x;
                int32_t y = (int32_t)a1 + g_origin_y + (int32_t)menu_off;
                if (x < 0 || y < 0) return 0;
                wm_content_fill_rect(current_window, (uint32_t)x, (uint32_t)y, 1, 1, rgb);
                return 0;
            }
            int32_t handle = buffer - 1;
            if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return 0;
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            if (x < 0 || y < 0 || (uint32_t)x >= images[handle].width || (uint32_t)y >= images[handle].height) return 0;
            uint8_t *px = &image_pixels[handle][((uint32_t)y * images[handle].width + (uint32_t)x) * 4];
            px[0] = (uint8_t)(rgb >> 16); px[1] = (uint8_t)(rgb >> 8); px[2] = (uint8_t)rgb; px[3] = 255;
            return 0;
        }

        case SYS_COPY_PIXEL: {
            int32_t src_x = (int32_t)(a0 >> 16), src_y = (int32_t)(a0 & 0xFFFF);
            int32_t src_buffer = (int32_t)a1;
            int32_t dest_x = (int32_t)(a2 >> 16), dest_y = (int32_t)(a2 & 0xFFFF);
            int32_t dest_buffer = (int32_t)a3;
            // leemos del origen (misma logica que SYS_READ_PIXEL, sin
            // pasar por el dispatcher para no repetir el calculo de
            // origen/menu dos veces innecesariamente)
            uint32_t rgb;
            if (src_buffer == 0) {
                current_window = task_ensure_window();
                if (current_window < 0) return 0;
                uint32_t menu_off = gadgets_menubar_height(current_window);
                int32_t x = src_x + g_origin_x, y = src_y + g_origin_y + (int32_t)menu_off;
                if (x < 0 || y < 0) return 0;
                rgb = wm_content_get_pixel(current_window, (uint32_t)x, (uint32_t)y);
            } else {
                int32_t handle = src_buffer - 1;
                if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return 0;
                if (src_x < 0 || src_y < 0 || (uint32_t)src_x >= images[handle].width || (uint32_t)src_y >= images[handle].height) return 0;
                const uint8_t *px = &image_pixels[handle][((uint32_t)src_y * images[handle].width + (uint32_t)src_x) * 4];
                rgb = ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
            }
            // escribimos en el destino
            if (dest_buffer == 0) {
                current_window = task_ensure_window();
                if (current_window < 0) return 0;
                uint32_t menu_off = gadgets_menubar_height(current_window);
                int32_t x = dest_x + g_origin_x, y = dest_y + g_origin_y + (int32_t)menu_off;
                if (x < 0 || y < 0) return 0;
                wm_content_fill_rect(current_window, (uint32_t)x, (uint32_t)y, 1, 1, rgb);
            } else {
                int32_t handle = dest_buffer - 1;
                if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return 0;
                if (dest_x < 0 || dest_y < 0 || (uint32_t)dest_x >= images[handle].width || (uint32_t)dest_y >= images[handle].height) return 0;
                uint8_t *px = &image_pixels[handle][((uint32_t)dest_y * images[handle].width + (uint32_t)dest_x) * 4];
                px[0] = (uint8_t)(rgb >> 16); px[1] = (uint8_t)(rgb >> 8); px[2] = (uint8_t)rgb; px[3] = 255;
            }
            return 0;
        }

        case SYS_CREATE_CANVAS: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_canvas(x, y, w, h, current_window);
        }

        case SYS_LOCK_BUFFER: {
            int32_t buffer = (int32_t)a0;
            if (buffer == 0) {
                current_window = task_ensure_window();
                if (current_window < 0) return 0;
                int32_t wx, wy; uint32_t ww, wh;
                if (!wm_get_window_client_rect(current_window, &wx, &wy, &ww, &wh)) return 0;
                g_locked_width = ww;
                g_locked_height = wh;
            } else {
                int32_t handle = buffer - 1;
                if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return 0;
                g_locked_width = images[handle].width;
                g_locked_height = images[handle].height;
            }
            g_buffer_locked = true;
            g_locked_buffer_id = buffer;
            return 0;
        }

        case SYS_UNLOCK_BUFFER:
            g_buffer_locked = false;
            return 0;

        case SYS_LOCKED_PIXELS:
            return g_buffer_locked ? (uint64_t)(int64_t)LOCKED_BUFFER_SENTINEL : 0;

        case SYS_LOCKED_PITCH:
            return g_buffer_locked ? (uint64_t)(g_locked_width * 4) : 0;

        case SYS_LOCKED_FORMAT:
            return g_buffer_locked ? 4 : 0; // 4 = RGB de 32 bits, igual que GraphicsFormat

        case SYS_READ_PIXEL_FAST: {
            if (!g_buffer_locked) return 0; // BlitzPlus real: DEBE haber un buffer bloqueado
            int32_t buffer = (int32_t)a2;
            if (buffer == 0) {
                current_window = task_ensure_window();
                if (current_window < 0) return 0;
                uint32_t menu_off = gadgets_menubar_height(current_window);
                int32_t x = (int32_t)a0 + g_origin_x;
                int32_t y = (int32_t)a1 + g_origin_y + (int32_t)menu_off;
                if (x < 0 || y < 0) return 0;
                return wm_content_get_pixel(current_window, (uint32_t)x, (uint32_t)y);
            }
            int32_t handle = buffer - 1;
            if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return 0;
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            if (x < 0 || y < 0 || (uint32_t)x >= images[handle].width || (uint32_t)y >= images[handle].height) return 0;
            const uint8_t *px = &image_pixels[handle][((uint32_t)y * images[handle].width + (uint32_t)x) * 4];
            return ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
        }

        case SYS_WRITE_PIXEL_FAST: {
            if (!g_buffer_locked) return 0;
            int32_t buffer = (int32_t)a3;
            uint32_t rgb = (uint32_t)a2 & 0xFFFFFF;
            if (buffer == 0) {
                current_window = task_ensure_window();
                if (current_window < 0) return 0;
                uint32_t menu_off = gadgets_menubar_height(current_window);
                int32_t x = (int32_t)a0 + g_origin_x;
                int32_t y = (int32_t)a1 + g_origin_y + (int32_t)menu_off;
                if (x < 0 || y < 0) return 0;
                wm_content_fill_rect(current_window, (uint32_t)x, (uint32_t)y, 1, 1, rgb);
                return 0;
            }
            int32_t handle = buffer - 1;
            if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return 0;
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            if (x < 0 || y < 0 || (uint32_t)x >= images[handle].width || (uint32_t)y >= images[handle].height) return 0;
            uint8_t *px = &image_pixels[handle][((uint32_t)y * images[handle].width + (uint32_t)x) * 4];
            px[0] = (uint8_t)(rgb >> 16); px[1] = (uint8_t)(rgb >> 8); px[2] = (uint8_t)rgb; px[3] = 255;
            return 0;
        }

        case SYS_COPY_PIXEL_FAST: {
            if (!g_buffer_locked) return 0;
            int32_t src_x = (int32_t)(a0 >> 16), src_y = (int32_t)(a0 & 0xFFFF);
            int32_t src_buffer = (int32_t)a1;
            int32_t dest_x = (int32_t)(a2 >> 16), dest_y = (int32_t)(a2 & 0xFFFF);
            int32_t dest_buffer = (int32_t)a3;
            uint32_t rgb;
            if (src_buffer == 0) {
                current_window = task_ensure_window();
                if (current_window < 0) return 0;
                uint32_t menu_off = gadgets_menubar_height(current_window);
                int32_t x = src_x + g_origin_x, y = src_y + g_origin_y + (int32_t)menu_off;
                if (x < 0 || y < 0) return 0;
                rgb = wm_content_get_pixel(current_window, (uint32_t)x, (uint32_t)y);
            } else {
                int32_t handle = src_buffer - 1;
                if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return 0;
                if (src_x < 0 || src_y < 0 || (uint32_t)src_x >= images[handle].width || (uint32_t)src_y >= images[handle].height) return 0;
                const uint8_t *px = &image_pixels[handle][((uint32_t)src_y * images[handle].width + (uint32_t)src_x) * 4];
                rgb = ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
            }
            if (dest_buffer == 0) {
                current_window = task_ensure_window();
                if (current_window < 0) return 0;
                uint32_t menu_off = gadgets_menubar_height(current_window);
                int32_t x = dest_x + g_origin_x, y = dest_y + g_origin_y + (int32_t)menu_off;
                if (x < 0 || y < 0) return 0;
                wm_content_fill_rect(current_window, (uint32_t)x, (uint32_t)y, 1, 1, rgb);
            } else {
                int32_t handle = dest_buffer - 1;
                if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return 0;
                if (dest_x < 0 || dest_y < 0 || (uint32_t)dest_x >= images[handle].width || (uint32_t)dest_y >= images[handle].height) return 0;
                uint8_t *px = &image_pixels[handle][((uint32_t)dest_y * images[handle].width + (uint32_t)dest_x) * 4];
                px[0] = (uint8_t)(rgb >> 16); px[1] = (uint8_t)(rgb >> 8); px[2] = (uint8_t)rgb; px[3] = 255;
            }
            return 0;
        }

        // ---- Filas de pixeles ----
        // Ver la nota grande junto a fila_imagen(), arriba: el porque, y
        // por que el trabajo por pixel se queda aqui y no en el programa.

        case SYS_FILA_LEER: {
            uint8_t *px; uint32_t ancho, alto;
            if (!fila_imagen((int32_t)a0, &px, &ancho, &alto)) return (uint64_t)-1;
            uint32_t paso = fila_paso(a5);
            if (!paso) return (uint64_t)-1;
            int32_t y = (int32_t)a2;
            uint32_t x, n, salto;
            if (!fila_recortar((int32_t)a1, y, (int32_t)a3, ancho, alto, &x, &n, &salto)) return 0;
            uint64_t destino = a4 + (uint64_t)salto * paso;
            if (!validate_buf(destino, (uint64_t)n * paso)) return (uint64_t)-1;
            const uint8_t *src = &px[((uint32_t)y * ancho + x) * 4];
            uint8_t *dst = (uint8_t *)destino;
            // Byte a byte, y no con un uint32_t*: el puntero lo da el
            // programa y puede no estar alineado a 4. Un uint32_t*
            // desalineado es UB -- funciona en ARM64 mientras
            // SCTLR_EL1.A este a 0, y deja de funcionar el dia que no.
            //
            // El orden es little-endian, para que el valor salga como
            // 0xRRGGBB: igual que devuelve ReadPixel, y igual que leen
            // nemo.leer_i32 en Lua y un array de enteros en Nemo Basic.
            for (uint32_t i = 0; i < n; i++) {
                uint8_t *d = &dst[i * paso];
                d[0] = src[i * 4 + 2];   // B
                d[1] = src[i * 4 + 1];   // G
                d[2] = src[i * 4 + 0];   // R
                for (uint32_t k = 3; k < paso; k++) d[k] = 0;   // el resto del hueco, a cero
            }
            return n;
        }

        case SYS_FILA_ESCRIBIR: {
            uint8_t *px; uint32_t ancho, alto;
            if (!fila_imagen((int32_t)a0, &px, &ancho, &alto)) return (uint64_t)-1;
            uint32_t paso = fila_paso(a5);
            if (!paso) return (uint64_t)-1;
            int32_t y = (int32_t)a2;
            uint32_t x, n, salto;
            if (!fila_recortar((int32_t)a1, y, (int32_t)a3, ancho, alto, &x, &n, &salto)) return 0;
            uint64_t origen = a4 + (uint64_t)salto * paso;
            if (!validate_buf(origen, (uint64_t)n * paso)) return (uint64_t)-1;
            const uint8_t *src = (const uint8_t *)origen;
            uint8_t *dst = &px[((uint32_t)y * ancho + x) * 4];
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *s = &src[i * paso];
                dst[i * 4 + 0] = s[2];    // R <- byte alto del 0xRRGGBB
                dst[i * 4 + 1] = s[1];    // G
                dst[i * 4 + 2] = s[0];    // B
                dst[i * 4 + 3] = 255;     // opaco, como WritePixel
            }
            return n;
        }

        case SYS_FILA_RELLENAR: {
            uint8_t *px; uint32_t ancho, alto;
            if (!fila_imagen((int32_t)a0, &px, &ancho, &alto)) return (uint64_t)-1;
            int32_t y = (int32_t)a2;
            uint32_t x, n, salto;
            if (!fila_recortar((int32_t)a1, y, (int32_t)a3, ancho, alto, &x, &n, &salto)) return 0;
            uint32_t rgb = (uint32_t)a4 & 0xFFFFFF;
            uint8_t r = (uint8_t)(rgb >> 16), g = (uint8_t)(rgb >> 8), b = (uint8_t)rgb;
            uint8_t *dst = &px[((uint32_t)y * ancho + x) * 4];
            for (uint32_t i = 0; i < n; i++) {
                dst[i * 4 + 0] = r; dst[i * 4 + 1] = g;
                dst[i * 4 + 2] = b; dst[i * 4 + 3] = 255;
            }
            return n;
        }

        // Longitud de la TIRA de pixeles que, empezando en (x, y) y
        // avanzando hacia la derecha (a3 > 0) o la izquierda (a3 < 0),
        // cumplen la prueba: a5 = 0 -> SON del color a4; a5 = 1 -> NO lo
        // son. Para al salirse de la imagen o al agotar |a3|. El pixel de
        // partida cuenta si cumple.
        //
        // Es la pieza que hace barato un relleno por inundacion: buscar
        // los dos extremos de un tramo y luego localizar los tramos de
        // las filas de arriba y abajo son los cuatro usos de esta misma
        // llamada, y ninguno cuesta una llamada por pixel.
        case SYS_FILA_TIRA: {
            uint8_t *px; uint32_t ancho, alto;
            if (!fila_imagen((int32_t)a0, &px, &ancho, &alto)) return (uint64_t)-1;
            int32_t x = (int32_t)a1, y = (int32_t)a2, max = (int32_t)a3;
            uint32_t color = (uint32_t)a4 & 0xFFFFFF;
            bool buscar_distinto = (a5 != 0);
            if (y < 0 || (uint32_t)y >= alto) return 0;
            if (x < 0 || (uint32_t)x >= ancho) return 0;
            // -(int64_t)INT32_MIN cabe en int64_t; -max en int32_t no.
            int64_t cuantos = (max < 0) ? -(int64_t)max : (int64_t)max;
            int64_t paso = (max < 0) ? -1 : 1;
            const uint8_t *fila = &px[(uint32_t)y * ancho * 4];
            int64_t n = 0, cx = x;
            while (n < cuantos && cx >= 0 && cx < (int64_t)ancho) {
                const uint8_t *p = &fila[cx * 4];
                uint32_t c = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
                if ((c == color) == buscar_distinto) break;   // se acabo la tira
                n++; cx += paso;
            }
            return (uint64_t)n;
        }

        case SYS_LOAD_FONT: {
            const char *n = validate_str(a0, 256);
            return n ? (uint64_t)(int64_t)font_load(n, (int32_t)a1, a2 != 0, a3 != 0, a4 != 0) : (uint64_t)-1;
        }

        case SYS_FREE_FONT:
            font_free((int32_t)a0);
            return 0;

        case SYS_SET_FONT: {
            int32_t idx = (int32_t)a0 - 1;
            if (idx >= 0 && idx < MAX_FONTS && fonts[idx].used) {
                g_current_font = idx;
                // Escala redondeada al entero mas cercano (minimo 1)
                // -- ej. alto pedido 24 con FONT_HEIGHT=7 da escala 3
                // (21 pixeles reales, la aproximacion mas cercana sin
                // pasarse mucho).
                int32_t scale = (fonts[idx].height + FONT_HEIGHT / 2) / FONT_HEIGHT;
                g_font_scale = (uint32_t)(scale < 1 ? 1 : scale);
                g_font_bold = fonts[idx].bold;
                g_cara = fonts[idx].cara;
            } else {
                // SetFont(0) o handle invalido -- vuelve a la fuente
                // de sistema por defecto (escala 1, sin negrita).
                g_current_font = -1;
                g_cara = 0;
                g_font_scale = 1;
                g_font_bold = false;
            }
            return 0;
        }

        case SYS_FONT_NAME: {
            int32_t idx = (int32_t)a0 - 1;
            uint32_t max_len = (uint32_t)a2;
            if (max_len == 0 || !validate_buf(a1, max_len)) return 0;
            char *out = (char *)a1;
            if (idx < 0 || idx >= MAX_FONTS || !fonts[idx].used) { out[0] = '\0'; return 0; }
            uint32_t i = 0;
            while (fonts[idx].name[i] != '\0' && i < max_len - 1) { out[i] = fonts[idx].name[i]; i++; }
            out[i] = '\0';
            return i;
        }

        case SYS_FONT_SIZE: {
            int32_t idx = (int32_t)a0 - 1;
            if (idx < 0 || idx >= MAX_FONTS || !fonts[idx].used) return 0;
            return (uint64_t)(int64_t)fonts[idx].height;
        }

        case SYS_FONT_STYLE: {
            int32_t idx = (int32_t)a0 - 1;
            if (idx < 0 || idx >= MAX_FONTS || !fonts[idx].used) return 0;
            // Antes: "italic ? 3 : 1" -- nunca miraba 'bold' en absoluto,
            // y jamas podia devolver 0 (ninguna fuente aparecia sin
            // estilo, aunque no tuviera ni negrita ni cursiva). Bits
            // convencionales de BlitzPlus: 1=negrita, 2=cursiva.
            // Confirmado en QEMU, auditoria de Nemo-Blitz.
            return (uint64_t)((fonts[idx].bold ? 1 : 0) | (fonts[idx].italic ? 2 : 0));
        }

        case SYS_FONT_WIDTH:
            // Ancho REAL en pantalla del caracter mas ancho -- ya
            // refleja la escala activa (SetFont), no siempre 5.
            if (g_cara) return fonts_avance(g_cara, 'n');
            return (uint64_t)(FONT_WIDTH * g_font_scale);

        case SYS_FONT_HEIGHT:
            if (g_cara) return fonts_alto_linea(g_cara);
            return (uint64_t)(FONT_HEIGHT * g_font_scale);

        case SYS_SET_GAMMA: {
            gamma_ensure_init();
            uint8_t src_r = (uint8_t)(a0 >> 16), src_g = (uint8_t)(a0 >> 8), src_b = (uint8_t)a0;
            uint8_t dst_r = (uint8_t)(a1 >> 16), dst_g = (uint8_t)(a1 >> 8), dst_b = (uint8_t)a1;
            // BlitzPlus real permite que los valores de destino "den
            // la vuelta" (roll-over) en vez de recortarse -- al
            // guardarlos ya en un uint8_t, el propio desbordamiento
            // de C hace ese "modulo 256" sin mas trabajo.
            g_gamma_r[src_r] = dst_r;
            g_gamma_g[src_g] = dst_g;
            g_gamma_b[src_b] = dst_b;
            return 0;
        }

        case SYS_UPDATE_GAMMA:
            // No-op: no tenemos tabla de gamma de hardware a la que
            // "empujar" los cambios -- se acepta el parametro
            // 'calibrate' por si tiene efectos secundarios, sin mas.
            return 0;

        case SYS_GAMMA_RED:
            gamma_ensure_init();
            return g_gamma_r[(uint8_t)a0];

        case SYS_GAMMA_GREEN:
            gamma_ensure_init();
            return g_gamma_g[(uint8_t)a0];

        case SYS_GAMMA_BLUE:
            gamma_ensure_init();
            return g_gamma_b[(uint8_t)a0];

        case SYS_GFX_DRIVER_NAME: {
            uint32_t max_len = (uint32_t)a2;
            if (max_len == 0 || !validate_buf(a1, max_len)) return 0;
            char *out = (char *)a1;
            const char *name = ((int32_t)a0 == 1) ? "Nemo OS Framebuffer" : "";
            uint32_t i = 0;
            while (name[i] != '\0' && i < max_len - 1) { out[i] = name[i]; i++; }
            out[i] = '\0';
            return i;
        }

        case SYS_GFX_MODE_FORMAT:
        case SYS_GRAPHICS_FORMAT:
            // 4 = formato RGB de 32 bits, byte alto sin usar -- el
            // mas parecido a como guardamos los pixeles internamente
            // (4 bytes por pixel; el byte "alto" en la clasificacion
            // de BlitzPlus es donde nosotros SI usamos alfa para
            // mezclar, pero de cara al programa es el equivalente
            // mas cercano de los 4 formatos documentados).
            return 4;

        case SYS_TOTAL_VID_MEM:
            // Valor nominal fijo -- no hay una tarjeta grafica de
            // verdad de la que consultar esto en QEMU con nuestro
            // framebuffer simple.
            return 64 * 1024 * 1024;

        case SYS_FONT_CHAR_ADVANCE:
            return (uint64_t)((FONT_WIDTH + 1) * g_font_scale);

        case SYS_SET_PANEL_COLOR:
            gadget_set_panel_color((int32_t)a0, (uint32_t)a1);
            return 0;

        case SYS_SET_PANEL_IMAGE: {
            const char *n = validate_str(a1, 256);
            int32_t image_handle = n ? image_load(n) : -1;
            // si no se encuentra (o no es NIMG), el panel conserva
            // la imagen que tenia: antes se quedaba en un cuadrado vacio
            if (image_handle < 0) return (uint64_t)-1;
            gadget_set_panel_image((int32_t)a0, image_handle);
            return 0;
        }

        case SYS_SET_GADGET_GROUP:
            gadget_set_group((int32_t)a0, (int32_t)a1);
            return 0;

        case SYS_GADGET_GROUP:
            return (uint64_t)(int64_t)gadget_get_group((int32_t)a0);

        case SYS_TFORM_IMAGE: {
            // a1..a4 ya llegan en punto fijo Q16.16 (convertidos por
            // el COMPILADOR con coma flotante real -- ver la nota
            // junto a FP_SHIFT, un poco mas arriba).
            tform_image((int32_t)a0, (int32_t)a1, (int32_t)a2, (int32_t)a3, (int32_t)a4);
            return 0;
        }

        case SYS_LOAD_SOUND: {
            const char *n = validate_str(a0, 256);
            return n ? (uint64_t)(int64_t)sound_load(n) : (uint64_t)-1;
        }

        case SYS_FREE_SOUND:
            sound_free((int32_t)a0);
            return 0;

        case SYS_PLAY_SOUND:
            sound_play((int32_t)a0);
            return (uint64_t)a0; // "canal" devuelto = el mismo handle de sonido (ver limitacion documentada)

        case SYS_SOUND_VOLUME: {
            // a1 = volumen ya escalado x1000 (0-1000), convertido en
            // el COMPILADOR con coma flotante real -- ver la nota
            // junto a sound_volume_permil.
            int32_t h = (int32_t)a0;
            if (h >= 0 && h < MAX_SOUNDS) sound_volume_permil[h] = (int32_t)a1;
            return 0;
        }

        case SYS_SOUND_PAN: {
            // a1 = pan ya escalado x1000 (-1000 a 1000), mismo motivo.
            int32_t h = (int32_t)a0;
            if (h >= 0 && h < MAX_SOUNDS) sound_pan_permil[h] = (int32_t)a1;
            return 0;
        }

        case SYS_SOUND_PITCH: {
            int32_t h = (int32_t)a0;
            if (h >= 0 && h < MAX_SOUNDS) sound_pitch_hz[h] = (uint32_t)a1;
            return 0;
        }

        case SYS_EXEC_FILE: {
            const char *n = validate_str(a0, 256);
            int32_t id = n ? exec_program(n, (const char *)0) : -1;
            return id >= 0 ? 1 : 0;
        }

        case SYS_CREATE_PROCESS: {
            // command$ puede traer argumentos separados por un
            // espacio (ej. "editor.pro archivo.txt") -- partimos en
            // el PRIMER espacio, igual que hace la shell con 'run'.
            const char *cmd = validate_str(a0, 256);
            if (!cmd) return (uint64_t)-1;
            char name_buf[64];
            uint32_t i = 0;
            while (cmd[i] != '\0' && cmd[i] != ' ' && i < sizeof(name_buf) - 1) {
                name_buf[i] = cmd[i];
                i++;
            }
            name_buf[i] = '\0';
            const char *arg = (cmd[i] == ' ') ? &cmd[i + 1] : (const char *)0;
            return (uint64_t)(int64_t)exec_program(name_buf, arg);
        }

        case SYS_COPY_RECT: {
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            uint32_t menu_off = gadgets_menubar_height(current_window);

            uint32_t src_enc = (uint32_t)(a2 >> 16);
            uint32_t w = (uint32_t)(a2 & 0xFFFF);
            uint32_t dst_enc = (uint32_t)(a3 >> 16);
            uint32_t h = (uint32_t)(a3 & 0xFFFF);
            int32_t src_img = src_enc == 0 ? -1 : (int32_t)(src_enc - 1);
            int32_t dst_img = dst_enc == 0 ? -1 : (int32_t)(dst_enc - 1);

            // El origen y el offset de ventana (Origin, barra de menu)
            // SOLO aplican al lado que sea la ventana -- un buffer de
            // imagen no tiene ninguno de los dos.
            int32_t x1 = (int32_t)a0 + (src_img < 0 ? g_origin_x : 0);
            int32_t y1 = (int32_t)a1 + (src_img < 0 ? g_origin_y + (int32_t)menu_off : 0);
            int32_t x2 = (int32_t)(a4 >> 32) + (dst_img < 0 ? g_origin_x : 0);
            int32_t y2 = (int32_t)(a4 & 0xFFFFFFFF) + (dst_img < 0 ? g_origin_y + (int32_t)menu_off : 0);
            if (x1 < 0 || y1 < 0 || x2 < 0 || y2 < 0) return (uint64_t)-1;

            bool ok = copy_rect_generic(current_window, src_img, (uint32_t)x1, (uint32_t)y1,
                                         dst_img, (uint32_t)x2, (uint32_t)y2, w, h);
            // (no se pide nada: el lienzo se publica solo -- ver la nota de arriba)
            return ok ? 0 : (uint64_t)-1;
        }

        case SYS_LOAD_IMAGE: {
            const char *n = validate_str(a0, 256);
            return n ? (uint64_t)(int64_t)image_load(n) : (uint64_t)-1;
        }

        case SYS_LOAD_IMAGE_EN: {
            const char *n = validate_str(a0, 256);
            return n ? (uint64_t)(int64_t)image_load_en(n, (uint32_t)a1) : (uint64_t)-1;
        }

        case SYS_FREE_IMAGE:
            image_free((int32_t)a0);
            return 0;

        case SYS_SET_IMAGE_HANDLE:
            image_set_handle((int32_t)a0, (int32_t)a1, (int32_t)a2);
            return 0;

        case SYS_GET_IMAGE_HANDLE: {
            int32_t handle = (int32_t)a0;
            if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return 0;
            return ((uint64_t)(uint32_t)images[handle].handle_x << 32) | (uint32_t)images[handle].handle_y;
        }

        case SYS_SET_AUTO_MID_HANDLE:
            g_auto_mid_handle = (a0 != 0);
            return 0;

        case SYS_MASK_IMAGE:
            image_mask((int32_t)a0, (uint32_t)a1);
            return 0;

        case SYS_COPY_IMAGE:
            return (uint64_t)(int64_t)image_copy((int32_t)a0);

        case SYS_SAVE_IMAGE: {
            const char *n = validate_str(a1, 256);
            return n && image_save((int32_t)a0, n) ? 0 : (uint64_t)-1;
        }

        case SYS_GRAB_IMAGE: {
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            uint32_t menu_off = gadgets_menubar_height(current_window);
            int32_t x = (int32_t)a0 + g_origin_x;
            int32_t y = (int32_t)a1 + g_origin_y + (int32_t)menu_off;
            if (x < 0 || y < 0) return (uint64_t)-1;
            return (uint64_t)(int64_t)image_grab(current_window, (uint32_t)x, (uint32_t)y, (uint32_t)a2, (uint32_t)a3);
        }

        case SYS_RESIZE_IMAGE:
            return image_resize((int32_t)a0, (uint32_t)a1, (uint32_t)a2) ? 0 : (uint64_t)-1;

        case SYS_ROTATE_IMAGE: {
            // a1 ya llega en punto fijo Q16.16, mismo motivo.
            return image_rotate((int32_t)a0, (int32_t)a1) ? 0 : (uint64_t)-1;
        }

        case SYS_DRAW_IMAGE_RECT: {
            int32_t handle = (int32_t)a0;
            if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return (uint64_t)-1;
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            int32_t wx, wy;
            uint32_t ww, wh;
            if (!wm_get_window_client_rect(current_window, &wx, &wy, &ww, &wh)) return (uint64_t)-1;
            (void)wx; (void)wy;
            uint32_t menu_off = gadgets_menubar_height(current_window);
            int32_t x = (int32_t)a1 + g_origin_x;
            int32_t y = (int32_t)a2 + g_origin_y + (int32_t)menu_off;
            bool solid = ((a3 >> 31) & 1) != 0; // DrawBlockRect (opaco) vs DrawImageRect (mezcla alfa)
            uint32_t rx = (uint32_t)((a3 >> 16) & 0x7FFF), ry = (uint32_t)(a3 & 0xFFFF);
            uint32_t rw = (uint32_t)(a4 >> 16), rh = (uint32_t)(a4 & 0xFFFF);
            if (rx + rw > images[handle].width || ry + rh > images[handle].height) return (uint64_t)-1;

            if (g_draw_target_image >= 0) {
                // SetBuffer ImageBuffer(x): el destino es otra imagen
                imgbuf_blit_rect(g_draw_target_image, handle,
                                 (int32_t)a1 + g_origin_x, (int32_t)a2 + g_origin_y,
                                 rx, ry, rw, rh, solid);
                return 0;
            }

            int32_t cx0, cy0, cx1, cy1;
            get_clip_bounds(ww, wh, menu_off, &cx0, &cy0, &cx1, &cy1);
            int32_t bx, by; uint32_t bw, bh, sdx, sdy;
            if (clip_blit(x, y, rw, rh, cx0, cy0, cx1, cy1, &bx, &by, &bw, &bh, &sdx, &sdy)) {
                const uint8_t *src = &image_pixels[handle][((ry + sdy) * images[handle].width + (rx + sdx)) * 4];
                wm_content_blit_image_rect(current_window, (uint32_t)bx, (uint32_t)by, bw, bh, images[handle].width, src, solid, images[handle].has_mask, images[handle].mask_color);
            }
            // (no se pide nada: el lienzo se publica solo -- ver la nota de arriba)
            return 0;
        }

        case SYS_LOAD_ANIM_IMAGE: {
            const char *n = validate_str(a0, 256);
            if (!n) return (uint64_t)-1;
            uint32_t cell_w = (uint32_t)(a1 >> 16), cell_h = (uint32_t)(a1 & 0xFFFF);
            return (uint64_t)(int64_t)image_load_anim(n, cell_w, cell_h, (uint32_t)a2, (uint32_t)a3);
        }

        case SYS_DRAW_IMAGE: {
            int32_t handle = (int32_t)a0;
            if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return (uint64_t)-1;

            if (g_draw_target_image >= 0) {
                // SetBuffer ImageBuffer(x): el destino es OTRA imagen.
                // Ni ventana, ni barra de menu, ni redibujado del
                // escritorio -- pintar el fondo de un nivel entero no
                // debe costar un repintado por tile.
                imgbuf_blit_image(g_draw_target_image, handle,
                                  (int32_t)a1 - images[handle].handle_x + g_origin_x,
                                  (int32_t)a2 - images[handle].handle_y + g_origin_y,
                                  (uint32_t)(a3 & 0x7FFFFFFF), ((a3 >> 31) & 1) != 0);
                return 0;
            }

            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            int32_t wx, wy;
            uint32_t ww, wh;
            if (!wm_get_window_client_rect(current_window, &wx, &wy, &ww, &wh)) return (uint64_t)-1;
            (void)wx; (void)wy;
            uint32_t menu_off = gadgets_menubar_height(current_window);
            // El punto de agarre (handle_x/handle_y) es el punto DE LA
            // IMAGEN que se alinea con (x,y) -- por defecto (0,0), la
            // esquina superior izquierda, pero MidHandle/HandleImage lo
            // puede mover (tipico para dibujar centrado en rotaciones).
            int32_t x = (int32_t)a1 - images[handle].handle_x + g_origin_x;
            int32_t y = (int32_t)a2 - images[handle].handle_y + g_origin_y + (int32_t)menu_off;

            bool solid = ((a3 >> 31) & 1) != 0; // DrawBlock (opaco) vs DrawImage (mezcla alfa)
            uint32_t frame = a3 & 0x7FFFFFFF;

            int32_t cx0, cy0, cx1, cy1;
            get_clip_bounds(ww, wh, menu_off, &cx0, &cy0, &cx1, &cy1);

            if (images[handle].cell_width > 0 && images[handle].cell_height > 0) {
                // Es un sprite sheet (LoadAnimImage) -- recortamos solo
                // la celda del fotograma pedido. El fotograma 0 (desde
                // el punto de vista del programa) corresponde a la
                // celda 'anim_first' de la hoja completa; recortamos
                // el indice al rango [0,anim_count) para no leer fuera
                // de la hoja si piden un fotograma invalido.
                uint32_t count = images[handle].anim_count > 0 ? images[handle].anim_count : 1;
                uint32_t cell_idx = images[handle].anim_first + (frame % count);
                uint32_t cols = images[handle].width / images[handle].cell_width;
                if (cols == 0) cols = 1;
                uint32_t rows = images[handle].height / images[handle].cell_height;
                if (rows == 0) rows = 1;
                uint32_t total_cells = cols * rows;
                if (cell_idx >= total_cells) cell_idx = cell_idx % total_cells; // defensivo: nunca leer fuera de la hoja real
                uint32_t fx = (cell_idx % cols) * images[handle].cell_width;
                uint32_t fy = (cell_idx / cols) * images[handle].cell_height;

                int32_t bx, by; uint32_t bw, bh, sdx, sdy;
                if (clip_blit(x, y, images[handle].cell_width, images[handle].cell_height, cx0, cy0, cx1, cy1, &bx, &by, &bw, &bh, &sdx, &sdy)) {
                    const uint8_t *src = &image_pixels[handle][((fy + sdy) * images[handle].width + (fx + sdx)) * 4];
                    wm_content_blit_image_rect(current_window, (uint32_t)bx, (uint32_t)by, bw, bh,
                                                images[handle].width, src, solid, images[handle].has_mask, images[handle].mask_color);
                }
            } else {
                int32_t bx, by; uint32_t bw, bh, sdx, sdy;
                if (clip_blit(x, y, images[handle].width, images[handle].height, cx0, cy0, cx1, cy1, &bx, &by, &bw, &bh, &sdx, &sdy)) {
                    const uint8_t *src = &image_pixels[handle][(sdy * images[handle].width + sdx) * 4];
                    wm_content_blit_image_rect(current_window, (uint32_t)bx, (uint32_t)by, bw, bh,
                                                images[handle].width, src, solid, images[handle].has_mask, images[handle].mask_color);
                }
            }
            // (no se pide nada: el lienzo se publica solo -- ver la nota de arriba)
            return 0;
        }

        case SYS_IMAGE_SIZE: {
            int32_t handle = (int32_t)a0;
            if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return 0;
            return ((uint64_t)images[handle].width << 32) | images[handle].height;
        }

        case SYS_CREATE_IMAGE:
            return (uint64_t)(int64_t)image_create((uint32_t)a0, (uint32_t)a1);

        case SYS_GET_SCREEN_SIZE:
            return ((uint64_t)fb_width() << 32) | fb_height();

        case SYS_DEFINE_BUTTON: {
            if (current_window < 0) return (uint64_t)-1;
            uint32_t id = (uint32_t)a0;
            int32_t x = (int32_t)a1, y = (int32_t)a2;
            uint32_t w = (uint32_t)(a3 >> 16);
            uint32_t h = (uint32_t)(a3 & 0xFFFF);
            uint32_t color = (uint32_t)a4;
            wm_define_button(current_window, id, x, y, w, h, color);
            wm_request_redraw();
            return 0;
        }

        case SYS_GET_BUTTON_ID: {
            if (current_window < 0) return 0;
            return wm_get_clicked_button(current_window);
        }

        case SYS_OPEN_FILE_DIALOG: {
            if (current_window < 0) return (uint64_t)-1;
            uint32_t start_dir = (uint32_t)a0;
            uint32_t out_max = (uint32_t)a2;
            if (!validate_buf(a1, out_max)) return (uint64_t)-1;
            char *out_name = (char *)a1;
            return (uint64_t)(int64_t)dialog_open_file(current_window, start_dir, out_name, out_max);
        }

        case SYS_SAVE_FILE_DIALOG: {
            if (current_window < 0) return (uint64_t)-1;
            uint32_t start_dir = (uint32_t)a0;
            uint32_t out_max = (uint32_t)a2;
            if (!validate_buf(a1, out_max)) return (uint64_t)-1;
            char *out_name = (char *)a1;
            return (uint64_t)(int64_t)dialog_save_file(current_window, start_dir, out_name, out_max);
        }

        case SYS_CREATE_WINDOW: {
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            const char *title = validate_str(a0, 256);
            if (!title) return (uint64_t)-1;
            int32_t x = (int32_t)a1, y = (int32_t)a2;
            uint32_t w = (uint32_t)a3, h = (uint32_t)a4;
            wm_configure_window(current_window, title, x, y, w, h);
            wm_set_event_mode(current_window, true);
            wm_request_redraw();
            return (uint64_t)(int64_t)current_window;
        }

        case SYS_READ_FILE_OPEN: {
            const char *n = validate_str(a0, 256);
            return n ? (uint64_t)(int64_t)readfile_open(n) : (uint64_t)-1;
        }

        case SYS_READ_FILE_LINE:
            if (!validate_buf(a1, a2)) return 0;
            return (uint64_t)readfile_line((int32_t)a0, (char *)a1, (uint32_t)a2);

        case SYS_READ_FILE_EOF:
            return readfile_eof((int32_t)a0) ? 1 : 0;

        case SYS_READ_FILE_CLOSE:
            readfile_close((int32_t)a0);
            return 0;

        // -- Gadgets estilo BlitzPlus --

        case SYS_CREATE_BUTTON: {
            current_window = task_ensure_window();
            const char *text = validate_str(a0, 4096);
            if (!text) return (uint64_t)-1;
            int32_t x = (int32_t)a1, y = (int32_t)a2;
            uint32_t w = (uint32_t)(a3 >> 16), h = (uint32_t)(a3 & 0xFFFF);
            uint32_t style = (uint32_t)a4;
            return (uint64_t)(int64_t)gadget_create_button(text, x, y, w, h, current_window, style);
        }
        case SYS_CREATE_LABEL: {
            current_window = task_ensure_window();
            const char *text = validate_str(a0, 4096);
            if (!text) return (uint64_t)-1;
            int32_t x = (int32_t)a1, y = (int32_t)a2;
            uint32_t w = (uint32_t)(a3 >> 16), h = (uint32_t)(a3 & 0xFFFF);
            uint32_t style = (uint32_t)a4;
            return (uint64_t)(int64_t)gadget_create_label(text, x, y, w, h, current_window, style);
        }
        case SYS_CREATE_PROGBAR: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_progbar(x, y, w, h, current_window);
        }
        case SYS_UPDATE_PROGBAR: {
            // a1 = valor ya escalado x1000 (0-1000), convertido en
            // el COMPILADOR con coma flotante real -- el kernel no
            // puede usarla (ver la nota junto a sound_volume_permil).
            gadget_update_progbar((int32_t)a0, (int32_t)a1);
            return 0;
        }
        case SYS_CREATE_SLIDER: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            uint32_t style = (uint32_t)a3;
            return (uint64_t)(int64_t)gadget_create_slider(x, y, w, h, current_window, style);
        }
        case SYS_CREATE_CONTEXT_MENU:
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            return (uint64_t)(int64_t)gadget_create_context_menu(current_window);

        case SYS_SHOW_CONTEXT_MENU:
            gadget_show_context_menu((int32_t)a0, (int32_t)a1, (int32_t)a2);
            return 0;

        case SYS_CREATE_SCROLLBAR: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            uint32_t style = (uint32_t)a3;
            return (uint64_t)(int64_t)gadget_create_scrollbar(x, y, w, h, current_window, style);
        }
        case SYS_SET_SLIDER_RANGE:
            gadget_set_slider_range((int32_t)a0, (int32_t)a1, (int32_t)a2);
            return 0;

        case SYS_SET_SLIDER_VALUE:
            gadget_set_slider_value((int32_t)a0, (int32_t)a1);
            return 0;

        case SYS_SLIDER_VALUE:
            return (uint64_t)(int64_t)gadget_slider_value((int32_t)a0);

        case SYS_CREATE_COMBOBOX: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_combobox(x, y, w, h, current_window);
        }
        case SYS_CREATE_TABBER: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_tabber(x, y, w, h, current_window);
        }

        case SYS_LOAD_ICON_STRIP:
            // LoadIconStrip = LoadImage con otro nombre -- mismo pool
            // de imagenes, ver la nota junto a image_get_info.
            { const char *n = validate_str(a0, 256); return n ? (uint64_t)(int64_t)image_load(n) : (uint64_t)-1; }

        case SYS_FREE_ICON_STRIP:
            image_free((int32_t)a0);
            return 0;

        case SYS_SET_GADGET_ICON_STRIP:
            gadget_set_icon_strip((int32_t)a0, (int32_t)a1);
            return 0;

        case SYS_CREATE_TOOLBAR: {
            // CreateToolBar(image$,...) recibe un NOMBRE DE ARCHIVO,
            // no un handle ya cargado -- lo cargamos aqui mismo
            // (reutilizando image_load, igual que LoadIconStrip).
            current_window = task_ensure_window();
            const char *n = validate_str(a0, 256);
            int32_t image_handle = n ? image_load(n) : -1;
            int32_t x = (int32_t)a1, y = (int32_t)a2;
            uint32_t w = (uint32_t)(a3 >> 16), h = (uint32_t)(a3 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_toolbar(image_handle, x, y, w, h, current_window);
        }

        case SYS_ENABLE_TOOLBAR_ITEM:
            gadget_enable_toolbar_item((int32_t)a0, (int32_t)a1, a2 != 0);
            return 0;

        case SYS_SET_TOOLBAR_TIPS: {
            const char *t = validate_str(a1, 4096);
            if (t) gadget_set_toolbar_tips((int32_t)a0, t);
            return 0;
        }

        case SYS_CREATE_TREEVIEW: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_treeview(x, y, w, h, current_window);
        }

        case SYS_TREEVIEW_ROOT:
            return (uint64_t)(int64_t)gadget_treeview_root((int32_t)a0);

        case SYS_ADD_TREEVIEW_NODE: {
            const char *t = validate_str(a0, 4096);
            return t ? (uint64_t)(int64_t)gadget_add_treeview_node(t, (int32_t)a1) : (uint64_t)-1;
        }

        case SYS_INSERT_TREEVIEW_NODE: {
            const char *t = validate_str(a1, 4096);
            return t ? (uint64_t)(int64_t)gadget_insert_treeview_node((int32_t)a0, t, (int32_t)a2) : (uint64_t)-1;
        }

        case SYS_MODIFY_TREEVIEW_NODE: {
            const char *t = validate_str(a1, 4096);
            if (t) gadget_modify_treeview_node((int32_t)a0, t);
            return 0;
        }

        case SYS_FREE_TREEVIEW_NODE:
            gadget_free_treeview_node((int32_t)a0);
            return 0;

        case SYS_EXPAND_TREEVIEW_NODE:
            gadget_expand_treeview_node((int32_t)a0, a1 != 0);
            return 0;

        case SYS_COUNT_TREEVIEW_NODES:
            return (uint64_t)(int64_t)gadget_count_treeview_nodes((int32_t)a0);

        case SYS_SELECTED_TREEVIEW_NODE:
            return (uint64_t)(int64_t)gadget_selected_treeview_node((int32_t)a0);

        case SYS_SELECT_TREEVIEW_NODE:
            gadget_select_treeview_node((int32_t)a0);
            return 0;

        case SYS_CREATE_PANEL: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_panel(x, y, w, h, current_window);
        }
        case SYS_CREATE_TEXTFIELD: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_textfield(x, y, w, h, current_window);
        }
        case SYS_CREATE_LISTBOX: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_listbox(x, y, w, h, current_window);
        }

        case SYS_GADGET_FREE:
            gadget_free((int32_t)a0);
            return 0;

        case SYS_GADGET_SET_TEXT: {
            const char *t = validate_str(a1, 4096);
            if (t) gadget_set_text((int32_t)a0, t);
            return 0;
        }

        // ---- El reparto de la tarjeta ----
        case SYS_DISK_FISICO:
            return disk_capacidad_fisica();

        case SYS_DISK_PARTICION: {
            uint8_t tipo; uint64_t ini, n;
            if (!disk_particion((int)a0, &tipo, &ini, &n)) return 0;
            return ((uint64_t)tipo << 56) | ((ini & 0xFFFFFFFULL) << 28) | (n & 0xFFFFFFFULL);
        }

        case SYS_DISK_NEMOFS: {
            uint64_t ini, n; bool de_tabla;
            disk_rango_nemofs(&ini, &n, &de_tabla);
            return ((uint64_t)(de_tabla ? 1 : 0) << 63) | ((ini & 0xFFFFFFFULL) << 28) | (n & 0xFFFFFFFULL);
        }

        case SYS_DISK_ESTIRAR: {
            uint64_t nuevos = 0;
            int r = disk_estirar_nemofs(&nuevos);
            return (uint64_t)(uint32_t)r;
        }

        case SYS_NEMOFS_USO: {
            if (!validate_buf(a0, 8)) return 0;
            uint32_t total = 0, usados = 0;
            nemofs_disk_usage(&total, &usados);
            ((uint32_t *)a0)[0] = total;
            ((uint32_t *)a0)[1] = usados;
            return 1;
        }

        case SYS_GADGET_GET_TEXT:
            if (!validate_buf(a1, a2)) return 0;
            return gadget_get_text((int32_t)a0, (char *)a1, (uint32_t)a2);

        case SYS_GADGET_RECT: {
            int32_t x, y; uint32_t w, h;
            if (!gadget_get_rect((int32_t)a0, &x, &y, &w, &h)) return (uint64_t)-1;
            return ((uint64_t)(uint16_t)x << 48) | ((uint64_t)(uint16_t)y << 32) |
                   ((uint64_t)(uint16_t)w << 16) | (uint16_t)h;
        }

        case SYS_GADGET_MOVE:
            gadget_move((int32_t)a0, (int32_t)a1, (int32_t)a2);
            return 0;

        case SYS_GADGET_RESIZE:
            gadget_resize((int32_t)a0, (uint32_t)a1, (uint32_t)a2);
            return 0;

        case SYS_GADGET_SHOW:
            gadget_show((int32_t)a0, a1 != 0);
            return 0;

        case SYS_GADGET_ENABLE:
            gadget_enable((int32_t)a0, a1 != 0);
            return 0;

        case SYS_GADGET_ACTIVATE:
            gadget_activate((int32_t)a0);
            return 0;

        case SYS_GADGET_EVENT:
            if (current_window < 0) return 0;
            return (uint64_t)(int64_t)gadget_poll_event(current_window);

        case SYS_LISTBOX_ADD_ITEM: {
            const char *t = validate_str(a1, 4096);
            if (t) gadget_listbox_add_item((int32_t)a0, t);
            return 0;
        }

        case SYS_LISTBOX_CLEAR:
            gadget_listbox_clear((int32_t)a0);
            return 0;

        case SYS_LISTBOX_SELECTED:
            return (uint64_t)(int64_t)gadget_listbox_selected((int32_t)a0);

        case SYS_LISTBOX_SELECT:
            gadget_listbox_select((int32_t)a0, (int32_t)a1);
            return 0;

        case SYS_LISTBOX_ITEM_COUNT:
            return gadget_listbox_item_count((int32_t)a0);

        case SYS_LISTBOX_ITEM_TEXT:
            if (!validate_buf(a2, a3)) return 0;
            return gadget_listbox_item_text((int32_t)a0, (int32_t)a1, (char *)a2, (uint32_t)a3);

        case SYS_CREATE_TEXTAREA: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_textarea(x, y, w, h, current_window);
        }
        case SYS_TEXTAREA_SET_TEXT: {
            const char *t = validate_str(a1, 65536);
            if (t) gadget_textarea_set_text((int32_t)a0, t);
            return 0;
        }

        case SYS_CREATE_TIMER: {
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            return (uint64_t)(int64_t)gadget_create_timer(current_window, (uint32_t)a0);
        }

        case SYS_PAUSE_TIMER:
            gadget_pause_timer((int32_t)a0);
            return 0;

        case SYS_RESUME_TIMER:
            gadget_resume_timer((int32_t)a0);
            return 0;

        case SYS_RESET_TIMER:
            gadget_reset_timer((int32_t)a0);
            return 0;

        case SYS_TIMER_TICKS:
            return (uint64_t)gadget_timer_ticks((int32_t)a0);

        case SYS_READ_BYTES_BANK: {
            int32_t bank = (int32_t)a0;
            int32_t file_handle = (int32_t)a1 - 100; // deshacemos el +100 de OpenFile/WriteFile
            uint32_t offset = (uint32_t)a2;
            uint32_t count = (uint32_t)a3;
            if (bank < 0 || bank >= MAX_BANKS || !banks[bank].used) return 0;
            if ((uint64_t)offset + count > banks[bank].size) {
                count = (offset < banks[bank].size) ? (banks[bank].size - offset) : 0;
            }
            if (count == 0) return 0;
            return (uint64_t)genfile_read_bytes(file_handle, &bank_data[bank][offset], count);
        }

        case SYS_WRITE_BYTES_BANK: {
            int32_t bank = (int32_t)a0;
            int32_t file_handle = (int32_t)a1 - 100;
            uint32_t offset = (uint32_t)a2;
            uint32_t count = (uint32_t)a3;
            if (bank < 0 || bank >= MAX_BANKS || !banks[bank].used) return (uint64_t)-1;
            if ((uint64_t)offset + count > banks[bank].size) return (uint64_t)-1;
            return genfile_write_bytes(file_handle, &bank_data[bank][offset], count) ? 0 : (uint64_t)-1;
        }

        case SYS_WINDOW_MENU:
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            return (uint64_t)(int64_t)gadget_window_menu(current_window);

        case SYS_CREATE_MENU: {
            const char *t = validate_str(a0, 256);
            return t ? (uint64_t)(int64_t)gadget_create_menu(t, (int32_t)a1, (int32_t)a2) : (uint64_t)-1;
        }

        case SYS_MENU_CHECK:
            gadget_menu_check((int32_t)a0, a1 != 0);
            return 0;

        case SYS_MENU_ENABLE:
            gadget_menu_enable((int32_t)a0, a1 != 0);
            return 0;

        case SYS_MENU_GET_TAG:
            return (uint64_t)(int64_t)gadget_menu_get_tag((int32_t)a0);

        case SYS_PEEK_EVENT:
            if (current_window < 0) return 0;
            return (uint64_t)(int64_t)gadgets_peek_raw_event(current_window);

        case SYS_FLUSH_EVENTS:
            if (current_window < 0) return 0;
            gadgets_flush_events(current_window, (int32_t)a0);
            return 0;

        case SYS_BUTTON_STATE:
            return gadget_button_state((int32_t)a0) ? 1 : 0;

        case SYS_SET_BUTTON_STATE:
            gadget_set_button_state((int32_t)a0, a1 != 0);
            return 0;

        case SYS_HOTKEY_EVENT: {
            uint16_t rawkey = (uint16_t)(a0 >> 8);
            uint8_t modifier = (uint8_t)(a0 & 0xFF);
            gadget_hotkey_event(rawkey, modifier, (int32_t)a1, (int32_t)a2, (int32_t)a3);
            return 0;
        }

        case SYS_GADGET_ENABLED:
            return gadget_is_enabled((int32_t)a0) ? 1 : 0;

        case SYS_TEXTAREA_ADD_TEXT: {
            const char *t = validate_str(a1, 65536);
            if (t) gadget_textarea_add_text((int32_t)a0, t);
            return 0;
        }

        case SYS_TEXTAREA_LEN:
            return gadget_textarea_len((int32_t)a0, (int32_t)a1);

        case SYS_TEXTAREA_LINE_LEN:
            return gadget_textarea_line_len((int32_t)a0, (int32_t)a1);

        case SYS_TEXTAREA_LINE_OF_CHAR:
            return (uint64_t)(int64_t)gadget_textarea_line_of_char((int32_t)a0, (int32_t)a1);

        case SYS_TEXTAREA_GET_TEXT:
            if (!validate_buf(a3, a4)) return 0;
            gadget_textarea_get_text((int32_t)a0, (int32_t)a1, (int32_t)a2, (char *)a3, (uint32_t)a4);
            return 0;

        case SYS_ACTIVATE_WINDOW:
            wm_activate_window((int32_t)a0);
            return 0;

        case SYS_ACTIVE_WINDOW:
            return (uint64_t)(int64_t)wm_get_focused_window();

        case SYS_MAXIMIZE_WINDOW:
            wm_maximize_window((int32_t)a0);
            return 0;

        case SYS_MINIMIZE_WINDOW:
            wm_minimize_window((int32_t)a0);
            return 0;

        case SYS_WINDOW_MAXIMIZED:
            return wm_window_maximized((int32_t)a0) ? 1 : 0;

        case SYS_WINDOW_MINIMIZED:
            return wm_window_minimized((int32_t)a0) ? 1 : 0;

        case SYS_SET_MIN_WINDOW_SIZE:
            wm_set_min_window_size((int32_t)a0, (uint32_t)a1, (uint32_t)a2);
            return 0;

        case SYS_GADGET_INSERT_ITEM: {
            const char *t = validate_str(a2, 4096);
            if (t) gadget_listbox_insert_item((int32_t)a0, (int32_t)a1, t);
            return 0;
        }

        case SYS_GADGET_REMOVE_ITEM:
            gadget_listbox_remove_item((int32_t)a0, (int32_t)a1);
            return 0;

        case SYS_GADGET_MODIFY_ITEM: {
            const char *t = validate_str(a2, 4096);
            if (t) gadget_listbox_modify_item((int32_t)a0, (int32_t)a1, t);
            return 0;
        }

        default:
            return (uint64_t)-1;
    }
}
