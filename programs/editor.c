// editor.c — Nemo OS
//
// Editor de texto v3, estilo Bloc de notas: barra de menus (Archivo,
// Edicion, Ayuda), Cortar/Copiar/Pegar via el portapapeles del
// sistema, y Abrir/Guardar como delegados en el dialogo COMUN del
// kernel (ver dialog.c) -- asi que este archivo ya no tiene ni una
// linea de navegacion de carpetas: eso vive una unica vez en el
// kernel, y cualquier programa lo reutiliza igual que hicimos aqui.
//
// Los menus desplegables SI son cosa nuestra (no del kernel) -- los
// dibujamos con las primitivas de siempre (SYS_DRAW_RECT/TEXT) y
// detectamos los clics con SYS_GET_MOUSE.

#include <stdint.h>
#include <stdbool.h>

#define SYS_WRITE_STRING     11
#define SYS_READ_CHAR        12
#define SYS_PUMP             14
#define SYS_CLIPBOARD_SET    3
#define SYS_CLIPBOARD_GET    4
#define SYS_FILE_OPEN        20
#define SYS_FILE_READ        21
#define SYS_FILE_WRITE       22
#define SYS_FILE_LIST        23
#define SYS_DRAW_RECT        30
#define SYS_DRAW_TEXT        31
#define SYS_DRAW_ICON        32
#define SYS_GET_WINDOW_SIZE  33
#define SYS_GET_MOUSE        34
#define SYS_OPEN_FILE_DIALOG 38
#define SYS_SAVE_FILE_DIALOG 39
#define SYS_GET_LAUNCH_ARG   6

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

// LIMITE REAL ENCONTRADO Y CORREGIDO: 60 lineas era demasiado poco
// incluso para programas de ejemplo reales de tamaño moderado (un
// CheckMenu.bb de 87 lineas se truncaba EN SILENCIO al cargarlo,
// cortando justo a mitad de un bloque Select/Case -- el usuario vio
// esto como un "error del compilador" cuando en realidad el archivo
// que nbc.pro recibia ya venia incompleto). Ampliado con margen real
// -- editor.pro corre con 16MB disponibles por tarea, y el coste de
// esta ampliacion es de apenas ~475KB.
#define MAX_LINES 2000
#define MAX_COLS  80

static char lines[MAX_LINES][MAX_COLS + 1];
static int line_count;
static int cursor_row, cursor_col;
static int win_w, win_h;
static bool running;
static char status_msg[48] = "";
static int32_t current_file_inode = -1; // -1 = documento nuevo, sin guardar aun
static char current_file_name[28] = "SIN TITULO";
static uint32_t documentos_dir = 0;

// -- seleccion de texto (para cortar/copiar) --
static bool has_selection = false;
static int sel_row = 0, sel_start_col = 0, sel_end_col = 0;

#define COLOR_BG        0x00202020
#define COLOR_TEXT      0x00E0E0E0
#define COLOR_CURSOR    0x0000FF00
#define COLOR_SELECTION 0x00405070
#define COLOR_MENUBAR   0x00D4D0C8
#define COLOR_MENUBAR_TEXT 0x00000000
#define COLOR_MENU_BG   0x00F0F0F0
#define COLOR_STATUSBAR 0x00303030
#define COLOR_STATUS_TEXT 0x00A0A0A0

#define MENUBAR_H 16
#define MENU_ROW_H 16
#define LINE_H 12
#define TEXT_TOP (MENUBAR_H + 4)

// -- menus --
#define MENU_NONE 0
#define MENU_FILE 1
#define MENU_EDIT 2
#define MENU_HELP 3
static int open_menu = MENU_NONE;

#define FILE_ITEMS 5
#define EDIT_ITEMS 4
#define HELP_ITEMS 1

// IMPORTANTE: nada de arrays estaticos de punteros a cadenas, ni de
// switch con muchos casos -- ver el Makefile (-fno-jump-tables
// -fno-tree-switch-conversion) y la explicacion completa mas abajo,
// en menu_item_text.
static const char *menu_item_text(int menu, int index) {
    if (menu == MENU_FILE) {
        if (index == 0) return "NUEVO";
        if (index == 1) return "ABRIR";
        if (index == 2) return "GUARDAR";
        if (index == 3) return "GUARDAR COMO";
        if (index == 4) return "SALIR";
    } else if (menu == MENU_EDIT) {
        if (index == 0) return "CORTAR";
        if (index == 1) return "COPIAR";
        if (index == 2) return "PEGAR";
        if (index == 3) return "SELECCIONAR TODO";
    } else if (menu == MENU_HELP) {
        if (index == 0) return "ACERCA DE";
    }
    return "";
}

