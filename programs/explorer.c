// explorer.c — Nemo OS
//
// Explorador de archivos v3: dos paneles (carpetas a la izquierda,
// contenido completo a la derecha) y selector de disco -- NemoFS o el
// disco FAT (el que se puede montar directamente en el Mac), para
// poder copiar archivos entre los dos.
//
// FAT v1 solo tiene directorio raiz (sin subcarpetas todavia), asi
// que el panel de carpetas se queda vacio cuando estamos en ese
// disco -- limitacion real del driver, no un error.

#include <stdint.h>
#include <stdbool.h>

#define SYS_READ_CHAR       12
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
#define SYS_FILE_CLIPBOARD_SET 26
#define SYS_FILE_CLIPBOARD_GET 27

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

static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    syscall5(SYS_DRAW_RECT, (uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h, color);
}
static void draw_text(int x, int y, const char *s, uint32_t color) {
    syscall5(SYS_DRAW_TEXT, (uint64_t)x, (uint64_t)y, (uint64_t)s, color, 0);
}
static void draw_icon(int x, int y, int icon_id) {
    syscall5(SYS_DRAW_ICON, (uint64_t)x, (uint64_t)y, (uint64_t)icon_id, 0, 0);
}
static void define_button(uint32_t id, int x, int y, int w, int h, uint32_t color) {
    uint64_t packed_wh = ((uint64_t)(uint16_t)w << 16) | (uint16_t)h;
    syscall5(SYS_DEFINE_BUTTON, id, (uint64_t)x, (uint64_t)y, packed_wh, color);
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
static int append_dec(char *buf, int pos, uint32_t value) {
    if (value == 0) { buf[pos++] = '0'; return pos; }
    char digits[10]; int n = 0;
    while (value > 0) { digits[n++] = (char)('0' + value % 10); value /= 10; }
    while (n > 0) buf[pos++] = digits[--n];
    return pos;
}

// -- estado --
#define MAX_ENTRIES 40
typedef struct {
    char name[28];
    uint32_t type;
    uint32_t size;
} entry_t;

static entry_t all_entries[MAX_ENTRIES];   // panel derecho: todo
static uint32_t all_count = 0;
static entry_t folder_entries[MAX_ENTRIES]; // panel izquierdo: solo carpetas
static uint32_t folder_count = 0;

static uint32_t current_volume = VOLUME_NEMOFS;
static uint32_t current_dir = 0; // solo relevante en NemoFS

// Desplazamiento de cada panel -- indice de la primera fila visible.
// Separados porque cada panel puede tener una cantidad de entradas
// distinta (el izquierdo solo carpetas, el derecho todo).
static uint32_t scroll_left = 0;
static uint32_t scroll_right = 0;

#define DIR_STACK_MAX 16
static uint32_t dir_stack[DIR_STACK_MAX];
static int dir_depth = 0;

static char status_msg[64] = "";
static char selected_name[28] = "";
static bool selected_is_dir = false;

// -- menu contextual (clic derecho) --
#define CTX_ITEM_H 18
static bool ctx_menu_open = false;
static int ctx_x, ctx_y;
static char ctx_target_name[28] = ""; // "" = clic en zona vacia (solo PEGAR)

static int win_w, win_h;
static bool running = true;

#define BTN_NEMOFS  1
#define BTN_FAT     2
#define BTN_UP      3
#define BTN_NEWDIR  4
#define BTN_COPY    5
#define BTN_EXIT    6

#define TOPBAR_H   24
#define PATHBAR_H  16
#define ROW_H      26
#define LEFT_W     110

#define COLOR_BG        0x00181C20
#define COLOR_TOPBAR    0x00303840
#define COLOR_BTN       0x00505860
#define COLOR_BTN_TEXT  0x00FFFFFF
#define COLOR_PATHBAR   0x00202830
#define COLOR_PATH_TEXT 0x0080A0FF
#define COLOR_LEFT_BG   0x00202830
#define COLOR_RIGHT_BG  0x00181C20
#define COLOR_DIVIDER   0x00404850
#define COLOR_TEXT      0x00E0E0E0
#define COLOR_STATUS    0x0080FF80

static void set_status(const char *msg) {
    int i = 0;
    while (msg[i] && i < 63) { status_msg[i] = msg[i]; i++; }
    status_msg[i] = '\0';
}

// ---- carga de listados ----

static void load_listing(void) {
    static uint8_t raw[MAX_ENTRIES * 40];
    uint64_t total = syscall5(SYS_FILE_LIST, current_dir, (uint64_t)raw, MAX_ENTRIES, current_volume, 0);
    uint64_t shown = total < MAX_ENTRIES ? total : MAX_ENTRIES;

    scroll_left = 0;
    scroll_right = 0;
    all_count = 0;
    folder_count = 0;
    for (uint64_t i = 0; i < shown; i++) {
        uint8_t *e = raw + i * 40;
        uint32_t type = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
        uint32_t size = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);

        entry_t *dst = &all_entries[all_count];
        dst->type = type;
        dst->size = size;
        int j = 0;
        while (e[12 + j] && j < 27) { dst->name[j] = (char)e[12 + j]; j++; }
        dst->name[j] = '\0';
        all_count++;

        if (type == TYPE_DIR && folder_count < MAX_ENTRIES) {
            entry_t *fd = &folder_entries[folder_count];
            *fd = *dst;
            folder_count++;
        }
    }
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

// Pide al kernel que abra este archivo con el editor -- "parent:nombre"
// es el formato que editor.c espera en su argumento de lanzamiento.
static void launch_editor_for(const char *name) {
    static char arg[32];
    int p = append_dec(arg, 0, current_dir);
    arg[p++] = ':';
    int i = 0;
    while (name[i] && p < 30) { arg[p++] = name[i]; i++; }
    arg[p] = '\0';
    syscall5(SYS_LAUNCH_PROGRAM, (uint64_t)"editor.pro", (uint64_t)arg, 0, 0, 0);
}

static void select_entry(const entry_t *e) {
    if (e->type == TYPE_DIR) {
        int32_t inode = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)e->name, current_dir, current_volume, 0, 0);
        if (inode >= 0) enter_dir((uint32_t)inode);
        return;
    }
    int i = 0;
    while (e->name[i] && i < 27) { selected_name[i] = e->name[i]; i++; }
    selected_name[i] = '\0';
    selected_is_dir = false;

    char msg[64];
    int p = 0;
    const char *pre = "TAMAÑO: ";
    int j = 0; while (pre[j]) msg[p++] = pre[j++];
    p = append_dec(msg, p, e->size);
    msg[p] = '\0';
    set_status(msg);

    // Cualquier archivo que no sea un programa se abre directamente
    // con el editor -- igual que hace doble clic en un .txt en
    // cualquier sistema operativo de verdad.
    if (current_volume == VOLUME_NEMOFS && !ends_with_pro(e->name)) {
        launch_editor_for(e->name);
    }
}

