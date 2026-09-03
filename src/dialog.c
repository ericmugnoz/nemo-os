// dialog.c — Nemo OS
//
// Implementacion del dialogo comun de abrir/guardar. Reutiliza
// directamente las funciones del kernel (nemofs_*, wm_content_*,
// input_*) sin pasar por la frontera de syscalls -- al fin y al cabo,
// ya estamos dentro del kernel cuando esto se ejecuta.

#include "dialog.h"
#include "nemofs.h"
#include "wm.h"
#include "input.h"
#include "tasks.h"
#include "icons_data.h"

#define DLG_MAX_ENTRIES 6 // filas de datos visibles a la vez en pantalla
#define DLG_MAX_LOADED  64 // cuantas entradas cargamos en memoria como maximo -- antes coincidia
                            // con DLG_MAX_ENTRIES, asi que una carpeta con mas de 6 archivos
                            // simplemente NUNCA mostraba el resto. Ahora se cargan hasta 64 y
                            // se navegan con la rueda del raton (ver scroll_offset mas abajo).
#define DLG_ROW_H 26
#define DLG_STACK_MAX 8

#define COLOR_DLG_BG          0x00F0F0F0
#define COLOR_DLG_TEXT        0x00000000
#define COLOR_DLG_INPUT_BG    0x00202020
#define COLOR_DLG_INPUT_TEXT  0x00E0E0E0
#define COLOR_DLG_HINT        0x00707070

typedef struct {
    char name[NEMOFS_MAX_NAME + 1];
    uint8_t type;
} dlg_entry_t;

static bool geometry(int32_t win, int *wx_out, int *wy_out, int *dx, int *dy, int *dw, int *dh, bool with_input) {
    int32_t wx, wy;
    uint32_t ww, wh;
    if (!wm_get_window_client_rect(win, &wx, &wy, &ww, &wh)) return false;
    *wx_out = wx;
    *wy_out = wy;

    *dw = (int)((ww > 260) ? 260 : (ww > 20 ? ww - 20 : ww));
    *dh = 210 + (with_input ? 26 : 0);
    if (*dh > (int)wh - 10) *dh = (int)wh - 10;
    if (*dh < 60) *dh = 60;
    *dx = ((int)ww - *dw) / 2;
    *dy = ((int)wh - *dh) / 2;
    return true;
}

static void draw_frame(int32_t win, int dx, int dy, int dw, int dh, const char *title) {
    wm_content_fill_rect(win, (uint32_t)dx, (uint32_t)dy, (uint32_t)dw, (uint32_t)dh, COLOR_DLG_BG);
    wm_content_fill_rect(win, (uint32_t)dx, (uint32_t)dy, (uint32_t)dw, 1, 0x00000000);
    wm_content_draw_string(win, (uint32_t)dx + 8, (uint32_t)dy + 8, title, COLOR_DLG_TEXT, 1);
}

static int row_y(int dy, int row) {
    return dy + 24 + row * DLG_ROW_H;
}

static uint32_t load_dir(uint32_t dir, dlg_entry_t *entries) {
    static nemofs_dirent_t tmp[DLG_MAX_LOADED];
    uint32_t total = nemofs_list_dir(dir, tmp, DLG_MAX_LOADED);
    uint32_t shown = (total < DLG_MAX_LOADED) ? total : DLG_MAX_LOADED;
    for (uint32_t i = 0; i < shown; i++) {
        int j = 0;
        while (tmp[i].name[j] && j < NEMOFS_MAX_NAME) { entries[i].name[j] = tmp[i].name[j]; j++; }
        entries[i].name[j] = '\0';
        entries[i].type = (uint8_t)tmp[i].type;
    }
    return shown;
}

static void draw_entries(int32_t win, int dx, int dy, dlg_entry_t *entries, uint32_t count, int stack_depth, uint32_t scroll_offset) {
    int row = 0;
    if (stack_depth > 0) {
        int ry = row_y(dy, 0);
        wm_content_blit_icon(win, (uint32_t)(dx + 8), (uint32_t)(ry - 3), ICON_SIZE, icon_get_rgba(ICON_FOLDER));
        wm_content_draw_string(win, (uint32_t)(dx + 34), (uint32_t)ry, ".. (SUBIR)", COLOR_DLG_TEXT, 1);
        row = 1;
    }
    // Cuantas filas de DATOS caben tras la de ".. (SUBIR)" si la hay
    // -- el resto (si count es mayor) se alcanza con la rueda del
    // raton, ver scroll_offset.
    int max_rows = DLG_MAX_ENTRIES - row;
    int shown_here = 0;
    for (uint32_t i = scroll_offset; i < count && shown_here < max_rows; i++) {
        int ry = row_y(dy, row);
        int icon = (entries[i].type == NEMOFS_TYPE_DIR) ? ICON_FOLDER : ICON_TXT;
        wm_content_blit_icon(win, (uint32_t)(dx + 8), (uint32_t)(ry - 3), ICON_SIZE, icon_get_rgba(icon));
        wm_content_draw_string(win, (uint32_t)(dx + 34), (uint32_t)ry, entries[i].name, COLOR_DLG_TEXT, 1);
        row++;
        shown_here++;
    }
    if (count == 0 && stack_depth == 0) {
        wm_content_draw_string(win, (uint32_t)(dx + 8), (uint32_t)(dy + 24), "(CARPETA VACIA)", COLOR_DLG_TEXT, 1);
    }
}

