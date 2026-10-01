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
//
// MEJORAS de esta revision:
//   - Fuente del doble de tamaño (SetFont a escala 2), consultando el
//     ancho/alto REAL con FontWidth()/FontHeight() en vez de asumir 5x7.
//   - Ajuste de linea (word-wrap): cualquier linea, tanto lo que
//     escribes como la salida de comandos, que no quepa en el ancho
//     actual de la ventana pasa sola a la fila de abajo -- ya no hay
//     un limite fijo de 48 columnas ni texto que se corte sin avisar.
//   - Scrollback consciente del tamaño real de la ventana: se calcula
//     en cada redibujado cuantas lineas (con su wrap) caben verticalmente
//     y se muestran las mas recientes; redimensionar la ventana ajusta
//     cuanto historial se ve, sin perder nada de lo guardado.
//   - Cursor intermitente (tipo MS-DOS/Linux) al final de la linea de
//     entrada, parpadeando por tiempo real (SYS_GET_TICKS), no solo
//     cuando hay una tecla nueva.
//   - Comandos nuevos: cls, pwd, mkdir, del/rm, cat/type, echo; ls
//     ahora muestra el tamaño de cada archivo.

#include <stdint.h>
#include <stdbool.h>

// -- Numeros de syscall (deben coincidir con syscall.h del kernel) --
#define SYS_GET_TICKS       2
#define SYS_WRITE_STRING    11
#define SYS_READ_CHAR       12
#define SYS_READ_CHAR_WAIT  13
#define SYS_PUMP            14
#define SYS_LAUNCH_PROGRAM  5
#define SYS_CONSOLE_BUSY    259
#define SYS_GET_LAUNCH_ARG  6
#define SYS_READ_CONSOLE_OUTPUT 7
#define SYS_FILE_OPEN       20
#define SYS_FILE_CLOSE      274  // soltar el hueco: en FAT solo hay ocho en todo el sistema
#define SYS_FILE_READ       21
#define SYS_FILE_LIST       23
#define SYS_DIR_CREATE      24
#define SYS_FILE_DELETE     25
#define SYS_DRAW_RECT       30
#define SYS_DRAW_TEXT       31
#define SYS_GET_WINDOW_SIZE 33
#define SYS_LOAD_FONT        189
#define SYS_SET_FONT         191
#define SYS_FONT_HEIGHT      196
#define SYS_FONT_CHAR_ADVANCE 206
#define SYS_CREATE_WINDOW    40
#define SYS_POLL_EVENT       8
// Red. La 309 escribe en texto la IP, la mascara, la
// pasarela o el DNS segun el selector; la 310 dice si la red esta en
// pie. Hacen falta aqui porque la direccion de la placa YA NO ES FIJA
// --se calcula de la MAC cuando nadie reparte IPs-- asi que la unica
// forma de saberla era leer el log del arranque por el puerto serie.
// Y ese log no se ve desde la propia maquina.
#define SYS_NET_LOCAL       309
#define SYS_NET_LISTA       310
#define EVENT_WINDOWCLOSE    0x803

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
static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}
static void put_dec(char *out, int *pos, int max, uint32_t value) {
    char tmp[12];
    int n = 0;
    if (value == 0) tmp[n++] = '0';
    while (value > 0 && n < 11) { tmp[n++] = (char)('0' + value % 10); value /= 10; }
    while (n > 0 && *pos < max - 1) out[(*pos)++] = tmp[--n];
}

// LINE_LEN ya no esta atado a ninguna anchura de ventana concreta --
// solo limita cuanto se puede guardar/escribir de una sola vez.
// MAX_LINES es el scrollback maximo guardado; cuantas de esas lineas
// se VEN de verdad depende del tamaño actual de la ventana (mas abajo).
#define LINE_LEN 200
#define MAX_LINES 300

static char lines[MAX_LINES][LINE_LEN];
static int line_count;
static char input_buf[LINE_LEN];
static char console_line_buf[LINE_LEN];
static int console_line_len = 0;
static int input_len;
static int win_w, win_h;
static bool running;