static void new_folder(void) {
    if (current_volume != VOLUME_NEMOFS) { set_status("FAT NO TIENE CARPETAS"); return; }
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
            set_status("CARPETA CREADA");
            return;
        }
    }
}

// Copia el archivo seleccionado al OTRO disco -- el motivo de ser de
// tener dos discos en el explorador: poder pasar archivos entre
// NemoFS y el disco que se puede montar en el Mac.
static void copy_selected_to_other_volume(void) {
    if (selected_name[0] == '\0') { set_status("NADA SELECCIONADO"); return; }

    uint32_t src_vol = current_volume;
    uint32_t dst_vol = (current_volume == VOLUME_NEMOFS) ? VOLUME_FAT : VOLUME_NEMOFS;

    int32_t src_id = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)selected_name, current_dir, src_vol, 0, 0);
    if (src_id < 0) { set_status("ERROR AL LEER ORIGEN"); return; }

    static uint8_t buf[268288]; // NemoFS admite archivos de hasta 268288 bytes (12 directos + 4 indirectos), ver la nota en nemofs.c
    int64_t bytes = (int64_t)syscall5(SYS_FILE_READ, (uint64_t)src_id, (uint64_t)buf, sizeof(buf), src_vol, 0);
    if (bytes < 0) { set_status("ERROR AL LEER ARCHIVO"); return; }
    if (bytes == (int64_t)sizeof(buf)) { set_status("AVISO: EL ARCHIVO PODRIA HABERSE TRUNCADO (DEMASIADO GRANDE)"); }

    uint32_t dst_parent = 0; // raiz en ambos casos -- sencillo y predecible
    int32_t dst_id = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)selected_name, dst_parent, dst_vol, 0, 0);
    if (dst_id < 0) { set_status("ERROR AL CREAR EN DESTINO"); return; }

    int64_t ok = (int64_t)syscall5(SYS_FILE_WRITE, (uint64_t)dst_id, (uint64_t)buf, (uint64_t)bytes, dst_vol, 0);
    if (ok < 0) {
        set_status("NO SE PUDO COPIAR (¿YA EXISTE ALLI?)");
    } else {
        set_status("COPIADO AL OTRO DISCO");
    }
}

