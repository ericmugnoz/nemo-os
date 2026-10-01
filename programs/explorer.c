// explorer.c — Nemo OS
//
// Explorador de archivos v4: arbol de carpetas de verdad a la
// izquierda (expandir/contraer, como cualquier explorador clasico) y
// una rejilla de iconos a la derecha con el contenido de la carpeta
// actual. Dos discos -- NemoFS y el disco FAT (SD o pendrive, segun
// lo que haya montado) -- para poder copiar archivos entre los dos.
//
// MEJORAS de esta revision:
//   - Arbol real en el panel izquierdo: cada carpeta se puede
//     expandir o contraer por separado (antes solo se veian las
//     subcarpetas de donde estabas, en una lista plana).
//   - Panel derecho como rejilla de iconos (icono arriba, nombre
//     debajo, varias columnas), no una lista de filas.
//   - Aspecto con relieve 3D en botones y paneles (estilo clasico).
//   - Copiar/Cortar/Pegar admite CARPETAS ENTERAS, recursivamente
//     (antes solo archivos sueltos).
//   - Comando nuevo "Cortar": mueve en vez de copiar -- borra el
//     origen automaticamente tras un pegado con exito, sin tener que
//     borrarlo a mano despues.

#include <stdint.h>
#include <stdbool.h>
#include "barra.h"   // reparto de la barra de botones, compartido

#define SYS_READ_CHAR       12
#define SYS_GET_TICKS        2   // latidos de 10 ms desde el arranque
#define SYS_PUMP            14
#define SYS_FILE_OPEN       20
#define SYS_FILE_READ       21
#define SYS_FILE_WRITE      22
#define SYS_FILE_LIST       23
#define SYS_DIR_CREATE      24
#define SYS_DRAW_RECT       30
#define SYS_DRAW_TEXT       31
#define SYS_DRAW_ICON       32
#define SYS_GET_WINDOW_SIZE 33
#define SYS_GET_MOUSE       34
#define SYS_GET_MOUSE_WHEEL 45
#define SYS_DEFINE_BUTTON   36
#define SYS_GET_BUTTON_ID   37
#define SYS_LAUNCH_PROGRAM  5
#define SYS_FILE_DELETE     25
#define SYS_FILE_CLOSE      274   // soltar el hueco: si no, a los 8 archivos no se abre ninguno mas
#define SYS_DRAW_FILE_SCALED 277  // dibuja un .nimg escalado leyendo el archivo, sin limite de tamaño
#define SYS_CREATE_IMAGE    52
#define SYS_IMAGE_SIZE      51
#define SYS_FREE_IMAGE      88
#define SYS_RESIZE_IMAGE    96
#define SYS_DRAW_IMAGE_RECT 98
#define SYS_SET_IMAGE_BUFFER 128
#define SYS_FILE_READ_AT    262   // leer por posicion, para copiar por trozos
#define SYS_FILE_APPEND     263   // añadir al final, idem
#define SYS_FILE_CLIPBOARD_SET 26
#define SYS_FILE_CLIPBOARD_GET 27
#define SYS_CREATE_WINDOW   40
#define SYS_POLL_EVENT       8
#define EVENT_WINDOWCLOSE    0x803
#define SYS_FILE_RENAME     234
#define SYS_DESKTOP_ICON_SCALE_GET 243

// Los archivos que macOS deja al copiar a una tarjeta ("._algo",
// ".Trashes", ".Spotlight-V100", ".fseventsd") no son del usuario y solo
// estorban: el explorador no los enseña.
static bool es_basura_de_mac(const char *n) {
    if (n[0] == '.' && n[1] == '_') return true;
    const char *ocultos[4];
    ocultos[0] = ".Trashes"; ocultos[1] = ".Spotlight-V100"; ocultos[2] = ".fseventsd"; ocultos[3] = ".DS_Store";
    for (int k = 0; k < 4; k++) {
        int i = 0;
        while (ocultos[k][i] && n[i] == ocultos[k][i]) i++;
        if (!ocultos[k][i] && !n[i]) return true;
    }
    return false;
}

#define VOLUME_NEMOFS 0
#define VOLUME_FAT    1

#define ICON_FOLDER 0
#define ICON_TXT    1
#define ICON_CODE   2
#define ICON_SIZE   24

#define TYPE_FILE 1
#define TYPE_DIR  2

static inline uint64_t syscall5(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    register uint64_t x4 __asm__("x4") = a4;
    register uint64_t x8 __asm__("x8") = num;
    __asm__ volatile("svc #0"
                      : "+r"(x0)
                      : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
                      : "memory");
    return x0;
}

// Con SEIS argumentos: hace falta para pasar el ORIGEN en x5
// a SYS_DRAW_FILE_SCALED (el volumen y la carpeta de donde leer el
// .nimg). Con syscall5, x5 llegaba con lo que hubiera en el registro.
static inline uint64_t syscall6(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    register uint64_t x4 __asm__("x4") = a4;
    register uint64_t x5 __asm__("x5") = a5;
    register uint64_t x8 __asm__("x8") = num;
    __asm__ volatile("svc #0"
                      : "+r"(x0)
                      : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                      : "memory");
    return x0;
}

static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    syscall5(SYS_DRAW_RECT, (uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h, color);
}
// ---- Fuentes ----
// El explorador escribia con la 5x7 del sistema, en mayusculas. Ahora usa
// la sans del sistema: 12 px para la interfaz y 10 px para los nombres de
// archivo (tienen que caber en celdas de 76 px). Las coordenadas de
// dibujo son las de siempre -- pensadas para glifos de 7 px de alto --
// y draw_text las compensa segun la fuente activa, igual que hace el
// kernel con su propia interfaz (UI_TEXT_DY en wm.c). Si una fuente no se
// pudiera cargar, se usa la del sistema sin compensar: nunca peor que antes.
#define SYS_LOAD_FONT  189
#define SYS_SET_FONT   191
#define SYS_TEXT_WIDTH 254
static uint64_t fuente_ui = 0, fuente_nombres = 0;
static int texto_dy = 0;
static void draw_text(int x, int y, const char *s, uint32_t color) {
    syscall5(SYS_DRAW_TEXT, (uint64_t)x, (uint64_t)(y + texto_dy), (uint64_t)s, color, 0);
}
static void usar_fuente(uint64_t f, int dy) {
    syscall5(SYS_SET_FONT, f, 0, 0, 0, 0);
    texto_dy = f ? dy : 0;
}
static void usar_fuente_ui(void)      { usar_fuente(fuente_ui, -3); }
static void usar_fuente_nombres(void) { usar_fuente(fuente_nombres, -2); }
// Ancho en pixeles de un texto, con la fuente activa
static int text_px(const char *t) { return (int)syscall5(SYS_TEXT_WIDTH, (uint64_t)t, 0, 0, 0, 0); }
static void cargar_fuentes(void) {
    int64_t a = (int64_t)syscall5(SYS_LOAD_FONT, (uint64_t)"sans", 12, 0, 0, 0);
    int64_t b = (int64_t)syscall5(SYS_LOAD_FONT, (uint64_t)"sans", 10, 0, 0, 0);
    fuente_ui = a > 0 ? (uint64_t)a : 0;
    fuente_nombres = b > 0 ? (uint64_t)b : 0;
    usar_fuente_ui();
}
static void draw_icon(int x, int y, int icon_id) {
    syscall5(SYS_DRAW_ICON, (uint64_t)x, (uint64_t)y, (uint64_t)icon_id, 1, 0);
}
static void draw_icon_scaled(int x, int y, int icon_id, int scale) {
    syscall5(SYS_DRAW_ICON, (uint64_t)x, (uint64_t)y, (uint64_t)icon_id, (uint64_t)scale, 0);
}
static void define_button(uint32_t id, int x, int y, int w, int h, uint32_t color) {
    uint64_t packed_wh = ((uint64_t)(uint16_t)w << 16) | (uint16_t)h;
    syscall5(SYS_DEFINE_BUTTON, id, (uint64_t)x, (uint64_t)y, packed_wh, color);
}

// -- relieve 3D (estilo clasico): una linea clara arriba-izquierda y
// oscura abajo-derecha para "elevado"; al reves para "hundido" (los
// paneles de lista y los botones ya pulsados usan esto ultimo). --
#define COLOR_BEVEL_LIGHT 0x00FFFFFF
#define COLOR_BEVEL_DARK  0x00404040

