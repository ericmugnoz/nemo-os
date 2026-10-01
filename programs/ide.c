// ide.c — Nemo OS
//
// IDE estilo BlitzPlus: varias pestañas de codigo abiertas a la vez,
// con "Compilar" / "Compilar y Ejecutar" integrado -- usa nbc.pro,
// el compilador+ensamblador que corre DENTRO del propio Nemo OS (ver
// nbc_main.c), asi que compilar desde aqui no necesita salir del
// sistema para nada.
//
// Construido sobre la misma base que editor.c (menus, portapapeles,
// dialogo comun de archivos), pero con el estado de "un solo
// documento" convertido en un array de documentos (uno por pestaña).

#include <stdint.h>
#include <stdbool.h>

#define SYS_CLIPBOARD_SET    3
#define SYS_CLIPBOARD_GET    4
#define SYS_LAUNCH_PROGRAM   5
#define SYS_GET_LAUNCH_ARG   6
#define SYS_READ_CONSOLE_OUTPUT 7
#define SYS_WRITE_STRING     11
#define SYS_READ_CHAR        12
#define SYS_PUMP             14
#define SYS_FILE_OPEN        20
#define SYS_FILE_READ        21
#define SYS_FILE_WRITE       22
#define SYS_FILE_LIST        23
#define SYS_DRAW_RECT        30
#define SYS_DRAW_TEXT        31
#define SYS_GET_WINDOW_SIZE  33
#define SYS_GET_MOUSE        34
#define SYS_OPEN_FILE_DIALOG 38
#define SYS_SAVE_FILE_DIALOG 39

#define CH_UP    0x11
#define CH_DOWN  0x12
#define CH_LEFT  0x13
#define CH_RIGHT 0x14

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

