// input_pi4.c -- Nemo OS, Raspberry Pi 4
// Entrada por USB (teclado HID boot a traves del VL805 / xHCI).
//
// La logica de teclado (codigos estilo Linux, traduccion a ASCII con la
// distribucion española, Caps Lock, colas de caracteres/scancodes,
// contadores de KeyHit) es la MISMA que en input.c de QEMU, copiada
// aqui para que el teclado fisico se comporte exactamente igual que el
// virtual. Lo unico nuevo es la capa de abajo: informes HID boot de 8
// bytes -> codigos Linux (evdev), con deteccion de flancos comparando
// cada informe con el anterior.
//
// Raton: informes HID boot de 3-4 bytes (botones, dx, dy, rueda) ->
// posicion relativa acumulada y limitada al framebuffer; botones,
// rueda y velocidades con la misma semantica que input.c.

#include "input.h"
#include "ramfb.h"
#include "timer.h"
#include "pcie_pi4.h"
#include "xhci_pi4.h"

void uart_puts(const char *s);
void uart_putc(char c);

// ---------------------------------------------------------------------
// Estado de teclado y colas (identico a input.c)
// ---------------------------------------------------------------------
static bool key_state[512];
static bool caps_lock_on = false;

#define CHAR_QUEUE_SIZE 64
static char char_queue[CHAR_QUEUE_SIZE];
static uint32_t char_queue_head = 0, char_queue_tail = 0;

#define SCAN_QUEUE_SIZE 32
static uint16_t scan_queue[SCAN_QUEUE_SIZE];
static uint32_t scan_queue_head = 0, scan_queue_tail = 0;

static uint32_t key_hit_counts[512];

// Autorrepeticion por software. El teclado esta en SET_IDLE(0): solo
// informa de cambios, asi que mantener una tecla pulsada da una sola
// pulsacion. En QEMU la repeticion la generaba evdev (value=2) y
// process_event() la trataba como pulsacion: vuelve a push_char, pero
// NO cuenta KeyHit ni entra en la cola de scancodes. Aqui igual.
// Tiempos por defecto de Linux: 500 ms de espera, ~33 ms entre repeticiones.
#define REPETICION_ESPERA_TICKS   50   // ticks de 10 ms -> 500 ms
#define REPETICION_PERIODO_TICKS  3    // 30 ms
static uint16_t repite_code = 0;          // tecla que se esta repitiendo (0 = ninguna)
static uint64_t repite_siguiente = 0;     // tick en que toca la proxima repeticion
static uint32_t mouse_hit_left_count = 0, mouse_hit_right_count = 0, mouse_hit_middle_count = 0;

// Estado del raton (identico a input.c)
static int32_t cur_mouse_x = 0, cur_mouse_y = 0;
static bool cur_btn_left = false, cur_btn_right = false, cur_btn_middle = false;
static int32_t cur_wheel_delta = 0;
static int32_t cur_wheel_total = 0;
static int32_t last_speed_x = 0, last_speed_y = 0;

#define KEY_ESC 1
#define KEY_BACKSPACE 14
#define KEY_ENTER 28
#define KEY_SPACE 57
#define KEY_LEFTSHIFT 42
#define KEY_RIGHTSHIFT 54
#define KEY_UP 103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108
#define KEY_MINUS 12
#define KEY_EQUAL 13
#define KEY_SEMICOLON 39
#define KEY_APOSTROPHE 40
#define KEY_CAPSLOCK 58
#define KEY_COMMA 51
#define KEY_DOT 52
#define KEY_SLASH 53
#define KEY_GRAVE 41
#define KEY_LEFTBRACE 26
#define KEY_RIGHTBRACE 27
#define KEY_BACKSLASH 43
#define KEY_LEFTALT 56
#define KEY_RIGHTALT 100
#define KEY_102ND 86 // tecla extra de los teclados ISO, a la izquierda de Z: "<" ">"