static void draw_bevel(int x, int y, int w, int h, uint32_t face, bool raised) {
    draw_rect(x, y, w, h, face);
    if (w <= 0 || h <= 0) return;
    uint32_t c1 = raised ? COLOR_BEVEL_LIGHT : COLOR_BEVEL_DARK;
    uint32_t c2 = raised ? COLOR_BEVEL_DARK  : COLOR_BEVEL_LIGHT;
    draw_rect(x, y, w, 1, c1);
    draw_rect(x, y, 1, h, c1);
    draw_rect(x, y + h - 1, w, 1, c2);
    draw_rect(x + w - 1, y, 1, h, c2);
}

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static bool str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
static char to_upper_ascii(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}
static bool ends_with_pro(const char *name) {
    int len = str_len(name);
    if (len < 4) return false;
    return name[len-4]=='.' &&
           to_upper_ascii(name[len-3])=='P' &&
           to_upper_ascii(name[len-2])=='R' &&
           to_upper_ascii(name[len-1])=='O';
}
static bool ends_with_lua(const char *name) {
    int n = 0; while (name[n]) n++;
    if (n < 4) return false;
    const char *x = name + n - 4;
    return x[0] == '.' && (x[1] == 'l' || x[1] == 'L') && (x[2] == 'u' || x[2] == 'U') && (x[3] == 'a' || x[3] == 'A');
}
// .html / .htm -> se abre con el visor (visor.lua, en ACCESORIOS)
static bool ends_with_html(const char *name) {
    int len = str_len(name);
    if (len >= 5 && name[len-5]=='.' && to_upper_ascii(name[len-4])=='H' && to_upper_ascii(name[len-3])=='T' &&
        to_upper_ascii(name[len-2])=='M' && to_upper_ascii(name[len-1])=='L') return true;
    if (len >= 4 && name[len-4]=='.' && to_upper_ascii(name[len-3])=='H' && to_upper_ascii(name[len-2])=='T' &&
        to_upper_ascii(name[len-1])=='M') return true;
    return false;
}
// .md / .markdown -> tambien al visor (lo convierte a HTML el mismo)
static bool ends_with_md(const char *name) {
    int len = str_len(name);
    if (len >= 3 && name[len-3]=='.' && to_upper_ascii(name[len-2])=='M' && to_upper_ascii(name[len-1])=='D') return true;
    if (len >= 9 && name[len-9]=='.' && to_upper_ascii(name[len-8])=='M' && to_upper_ascii(name[len-7])=='A' && to_upper_ascii(name[len-6])=='R' &&
        to_upper_ascii(name[len-5])=='K' && to_upper_ascii(name[len-4])=='D' && to_upper_ascii(name[len-3])=='O' && to_upper_ascii(name[len-2])=='W' && to_upper_ascii(name[len-1])=='N') return true;
    return false;
}
// .nb = Nemo Basic, el BASIC propio del sistema. (Antes era .bb, la
// extension de Nemo-Blitz/BlitzPlus, que quedo atras con la
// reescritura del compilador.)
static bool ends_with_nb(const char *name) {
    int len = str_len(name);
    if (len < 3) return false;
    return name[len-3]=='.' &&
           to_upper_ascii(name[len-2])=='N' &&
           to_upper_ascii(name[len-1])=='B';
}
// Partir un nombre en DOS lineas que quepan en max_px, midiendo pixeles
// de verdad con la fuente activa (antes se contaban caracteres a 6 px).
// Se mantiene lo aprendido: preferir cortar en la extension
// ("CREATEWINDOW" / ".PRO"), y no dejar 1-2 letras sueltas abajo.
static void copiar_trozo(char *dst, const char *src, int desde, int hasta) {
    int k = 0;
    for (int i = desde; i < hasta && k < 27; i++) dst[k++] = src[i];
    dst[k] = '\0';
}
static int cabe_trozo(const char *src, int desde, int hasta, int max_px) {
    char t[28]; copiar_trozo(t, src, desde, hasta); return text_px(t) <= max_px;
}
static void wrap_name_2lines(char *line1, char *line2, const char *name, int max_px) {
    int len = str_len(name);
    if (len > 27) len = 27;
    line2[0] = '\0';
    if (cabe_trozo(name, 0, len, max_px)) { copiar_trozo(line1, name, 0, len); return; }
    int dot = -1;
    for (int i = len - 1; i > 0; i--) { if (name[i] == '.') { dot = i; break; } }
    if (dot > 0 && cabe_trozo(name, 0, dot, max_px) && cabe_trozo(name, dot, len, max_px)) {
        copiar_trozo(line1, name, 0, dot); copiar_trozo(line2, name, dot, len); return;
    }
    int cut = 1;                                           // el trozo mas largo que cabe
    while (cut < len && cabe_trozo(name, 0, cut + 1, max_px)) cut++;
    if (len - cut > 0 && len - cut < 3 && cut > 5) cut = len - 3;   // sin letras sueltas
    copiar_trozo(line1, name, 0, cut);
    if (cabe_trozo(name, cut, len, max_px)) { copiar_trozo(line2, name, cut, len); return; }
    // No cabe en dos lineas: la segunda muestra "..." y el FINAL del nombre,
    // para que la extension se vea siempre ("GUIA_PROGR" / "...N_LUA.MD").
    // Antes el final se perdia en silencio.
    int desde = cut + 1;
    for (;;) {
        char t[32] = "\xE2\x80\xA6";                    // "..." (un solo caracter, U+2026)
        int k = 3;
        for (int i = desde; i < len && k < 30; i++) t[k++] = name[i];
        t[k] = '\0';
        if (text_px(t) <= max_px || desde >= len - 1) {
            int m = 0; while (t[m] && m < 27) { line2[m] = t[m]; m++; } line2[m] = '\0';
            return;
        }
        desde++;
    }
}

static int append_dec(char *buf, int pos, uint32_t value) {
    if (value == 0) { buf[pos++] = '0'; return pos; }
    char digits[10]; int n = 0;
    while (value > 0) { digits[n++] = (char)('0' + value % 10); value /= 10; }
    while (n > 0) buf[pos++] = digits[--n];
    return pos;
}

// -- estado --
// BUG REAL CORREGIDO: SYS_FILE_LIST tenia un tope de 64 fijado DENTRO
// del kernel (no aqui) -- una carpeta con mas archivos que eso perdia
// los que sobraban en cualquier operacion que dependiera del listado,
// copiar una carpeta entera incluido. El kernel ya admite hasta 512;
// aqui hace falta pedir/recibir hasta ese mismo numero para que sirva
// de algo.
#define MAX_ENTRIES 512
typedef struct {
    char name[28];
    uint32_t type;
    uint32_t size;
} entry_t;

static entry_t all_entries[MAX_ENTRIES];   // panel derecho: contenido de la carpeta actual
static uint32_t all_count = 0;

static uint32_t current_volume = VOLUME_NEMOFS;
static uint32_t current_dir = 0;

static uint32_t scroll_right = 0; // en FILAS de la rejilla, no en entradas sueltas
static uint32_t scroll_left = 0;  // en filas del arbol

#define DIR_STACK_MAX 16
static uint32_t dir_stack[DIR_STACK_MAX];
static int dir_depth = 0;

static char status_msg[80] = "";
static char selected_name[28] = "";
static bool selected_is_dir = false;
#define DOBLE_CLIC_TICKS 50            // medio segundo, como los iconos del escritorio
static char ultimo_clic[28] = "";      // la entrada del ultimo clic, y cuando fue
static uint64_t ultimo_clic_tick = 0;

// -- portapapeles de archivos: recordamos ademas si era una carpeta y
// si la operacion era "cortar" (mover) en vez de "copiar". --
static bool clipboard_is_dir = false;
static bool clipboard_is_cut = false;

// -- menu contextual (clic derecho) --
#define CTX_ITEM_H 18
static bool ctx_menu_open = false;
static int ctx_x, ctx_y;
static char ctx_target_name[28] = ""; // "" = clic en zona vacia (solo PEGAR)
static bool ctx_target_is_dir = false;

// -- cuadro de renombrar --
static bool rename_mode = false;
static char rename_buf[28] = "";
static int rename_len = 0;
static char rename_original[28] = ""; // el nombre que se esta renombrando

static int win_w, win_h;
static bool running = true;