static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    syscall5(SYS_DRAW_RECT, (uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h, color);
}
// ---- Fuente ----
// La mono de 10 px del sistema: avanza EXACTAMENTE 6 px por caracter,
// como la 5x7 de antes, asi que todas las columnas y anchos calculados
// para ella siguen valiendo; ahora con minusculas y acentos. draw_text
// compensa la altura para que el texto quede donde quedaba (las
// coordenadas son las de siempre, pensadas para glifos de 7 px).
#ifndef SYS_LOAD_FONT
#define SYS_LOAD_FONT 189
#endif
#ifndef SYS_SET_FONT
#define SYS_SET_FONT  191
#endif
static int texto_dy = 0;
static void draw_text(int x, int y, const char *s, uint32_t color) {
    syscall5(SYS_DRAW_TEXT, (uint64_t)x, (uint64_t)(y + texto_dy), (uint64_t)s, color, 0);
}
static void cargar_fuente_mono(void) {
    int64_t f = (int64_t)syscall5(SYS_LOAD_FONT, (uint64_t)"mono", 10, 0, 0, 0);
    if (f > 0) { syscall5(SYS_SET_FONT, (uint64_t)f, 0, 0, 0, 0); texto_dy = -2; }
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}
static bool str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
static char to_upper_ascii(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }
static bool str_contains_ci(const char *hay, const char *needle) {
    int hl = str_len(hay), nl = str_len(needle);
    if (nl == 0) return true;
    for (int i = 0; i + nl <= hl; i++) {
        bool ok = true;
        for (int j = 0; j < nl; j++) {
            if (to_upper_ascii(hay[i + j]) != to_upper_ascii(needle[j])) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

// LIMITE REAL ENCONTRADO Y CORREGIDO: 60 lineas era demasiado poco
// (misma causa raiz que en editor.c -- un archivo de ejemplo real de
// 87 lineas se truncaba EN SILENCIO al abrirlo, dando la impresion de
// un "error del compilador" cuando el archivo que nbc.pro recibia ya
// venia incompleto). Con MAX_TABS=6, el coste de ampliar esto es de
// ~1.24MB -- de sobra dentro de los 16MB disponibles por tarea.
#define MAX_LINES 2000
// Y las COLUMNAS: eran 80, y una linea mas larga se cortaba EN
// SILENCIO al abrir el archivo; al compilar, el IDE guardaba lo cortado y el
// compilador veia un programa roto ("se esperaba un numero..." en una linea
// que en el disco estaba bien). La misma leccion que con las filas. Ahora
// 255 (unos 3,6 MB con las 6 pestañas), y si aun asi una linea no cabe, se
// avisa y el archivo NO se guarda: el del disco queda intacto.
#define MAX_COLS  255
#define MAX_TABS  6

typedef struct {
    bool used;
    char lines[MAX_LINES][MAX_COLS + 1];
    int line_count;
    int cursor_row, cursor_col;
    int32_t file_inode; // -1 = documento nuevo, sin guardar aun
    int32_t file_parent; // carpeta donde vive de verdad (0 = raiz);
                          // NO se toca al compilar: nbc.pro abre ahi
                          // directamente, sin copia en la raiz.
    // Y en QUE DISCO. Antes se pasaba 0 (NemoFS) a pelo en
    // las llamadas de archivo, asi que un .nb de la tarjeta no se podia
    // abrir ni guardar. Va por pestaña: se pueden tener a la vez un
    // archivo del sistema y otro de la tarjeta. 0 = NemoFS, 1 = FAT.
    uint32_t file_volume;
    char file_name[28];
    bool has_selection;
    int sel_row, sel_start_col, sel_end_col;
    // Primera linea VISIBLE de ESTE documento. Antes el IDE dibujaba
    // siempre desde la linea 0, asi que un programa mas largo que la
    // ventana no se podia ver entero. Va por documento, no global, para
    // que cada pestaña recuerde por donde iba.
    int scroll_top;
    // Primera COLUMNA visible: una linea mas ancha que la
    // ventana se veia cortada y el cursor se perdia por la derecha.
    int scroll_col;
    // Al abrirlo se cortaron lineas demasiado largas: no se guarda nunca,
    // para no destruir el archivo del disco.
    bool recortado;
} Document;

static Document docs[MAX_TABS];
static int active_tab;
static int tab_count;

static int win_w, win_h;
static bool running;
static char status_msg[48] = "";
static uint32_t documentos_dir = 0;

#define COLOR_BG        0x00202020
#define COLOR_TEXT      0x00E0E0E0
#define COLOR_CURSOR    0x0000FF00
#define COLOR_SELECTION 0x00405070
#define COLOR_MENUBAR   0x00D4D0C8
#define COLOR_MENUBAR_TEXT 0x00000000
#define COLOR_MENU_BG   0x00F0F0F0
#define COLOR_STATUSBAR 0x00303030
#define COLOR_STATUS_TEXT 0x00A0A0A0
#define COLOR_TAB_ACTIVE   0x00202020
#define COLOR_TAB_INACTIVE 0x00B8B4A8
#define COLOR_TAB_BAR      0x00C8C4B8
#define COLOR_BUILD_BG     0x00101018
#define COLOR_BUILD_TEXT   0x0000CC44
#define COLOR_BUILD_ERR    0x00FF6644

// ---- resaltado de Bitacora (.bit) -- todo en tonos de verde, para
// que un archivo .bit se reconozca de un vistazo frente a un .bb.
// El brillo/tono distingue palabra clave / tipo / cadena / numero /
// comentario, pero nunca sale del verde.
#define COLOR_BIT_DEFAULT  0x0044CC55 // identificadores, operadores, puntuacion
#define COLOR_BIT_KEYWORD  0x0033FF66 // bitacora, si, mientras, funcion...
#define COLOR_BIT_TYPE     0x0000CC88 // entero / texto / booleano
#define COLOR_BIT_STRING   0x00E0E0E0 // "cadenas" -- texto literal, no es
                                       // parte del lenguaje, se distingue
                                       // en blanco en vez de verde
#define COLOR_BIT_NUMBER   0x0099FF99 // literales numericos, verdadero/falso
#define COLOR_BIT_COMMENT  0x00337733 // // comentarios

#define MENUBAR_H 16
#define TAB_BAR_H 16
#define MENU_ROW_H 16
#define LINE_H 12
#define TEXT_TOP (MENUBAR_H + TAB_BAR_H + 4)

#define SYS_GET_MOUSE_WHEEL 45

// Cuantas lineas de codigo caben entre las pestañas y la barra de
// estado.
static int lineas_visibles(void) {
    int alto = win_h - 12 - TEXT_TOP;   // el -12 es la barra de estado
    int n = alto / LINE_H;
    return n < 1 ? 1 : n;
}

// Mueve la ventana de texto lo justo para que el cursor quede dentro.
// Se llama al dibujar, asi que ningun sitio que mueva el cursor tiene
// que acordarse de nada.
static void asegurar_cursor_visible(Document *d) {
    int visibles = lineas_visibles();
    if (d->cursor_row < d->scroll_top) d->scroll_top = d->cursor_row;
    if (d->cursor_row >= d->scroll_top + visibles) d->scroll_top = d->cursor_row - visibles + 1;
    int max_top = d->line_count - visibles;
    if (max_top < 0) max_top = 0;
    if (d->scroll_top > max_top) d->scroll_top = max_top;
    if (d->scroll_top < 0) d->scroll_top = 0;
    // en horizontal: que la columna del cursor quede a la vista
    int columnas = (win_w - 8) / 6;
    if (columnas < 10) columnas = 10;
    if (d->cursor_col < d->scroll_col) d->scroll_col = d->cursor_col;
    if (d->cursor_col >= d->scroll_col + columnas) d->scroll_col = d->cursor_col - columnas + 1;
    if (d->scroll_col < 0) d->scroll_col = 0;
}
#define TAB_W 104   // 90 dejaba un nombre de 12 caracteres pegado a la X

// -- menus --
#define MENU_NONE 0
#define MENU_FILE 1
#define MENU_EDIT 2
#define MENU_RUN  3
#define MENU_HELP 4
static int open_menu = MENU_NONE;

#define FILE_ITEMS 7
#define EDIT_ITEMS 4
#define RUN_ITEMS  2
#define HELP_ITEMS 1

#define MENU_X_FILE 0
#define MENU_X_EDIT 64
#define MENU_X_RUN  128
#define MENU_X_HELP 200

// IMPORTANTE: nada de arrays estaticos de punteros a cadenas, ni de
// switch con muchos casos -- mismo motivo que en todo el resto del
// proyecto (ver el Makefile: -fno-jump-tables -fno-tree-switch-conversion).
static const char *menu_item_text(int menu, int index) {
    if (menu == MENU_FILE) {
        if (index == 0) return "Nuevo";
        if (index == 1) return "Nueva pestaña";
        if (index == 2) return "Abrir";
        if (index == 3) return "Guardar";
        if (index == 4) return "Guardar como";
        if (index == 5) return "Cerrar pestaña";
        if (index == 6) return "Salir";
    } else if (menu == MENU_EDIT) {
        if (index == 0) return "Cortar";
        if (index == 1) return "Copiar";
        if (index == 2) return "Pegar";
        if (index == 3) return "Seleccionar todo";
    } else if (menu == MENU_RUN) {
        if (index == 0) return "Compilar";
        if (index == 1) return "Compilar y ejecutar";
    } else if (menu == MENU_HELP) {
        if (index == 0) return "Acerca de";
    }
    return "";
}

// -- dialogos --
#define DLG_NONE  0
#define DLG_ABOUT 1
#define DLG_BUILD 2
static int dialog_mode = DLG_NONE;

// -- estado de compilacion en curso --
#define BUILD_LOG_LINES 10
#define BUILD_LOG_COLS  70
static char build_log[BUILD_LOG_LINES][BUILD_LOG_COLS + 1];
static int build_log_count;
static char build_line_buf[BUILD_LOG_COLS + 1];
static int build_line_len;
static bool build_in_progress;
static bool build_auto_run;
static bool build_launched; // true una vez que YA lanzamos el .pro resultante -- seguimos escuchando su salida
static int build_tab = -1;   // la pestaña que se compilo (para ir a la linea de un error)
static bool build_is_window_app; // true si el codigo fuente usa graficos/gadgets -- no hace falta shell
static char build_target_name[32]; // el .nb que se esta compilando
// Carpeta donde vive ese .nb -- y por tanto donde nbc.pro deja el .pro,
// porque lo escribe JUNTO AL FUENTE, no en la raiz. Sin esto, lanzar el
// resultado solo por su nombre fallaba en silencio para cualquier
// archivo que no estuviera en la raiz.
static uint32_t build_target_parent;

static void set_status(const char *msg) {
    int i = 0;
    while (msg[i] && i < 47) { status_msg[i] = msg[i]; i++; }
    status_msg[i] = '\0';
}

// ---- documentos / pestañas ----

static void clear_doc(Document *d) {
    for (int i = 0; i < MAX_LINES; i++) d->lines[i][0] = '\0';
    d->line_count = 1;
    d->cursor_row = 0;
    d->scroll_top = 0;   // documento nuevo o recien abierto: empezar arriba
    d->scroll_col = 0;
    d->recortado = false;
    d->cursor_col = 0;
    d->has_selection = false;
    d->file_inode = -1;
    d->file_volume = 0;   // un documento nuevo nace en NemoFS
    d->file_parent = 0;
    const char *untitled = "Sin título";
    int i = 0;
    while (untitled[i]) { d->file_name[i] = untitled[i]; i++; }
    d->file_name[i] = '\0';
    d->used = true;
}

static void new_tab(void) {
    if (tab_count >= MAX_TABS) { set_status("Máximo de pestañas"); return; }
    clear_doc(&docs[tab_count]);
    active_tab = tab_count;
    tab_count++;
}

static void copy_document(Document *dst, const Document *src) {
    // Nada de "docs[i] = docs[i+1]" (copia de struct completo) --
    // para una estructura tan grande, el compilador lo convierte en
    // una llamada a memcpy(), que no existe en nuestro entorno sin
    // libreria estandar. Copiamos a mano, byte a byte.
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < sizeof(Document); i++) d[i] = s[i];
}

static void close_tab(int idx) {
    if (tab_count <= 1) { set_status("Es la única pestaña"); return; }
    for (int i = idx; i < tab_count - 1; i++) copy_document(&docs[i], &docs[i + 1]);
    tab_count--;
    if (active_tab >= tab_count) active_tab = tab_count - 1;
}

// ---- menu desplegable: dibujo y deteccion de clics ----

static void draw_menubar(void) {
    draw_rect(0, 0, win_w, MENUBAR_H, COLOR_MENUBAR);
    draw_text(6, 4, "Archivo", COLOR_MENUBAR_TEXT);
    draw_text(70, 4, "Edición", COLOR_MENUBAR_TEXT);
    draw_text(134, 4, "Ejecutar", COLOR_MENUBAR_TEXT);
    draw_text(206, 4, "Ayuda", COLOR_MENUBAR_TEXT);
}

static void draw_tabbar(void) {
    draw_rect(0, MENUBAR_H, win_w, TAB_BAR_H, COLOR_TAB_BAR);
    int x = 2;
    for (int i = 0; i < tab_count; i++) {
        bool active = (i == active_tab);
        draw_rect(x, MENUBAR_H, TAB_W - 2, TAB_BAR_H, active ? COLOR_TAB_ACTIVE : COLOR_TAB_INACTIVE);
        // El titulo, recortado por CARACTERES, no por bytes: una letra con
        // tilde son dos bytes en UTF-8, y "Sin título" salia "Sin títu".
        // Caben 12 (72 px a 6 px por caracter, antes de la X); si no, 11
        // y "...".
        char label[40];
        const char *nm = docs[i].file_name;
        int total = 0;
        for (int k = 0; nm[k]; k++) if ((nm[k] & 0xC0) != 0x80) total++;
        int caben = (total <= 12) ? 12 : 11, n = 0, j = 0;
        while (nm[j] && j < 36) {
            if ((nm[j] & 0xC0) != 0x80) { if (n == caben) break; n++; }   // inicio de un caracter
            label[j] = nm[j]; j++;
        }
        if (total > 12) { label[j++] = (char)0xE2; label[j++] = (char)0x80; label[j++] = (char)0xA6; }   // "..."
        label[j] = '\0';
        draw_text(x + 4, MENUBAR_H + 4, label, active ? COLOR_TEXT : COLOR_MENUBAR_TEXT);
        draw_text(x + TAB_W - 14, MENUBAR_H + 4, "X", active ? COLOR_TEXT : COLOR_MENUBAR_TEXT);
        x += TAB_W;
    }
}

static void draw_dropdown(int menu, int count, int x) {
    int w = 150;
    int h = count * MENU_ROW_H + 4;
    draw_rect(x, MENUBAR_H, w, h, COLOR_MENU_BG);
    draw_rect(x, MENUBAR_H, w, 1, 0x00000000);
    for (int i = 0; i < count; i++) {
        draw_text(x + 6, MENUBAR_H + 4 + i * MENU_ROW_H, menu_item_text(menu, i), COLOR_MENUBAR_TEXT);
    }
}

// ---- portapapeles ----

static void copy_selection_to_clipboard(void) {
    Document *d = &docs[active_tab];
    if (!d->has_selection) return;
    int a = d->sel_start_col < d->sel_end_col ? d->sel_start_col : d->sel_end_col;
    int b = d->sel_start_col < d->sel_end_col ? d->sel_end_col : d->sel_start_col;
    syscall5(SYS_CLIPBOARD_SET, (uint64_t)&d->lines[d->sel_row][a], (uint64_t)(b - a), 0, 0, 0);
    set_status("Copiado");
}

static void delete_selection(void) {
    Document *d = &docs[active_tab];
    if (!d->has_selection) return;
    int a = d->sel_start_col < d->sel_end_col ? d->sel_start_col : d->sel_end_col;
    int b = d->sel_start_col < d->sel_end_col ? d->sel_end_col : d->sel_start_col;
    int len = str_len(d->lines[d->sel_row]);
    for (int i = a; i + (b - a) < len + 1; i++) d->lines[d->sel_row][i] = d->lines[d->sel_row][i + (b - a)];
    d->cursor_row = d->sel_row;
    d->cursor_col = a;
    d->has_selection = false;
}

static void paste_from_clipboard(void) {
    Document *d = &docs[active_tab];
    static char buf[512];
    uint64_t n = syscall5(SYS_CLIPBOARD_GET, (uint64_t)buf, sizeof(buf) - 1, 0, 0, 0);
    for (uint64_t i = 0; i < n; i++) {
        int len = str_len(d->lines[d->cursor_row]);
        if (len >= MAX_COLS) break;
        for (int j = len; j > d->cursor_col; j--) d->lines[d->cursor_row][j] = d->lines[d->cursor_row][j - 1];
        d->lines[d->cursor_row][d->cursor_col] = buf[i];
        d->lines[d->cursor_row][len + 1] = '\0';
        d->cursor_col++;
    }
    set_status("Pegado");
}

// ---- edicion de texto ----

static void insert_char(char c) {
    Document *d = &docs[active_tab];
    if (d->has_selection) delete_selection();
    int len = str_len(d->lines[d->cursor_row]);
    if (len >= MAX_COLS) return;
    for (int i = len; i > d->cursor_col; i--) d->lines[d->cursor_row][i] = d->lines[d->cursor_row][i - 1];
    d->lines[d->cursor_row][d->cursor_col] = c;
    d->lines[d->cursor_row][len + 1] = '\0';
    d->cursor_col++;
}

static void split_line(void) {
    Document *d = &docs[active_tab];
    if (d->line_count >= MAX_LINES) return;
    for (int i = d->line_count; i > d->cursor_row + 1; i--) {
        int j = 0;
        while (d->lines[i - 1][j]) { d->lines[i][j] = d->lines[i - 1][j]; j++; }
        d->lines[i][j] = '\0';
    }
    int len = str_len(d->lines[d->cursor_row]);
    int j = 0;
    for (int i = d->cursor_col; i < len; i++) d->lines[d->cursor_row + 1][j++] = d->lines[d->cursor_row][i];
    d->lines[d->cursor_row + 1][j] = '\0';
    d->lines[d->cursor_row][d->cursor_col] = '\0';
    d->line_count++;
    d->cursor_row++;
    d->cursor_col = 0;
}

static void backspace(void) {
    Document *d = &docs[active_tab];
    if (d->has_selection) { delete_selection(); return; }
    if (d->cursor_col > 0) {
        int len = str_len(d->lines[d->cursor_row]);
        for (int i = d->cursor_col - 1; i < len; i++) d->lines[d->cursor_row][i] = d->lines[d->cursor_row][i + 1];
        d->cursor_col--;
    } else if (d->cursor_row > 0) {
        int prev_len = str_len(d->lines[d->cursor_row - 1]);
        int this_len = str_len(d->lines[d->cursor_row]);
        if (prev_len + this_len < MAX_COLS) {
            for (int i = 0; i < this_len; i++) d->lines[d->cursor_row - 1][prev_len + i] = d->lines[d->cursor_row][i];
            d->lines[d->cursor_row - 1][prev_len + this_len] = '\0';
            for (int i = d->cursor_row; i < d->line_count - 1; i++) {
                int j = 0;
                while (d->lines[i + 1][j]) { d->lines[i][j] = d->lines[i + 1][j]; j++; }
                d->lines[i][j] = '\0';
            }
            d->line_count--;
            d->cursor_row--;
            d->cursor_col = prev_len;
        }
    }
}

static void move_cursor(char c) {
    Document *d = &docs[active_tab];
    d->has_selection = false;
    if (c == CH_LEFT) {
        if (d->cursor_col > 0) d->cursor_col--;
        else if (d->cursor_row > 0) { d->cursor_row--; d->cursor_col = str_len(d->lines[d->cursor_row]); }
    } else if (c == CH_RIGHT) {
        int len = str_len(d->lines[d->cursor_row]);
        if (d->cursor_col < len) d->cursor_col++;
        else if (d->cursor_row < d->line_count - 1) { d->cursor_row++; d->cursor_col = 0; }
    } else if (c == CH_UP) {
        if (d->cursor_row > 0) {
            d->cursor_row--;
            int len = str_len(d->lines[d->cursor_row]);
            if (d->cursor_col > len) d->cursor_col = len;
        }
    } else if (c == CH_DOWN) {
        if (d->cursor_row < d->line_count - 1) {
            d->cursor_row++;
            int len = str_len(d->lines[d->cursor_row]);
            if (d->cursor_col > len) d->cursor_col = len;
        }
    }
}

static void select_all(void) {
    Document *d = &docs[active_tab];
    d->sel_row = d->cursor_row;
    d->sel_start_col = 0;
    d->sel_end_col = str_len(d->lines[d->cursor_row]);
    d->has_selection = true;
}

// ---- archivos: abrir / guardar ----

static void find_documentos(void) {
    static uint8_t raw[16 * 40];
    uint64_t total = syscall5(SYS_FILE_LIST, 0, (uint64_t)raw, 16, 0, 0);
    uint64_t shown = total < 16 ? total : 16;
    for (uint64_t i = 0; i < shown; i++) {
        uint8_t *e = raw + i * 40;
        uint32_t inode = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
        char name[28];
        int j = 0;
        while (e[12 + j] && j < 27) { name[j] = (char)e[12 + j]; j++; }
        name[j] = '\0';
        if (str_eq(name, "DOCUMENTOS")) { documentos_dir = inode; return; }
    }
}

static void load_file(int32_t inode, const char *name, uint32_t volume) {
    Document *d = &docs[active_tab];
    static char content[MAX_LINES * (MAX_COLS + 1)];
    int64_t bytes = (int64_t)syscall5(SYS_FILE_READ, (uint64_t)inode, (uint64_t)content, sizeof(content) - 1, volume, 0);
    if (bytes < 0) bytes = 0;

    clear_doc(d);
    d->file_inode = inode;
    d->file_volume = volume;
    d->file_parent = 0;   // por defecto raiz; try_open_from_launch_arg lo corrige si aplica
    int i = 0, ni = 0;
    while (name[i] && ni < 27) { d->file_name[ni] = name[i]; i++; ni++; }
    d->file_name[ni] = '\0';

    int row = 0, col = 0, cortadas = 0;
    bool esta_cortada = false;
    int64_t p = 0;
    for (; p < bytes && row < MAX_LINES; p++) {
        char c = content[p];
        if (c == '\n') { d->lines[row][col] = '\0'; row++; col = 0; esta_cortada = false; }
        else if (col < MAX_COLS) { d->lines[row][col] = c; col++; }
        else if (!esta_cortada) { esta_cortada = true; cortadas++; }    // antes: se perdia en silencio
    }
    if (cortadas > 0) d->recortado = true;
    // BUG REAL CORREGIDO: si el archivo terminaba en '\n' (como
    // siempre lo deja save_to_inode, incluso tras la ultima linea),
    // el salto final YA cerro la ultima linea real dentro del bucle
    // -- cerrar aqui ademas, sin condicion, añadia una linea en
    // blanco de mas cada vez que se abria el archivo. Guardado ese
    // archivo (p.ej. al compilar) y vuelto a abrir, cada ciclo sumaba
    // otra linea vacia: el sintoma real era "el archivo crece solo
    // cada vez que lo compilo". Solo se cierra una linea aqui si
    // quedaba contenido sin terminar en salto de linea, o si el
    // archivo estaba vacio del todo.
    if (col > 0 || bytes == 0) {
        d->lines[row][col] = '\0';
    } else if (row > 0) {
        row--;   // el ultimo '\n' ya cerro la ultima linea real
    }
    // AVISO REAL, NO SILENCIOSO -- ver la nota identica en editor.c.
    bool truncated = (p < bytes) || (bytes == (int64_t)(sizeof(content) - 1));
    d->line_count = row + 1;
    d->cursor_row = 0;
    d->scroll_top = 0;   // al abrir un archivo, empezar por el principio
    d->cursor_col = 0;
    // Cualquier recorte al abrir deja el documento SIN GUARDAR:
    // antes se avisaba, pero compilar (que guarda antes) destruia igual la
    // parte que no se habia cargado.
    if (truncated) d->recortado = true;
    if (truncated) set_status("Archivo demasiado grande: se ve incompleto y no se guardará");
    else if (d->recortado) set_status("Líneas de más de 255 caracteres cortadas: no se guardará");
    else set_status("Abierto");
}

// El argumento de lanzamiento (si lo hay) viene en formato
// "inodo_padre:nombre" -- lo construye el explorador cuando pide
// abrir un archivo con nosotros via SYS_LAUNCH_PROGRAM.
static void try_open_from_launch_arg(void) {
    static char arg[48];   // (32 cortaba "carpeta:nombre" con nombres largos)
    uint64_t len = syscall5(SYS_GET_LAUNCH_ARG, (uint64_t)arg, sizeof(arg), 0, 0, 0);
    if (len == 0) return;
    int i = 0;
    // "padre:nombre" de siempre, y ademas "F:padre:nombre" para un
    // archivo de la tarjeta. El prefijo es opcional: quien lanzaba el
    // IDE antes de esto sigue funcionando igual.
    uint32_t volume = 0;
    if ((arg[0] == 'F' || arg[0] == 'f') && arg[1] == ':') { volume = 1; i = 2; }

    uint32_t parent = 0;
    int digitos = 0;
    while (arg[i] >= '0' && arg[i] <= '9') { parent = parent * 10 + (uint32_t)(arg[i] - '0'); i++; digitos++; }
    if (digitos == 0 || arg[i] != ':') return;
    const char *name = &arg[i + 1];
    int32_t inode = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)name, parent, volume, 0, 0);
    if (inode >= 0) {
        load_file(inode, name, volume);
        docs[active_tab].file_parent = (int32_t)parent;   // real: no siempre es la raiz
    }
}