// Las flechas no tienen caracter ASCII propio -- usamos codigos de
// control sin uso (0x11-0x14) para representarlas ante los programas.
#define CH_UP    0x11
#define CH_DOWN  0x12
#define CH_LEFT  0x13
#define CH_RIGHT 0x14

static char keycode_to_ascii(uint16_t code, bool shift, bool alt) {
    // digitos 1-0 (codigos 2-11), fila superior del teclado
    static const char digits[]     = "1234567890";
    // Fila de simbolos con Shift, segun la distribucion espanola real
    // (confirmada con el teclado fisico del usuario) -- muy distinta
    // de un teclado americano. El punto medio "·" de la tecla 3 no
    // esta en nuestra fuente/ASCII, lo aproximamos con '#'.
    static const char digits_top[] = "!\"#$%&/()=";
    if (code >= 2 && code <= 11) {
        // Con Option, la fila de numeros da simbolos de programacion
        // muy utiles que no teniamos manera fiable de conseguir antes
        // (documento de referencia del teclado fisico real).
        if (alt) {
            switch (code) {
                case 2: return '|';  // Option+1
                case 3: return '@';  // Option+2
                case 4: return '#';  // Option+3
                case 5: return '~';  // Option+4
                case 8: return '{';  // Option+7
                case 9: return '[';  // Option+8
                case 10: return ']'; // Option+9
                case 11: return '}'; // Option+0
                default: break; // Option+5/6 dan simbolos no-ASCII (½ ¬), los dejamos pasar
            }
        }
        return shift ? digits_top[code - 2] : digits[code - 2];
    }

    // letras -- QWERTY, en el orden fisico real del teclado
    static const uint16_t qwerty_codes[] = {
        16,17,18,19,20,21,22,23,24,25,           // Q W E R T Y U I O P
        30,31,32,33,34,35,36,37,38,              // A S D F G H J K L
        44,45,46,47,48,49,50                      // Z X C V B N M
    };
    static const char qwerty_upper[] = "QWERTYUIOPASDFGHJKLZXCVBNM";
    for (int i = 0; i < 26; i++) {
        if (qwerty_codes[i] == code) {
            char c = qwerty_upper[i];
            bool upper = shift != caps_lock_on; // XOR -- los dos a la vez se cancelan, como en cualquier teclado real
            return upper ? c : (char)(c - 'A' + 'a');
        }
    }

    if (code == KEY_SPACE) return ' ';
    if (code == KEY_ENTER) return '\n';
    if (code == KEY_BACKSPACE) return '\b';
    if (code == KEY_UP) return (char)CH_UP;
    if (code == KEY_DOWN) return (char)CH_DOWN;
    if (code == KEY_LEFT) return (char)CH_LEFT;
    if (code == KEY_RIGHT) return (char)CH_RIGHT;
    if (code == KEY_ESC) return 27;

    // Puntuacion -- segun la distribucion espanola real (foto del
    // teclado fisico + pruebas ya confirmadas). Las teclas de acento
    // muerto (´ ¨) y la Ç las dejamos sin mapear por ahora -- no
    // hacen falta para programar, y una tecla muerta de verdad
    // necesitaria logica especial (esperar la siguiente pulsacion
    // para combinarse con ella).
    if (code == KEY_DOT) return shift ? ':' : '.';
    if (code == KEY_COMMA) return shift ? ';' : ',';
    if (code == KEY_102ND) return shift ? '>' : '<';
    if (code == KEY_SLASH) return shift ? '_' : '-'; // en este teclado, el codigo 53 es el guion, no la barra
    if (code == KEY_MINUS) return shift ? '?' : '\''; // codigo 12: apostrofo sin Shift, interrogacion con Shift
    if (code == KEY_RIGHTBRACE) return shift ? '+' : '*'; // estaba al reves: es * sin Shift, + con Shift
    if (code == KEY_GRAVE && alt) return '\\'; // Option sobre la tecla "º", unico sitio donde conseguimos la barra invertida

    // Combinacion especifica de teclado Mac en español: Option+Ñ da
    // "~" en macOS. La Ñ fisica suele caer en la misma posicion que
    // el ';' de un teclado americano -- es nuestra mejor estimacion,
    // avisamos al usuario que la confirme.
    if (alt && code == KEY_SEMICOLON) return '~';

    return 0; // tecla sin traduccion ASCII (F1-F12, etc.)
}