// BUG REAL CORREGIDO: la geometria del menu estaba
// ESCRITA DOS VECES, al dibujarlo y al atender el clic, y no
// coincidian: el dibujo contaba 5 opciones y el clic 4. Dos
// consecuencias que se veian usando el programa:
//   1. "Pegar" (la 5a) caia FUERA del alto que usaba el clic, asi que
//      pulsar justo encima de la palabra no hacia nada.
//   2. Cerca del borde inferior, el menu se recoloca con 'win_h - h', y
//      como cada lado calculaba una 'h' distinta, el menu dibujado y
//      sus zonas sensibles quedaban desplazados entre si: se pulsaba
//      una opcion y salia la de al lado. En un menu con "Eliminar"
//      arriba, eso es mucho mas que una molestia.
// Ahora la geometria se calcula UNA vez, aqui, y los dos la piden.
static void ctx_menu_geom(int *x, int *y, int *w, int *h, int *count) {
    *count = ctx_target_name[0] ? 5 : 1;
    *w = 130;
    *h = *count * CTX_ITEM_H + 4;
    *x = ctx_x; *y = ctx_y;
    if (*x + *w > win_w) *x = win_w - *w;
    if (*y + *h > win_h) *y = win_h - *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

#define BTN_NEMOFS  1
#define BTN_FAT     2
#define BTN_UP      3
#define BTN_NEWDIR  4
#define BTN_COPY    5
#define BTN_EXIT    6


#define TOPBAR_H    24
#define PATHBAR_H   16
#define STATUSBAR_H 16
#define LEFT_W      130
// BUG REAL CORREGIDO: el icono de carpeta mide ICON_SIZE (24px) de
// alto, pero la fila del arbol media solo 18px -- el icono se salia
// por debajo y pisaba la fila siguiente, notandose sobre todo al
// expandir una carpeta (las filas nuevas quedaban casi encima de las
// de al lado). 26px deja sitio de sobra al icono con margen arriba y
// abajo.
#define TREE_ROW_H  26
#define INDENT_PX   12
// Tamaño de icono: se consulta al arrancar el ajuste que se elige en
// AJUSTES (compartido con el escritorio) -- 1 (24x24) o 2 (48x48). La
// celda de la rejilla se calcula a partir de eso, no es fija.
static int32_t grid_icon_scale = 1;
static int GRID_CELL_W_v = 92, GRID_CELL_H_v = 66, GRID_ICON_PX_v = 24;
#define GRID_CELL_W  GRID_CELL_W_v
#define GRID_CELL_H  GRID_CELL_H_v
#define GRID_ICON_PX GRID_ICON_PX_v
#define SCROLL_BTN_W 20
#define SCROLL_BTN_H 16   // fila reservada al fondo de cada panel para sus botones de scroll

#define COLOR_BG        0x00C0C0C0
#define COLOR_TOPBAR    0x00C0C0C0
#define COLOR_BTN       0x00C0C0C0
#define COLOR_BTN_TEXT  0x00000000
#define COLOR_PATHBAR   0x00FFFFFF
#define COLOR_PATH_TEXT 0x00000000
#define COLOR_LEFT_BG   0x00FFFFFF
#define COLOR_RIGHT_BG  0x00FFFFFF
#define COLOR_DIVIDER   0x00808080
#define COLOR_TEXT      0x00000000
#define COLOR_TEXT_SEL  0x00FFFFFF
#define COLOR_SEL_BG    0x00000080
#define COLOR_STATUS    0x00000000
#define COLOR_STATUSBAR 0x00C0C0C0

static void set_status(const char *msg) {
    int i = 0;
    while (msg[i] && i < 79) { status_msg[i] = msg[i]; i++; }
    status_msg[i] = '\0';
}

// ---------------------------------------------------------------------
// Arbol de carpetas (panel izquierdo)
// ---------------------------------------------------------------------
// Guardado en orden de aparicion (no fisicamente ordenado por
// posicion visual): cada nodo sabe quien es su padre, y en cada
// redibujado se recorre el arbol en profundidad para saber que fila
// va en que orden. Esto evita tener que desplazar el array cada vez
// que se cargan hijos nuevos.
#define MAX_TREE 160
typedef struct {
    uint32_t inode;
    uint32_t volume;
    int16_t parent;      // indice en tree[], -1 = raiz (un disco)
    int8_t depth;
    bool expanded;
    bool children_loaded;
    char name[28];
} tree_node_t;

static tree_node_t tree[MAX_TREE];
static int tree_count = 0;
static int visible_tree[MAX_TREE];
static int visible_tree_count = 0;

static void tree_load_children(int idx) {
    if (tree[idx].children_loaded) return;
    tree[idx].children_loaded = true;
    static uint8_t raw[MAX_ENTRIES * 40];
    uint64_t total = syscall5(SYS_FILE_LIST, tree[idx].inode, (uint64_t)raw, MAX_ENTRIES, tree[idx].volume, 0);
    uint64_t shown = total < MAX_ENTRIES ? total : MAX_ENTRIES;
    for (uint64_t i = 0; i < shown && tree_count < MAX_TREE; i++) {
        uint8_t *e = raw + i * 40;
        uint32_t type = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
        if (type != TYPE_DIR) continue;
        if (es_basura_de_mac((const char *)&e[12])) continue;
        uint32_t inode = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
        tree_node_t *n = &tree[tree_count];
        n->inode = inode;
        n->volume = tree[idx].volume;
        n->parent = (int16_t)idx;
        n->depth = (int8_t)(tree[idx].depth + 1);
        n->expanded = false;
        n->children_loaded = false;
        int j = 0;
        while (e[12 + j] && j < 27) { n->name[j] = (char)e[12 + j]; j++; }
        n->name[j] = '\0';
        tree_count++;
    }
}

static void tree_init(void) {
    tree_count = 0;
    tree[tree_count].inode = 0; tree[tree_count].volume = VOLUME_NEMOFS;
    tree[tree_count].parent = -1; tree[tree_count].depth = 0;
    tree[tree_count].expanded = true; tree[tree_count].children_loaded = false;
    { const char *s = "NemoFS"; int j = 0; while (s[j]) { tree[tree_count].name[j] = s[j]; j++; } tree[tree_count].name[j] = '\0'; }
    tree_count++;
    tree[tree_count].inode = 0; tree[tree_count].volume = VOLUME_FAT;
    tree[tree_count].parent = -1; tree[tree_count].depth = 0;
    tree[tree_count].expanded = false; tree[tree_count].children_loaded = false;
    { const char *s = "FAT"; int j = 0; while (s[j]) { tree[tree_count].name[j] = s[j]; j++; } tree[tree_count].name[j] = '\0'; }
    tree_count++;
    tree_load_children(0);
}

static void build_visible_rec(int idx) {
    if (visible_tree_count >= MAX_TREE) return;
    visible_tree[visible_tree_count++] = idx;
    if (!tree[idx].expanded) return;
    for (int i = 0; i < tree_count; i++) {
        if (tree[i].parent == idx) build_visible_rec(i);
    }
}
static void build_visible_tree(void) {
    visible_tree_count = 0;
    for (int i = 0; i < tree_count; i++) {
        if (tree[i].parent == -1) build_visible_rec(i);
    }
}

// ---------------------------------------------------------------------
// MINIATURAS DE.nimg
//
// Un archivo de imagen se ve como es, no como un icono generico.
//
// Todas las miniaturas de la carpeta viven en UN MOSAICO, una sola
// imagen con una celda por archivo: el sistema tiene 64 huecos de imagen
// y una carpeta con cien .nimg no cabria de otra forma.
//
// Cada celda la pinta el kernel con SYS_MAKE_THUMBNAIL, que lee el
// archivo fila a fila SIN cargar la imagen. Asi tambien tienen miniatura
// las mas grandes que el maximo del sistema (un fondo de pantalla de
// 1920x1080), y no se reservan megas para pintar 24 pixeles.
//
// Se monta al leer la carpeta, no al dibujar: leer del disco no puede
// pasar sesenta veces por segundo.
#define MAX_MINIATURAS 200
static int32_t mosaico = -1;
static int32_t mos_cols = 1, mos_lado = 0;
static int16_t mini_de[MAX_ENTRIES];      // entrada -> celda, o -1

static bool ends_with_nimg(const char *n) {
    int L = 0; while (n[L]) L++;
    if (L < 5) return false;
    const char *e = n + L - 5;
    return (e[0] == '.') &&
           (e[1] == 'n' || e[1] == 'N') && (e[2] == 'i' || e[2] == 'I') &&
           (e[3] == 'm' || e[3] == 'M') && (e[4] == 'g' || e[4] == 'G');
}

static void soltar_miniaturas(void) {
    if (mosaico >= 0) { syscall5(SYS_FREE_IMAGE, (uint64_t)mosaico, 0, 0, 0, 0); mosaico = -1; }
    for (uint32_t i = 0; i < MAX_ENTRIES; i++) mini_de[i] = -1;
}

// Dibuja un trozo de 'img' en (x,y) SIN escalar y sin mascara (opaco):
// en un explorador hay que ver los pixeles como son.
static void blit_trozo(int32_t img, int x, int y, int rx, int ry, int rw, int rh) {
    uint64_t a3 = (((uint64_t)(rx & 0x7FFF)) << 16) | (uint64_t)(ry & 0xFFFF) | (1ULL << 31);
    syscall5(SYS_DRAW_IMAGE_RECT, (uint64_t)img, (uint64_t)x, (uint64_t)y, a3,
             (((uint64_t)(rw & 0xFFFF)) << 16) | (uint64_t)(rh & 0xFFFF));
}

// El origen que esperan SYS_DRAW_FILE_SCALED y SYS_MINIATURA_DOBLE en x5:
// los 32 bits bajos la carpeta, los 32 altos el volumen. Ver la nota de
// nimg_fuente_abrir() en src/syscall.c.
static inline uint64_t origen_actual(void) {
    return ((uint64_t)current_volume << 32) | (uint64_t)current_dir;
}

static void construir_miniaturas(void) {
    soltar_miniaturas();
    // Antes aqui habia un "if (current_volume != VOLUME_NEMOFS)
    // return;" porque el kernel solo sabia leer .nimg de NemoFS: en la
    // tarjeta no habia miniaturas y no se decia por que. Ahora el origen va
    // en x5 y el kernel lee del volumen que se le diga.

    int lado = GRID_ICON_PX * (int)grid_icon_scale;
    if (lado < 8) lado = 8;
    mos_lado = lado;

    uint32_t cuantas = 0;
    for (uint32_t i = 0; i < all_count && cuantas < MAX_MINIATURAS; i++)
        if (all_entries[i].type != TYPE_DIR && ends_with_nimg(all_entries[i].name)) cuantas++;
    if (cuantas == 0) return;

    int cols = 1;
    while ((uint32_t)(cols * cols) < cuantas) cols++;
    if (cols * lado > 1024) cols = 1024 / lado;
    int filas = (int)((cuantas + cols - 1) / cols);
    if (filas * lado > 1024) filas = 1024 / lado;
    if (cols < 1 || filas < 1) return;

    mosaico = (int32_t)syscall5(SYS_CREATE_IMAGE, (uint64_t)(cols * lado), (uint64_t)(filas * lado), 0, 0, 0);
    if (mosaico < 0) return;
    mos_cols = cols;

    int hechas = 0;
    for (uint32_t i = 0; i < all_count && hechas < cols * filas; i++) {
        entry_t *e = &all_entries[i];
        if (e->type == TYPE_DIR || !ends_with_nimg(e->name)) continue;

        // Una sola llamada: el kernel lee el archivo y pinta la miniatura
        // en su celda. No pasa por la tabla de imagenes, asi que funciona
        // tambien con las mas grandes que su maximo -- un fondo de
        // pantalla de 1920x1080 no tenia miniatura y no se veia por que.
        int cx = (hechas % cols) * lado, cy = (hechas / cols) * lado;
        if (syscall6(SYS_DRAW_FILE_SCALED, (uint64_t)e->name, (uint64_t)mosaico,
                     (((uint64_t)cx) << 16) | (uint64_t)cy,
                     (((uint64_t)lado) << 16) | (uint64_t)lado, 1 /* miniatura */,
                     origen_actual()) == 0) {
            mini_de[i] = (int16_t)hechas;
            hechas++;
        }
    }
}

// ---------------------------------------------------------------------
// Listado del panel derecho (carpeta actual)
// ---------------------------------------------------------------------
static void load_listing(void) {
    static uint8_t raw[MAX_ENTRIES * 40];
    uint64_t total = syscall5(SYS_FILE_LIST, current_dir, (uint64_t)raw, MAX_ENTRIES, current_volume, 0);
    uint64_t shown = total < MAX_ENTRIES ? total : MAX_ENTRIES;

    scroll_right = 0;
    all_count = 0;
    for (uint64_t i = 0; i < shown; i++) {
        uint8_t *e = raw + i * 40;
        if (es_basura_de_mac((const char *)&e[12])) continue;   // lo que deja macOS al copiar
        entry_t *dst = &all_entries[all_count];
        dst->type = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
        dst->size = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        int j = 0;
        while (e[12 + j] && j < 27) { dst->name[j] = (char)e[12 + j]; j++; }
        dst->name[j] = '\0';
        all_count++;
    }
    construir_miniaturas();
}

static void switch_volume(uint32_t vol) {
    current_volume = vol;
    current_dir = 0;
    dir_depth = 0;
    selected_name[0] = '\0';
    load_listing();
}

static void enter_dir(uint32_t inode) {
    if (dir_depth < DIR_STACK_MAX) dir_stack[dir_depth++] = current_dir;
    current_dir = inode;
    selected_name[0] = '\0';
    load_listing();
}

static void go_up(void) {
    if (dir_depth == 0) return;
    dir_depth--;
    current_dir = dir_stack[dir_depth];
    selected_name[0] = '\0';
    load_listing();
}

// Abre un archivo de la carpeta actual con el programa que se le diga,
// pasandole "padre:nombre" -- el formato que entienden editor.pro,
// ide.pro, visor.lua y pintor.lua -- o "F:padre:nombre" si estamos en
// la tarjeta.
//
// Esto eran TRES funciones casi identicas, una por
// programa, copiadas unas de otras. Es justo la forma en que se cuelan
// las diferencias: al añadir el prefijo del disco habria que haberse
// acordado de las tres, y con la cuarta (el Pintor) habrian sido
// cuatro. Ahora hay un solo sitio donde se arma el argumento.
static void launch_para(const char *programa, const char *name) {
    static char arg[52];
    int p = 0;
    if (current_volume != VOLUME_NEMOFS) { arg[p++] = 'F'; arg[p++] = ':'; }
    p = append_dec(arg, p, current_dir);
    arg[p++] = ':';
    int i = 0;
    while (name[i] && p < (int)sizeof(arg) - 2) { arg[p++] = name[i]; i++; }
    arg[p] = '\0';
    syscall5(SYS_LAUNCH_PROGRAM, (uint64_t)programa, (uint64_t)arg, (uint64_t)current_dir, 0, 0);
}

// ¿Este .pro se declaro como aplicacion de CONSOLA?
//
// Lee el campo 'version' de la cabecera NEXE, que son los 16 primeros
// bytes del archivo: 1 = escritorio, 2 = consola. Un .pro anterior a
// esta marca lleva 1, asi que se abre como siempre.
static bool pro_es_de_consola(const char *name) {
    int32_t id = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)name, current_dir, current_volume, 0, 0);
    if (id < 0) return false;
    uint8_t cab[24];
    int64_t leidos = (int64_t)syscall5(SYS_FILE_READ, (uint64_t)id, (uint64_t)cab, sizeof(cab), current_volume, 0);
    // Se cierra AQUI, con la cabecera ya leida: esta funcion se llama en
    // cada doble clic sobre un .pro, y antes se iba por cualquiera de sus
    // cinco returns dejando el hueco tomado. Ocho programas lanzados desde
    // la tarjeta y el explorador se quedaba sin poder abrir nada.
    syscall5(SYS_FILE_CLOSE, (uint64_t)id, current_volume, 0, 0, 0);
    if (leidos < 8) return false;
    if (cab[0] != 'N' || cab[1] != 'E' || cab[2] != 'X' || cab[3] != 'E') return false;
    uint32_t version = (uint32_t)cab[4] | ((uint32_t)cab[5] << 8) |
                       ((uint32_t)cab[6] << 16) | ((uint32_t)cab[7] << 24);

    // Dos formatos conviven:
    //   version 1/2 -- cabecera de 16 bytes; la marca de consola ES la
    //                  propia version (2 = consola)
    //   version 3   -- cabecera de 24 bytes; la marca va en el campo
    //                  'flags' (bit 0), en el desplazamiento 20
    if (version == 2) return true;
    if (version >= 3 && leidos >= 24) {
        uint32_t flags = (uint32_t)cab[20] | ((uint32_t)cab[21] << 8) |
                         ((uint32_t)cab[22] << 16) | ((uint32_t)cab[23] << 24);
        return (flags & 1u) != 0;
    }
    return false;
}

