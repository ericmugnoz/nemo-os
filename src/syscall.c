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
#include "fat.h"
#include "sound.h"

// Tabla de "handles" para ARCHIVOS FAT -- el driver FAT no tiene
// inodos persistentes como NemoFS (localiza archivos por nombre cada
// vez), asi que aqui les damos un numero pequeño y estable para que
// los programas los puedan usar igual que un inodo de NemoFS.
#define MAX_FAT_HANDLES 8
typedef struct {
    bool used;
    bool has_entry;              // true si ya existe en disco
    fat_dirent_t entry;          // valido solo si has_entry
    char pending_name[FAT_NAME_LEN]; // usado si !has_entry, para crearlo al escribir
} fat_handle_t;
static fat_handle_t fat_handles[MAX_FAT_HANDLES];

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
        if (!fat_handles[i].used) {
            fat_handles[i].used = true;
            fat_handles[i].has_entry = exists;
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
#define MAX_READ_FILES 8
#define READ_FILE_BUF_SIZE 16384
static bool rf_used[MAX_READ_FILES];
static uint32_t rf_pos[MAX_READ_FILES];
static uint32_t rf_len[MAX_READ_FILES];
static char rf_buf[MAX_READ_FILES][READ_FILE_BUF_SIZE];

// Busca el archivo primero en la raiz, y si no esta ahi, dentro de
// DOCUMENTOS -- los dos sitios donde suele vivir un archivo de texto.
static int32_t readfile_resolve_inode(const char *filename) {
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
    for (int i = 0; i < MAX_READ_FILES; i++) if (!rf_used[i]) { slot = i; break; }
    if (slot < 0) return -1;

    int32_t inode = readfile_resolve_inode(filename);
    if (inode < 0) return -1;

    int32_t bytes = nemofs_read_file((uint32_t)inode, rf_buf[slot], READ_FILE_BUF_SIZE - 1);
    if (bytes < 0) return -1;

    rf_buf[slot][bytes] = '\0';
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
    if (handle < 0 || handle >= MAX_READ_FILES) return;
    rf_used[handle] = false;
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
static uint8_t genfile_buf[MAX_GEN_FILES][GEN_FILE_BUF_SIZE];

// mode: 0=OpenFile (busca en NemoFS raiz/DOCUMENTOS y luego FAT; si
// no existe en ningun sitio, FALLA -- BlitzPlus real no crea archivos
// con OpenFile), 1=WriteFile (siempre vacio, sin importar si ya
// existia -- pero respeta DONDE vivia, para no duplicarlo en otro
// volumen; SI crea el archivo si no existia en ningun sitio).
static int32_t genfile_open(const char *name, uint32_t mode) {
    int32_t slot = -1;
    for (int i = 0; i < MAX_GEN_FILES; i++) if (!genfiles[i].used) { slot = i; break; }
    if (slot < 0) return -1;

    genfiles[slot].pos = 0;
    genfiles[slot].dirty = false;
    int n = 0;
    while (name[n] != '\0' && n < 63) { genfiles[slot].name[n] = name[n]; n++; }
    genfiles[slot].name[n] = '\0';

    int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, name);
    uint32_t parent_used = NEMOFS_ROOT_INODE;
    if (idx < 0) {
        int32_t docs = nemofs_find_child(NEMOFS_ROOT_INODE, "DOCUMENTOS");
        if (docs >= 0) {
            int32_t idx2 = nemofs_find_child((uint32_t)docs, name);
            if (idx2 >= 0) { idx = idx2; parent_used = (uint32_t)docs; }
        }
    }

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
    if (fat_find_root(name, &entry)) {
        // Ya existia en FAT.
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

    // No existia en ningun sitio -- WriteFile SI lo crea vacio.
    genfiles[slot].volume = VOLUME_NEMOFS;
    genfiles[slot].nemofs_inode = -1;
    genfiles[slot].nemofs_parent = NEMOFS_ROOT_INODE;
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

static int32_t dir_open(uint32_t parent) {
    int32_t slot = -1;
    for (int i = 0; i < MAX_DIR_HANDLES; i++) if (!dir_iters[i].used) { slot = i; break; }
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
static int32_t file_size_by_name(const char *name) {
    int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, name);
    uint32_t parent = NEMOFS_ROOT_INODE;
    if (idx < 0) {
        int32_t docs = nemofs_find_child(NEMOFS_ROOT_INODE, "DOCUMENTOS");
        if (docs >= 0) {
            int32_t idx2 = nemofs_find_child((uint32_t)docs, name);
            if (idx2 >= 0) { idx = idx2; parent = (uint32_t)docs; }
        }
    }
    if (idx >= 0) {
        // No tenemos una consulta directa "dame el tamaño de este
        // inodo" -- reutilizamos el listado de su carpeta padre y
        // buscamos la entrada por inodo, igual que en file_type_by_name.
        static nemofs_dirent_t tmp[64];
        uint32_t total = nemofs_list_dir(parent, tmp, 64);
        uint32_t n = total < 64 ? total : 64;
        for (uint32_t i = 0; i < n; i++) {
            if ((int32_t)tmp[i].inode == idx) return (int32_t)tmp[i].size;
        }
        return -1;
    }
    fat_dirent_t entry;
    if (fat_find_root(name, &entry)) return (int32_t)entry.size;
    return -1;
}

static int32_t file_type_by_name(const char *name) {
    int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, name);
    uint32_t parent = NEMOFS_ROOT_INODE;
    if (idx < 0) {
        int32_t docs = nemofs_find_child(NEMOFS_ROOT_INODE, "DOCUMENTOS");
        if (docs >= 0) {
            int32_t idx2 = nemofs_find_child((uint32_t)docs, name);
            if (idx2 >= 0) { idx = idx2; parent = (uint32_t)docs; }
        }
    }
    if (idx >= 0) {
        static nemofs_dirent_t tmp[64];
        uint32_t total = nemofs_list_dir(parent, tmp, 64);
        uint32_t n = total < 64 ? total : 64;
        for (uint32_t i = 0; i < n; i++) {
            if ((int32_t)tmp[i].inode == idx) return (int32_t)tmp[i].type;
        }
        return 1; // por si acaso, asumimos archivo
    }
    fat_dirent_t entry;
    if (fat_find_root(name, &entry)) return entry.is_dir ? NEMOFS_TYPE_DIR : NEMOFS_TYPE_FILE;
    return 0;
}

static void delete_anywhere(const char *name, int32_t *out_ok) {
    int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, name);
    if (idx >= 0) { *out_ok = nemofs_delete(NEMOFS_ROOT_INODE, name) ? 0 : -1; return; }
    int32_t docs = nemofs_find_child(NEMOFS_ROOT_INODE, "DOCUMENTOS");
    if (docs >= 0) {
        int32_t idx2 = nemofs_find_child((uint32_t)docs, name);
        if (idx2 >= 0) { *out_ok = nemofs_delete((uint32_t)docs, name) ? 0 : -1; return; }
    }
    *out_ok = fat_delete_file(name) ? 0 : -1;
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
#define MAX_IMAGES 16

// Origen de dibujo (Origin/Viewport) -- desplazamiento que se suma a
// TODAS las coordenadas de las syscalls de dibujo (Rect/Text/Ovalo/
// Imagen), asi que Plot/Rect/Line/Oval/Text/DrawImage lo respetan
// automaticamente sin que el compilador tenga que tocar cada una por
// separado -- Line en particular ya dibuja llamando repetidamente a
// SYS_DRAW_RECT, asi que tambien queda cubierta gratis.
static int32_t g_origin_x = 0, g_origin_y = 0;

// ImageBuffer(handle): redirige el dibujo hacia una imagen en vez de
// la ventana -- -1 significa "dibujar en la ventana", como siempre.
// Cubre Rect/Plot/Line/Cls (todos pasan por SYS_DRAW_RECT) y Oval.
// Text y DrawImage anidado siguen yendo SIEMPRE a la ventana --
// limitacion documentada, cubrir esos tambien pediria duplicar mucho
// mas codigo de bajo nivel.
static int32_t g_draw_target_image = -1;

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
static int32_t g_draw_target_canvas = -1; // id de gadget Canvas activo, -1 = ninguno

// Viewport(x,y,w,h): recorta el dibujo a un rectangulo, SEPARADO de
// Origin (que solo desplaza, no recorta) -- coordenadas LOCALES de
// la ventana, antes de sumar menu_off. g_viewport_active=false
// significa "sin recorte", toda la ventana como siempre.
static bool g_viewport_active = false;
static int32_t g_viewport_x = 0, g_viewport_y = 0;
static uint32_t g_viewport_w = 0, g_viewport_h = 0;

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
#define IMAGE_MAX_DIM 256
#define IMAGE_SLOT_BYTES (IMAGE_MAX_DIM * IMAGE_MAX_DIM * 4) // 256KB por hueco -- 4MB en total, de sobra con los 512MB de RAM de QEMU
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
__attribute__((aligned(4096)))
static uint8_t image_pixels[MAX_IMAGES][IMAGE_SLOT_BYTES];

// AutoMidHandle(true) hace que LoadImage/CreateImage centren el punto
// de agarre automaticamente (ancho/2, alto/2) segun se crean, sin
// tener que llamar a MidHandle a mano cada vez.
static bool g_auto_mid_handle = false;

// Buffer temporal para leer el archivo ENTERO (cabecera + pixeles)
// antes de repartirlo en el hueco del pool.
static uint8_t image_load_buf[12 + IMAGE_SLOT_BYTES];

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
    for (int i = 0; i < MAX_SOUNDS; i++) if (!sounds[i].used) { slot = i; break; }
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

static void sound_play(int32_t handle) {
    if (handle < 0 || handle >= MAX_SOUNDS || !sounds[handle].used) return;

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

    sound_play_blocking(play_scratch_b, frames);
}

static int32_t image_load(const char *filename) {
    int32_t slot = -1;
    for (int i = 0; i < MAX_IMAGES; i++) if (!images[i].used) { slot = i; break; }
    if (slot < 0) return -1; // sin huecos libres

    int32_t inode = readfile_resolve_inode(filename);
    if (inode < 0) return -1;

    int32_t bytes = nemofs_read_file((uint32_t)inode, image_load_buf, sizeof(image_load_buf));
    if (bytes < 12) return -1; // ni siquiera cabe la cabecera

    if (image_load_buf[0] != 'N' || image_load_buf[1] != 'I' || image_load_buf[2] != 'M' || image_load_buf[3] != 'G') {
        return -1; // no es nuestro formato
    }
    uint32_t width = (uint32_t)image_load_buf[4] | ((uint32_t)image_load_buf[5] << 8) |
                      ((uint32_t)image_load_buf[6] << 16) | ((uint32_t)image_load_buf[7] << 24);
    uint32_t height = (uint32_t)image_load_buf[8] | ((uint32_t)image_load_buf[9] << 8) |
                       ((uint32_t)image_load_buf[10] << 16) | ((uint32_t)image_load_buf[11] << 24);

    if (width == 0 || height == 0 || width > IMAGE_MAX_DIM || height > IMAGE_MAX_DIM) return -1;
    uint32_t needed = width * height * 4;
    if ((uint32_t)bytes < 12 + needed) return -1; // el archivo no trae todos los pixeles que dice su cabecera

    for (uint32_t i = 0; i < needed; i++) image_pixels[slot][i] = image_load_buf[12 + i];

    images[slot].width = width;
    images[slot].height = height;
    images[slot].handle_x = g_auto_mid_handle ? (int32_t)(width / 2) : 0;
    images[slot].handle_y = g_auto_mid_handle ? (int32_t)(height / 2) : 0;
    images[slot].used = true;
    return slot;
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
static uint8_t tform_tmp_buf[IMAGE_SLOT_BYTES]; // estatico -- 256KB es demasiado para la pila
static void tform_image(int32_t handle, int32_t a, int32_t b, int32_t c, int32_t d) {
    if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return;
    uint32_t w = images[handle].width, h = images[handle].height;
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
#define MAX_FONTS 16
typedef struct {
    bool used;
    char name[32];
    int32_t height;
    bool bold, italic, underlined;
} font_slot_t;
static font_slot_t fonts[MAX_FONTS];
static int32_t g_current_font = -1; // indice (no handle) de la fuente activada con SetFont, -1 = ninguna

// Aunque no rasterizamos TrueType de verdad, SI podemos dar un tamaño
// y negrita REALES: wm_content_draw_string ya admite un factor de
// escala entero (cada pixel de la fuente 5x7 se dibuja como un
// bloque escala x escala), y la negrita se consigue dibujando el
// texto DOS VECES con un desplazamiento de 1 pixel -- sin necesitar
// autoria manual de glifos nuevos. Solo afecta al comando Text (lo
// que dibuja el PROGRAMA), no a los botones/menus/etc del propio
// SO, que siguen usando la fuente de sistema a escala 1 siempre.
static uint32_t g_font_scale = 1;
static bool g_font_bold = false;

static int32_t font_load(const char *name, int32_t height, bool bold, bool italic, bool underlined) {
    for (int i = 0; i < MAX_FONTS; i++) {
        if (!fonts[i].used) {
            fonts[i].used = true;
            int j = 0;
            while (name[j] != '\0' && j < 31) { fonts[i].name[j] = name[j]; j++; }
            fonts[i].name[j] = '\0';
            fonts[i].height = height;
            fonts[i].bold = bold;
            fonts[i].italic = italic;
            fonts[i].underlined = underlined;
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
    for (int i = 0; i < MAX_IMAGES; i++) if (!images[i].used) { slot = i; break; }
    if (slot < 0) return -1;

    uint32_t needed = width * height * 4;
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
    images[handle].used = false;
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
    static uint8_t resize_temp[IMAGE_SLOT_BYTES];
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
    uint32_t needed_new = new_w * new_h * 4;
    for (uint32_t i = 0; i < needed_new; i++) image_pixels[handle][i] = resize_temp[i];
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

    static uint8_t rotate_temp[IMAGE_SLOT_BYTES];
    uint32_t needed_new = new_w * new_h * 4;
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
    for (uint32_t i = 0; i < needed_new; i++) image_pixels[handle][i] = rotate_temp[i];

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
    for (int i = 0; i < MAX_IMAGES; i++) if (!images[i].used) { slot = i; break; }
    if (slot < 0) return -1;
    uint32_t needed = images[handle].width * images[handle].height * 4;
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
    static uint8_t save_buf[12 + IMAGE_SLOT_BYTES];
    save_buf[0] = 'N'; save_buf[1] = 'I'; save_buf[2] = 'M'; save_buf[3] = 'G';
    uint32_t w = images[handle].width, h = images[handle].height;
    save_buf[4] = (uint8_t)w; save_buf[5] = (uint8_t)(w >> 8); save_buf[6] = (uint8_t)(w >> 16); save_buf[7] = (uint8_t)(w >> 24);
    save_buf[8] = (uint8_t)h; save_buf[9] = (uint8_t)(h >> 8); save_buf[10] = (uint8_t)(h >> 16); save_buf[11] = (uint8_t)(h >> 24);
    uint32_t needed = w * h * 4;
    for (uint32_t i = 0; i < needed; i++) save_buf[12 + i] = image_pixels[handle][i];

    int32_t idx = nemofs_find_child(NEMOFS_ROOT_INODE, filename);
    if (idx < 0) idx = nemofs_create(NEMOFS_ROOT_INODE, filename, NEMOFS_TYPE_FILE);
    if (idx < 0) return false;
    return nemofs_write_file((uint32_t)idx, save_buf, 12 + needed);
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
__attribute__((aligned(16)))
static uint8_t bank_data[MAX_BANKS][BANK_MAX_SIZE];

static int32_t bank_create(uint32_t size) {
    if (size > BANK_MAX_SIZE) return -1;
    int32_t slot = -1;
    for (int i = 0; i < MAX_BANKS; i++) if (!banks[i].used) { slot = i; break; }
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
static bool g_buffer_locked = false;
static int32_t g_locked_buffer_id = 0; // 0=ventana, N=imagen N-1 (misma convencion que SetBuffer)
static uint32_t g_locked_width = 0, g_locked_height = 0;
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
    for (int i = 0; i < MAX_IMAGES; i++) if (!images[i].used) { slot = i; break; }
    if (slot < 0) return -1;
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
#define MAX_CONSOLE_WINDOWS 8 // debe coincidir con MAX_WINDOWS de wm.c
typedef struct {
    char buf[CONSOLE_QUEUE_SIZE];
    uint32_t read_pos, write_pos;
} console_queue_t;
static console_queue_t console_queues[MAX_CONSOLE_WINDOWS];

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

uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    int32_t current_window = task_get_current_window();

    switch (num) {

        // -- Sistema --

        case SYS_EXIT: {
            if (current_window >= 0) {
                gadgets_free_window(current_window);
                wm_destroy_window(current_window);
            }
            return 0;
        }

        case SYS_SLEEP: {
            uint64_t ticks_to_wait = a0;
            uint64_t start = timer_get_ticks();
            while (timer_get_ticks() - start < ticks_to_wait) {
                task_yield();
            }
            return 0;
        }

        case SYS_GET_TICKS:
            return timer_get_ticks();

        case SYS_LAUNCH_PROGRAM: {
            const char *name = (const char *)a0;
            const char *arg = (a1 != 0) ? (const char *)a1 : "";
            uint32_t search_dir = (uint32_t)a2;
            wm_request_launch(name, arg, current_window, search_dir);
            return 0;
        }

        case SYS_GET_LAUNCH_ARG: {
            char *out = (char *)a0;
            uint32_t max_len = (uint32_t)a1;
            const char *arg = task_get_launch_arg();
            uint32_t i = 0;
            while (arg[i] != '\0' && i < max_len - 1) { out[i] = arg[i]; i++; }
            out[i] = '\0';
            return i;
        }

        case SYS_CLIPBOARD_SET: {
            const char *src = (const char *)a0;
            uint32_t len = (uint32_t)a1;
            if (len > sizeof(clipboard_buf)) len = sizeof(clipboard_buf);
            for (uint32_t i = 0; i < len; i++) clipboard_buf[i] = src[i];
            clipboard_len = len;
            return 0;
        }

        case SYS_CLIPBOARD_GET: {
            char *dst = (char *)a0;
            uint32_t max_len = (uint32_t)a1;
            uint32_t len = (clipboard_len < max_len) ? clipboard_len : max_len;
            for (uint32_t i = 0; i < len; i++) dst[i] = clipboard_buf[i];
            return len;
        }

        // -- Texto / consola --

        case SYS_WRITE_CHAR:
            uart_putc((char)a0);
            return 0;

        case SYS_WRITE_STRING: {
            int32_t console_target = task_get_console_window();
            if (console_target >= 0) {
                console_queue_write(console_target, (const char *)a0);
            } else {
                uart_puts((const char *)a0);
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

        case SYS_READ_CHAR: {
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
                if (current_window == wm_get_focused_window() && input_read_char(&c)) {
                    return (uint64_t)(uint8_t)c;
                }
                task_yield();
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
            task_yield();
            return 0;
        }

        // -- Archivos --

        case SYS_FILE_OPEN: {
            const char *name = (const char *)a0;
            uint32_t parent = (uint32_t)a1;
            uint32_t volume = (uint32_t)a2;
            if (volume == VOLUME_FAT) {
                return (uint64_t)(int64_t)fat_handle_open(parent, name);
            }
            int32_t idx = nemofs_find_child(parent, name);
            if (idx < 0) idx = nemofs_create(parent, name, NEMOFS_TYPE_FILE);
            return (uint64_t)(int64_t)idx;
        }

        case SYS_FILE_READ: {
            uint32_t id = (uint32_t)a0;
            void *buf = (void *)a1;
            uint32_t max_size = (uint32_t)a2;
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
            const void *buf = (const void *)a1;
            uint32_t size = (uint32_t)a2;
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
                return 0;
            }
            return nemofs_write_file(id, buf, size) ? 0 : (uint64_t)-1;
        }

        case SYS_FILE_LIST: {
            uint32_t parent = (uint32_t)a0;
            uint8_t *out = (uint8_t *)a1;
            uint32_t max_entries = (uint32_t)a2;
            uint32_t volume = (uint32_t)a3;

            if (volume == VOLUME_FAT) {
                // 'parent' puede venir con el desplazamiento (ver
                // FAT_DIR_CLUSTER_OFFSET) si el programa esta
                // navegando dentro de una carpeta abierta antes, o
                // ser 0 (raiz).
                uint32_t real_parent = (parent >= FAT_DIR_CLUSTER_OFFSET) ? (parent - FAT_DIR_CLUSTER_OFFSET) : 0;
                static fat_dirent_t tmp[64];
                uint32_t cap = (max_entries < 64) ? max_entries : 64;
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

            static nemofs_dirent_t tmp[64];
            uint32_t cap = (max_entries < 64) ? max_entries : 64;
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
            const char *name = (const char *)a0;
            uint32_t parent = (uint32_t)a1;
            uint32_t volume = (uint32_t)a2;
            if (volume == VOLUME_FAT) return (uint64_t)-1; // FAT v1 no tiene subcarpetas
            int32_t idx = nemofs_create(parent, name, NEMOFS_TYPE_DIR);
            return (uint64_t)(int64_t)idx;
        }

        case SYS_FILE_DELETE: {
            const char *name = (const char *)a0;
            uint32_t parent = (uint32_t)a1;
            uint32_t volume = (uint32_t)a2;
            if (volume == VOLUME_FAT) return fat_delete_file(name) ? 0 : (uint64_t)-1;
            return nemofs_delete(parent, name) ? 0 : (uint64_t)-1;
        }

        case SYS_FILE_CLIPBOARD_SET: {
            const char *name = (const char *)a0;
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
            char *out = (char *)a0;
            uint32_t max_len = (uint32_t)a1;
            uint32_t i = 0;
            while (file_clip_name[i] != '\0' && i < max_len - 1) { out[i] = file_clip_name[i]; i++; }
            out[i] = '\0';
            return ((uint64_t)file_clip_parent << 32) | file_clip_volume;
        }

        case SYS_DEBUG_LOG:
            uart_puts("DEBUG: ");
            uart_puts((const char *)a0);
            uart_puts("\n");
            return 0;

        // -- Graficos y ventana --

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
            wm_request_redraw();
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
            const char *str = (const char *)a2;
            uint32_t color = (uint32_t)a3;

            int32_t cx0, cy0, cx1, cy1;
            get_clip_bounds(ww, wh, menu_off, &cx0, &cy0, &cx1, &cy1);
            if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1) return (uint64_t)-1;

            wm_content_draw_string(current_window, (uint32_t)x, (uint32_t)y, str, color, g_font_scale);
            if (g_font_bold) {
                // Negrita "falsa": se dibuja una segunda vez desplazada
                // 1 pixel a la derecha -- efecto real de engrosado,
                // sin necesitar un juego de glifos en negrita aparte.
                wm_content_draw_string(current_window, (uint32_t)x + 1, (uint32_t)y, str, color, g_font_scale);
            }
            wm_request_redraw();
            return 0;
        }

        case SYS_DRAW_ICON: {
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            uint32_t menu_off = gadgets_menubar_height(current_window);
            int32_t x = (int32_t)a0, y = (int32_t)a1 + (int32_t)menu_off;
            int icon_id = (int)a2;
            const uint8_t *rgba = icon_get_rgba(icon_id);
            if (!rgba || x < 0 || y < (int32_t)menu_off) return (uint64_t)-1;

            wm_content_blit_icon(current_window, (uint32_t)x, (uint32_t)y, ICON_SIZE, rgba);
            wm_request_redraw();
            return 0;
        }

        case SYS_GET_WINDOW_SIZE: {
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
            wm_request_redraw();
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
            return readfile_read_bytes((int32_t)a0, (uint8_t *)a1, (uint32_t)a2);

        case SYS_GET_MOUSE_Z:
            return (uint64_t)(int64_t)mouse_wheel_total();

        case SYS_FLUSH_MOUSE:
            input_flush_mouse();
            return 0;

        case SYS_SET_TITLE: {
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            wm_set_title(current_window, (const char *)a0);
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

        case SYS_KEY_DOWN:
            return key_is_down((uint16_t)a0) ? 1 : 0;

        case SYS_KEY_HIT:
            return key_was_hit((uint16_t)a0);

        case SYS_GET_KEY: {
            uint16_t code;
            return input_read_scancode(&code) ? (uint64_t)code : 0;
        }

        case SYS_FLUSH_KEYS:
            input_flush_keys();
            return 0;

        case SYS_MOUSE_HIT:
            return mouse_button_was_hit((int)a0);

        case SYS_GET_MOUSE_SPEED: {
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

        case SYS_GENFILE_OPEN:
            return (uint64_t)(int64_t)genfile_open((const char *)a0, (uint32_t)a1);

        case SYS_GENFILE_READ_BYTES:
            return genfile_read_bytes((int32_t)a0, (uint8_t *)a1, (uint32_t)a2);

        case SYS_GENFILE_WRITE_BYTES:
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
            return dir_next((int32_t)a0, (char *)a1, (uint32_t)a2);

        case SYS_DIR_CLOSE:
            dir_close((int32_t)a0);
            return 0;

        case SYS_FILE_SIZE_BY_NAME:
            return (uint64_t)(int64_t)file_size_by_name((const char *)a0);

        case SYS_FILE_TYPE_BY_NAME:
            return (uint64_t)(int64_t)file_type_by_name((const char *)a0);

        case SYS_FIND_CHILD:
            return (uint64_t)(int64_t)nemofs_find_child((uint32_t)a1, (const char *)a0);

        case SYS_DELETE_ANYWHERE: {
            int32_t ok = -1;
            delete_anywhere((const char *)a0, &ok);
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

        case SYS_LOAD_FONT:
            return (uint64_t)(int64_t)font_load((const char *)a0, (int32_t)a1, a2 != 0, a3 != 0, a4 != 0);

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
            } else {
                // SetFont(0) o handle invalido -- vuelve a la fuente
                // de sistema por defecto (escala 1, sin negrita).
                g_current_font = -1;
                g_font_scale = 1;
                g_font_bold = false;
            }
            return 0;
        }

        case SYS_FONT_NAME: {
            int32_t idx = (int32_t)a0 - 1;
            char *out = (char *)a1;
            uint32_t max_len = (uint32_t)a2;
            if (max_len == 0) return 0;
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
            return fonts[idx].italic ? 3 : 1;
        }

        case SYS_FONT_WIDTH:
            // Ancho REAL en pantalla del caracter mas ancho -- ya
            // refleja la escala activa (SetFont), no siempre 5.
            return (uint64_t)(FONT_WIDTH * g_font_scale);

        case SYS_FONT_HEIGHT:
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
            char *out = (char *)a1;
            uint32_t max_len = (uint32_t)a2;
            if (max_len == 0) return 0;
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
            int32_t image_handle = image_load((const char *)a1);
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

        case SYS_LOAD_SOUND:
            return (uint64_t)(int64_t)sound_load((const char *)a0);

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
            int32_t id = exec_program((const char *)a0, (const char *)0);
            return id >= 0 ? 1 : 0;
        }

        case SYS_CREATE_PROCESS: {
            // command$ puede traer argumentos separados por un
            // espacio (ej. "editor.pro archivo.txt") -- partimos en
            // el PRIMER espacio, igual que hace la shell con 'run'.
            const char *cmd = (const char *)a0;
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
            wm_request_redraw();
            return ok ? 0 : (uint64_t)-1;
        }

        case SYS_LOAD_IMAGE:
            return (uint64_t)(int64_t)image_load((const char *)a0);

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

        case SYS_SAVE_IMAGE:
            return image_save((int32_t)a0, (const char *)a1) ? 0 : (uint64_t)-1;

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

            int32_t cx0, cy0, cx1, cy1;
            get_clip_bounds(ww, wh, menu_off, &cx0, &cy0, &cx1, &cy1);
            int32_t bx, by; uint32_t bw, bh, sdx, sdy;
            if (clip_blit(x, y, rw, rh, cx0, cy0, cx1, cy1, &bx, &by, &bw, &bh, &sdx, &sdy)) {
                const uint8_t *src = &image_pixels[handle][((ry + sdy) * images[handle].width + (rx + sdx)) * 4];
                wm_content_blit_image_rect(current_window, (uint32_t)bx, (uint32_t)by, bw, bh, images[handle].width, src, solid, images[handle].has_mask, images[handle].mask_color);
            }
            wm_request_redraw();
            return 0;
        }

        case SYS_LOAD_ANIM_IMAGE: {
            uint32_t cell_w = (uint32_t)(a1 >> 16), cell_h = (uint32_t)(a1 & 0xFFFF);
            return (uint64_t)(int64_t)image_load_anim((const char *)a0, cell_w, cell_h, (uint32_t)a2, (uint32_t)a3);
        }

        case SYS_DRAW_IMAGE: {
            int32_t handle = (int32_t)a0;
            if (handle < 0 || handle >= MAX_IMAGES || !images[handle].used) return (uint64_t)-1;
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
            wm_request_redraw();
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
            char *out_name = (char *)a1;
            uint32_t out_max = (uint32_t)a2;
            return (uint64_t)(int64_t)dialog_open_file(current_window, start_dir, out_name, out_max);
        }

        case SYS_SAVE_FILE_DIALOG: {
            if (current_window < 0) return (uint64_t)-1;
            uint32_t start_dir = (uint32_t)a0;
            char *out_name = (char *)a1;
            uint32_t out_max = (uint32_t)a2;
            return (uint64_t)(int64_t)dialog_save_file(current_window, start_dir, out_name, out_max);
        }

        case SYS_CREATE_WINDOW: {
            current_window = task_ensure_window();
            if (current_window < 0) return (uint64_t)-1;
            const char *title = (const char *)a0;
            int32_t x = (int32_t)a1, y = (int32_t)a2;
            uint32_t w = (uint32_t)a3, h = (uint32_t)a4;
            wm_configure_window(current_window, title, x, y, w, h);
            wm_set_event_mode(current_window, true);
            wm_request_redraw();
            return (uint64_t)(int64_t)current_window;
        }

        case SYS_READ_FILE_OPEN:
            return (uint64_t)(int64_t)readfile_open((const char *)a0);

        case SYS_READ_FILE_LINE:
            return (uint64_t)readfile_line((int32_t)a0, (char *)a1, (uint32_t)a2);

        case SYS_READ_FILE_EOF:
            return readfile_eof((int32_t)a0) ? 1 : 0;

        case SYS_READ_FILE_CLOSE:
            readfile_close((int32_t)a0);
            return 0;

        // -- Gadgets estilo BlitzPlus --

        case SYS_CREATE_BUTTON: {
            current_window = task_ensure_window();
            const char *text = (const char *)a0;
            int32_t x = (int32_t)a1, y = (int32_t)a2;
            uint32_t w = (uint32_t)(a3 >> 16), h = (uint32_t)(a3 & 0xFFFF);
            uint32_t style = (uint32_t)a4;
            return (uint64_t)(int64_t)gadget_create_button(text, x, y, w, h, current_window, style);
        }
        case SYS_CREATE_LABEL: {
            current_window = task_ensure_window();
            const char *text = (const char *)a0;
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
            return (uint64_t)(int64_t)image_load((const char *)a0);

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
            int32_t image_handle = image_load((const char *)a0);
            int32_t x = (int32_t)a1, y = (int32_t)a2;
            uint32_t w = (uint32_t)(a3 >> 16), h = (uint32_t)(a3 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_toolbar(image_handle, x, y, w, h, current_window);
        }

        case SYS_ENABLE_TOOLBAR_ITEM:
            gadget_enable_toolbar_item((int32_t)a0, (int32_t)a1, a2 != 0);
            return 0;

        case SYS_SET_TOOLBAR_TIPS:
            gadget_set_toolbar_tips((int32_t)a0, (const char *)a1);
            return 0;

        case SYS_CREATE_TREEVIEW: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_treeview(x, y, w, h, current_window);
        }

        case SYS_TREEVIEW_ROOT:
            return (uint64_t)(int64_t)gadget_treeview_root((int32_t)a0);

        case SYS_ADD_TREEVIEW_NODE:
            return (uint64_t)(int64_t)gadget_add_treeview_node((const char *)a0, (int32_t)a1);

        case SYS_INSERT_TREEVIEW_NODE:
            return (uint64_t)(int64_t)gadget_insert_treeview_node((int32_t)a0, (const char *)a1, (int32_t)a2);

        case SYS_MODIFY_TREEVIEW_NODE:
            gadget_modify_treeview_node((int32_t)a0, (const char *)a1);
            return 0;

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

        case SYS_GADGET_SET_TEXT:
            gadget_set_text((int32_t)a0, (const char *)a1);
            return 0;

        case SYS_GADGET_GET_TEXT:
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

        case SYS_LISTBOX_ADD_ITEM:
            gadget_listbox_add_item((int32_t)a0, (const char *)a1);
            return 0;

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
            return gadget_listbox_item_text((int32_t)a0, (int32_t)a1, (char *)a2, (uint32_t)a3);

        case SYS_CREATE_TEXTAREA: {
            current_window = task_ensure_window();
            int32_t x = (int32_t)a0, y = (int32_t)a1;
            uint32_t w = (uint32_t)(a2 >> 16), h = (uint32_t)(a2 & 0xFFFF);
            return (uint64_t)(int64_t)gadget_create_textarea(x, y, w, h, current_window);
        }
        case SYS_TEXTAREA_SET_TEXT:
            gadget_textarea_set_text((int32_t)a0, (const char *)a1);
            return 0;

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

        case SYS_CREATE_MENU:
            return (uint64_t)(int64_t)gadget_create_menu((const char *)a0, (int32_t)a1, (int32_t)a2);

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

        case SYS_TEXTAREA_ADD_TEXT:
            gadget_textarea_add_text((int32_t)a0, (const char *)a1);
            return 0;

        case SYS_TEXTAREA_LEN:
            return gadget_textarea_len((int32_t)a0, (int32_t)a1);

        case SYS_TEXTAREA_LINE_LEN:
            return gadget_textarea_line_len((int32_t)a0, (int32_t)a1);

        case SYS_TEXTAREA_LINE_OF_CHAR:
            return (uint64_t)(int64_t)gadget_textarea_line_of_char((int32_t)a0, (int32_t)a1);

        case SYS_TEXTAREA_GET_TEXT:
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

        case SYS_GADGET_INSERT_ITEM:
            gadget_listbox_insert_item((int32_t)a0, (int32_t)a1, (const char *)a2);
            return 0;

        case SYS_GADGET_REMOVE_ITEM:
            gadget_listbox_remove_item((int32_t)a0, (int32_t)a1);
            return 0;

        case SYS_GADGET_MODIFY_ITEM:
            gadget_listbox_modify_item((int32_t)a0, (int32_t)a1, (const char *)a2);
            return 0;

        default:
            return (uint64_t)-1;
    }
}
