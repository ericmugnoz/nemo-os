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
static void draw_text(int x, int y, const char *s, uint32_t color) {
    syscall5(SYS_DRAW_TEXT, (uint64_t)x, (uint64_t)y, (uint64_t)s, color, 0);
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
#define MAX_COLS  80
#define MAX_TABS  6

typedef struct {
    bool used;
    char lines[MAX_LINES][MAX_COLS + 1];
    int line_count;
    int cursor_row, cursor_col;
    int32_t file_inode; // -1 = documento nuevo, sin guardar aun
    char file_name[28];
    bool has_selection;
    int sel_row, sel_start_col, sel_end_col;
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

#define MENUBAR_H 16
#define TAB_BAR_H 16
#define MENU_ROW_H 16
#define LINE_H 12
#define TEXT_TOP (MENUBAR_H + TAB_BAR_H + 4)
#define TAB_W 90

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
        if (index == 0) return "NUEVO";
        if (index == 1) return "NUEVA PESTANA";
        if (index == 2) return "ABRIR";
        if (index == 3) return "GUARDAR";
        if (index == 4) return "GUARDAR COMO";
        if (index == 5) return "CERRAR PESTANA";
        if (index == 6) return "SALIR";
    } else if (menu == MENU_EDIT) {
        if (index == 0) return "CORTAR";
        if (index == 1) return "COPIAR";
        if (index == 2) return "PEGAR";
        if (index == 3) return "SELECCIONAR TODO";
    } else if (menu == MENU_RUN) {
        if (index == 0) return "COMPILAR";
        if (index == 1) return "COMPILAR Y EJECUTAR";
    } else if (menu == MENU_HELP) {
        if (index == 0) return "ACERCA DE";
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
static bool build_is_window_app; // true si el codigo fuente usa graficos/gadgets -- no hace falta shell
static char build_target_name[32]; // el .bb que se esta compilando

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
    d->cursor_col = 0;
    d->has_selection = false;
    d->file_inode = -1;
    const char *untitled = "SIN TITULO";
    int i = 0;
    while (untitled[i]) { d->file_name[i] = untitled[i]; i++; }
    d->file_name[i] = '\0';
    d->used = true;
}

static void new_tab(void) {
    if (tab_count >= MAX_TABS) { set_status("MAXIMO DE PESTANAS"); return; }
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
    if (tab_count <= 1) { set_status("ES LA UNICA PESTANA"); return; }
    for (int i = idx; i < tab_count - 1; i++) copy_document(&docs[i], &docs[i + 1]);
    tab_count--;
    if (active_tab >= tab_count) active_tab = tab_count - 1;
}

// ---- menu desplegable: dibujo y deteccion de clics ----

static void draw_menubar(void) {
    draw_rect(0, 0, win_w, MENUBAR_H, COLOR_MENUBAR);
    draw_text(6, 4, "ARCHIVO", COLOR_MENUBAR_TEXT);
    draw_text(70, 4, "EDICION", COLOR_MENUBAR_TEXT);
    draw_text(134, 4, "EJECUTAR", COLOR_MENUBAR_TEXT);
    draw_text(206, 4, "AYUDA", COLOR_MENUBAR_TEXT);
}

static void draw_tabbar(void) {
    draw_rect(0, MENUBAR_H, win_w, TAB_BAR_H, COLOR_TAB_BAR);
    int x = 2;
    for (int i = 0; i < tab_count; i++) {
        bool active = (i == active_tab);
        draw_rect(x, MENUBAR_H, TAB_W - 2, TAB_BAR_H, active ? COLOR_TAB_ACTIVE : COLOR_TAB_INACTIVE);
        char label[12];
        int j = 0;
        while (docs[i].file_name[j] && j < 9) { label[j] = docs[i].file_name[j]; j++; }
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
    set_status("COPIADO");
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
    set_status("PEGADO");
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

static void load_file(int32_t inode, const char *name) {
    Document *d = &docs[active_tab];
    static char content[MAX_LINES * (MAX_COLS + 1)];
    int64_t bytes = (int64_t)syscall5(SYS_FILE_READ, (uint64_t)inode, (uint64_t)content, sizeof(content) - 1, 0, 0);
    if (bytes < 0) bytes = 0;

    clear_doc(d);
    d->file_inode = inode;
    int i = 0, ni = 0;
    while (name[i] && ni < 27) { d->file_name[ni] = name[i]; i++; ni++; }
    d->file_name[ni] = '\0';

    int row = 0, col = 0;
    int64_t p = 0;
    for (; p < bytes && row < MAX_LINES; p++) {
        char c = content[p];
        if (c == '\n') { d->lines[row][col] = '\0'; row++; col = 0; }
        else if (col < MAX_COLS) { d->lines[row][col] = c; col++; }
    }
    d->lines[row][col] = '\0';
    // AVISO REAL, NO SILENCIOSO -- ver la nota identica en editor.c.
    bool truncated = (p < bytes) || (bytes == (int64_t)(sizeof(content) - 1));
    d->line_count = row + 1;
    d->cursor_row = 0;
    d->cursor_col = 0;
    set_status(truncated ? "ABIERTO (TRUNCADO -- ARCHIVO DEMASIADO GRANDE)" : "ABIERTO");
}

// El argumento de lanzamiento (si lo hay) viene en formato
// "inodo_padre:nombre" -- lo construye el explorador cuando pide
// abrir un archivo con nosotros via SYS_LAUNCH_PROGRAM.
static void try_open_from_launch_arg(void) {
    static char arg[32];
    uint64_t len = syscall5(SYS_GET_LAUNCH_ARG, (uint64_t)arg, sizeof(arg), 0, 0, 0);
    if (len == 0) return;
    int i = 0;
    uint32_t parent = 0;
    while (arg[i] >= '0' && arg[i] <= '9') { parent = parent * 10 + (uint32_t)(arg[i] - '0'); i++; }
    if (arg[i] != ':') return;
    const char *name = &arg[i + 1];
    int32_t inode = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)name, parent, 0, 0, 0);
    if (inode >= 0) load_file(inode, name);
}

static void save_to_inode(Document *d, int32_t inode) {
    static char content[MAX_LINES * (MAX_COLS + 1)];
    int pos = 0;
    for (int i = 0; i < d->line_count; i++) {
        int len = str_len(d->lines[i]);
        for (int j = 0; j < len; j++) content[pos++] = d->lines[i][j];
        content[pos++] = '\n';
    }
    syscall5(SYS_FILE_WRITE, (uint64_t)inode, (uint64_t)content, (uint64_t)pos, 0, 0);
}

static void do_open(void) {
    static char name_buf[28];
    int32_t inode = (int32_t)syscall5(SYS_OPEN_FILE_DIALOG, 0, (uint64_t)name_buf, sizeof(name_buf), 0, 0);
    if (inode < 0) return;
    new_tab();
    load_file(inode, name_buf);
}

static void do_save_as(void) {
    Document *d = &docs[active_tab];
    static char name_buf[28];
    int32_t inode = (int32_t)syscall5(SYS_SAVE_FILE_DIALOG, 0, (uint64_t)name_buf, sizeof(name_buf), 0, 0);
    if (inode < 0) return;
    d->file_inode = inode;
    int i = 0;
    while (name_buf[i] && i < 27) { d->file_name[i] = name_buf[i]; i++; }
    d->file_name[i] = '\0';
    save_to_inode(d, inode);
    set_status("GUARDADO");
}

static void do_save(void) {
    Document *d = &docs[active_tab];
    if (d->file_inode >= 0) { save_to_inode(d, d->file_inode); set_status("GUARDADO"); }
    else do_save_as();
}

// ---- compilar / compilar y ejecutar ----

static bool ends_with_bb(const char *name) {
    int len = str_len(name);
    if (len < 4) return false;
    return name[len - 3] == '.' && to_upper_ascii(name[len - 2]) == 'B' && to_upper_ascii(name[len - 1]) == 'B';
}

// nbc.pro busca en la raiz de NemoFS -- guardamos ahi una copia del
// contenido actual con el nombre actual, independientemente de donde
// viva "oficialmente" el archivo (DOCUMENTOS u otro sitio).
static void save_current_to_root(Document *d) {
    int32_t inode = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)d->file_name, 0, 0, 0, 0);
    if (inode < 0) return;
    save_to_inode(d, (uint32_t)inode);
}