// ---- Un clic selecciona; dos clics abren ----
// Antes, un solo clic entraba en una carpeta o abria un archivo. Ahora, como
// en el escritorio y en cualquier sistema: un clic marca la entrada y dice
// que es; dos seguidos sobre la misma (en medio segundo, lo mismo que los
// iconos del escritorio: DOUBLE_CLICK_TICKS en wm.c) la abren. Enter abre
// la seleccionada.
static void seleccionar_entrada(const entry_t *e) {
    int i = 0;
    while (e->name[i] && i < 27) { selected_name[i] = e->name[i]; i++; }
    selected_name[i] = '\0';
    selected_is_dir = (e->type == TYPE_DIR);
    if (selected_is_dir) { set_status("Carpeta: doble clic o Enter para abrirla"); return; }
    char msg[64];
    int p = 0;
    const char *pre = "Tamaño: ";
    int j = 0; while (pre[j]) msg[p++] = pre[j++];
    p = append_dec(msg, p, e->size);
    const char *post = " bytes";
    j = 0; while (post[j] && p < 62) msg[p++] = post[j++];
    msg[p] = '\0';
    set_status(msg);
}

static void abrir_entrada(const entry_t *e) {
    if (e->type == TYPE_DIR) {
        int32_t inode = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)e->name, current_dir, current_volume, 0, 0);
        if (inode >= 0) enter_dir((uint32_t)inode);
        return;
    }

    // Doble clic segun el tipo de archivo, igual que en cualquier
    // sistema operativo de verdad: un .pro se EJECUTA, un .nb se abre
    // con el IDE (es codigo fuente, no texto suelto), y cualquier otra
    // cosa (.txt incluido) se abre con el editor.
    if (current_volume == VOLUME_NEMOFS) {
        if (ends_with_pro(e->name) || ends_with_lua(e->name)) {
            // un .lua se ejecuta igual que un .pro: el kernel lo redirige
            // a LUA.PRO (ver task_spawn_from_file). Para EDITARLO se abre
            // desde el IDE o el editor, como cualquier programa.
            //
            // Un .pro marcado como APLICACION DE CONSOLA se abre dentro
            // de una shell, que es quien sabe dibujar su salida y
            // pasarle el teclado. Sin eso, un programa que hace Print o
            // Input$ se quedaba corriendo en segundo plano, escribiendo
            // en el puerto serie, sin ninguna ventana donde verlo.
            //
            // La marca la pone el compilador cuando el programa declara
            // "Console" en una linea suelta. Sin declaracion es de
            // escritorio, como han sido todos hasta ahora.
            if (ends_with_pro(e->name) && pro_es_de_consola(e->name)) {
                // "@<carpeta> run programa.pro": la shell empieza
                // en la carpeta del programa, no en la raiz
                char cmd[64];
                int q = 0;
                cmd[q++] = '@';
                { char num[12]; int nn = 0; uint32_t v = current_dir;
                  if (v == 0) num[nn++] = '0';
                  while (v > 0) { num[nn++] = (char)('0' + v % 10); v /= 10; }
                  while (nn > 0) cmd[q++] = num[--nn]; }
                const char *run = " run ";
                for (int r = 0; run[r]; r++) cmd[q++] = run[r];
                int k = 0;
                while (e->name[k] && q < (int)sizeof(cmd) - 1) cmd[q++] = e->name[k++];
                cmd[q] = '\0';
                syscall5(SYS_LAUNCH_PROGRAM, (uint64_t)"shell.pro", (uint64_t)cmd, (uint64_t)current_dir, 0, 0);
                return;
            }
            syscall5(SYS_LAUNCH_PROGRAM, (uint64_t)e->name, (uint64_t)"", (uint64_t)current_dir, 0, 0);
        } else if (ends_with_nb(e->name)) {
            launch_para("ide.pro", e->name);
        } else if (ends_with_html(e->name) || ends_with_md(e->name)) {
            launch_para("visor.lua", e->name);
        } else if (ends_with_nimg(e->name)) {
            // Una imagen se abre con el Pintor, no con el
            // editor de texto. El Navegante ya lo hacia asi desde que
            // existe el Pintor; aqui se habia quedado el reparto viejo,
            // de cuando la unica forma de "abrir" algo que no fuera
            // codigo era mirarlo como texto.
            launch_para("pintor.lua", e->name);
        } else {
            launch_para("editor.pro", e->name);
        }
    } else {
        // En la TARJETA ya se puede abrir algo. Antes el
        // doble clic no hacia absolutamente nada aqui, sin decir por
        // que. El editor y el IDE saben leer y guardar en FAT desde
        // hoy; los demas (Pintor, visor) todavia no, y ejecutar un
        // programa desde FAT no se sostiene: el cargador lee de NemoFS.
        if (ends_with_nb(e->name)) {
            launch_para("ide.pro", e->name);
        } else if (ends_with_pro(e->name) || ends_with_lua(e->name)) {
            set_status("Copia el programa a NemoFS para ejecutarlo");
        } else if (ends_with_nimg(e->name)) {
            // Ya se puede abrir una imagen de la TARJETA: el
            // kernel lee .nimg de los dos volumenes y el Pintor entiende el
            // prefijo "F:" que este mismo launch_para() venia poniendo desde
            // hace tiempo sin que nadie lo leyera.
            launch_para("pintor.lua", e->name);
        } else if (ends_with_html(e->name) || ends_with_md(e->name)) {
            set_status("Copia el archivo a NemoFS para abrirlo");
        } else {
            launch_para("editor.pro", e->name);
        }
    }
}

static void new_folder(void) {
    if (current_volume != VOLUME_NEMOFS) { set_status("FAT no tiene carpetas"); return; }
    for (int n = 1; n <= 99; n++) {
        char name[16];
        int p = 0;
        const char *pre = "NUEVA";
        int j = 0; while (pre[j]) name[p++] = pre[j++];
        p = append_dec(name, p, (uint32_t)n);
        name[p] = '\0';

        int32_t idx = (int32_t)syscall5(SYS_DIR_CREATE, (uint64_t)name, current_dir, current_volume, 0, 0);
        if (idx >= 0) {
            load_listing();
            set_status("Carpeta creada");
            return;
        }
    }
}