// -3 = fuera del dialogo, -2 = fila "subir", -1 = ninguna fila, >=0 = indice
static int row_hit_test(int dx, int dy, int dw, int dh, int stack_depth, uint32_t count, uint32_t scroll_offset, int32_t mx, int32_t my) {
    if (mx < dx || mx >= dx + dw || my < dy || my >= dy + dh) return -3;
    int row = 0;
    if (stack_depth > 0) {
        int ry = row_y(dy, 0);
        if (my >= ry - 2 && my < ry - 2 + DLG_ROW_H) return -2;
        row = 1;
    }
    int max_rows = DLG_MAX_ENTRIES - row;
    int shown_here = 0;
    for (uint32_t i = scroll_offset; i < count && shown_here < max_rows; i++) {
        int ry = row_y(dy, row);
        if (my >= ry - 2 && my < ry - 2 + DLG_ROW_H) return (int)i;
        row++;
        shown_here++;
    }
    return -1;
}

int32_t dialog_open_file(int32_t win, uint32_t start_dir, char *out_name, uint32_t out_name_max) {
    uint32_t current_dir = start_dir;
    uint32_t stack[DLG_STACK_MAX];
    int stack_depth = 0;

    static dlg_entry_t entries[DLG_MAX_LOADED];
    uint32_t entry_count = load_dir(current_dir, entries);
    uint32_t scroll_offset = 0;

    bool last_left = false;

    while (1) {
        int wx, wy, dx, dy, dw, dh;
        if (!geometry(win, &wx, &wy, &dx, &dy, &dw, &dh, false)) return -1; // ventana cerrada

        int32_t cwx, cwy;
        uint32_t cww, cwh;
        if (wm_get_window_client_rect(win, &cwx, &cwy, &cww, &cwh)) {
            (void)cwx; (void)cwy;
            wm_content_fill_rect(win, 0, 0, cww, cwh, 0x00000000);
        }

        draw_frame(win, dx, dy, dw, dh, "ABRIR ARCHIVO (CLIC PARA ELEGIR)");
        draw_entries(win, dx, dy, entries, entry_count, stack_depth, scroll_offset);
        wm_content_draw_string(win, (uint32_t)(dx + 8), (uint32_t)(dy + dh - 16), "ESC CANCELA, RUEDA DESPLAZA", COLOR_DLG_HINT, 1);
        wm_request_redraw();

        task_yield();

        bool focused = (win == wm_get_focused_window());
        if (focused) {
            // Rueda del raton -- desplaza la lista, con margen para
            // que siempre quede al menos una fila visible al final.
            int32_t wheel = mouse_wheel_delta();
            if (wheel != 0) {
                uint32_t max_scroll = (entry_count > DLG_MAX_ENTRIES) ? entry_count - DLG_MAX_ENTRIES : 0;
                if (wheel < 0) {
                    scroll_offset += (uint32_t)(-wheel);
                    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
                } else {
                    uint32_t dec = (uint32_t)wheel;
                    scroll_offset = (dec > scroll_offset) ? 0 : scroll_offset - dec;
                }
            }

            bool left = mouse_left_down();
            if (left && !last_left) {
                int lx = mouse_x() - wx;
                int ly = mouse_y() - wy;
                int hit = row_hit_test(dx, dy, dw, dh, stack_depth, entry_count, scroll_offset, lx, ly);
                if (hit == -2 && stack_depth > 0) {
                    stack_depth--;
                    current_dir = stack[stack_depth];
                    entry_count = load_dir(current_dir, entries);
                    scroll_offset = 0;
                } else if (hit >= 0) {
                    if (entries[hit].type == NEMOFS_TYPE_DIR) {
                        int32_t child = nemofs_find_child(current_dir, entries[hit].name);
                        if (child >= 0 && stack_depth < DLG_STACK_MAX) {
                            stack[stack_depth++] = current_dir;
                            current_dir = (uint32_t)child;
                            entry_count = load_dir(current_dir, entries);
                            scroll_offset = 0;
                        }
                    } else {
                        int32_t inode = nemofs_find_child(current_dir, entries[hit].name);
                        if (inode >= 0) {
                            int j = 0;
                            while (entries[hit].name[j] && j < (int)out_name_max - 1) {
                                out_name[j] = entries[hit].name[j];
                                j++;
                            }
                            out_name[j] = '\0';
                            return inode;
                        }
                    }
                }
            }
            last_left = left;

            char c;
            if (input_read_char(&c) && c == 27) return -1; // Esc
        }
    }
}