// Metricas reales de la fuente activa (tras SetFont), consultadas una
// vez al arrancar -- nunca asumimos 5x7 a partir de aqui.
static int char_w, char_h;

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

// Descarta las lineas mas antiguas del scrollback cuando se supera el
// limite de ALMACENAMIENTO (no el de pantalla -- eso se calcula aparte
// en cada redibujado, ver compute_first_visible). Esto solo protege la
// memoria ante un historial que crece sin fin.
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
#define COLOR_CURSOR 0x0000FFCC
#define MARGIN 4

static void build_path_string(char *out, int max);

// -- Ajuste de linea (word-wrap) y scrollback consciente del tamaño --
//
// No partimos por palabras (esto es una consola de comandos, no un
// procesador de texto): cortamos por columnas, como hace cualquier
// terminal real cuando una linea es mas larga que el ancho disponible.

static int cols_per_row(void) {
    int cw = (char_w > 0) ? char_w : 1;
    int c = (win_w - 2 * MARGIN) / cw;
    return (c < 8) ? 8 : c;
}

static int rows_for_len(int len) {
    if (len <= 0) return 1;
    int cols = cols_per_row();
    return (len + cols - 1) / cols;
}

// Dibuja 's' ajustado a la anchura actual, empezando en y0. Si
// 'show_cursor' es true, dibuja ademas un cursor intermitente justo
// despues del ultimo caracter (en una fila nueva si la anterior
// quedo exactamente llena, como en cualquier terminal). Devuelve la
// coordenada Y siguiente, para encadenar mas lineas debajo.
static int draw_wrapped_ex(const char *s, int y0, uint32_t color, bool show_cursor, bool cursor_on) {
    int cols = cols_per_row();
    int len = str_len(s);
    static char chunk[LINE_LEN];
    int y = y0;
    int pos = 0;
    int last_n = 0;
    if (len == 0) {
        draw_text(MARGIN, y, "", color);
        last_n = 0;
    } else {
        while (pos < len) {
            int n = len - pos;
            if (n > cols) n = cols;
            for (int i = 0; i < n; i++) chunk[i] = s[pos + i];
            chunk[n] = '\0';
            draw_text(MARGIN, y, chunk, color);
            last_n = n;
            pos += n;
            if (pos < len) y += char_h;
        }
    }
    if (show_cursor && cursor_on) {
        int cx, cy;
        if (last_n >= cols) { cx = MARGIN; cy = y + char_h; }
        else { cx = MARGIN + last_n * char_w; cy = y; }
        draw_rect(cx, cy, char_w > 1 ? char_w - 1 : char_w, char_h, COLOR_CURSOR);
    }
    return y + char_h;
}

static int draw_wrapped(const char *s, int y0, uint32_t color) {
    return draw_wrapped_ex(s, y0, color, false, false);
}

// Cuantas filas de pantalla ocupa 'lines[idx]' una vez ajustada.
static int rows_for_stored_line(int idx) {
    return rows_for_len(str_len(lines[idx]));
}

// Decide desde que linea del historial empezar a dibujar para que
// todo (historial + linea de entrada, ya ajustados) quepa en el alto
// actual de la ventana, mostrando siempre lo MAS RECIENTE.
static int compute_first_visible(int input_rows) {
    int ch = (char_h > 0) ? char_h : 1;
    int avail_rows = (win_h - 2 * MARGIN) / ch;
    int budget = avail_rows - input_rows;
    if (budget < 1) budget = 1;
    int used = 0;
    int i = line_count;
    while (i > 0) {
        int r = rows_for_stored_line(i - 1);
        if (used + r > budget) break;
        used += r;
        i--;
    }
    return i;
}

// Parpadeo del cursor por tiempo real: medio segundo encendido, medio
// apagado (misma cadencia que una terminal Linux o el propio MS-DOS).
#define BLINK_TICKS 50 // ticks de 10ms -> 500ms
static bool cursor_blink_on(void) {
    uint64_t ticks = syscall5(SYS_GET_TICKS, 0, 0, 0, 0, 0);
    return ((ticks / BLINK_TICKS) & 1) == 0;
}