// Copia el archivo seleccionado al OTRO disco -- el motivo de tener
// dos discos en el explorador: poder pasar archivos entre NemoFS y el
// disco que se puede montar en el Mac.
// ---------------------------------------------------------------------
// Copiar un archivo, con aviso si no cabe
// ---------------------------------------------------------------------
// COPIA POR TROZOS.
//
// Antes se leia el archivo ENTERO a un buffer de 4 MB y se escribia de
// una vez. Eso traia dos problemas: los archivos de entre 4 y 8,25 MB
// (el techo real de NemoFS) no se podian copiar aunque el sistema
// pudiera guardarlos, y ese buffer se comia 4 de los 16 MB de la tarea
// del explorador estuviera copiando o no.
//
// Ahora se lee con SYS_FILE_READ_AT (por posicion) y se escribe con
// SYS_FILE_APPEND (añadir al final), los dos en trozos. El buffer baja
// a 64 KB -- sesenta y cuatro veces menos -- y desaparece el limite de
// tamaño: lo que quepa en el destino se copia.
//
// El historial que conviene no repetir: hubo una epoca en que un
// archivo mas grande que el buffer se copiaba TRUNCADO sin decir nada.
// Un .pro de 2,1 MB llegaba cortado, arrancaba bien (el codigo estaba
// en los primeros 262 KB) y fallaba por los datos del final -- se
// persiguio el fallo dentro del compilador, donde no estaba. Por eso
// aqui cualquier trozo que falle a medias deja la copia por MALA: un
// archivo a medias es peor que ningun archivo.
#define COPIA_BUF_SIZE (64u * 1024u)
static uint8_t copia_buf[COPIA_BUF_SIZE];

typedef enum {
    COPIA_OK = 0,
    COPIA_DEMASIADO_GRANDE,
    COPIA_ERROR_ORIGEN,
    COPIA_ERROR_DESTINO,
} copia_resultado_t;

static copia_resultado_t copiar_archivo(const char *src_name, uint32_t src_dir, uint32_t src_vol,
                                        const char *dst_name, uint32_t dst_dir, uint32_t dst_vol) {
    int32_t src_id = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)src_name, src_dir, src_vol, 0, 0);
    if (src_id < 0) return COPIA_ERROR_ORIGEN;

    int32_t dst_id = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)dst_name, dst_dir, dst_vol, 0, 0);
    if (dst_id < 0) { syscall5(SYS_FILE_CLOSE, (uint64_t)src_id, src_vol, 0, 0, 0); return COPIA_ERROR_DESTINO; }

    // Los huecos de archivo abierto son OCHO en todo el sistema y hasta
    // ahora no habia forma de soltarlos: el explorador no termina nunca
    // mientras se usa, asi que a la novena copia dejaba de poder abrir
    // nada y avisaba de que "el origen ya no existe". Se cierran por
    // TODOS los caminos, tambien por los de error -- el mismo cuidado
    // que con cualquier otro recurso del kernel.
    copia_resultado_t r = COPIA_OK;

    // El destino se deja VACIO antes de empezar: si ya existia con
    // contenido, añadir le pegaria lo nuevo detras de lo viejo.
    if ((int64_t)syscall5(SYS_FILE_WRITE, (uint64_t)dst_id, (uint64_t)copia_buf, 0, dst_vol, 0) < 0) {
        r = COPIA_ERROR_DESTINO;
    } else {
        uint64_t pos = 0;
        for (;;) {
            int64_t leidos = (int64_t)syscall5(SYS_FILE_READ_AT, (uint64_t)src_id,
                                               (uint64_t)copia_buf, COPIA_BUF_SIZE, pos, src_vol);
            if (leidos < 0) { r = COPIA_ERROR_ORIGEN; break; }
            if (leidos == 0) break;                       // fin del archivo

            int64_t escritos = (int64_t)syscall5(SYS_FILE_APPEND, (uint64_t)dst_id,
                                                 (uint64_t)copia_buf, (uint64_t)leidos, dst_vol, 0);
            // Si no cabe o falla a media copia, el destino se queda a
            // medias: se borra, para no dejar un archivo que parece estar
            // y no esta.
            if (escritos != leidos) {
                syscall5(SYS_FILE_DELETE, (uint64_t)dst_name, dst_dir, dst_vol, 0, 0);
                r = COPIA_DEMASIADO_GRANDE;
                break;
            }
            pos += (uint64_t)leidos;
        }
    }

    syscall5(SYS_FILE_CLOSE, (uint64_t)dst_id, dst_vol, 0, 0, 0);
    syscall5(SYS_FILE_CLOSE, (uint64_t)src_id, src_vol, 0, 0, 0);
    return r;
}

static void copy_selected_to_other_volume(void) {
    if (selected_name[0] == '\0') { set_status("Nada seleccionado"); return; }

    uint32_t src_vol = current_volume;
    uint32_t dst_vol = (current_volume == VOLUME_NEMOFS) ? VOLUME_FAT : VOLUME_NEMOFS;

    copia_resultado_t r = copiar_archivo(selected_name, current_dir, src_vol,
                                         selected_name, 0, dst_vol);
    switch (r) {
        case COPIA_OK:            set_status("Copiado al otro disco"); break;
        case COPIA_DEMASIADO_GRANDE: set_status("No copiado: archivo demasiado grande"); break;
        case COPIA_ERROR_ORIGEN:  set_status("Error al leer el origen"); break;
        case COPIA_ERROR_DESTINO: set_status("NO SE PUDO COPIAR (¿YA EXISTE ALLI?)"); break;
    }
}

// ---------------------------------------------------------------------
// Copiar/borrar carpetas enteras, recursivamente
// ---------------------------------------------------------------------
// OJO con el buffer 'raw' de SYS_FILE_LIST: es 'static' (no cabe en la
// pila) y por tanto COMPARTIDO entre llamadas recursivas. Si se leyera
// una entrada de 'raw' y LUEGO se recursara antes de terminar de usar
// las demas entradas, la llamada hija pisaria 'raw' a mitad de lectura
// del padre. Por eso cada nivel vacia 'raw' entero en un array LOCAL
// (en la pila, uno por nivel de recursion) ANTES de procesar ninguna
// entrada -- a partir de ahi ya es seguro recursar.

// MEMORIA DE PILA EN LAS RECURSIVAS.
//
// Estas dos funciones se llaman a si mismas por cada carpeta de dentro, y
// cada nivel se guardaba el listado ENTERO en la pila: 512 entradas de
// 32 bytes son 16 KB por nivel. Con carpetas anidadas de verdad eso se
// pone en cien y pico KB de pila, y pasarse de pila no da un error: da un
// Data Abort en un sitio que no tiene nada que ver con la causa.
//
// Ahora solo se copian a la pila los nombres de las SUBCARPETAS, que son
// pocas: los archivos se tratan en una primera pasada leyendo 'raw'
// directamente, porque copiarlos y borrarlos no toca 'raw' -- y 'raw' es
// estatico y compartido, asi que solo se puede usar ANTES de recursar.
//
// De 16 KB por nivel a menos de 2. Y hay tope de profundidad: mejor decir
// "incompleto" que reventar la pila.
#define MAX_SUBCARPETAS 60
#define MAX_PROFUNDIDAD 12

typedef struct { char name[28]; } nombre_t;

static bool copy_dir_contents(uint32_t src_dir, uint32_t src_vol, uint32_t dst_dir, uint32_t dst_vol, uint32_t hondo) {
    if (hondo >= MAX_PROFUNDIDAD) return false;

    static uint8_t raw[MAX_ENTRIES * 40];
    uint64_t total = syscall5(SYS_FILE_LIST, src_dir, (uint64_t)raw, MAX_ENTRIES, src_vol, 0);
    uint64_t shown = total < MAX_ENTRIES ? total : MAX_ENTRIES;

    nombre_t subdirs[MAX_SUBCARPETAS];
    uint32_t ns = 0;
    bool all_ok = (total <= MAX_ENTRIES);   // si la carpeta trae mas de las que caben, incompleta

    // Primera pasada: los archivos, leyendo 'raw' tal cual. Aqui todavia
    // no se ha recursado, asi que 'raw' sigue siendo el de esta carpeta.
    for (uint64_t i = 0; i < shown; i++) {
        uint8_t *e = raw + i * 40;
        uint32_t tipo = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
        char nombre[28];
        int j = 0;
        while (e[12 + j] && j < 27) { nombre[j] = (char)e[12 + j]; j++; }
        nombre[j] = '\0';
        if (tipo == TYPE_DIR) {
            if (ns < MAX_SUBCARPETAS) {
                for (int k = 0; k < 28; k++) subdirs[ns].name[k] = nombre[k];
                ns++;
            } else {
                all_ok = false;             // mas subcarpetas de las que caben
            }
        } else {
            // Un archivo que no quepa hace que la copia de la carpeta
            // entera se marque como incompleta -- el usuario ve
            // "PEGADO INCOMPLETO", no un exito enganoso.
            if (copiar_archivo(nombre, src_dir, src_vol,
                               nombre, dst_dir, dst_vol) != COPIA_OK) {
                all_ok = false;
            }
        }
    }

    // Segunda pasada: las subcarpetas. A partir de aqui 'raw' deja de
    // valer, porque cada recursion lo reescribe con su propio listado.
    for (uint32_t i = 0; i < ns; i++) {
        int32_t new_dir = (int32_t)syscall5(SYS_DIR_CREATE, (uint64_t)subdirs[i].name, dst_dir, dst_vol, 0, 0);
        int32_t src_child = (new_dir >= 0) ? (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)subdirs[i].name, src_dir, src_vol, 0, 0) : -1;
        if (new_dir < 0 || src_child < 0 ||
            !copy_dir_contents((uint32_t)src_child, src_vol, (uint32_t)new_dir, dst_vol, hondo + 1)) {
            all_ok = false;
        }
    }
    return all_ok;
}