static void save_to_inode(Document *d, int32_t inode) {
    if (d->recortado) {
        // al abrirlo se cortaron lineas: guardarlo destruiria el archivo
        set_status("No se guarda: al abrirlo no se cargó entero (se destruiría el archivo)");
        return;
    }
    static char content[MAX_LINES * (MAX_COLS + 1)];
    int pos = 0;
    for (int i = 0; i < d->line_count; i++) {
        int len = str_len(d->lines[i]);
        for (int j = 0; j < len; j++) content[pos++] = d->lines[i][j];
        content[pos++] = '\n';
    }
    syscall5(SYS_FILE_WRITE, (uint64_t)inode, (uint64_t)content, (uint64_t)pos, d->file_volume, 0);
}

static void do_open(void) {
    static char name_buf[28];
    // BUG REAL CORREGIDO: el dialogo navega libremente por subcarpetas
    // (se puede entrar en DOCUMENTOS y elegir un archivo alli), pero
    // antes solo se leia el inodo -- el documento quedaba con
    // file_parent=0 (raiz) SIEMPRE, aunque el archivo viviera en otro
    // sitio. Al compilar, nbc.pro buscaba en la raiz, no lo encontraba
    // y creaba uno nuevo VACIO alli ("copia el .bb con 0 bytes... no
    // crea el .pro"). Ahora se decodifica el valor de 64 bits completo
    // (carpeta real en los bits altos, inodo en los bajos).
    uint64_t r = syscall5(SYS_OPEN_FILE_DIALOG, 0, (uint64_t)name_buf, sizeof(name_buf), 0, 0);
    if (r == (uint64_t)-1) return;
    int32_t parent = (int32_t)(r >> 32);
    int32_t inode = (int32_t)(r & 0xFFFFFFFFu);
    new_tab();
    load_file(inode, name_buf, 0);   // el dialogo comun solo navega NemoFS
    docs[active_tab].file_parent = parent;
}