static void push_char(char c) {
    uint32_t next = (char_queue_tail + 1) % CHAR_QUEUE_SIZE;
    if (next == char_queue_head) return; // cola llena, descartamos
    char_queue[char_queue_tail] = c;
    char_queue_tail = next;
}

bool input_read_char(char *out) {
    if (char_queue_head == char_queue_tail) return false;
    *out = char_queue[char_queue_head];
    char_queue_head = (char_queue_head + 1) % CHAR_QUEUE_SIZE;
    return true;
}

void input_flush_chars(void) {
    char_queue_head = char_queue_tail;
}

bool key_is_down(uint16_t keycode) {
    if (keycode >= 512) return false;
    return key_state[keycode];
}

bool input_read_scancode(uint16_t *out) {
    if (scan_queue_head == scan_queue_tail) return false;
    *out = scan_queue[scan_queue_head];
    scan_queue_head = (scan_queue_head + 1) % SCAN_QUEUE_SIZE;
    return true;
}

uint32_t key_was_hit(uint16_t keycode) {
    if (keycode >= 512) return 0;
    uint32_t c = key_hit_counts[keycode];
    key_hit_counts[keycode] = 0;
    return c;
}

void input_flush_keys(void) {
    input_flush_chars();
    scan_queue_head = scan_queue_tail;
    for (int i = 0; i < 512; i++) key_hit_counts[i] = 0;
    mouse_hit_left_count = 0;
    mouse_hit_right_count = 0;
    mouse_hit_middle_count = 0;
}

// ---------------------------------------------------------------------
// Capa HID: informe boot -> eventos de tecla. Misma semantica que
// process_event() de input.c: flanco de subida para KeyHit y la cola
// de scancodes, Caps Lock conmuta en el flanco, caracter ASCII al
// pulsar cualquier tecla que no sea modificador.
// ---------------------------------------------------------------------

// Usage IDs HID (pagina 0x07) -> codigos Linux. Misma correspondencia
// fisica que usa QEMU, asi que keycode_to_ascii vale tal cual.
static const uint16_t hid_a_linux[256] = {
    [0x04]=30,[0x05]=48,[0x06]=46,[0x07]=32,[0x08]=18,[0x09]=33,[0x0A]=34,[0x0B]=35,   // a b c d e f g h
    [0x0C]=23,[0x0D]=36,[0x0E]=37,[0x0F]=38,[0x10]=50,[0x11]=49,[0x12]=24,[0x13]=25,   // i j k l m n o p
    [0x14]=16,[0x15]=19,[0x16]=31,[0x17]=20,[0x18]=22,[0x19]=47,[0x1A]=17,[0x1B]=45,   // q r s t u v w x
    [0x1C]=21,[0x1D]=44,                                                             // y z
    [0x1E]=2,[0x1F]=3,[0x20]=4,[0x21]=5,[0x22]=6,[0x23]=7,[0x24]=8,[0x25]=9,[0x26]=10,[0x27]=11, // 1..9,0
    [0x28]=28,[0x29]=1,[0x2A]=14,[0x2B]=15,[0x2C]=57,                                // Enter Esc Backspace Tab Space
    [0x2D]=12,[0x2E]=13,[0x2F]=26,[0x30]=27,[0x31]=43,[0x32]=43,                     // - = [ ] backslash (non-US #)
    [0x33]=39,[0x34]=40,[0x35]=41,[0x36]=51,[0x37]=52,[0x38]=53,[0x39]=58,           // ; ' ` , . / CapsLock
    [0x3A]=59,[0x3B]=60,[0x3C]=61,[0x3D]=62,[0x3E]=63,[0x3F]=64,                     // F1..F6
    [0x40]=65,[0x41]=66,[0x42]=67,[0x43]=68,[0x44]=87,[0x45]=88,                     // F7..F12
    [0x46]=99,[0x47]=70,[0x48]=119,[0x49]=110,[0x4A]=102,[0x4B]=104,                 // PrtSc ScrLk Pause Ins Home PgUp
    [0x4C]=111,[0x4D]=107,[0x4E]=109,[0x4F]=106,[0x50]=105,[0x51]=108,[0x52]=103,    // Del End PgDn Right Left Down Up
    [0x53]=69,[0x54]=98,[0x55]=55,[0x56]=74,[0x57]=78,[0x58]=96,                     // NumLock KP/ KP* KP- KP+ KPEnter
    [0x59]=79,[0x5A]=80,[0x5B]=81,[0x5C]=75,[0x5D]=76,[0x5E]=77,[0x5F]=71,[0x60]=72,[0x61]=73, // KP1..KP9
    [0x62]=82,[0x63]=83,[0x64]=86,[0x65]=127,                                        // KP0 KP. 102nd Compose
};
static const uint16_t modif_a_linux[8] = { 29, 42, 56, 125, 97, 54, 100, 126 }; // LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI

static uint8_t informe_anterior[8];
static bool teclado_listo = false;

static void tecla_evento(uint16_t code, bool down) {
    bool was_down = (code < 512) ? key_state[code] : false;
    if (code < 512) key_state[code] = down;

    if (down && !was_down && code < 512) {
        key_hit_counts[code]++;
        uint32_t next = (scan_queue_tail + 1) % SCAN_QUEUE_SIZE;
        if (next != scan_queue_head) { scan_queue[scan_queue_tail] = code; scan_queue_tail = next; }
    }
    if (code == KEY_CAPSLOCK) {
        if (down && !was_down) caps_lock_on = !caps_lock_on;
        return;
    }
    if (down && code != KEY_LEFTSHIFT && code != KEY_RIGHTSHIFT &&
        code != KEY_LEFTALT && code != KEY_RIGHTALT) {
        bool shift = key_state[KEY_LEFTSHIFT] || key_state[KEY_RIGHTSHIFT];
        bool alt = key_state[KEY_LEFTALT] || key_state[KEY_RIGHTALT];
        char c = keycode_to_ascii(code, shift, alt);
        if (c != 0) push_char(c);
        // La ultima tecla pulsada es la que se repite (como en un PC)
        repite_code = code;
        repite_siguiente = timer_get_ticks() + REPETICION_ESPERA_TICKS;
    }
    if (!down && code == repite_code) repite_code = 0;   // soltada: fin de la repeticion
}

// Llamada desde input_poll(): genera las repeticiones pendientes.
static void autorrepeticion(void) {
    if (!repite_code) return;
    if (!key_state[repite_code]) { repite_code = 0; return; }
    uint64_t ahora = timer_get_ticks();
    if (ahora < repite_siguiente) return;
    repite_siguiente = ahora + REPETICION_PERIODO_TICKS;
    bool shift = key_state[KEY_LEFTSHIFT] || key_state[KEY_RIGHTSHIFT];
    bool alt = key_state[KEY_LEFTALT] || key_state[KEY_RIGHTALT];
    char c = keycode_to_ascii(repite_code, shift, alt);
    if (c != 0) push_char(c);
}

static bool en_informe(const uint8_t *r, uint8_t usage) {
    for (int i = 2; i < 8; i++) if (r[i] == usage) return true;
    return false;
}