// Vacia y borra 'name' (una carpeta) recursivamente -- nemofs_delete
// se niega a borrar una carpeta que todavia tenga contenido, asi que
// hay que dejarla vacia primero. Misma precaucion con 'raw' que arriba.
static bool delete_dir_recursive(const char *name, uint32_t parent, uint32_t vol, uint32_t hondo) {
    if (hondo >= MAX_PROFUNDIDAD) return false;
    int32_t dir_id = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)name, parent, vol, 0, 0);
    if (dir_id < 0) return false;

    static uint8_t raw[MAX_ENTRIES * 40];
    uint64_t total = syscall5(SYS_FILE_LIST, (uint32_t)dir_id, (uint64_t)raw, MAX_ENTRIES, vol, 0);
    uint64_t shown = total < MAX_ENTRIES ? total : MAX_ENTRIES;

    // Misma cuenta de pila que en copy_dir_contents: solo las
    // subcarpetas viajan en la pila; los archivos se borran leyendo
    // 'raw' en la primera pasada, antes de recursar.
    nombre_t subdirs[MAX_SUBCARPETAS];
    uint32_t ns = 0;
    bool ok = (total <= MAX_ENTRIES);

    for (uint64_t i = 0; i < shown; i++) {
        uint8_t *e = raw + i * 40;
        uint32_t tipo = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
        char nombre[28];
        int j = 0;
        while (e[12 + j] && j < 27) { nombre[j] = (char)e[12 + j]; j++; }
        nombre[j] = '\0';
        if (tipo == TYPE_DIR) {
            if (ns < MAX_SUBCARPETAS) {
                for (int k = 0; k < 28; k++) subdirs[ns].name[k] = nombre[k];
                ns++;
            } else ok = false;
        } else {
            if ((int64_t)syscall5(SYS_FILE_DELETE, (uint64_t)nombre, (uint32_t)dir_id, vol, 0, 0) < 0) ok = false;
        }
    }

    for (uint32_t i = 0; i < ns; i++) {
        if (!delete_dir_recursive(subdirs[i].name, (uint32_t)dir_id, vol, hondo + 1)) ok = false;
    }
    if ((int64_t)syscall5(SYS_FILE_DELETE, (uint64_t)name, parent, vol, 0, 0) < 0) ok = false;
    return ok;
}

// -- menu contextual: eliminar / copiar / cortar / pegar --

static void begin_rename(const char *name) {
    int i = 0;
    while (name[i] && i < 27) { rename_original[i] = name[i]; rename_buf[i] = name[i]; i++; }
    rename_original[i] = '\0';
    rename_buf[i] = '\0';
    rename_len = i;
    rename_mode = true;
}

static void apply_rename(void) {
    rename_mode = false;
    if (rename_len == 0 || str_eq(rename_buf, rename_original)) return;
    if (current_volume != VOLUME_NEMOFS) { set_status("FAT no admite renombrar todavía"); return; }
    int64_t ok = (int64_t)syscall5(SYS_FILE_RENAME, (uint64_t)rename_original, (uint64_t)rename_buf, current_dir, current_volume, 0);
    if (ok == 0) {
        set_status("Renombrado");
        if (str_eq(selected_name, rename_original)) {
            int i = 0; while (rename_buf[i] && i < 27) { selected_name[i] = rename_buf[i]; i++; } selected_name[i] = '\0';
        }
        load_listing();
    } else {
        set_status("NO SE PUDO RENOMBRAR (¿YA EXISTE ESE NOMBRE?)");
    }
}

static void ctx_delete(const char *name, bool is_dir) {
    bool ok = is_dir
        ? delete_dir_recursive(name, current_dir, current_volume, 0)
        : (int64_t)syscall5(SYS_FILE_DELETE, (uint64_t)name, current_dir, current_volume, 0, 0) >= 0;
    if (!ok) set_status("No se pudo eliminar (del todo)");
    else set_status("Eliminado");
    load_listing();
}

static void ctx_copy(const char *name, bool is_dir, bool cut) {
    syscall5(SYS_FILE_CLIPBOARD_SET, (uint64_t)name, current_dir, current_volume, 0, 0);
    clipboard_is_dir = is_dir;
    clipboard_is_cut = cut;
    set_status(cut ? "Cortado (se moverá al pegar)" : "Copiado al portapapeles");
}

static void ctx_paste(void) {
    static char name[28];
    int64_t packed = (int64_t)syscall5(SYS_FILE_CLIPBOARD_GET, (uint64_t)name, sizeof(name), 0, 0, 0);
    if (packed < 0) { set_status("El portapapeles de archivos está vacío"); return; }
    uint32_t src_parent = (uint32_t)((uint64_t)packed >> 32);
    uint32_t src_volume = (uint32_t)((uint64_t)packed & 0xFFFFFFFF);

    bool ok;
    if (clipboard_is_dir) {
        int32_t new_dir = (int32_t)syscall5(SYS_DIR_CREATE, (uint64_t)name, current_dir, current_volume, 0, 0);
        int32_t src_id = (new_dir >= 0) ? (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)name, src_parent, src_volume, 0, 0) : -1;
        ok = (new_dir >= 0) && (src_id >= 0) && copy_dir_contents((uint32_t)src_id, src_volume, (uint32_t)new_dir, current_volume, 0);
        set_status(ok ? "Carpeta pegada" : "Pegado incompleto o fallido (¿ya existe?)");
    } else {
        copia_resultado_t r = copiar_archivo(name, src_parent, src_volume,
                                             name, current_dir, current_volume);
        ok = (r == COPIA_OK);
        switch (r) {
            case COPIA_OK:               set_status("Pegado"); break;
            case COPIA_DEMASIADO_GRANDE: set_status("No pegado: archivo demasiado grande"); break;
            case COPIA_ERROR_ORIGEN:     set_status("El origen ya no existe"); break;
            case COPIA_ERROR_DESTINO:    set_status("NO SE PUDO PEGAR (¿YA EXISTE?)"); break;
        }
    }

    if (ok && clipboard_is_cut) {
        // Cortar = mover: borramos el origen solo si el pegado salio
        // bien, y solo si no era el mismo sitio (cortar y pegar sin
        // moverlo de carpeta no debe borrar lo que se acaba de crear).
        bool same_place = (src_parent == current_dir && src_volume == current_volume);
        if (!same_place) {
            if (clipboard_is_dir) delete_dir_recursive(name, src_parent, src_volume, 0);
            else syscall5(SYS_FILE_DELETE, (uint64_t)name, src_parent, src_volume, 0, 0);
        }
        clipboard_is_cut = false; // un "cortar" se consume en el primer pegado, como en cualquier sistema real
    }
    load_listing();
}

// ---------------------------------------------------------------------
// Dibujo
// ---------------------------------------------------------------------
static uint32_t tree_visible_rows(int content_h) {
    if (content_h <= 0) return 0;
    return (uint32_t)(content_h / TREE_ROW_H);
}
static uint32_t grid_columns(int right_w) {
    uint32_t c = (uint32_t)(right_w / GRID_CELL_W);
    return c == 0 ? 1 : c;
}
static uint32_t grid_visible_rows(int content_h) {
    if (content_h <= 0) return 0;
    return (uint32_t)(content_h / GRID_CELL_H);
}
static uint32_t clamp_scroll_rows(uint32_t scroll, uint32_t total_rows, uint32_t visible) {
    if (visible == 0 || total_rows <= visible) return 0;
    uint32_t max_scroll = total_rows - visible;
    return scroll > max_scroll ? max_scroll : scroll;
}

// El rotulo de un boton de la barra de arriba, centrado en su ancho
static void rotulo_boton(int x, int w, const char *t) {
    draw_text(x + (w - text_px(t)) / 2, 7, t, COLOR_BTN_TEXT);
}