// -- menu contextual: eliminar / copiar / pegar (dentro del mismo disco) --

static void ctx_delete(const char *name) {
    int64_t ok = (int64_t)syscall5(SYS_FILE_DELETE, (uint64_t)name, current_dir, current_volume, 0, 0);
    if (ok < 0) set_status("NO SE PUDO ELIMINAR");
    else { set_status("ELIMINADO"); load_listing(); }
}

static void ctx_copy(const char *name) {
    syscall5(SYS_FILE_CLIPBOARD_SET, (uint64_t)name, current_dir, current_volume, 0, 0);
    set_status("COPIADO AL PORTAPAPELES");
}

static void ctx_paste(void) {
    static char name[28];
    int64_t packed = (int64_t)syscall5(SYS_FILE_CLIPBOARD_GET, (uint64_t)name, sizeof(name), 0, 0, 0);
    if (packed < 0) { set_status("PORTAPAPELES DE ARCHIVOS VACIO"); return; }
    uint32_t src_parent = (uint32_t)((uint64_t)packed >> 32);
    uint32_t src_volume = (uint32_t)((uint64_t)packed & 0xFFFFFFFF);

    int32_t src_id = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)name, src_parent, src_volume, 0, 0);
    if (src_id < 0) { set_status("ORIGEN YA NO EXISTE"); return; }

    static uint8_t buf[268288]; // NemoFS admite archivos de hasta 268288 bytes (12 directos + 4 indirectos), ver la nota en nemofs.c
    int64_t bytes = (int64_t)syscall5(SYS_FILE_READ, (uint64_t)src_id, (uint64_t)buf, sizeof(buf), src_volume, 0);
    if (bytes < 0) { set_status("ERROR AL LEER"); return; }

    int32_t dst_id = (int32_t)syscall5(SYS_FILE_OPEN, (uint64_t)name, current_dir, current_volume, 0, 0);
    if (dst_id < 0) { set_status("ERROR AL CREAR"); return; }

    int64_t ok = (int64_t)syscall5(SYS_FILE_WRITE, (uint64_t)dst_id, (uint64_t)buf, (uint64_t)bytes, current_volume, 0);
    if (ok < 0) { set_status("NO SE PUDO PEGAR (¿YA EXISTE?)"); }
    else { set_status("PEGADO"); load_listing(); }
}

// ---- dibujo ----

// Cuantas filas caben de verdad en el alto disponible -- lo comparten
// el dibujado y el manejo de la rueda (para saber hasta donde se
// puede desplazar sin dejar hueco vacio de mas al final).
static uint32_t visible_rows(int content_y) {
    int content_h = win_h - content_y;
    if (content_h <= 0) return 0;
    return (uint32_t)(content_h / ROW_H);
}
static uint32_t clamp_scroll(uint32_t scroll, uint32_t count, uint32_t rows) {
    if (rows == 0 || count <= rows) return 0;
    uint32_t max_scroll = count - rows;
    return scroll > max_scroll ? max_scroll : scroll;
}

