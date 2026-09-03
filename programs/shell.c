// shell.c — Nemo OS
//
// La primera "aplicacion" de verdad de Nemo OS: una shell interactiva
// que corre como programa .pro normal, usando UNICAMENTE las syscalls
// del kernel -- ni una sola llamada directa a hardware ni a funciones
// del kernel por su nombre. Es exactamente el mismo contrato que
// tendra cualquier programa futuro (incluidos los que salgan del
// compilador propio de Nemo OS).
//
// Compilado freestanding (sin libc), igual que el propio kernel.

#include <stdint.h>
#include <stdbool.h>

// -- Numeros de syscall (deben coincidir con syscall.h del kernel) --
#define SYS_WRITE_STRING    11
#define SYS_READ_CHAR       12
#define SYS_READ_CHAR_WAIT  13
#define SYS_PUMP            14
#define SYS_LAUNCH_PROGRAM  5
#define SYS_GET_LAUNCH_ARG  6
#define SYS_READ_CONSOLE_OUTPUT 7
#define SYS_FILE_LIST       23
#define SYS_DRAW_RECT       30
#define SYS_DRAW_TEXT       31
#define SYS_GET_WINDOW_SIZE 33

#define DIR_ENTRY_SIZE 40
#define TYPE_DIR 2

#define VOLUME_NEMOFS 0
#define VOLUME_FAT    1

// Wrapper generico de syscall: numero en x8, argumentos en x0-x4,
// resultado en x0. Es la misma convencion que usa Linux en ARM64.
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

static void write_string(const char *s) {
    syscall5(SYS_WRITE_STRING, (uint64_t)s, 0, 0, 0, 0);
}
static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    syscall5(SYS_DRAW_RECT, (uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h, color);
}
static void draw_text(int x, int y, const char *s, uint32_t color) {
    syscall5(SYS_DRAW_TEXT, (uint64_t)x, (uint64_t)y, (uint64_t)s, color, 0);
}

// -- utilidades de cadenas, sin libc --
static bool str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