static void redraw(void) {
    usar_fuente_ui();
    draw_rect(0, 0, win_w, win_h, COLOR_BG);

    // Barra de botones, con relieve. El reparto lo lleva barra.h (ver la
    // nota de esa cabecera): con las coordenadas a mano, en una ventana
    // estrecha "Copiar a otro" acababa debajo de "Salir".
    draw_rect(0, 0, win_w, TOPBAR_H, COLOR_TOPBAR);
    const int alto_btn = TOPBAR_H - 4;
    const int x_salir = barra_anclar_derecha(win_w, 58, 4);

    barra_t b;
    barra_iniciar(&b, 2, x_salir, 2);
    int x;

    if (barra_hueco(&b, 62, &x)) {
        define_button(BTN_NEMOFS, x, 2, 62, alto_btn, COLOR_BTN);
        draw_bevel(x, 2, 62, alto_btn, COLOR_BTN, current_volume != VOLUME_NEMOFS);
        rotulo_boton(x, 62, "NemoFS");
    }
    if (barra_hueco(&b, 50, &x)) {
        define_button(BTN_FAT, x, 2, 50, alto_btn, COLOR_BTN);
        draw_bevel(x, 2, 50, alto_btn, COLOR_BTN, current_volume != VOLUME_FAT);
        rotulo_boton(x, 50, "FAT");
    }
    if (barra_hueco(&b, 50, &x)) {
        define_button(BTN_UP, x, 2, 50, alto_btn, COLOR_BTN);
        draw_bevel(x, 2, 50, alto_btn, COLOR_BTN, true);
        rotulo_boton(x, 50, "Subir");
    }
    if (barra_hueco(&b, 76, &x)) {
        define_button(BTN_NEWDIR, x, 2, 76, alto_btn, COLOR_BTN);
        draw_bevel(x, 2, 76, alto_btn, COLOR_BTN, true);
        rotulo_boton(x, 76, "Carpeta");
    }
    if (barra_hueco(&b, 96, &x)) {
        define_button(BTN_COPY, x, 2, 96, alto_btn, COLOR_BTN);
        draw_bevel(x, 2, 96, alto_btn, COLOR_BTN, true);
        rotulo_boton(x, 96, "Copiar a otro");
    }
    define_button(BTN_EXIT, x_salir, 2, 58, alto_btn, 0x00804040);
    draw_bevel(x_salir, 2, 58, alto_btn, 0x00A05050, true);
    rotulo_boton(x_salir, 58, "Salir");

    // Barra de ruta
    draw_rect(0, TOPBAR_H, win_w, PATHBAR_H, COLOR_PATHBAR);
    const char *vol_label = (current_volume == VOLUME_NEMOFS) ? "NemoFS:/" : "FAT (SD o pendrive):/";
    draw_text(4, TOPBAR_H + 3, vol_label, COLOR_PATH_TEXT);

    int content_y = TOPBAR_H + PATHBAR_H;
    int content_h = win_h - content_y - STATUSBAR_H - SCROLL_BTN_H;
    if (content_h < 0) content_h = 0;
    int scrollbtn_y = content_y + content_h;

    // -- Panel izquierdo: arbol de carpetas --
    build_visible_tree();
    uint32_t tree_rows = tree_visible_rows(content_h);
    scroll_left = clamp_scroll_rows(scroll_left, (uint32_t)visible_tree_count, tree_rows);

    draw_bevel(0, content_y, LEFT_W, content_h, COLOR_LEFT_BG, false);
    for (uint32_t row = 0; row < tree_rows && scroll_left + row < (uint32_t)visible_tree_count; row++) {
        int idx = visible_tree[scroll_left + row];
        tree_node_t *nd = &tree[idx];
        int ry = content_y + 1 + (int)row * TREE_ROW_H;
        int tx = 2 + nd->depth * INDENT_PX;

        bool is_current = (nd->inode == current_dir && nd->volume == current_volume);
        if (is_current) draw_rect(tx, ry, LEFT_W - tx - 1, TREE_ROW_H, COLOR_SEL_BG);

        // Caja de expandir/contraer -- siempre se ofrece en toda
        // carpeta, ya que no sabemos si tiene subcarpetas hasta
        // cargarlas la primera vez que se expande.
        draw_bevel(tx, ry + 8, 9, 9, COLOR_BTN, true);
        draw_rect(tx + 2, ry + 12, 5, 1, COLOR_TEXT);                        // "-"
        if (!nd->expanded) draw_rect(tx + 4, ry + 10, 1, 5, COLOR_TEXT);    // "+"

        draw_icon(tx + 12, ry + 1, ICON_FOLDER);
        usar_fuente_nombres();
        draw_text(tx + 12 + ICON_SIZE + 2, ry + 9, nd->name, is_current ? COLOR_TEXT_SEL : COLOR_TEXT);
        usar_fuente_ui();
    }

    // Botones de scroll del arbol (arriba/abajo), en su esquina
    // inferior derecha -- para poder llegar a cualquier carpeta por
    // muy larga que sea la lista, sin depender de la rueda del raton.
    // BUG REAL CORREGIDO: estos 4 botones de scroll (2 por panel) se
    // registraban con SYS_DEFINE_BUTTON, pero cada ventana admite como
    // MAXIMO 8 botones (limite del kernel) -- con los 6 de la barra
    // superior, los 2 ultimos (los de la rejilla) se quedaban sin
    // hueco y el kernel los ignoraba en silencio: se veian pero no
    // respondian al clic. Ahora se detectan por coordenadas (igual
    // que las filas del arbol o las celdas de la rejilla), sin pasar
    // por el sistema de botones -- sin limite de cuantos puede haber.
    draw_bevel(LEFT_W - SCROLL_BTN_W * 2, scrollbtn_y, SCROLL_BTN_W, SCROLL_BTN_H, COLOR_BTN, true);
    draw_text(LEFT_W - SCROLL_BTN_W * 2 + 7, scrollbtn_y + 4, "^", COLOR_TEXT);
    draw_bevel(LEFT_W - SCROLL_BTN_W, scrollbtn_y, SCROLL_BTN_W, SCROLL_BTN_H, COLOR_BTN, true);
    draw_text(LEFT_W - SCROLL_BTN_W + 7, scrollbtn_y + 4, "v", COLOR_TEXT);

    // Divisor
    draw_rect(LEFT_W, content_y, 2, content_h + SCROLL_BTN_H, COLOR_DIVIDER);

    // -- Panel derecho: rejilla de iconos --
    int right_x = LEFT_W + 2;
    int right_w = win_w - right_x;
    uint32_t columns = grid_columns(right_w);
    uint32_t grid_rows_total = (all_count + columns - 1) / columns;
    uint32_t grid_rows_visible = grid_visible_rows(content_h);
    scroll_right = clamp_scroll_rows(scroll_right, grid_rows_total, grid_rows_visible);

    draw_bevel(right_x, content_y, right_w, content_h, COLOR_RIGHT_BG, false);
    for (uint32_t i = scroll_right * columns; i < all_count; i++) {
        uint32_t rel = i - scroll_right * columns;
        uint32_t col = rel % columns;
        uint32_t row = rel / columns;
        int cx = right_x + 2 + (int)(col * GRID_CELL_W);
        int cy = content_y + 2 + (int)(row * GRID_CELL_H);
        if (cy + GRID_CELL_H > win_h - STATUSBAR_H) break;

        entry_t *e = &all_entries[i];
        int icon;
        if (e->type == TYPE_DIR) icon = ICON_FOLDER;
        else if (ends_with_pro(e->name)) icon = ICON_CODE;
        else icon = ICON_TXT;

        bool sel = str_eq(e->name, selected_name);
        if (sel) draw_rect(cx, cy, GRID_CELL_W - 4, GRID_CELL_H - 2, COLOR_SEL_BG);
        int ix = cx + (GRID_CELL_W - 4 - GRID_ICON_PX) / 2;
        if (mosaico >= 0 && i < MAX_ENTRIES && mini_de[i] >= 0) {
            // tiene miniatura: se ve el archivo, no un icono generico
            int c = mini_de[i];
            blit_trozo(mosaico, ix, cy + 4,
                       (c % mos_cols) * mos_lado, (c / mos_cols) * mos_lado, mos_lado, mos_lado);
        } else {
            draw_icon_scaled(ix, cy + 4, icon, (int)grid_icon_scale);
        }
        char line1[28], line2[28];
        usar_fuente_nombres();
        wrap_name_2lines(line1, line2, e->name, GRID_CELL_W - 6);
        uint32_t name_color = sel ? COLOR_TEXT_SEL : COLOR_TEXT;
        int usable = GRID_CELL_W - 4; // ancho de la celda menos el margen de cada lado
        int w1 = text_px(line1);
        int off1 = (usable - w1) / 2; if (off1 < 1) off1 = 1;
        draw_text(cx + off1, cy + GRID_ICON_PX + 10, line1, name_color);
        if (line2[0]) {
            int w2 = text_px(line2);
            int off2 = (usable - w2) / 2; if (off2 < 1) off2 = 1;
            draw_text(cx + off2, cy + GRID_ICON_PX + 23, line2, name_color);   // 13 px: el alto de linea de la sans 10
        }
    }

    usar_fuente_ui();
    // Botones de scroll de la rejilla, misma idea, en su esquina
    // inferior derecha.
    draw_bevel(win_w - SCROLL_BTN_W * 2, scrollbtn_y, SCROLL_BTN_W, SCROLL_BTN_H, COLOR_BTN, true);
    draw_text(win_w - SCROLL_BTN_W * 2 + 7, scrollbtn_y + 4, "^", COLOR_TEXT);
    draw_bevel(win_w - SCROLL_BTN_W, scrollbtn_y, SCROLL_BTN_W, SCROLL_BTN_H, COLOR_BTN, true);
    draw_text(win_w - SCROLL_BTN_W + 7, scrollbtn_y + 4, "v", COLOR_TEXT);

    // Barra de estado, al fondo de la ventana
    int sy = win_h - STATUSBAR_H;
    draw_bevel(0, sy, win_w, STATUSBAR_H, COLOR_STATUSBAR, false);
    if (status_msg[0]) draw_text(4, sy + 3, status_msg, COLOR_STATUS);

    // Menu contextual (clic derecho), encima de todo lo demas
    if (ctx_menu_open) {
        int x, y, w, h, count;
        ctx_menu_geom(&x, &y, &w, &h, &count);

        draw_bevel(x, y, w, h, 0x00F0F0F0, true);

        int row = 0;
        if (ctx_target_name[0]) {
            draw_text(x + 6, y + 4 + row * CTX_ITEM_H, "Eliminar", 0x00000000); row++;
            draw_text(x + 6, y + 4 + row * CTX_ITEM_H, "Copiar", 0x00000000); row++;
            draw_text(x + 6, y + 4 + row * CTX_ITEM_H, "Cortar", 0x00000000); row++;
            draw_text(x + 6, y + 4 + row * CTX_ITEM_H, "Renombrar", 0x00000000); row++;
        }
        draw_text(x + 6, y + 4 + row * CTX_ITEM_H, "Pegar", 0x00000000);
    }

    // Cuadro de renombrar, encima de todo -- estilo similar al del
    // dialogo "Guardar como": caja de texto con lo que se va tecleando
    // y una pista de las teclas que lo cierran.
    if (rename_mode) {
        int w = 220, h = 54;
        int x = (win_w - w) / 2, y = (win_h - h) / 2;
        draw_bevel(x, y, w, h, 0x00F0F0F0, true);
        draw_text(x + 8, y + 6, "Renombrar:", 0x00000000);
        draw_bevel(x + 8, y + 20, w - 16, 16, 0x00FFFFFF, false);
        draw_text(x + 12, y + 24, rename_buf, 0x00000000);
        draw_text(x + 8, y + 40, "Enter acepta, Esc cancela", 0x00606060);
    }
}

// ---------------------------------------------------------------------
// Entrada (raton, teclado)
// ---------------------------------------------------------------------

// Fila del arbol bajo (mx,my), o -1 si el punto cae fuera. 'toggle'
// se pone a true si el clic cayo justo en la caja de expandir/contraer.
static int tree_row_at(int mx, int my, bool *toggle) {
    int content_y = TOPBAR_H + PATHBAR_H;
    if (my < content_y || mx >= LEFT_W) return -1;
    uint32_t row = (uint32_t)((my - content_y - 1) / TREE_ROW_H);
    uint32_t vi = scroll_left + row;
    if (vi >= (uint32_t)visible_tree_count) return -1;
    int idx = visible_tree[vi];
    int tx = 2 + tree[idx].depth * INDENT_PX;
    *toggle = (mx >= tx && mx < tx + 9);
    return idx;
}

// Entrada de la rejilla derecha bajo (mx,my), o NULL si no hay ninguna ahi.
static const entry_t *grid_entry_at(int mx, int my) {
    int content_y = TOPBAR_H + PATHBAR_H;
    int right_x = LEFT_W + 2;
    if (my < content_y || mx < right_x) return 0;
    int right_w = win_w - right_x;
    uint32_t columns = grid_columns(right_w);
    uint32_t col = (uint32_t)((mx - right_x) / GRID_CELL_W);
    uint32_t row = (uint32_t)((my - content_y) / GRID_CELL_H);
    if (col >= columns) return 0;
    uint32_t idx = (scroll_right + row) * columns + col;
    if (idx < all_count) return &all_entries[idx];
    return 0;
}

static int compute_scrollbtn_y(void) {
    int content_y = TOPBAR_H + PATHBAR_H;
    int content_h = win_h - content_y - STATUSBAR_H - SCROLL_BTN_H;
    if (content_h < 0) content_h = 0;
    return content_y + content_h;
}