// -- dialogo "Acerca de", el unico que sigue siendo cosa nuestra --
// Abrir/Guardar como ahora los resuelve el kernel (dialog.c) via
// syscall, asi que no hay estado de navegacion que mantener aqui.
#define DLG_NONE 0
#define DLG_ABOUT 1
static int dialog_mode = DLG_NONE;

static void clear_buffer(void) {
    for (int i = 0; i < MAX_LINES; i++) lines[i][0] = '\0';
    line_count = 1;
    cursor_row = 0;
    cursor_col = 0;
    has_selection = false;
    current_file_inode = -1;
    const char *untitled = "SIN TITULO";
    int i = 0;
    while (untitled[i]) { current_file_name[i] = untitled[i]; i++; }
    current_file_name[i] = '\0';
}

static void set_status(const char *msg) {
    int i = 0;
    while (msg[i] && i < 47) { status_msg[i] = msg[i]; i++; }
    status_msg[i] = '\0';
}

// ---- menu desplegable: dibujo y deteccion de clics ----

static void draw_menubar(void) {
    draw_rect(0, 0, win_w, MENUBAR_H, COLOR_MENUBAR);
    draw_text(6, 4, "ARCHIVO", COLOR_MENUBAR_TEXT);
    draw_text(70, 4, "EDICION", COLOR_MENUBAR_TEXT);
    draw_text(134, 4, "AYUDA", COLOR_MENUBAR_TEXT);
}

static void draw_dropdown(int menu, int count, int x) {
    int w = 130;
    int h = count * MENU_ROW_H + 4;
    draw_rect(x, MENUBAR_H, w, h, COLOR_MENU_BG);
    draw_rect(x, MENUBAR_H, w, 1, 0x00000000);
    for (int i = 0; i < count; i++) {
        draw_text(x + 6, MENUBAR_H + 4 + i * MENU_ROW_H, menu_item_text(menu, i), COLOR_MENUBAR_TEXT);
    }
}

// ---- portapapeles ----

static void copy_selection_to_clipboard(void) {
    if (!has_selection) return;
    int a = sel_start_col < sel_end_col ? sel_start_col : sel_end_col;
    int b = sel_start_col < sel_end_col ? sel_end_col : sel_start_col;
    syscall5(SYS_CLIPBOARD_SET, (uint64_t)&lines[sel_row][a], (uint64_t)(b - a), 0, 0, 0);
    set_status("COPIADO");
}

static void delete_selection(void) {
    if (!has_selection) return;
    int a = sel_start_col < sel_end_col ? sel_start_col : sel_end_col;
    int b = sel_start_col < sel_end_col ? sel_end_col : sel_start_col;
    int len = str_len(lines[sel_row]);
    for (int i = a; i + (b - a) < len + 1; i++) lines[sel_row][i] = lines[sel_row][i + (b - a)];
    cursor_row = sel_row;
    cursor_col = a;
    has_selection = false;
}

static void paste_from_clipboard(void) {
    static char buf[512];
    uint64_t n = syscall5(SYS_CLIPBOARD_GET, (uint64_t)buf, sizeof(buf) - 1, 0, 0, 0);
    for (uint64_t i = 0; i < n; i++) {
        int len = str_len(lines[cursor_row]);
        if (len >= MAX_COLS) break;
        for (int j = len; j > cursor_col; j--) lines[cursor_row][j] = lines[cursor_row][j - 1];
        lines[cursor_row][cursor_col] = buf[i];
        lines[cursor_row][len + 1] = '\0';
        cursor_col++;
    }
    set_status("PEGADO");
}

// ---- edicion de texto ----

static void insert_char(char c) {
    if (has_selection) delete_selection();
    int len = str_len(lines[cursor_row]);
    if (len >= MAX_COLS) return;
    for (int i = len; i > cursor_col; i--) lines[cursor_row][i] = lines[cursor_row][i - 1];
    lines[cursor_row][cursor_col] = c;
    lines[cursor_row][len + 1] = '\0';
    cursor_col++;
}