static void make_pro_name(const char *input, char *output, int max_len) {
    int len = str_len(input);
    int base_len = (len >= 3) ? len - 3 : len;
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
static bool source_uses_graphics(Document *d) {
    for (int i = 0; i < d->line_count; i++) {
        const char *l = d->lines[i];
        if (str_contains_ci(l, "CreateWindow")) return true;
        if (str_contains_ci(l, "Cls")) return true;
        if (str_contains_ci(l, "Plot")) return true;
        if (str_contains_ci(l, "Line")) return true;
        if (str_contains_ci(l, "Rect")) return true;
        if (str_contains_ci(l, "CreateButton")) return true;
        if (str_contains_ci(l, "CreatePanel")) return true;
        if (str_contains_ci(l, "CreateTextField")) return true;
        if (str_contains_ci(l, "CreateListBox")) return true;
        if (str_contains_ci(l, "CreateTextArea")) return true;
        if (str_contains_ci(l, "CreateMenu")) return true;
        if (str_contains_ci(l, "WindowMenu")) return true;
    }
    return false;
}

static void start_compile(bool also_run) {
    Document *d = &docs[active_tab];
    if (!ends_with_bb(d->file_name)) {
        set_status("EL ARCHIVO DEBE LLAMARSE *.BB");
        return;
    }
    set_status(also_run ? "COMPILANDO Y EJECUTANDO..." : "COMPILANDO...");
    save_current_to_root(d);

    build_log_count = 0;
    build_line_len = 0;
    build_line_buf[0] = '\0';
    build_in_progress = true;
    build_auto_run = also_run;
    build_launched = false;
    build_is_window_app = source_uses_graphics(d);
    { int i = 0; while (d->file_name[i] && i < 31) { build_target_name[i] = d->file_name[i]; i++; } build_target_name[i] = '\0'; }
    dialog_mode = DLG_BUILD;

    // 0xFFFFFFFF = sin preferencia de carpeta -- nbc.pro se busca en
    // la raiz y luego en PROGRAMAS, como hace 'run' en la shell.
    syscall5(SYS_LAUNCH_PROGRAM, (uint64_t)"nbc.pro", (uint64_t)d->file_name, 0xFFFFFFFF, 0, 0);
}

static void build_log_push(const char *line) {
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
                        syscall5(SYS_LAUNCH_PROGRAM, (uint64_t)pro_name, 0, 0xFFFFFFFF, 0, 0);
                        dialog_mode = DLG_NONE;
                        set_status("EJECUTANDO...");
                    } else {
                        // Programa de solo consola -- lo abrimos dentro
                        // de una SHELL nueva, con "run <programa>" ya
                        // preparado, para que su salida (si la tiene)
                        // tenga un sitio natural donde mostrarse.
                        char shell_cmd[36];
                        int si = 0;
                        const char *prefix = "run ";
                        while (prefix[si]) { shell_cmd[si] = prefix[si]; si++; }
                        int pi = 0;
                        while (pro_name[pi] && si < 35) { shell_cmd[si++] = pro_name[pi++]; }
                        shell_cmd[si] = '\0';
                        syscall5(SYS_LAUNCH_PROGRAM, (uint64_t)"shell.pro", (uint64_t)shell_cmd, 0xFFFFFFFF, 0, 0);
                        dialog_mode = DLG_NONE;
                        set_status("EJECUTANDO EN UNA SHELL NUEVA");
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
    for (int i = 0; i < d->line_count; i++) {
        if (d->has_selection && i == d->sel_row) {
            int a = d->sel_start_col < d->sel_end_col ? d->sel_start_col : d->sel_end_col;
            int b = d->sel_start_col < d->sel_end_col ? d->sel_end_col : d->sel_start_col;
            draw_rect(4 + a * 6, text_top + i * LINE_H, (b - a) * 6, 10, COLOR_SELECTION);
        }
        draw_text(4, text_top + i * LINE_H, d->lines[i], COLOR_TEXT);
    }

    int cx = 4 + d->cursor_col * 6;
    int cy = text_top + d->cursor_row * LINE_H;
    draw_rect(cx, cy, 2, 10, COLOR_CURSOR);

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
        draw_text(dx + 8, dy + 8, "NEMO OS - IDE", COLOR_MENUBAR_TEXT);
        draw_text(dx + 8, dy + 24, "PESTANAS + COMPILAR/EJECUTAR", COLOR_MENUBAR_TEXT);
        draw_text(dx + 8, dy + 38, "INTEGRADO (NBC.PRO)", COLOR_MENUBAR_TEXT);
        draw_text(dx + 8, dy + dh - 16, "ENTER PARA CERRAR", COLOR_STATUS_TEXT);
    } else if (dialog_mode == DLG_BUILD) {
        int dw = win_w - 20;
        int dh = 180;
        int dx = 10;
        int dy = (win_h - dh) / 2;
        draw_rect(dx, dy, dw, dh, COLOR_BUILD_BG);
        draw_rect(dx, dy, dw, 1, 0x00000000);
        draw_text(dx + 6, dy + 4,
                  build_in_progress ? "COMPILANDO..." : (build_launched ? "EJECUTANDO (ENTER PARA CERRAR)" : "COMPILACION TERMINADA"),
                  COLOR_TEXT);
        for (int i = 0; i < build_log_count; i++) {
            bool is_err = str_contains_ci(build_log[i], "error");
            draw_text(dx + 6, dy + 18 + i * 12, build_log[i], is_err ? COLOR_BUILD_ERR : COLOR_BUILD_TEXT);
        }
        if (!build_in_progress) draw_text(dx + 6, dy + dh - 14, "ENTER PARA CERRAR", COLOR_STATUS_TEXT);
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