static void do_save_as(void) {
    Document *d = &docs[active_tab];
    static char name_buf[28];
    // Mismo formato empaquetado que do_open() -- ver la nota alli.
    uint64_t r = syscall5(SYS_SAVE_FILE_DIALOG, 0, (uint64_t)name_buf, sizeof(name_buf), 0, 0);
    if (r == (uint64_t)-1) return;
    int32_t parent = (int32_t)(r >> 32);
    int32_t inode = (int32_t)(r & 0xFFFFFFFFu);
    d->file_inode = inode;
    d->file_parent = parent;
    d->file_volume = 0;   // el dialogo comun guarda siempre en NemoFS
    int i = 0;
    while (name_buf[i] && i < 27) { d->file_name[i] = name_buf[i]; i++; }
    d->file_name[i] = '\0';
    save_to_inode(d, inode);
    set_status("Guardado");
}

static void do_save(void) {
    Document *d = &docs[active_tab];
    if (d->file_inode >= 0) { save_to_inode(d, d->file_inode); set_status("Guardado"); }
    else do_save_as();
}

// ---- compilar / compilar y ejecutar ----

// El BASIC propio de Nemo OS usa la extension .nb (Nemo Basic). La
// anterior, .bb, era la de Nemo-Blitz/BlitzPlus, que quedo atras con
// la reescritura del compilador.
static bool ends_with_nb(const char *name) {
    int len = str_len(name);
    if (len < 4) return false;
    return name[len - 3] == '.' && to_upper_ascii(name[len - 2]) == 'N' && to_upper_ascii(name[len - 1]) == 'B';
}

static bool ends_with_bit(const char *name) {
    int len = str_len(name);
    if (len < 4) return false;
    return name[len - 4] == '.' && to_upper_ascii(name[len - 3]) == 'B'
        && to_upper_ascii(name[len - 2]) == 'I' && to_upper_ascii(name[len - 1]) == 'T';
}

// -- tokenizador de resaltado para Bitacora --