static void split_line(void) {
    if (line_count >= MAX_LINES) return;
    for (int i = line_count; i > cursor_row + 1; i--) {
        int j = 0;
        while (lines[i - 1][j]) { lines[i][j] = lines[i - 1][j]; j++; }
        lines[i][j] = '\0';
    }
    int len = str_len(lines[cursor_row]);
    int j = 0;
    for (int i = cursor_col; i < len; i++) lines[cursor_row + 1][j++] = lines[cursor_row][i];
    lines[cursor_row + 1][j] = '\0';
    lines[cursor_row][cursor_col] = '\0';
    line_count++;
    cursor_row++;
    cursor_col = 0;
}

static void backspace(void) {
    if (has_selection) { delete_selection(); return; }
    if (cursor_col > 0) {
        int len = str_len(lines[cursor_row]);
        for (int i = cursor_col - 1; i < len; i++) lines[cursor_row][i] = lines[cursor_row][i + 1];
        cursor_col--;
    } else if (cursor_row > 0) {
        int prev_len = str_len(lines[cursor_row - 1]);
        int this_len = str_len(lines[cursor_row]);
        if (prev_len + this_len < MAX_COLS) {
            for (int i = 0; i < this_len; i++) lines[cursor_row - 1][prev_len + i] = lines[cursor_row][i];
            lines[cursor_row - 1][prev_len + this_len] = '\0';
            for (int i = cursor_row; i < line_count - 1; i++) {
                int j = 0;
                while (lines[i + 1][j]) { lines[i][j] = lines[i + 1][j]; j++; }
                lines[i][j] = '\0';
            }
            line_count--;
            cursor_row--;
            cursor_col = prev_len;
        }
    }
}

static void move_cursor(char c) {
    has_selection = false;
    if (c == CH_LEFT) {
        if (cursor_col > 0) cursor_col--;
        else if (cursor_row > 0) { cursor_row--; cursor_col = str_len(lines[cursor_row]); }
    } else if (c == CH_RIGHT) {
        int len = str_len(lines[cursor_row]);
        if (cursor_col < len) cursor_col++;
        else if (cursor_row < line_count - 1) { cursor_row++; cursor_col = 0; }
    } else if (c == CH_UP) {
        if (cursor_row > 0) {
            cursor_row--;
            int len = str_len(lines[cursor_row]);
            if (cursor_col > len) cursor_col = len;
        }
    } else if (c == CH_DOWN) {
        if (cursor_row < line_count - 1) {
            cursor_row++;
            int len = str_len(lines[cursor_row]);
            if (cursor_col > len) cursor_col = len;
        }
    }
}

static void select_all(void) {
    // Simplificado: seleccionamos solo la linea actual completa --
    // suficiente para copiar fragmentos de una linea; seleccionar
    // varias lineas a la vez queda para una version futura.
    sel_row = cursor_row;
    sel_start_col = 0;
    sel_end_col = str_len(lines[cursor_row]);
    has_selection = true;
}

// ---- archivos: abrir / guardar ----
//
// Ya NO hay aqui ninguna funcion de navegacion de carpetas -- eso lo
// resuelve el dialogo comun del kernel (SYS_OPEN_FILE_DIALOG /
// SYS_SAVE_FILE_DIALOG). Estas funciones solo se encargan de lo que
// es especifico del editor: volcar el contenido a/desde nuestro
// buffer de lineas.

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
        if (str_eq(name, "DOCUMENTOS")) {
            documentos_dir = inode;
            return;
        }
    }
}