static void redraw_console(void) {
    draw_rect(0, 0, win_w, win_h, COLOR_BG);

    // Linea de entrada (prompt + lo escrito) como una unica cadena --
    // asi el cursor al final se calcula exactamente igual que para
    // cualquier otra linea ajustada.
    //
    // Mientras un programa lanzado desde aqui esta pidiendo datos al
    // usuario, la shell ESCONDE su prompt: si no, se ven a la vez la
    // pregunta del programa y el "C:\>" de la shell, y no hay forma
    // de saber quien espera que escribas. La shell sigue viva y
    // redibujando; solo calla mientras no le toca.
    bool otro_pide_datos = (syscall5(SYS_CONSOLE_BUSY, 0, 0, 0, 0, 0) != 0);

    char prompt[40];
    build_path_string(prompt, sizeof(prompt));
    static char cur[LINE_LEN + 43];
    int p = 0;
    if (!otro_pide_datos) {
        int plen = str_len(prompt);
        for (int k = 0; k < plen && p < (int)sizeof(cur) - 1; k++) cur[p++] = prompt[k];
        if (p < (int)sizeof(cur) - 1) cur[p++] = '>';
        if (p < (int)sizeof(cur) - 1) cur[p++] = ' ';
        int i = 0;
        while (input_buf[i] != '\0' && p < (int)sizeof(cur) - 1) cur[p++] = input_buf[i++];
    }
    cur[p] = '\0';

    int input_rows = rows_for_len(str_len(cur));
    int first = compute_first_visible(input_rows);

    int y = MARGIN;
    for (int li = first; li < line_count; li++) {
        y = draw_wrapped(lines[li], y, COLOR_TEXT);
    }

    // La linea de salida que AUN no ha terminado (un programa que
    // escribio sin llegar a poner un salto de linea todavia).
    //
    // Antes no se dibujaba: se guardaba en console_line_buf y solo
    // aparecia al llegar el '\n'. Con Print eso casi no se nota,
    // pero con el eco de Input$ era muy visible -- escribias y no
    // veias nada hasta pulsar Enter, cuando salia todo de golpe.
    if (console_line_buf[0] != '\0') {
        y = draw_wrapped(console_line_buf, y, COLOR_TEXT);
    }
    // El cursor tampoco se dibuja mientras otro pide datos: sin
    // prompt delante, quedaria un bloque parpadeando solo, que
    // confunde mas todavia.
    draw_wrapped_ex(cur, y, COLOR_PROMPT, true, !otro_pide_datos && cursor_blink_on());
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
    static uint8_t raw[32 * DIR_ENTRY_SIZE];
    uint64_t total = syscall5(SYS_FILE_LIST, current_dir, (uint64_t)raw, 32, current_volume, 0);

    uint64_t shown = total < 32 ? total : 32;
    for (uint64_t idx = 0; idx < shown; idx++) {
        uint8_t *e = raw + idx * DIR_ENTRY_SIZE;
        uint32_t type = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
        uint32_t size = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);

        char line[LINE_LEN];
        int j = 0;
        while (e[12 + j] != 0 && j < 27) { line[j] = (char)e[12 + j]; j++; }
        // Alineamos igual que un "dir" clasico: nombre, luego tamaño
        // o "<DIR>", con algo de relleno para que quede en columnas.
        while (j < 20) line[j++] = ' ';
        if (type == TYPE_DIR) {
            line[j++] = '<'; line[j++] = 'D'; line[j++] = 'I'; line[j++] = 'R'; line[j++] = '>';
            line[j] = '\0';
        } else {
            put_dec(line, &j, LINE_LEN, size);
            line[j++] = 'B'; line[j] = '\0';
        }
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

// Busca 'name' entre las entradas de current_dir. Si 'want_type' es
// -1 acepta cualquier tipo; si no, solo esa. Devuelve el inodo (o -1)
// y, si se pide, el tamaño.
static int32_t find_in_current_dir(const char *name, int want_type, uint32_t *out_size) {
    // 256 entradas: antes 32, y en una carpeta llena (DOCUMENTOS)
    // 'run' o 'cd' no encontraban lo que estaba de la 33 en adelante
    static uint8_t raw[256 * DIR_ENTRY_SIZE];
    uint64_t total = syscall5(SYS_FILE_LIST, current_dir, (uint64_t)raw, 256, current_volume, 0);
    uint64_t shown = total < 256 ? total : 256;

    for (uint64_t idx = 0; idx < shown; idx++) {
        uint8_t *e = raw + idx * DIR_ENTRY_SIZE;
        uint32_t inode = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
        uint32_t type = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
        uint32_t size = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        char entry_name[LINE_LEN];
        copy_bounded(entry_name, (const char *)&e[12], 27);
        if (str_eq_ci(entry_name, name) && (want_type < 0 || (int)type == want_type)) {
            if (out_size) *out_size = size;
            return (int32_t)inode;
        }
    }
    return -1;
}

// Busca 'name' entre las entradas de current_dir; si es una carpeta,
// entra en ella. Devuelve 'true' si tuvo exito.
static bool cmd_cd_into(const char *name) {
    uint32_t dummy;
    int32_t inode = find_in_current_dir(name, TYPE_DIR, &dummy);
    if (inode < 0) return false;
    if (dir_stack_top < DIR_STACK_SIZE - 1) {
        dir_stack[dir_stack_top++] = current_dir;
        current_dir = (uint32_t)inode;
        copy_bounded(path_names[path_depth], name, 27);
        path_depth++;
        return true;
    }
    return false;
}

static void cmd_cd(const char *arg) {
    if (current_volume == VOLUME_FAT) {
        print_line("FAT no tiene subcarpetas todavia. Usa 'C:' para volver.");
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
        print_line("Cambiado al disco FAT (SD o pendrive, segun lo que haya montado).");
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
// ---------------------------------------------------------------------
// ip -- la configuracion de red de esta maquina
//
// POR QUE HACE FALTA. La direccion de la placa ya no esta escrita en
// ningun sitio: con router la da el DHCP, y sin router se calcula a
// partir de la MAC para que dos placas unidas por un cable no cojan la
// misma. El arranque la anuncia por el puerto serie, pero ese log no se
// ve desde la propia maquina -- y quien esta delante de la pantalla de
// Nemo OS no tiene forma de saber a que direccion llamarle desde fuera.
//
// Sin esto, la respuesta a "cual es mi IP?" era reiniciar con un cable
// serie conectado. Con esto son dos letras.
// ---------------------------------------------------------------------
static void anadir_campo(char *linea, int *pos, int max,
                         const char *etiqueta, uint32_t cual) {
    char valor[24];
    // El kernel pide 16 bytes como minimo (una IP puede medir 15 letras
    // y su cero); con menos no escribe nada en vez de cortar, porque una
    // IP recortada es otra IP con pinta de buena.
    uint32_t n = (uint32_t)syscall5(SYS_NET_LOCAL, (uint64_t)(uintptr_t)valor,
                                    sizeof valor, cual, 0, 0);
    if (n == 0) copy_bounded(valor, "?", sizeof valor - 1);

    int k = 0;
    while (etiqueta[k] && *pos < max - 1) linea[(*pos)++] = etiqueta[k++];
    k = 0;
    while (valor[k] && *pos < max - 1) linea[(*pos)++] = valor[k++];
    linea[*pos] = '\0';
}

static void cmd_ip(void) {
    char linea[LINE_LEN];
    int pos;

    if (syscall5(SYS_NET_LISTA, 0, 0, 0, 0, 0) == 0) {
        print_line("La red no esta lista: no hay enlace o todavia no hay direccion.");
        print_line("Con cable puesto tarda unos segundos en pedirla al router.");
        return;
    }

    pos = 0;
    anadir_campo(linea, &pos, (int)sizeof linea, "IP:       ", 0);
    print_line(linea);

    pos = 0;
    anadir_campo(linea, &pos, (int)sizeof linea, "Mascara:  ", 1);
    print_line(linea);

    // La pasarela y el DNS vienen a 0.0.0.0 cuando no hay router. No es
    // un error y conviene decirlo, porque "0.0.0.0" a secas parece uno:
    // con un cable directo entre dos maquinas no existen y no hacen
    // falta para hablar con la del otro extremo.
    char pasarela[24];
    uint32_t n = (uint32_t)syscall5(SYS_NET_LOCAL, (uint64_t)(uintptr_t)pasarela,
                                     sizeof pasarela, 2, 0, 0);
    bool sin_router = (n == 0) || str_eq(pasarela, "0.0.0.0");

    pos = 0;
    anadir_campo(linea, &pos, (int)sizeof linea, "Pasarela: ", 2);
    print_line(linea);

    pos = 0;
    anadir_campo(linea, &pos, (int)sizeof linea, "DNS:      ", 3);
    print_line(linea);

    if (sin_router) {
        print_line("Sin pasarela: no hay router. La direccion sale de la MAC,");
        print_line("y sirve para hablar con otra maquina por el mismo cable.");
    }
}

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
// ---- Empezar en una carpeta por su numero ----
// El IDE y el explorador abren la shell con "@<carpeta> run programa.pro"
// para los programas de consola: antes la shell empezaba siempre en la raiz y
// no encontraba un programa guardado en otra carpeta ("archivo no
// encontrado"). La ruta de nombres (la del aviso C:\DOCUMENTOS\...>) se
// reconstruye buscando la carpeta desde la raiz.
static uint8_t raw_ruta[DIR_STACK_SIZE][128 * DIR_ENTRY_SIZE];
static bool buscar_ruta(uint32_t carpeta, uint32_t objetivo, int nivel) {
    if (nivel >= DIR_STACK_SIZE - 1) return false;
    uint64_t total = syscall5(SYS_FILE_LIST, carpeta, (uint64_t)raw_ruta[nivel], 128, VOLUME_NEMOFS, 0);
    uint64_t n = total < 128 ? total : 128;
    for (uint64_t k = 0; k < n; k++) {
        uint8_t *e = raw_ruta[nivel] + k * DIR_ENTRY_SIZE;
        uint32_t inode = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
        uint32_t type = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
        if (type != TYPE_DIR) continue;
        copy_bounded(path_names[nivel], (const char *)&e[12], 27);
        dir_stack[nivel] = carpeta;
        if (inode == objetivo) { dir_stack_top = nivel + 1; path_depth = nivel + 1; current_dir = objetivo; return true; }
        if (buscar_ruta(inode, objetivo, nivel + 1)) return true;
    }
    return false;
}
static void situarse_en(uint32_t carpeta) {
    current_volume = VOLUME_NEMOFS;
    current_dir = 0; dir_stack_top = 0; path_depth = 0;
    if (carpeta != 0 && !buscar_ruta(0, carpeta, 0)) { current_dir = 0; dir_stack_top = 0; path_depth = 0; }
}

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

static void cmd_mkdir(const char *name) {
    if (name[0] == '\0') { print_line("Uso: mkdir <nombre>"); return; }
    if (current_volume == VOLUME_FAT) { print_line("FAT no admite crear carpetas todavia."); return; }
    int64_t r = (int64_t)syscall5(SYS_DIR_CREATE, (uint64_t)name, current_dir, current_volume, 0, 0);
    print_line(r >= 0 ? "Carpeta creada." : "No se pudo crear la carpeta.");
}

static void cmd_del(const char *name) {
    if (name[0] == '\0') { print_line("Uso: del <nombre>  (o rm <nombre>)"); return; }
    int64_t r = (int64_t)syscall5(SYS_FILE_DELETE, (uint64_t)name, current_dir, current_volume, 0, 0);
    print_line(r == 0 ? "Borrado." : "No se encontro ese archivo.");
}

// cat/type: imprime el contenido de un archivo de texto, linea a
// linea (cada '\n' del archivo se convierte en una linea propia de la
// consola, que a su vez se ajusta sola si es mas ancha que la
// ventana). Comprobamos antes que el archivo existe de verdad --
// SYS_FILE_OPEN crearia uno vacio si no, y eso seria sorprendente
// para un comando de solo lectura.
static void cmd_cat(const char *name) {
    if (name[0] == '\0') { print_line("Uso: cat <archivo>  (o type <archivo>)"); return; }
    int32_t inode = find_in_current_dir(name, 0 /* TYPE_FILE */, 0);
    if (inode < 0) { print_line("No se encontro ese archivo."); return; }

    int64_t handle = (int64_t)syscall5(SYS_FILE_OPEN, (uint64_t)name, current_dir, current_volume, 0, 0);
    if (handle < 0) { print_line("No se pudo abrir el archivo."); return; }

    static uint8_t buf[8192];
    int64_t got = (int64_t)syscall5(SYS_FILE_READ, (uint64_t)handle, (uint64_t)buf, sizeof(buf) - 1, current_volume, 0);
    // Cerrar AQUI, con el contenido ya leido: lo que viene despues es
    // partir el texto en lineas y pintarlo, y no necesita el archivo.
    // Los huecos de archivo abierto de FAT son OCHO en todo el sistema y
    // no se sueltan hasta que muere el programa que los pidio -- una
    // shell abierta un rato haciendo 'cat' de archivos de la tarjeta los
    // agotaba, y a partir de ahi nadie podia abrir nada.
    syscall5(SYS_FILE_CLOSE, (uint64_t)handle, current_volume, 0, 0, 0);
    if (got < 0) got = 0;
    buf[got] = 0;

    char line[LINE_LEN];
    int j = 0;
    for (int64_t k = 0; k < got; k++) {
        char c = (char)buf[k];
        if (c == '\n' || j >= LINE_LEN - 1) {
            line[j] = '\0';
            print_line(line);
            j = 0;
            if (c != '\n') line[j++] = c; // el caracter que hizo desbordar no se pierde
        } else if (c != '\r') {
            line[j++] = c;
        }
    }
    if (j > 0) { line[j] = '\0'; print_line(line); }
}

static void run_command(const char *cmd) {
    if (cmd[0] == '\0') {
        return;
    } else if (str_eq(cmd, "help")) {
        print_line("Navegacion: ls, cd <carpeta>, cd .., pwd, C:, F:, disco fat|nemofs");
        print_line("Archivos:   mkdir <n>, del/rm <n>, cat/type <n>, echo <texto>");
        print_line("Programas:  run <archivo.pro> [arg]");
        print_line("Red:        ip (o 'red'): la direccion de esta maquina");
        print_line("Otros:      about, clear/cls, exit");
    } else if (str_eq(cmd, "about")) {
        print_line("Nemo OS Shell -- corre via syscalls, sin acceso directo al hardware");
    } else if (str_eq(cmd, "clear") || str_eq(cmd, "cls")) {
        line_count = 0;
    } else if (str_eq(cmd, "exit")) {
        print_line("Cerrando shell...");
        running = false;
    } else if (str_eq(cmd, "ip") || str_eq(cmd, "red")) {
        cmd_ip();
    } else if (str_eq(cmd, "ls")) {
        cmd_ls();
    } else if (str_eq(cmd, "pwd")) {
        char p[40];
        build_path_string(p, sizeof(p));
        print_line(p);
    } else if (cmd[0] == 'c' && cmd[1] == 'd' && (cmd[2] == ' ' || cmd[2] == '\0')) {
        const char *arg = (cmd[2] == ' ') ? &cmd[3] : &cmd[2];
        cmd_cd(arg);
    } else if (cmd[0] == 'd' && cmd[1] == 'i' && cmd[2] == 's' && cmd[3] == 'c' && cmd[4] == 'o' && cmd[5] == ' ') {
        cmd_disk(&cmd[6]);
    } else if (cmd[0] == 'r' && cmd[1] == 'u' && cmd[2] == 'n' && (cmd[3] == ' ' || cmd[3] == '\0')) {
        const char *arg = (cmd[3] == ' ') ? &cmd[4] : &cmd[3];
        cmd_run(arg);
    } else if (cmd[0] == 'm' && cmd[1] == 'k' && cmd[2] == 'd' && cmd[3] == 'i' && cmd[4] == 'r' && (cmd[5] == ' ' || cmd[5] == '\0')) {
        const char *arg = (cmd[5] == ' ') ? &cmd[6] : &cmd[5];
        cmd_mkdir(arg);
    } else if (cmd[0] == 'd' && cmd[1] == 'e' && cmd[2] == 'l' && (cmd[3] == ' ' || cmd[3] == '\0')) {
        const char *arg = (cmd[3] == ' ') ? &cmd[4] : &cmd[3];
        cmd_del(arg);
    } else if (cmd[0] == 'r' && cmd[1] == 'm' && (cmd[2] == ' ' || cmd[2] == '\0')) {
        const char *arg = (cmd[2] == ' ') ? &cmd[3] : &cmd[2];
        cmd_del(arg);
    } else if (cmd[0] == 'c' && cmd[1] == 'a' && cmd[2] == 't' && (cmd[3] == ' ' || cmd[3] == '\0')) {
        const char *arg = (cmd[3] == ' ') ? &cmd[4] : &cmd[3];
        cmd_cat(arg);
    } else if (cmd[0] == 't' && cmd[1] == 'y' && cmd[2] == 'p' && cmd[3] == 'e' && (cmd[4] == ' ' || cmd[4] == '\0')) {
        const char *arg = (cmd[4] == ' ') ? &cmd[5] : &cmd[4];
        cmd_cat(arg);
    } else if (cmd[0] == 'e' && cmd[1] == 'c' && cmd[2] == 'h' && cmd[3] == 'o' && (cmd[4] == ' ' || cmd[4] == '\0')) {
        const char *arg = (cmd[4] == ' ') ? &cmd[5] : &cmd[4];
        print_line(arg);
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

    // Ventana de inicio mas grande que la que nos daria el lanzador por
    // defecto -- con el tipo de letra de sistema y el nuevo ajuste de
    // linea, esto deja sitio de sobra para que los mensajes de ayuda y
    // la salida de comandos quepan en una sola fila casi siempre, en
    // vez de partirse a cada momento.
    syscall5(SYS_CREATE_WINDOW, (uint64_t)"Shell", 40, 40, 560, 380);

    // NOTA REAL: el sistema de fuentes solo admite escalas ENTERAS del
    // tamaño base (5x7) -- x1 o x2, sin nada intermedio (con altura
    // pedida <=10 redondea a x1, con >=11 salta directo a x2; no hay
    // ningun valor que de un x1.5). x2 (el doble exacto) resulto
    // demasiado grande para una consola de texto corriente, asi que
    // nos quedamos en el tamaño de sistema (x1) y ponemos el esfuerzo
    // en el espaciado, que es lo que de verdad se echaba en falta.
    // "Shell" no es una familia del paquete de fuentes: la
    // busqueda caia a la SANS de 10, proporcional, y el cursor -- que se
    // coloca contando columnas de ancho fijo -- quedaba separado del
    // texto. La mono de 10 avanza 6 px por caracter, como la 5x7.
    int64_t font = (int64_t)syscall5(SYS_LOAD_FONT, (uint64_t)"mono", 10, 0, 0, 0);
    if (font > 0) syscall5(SYS_SET_FONT, (uint64_t)font, 0, 0, 0, 0);
    // BUG REAL CORREGIDO: SYS_FONT_WIDTH da el ancho del GLIFO (5px),
    // pero el AVANCE real entre caracteres al dibujar texto es
    // (FONT_WIDTH+1) -- el pixel de separacion cuenta tambien. Usar el
    // ancho de glifo aqui subestimaba el hueco real por caracter, asi
    // que el ajuste de linea dejaba pasar mas texto del que de verdad
    // cabe: el sobrante se salia por el borde de la ventana en vez de
    // bajar de linea. SYS_FONT_CHAR_ADVANCE da el valor correcto.
    char_w = (int)syscall5(SYS_FONT_CHAR_ADVANCE, 0, 0, 0, 0, 0);
    int glyph_h = (int)syscall5(SYS_FONT_HEIGHT, 0, 0, 0, 0, 0);
    if (char_w <= 0) char_w = 6;
    if (glyph_h <= 0) glyph_h = 7;
    // Espaciado generoso entre lineas (150% del alto del glifo, como
    // una terminal comoda) en vez del practicamente-pegado de antes.
    char_h = glyph_h + glyph_h / 2;
    // Con una fuente de las nuevas, SYS_FONT_HEIGHT ya da el alto de LINEA
    // (13 px en la mono 10), con su interlineado: no hay que multiplicarlo.
    if (glyph_h >= 10) char_h = glyph_h + 1;

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
        if (alen > 0 && launch_cmd[0] == '@') {
            // "@<carpeta> orden": situarse en esa carpeta y quitar el prefijo
            uint32_t carpeta = 0; int k = 1;
            while (launch_cmd[k] >= '0' && launch_cmd[k] <= '9') { carpeta = carpeta * 10 + (uint32_t)(launch_cmd[k] - '0'); k++; }
            while (launch_cmd[k] == ' ') k++;
            situarse_en(carpeta);
            int d = 0; while (launch_cmd[k]) launch_cmd[d++] = launch_cmd[k++];
            launch_cmd[d] = '\0';
        }
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

    bool last_blink = cursor_blink_on();

    while (running) {
        // A diferencia de antes, ya NO esperamos aqui bloqueados a
        // que llegue una tecla -- necesitamos poder comprobar tambien
        // si algun programa lanzado con 'run' tiene salida pendiente
        // (y que el cursor siga parpadeando aunque no pase nada mas),
        // asi que cedemos el control una vez por vuelta y miramos
        // todo eso cada vez (como hace el explorador).
        uint64_t pump = syscall5(SYS_PUMP, 0, 0, 0, 0, 0);
        if ((int64_t)pump < 0) {
            // La ventana se cerro desde fuera (destruida por otra via).
            running = false;
            break;
        }
        // BUG REAL CORREGIDO: SYS_CREATE_WINDOW (para pedir nuestro
        // propio tamaño de ventana) pone la ventana en "modo evento" --
        // el mismo que usa el CreateWindow real de BlitzBasic. En ese
        // modo, pulsar la X YA NO destruye la ventana por su cuenta:
        // solo dispara EVENT_WINDOWCLOSE (0x803) y es el programa quien
        // debe comprobarlo y decidir cuando terminar. Sin esto la X se
        // quedaba sin efecto -- la ventana nunca se cerraba.
        if ((int64_t)syscall5(SYS_POLL_EVENT, 0, 0, 0, 0, 0) == EVENT_WINDOWCLOSE) {
            running = false;
            break;
        }

        // Volvemos a consultar el tamaño por si la ventana cambio
        // (maximizar, redimensionar) desde la ultima vuelta -- con el
        // ajuste de linea esto ya recalcula solo cuanto texto cabe.
        uint64_t sz = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
        int new_w = (int)(sz >> 32);
        int new_h = (int)(sz & 0xFFFFFFFF);
        bool need_redraw = (new_w > 0 && new_w != win_w) || (new_h > 0 && new_h != win_h);
        if (new_w > 0) win_w = new_w;
        if (new_h > 0) win_h = new_h;

        // -- parpadeo del cursor: solo pedimos un redibujado cuando
        // de verdad cambia de fase (cada ~500ms), no en cada vuelta --
        bool blink_now = cursor_blink_on();
        if (blink_now != last_blink) { last_blink = blink_now; need_redraw = true; }

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
            // Retroceso en la SALIDA de consola: quita el ultimo
            // caracter de la linea en curso.
            //
            // Hace falta porque el eco de Input$ (en el kernel) manda
            // "\b \b" para borrar en pantalla, y aqui nadie
            // interpretaba el \b: se guardaba como un caracter mas y
            // la fuente lo dibujaba como '?'. Borrar dentro de un
            // programa llenaba la linea de interrogaciones.
            if (cc == '\b') {
                if (cursor_row >= 0) {
                    if (cursor_col > 0) cursor_col--;
                } else if (console_line_len > 0) {
                    console_line_len--;
                    console_line_buf[console_line_len] = '\0';
                }
                need_redraw = true;
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
                int plen = str_len(prompt);

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
            // Cualquier tecla reinicia la fase del parpadeo a
            // "encendido" -- igual que en una terminal real, para que
            // el cursor no desaparezca justo cuando estas escribiendo.
            last_blink = true;
            need_redraw = true;
        }

        if (need_redraw) redraw_console();
    }

    write_string("shell: terminada.\n");
}