static bool bit_is_keyword(const char *w) {
    // NUNCA una tabla estatica indexada de punteros a texto aqui --
    // ver LECCION IMPORTANTE en programas-nemo-os.md: los .pro son
    // binarios planos sin reubicacion al cargar. Cada cadena en su
    // PROPIA llamada.
    return str_eq(w, "bitacora") || str_eq(w, "fin_bitacora") || str_eq(w, "inicio")
        || str_eq(w, "fin") || str_eq(w, "var") || str_eq(w, "si") || str_eq(w, "entonces")
        || str_eq(w, "sino") || str_eq(w, "fin_si") || str_eq(w, "mientras")
        || str_eq(w, "fin_mientras") || str_eq(w, "escribir") || str_eq(w, "funcion")
        || str_eq(w, "fin_funcion") || str_eq(w, "retorna") || str_eq(w, "estructura")
        || str_eq(w, "fin_estructura") || str_eq(w, "externa") || str_eq(w, "syscall")
        || str_eq(w, "arreglo") || str_eq(w, "de") || str_eq(w, "o") || str_eq(w, "y")
        || str_eq(w, "no");
}
static bool bit_is_type(const char *w) {
    return str_eq(w, "entero") || str_eq(w, "texto") || str_eq(w, "booleano");
}
static bool bit_is_literal_word(const char *w) {
    return str_eq(w, "verdadero") || str_eq(w, "falso");
}
static bool bit_is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static bool bit_is_digit(char c) { return c >= '0' && c <= '9'; }

// Dibuja una linea de codigo Bitacora coloreada token a token (todo
// en tonos de verde -- ver macros COLOR_BIT_*). Fuente monoespaciada
// de 6px de avance, igual que el resto del editor (ver COLOR_SELECTION
// y el cursor, que ya usan ese mismo 6 a mano).
static void draw_line_bitacora(int x, int y, const char *line) {
    char tok[MAX_COLS + 1];
    int len = str_len(line);
    int i = 0, col = 0;
    while (i < len) {
        char c = line[i];

        if (c == '/' && i + 1 < len && line[i + 1] == '/') {
            draw_text(x + col * 6, y, &line[i], COLOR_BIT_COMMENT);
            return;
        }

        if (c == '"') {
            int j = i + 1;
            while (j < len && line[j] != '"') j++;
            if (j < len) j++; // incluir comilla de cierre
            int n = j - i;
            for (int k = 0; k < n; k++) tok[k] = line[i + k];
            tok[n] = '\0';
            draw_text(x + col * 6, y, tok, COLOR_BIT_STRING);
            col += n; i = j;
            continue;
        }

        if (bit_is_digit(c)) {
            int j = i;
            while (j < len && bit_is_digit(line[j])) j++;
            int n = j - i;
            for (int k = 0; k < n; k++) tok[k] = line[i + k];
            tok[n] = '\0';
            draw_text(x + col * 6, y, tok, COLOR_BIT_NUMBER);
            col += n; i = j;
            continue;
        }

        if (bit_is_ident_char(c) && !bit_is_digit(c)) {
            int j = i;
            while (j < len && bit_is_ident_char(line[j])) j++;
            int n = j - i;
            for (int k = 0; k < n; k++) tok[k] = line[i + k];
            tok[n] = '\0';
            uint32_t tc = COLOR_BIT_DEFAULT;
            if (bit_is_keyword(tok)) tc = COLOR_BIT_KEYWORD;
            else if (bit_is_type(tok)) tc = COLOR_BIT_TYPE;
            else if (bit_is_literal_word(tok)) tc = COLOR_BIT_NUMBER;
            draw_text(x + col * 6, y, tok, tc);
            col += n; i = j;
            continue;
        }

        // operadores, puntuacion, espacios: caracter suelto en el
        // tono base -- sin tabla de operadores compuestos (==, <=...),
        // se leen igual de bien caracter a caracter.
        tok[0] = c; tok[1] = '\0';
        draw_text(x + col * 6, y, tok, COLOR_BIT_DEFAULT);
        col += 1; i += 1;
    }
}

// nbc.pro ya sabe abrir un archivo en CUALQUIER carpeta (recibe
// "padre:nombre" como argumento de lanzamiento) -- ya no hace falta
// duplicar el archivo en la raiz solo para que lo encuentre. Guardamos
// el contenido actual en su sitio real (si el documento tiene inodo)
// para que nbc.pro compile siempre lo ultimo editado.
static void guardar_antes_de_compilar(Document *d) {
    if (d->file_inode >= 0) save_to_inode(d, (uint32_t)d->file_inode);
}

// Construye el argumento de lanzamiento de nbc.pro: "padre:nombre".
// Mismo formato que ya usa el explorador para abrir archivos con
// nosotros -- nbc_main.c lo entiende (y sigue aceptando un nombre a
// secas, sin prefijo, para "run nbc.pro archivo.bb" desde la shell).
static void construir_arg_nbc(const Document *d, char *out, int max_len) {
    char num[12];
    int n = 0, val = d->file_parent;
    if (val == 0) { num[n++] = '0'; }
    else { char tmp[12]; int t = 0; while (val > 0) { tmp[t++] = (char)('0' + val % 10); val /= 10; }
           while (t > 0) num[n++] = tmp[--t]; }
    num[n] = '\0';
    int i = 0, j = 0;
    while (num[j] && i < max_len - 1) out[i++] = num[j++];
    if (i < max_len - 1) out[i++] = ':';
    j = 0;
    while (d->file_name[j] && i < max_len - 1) out[i++] = d->file_name[j++];
    out[i] = '\0';
}

// BUG REAL CORREGIDO: esto asumia que la extension del archivo de
// origen medía SIEMPRE 2 caracteres ("len-3": punto + 2), y quitaba
// justo esos 3. nbc.pro (el que de verdad crea el .pro) corta desde
// el ULTIMO PUNTO, sea cual sea lo que venga despues -- para un
// origen como "WINDOWMENU.BBST" (extension de 4 caracteres) las dos
// formulas daban nombres DISTINTOS ("WINDOWMENU.BB.pro" aqui,
// "WINDOWMENU.pro" el real), asi que "Compilar y Ejecutar" intentaba
// lanzar un archivo que no existia. Ahora usa la misma regla que
// nbc.pro: cortar desde el ultimo punto.
static void make_pro_name(const char *input, char *output, int max_len) {
    int len = str_len(input);
    int base_len = len;
    for (int k = len; k > 0; k--) {
        if (input[k - 1] == '.') { base_len = k - 1; break; }
    }
    int i = 0;
    for (; i < base_len && i < max_len - 5; i++) output[i] = input[i];
    const char *suf = ".pro";
    int j = 0;
    while (suf[j] && i < max_len - 1) output[i++] = suf[j++];
    output[i] = '\0';
}

// Detecta si el codigo fuente usa graficos o gadgets -- si es asi, el
// propio kernel le crea una ventana real en cuanto el programa llama
// a la primera de esas funciones (task_ensure_window), asi que no
// hace falta ninguna shell de por medio: se lanza directamente, igual
// que un icono de escritorio. Si NO las usa (solo Print), seguimos
// necesitando una shell para que su salida tenga donde mostrarse.
// ¿Es un caracter que puede formar parte de un nombre? Se usa para
// exigir que la palabra buscada aparezca SUELTA, no dentro de otra.
static bool es_caracter_de_nombre(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '#';
}

// Busca una palabra COMPLETA, sin distinguir mayusculas.
//
// Hace falta la version con limites de palabra, y no un
// str_contains_ci a secas: si "Color" valiera dentro de cualquier
// texto, un programa de consola con una variable llamada "color" se
// tomaria por programa grafico, y su salida acabaria en el puerto
// serie en vez de en una shell -- es decir, el usuario no veria nada.
// El error importa en esa direccion, asi que mejor ser estrictos.
static bool contiene_palabra_ci(const char *hay, const char *palabra) {
    int hl = str_len(hay), nl = str_len(palabra);
    if (nl == 0) return true;
    for (int i = 0; i + nl <= hl; i++) {
        bool ok = true;
        for (int j = 0; j < nl; j++) {
            if (to_upper_ascii(hay[i + j]) != to_upper_ascii(palabra[j])) { ok = false; break; }
        }
        if (!ok) continue;
        if (i > 0 && es_caracter_de_nombre(hay[i - 1])) continue;          // pegada por la izquierda
        if (i + nl < hl && es_caracter_de_nombre(hay[i + nl])) continue;   // pegada por la derecha
        return true;
    }
    return false;
}