// Un informe boot: [modificadores][reservado][hasta 6 teclas pulsadas]
static void informe_hid(const uint8_t *r, uint32_t len) {
    if (len < 8) return;
    if (r[2] == 0x01) return;   // ErrorRollOver: demasiadas teclas, ignorar

    uint8_t cambio = r[0] ^ informe_anterior[0];
    for (int b = 0; b < 8; b++)
        if (cambio & (1u << b)) tecla_evento(modif_a_linux[b], (r[0] >> b) & 1);

    for (int i = 2; i < 8; i++) {   // soltadas: estaban y ya no
        uint8_t u = informe_anterior[i];
        if (u && !en_informe(r, u) && hid_a_linux[u]) tecla_evento(hid_a_linux[u], false);
    }
    for (int i = 2; i < 8; i++) {   // pulsadas: estan y antes no
        uint8_t u = r[i];
        if (u && !en_informe(informe_anterior, u) && hid_a_linux[u]) tecla_evento(hid_a_linux[u], true);
    }
    for (int i = 0; i < 8; i++) informe_anterior[i] = r[i];
}

// Un informe boot de raton: [botones][dx][dy][rueda opcional], dx/dy/rueda
// en int8 relativo. Misma semantica que el raton de input.c: los
// contadores de MouseHit suben en el flanco, la rueda se acumula.
static void informe_raton(const uint8_t *r, uint32_t len) {
    uint8_t botones; int32_t dx, dy, rueda = 0; bool hay_rueda = false;

    if (len >= 6 && r[0] == 0x01) {
        // Report protocol de ratones genericos (el nuestro, VID 0000):
        // [ID=01][botones][X:12][Y:12][rueda:8]. X e Y de 12 bits con
        // signo, empaquetados en 3 bytes.
        botones = r[1];
        int32_t x12 = r[2] | ((r[3] & 0x0F) << 8);
        int32_t y12 = (r[3] >> 4) | (r[4] << 4);
        if (x12 & 0x800) x12 -= 0x1000;
        if (y12 & 0x800) y12 -= 0x1000;
        dx = x12; dy = y12;
        rueda = (int8_t)r[5]; hay_rueda = true;
    } else {
        // Protocolo boot: [botones][dx][dy][rueda opcional]
        if (len < 3) return;
        botones = r[0];
        dx = (int8_t)r[1]; dy = (int8_t)r[2];
        if (len >= 4) { rueda = (int8_t)r[3]; hay_rueda = true; }
    }

    // Botones: contador de MouseHit en el flanco de subida (como input.c)
    bool l = botones & 1, d = botones & 2, m = botones & 4;
    if (l && !cur_btn_left)   mouse_hit_left_count++;
    if (d && !cur_btn_right)  mouse_hit_right_count++;
    if (m && !cur_btn_middle) mouse_hit_middle_count++;
    cur_btn_left = l; cur_btn_right = d; cur_btn_middle = m;

    // Posicion relativa acumulada, limitada al framebuffer
    int32_t w = (int32_t)fb_width(), h = (int32_t)fb_height();
    cur_mouse_x += dx; cur_mouse_y += dy;
    if (cur_mouse_x < 0) cur_mouse_x = 0; else if (cur_mouse_x > w - 1) cur_mouse_x = w - 1;
    if (cur_mouse_y < 0) cur_mouse_y = 0; else if (cur_mouse_y > h - 1) cur_mouse_y = h - 1;

    if (hay_rueda) { cur_wheel_delta += rueda; cur_wheel_total += rueda; }
}