// Si (mx,my) cae en alguno de los 4 botones de scroll, aplica el
// desplazamiento correspondiente y devuelve true. clamp_scroll_rows
// se encarga de no pasarse de limites en el siguiente redibujado.
static bool try_scroll_button_click(int mx, int my) {
    int by = compute_scrollbtn_y();
    if (my < by || my >= by + SCROLL_BTN_H) return false;
    if (mx >= LEFT_W - SCROLL_BTN_W * 2 && mx < LEFT_W - SCROLL_BTN_W) {
        if (scroll_left > 0) scroll_left--;
        return true;
    }
    if (mx >= LEFT_W - SCROLL_BTN_W && mx < LEFT_W) {
        scroll_left++;
        return true;
    }
    if (mx >= win_w - SCROLL_BTN_W * 2 && mx < win_w - SCROLL_BTN_W) {
        if (scroll_right > 0) scroll_right--;
        return true;
    }
    if (mx >= win_w - SCROLL_BTN_W && mx < win_w) {
        scroll_right++;
        return true;
    }
    return false;
}

static void handle_left_click(int mx, int my) {
    if (my < TOPBAR_H) return;
    if (mx < LEFT_W) {
        bool toggle = false;
        int idx = tree_row_at(mx, my, &toggle);
        if (idx < 0) return;
        if (toggle) {
            tree[idx].expanded = !tree[idx].expanded;
            if (tree[idx].expanded) tree_load_children(idx);
        } else {
            tree[idx].expanded = true;
            tree_load_children(idx);
            current_volume = tree[idx].volume;
            current_dir = tree[idx].inode;
            dir_depth = 0; // navegar por el arbol reinicia la pila de "subir"
            selected_name[0] = '\0';
            load_listing();
        }
        return;
    }
    const entry_t *e = grid_entry_at(mx, my);
    if (!e) { selected_name[0] = '\0'; ultimo_clic[0] = '\0'; return; }   // clic en el hueco: nada seleccionado
    uint64_t ahora = syscall5(SYS_GET_TICKS, 0, 0, 0, 0, 0);
    bool doble = str_eq(e->name, ultimo_clic) && (ahora - ultimo_clic_tick) < DOBLE_CLIC_TICKS;
    seleccionar_entrada(e);
    if (doble) {
        ultimo_clic[0] = '\0';          // un tercer clic empieza de nuevo
        abrir_entrada(e);
    } else {
        int i = 0; while (e->name[i] && i < 27) { ultimo_clic[i] = e->name[i]; i++; } ultimo_clic[i] = '\0';
        ultimo_clic_tick = ahora;
    }
}

static void open_context_menu(int mx, int my) {
    ctx_x = mx;
    ctx_y = my;
    ctx_target_name[0] = '\0';

    const entry_t *e = grid_entry_at(mx, my);
    if (e) {
        int i = 0;
        while (e->name[i] && i < 27) { ctx_target_name[i] = e->name[i]; i++; }
        ctx_target_name[i] = '\0';
        ctx_target_is_dir = (e->type == TYPE_DIR);
    }
    ctx_menu_open = true;
}

static void handle_context_click(int mx, int my) {
    int x, y, w, h, count;
    ctx_menu_geom(&x, &y, &w, &h, &count);

    ctx_menu_open = false; // se cierra siempre, caiga donde caiga el clic

    if (mx < x || mx >= x + w || my < y || my >= y + h) return;

    // La fila se cuenta desde donde se dibuja el PRIMER texto (y + 4),
    // no desde y + 2: con el desfase de antes, los dos pixeles altos de
    // cada fila activaban la opcion de arriba.
    int item = (my - y - 4) / CTX_ITEM_H;
    if (item < 0) item = 0;                 // el borde de arriba cuenta como la primera
    if (item >= count) return;              // por si acaso: fuera del menu, nada

    if (ctx_target_name[0]) {
        if (item == 0) ctx_delete(ctx_target_name, ctx_target_is_dir);
        else if (item == 1) ctx_copy(ctx_target_name, ctx_target_is_dir, false);
        else if (item == 2) ctx_copy(ctx_target_name, ctx_target_is_dir, true);
        else if (item == 3) begin_rename(ctx_target_name);
        else if (item == 4) ctx_paste();
    } else {
        if (item == 0) ctx_paste();
    }
}

__attribute__((section(".text.start")))
void _start(void) {
    // Ventana de inicio mas grande que la que da el lanzador por
    // defecto -- con celdas de rejilla mas anchas hace falta mas
    // sitio para que se vean varias columnas comodas.
    syscall5(SYS_CREATE_WINDOW, (uint64_t)"Explorador", 40, 40, 640, 420);
    cargar_fuentes();

    uint64_t size = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
    win_w = (int)(size >> 32);
    win_h = (int)(size & 0xFFFFFFFF);
    if (win_w <= 0) win_w = 640;
    if (win_h <= 0) win_h = 420;

    // Escala de icono compartida con el escritorio (AJUSTES): 1 =
    // celdas compactas, 2 = celdas anchas con sitio para dos lineas
    // de nombre.
    grid_icon_scale = (int32_t)syscall5(SYS_DESKTOP_ICON_SCALE_GET, 0, 0, 0, 0, 0);
    if (grid_icon_scale != 2) grid_icon_scale = 1;
    if (grid_icon_scale == 2) {
        // Anchos medidos con la sans 10: en 86 px utiles caben en una linea
        // DOCUMENTOS (72), CALCULADORA (77), CREATEWINDOW (85); los mas
        // largos se parten por la extension. Con la 5x7 bastaban 76.
        GRID_ICON_PX_v = 48; GRID_CELL_W_v = 116; GRID_CELL_H_v = 94;   // dos lineas de nombre en sans 10
    } else {
        GRID_ICON_PX_v = 24; GRID_CELL_W_v = 92; GRID_CELL_H_v = 66;
    }

    tree_init();
    load_listing();
    redraw();

    bool last_left = false;
    bool last_right = false;

    while (running) {
        uint64_t pump = syscall5(SYS_PUMP, 0, 0, 0, 0, 0);
        if ((int64_t)pump < 0) break;
        // SYS_CREATE_WINDOW deja la ventana en "modo evento": la X ya
        // no la destruye sola, solo dispara EVENT_WINDOWCLOSE (0x803).
        if ((int64_t)syscall5(SYS_POLL_EVENT, 0, 0, 0, 0, 0) == EVENT_WINDOWCLOSE) break;

        uint64_t sz = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
        int nw = (int)(sz >> 32), nh = (int)(sz & 0xFFFFFFFF);
        bool need_redraw = (nw > 0 && nw != win_w) || (nh > 0 && nh != win_h);
        if (nw > 0) win_w = nw;
        if (nh > 0) win_h = nh;

        uint32_t btn = (uint32_t)syscall5(SYS_GET_BUTTON_ID, 0, 0, 0, 0, 0);
        if (btn == BTN_NEMOFS) { if (current_volume != VOLUME_NEMOFS) switch_volume(VOLUME_NEMOFS); need_redraw = true; }
        else if (btn == BTN_FAT) { if (current_volume != VOLUME_FAT) switch_volume(VOLUME_FAT); need_redraw = true; }
        else if (btn == BTN_UP) { go_up(); need_redraw = true; }
        else if (btn == BTN_NEWDIR) { new_folder(); need_redraw = true; }
        else if (btn == BTN_COPY) { copy_selected_to_other_volume(); need_redraw = true; }
        else if (btn == BTN_EXIT) { running = false; break; }

        uint64_t m = syscall5(SYS_GET_MOUSE, 0, 0, 0, 0, 0);
        if (m != (uint64_t)-1) {
            int mx = (int)((m >> 32) & 0xFFFF);
            int my = (int)((m >> 16) & 0xFFFF);
            bool left = (m & 1) != 0;
            bool right = (m & 2) != 0;

            int32_t wheel = (int32_t)syscall5(SYS_GET_MOUSE_WHEEL, 0, 0, 0, 0, 0);
            if (wheel != 0) {
                uint32_t *scroll = (mx < LEFT_W) ? &scroll_left : &scroll_right;
                if (wheel > 0) *scroll = ((uint32_t)wheel > *scroll) ? 0 : *scroll - (uint32_t)wheel;
                else *scroll += (uint32_t)(-wheel);
                need_redraw = true;
            }

            if (right && !last_right && !ctx_menu_open && my >= TOPBAR_H) {
                open_context_menu(mx, my);
                need_redraw = true;
            }
            last_right = right;

            if (left && !last_left) {
                if (rename_mode) {
                    // El cuadro de renombrar solo se cierra con teclado
                    // (Enter/Esc) -- un clic de mas no debe navegar por
                    // debajo sin querer mientras esta abierto.
                } else if (ctx_menu_open) {
                    handle_context_click(mx, my);
                } else if (!try_scroll_button_click(mx, my)) {
                    handle_left_click(mx, my);
                }
                need_redraw = true;
            }
            last_left = left;
        } else {
            last_left = false;
            last_right = false;
        }

        char c = (char)syscall5(SYS_READ_CHAR, 0, 0, 0, 0, 0);
        if (rename_mode) {
            if (c == 27) { rename_mode = false; need_redraw = true; }
            else if (c == '\n') { apply_rename(); need_redraw = true; }
            else if (c == '\b') {
                if (rename_len > 0) { rename_len--; rename_buf[rename_len] = '\0'; need_redraw = true; }
            } else if (c >= 32 && c < 127 && rename_len < 27) {
                // Mayusculas: coherente con como se ven ya los nombres
                // en el resto del explorador y con el dialogo de guardar.
                char up = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
                rename_buf[rename_len++] = up;
                rename_buf[rename_len] = '\0';
                need_redraw = true;
            }
        } else if (c == '\n' && selected_name[0]) {
            // Enter abre la entrada seleccionada
            for (uint32_t k = 0; k < all_count; k++) {
                if (str_eq(all_entries[k].name, selected_name)) { abrir_entrada(&all_entries[k]); need_redraw = true; break; }
            }
        } else if (c == 'q' || c == 'Q') {
            running = false;
        }

        if (need_redraw) redraw();
    }
}