// ¿El programa necesita su propia ventana, o le basta con una shell?
//
// Un programa que dibuja o que lee teclado/raton necesita ventana: el
// kernel se la crea en cuanto llama a cualquiera de esas cosas. Uno
// que solo hace Print se abre dentro de una shell, que es donde su
// salida tiene sentido.
static bool source_uses_graphics(Document *d) {
    // OJO con la forma de esta tabla: es un array de CADENAS
    // (char[N][16]), no de PUNTEROS a cadena (const char *[N]).
    //
    // Con punteros, el enlazador escribe aqui direcciones ABSOLUTAS,
    // calculadas como si el programa se cargara en la direccion 0 --
    // pero cada tarea de Nemo OS carga en la suya, asi que al
    // ejecutar apuntarian a cualquier sitio. Un array de cadenas
    // guarda el texto DENTRO de la propia tabla: no hay ninguna
    // direccion que corregir.
    //
    // (Esto reventaba de verdad: Data Abort con FAR_EL1=0x3a70, una
    // direccion pequeña que no cae dentro del area de la tarea --
    // señal inconfundible de un desplazamiento usado como direccion.)
    // 20, no 16: el nombre mas largo de la lista ("CreateTextField")
    // mide 15 mas el terminador, o sea que en 16 entraria JUSTO. Si
    // alguien añade uno mas largo, C lo recorta sin avisar y la
    // palabra deja de reconocerse. Con 20 hay margen.
    static const char palabras[][20] = {
        // graficos del BASIC propio
        "Graphics", "Cls", "Plot", "Line", "Rect", "Oval", "Text", "Color",
        // entrada: tambien necesita una ventana con foco
        "KeyDown", "KeyHit", "GetKey", "MouseX", "MouseY", "MouseDown",
        // gadgets (ventanas con controles)
        "CreateWindow", "CreateButton", "CreatePanel", "CreateTextField",
        "CreateListBox", "CreateTextArea", "CreateMenu", "WindowMenu",
    };
    const int n = (int)(sizeof(palabras) / sizeof(palabras[0]));
    for (int i = 0; i < d->line_count; i++) {
        const char *l = d->lines[i];
        // Saltar los comentarios: una linea que empieza por ';' o por
        // una comilla no es codigo, y mencionar "Rect" ahi no convierte
        // el programa en grafico.
        int k = 0;
        while (l[k] == ' ' || l[k] == '\t') k++;
        if (l[k] == ';' || l[k] == '\'') continue;
        for (int w = 0; w < n; w++) {
            if (contiene_palabra_ci(l, palabras[w])) return true;
        }
    }
    return false;
}

static void build_log_push(const char *line);   // definida mas abajo

static void start_compile(bool also_run) {
    Document *d = &docs[active_tab];
    if (!ends_with_nb(d->file_name)) {
        set_status("El archivo debe llamarse *.NB");
        return;
    }
    set_status(also_run ? "Compilando y ejecutando..." : "Compilando...");
    guardar_antes_de_compilar(d);

    build_log_count = 0;
    build_line_len = 0;
    build_line_buf[0] = '\0';
    build_in_progress = true;
    build_auto_run = also_run;
    build_launched = false;
    build_is_window_app = source_uses_graphics(d);
    { int i = 0; while (d->file_name[i] && i < 31) { build_target_name[i] = d->file_name[i]; i++; } build_target_name[i] = '\0'; }
    build_target_parent = (uint32_t)d->file_parent;
    build_tab = active_tab;
    dialog_mode = DLG_BUILD;

    // 0xFFFFFFFF = sin preferencia de carpeta para BUSCAR "nbc.pro" (se
    // busca en la raiz y luego en PROGRAMAS, como hace 'run' en la
    // shell) -- no confundir con la carpeta del ARCHIVO A COMPILAR, que
    // va dentro del argumento de lanzamiento ("padre:nombre").
    static char arg_nbc[40];
    construir_arg_nbc(d, arg_nbc, sizeof(arg_nbc));
    syscall5(SYS_LAUNCH_PROGRAM, (uint64_t)"nbc.pro", (uint64_t)arg_nbc, 0xFFFFFFFF, 0, 0);
}

// ---- Ir a la linea del error ----
// Un error del compilador ("nbc: error en la linea 12: ...", o "error en
// utiles.nb, linea 5: ...") o del programa al ejecutarse ("Error en tiempo de
// ejecucion, linea 23: ..." o "... linea 2 de calculos.nb: ...") lleva el
// cursor a esa linea, en la pestaña de ese archivo si esta abierta.
static bool nombre_igual_ci(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i]) { if (to_upper_ascii(a[i]) != to_upper_ascii(b[i])) return false; i++; }
    return a[i] == b[i];
}
static void ir_a_linea_de_error(const char *l) {
    // "linea N": N, y el archivo si lo dice ("de X.nb" detras, o "en X.nb," delante)
    int i = 0, n = -1;
    char archivo[32]; archivo[0] = '\0';
    for (; l[i]; i++) {
        if ((l[i] == 'l' || l[i] == 'L') && l[i+1] == 'i' && l[i+2] == 'n' && l[i+3] == 'e' && l[i+4] == 'a' && l[i+5] == ' ' && l[i+6] >= '0' && l[i+6] <= '9') {
            n = 0; i += 6;
            while (l[i] >= '0' && l[i] <= '9') { n = n * 10 + (l[i] - '0'); i++; }
            break;
        }
    }
    if (n <= 0) return;
    if (l[i] == ' ' && l[i+1] == 'd' && l[i+2] == 'e' && l[i+3] == ' ') {          // "... linea 2 de calculos.nb:"
        int k = 0; i += 4;
        while (l[i] && l[i] != ':' && k < 31) archivo[k++] = l[i++];
        archivo[k] = '\0';
    } else {                                                                       // "error en utiles.nb, linea 5:"
        const char *p = l;
        // el nombre sigue a "error en " (el primero: el mensaje puede tener mas "en ")
        for (int j = 0; l[j]; j++)
            if (l[j] == 'r' && l[j+1] == 'o' && l[j+2] == 'r' && l[j+3] == ' ' && l[j+4] == 'e' && l[j+5] == 'n' && l[j+6] == ' ') { p = &l[j + 7]; break; }
        int k = 0;
        while (p[k] && p[k] != ',' && p[k] != ' ' && k < 31) { archivo[k] = p[k]; k++; }
        archivo[k] = '\0';
        if (!(k > 3 && archivo[k-3] == '.' && (archivo[k-2] == 'n' || archivo[k-2] == 'N'))) archivo[0] = '\0';
    }
    int tab = build_tab;
    if (archivo[0]) {
        tab = -1;
        for (int t = 0; t < tab_count; t++) if (nombre_igual_ci(docs[t].file_name, archivo)) tab = t;
    }
    if (tab < 0 || tab >= tab_count) return;
    active_tab = tab;
    Document *d = &docs[tab];
    d->cursor_row = (n - 1 < d->line_count) ? n - 1 : d->line_count - 1;
    if (d->cursor_row < 0) d->cursor_row = 0;
    d->cursor_col = 0;
    d->has_selection = false;
}