// Un informe de pantalla tactil, ya normalizado por xhci_pi4.c:
// [0] dedo apoyado, [1..2] X, [3..4] Y, X e Y de 0 a 32767.
//
// La diferencia de fondo con el raton es que el dedo es ABSOLUTO: no
// dice cuanto se ha movido, dice donde esta. Asi que aqui no se suma,
// se fija. El dedo apoyado hace de boton izquierdo, con el mismo
// criterio de flanco, de modo que todo lo que ya sabe reaccionar a un
// clic reacciona a un toque sin enterarse de que viene de un dedo.
static void informe_tactil(const uint8_t *r, uint32_t len) {
    if (len < 5) return;

    uint32_t xn = (uint32_t)r[1] | ((uint32_t)r[2] << 8);
    uint32_t yn = (uint32_t)r[3] | ((uint32_t)r[4] << 8);
    if (xn > 32767) xn = 32767;
    if (yn > 32767) yn = 32767;

    int32_t w = (int32_t)fb_width(), h = (int32_t)fb_height();
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    cur_mouse_x = (int32_t)(((uint64_t)xn * (uint32_t)(w - 1)) / 32767u);
    cur_mouse_y = (int32_t)(((uint64_t)yn * (uint32_t)(h - 1)) / 32767u);

    bool dedo = r[0] != 0;
    if (dedo && !cur_btn_left) mouse_hit_left_count++;
    cur_btn_left = dedo;
}

static void informe_hid_despacho(int tipo, const uint8_t *r, uint32_t len) {
    if (tipo == XHCI_HID_TECLADO) informe_hid(r, len);
    else if (tipo == XHCI_HID_RATON) informe_raton(r, len);
    else if (tipo == XHCI_HID_TACTIL) informe_tactil(r, len);
}

// ---------------------------------------------------------------------
// API
// ---------------------------------------------------------------------
bool input_init(void) {
    if (!pcie_init_pi4()) {
        uart_puts("input_pi4: PCIe/VL805 no disponible -- sin USB.\n");
        return false;
    }
    if (!xhci_init_pi4()) {
        uart_puts("input_pi4: sin dispositivos USB de entrada.\n");
        return false;
    }
    cur_mouse_x = (int32_t)(fb_width() / 2);   // cursor centrado, como en input.c
    cur_mouse_y = (int32_t)(fb_height() / 2);
    last_speed_x = cur_mouse_x; last_speed_y = cur_mouse_y;
    teclado_listo = true;
    uart_puts("input_pi4: entrada USB lista.\n");
    return true;
}

void disk_pi4_actualizar_usb(void);   // disk_pi4.c: (re)monta la FAT si entra/sale un pendrive

void input_poll(void) {
    if (!teclado_listo) return;
    xhci_poll(informe_hid_despacho);
    autorrepeticion();
    disk_pi4_actualizar_usb();
}

// Raton (identico a input.c)
int32_t mouse_x(void) { return cur_mouse_x; }
int32_t mouse_y(void) { return cur_mouse_y; }
bool mouse_left_down(void) { return cur_btn_left; }
bool mouse_right_down(void) { return cur_btn_right; }
bool mouse_middle_down(void) { return cur_btn_middle; }
int32_t mouse_wheel_delta(void) {
    int32_t d = cur_wheel_delta;
    cur_wheel_delta = 0;
    return d;
}
int32_t mouse_wheel_total(void) { return cur_wheel_total; }

uint32_t mouse_button_was_hit(int button) {
    if (button == 1) { uint32_t c = mouse_hit_left_count; mouse_hit_left_count = 0; return c; }
    if (button == 2) { uint32_t c = mouse_hit_right_count; mouse_hit_right_count = 0; return c; }
    if (button == 3) { uint32_t c = mouse_hit_middle_count; mouse_hit_middle_count = 0; return c; }
    return 0;
}

int32_t mouse_x_speed(void) {
    int32_t d = cur_mouse_x - last_speed_x;
    last_speed_x = cur_mouse_x;
    return d;
}

int32_t mouse_y_speed(void) {
    int32_t d = cur_mouse_y - last_speed_y;
    last_speed_y = cur_mouse_y;
    return d;
}

void mouse_move_to(int32_t x, int32_t y) {
    cur_mouse_x = x;
    cur_mouse_y = y;
    last_speed_x = x;
    last_speed_y = y;
}

void input_flush_mouse(void) {
    mouse_hit_left_count = 0;
    mouse_hit_right_count = 0;
    mouse_hit_middle_count = 0;
}