int32_t dialog_save_file(int32_t win, uint32_t start_dir, char *out_name, uint32_t out_name_max) {
    uint32_t current_dir = start_dir;
    uint32_t stack[DLG_STACK_MAX];
    int stack_depth = 0;

    static dlg_entry_t entries[DLG_MAX_LOADED];
    uint32_t entry_count = load_dir(current_dir, entries);
    uint32_t scroll_offset = 0;

    char name_buf[NEMOFS_MAX_NAME + 1];
    int name_len = 0;
    name_buf[0] = '\0';

    bool last_left = false;

    while (1) {
        int wx, wy, dx, dy, dw, dh;
        if (!geometry(win, &wx, &wy, &dx, &dy, &dw, &dh, true)) return -1;

        int32_t cwx, cwy;
        uint32_t cww, cwh;
        if (wm_get_window_client_rect(win, &cwx, &cwy, &cww, &cwh)) {
            (void)cwx; (void)cwy;
            wm_content_fill_rect(win, 0, 0, cww, cwh, 0x00000000);
        }

        draw_frame(win, dx, dy, dw, dh, "GUARDAR COMO:");
        wm_content_fill_rect(win, (uint32_t)(dx + 8), (uint32_t)(dy + 22), (uint32_t)(dw - 16), 18, COLOR_DLG_INPUT_BG);
        wm_content_draw_string(win, (uint32_t)(dx + 12), (uint32_t)(dy + 27), name_buf, COLOR_DLG_INPUT_TEXT, 1);

        // Las entradas van desplazadas 26px hacia abajo para dejar
        // sitio a la caja de texto -- reutilizamos row_y con un origen
        // desplazado en vez de cambiar la constante global.
        int list_dy = dy + 22;
        draw_entries(win, dx, list_dy, entries, entry_count, stack_depth, scroll_offset);

        wm_content_draw_string(win, (uint32_t)(dx + 8), (uint32_t)(dy + dh - 16), "ENTER GUARDA, ESC CANCELA, RUEDA DESPLAZA", COLOR_DLG_HINT, 1);
        wm_request_redraw();

        task_yield();

        bool focused = (win == wm_get_focused_window());
        if (focused) {
            int32_t wheel = mouse_wheel_delta();
            if (wheel != 0) {
                uint32_t max_scroll = (entry_count > DLG_MAX_ENTRIES) ? entry_count - DLG_MAX_ENTRIES : 0;
                if (wheel < 0) {
                    scroll_offset += (uint32_t)(-wheel);
                    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
                } else {
                    uint32_t dec = (uint32_t)wheel;
                    scroll_offset = (dec > scroll_offset) ? 0 : scroll_offset - dec;
                }
            }

            bool left = mouse_left_down();
            if (left && !last_left) {
                int lx = mouse_x() - wx;
                int ly = mouse_y() - wy;
                int hit = row_hit_test(dx, list_dy, dw, dh - (list_dy - dy), stack_depth, entry_count, scroll_offset, lx, ly);
                if (hit == -2 && stack_depth > 0) {
                    stack_depth--;
                    current_dir = stack[stack_depth];
                    entry_count = load_dir(current_dir, entries);
                    scroll_offset = 0;
                } else if (hit >= 0) {
                    if (entries[hit].type == NEMOFS_TYPE_DIR) {
                        int32_t child = nemofs_find_child(current_dir, entries[hit].name);
                        if (child >= 0 && stack_depth < DLG_STACK_MAX) {
                            stack[stack_depth++] = current_dir;
                            current_dir = (uint32_t)child;
                            entry_count = load_dir(current_dir, entries);
                            scroll_offset = 0;
                        }
                    } else {
                        // Un archivo existente -- lo ponemos en la caja
                        // de texto, el usuario confirma con Enter.
                        int j = 0;
                        while (entries[hit].name[j] && j < NEMOFS_MAX_NAME) { name_buf[j] = entries[hit].name[j]; j++; }
                        name_buf[j] = '\0';
                        name_len = j;
                    }
                }
            }
            last_left = left;

            char c;
            if (input_read_char(&c)) {
                if (c == 27) {
                    return -1;
                } else if (c == '\n') {
                    if (name_len > 0) {
                        int32_t inode = nemofs_find_child(current_dir, name_buf);
                        if (inode < 0) inode = nemofs_create(current_dir, name_buf, NEMOFS_TYPE_FILE);
                        if (inode >= 0) {
                            int j = 0;
                            while (name_buf[j] && j < (int)out_name_max - 1) { out_name[j] = name_buf[j]; j++; }
                            out_name[j] = '\0';
                            return inode;
                        }
                    }
                } else if (c == '\b') {
                    if (name_len > 0) { name_len--; name_buf[name_len] = '\0'; }
                } else if (c >= 32 && c < 127 && name_len < NEMOFS_MAX_NAME) {
                    char up = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
                    name_buf[name_len++] = up;
                    name_buf[name_len] = '\0';
                }
            }
        }
    }
}