static void build_log_push(const char *line) {
    {   // un error: a su linea
        bool es_error = false;
        for (int j = 0; line[j]; j++) if ((line[j] == 'e' || line[j] == 'E') && line[j+1] == 'r' && line[j+2] == 'r' && line[j+3] == 'o' && line[j+4] == 'r') es_error = true;
        if (es_error) ir_a_linea_de_error(line);
    }
    if (build_log_count < BUILD_LOG_LINES) {
        int i = 0;
        while (line[i] && i < BUILD_LOG_COLS) { build_log[build_log_count][i] = line[i]; i++; }
        build_log[build_log_count][i] = '\0';
        build_log_count++;
    } else {
        for (int i = 0; i < BUILD_LOG_LINES - 1; i++) {
            int j = 0;
            while (build_log[i + 1][j]) { build_log[i][j] = build_log[i + 1][j]; j++; }
            build_log[i][j] = '\0';
        }
        int i = 0;
        while (line[i] && i < BUILD_LOG_COLS) { build_log[BUILD_LOG_LINES - 1][i] = line[i]; i++; }
        build_log[BUILD_LOG_LINES - 1][i] = '\0';
    }
}

// Se llama en cada vuelta del bucle principal mientras haya una
// compilacion (o el programa que lanzamos despues) en marcha --
// nbc.pro (y luego el propio programa compilado, si lo lanzamos) se
// ejecutan con esta ventana como "padre", asi que su salida nos
// llega por esta misma cola que ya usa la shell para los programas
// que lanza con 'run'. Seguimos escuchando incluso DESPUES de que
// nbc.pro termine, para no perder ni una linea de lo que imprima el
// programa recien compilado (los que no tienen ventana propia,
// como uno que solo hace Print, no tendrian donde mas mostrarla).
static void poll_build_output(void) {
    if (!build_in_progress && !build_launched) return;
    char c;
    while ((c = (char)syscall5(SYS_READ_CONSOLE_OUTPUT, 0, 0, 0, 0, 0)) != 0) {
        if (c == '\n') {
            build_log_push(build_line_buf);
            if (build_in_progress && str_contains_ci(build_line_buf, "listo ->")) {
                build_in_progress = false;
                if (build_auto_run) {
                    char pro_name[32];
                    make_pro_name(build_target_name, pro_name, sizeof(pro_name));
                    if (build_is_window_app) {
                        // Programa grafico/con gadgets -- el kernel le
                        // crea su propia ventana real en cuanto llama a
                        // la primera funcion de ese tipo, asi que lo
                        // lanzamos directo, sin shell de por medio.
                        // La carpeta va explicita: nbc.pro deja el .pro
                        // junto al fuente, asi que buscarlo solo en la
                        // raiz no vale.
                        syscall5(SYS_LAUNCH_PROGRAM, (uint64_t)pro_name, 0, (uint64_t)build_target_parent, 0, 0);
                        dialog_mode = DLG_NONE;
                        set_status("Ejecutando...");
                    } else {
                        // Programa de solo consola -- lo abrimos dentro
                        // de una SHELL nueva, con "run <programa>" ya
                        // preparado, para que su salida (si la tiene)
                        // tenga un sitio natural donde mostrarse.
                        // "@<carpeta> run programa.pro": la shell
                        // empieza en la carpeta del programa (antes, en la raiz,
                        // y un programa de consola de otra carpeta no se encontraba)
                        char shell_cmd[64];
                        int si = 0;
                        shell_cmd[si++] = '@';
                        { char num[12]; int nn = 0; uint32_t v = build_target_parent;
                          if (v == 0) num[nn++] = '0';
                          while (v > 0) { num[nn++] = (char)('0' + v % 10); v /= 10; }
                          while (nn > 0) shell_cmd[si++] = num[--nn]; }
                        const char *prefix = " run ";
                        for (int q = 0; prefix[q]; q++) shell_cmd[si++] = prefix[q];
                        int pi = 0;
                        while (pro_name[pi] && si < 63) { shell_cmd[si++] = pro_name[pi++]; }
                        shell_cmd[si] = '\0';
                        // LIMITE CONOCIDO: la shell nueva arranca en la
                        // raiz, no en la carpeta del programa (el tercer
                        // argumento solo dice donde BUSCAR shell.pro, no
                        // en que carpeta se situa). Asi que un programa
                        // de consola guardado fuera de la raiz compila
                        // bien pero la shell no lo encontrara al
                        // ejecutarlo. Los graficos si funcionan en
                        // cualquier carpeta, porque se lanzan directos.
                        syscall5(SYS_LAUNCH_PROGRAM, (uint64_t)"shell.pro", (uint64_t)shell_cmd, 0xFFFFFFFF, 0, 0);
                        dialog_mode = DLG_NONE;
                        set_status("Ejecutando en una shell nueva");
                    }
                }
            } else if (build_in_progress && str_contains_ci(build_line_buf, "error")) {
                build_in_progress = false;
            }
            build_line_buf[0] = '\0';
            build_line_len = 0;
        } else if (build_line_len < BUILD_LOG_COLS) {
            build_line_buf[build_line_len++] = c;
            build_line_buf[build_line_len] = '\0';
        }
    }
}

// ---- dibujo ----

static void redraw(void) {
    Document *d = &docs[active_tab];
    draw_rect(0, 0, win_w, win_h, COLOR_BG);
    draw_menubar();
    draw_tabbar();

    int text_top = TEXT_TOP;
    bool es_bitacora = ends_with_bit(d->file_name);
    asegurar_cursor_visible(d);
    int visibles = lineas_visibles();
    for (int i = d->scroll_top; i < d->line_count && i < d->scroll_top + visibles; i++) {
        int y = text_top + (i - d->scroll_top) * LINE_H;
        if (d->has_selection && i == d->sel_row) {
            int a = d->sel_start_col < d->sel_end_col ? d->sel_start_col : d->sel_end_col;
            int b = d->sel_start_col < d->sel_end_col ? d->sel_end_col : d->sel_start_col;
            a -= d->scroll_col; b -= d->scroll_col;                 // desplazamiento horizontal
            if (a < 0) a = 0;
            if (b > a) draw_rect(4 + a * 6, y, (b - a) * 6, 10, COLOR_SELECTION);
        }
        // desde la primera columna visible
        const char *vis = (str_len(d->lines[i]) > d->scroll_col) ? d->lines[i] + d->scroll_col : "";
        if (es_bitacora) draw_line_bitacora(4, y, vis);
        else draw_text(4, y, vis, COLOR_TEXT);
    }

    // El cursor solo se dibuja si su linea esta a la vista.
    if (d->cursor_row >= d->scroll_top && d->cursor_row < d->scroll_top + visibles) {
        int cx = 4 + (d->cursor_col - d->scroll_col) * 6;
        int cy = text_top + (d->cursor_row - d->scroll_top) * LINE_H;
        draw_rect(cx, cy, 2, 10, COLOR_CURSOR);
    }

    int status_y = win_h - 12;
    draw_rect(0, status_y - 2, win_w, 14, COLOR_STATUSBAR);
    draw_text(4, status_y, d->file_name, COLOR_STATUS_TEXT);
    if (status_msg[0] != '\0') draw_text(win_w - 160, status_y, status_msg, COLOR_STATUS_TEXT);

    if (open_menu == MENU_FILE) draw_dropdown(MENU_FILE, FILE_ITEMS, MENU_X_FILE);
    else if (open_menu == MENU_EDIT) draw_dropdown(MENU_EDIT, EDIT_ITEMS, MENU_X_EDIT);
    else if (open_menu == MENU_RUN) draw_dropdown(MENU_RUN, RUN_ITEMS, MENU_X_RUN);
    else if (open_menu == MENU_HELP) draw_dropdown(MENU_HELP, HELP_ITEMS, MENU_X_HELP);

    if (dialog_mode == DLG_ABOUT) {
        int dw = win_w > 240 ? 240 : win_w - 20;
        int dh = 110;
        int dx = (win_w - dw) / 2;
        int dy = (win_h - dh) / 2;
        draw_rect(dx, dy, dw, dh, COLOR_MENU_BG);
        draw_rect(dx, dy, dw, 1, 0x00000000);
        draw_text(dx + 8, dy + 8, "Nemo OS - IDE", COLOR_MENUBAR_TEXT);
        draw_text(dx + 8, dy + 24, "Pestañas + compilar/ejecutar", COLOR_MENUBAR_TEXT);
        draw_text(dx + 8, dy + 38, "integrado (nbc.pro)", COLOR_MENUBAR_TEXT);
        draw_text(dx + 8, dy + dh - 16, "Enter para cerrar", COLOR_STATUS_TEXT);
    } else if (dialog_mode == DLG_BUILD) {
        int dw = win_w - 20;
        int dh = 180;
        int dx = 10;
        int dy = (win_h - dh) / 2;
        draw_rect(dx, dy, dw, dh, COLOR_BUILD_BG);
        draw_rect(dx, dy, dw, 1, 0x00000000);
        draw_text(dx + 6, dy + 4,
                  build_in_progress ? "Compilando..." : (build_launched ? "Ejecutando (Enter para cerrar)" : "Compilación terminada"),
                  COLOR_TEXT);
        for (int i = 0; i < build_log_count; i++) {
            bool is_err = str_contains_ci(build_log[i], "error");
            draw_text(dx + 6, dy + 18 + i * 12, build_log[i], is_err ? COLOR_BUILD_ERR : COLOR_BUILD_TEXT);
        }
        if (!build_in_progress) draw_text(dx + 6, dy + dh - 14, "Enter para cerrar", COLOR_STATUS_TEXT);
    }
}