static void load_file(int32_t inode, const char *name) {
    static char content[MAX_LINES * (MAX_COLS + 1)];
    int64_t bytes = (int64_t)syscall5(SYS_FILE_READ, (uint64_t)inode, (uint64_t)content, sizeof(content) - 1, 0, 0);
    if (bytes < 0) bytes = 0;

    clear_buffer();
    current_file_inode = inode;
    int i = 0, ni = 0;
    while (name[i] && ni < 27) { current_file_name[ni] = name[i]; i++; ni++; }
    current_file_name[ni] = '\0';

    int row = 0, col = 0;
    int64_t p = 0;
    for (; p < bytes && row < MAX_LINES; p++) {
        char c = content[p];
        if (c == '\n') {
            lines[row][col] = '\0';
            row++;
            col = 0;
        } else if (col < MAX_COLS) {
            lines[row][col] = c;
            col++;
        }
    }
    lines[row][col] = '\0';
    // AVISO REAL, NO SILENCIOSO: si quedaba contenido leido sin
    // colocar (nos quedamos sin filas disponibles ANTES de terminar
    // de leer el archivo), lo decimos -- antes esto pasaba
    // desapercibido y parecia un bug del programa que se estaba
    // editando, cuando en realidad era el editor cortando el archivo
    // sin avisar. Comprobamos 'p < bytes' (quedaba contenido sin
    // procesar), no solo 'row >= MAX_LINES' (que tambien seria
    // cierto, sin ser un problema, si el archivo termina justo al
    // llegar a la ultima fila disponible).
    bool truncated = (p < bytes) || (bytes == (int64_t)(sizeof(content) - 1));
    line_count = row + 1;
    cursor_row = 0;
    cursor_col = 0;
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
    while (arg[i] >= '0' && arg[i] <= '9') {
        parent = parent * 10 + (uint32_t)(arg[i] - '0');
        i++;
    }
    if (arg[i] != ':') return; // formato inesperado, lo ignoramos
    const char *name = &arg[i + 1];

    int32_t inode = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)name, parent, 0, 0, 0);
    if (inode >= 0) load_file(inode, name);
}

static void save_to_inode(int32_t inode) {
    static char content[MAX_LINES * (MAX_COLS + 1)];
    int pos = 0;
    for (int i = 0; i < line_count; i++) {
        int len = str_len(lines[i]);
        for (int j = 0; j < len; j++) content[pos++] = lines[i][j];
        content[pos++] = '\n';
    }
    syscall5(SYS_FILE_WRITE, (uint64_t)inode, (uint64_t)content, (uint64_t)pos, 0, 0);
    set_status("GUARDADO");
}

// Muestra el dialogo comun de "Abrir" (vive en el kernel). Bloquea
// hasta que el usuario elige un archivo o cancela.
static void do_open(void) {
    static char name_buf[28];
    int32_t inode = (int32_t)syscall5(SYS_OPEN_FILE_DIALOG, documentos_dir, (uint64_t)name_buf, sizeof(name_buf), 0, 0);
    if (inode < 0) return; // cancelado
    load_file(inode, name_buf);
}

// Igual, pero para "Guardar como".
static void do_save_as(void) {
    static char name_buf[28];
    int32_t inode = (int32_t)syscall5(SYS_SAVE_FILE_DIALOG, documentos_dir, (uint64_t)name_buf, sizeof(name_buf), 0, 0);
    if (inode < 0) return; // cancelado
    current_file_inode = inode;
    int i = 0;
    while (name_buf[i] && i < 27) { current_file_name[i] = name_buf[i]; i++; }
    current_file_name[i] = '\0';
    save_to_inode(inode);
}

static void do_save(void) {
    if (current_file_inode >= 0) {
        save_to_inode((uint32_t)current_file_inode);
    } else {
        do_save_as();
    }
}

// ---- dibujo ----

static void redraw(void) {
    draw_rect(0, 0, win_w, win_h, COLOR_BG);
    draw_menubar();

    int text_top = TEXT_TOP;
    for (int i = 0; i < line_count; i++) {
        if (has_selection && i == sel_row) {
            int a = sel_start_col < sel_end_col ? sel_start_col : sel_end_col;
            int b = sel_start_col < sel_end_col ? sel_end_col : sel_start_col;
            draw_rect(4 + a * 6, text_top + i * LINE_H, (b - a) * 6, 10, COLOR_SELECTION);
        }
        draw_text(4, text_top + i * LINE_H, lines[i], COLOR_TEXT);
    }

    int cx = 4 + cursor_col * 6;
    int cy = text_top + cursor_row * LINE_H;
    draw_rect(cx, cy, 2, 10, COLOR_CURSOR);

    // Barra de estado
    int status_y = win_h - 12;
    draw_rect(0, status_y - 2, win_w, 14, COLOR_STATUSBAR);
    draw_text(4, status_y, current_file_name, COLOR_STATUS_TEXT);
    if (status_msg[0] != '\0') {
        draw_text(win_w - 110, status_y, status_msg, COLOR_STATUS_TEXT);
    }

    // Menus desplegables (encima de todo lo demas)
    if (open_menu == MENU_FILE) draw_dropdown(MENU_FILE, FILE_ITEMS, 0);
    else if (open_menu == MENU_EDIT) draw_dropdown(MENU_EDIT, EDIT_ITEMS, 64);
    else if (open_menu == MENU_HELP) draw_dropdown(MENU_HELP, HELP_ITEMS, 128);

    // El unico dialogo que sigue siendo nuestro: Acerca de
    if (dialog_mode == DLG_ABOUT) {
        int dw = win_w > 220 ? 220 : win_w - 20;
        int dh = 100;
        int dx = (win_w - dw) / 2;
        int dy = (win_h - dh) / 2;
        draw_rect(dx, dy, dw, dh, COLOR_MENU_BG);
        draw_rect(dx, dy, dw, 1, 0x00000000);
        draw_text(dx + 8, dy + 8, "NEMO OS - EDITOR DE TEXTO", COLOR_MENUBAR_TEXT);
        draw_text(dx + 8, dy + 24, "MENUS, PORTAPAPELES Y", COLOR_MENUBAR_TEXT);
        draw_text(dx + 8, dy + 38, "DIALOGO COMUN DE ARCHIVOS", COLOR_MENUBAR_TEXT);
        draw_text(dx + 8, dy + dh - 16, "ENTER PARA CERRAR", COLOR_STATUS_TEXT);
    }
}