static void redraw(void) {
    draw_rect(0, 0, win_w, win_h, COLOR_BG);

    // Barra de botones
    draw_rect(0, 0, win_w, TOPBAR_H, COLOR_TOPBAR);
    define_button(BTN_NEMOFS, 2, 2, 62, TOPBAR_H - 4, (current_volume == VOLUME_NEMOFS) ? 0x00305080 : COLOR_BTN);
    draw_text(6, 7, "NEMOFS", COLOR_BTN_TEXT);
    define_button(BTN_FAT, 66, 2, 50, TOPBAR_H - 4, (current_volume == VOLUME_FAT) ? 0x00305080 : COLOR_BTN);
    draw_text(72, 7, "FAT", COLOR_BTN_TEXT);
    define_button(BTN_UP, 118, 2, 50, TOPBAR_H - 4, COLOR_BTN);
    draw_text(126, 7, "SUBIR", COLOR_BTN_TEXT);
    define_button(BTN_NEWDIR, 170, 2, 66, TOPBAR_H - 4, COLOR_BTN);
    draw_text(176, 7, "CARPETA", COLOR_BTN_TEXT);
    define_button(BTN_COPY, 238, 2, 90, TOPBAR_H - 4, COLOR_BTN);
    draw_text(244, 7, "COPIAR A OTRO", COLOR_BTN_TEXT);
    define_button(BTN_EXIT, win_w - 62, 2, 58, TOPBAR_H - 4, 0x00804040);
    draw_text(win_w - 56, 7, "SALIR", COLOR_BTN_TEXT);

    // Barra de ruta / estado
    draw_rect(0, TOPBAR_H, win_w, PATHBAR_H, COLOR_PATHBAR);
    const char *vol_label = (current_volume == VOLUME_NEMOFS) ? "NEMOFS:/" : "FAT (DISCO MAC):/";
    draw_text(4, TOPBAR_H + 3, vol_label, COLOR_PATH_TEXT);
    if (status_msg[0]) draw_text(win_w - 200, TOPBAR_H + 3, status_msg, COLOR_STATUS);

    int content_y = TOPBAR_H + PATHBAR_H;
    int content_h = win_h - content_y;
    uint32_t rows = visible_rows(content_y);
    scroll_left = clamp_scroll(scroll_left, folder_count, rows);
    scroll_right = clamp_scroll(scroll_right, all_count, rows);

    // Panel izquierdo: solo carpetas
    draw_rect(0, content_y, LEFT_W, content_h, COLOR_LEFT_BG);
    for (uint32_t i = scroll_left; i < folder_count; i++) {
        int ry = content_y + 2 + (int)(i - scroll_left) * ROW_H;
        if (ry + ROW_H > win_h) break;
        draw_icon(4, ry, ICON_FOLDER);
        draw_text(30, ry + 8, folder_entries[i].name, COLOR_TEXT);
    }
    if (folder_count == 0) {
        draw_text(4, content_y + 4, "(SIN CARPETAS)", COLOR_TEXT);
    }

    // Divisor
    draw_rect(LEFT_W, content_y, 1, content_h, COLOR_DIVIDER);

    // Panel derecho: todo (carpetas + archivos, con tipo y tamaño)
    int right_x = LEFT_W + 1;
    int right_w = win_w - right_x;
    draw_rect(right_x, content_y, right_w, content_h, COLOR_RIGHT_BG);
    for (uint32_t i = scroll_right; i < all_count; i++) {
        int ry = content_y + 2 + (int)(i - scroll_right) * ROW_H;
        if (ry + ROW_H > win_h) break;
        entry_t *e = &all_entries[i];

        int icon;
        if (e->type == TYPE_DIR) icon = ICON_FOLDER;
        else if (ends_with_pro(e->name)) icon = ICON_CODE;
        else icon = ICON_TXT;
        draw_icon(right_x + 4, ry, icon);

        uint32_t name_color = str_eq(e->name, selected_name) ? COLOR_STATUS : COLOR_TEXT;
        draw_text(right_x + 30, ry + 8, e->name, name_color);

        if (e->type != TYPE_DIR && right_w > 160) {
            char size_str[16];
            int p = append_dec(size_str, 0, e->size);
            size_str[p] = '\0';
            draw_text(right_x + right_w - 60, ry + 8, size_str, COLOR_TEXT);
        }
    }

    // Menu contextual (clic derecho), encima de todo lo demas
    if (ctx_menu_open) {
        int count = ctx_target_name[0] ? 3 : 1;
        int w = 120;
        int h = count * CTX_ITEM_H + 4;
        int x = ctx_x, y = ctx_y;
        if (x + w > win_w) x = win_w - w;
        if (y + h > win_h) y = win_h - h;

        draw_rect(x, y, w, h, 0x00F0F0F0);
        draw_rect(x, y, w, 1, 0x00000000);

        int row = 0;
        if (ctx_target_name[0]) {
            draw_text(x + 6, y + 4 + row * CTX_ITEM_H, "ELIMINAR", 0x00000000);
            row++;
            draw_text(x + 6, y + 4 + row * CTX_ITEM_H, "COPIAR", 0x00000000);
            row++;
        }
        draw_text(x + 6, y + 4 + row * CTX_ITEM_H, "PEGAR", 0x00000000);
    }
}