// Igual que str_eq, pero sin distinguir mayusculas de minusculas --
// la usamos para nombres de archivo/carpeta, porque las carpetas del
// sistema estan en MAYUSCULAS y la fuente de pantalla tambien
// convierte todo a mayusculas al mostrarlo (asi que visualmente
// parecen coincidir aunque los bytes reales sean distintos).
static char to_upper_ascii(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}
static bool str_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (to_upper_ascii(*a) != to_upper_ascii(*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

#define LINE_LEN 48
#define MAX_LINES 22

static char lines[MAX_LINES][LINE_LEN];
static int line_count;
static char input_buf[LINE_LEN];
static char console_line_buf[LINE_LEN];
static int console_line_len = 0;
static int input_len;
static int win_w, win_h;
static bool running;

// -- cursor posicionado (Locate) --
//
// Por defecto (cursor_row=-1) el texto que llega solo se acumula
// linea a linea, tal y como funcionaba siempre. Locate(x,y), desde el
// programa lanzado, manda una secuencia de 3 bytes por el MISMO canal
// de texto normal (marcador 0x01, luego x+1 e y+1 -- el +1 evita que
// una coordenada 0 se confunda con el byte de fin de cadena): al
// verla, activamos el modo posicionado y los caracteres que lleguen
// despues se escriben DIRECTAMENTE en esa fila/columna, sobreescribiendo
// lo que hubiera, hasta el siguiente salto de linea (que nos devuelve
// al modo normal de apilar lineas).
static int cursor_row = -1;
static int cursor_col = 0;
static bool esc_pending = false;
static int esc_stage = 0; // 0=esperando x+1, 1=esperando y+1
static int esc_x = 0;

static void write_positioned_char(char c) {
    if (cursor_row < 0 || cursor_row >= MAX_LINES) return;
    if (cursor_col < 0) cursor_col = 0;
    if (cursor_col >= LINE_LEN - 1) return; // no cabe mas en esta linea
    bool was_end = (lines[cursor_row][cursor_col] == '\0');
    lines[cursor_row][cursor_col] = c;
    cursor_col++;
    if (was_end) lines[cursor_row][cursor_col] = '\0'; // extendemos el final si escribiamos justo ahi
    if (cursor_row >= line_count) line_count = cursor_row + 1; // aseguramos que esta fila se vea
}

#define DIR_STACK_SIZE 16
static uint32_t current_dir;
static uint32_t dir_stack[DIR_STACK_SIZE];
static int dir_stack_top;
static uint32_t current_volume;

// Nombres de las carpetas del camino actual, en paralelo a
// dir_stack (que guarda los inodos) -- esto es lo que nos deja
// mostrar un prompt de verdad tipo "C:\DOCUMENTOS\DOCS>".
static char path_names[DIR_STACK_SIZE][28];
static int path_depth;

static void scroll_if_needed(void) {
    if (line_count >= MAX_LINES) {
        for (int i = 1; i < MAX_LINES; i++) {
            for (int j = 0; j < LINE_LEN; j++) lines[i - 1][j] = lines[i][j];
        }
        line_count = MAX_LINES - 1;
    }
}

static void print_line(const char *s) {
    scroll_if_needed();
    int i = 0;
    while (s[i] != '\0' && i < LINE_LEN - 1) {
        lines[line_count][i] = s[i];
        i++;
    }
    lines[line_count][i] = '\0';
    line_count++;
}

#define COLOR_BG     0x00101820
#define COLOR_TEXT   0x0000CC44
#define COLOR_PROMPT 0x0000FFCC

static void build_path_string(char *out, int max);

static void redraw_console(void) {
    draw_rect(0, 0, win_w, win_h, COLOR_BG);

    for (int i = 0; i < line_count; i++) {
        draw_text(4, 4 + i * 8, lines[i], COLOR_TEXT);
    }

    // linea de entrada actual, con prompt tipo "C:\CARPETA> "
    char prompt[40];
    build_path_string(prompt, sizeof(prompt));
    int plen = 0;
    while (prompt[plen] != '\0') plen++;

    char cur[LINE_LEN + 43];
    int p = 0;
    for (int k = 0; k < plen && p < (int)sizeof(cur) - 1; k++) cur[p++] = prompt[k];
    if (p < (int)sizeof(cur) - 1) cur[p++] = '>';
    if (p < (int)sizeof(cur) - 1) cur[p++] = ' ';
    int i = 0;
    while (input_buf[i] != '\0' && p < (int)sizeof(cur) - 1) {
        cur[p++] = input_buf[i];
        i++;
    }
    cur[p] = '\0';
    draw_text(4, 4 + line_count * 8, cur, COLOR_PROMPT);
}

// Copia hasta 'max_len' caracteres desde 'src' (que puede no estar
// terminado en \0 dentro de su propio campo de tamaño fijo) hasta
// encontrar un \0 o llegar al limite.
static void copy_bounded(char *dst, const char *src, int max_len) {
    int i = 0;
    while (src[i] != '\0' && i < max_len) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void cmd_ls(void) {
    static uint8_t raw[10 * DIR_ENTRY_SIZE];
    uint64_t total = syscall5(SYS_FILE_LIST, current_dir, (uint64_t)raw, 10, current_volume, 0);

    uint64_t shown = total < 10 ? total : 10;
    for (uint64_t i = 0; i < shown; i++) {
        uint8_t *e = raw + i * DIR_ENTRY_SIZE;
        uint32_t type = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);

        char line[LINE_LEN];
        int j = 0;
        while (e[12 + j] != 0 && j < LINE_LEN - 3) { line[j] = (char)e[12 + j]; j++; }
        if (type == TYPE_DIR) { line[j] = '/'; j++; }
        line[j] = '\0';
        print_line(line);
    }

    if (total == 0) print_line("(vacio)");
}

// Construye el texto de la ruta actual, estilo DOS: "C:\CARPETA\SUB".
// C: es NemoFS, F: es el disco FAT (el que se monta en el Mac) --
// nombres elegidos para que compilar archivos de BlitzPlus (que
// suelen vivir en C:) resulte comodo y familiar.
static void build_path_string(char *out, int max) {
    int pos = 0;
    const char *drive = (current_volume == VOLUME_NEMOFS) ? "C:" : "F:";
    int i = 0;
    while (drive[i] != '\0' && pos < max - 1) out[pos++] = drive[i++];
    if (pos < max - 1) out[pos++] = '\\';
    for (int d = 0; d < path_depth; d++) {
        int j = 0;
        while (path_names[d][j] != '\0' && pos < max - 1) out[pos++] = path_names[d][j++];
        if (d < path_depth - 1 && pos < max - 1) out[pos++] = '\\';
    }
    out[pos] = '\0';
}

// Busca 'name' entre las entradas de current_dir; si es una carpeta,
// entra en ella. Devuelve 'true' si tuvo exito.
static bool cmd_cd_into(const char *name) {
    static uint8_t raw[32 * DIR_ENTRY_SIZE];
    uint64_t total = syscall5(SYS_FILE_LIST, current_dir, (uint64_t)raw, 32, current_volume, 0);
    uint64_t shown = total < 32 ? total : 32;

    for (uint64_t i = 0; i < shown; i++) {
        uint8_t *e = raw + i * DIR_ENTRY_SIZE;
        uint32_t inode = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
        uint32_t type = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);

        char entry_name[LINE_LEN];
        copy_bounded(entry_name, (const char *)&e[12], 27);

        if (type == TYPE_DIR && str_eq_ci(entry_name, name)) {
            if (dir_stack_top < DIR_STACK_SIZE - 1) {
                dir_stack[dir_stack_top++] = current_dir;
                current_dir = inode;
                copy_bounded(path_names[path_depth], entry_name, 27);
                path_depth++;
                return true;
            }
        }
    }
    return false;
}

static void cmd_cd(const char *arg) {
    if (current_volume == VOLUME_FAT) {
        print_line("FAT no tiene subcarpetas todavia. Usa 'nemofs' para volver.");
        return;
    }
    if (arg[0] == '\0') {
        print_line("Uso: cd <carpeta>  (o 'cd ..' para subir)");
        return;
    }
    if (str_eq(arg, "..")) {
        if (dir_stack_top > 0) {
            current_dir = dir_stack[--dir_stack_top];
            if (path_depth > 0) path_depth--;
        } else {
            print_line("Ya estas en la raiz.");
        }
        return;
    }
    if (!cmd_cd_into(arg)) {
        print_line("No se encontro esa carpeta.");
    }
}

static void cmd_disk(const char *arg) {
    if (str_eq(arg, "fat")) {
        current_volume = VOLUME_FAT;
        current_dir = 0;
        dir_stack_top = 0;
        path_depth = 0;
        print_line("Cambiado al disco FAT (el que se monta en el Mac).");
    } else if (str_eq(arg, "nemofs")) {
        current_volume = VOLUME_NEMOFS;
        current_dir = 0;
        dir_stack_top = 0;
        path_depth = 0;
        print_line("Cambiado a NemoFS.");
    } else {
        print_line("Uso: disco fat | disco nemofs");
    }
}

// Cambio de disco al estilo DOS: escribir "C:" o "F:" solos cambia de
// disco directamente, sin necesidad de "disco fat"/"disco nemofs".
static void switch_drive(uint32_t volume) {
    current_volume = volume;
    current_dir = 0;
    dir_stack_top = 0;
    path_depth = 0;
}

// Pide al kernel que lance un .pro por nombre -- lo busca primero en
// la raiz (donde suelen aterrizar los archivos copiados desde FAT) y
// si no, en PROGRAMAS. No hace falta que este ya "instalado": basta
// con que sea un archivo real en el disco.
static void cmd_run(const char *name_and_arg) {
    if (name_and_arg[0] == '\0') {
        print_line("Uso: run <archivo.pro> [argumento]");
        return;
    }
    // Separamos el nombre del programa del resto (si hay un espacio,
    // todo lo que venga despues es el argumento que le pasamos --
    // por ejemplo, "run nbc.pro ejemplo.bb" lanza nbc.pro con
    // "ejemplo.bb" como argumento de lanzamiento).
    char pro_name[32];
    int i = 0;
    while (name_and_arg[i] != '\0' && name_and_arg[i] != ' ' && i < 31) {
        pro_name[i] = name_and_arg[i];
        i++;
    }
    pro_name[i] = '\0';
    const char *extra_arg = (name_and_arg[i] == ' ') ? &name_and_arg[i + 1] : "";

    syscall5(SYS_LAUNCH_PROGRAM, (uint64_t)pro_name, (uint64_t)extra_arg, (uint64_t)current_dir, 0, 0);
}

static void run_command(const char *cmd) {
    if (cmd[0] == '\0') {
        return;
    } else if (str_eq(cmd, "help")) {
        print_line("Comandos: help, about, clear, exit, ls, cd, run, C:, F:");
    } else if (str_eq(cmd, "about")) {
        print_line("Nemo OS Shell v0.1 -- corre via syscalls");
    } else if (str_eq(cmd, "clear")) {
        line_count = 0;
    } else if (str_eq(cmd, "exit")) {
        print_line("Cerrando shell...");
        running = false;
    } else if (str_eq(cmd, "ls")) {
        cmd_ls();
    } else if (cmd[0] == 'c' && cmd[1] == 'd' && (cmd[2] == ' ' || cmd[2] == '\0')) {
        const char *arg = (cmd[2] == ' ') ? &cmd[3] : &cmd[2];
        cmd_cd(arg);
    } else if (cmd[0] == 'd' && cmd[1] == 'i' && cmd[2] == 's' && cmd[3] == 'c' && cmd[4] == 'o' && cmd[5] == ' ') {
        cmd_disk(&cmd[6]);
    } else if (cmd[0] == 'r' && cmd[1] == 'u' && cmd[2] == 'n' && (cmd[3] == ' ' || cmd[3] == '\0')) {
        const char *arg = (cmd[3] == ' ') ? &cmd[4] : &cmd[3];
        cmd_run(arg);
    } else if ((cmd[0] == 'c' || cmd[0] == 'C') && cmd[1] == ':' && cmd[2] == '\0') {
        switch_drive(VOLUME_NEMOFS);
    } else if ((cmd[0] == 'f' || cmd[0] == 'F') && cmd[1] == ':' && cmd[2] == '\0') {
        switch_drive(VOLUME_FAT);
    } else {
        print_line("Comando desconocido. Prueba 'help'.");
    }
}

__attribute__((section(".text.start")))
void _start(void) {
    // Inicializacion explicita de todo el estado -- no confiamos en
    // que la memoria venga a cero solo porque son variables globales
    // (ver el comentario sobre .bss en hello_linker.ld para el porque).
    line_count = 0;
    input_len = 0;
    input_buf[0] = '\0';
    console_line_len = 0;
    console_line_buf[0] = '\0';
    running = true;
    current_dir = 0; // NEMOFS_ROOT_INODE
    dir_stack_top = 0;
    current_volume = VOLUME_NEMOFS;
    path_depth = 0;
    cursor_row = -1;
    cursor_col = 0;
    esc_pending = false;
    esc_stage = 0;
    esc_x = 0;

    uint64_t size = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
    win_w = (int)(size >> 32);
    win_h = (int)(size & 0xFFFFFFFF);
    if (win_w <= 0) win_w = 300;
    if (win_h <= 0) win_h = 180;

    write_string("shell: iniciada.\n");

    print_line("Nemo OS Shell");
    print_line("Escribe 'help' para ver los comandos.");
    print_line("");
    redraw_console();

    // Si nos lanzaron con un comando de arranque (por ejemplo, el
    // IDE nos abre asi tras compilar: "run miprograma.pro"), lo
    // ejecutamos automaticamente, como si el usuario lo hubiera
    // escrito el mismo -- asi el propio eco del comando y su salida
    // quedan integrados en el historial normal de la shell.
    {
        static char launch_cmd[64];
        uint64_t alen = syscall5(SYS_GET_LAUNCH_ARG, (uint64_t)launch_cmd, sizeof(launch_cmd), 0, 0, 0);
        if (alen > 0) {
            char with_prompt[LINE_LEN + 3];
            with_prompt[0] = '>'; with_prompt[1] = ' ';
            int i = 0;
            while (launch_cmd[i] && i < LINE_LEN - 3) { with_prompt[2 + i] = launch_cmd[i]; i++; }
            with_prompt[2 + i] = '\0';
            print_line(with_prompt);
            run_command(launch_cmd);
            redraw_console();
        }
    }

    while (running) {
        // A diferencia de antes, ya NO esperamos aqui bloqueados a
        // que llegue una tecla -- necesitamos poder comprobar tambien
        // si algun programa lanzado con 'run' tiene salida pendiente,
        // asi que cedemos el control una vez por vuelta y miramos las
        // dos cosas cada vez (como hace el explorador).
        uint64_t pump = syscall5(SYS_PUMP, 0, 0, 0, 0, 0);
        if ((int64_t)pump < 0) {
            // La ventana se cerro desde fuera (boton X).
            running = false;
            break;
        }

        // Volvemos a consultar el tamaño por si la ventana cambio
        // (maximizar, redimensionar) desde la ultima vuelta.
        uint64_t sz = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
        int new_w = (int)(sz >> 32);
        int new_h = (int)(sz & 0xFFFFFFFF);
        bool need_redraw = (new_w > 0 && new_w != win_w) || (new_h > 0 && new_h != win_h);
        if (new_w > 0) win_w = new_w;
        if (new_h > 0) win_h = new_h;

        // -- salida de un programa lanzado con 'run', si lo hay --
        // Se va acumulando caracter a caracter hasta un salto de
        // linea, y entonces se vuelca como si fuera una linea mas de
        // la propia shell -- asi el resultado se integra en el
        // historial sin que nadie tenga que dibujar por encima de
        // nadie.
        char cc;
        while ((cc = (char)syscall5(SYS_READ_CONSOLE_OUTPUT, 0, 0, 0, 0, 0)) != 0) {
            if (esc_pending) {
                if (esc_stage == 0) {
                    esc_x = (uint8_t)cc - 1; // deshacemos el +1 que aplico rt_locate
                    esc_stage = 1;
                } else {
                    cursor_col = esc_x;
                    cursor_row = (uint8_t)cc - 1;
                    esc_pending = false;
                    esc_stage = 0;
                }
                need_redraw = true;
                continue;
            }
            if (cc == '\x01') { // marcador de Locate -- los proximos 2 bytes son x+1, y+1
                esc_pending = true;
                esc_stage = 0;
                continue;
            }
            if (cc == '\n') {
                if (cursor_row >= 0) {
                    // veniamos de un Locate -- un salto de linea nos
                    // devuelve al modo normal de apilar lineas.
                    cursor_row = -1;
                    cursor_col = 0;
                } else {
                    print_line(console_line_buf);
                    console_line_len = 0;
                    console_line_buf[0] = '\0';
                }
            } else if (cursor_row >= 0) {
                write_positioned_char(cc);
            } else if (console_line_len < LINE_LEN - 1) {
                console_line_buf[console_line_len++] = cc;
                console_line_buf[console_line_len] = '\0';
            }
            need_redraw = true;
        }

        // -- teclado, ahora sin bloquear -- puede que no haya ninguna
        // tecla pendiente en esta vuelta, y eso es normal.
        char c;
        while ((c = (char)syscall5(SYS_READ_CHAR, 0, 0, 0, 0, 0)) != 0) {
            if (c == '\n') {
                char prompt[40];
                build_path_string(prompt, sizeof(prompt));
                int plen = 0;
                while (prompt[plen] != '\0') plen++;

                char with_prompt[LINE_LEN + 43];
                int p = 0;
                for (int k = 0; k < plen && p < (int)sizeof(with_prompt) - 1; k++) with_prompt[p++] = prompt[k];
                if (p < (int)sizeof(with_prompt) - 1) with_prompt[p++] = '>';
                if (p < (int)sizeof(with_prompt) - 1) with_prompt[p++] = ' ';
                int i = 0;
                while (input_buf[i] != '\0' && p < (int)sizeof(with_prompt) - 1) {
                    with_prompt[p++] = input_buf[i];
                    i++;
                }
                with_prompt[p] = '\0';
                print_line(with_prompt);

                run_command(input_buf);

                input_len = 0;
                input_buf[0] = '\0';
            } else if (c == '\b') {
                if (input_len > 0) {
                    input_len--;
                    input_buf[input_len] = '\0';
                }
            } else {
                if (input_len < LINE_LEN - 1) {
                    input_buf[input_len] = c;
                    input_len++;
                    input_buf[input_len] = '\0';
                }
            }
            need_redraw = true;
        }

        if (need_redraw) redraw_console();
    }

    write_string("shell: terminada.\n");
}