// ---- entrada ----

static void handle_menu_click(int mx, int my) {
    if (my < MENUBAR_H) {
        if (mx < 64) open_menu = (open_menu == MENU_FILE) ? MENU_NONE : MENU_FILE;
        else if (mx < 128) open_menu = (open_menu == MENU_EDIT) ? MENU_NONE : MENU_EDIT;
        else if (mx < 192) open_menu = (open_menu == MENU_HELP) ? MENU_NONE : MENU_HELP;
        else open_menu = MENU_NONE;
        return;
    }

    if (open_menu == MENU_NONE) return;

    int x0 = (open_menu == MENU_FILE) ? 0 : (open_menu == MENU_EDIT) ? 64 : 128;
    int count = (open_menu == MENU_FILE) ? FILE_ITEMS : (open_menu == MENU_EDIT) ? EDIT_ITEMS : HELP_ITEMS;
    int w = 130;
    int h = count * MENU_ROW_H + 4;

    if (mx < x0 || mx >= x0 + w || my < MENUBAR_H || my >= MENUBAR_H + h) {
        open_menu = MENU_NONE;
        return;
    }

    int item = (my - MENUBAR_H - 2) / MENU_ROW_H;
    if (item < 0 || item >= count) { open_menu = MENU_NONE; return; }

    // Cerramos el menu ANTES de llamar a los dialogos de
    // abrir/guardar, porque esas llamadas bloquean (ceden el control
    // a otras tareas) hasta que el usuario termina -- no queremos que
    // el menu se quede "abierto" logicamente durante todo ese tiempo.
    open_menu = MENU_NONE;

    if (x0 == 0) { // ARCHIVO
        if (item == 0) clear_buffer();
        else if (item == 1) do_open();
        else if (item == 2) do_save();
        else if (item == 3) do_save_as();
        else if (item == 4) running = false;
    } else if (x0 == 64) { // EDICION
        if (item == 0) { copy_selection_to_clipboard(); delete_selection(); }
        else if (item == 1) copy_selection_to_clipboard();
        else if (item == 2) paste_from_clipboard();
        else if (item == 3) select_all();
    } else { // AYUDA
        if (item == 0) dialog_mode = DLG_ABOUT;
    }
}

static void handle_dialog_key(char c) {
    if (dialog_mode == DLG_ABOUT && c == '\n') {
        dialog_mode = DLG_NONE;
    }
}

__attribute__((section(".text.start")))
void _start(void) {
    clear_buffer();
    running = true;
    status_msg[0] = '\0';
    open_menu = MENU_NONE;
    dialog_mode = DLG_NONE;
    documentos_dir = 0;

    find_documentos();
    try_open_from_launch_arg();

    uint64_t size = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
    win_w = (int)(size >> 32);
    win_h = (int)(size & 0xFFFFFFFF);
    if (win_w <= 0) win_w = 420;
    if (win_h <= 0) win_h = 300;

    redraw();

    bool last_left = false;

    while (running) {
        uint64_t pump = syscall5(SYS_PUMP, 0, 0, 0, 0, 0);
        if ((int64_t)pump < 0) break;

        uint64_t sz = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
        int nw = (int)(sz >> 32), nh = (int)(sz & 0xFFFFFFFF);
        bool need_redraw = (nw > 0 && nw != win_w) || (nh > 0 && nh != win_h);
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