// ---- entrada ----

// Devuelve un puntero a la entrada bajo (mx,my), o NULL si el punto
// cae fuera de cualquier fila (cabecera, divisor, zona vacia...).
// La comparten el clic izquierdo (seleccionar) y el derecho (abrir el
// menu contextual sobre esa entrada).
static const entry_t *entry_at(int mx, int my) {
    int content_y = TOPBAR_H + PATHBAR_H;
    if (my < content_y) return 0;
    uint32_t row = (uint32_t)((my - content_y - 2) / ROW_H);

    if (mx < LEFT_W) {
        row += scroll_left;
        if (row < folder_count) return &folder_entries[row];
    } else {
        row += scroll_right;
        if (row < all_count) return &all_entries[row];
    }
    return 0;
}

static void handle_click(int mx, int my) {
    if (my < TOPBAR_H) return;
    const entry_t *e = entry_at(mx, my);
    if (e) select_entry(e);
}

static void open_context_menu(int mx, int my) {
    ctx_x = mx;
    ctx_y = my;
    ctx_target_name[0] = '\0';

    const entry_t *e = entry_at(mx, my);
    if (e) {
        int i = 0;
        while (e->name[i] && i < 27) { ctx_target_name[i] = e->name[i]; i++; }
        ctx_target_name[i] = '\0';
    }
    ctx_menu_open = true;
}

static void handle_context_click(int mx, int my) {
    int count = ctx_target_name[0] ? 3 : 1;
    int w = 120;
    int h = count * CTX_ITEM_H + 4;
    int x = ctx_x, y = ctx_y;
    if (x + w > win_w) x = win_w - w;
    if (y + h > win_h) y = win_h - h;

    ctx_menu_open = false; // se cierra siempre, caiga donde caiga el clic

    if (mx < x || mx >= x + w || my < y || my >= y + h) return; // clic fuera -- solo cerrar

    int item = (my - y - 2) / CTX_ITEM_H;

    if (ctx_target_name[0]) {
        if (item == 0) ctx_delete(ctx_target_name);
        else if (item == 1) ctx_copy(ctx_target_name);
        else if (item == 2) ctx_paste();
    } else {
        if (item == 0) ctx_paste();
    }
}

__attribute__((section(".text.start")))
void _start(void) {
    uint64_t size = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
    win_w = (int)(size >> 32);
    win_h = (int)(size & 0xFFFFFFFF);
    if (win_w <= 0) win_w = 480;
    if (win_h <= 0) win_h = 300;

    load_listing();
    redraw();

    bool last_left = false;
    bool last_right = false;

    while (running) {
        uint64_t pump = syscall5(SYS_PUMP, 0, 0, 0, 0, 0);
        if ((int64_t)pump < 0) break;

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
                if (wheel > 0) {
                    *scroll = ((uint32_t)wheel > *scroll) ? 0 : *scroll - (uint32_t)wheel;
                } else {
                    *scroll += (uint32_t)(-wheel);
                }
                need_redraw = true;
            }

            if (right && !last_right && !ctx_menu_open && my >= TOPBAR_H) {
                open_context_menu(mx, my);
                need_redraw = true;
            }
            last_right = right;

            if (left && !last_left) {
                if (ctx_menu_open) handle_context_click(mx, my);
                else handle_click(mx, my);
                need_redraw = true;
            }
            last_left = left;
        } else {
            last_left = false;
            last_right = false;
        }

        char c;
        if ((c = (char)syscall5(SYS_READ_CHAR, 0, 0, 0, 0, 0)) == 'q' || c == 'Q') {
            running = false;
        }

        if (need_redraw) redraw();
    }
}