// ---- entrada ----

static void handle_tabbar_click(int mx) {
    int idx = mx / TAB_W;
    if (idx < 0 || idx >= tab_count) return;
    int local_x = mx - idx * TAB_W;
    if (local_x >= TAB_W - 16) close_tab(idx);
    else active_tab = idx;
}

static void handle_menu_click(int mx, int my) {
    if (open_menu == MENU_NONE && my >= MENUBAR_H && my < MENUBAR_H + TAB_BAR_H) {
        handle_tabbar_click(mx);
        return;
    }
    if (my < MENUBAR_H) {
        if (mx < MENU_X_EDIT) open_menu = (open_menu == MENU_FILE) ? MENU_NONE : MENU_FILE;
        else if (mx < MENU_X_RUN) open_menu = (open_menu == MENU_EDIT) ? MENU_NONE : MENU_EDIT;
        else if (mx < MENU_X_HELP) open_menu = (open_menu == MENU_RUN) ? MENU_NONE : MENU_RUN;
        else open_menu = (open_menu == MENU_HELP) ? MENU_NONE : MENU_HELP;
        return;
    }

    if (open_menu == MENU_NONE) return;

    int x0 = (open_menu == MENU_FILE) ? MENU_X_FILE : (open_menu == MENU_EDIT) ? MENU_X_EDIT
             : (open_menu == MENU_RUN) ? MENU_X_RUN : MENU_X_HELP;
    int count = (open_menu == MENU_FILE) ? FILE_ITEMS : (open_menu == MENU_EDIT) ? EDIT_ITEMS
                : (open_menu == MENU_RUN) ? RUN_ITEMS : HELP_ITEMS;
    int w = 150;
    int h = count * MENU_ROW_H + 4;

    if (mx < x0 || mx >= x0 + w || my < MENUBAR_H || my >= MENUBAR_H + h) { open_menu = MENU_NONE; return; }

    int item = (my - MENUBAR_H - 2) / MENU_ROW_H;
    if (item < 0 || item >= count) { open_menu = MENU_NONE; return; }

    open_menu = MENU_NONE;

    if (x0 == MENU_X_FILE) {
        if (item == 0) clear_doc(&docs[active_tab]);
        else if (item == 1) new_tab();
        else if (item == 2) do_open();
        else if (item == 3) do_save();
        else if (item == 4) do_save_as();
        else if (item == 5) close_tab(active_tab);
        else if (item == 6) running = false;
    } else if (x0 == MENU_X_EDIT) {
        if (item == 0) { copy_selection_to_clipboard(); delete_selection(); }
        else if (item == 1) copy_selection_to_clipboard();
        else if (item == 2) paste_from_clipboard();
        else if (item == 3) select_all();
    } else if (x0 == MENU_X_RUN) {
        if (item == 0) start_compile(false);
        else if (item == 1) start_compile(true);
    } else {
        if (item == 0) dialog_mode = DLG_ABOUT;
    }
}

static void handle_dialog_key(char c) {
    if (c != '\n') return;
    if (dialog_mode == DLG_ABOUT) dialog_mode = DLG_NONE;
    else if (dialog_mode == DLG_BUILD && !build_in_progress) {
        dialog_mode = DLG_NONE;
        build_launched = false;
    }
}

__attribute__((section(".text.start")))
void _start(void) {
    cargar_fuente_mono();
    tab_count = 1;
    active_tab = 0;
    clear_doc(&docs[0]);
    for (int i = 1; i < MAX_TABS; i++) docs[i].used = false;

    running = true;
    status_msg[0] = '\0';
    open_menu = MENU_NONE;
    dialog_mode = DLG_NONE;
    build_in_progress = false;
    build_launched = false;
    build_is_window_app = false;
    documentos_dir = 0;

    find_documentos();
    try_open_from_launch_arg();

    uint64_t size = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
    win_w = (int)(size >> 32);
    win_h = (int)(size & 0xFFFFFFFF);
    if (win_w <= 0) win_w = 480;
    if (win_h <= 0) win_h = 320;

    redraw();

    bool last_left = false;

    while (running) {
        uint64_t pump = syscall5(SYS_PUMP, 0, 0, 0, 0, 0);
        if ((int64_t)pump < 0) break;

        bool need_redraw = false;

        poll_build_output();
        if (dialog_mode == DLG_BUILD) need_redraw = true;

        uint64_t sz = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
        int nw = (int)(sz >> 32), nh = (int)(sz & 0xFFFFFFFF);
        if ((nw > 0 && nw != win_w) || (nh > 0 && nh != win_h)) need_redraw = true;
        if (nw > 0) win_w = nw;
        if (nh > 0) win_h = nh;

        // Rueda del raton: positivo = hacia arriba. Se consulta una vez
        // por vuelta porque el sistema devuelve el ACUMULADO desde la
        // ultima consulta y lo pone a cero al leerlo. Solo mueve el
        // texto cuando no hay ningun dialogo abierto delante.
        if (dialog_mode == DLG_NONE) {
            int64_t rueda = (int64_t)syscall5(SYS_GET_MOUSE_WHEEL, 0, 0, 0, 0, 0);
            if (rueda != 0) {
                Document *dv = &docs[active_tab];
                dv->scroll_top -= (int)rueda * 3;
                int max_top = dv->line_count - lineas_visibles();
                if (max_top < 0) max_top = 0;
                if (dv->scroll_top > max_top) dv->scroll_top = max_top;
                if (dv->scroll_top < 0) dv->scroll_top = 0;
                need_redraw = true;
            }
        }

        uint64_t m = syscall5(SYS_GET_MOUSE, 0, 0, 0, 0, 0);
        if (m != (uint64_t)-1) {
            int mx = (int)((m >> 32) & 0xFFFF);
            int my = (int)((m >> 16) & 0xFFFF);
            bool left = (m & 1) != 0;
            if (left && !last_left && dialog_mode == DLG_NONE) {
                handle_menu_click(mx, my);
                need_redraw = true;
            }
            last_left = left;
        } else {
            last_left = false;
        }

        char c;
        while ((c = (char)syscall5(SYS_READ_CHAR, 0, 0, 0, 0, 0)) != 0) {
            if (dialog_mode != DLG_NONE) {
                handle_dialog_key(c);
            } else if (open_menu != MENU_NONE) {
                open_menu = MENU_NONE;
            } else {
                status_msg[0] = '\0';
                if (c == '\n') split_line();
                else if (c == '\b') backspace();
                else if (c == CH_UP || c == CH_DOWN || c == CH_LEFT || c == CH_RIGHT) move_cursor(c);
                else if (c >= 32 && c < 127) insert_char(c);
            }
            need_redraw = true;
        }

        if (need_redraw) redraw();
    }
}
