// input.c — Nemo OS
//
// Driver virtio-input para teclado y raton (como "tablet", que reporta
// posicion ABSOLUTA en vez de movimientos relativos -- asi el cursor
// de Nemo OS sigue exactamente al cursor real de tu Mac sin tener que
// "capturar" el raton dentro de la ventana de QEMU).
//
// A diferencia del disco (peticion -> respuesta), aqui el dispositivo
// nos EMPUJA eventos: nosotros le damos de antemano un monton de
// buffers vacios, y el dispositivo los va rellenando segun ocurren
// eventos (una tecla, un movimiento de raton...). Nuestro trabajo es
// mirar periodicamente si hay buffers rellenados, procesarlos, y
// devolverlos vacios a la cola para que se puedan reutilizar.

#include "input.h"
#include "uart.h"
#include "ramfb.h"

#define VIRTIO_MMIO_BASE   0x0a000000UL
#define VIRTIO_MMIO_STRIDE 0x200UL
#define VIRTIO_MMIO_SLOTS  32
#define VIRTIO_MAGIC       0x74726976UL
#define VIRTIO_DEVICE_ID_INPUT 18

#define REG_MAGIC      0x000
#define REG_DEVICE_ID  0x008
#define REG_DRIVER_FEATURES 0x020
#define REG_DRIVER_FEATURES_SEL 0x024
#define REG_QUEUE_SEL  0x030
#define REG_QUEUE_NUM_MAX 0x034
#define REG_QUEUE_NUM  0x038
#define REG_QUEUE_READY 0x044
#define REG_QUEUE_NOTIFY 0x050
#define REG_STATUS     0x070
#define REG_QUEUE_DESC_LOW 0x080
#define REG_QUEUE_DESC_HIGH 0x084
#define REG_QUEUE_DRIVER_LOW 0x090
#define REG_QUEUE_DRIVER_HIGH 0x094
#define REG_QUEUE_DEVICE_LOW 0x0a0
#define REG_QUEUE_DEVICE_HIGH 0x0a4
#define REG_CONFIG     0x100

#define CFG_SELECT 0x00
#define CFG_SUBSEL 0x01
#define CFG_SIZE   0x02
#define CFG_DATA   0x08

#define CFG_ID_SERIAL 0x02
#define CFG_ABS_INFO  0x12

#define STATUS_ACKNOWLEDGE 1
#define STATUS_DRIVER      2
#define STATUS_DRIVER_OK   4
#define STATUS_FEATURES_OK 8
#define VIRTIO_F_VERSION_1_BIT 0

#define QUEUE_SIZE 16
#define VIRTQ_DESC_F_WRITE 2

#define MAX_INPUT_DEVS 2
#define KBD_DEV 0
#define MOUSE_DEV 1

struct virtq_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[QUEUE_SIZE]; };
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_used { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[QUEUE_SIZE]; };

// Formato del evento tal y como lo rellena el dispositivo (little
// endian, sin swaps -- a diferencia de fw_cfg, virtio SI usa la
// endianness nativa del invitado).
struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03

#define REL_WHEEL 0x08 // rueda del raton -- valor +1/-1 (o mas, si el scroll es rapido) por muesca

#define ABS_X 0x00
#define ABS_Y 0x01

#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

__attribute__((aligned(4096))) static struct virtq_desc desc_table[MAX_INPUT_DEVS][QUEUE_SIZE];
__attribute__((aligned(4096))) static struct virtq_avail avail_ring[MAX_INPUT_DEVS];
__attribute__((aligned(4096))) static struct virtq_used used_ring[MAX_INPUT_DEVS];
__attribute__((aligned(16))) static struct virtio_input_event event_bufs[MAX_INPUT_DEVS][QUEUE_SIZE];

static uint64_t mmio_base[MAX_INPUT_DEVS];
static bool dev_present[MAX_INPUT_DEVS];
static uint16_t last_used_idx[MAX_INPUT_DEVS];
static uint32_t devs_found = 0;
static uint8_t logical_to_physical[MAX_INPUT_DEVS]; // igual que en disk.c: por serie, no por orden

static int32_t abs_min_x = 0, abs_max_x = 32767;
static int32_t abs_min_y = 0, abs_max_y = 32767;

static int32_t cur_mouse_x = 0, cur_mouse_y = 0;
static bool cur_btn_left = false, cur_btn_right = false, cur_btn_middle = false;
static int32_t cur_wheel_delta = 0;
static int32_t cur_wheel_total = 0; // acumulado desde que arranco el programa, para MouseZ() -- NUNCA se resetea al leerlo (a diferencia de mouse_wheel_delta)
static bool key_state[512];
static bool caps_lock_on = false;

#define CHAR_QUEUE_SIZE 64
static char char_queue[CHAR_QUEUE_SIZE];
static uint32_t char_queue_head = 0, char_queue_tail = 0;

// Cola de SCANCODES pulsados (para GetKey/WaitKey) -- separada de
// char_queue porque esta lleva el codigo crudo, incluso de teclas sin
// traduccion ASCII.
#define SCAN_QUEUE_SIZE 32
static uint16_t scan_queue[SCAN_QUEUE_SIZE];
static uint32_t scan_queue_head = 0, scan_queue_tail = 0;

// Contadores de "cuantas veces se ha pulsado esta tecla/boton" desde
// la ultima consulta -- BlitzPlus real devuelve un NUMERO, no un
// simple si/no (confirmado en el manual: "Devuelve el numero de
// veces que se ha pulsado"). Se incrementan en el flanco de subida, y
// se leen-y-resetean a la vez al consultarlos.
static uint32_t key_hit_counts[512];
static uint32_t mouse_hit_left_count = 0, mouse_hit_right_count = 0, mouse_hit_middle_count = 0;

// Ultima posicion del raton "vista" por MouseXSpeed/MouseYSpeed, para
// poder calcular la diferencia desde la ultima vez que se pregunto.
static int32_t last_speed_x = 0, last_speed_y = 0;

static inline uint32_t mmio_read(uint8_t p, uint32_t off) { return *(volatile uint32_t *)(mmio_base[p] + off); }
static inline void mmio_write(uint8_t p, uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio_base[p] + off) = v; }
static inline uint8_t mmio_read8(uint8_t p, uint32_t off) { return *(volatile uint8_t *)(mmio_base[p] + off); }
static inline void mmio_write8(uint8_t p, uint32_t off, uint8_t v) { *(volatile uint8_t *)(mmio_base[p] + off) = v; }

static void find_input_devices(void) {
    uart_puts("input checkpoint: buscando dispositivos...\n");
    for (int i = 0; i < VIRTIO_MMIO_SLOTS && devs_found < MAX_INPUT_DEVS; i++) {
        uint64_t base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_STRIDE;
        if (*(volatile uint32_t *)(base + REG_MAGIC) != VIRTIO_MAGIC) continue;
        if (*(volatile uint32_t *)(base + REG_DEVICE_ID) == VIRTIO_DEVICE_ID_INPUT) {
            mmio_base[devs_found] = base;
            devs_found++;
        }
    }
    uart_puts("input checkpoint: busqueda terminada, encontrados = ");
    uart_putc('0' + devs_found);
    uart_puts("\n");
}

static void read_serial(uint8_t phys, char out[128]) {
    mmio_write8(phys, REG_CONFIG + CFG_SELECT, CFG_ID_SERIAL);
    mmio_write8(phys, REG_CONFIG + CFG_SUBSEL, 0);
    uint8_t size = mmio_read8(phys, REG_CONFIG + CFG_SIZE);
    for (int i = 0; i < 128; i++) out[i] = 0;
    for (int i = 0; i < size && i < 128; i++) {
        out[i] = (char)mmio_read8(phys, REG_CONFIG + CFG_DATA + i);
    }
}

static void read_abs_info(uint8_t phys, uint8_t axis, int32_t *out_min, int32_t *out_max) {
    mmio_write8(phys, REG_CONFIG + CFG_SELECT, CFG_ABS_INFO);
    mmio_write8(phys, REG_CONFIG + CFG_SUBSEL, axis);
    uint32_t min = mmio_read(phys, REG_CONFIG + CFG_DATA + 0);
    uint32_t max = mmio_read(phys, REG_CONFIG + CFG_DATA + 4);
    *out_min = (int32_t)min;
    *out_max = (int32_t)max;
}

static bool serial_eq(const char *a, const char *b) {
    int i = 0;
    while (b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return true;
}

static bool init_one_dev(uint8_t phys) {
    mmio_write(phys, REG_STATUS, 0);
    mmio_write(phys, REG_STATUS, STATUS_ACKNOWLEDGE);
    mmio_write(phys, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    mmio_write(phys, REG_DRIVER_FEATURES_SEL, 0);
    mmio_write(phys, REG_DRIVER_FEATURES, 0);
    mmio_write(phys, REG_DRIVER_FEATURES_SEL, 1);
    mmio_write(phys, REG_DRIVER_FEATURES, 1 << VIRTIO_F_VERSION_1_BIT);

    mmio_write(phys, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!(mmio_read(phys, REG_STATUS) & STATUS_FEATURES_OK)) return false;

    // Cola 0 = eventq (la unica que usamos; la cola 1, statusq, no la
    // necesitamos para leer eventos)
    mmio_write(phys, REG_QUEUE_SEL, 0);
    if (mmio_read(phys, REG_QUEUE_NUM_MAX) == 0) return false;
    mmio_write(phys, REG_QUEUE_NUM, QUEUE_SIZE);

    uint64_t desc_addr = (uint64_t)desc_table[phys];
    uint64_t avail_addr = (uint64_t)&avail_ring[phys];
    uint64_t used_addr = (uint64_t)&used_ring[phys];
    mmio_write(phys, REG_QUEUE_DESC_LOW, (uint32_t)(desc_addr & 0xFFFFFFFF));
    mmio_write(phys, REG_QUEUE_DESC_HIGH, (uint32_t)(desc_addr >> 32));
    mmio_write(phys, REG_QUEUE_DRIVER_LOW, (uint32_t)(avail_addr & 0xFFFFFFFF));
    mmio_write(phys, REG_QUEUE_DRIVER_HIGH, (uint32_t)(avail_addr >> 32));
    mmio_write(phys, REG_QUEUE_DEVICE_LOW, (uint32_t)(used_addr & 0xFFFFFFFF));
    mmio_write(phys, REG_QUEUE_DEVICE_HIGH, (uint32_t)(used_addr >> 32));
    mmio_write(phys, REG_QUEUE_READY, 1);

    mmio_write(phys, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);

    // Rellenamos la cola con buffers vacios para que el dispositivo
    // tenga donde escribir los primeros eventos
    for (int i = 0; i < QUEUE_SIZE; i++) {
        desc_table[phys][i].addr = (uint64_t)&event_bufs[phys][i];
        desc_table[phys][i].len = sizeof(struct virtio_input_event);
        desc_table[phys][i].flags = VIRTQ_DESC_F_WRITE;
        desc_table[phys][i].next = 0;
        avail_ring[phys].ring[i] = (uint16_t)i;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    avail_ring[phys].idx = QUEUE_SIZE;
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_write(phys, REG_QUEUE_NOTIFY, 0);

    dev_present[phys] = true;
    last_used_idx[phys] = 0;
    return true;
}

bool input_init(void) {
    find_input_devices();
    if (devs_found == 0) {
        uart_puts("input: no se encontraron dispositivos virtio-input\n");
        return false;
    }

    for (uint32_t i = 0; i < devs_found; i++) {
        init_one_dev((uint8_t)i);
        logical_to_physical[i] = (uint8_t)i;
    }

    char serial[128];
    for (uint32_t phys = 0; phys < devs_found; phys++) {
        read_serial((uint8_t)phys, serial);
        uart_puts("input: dispositivo fisico ");
        uart_putc('0' + phys);
        uart_puts(" -> serie '");
        uart_puts(serial);
        uart_puts("'\n");

        if (serial_eq(serial, "NEMOKBD")) {
            logical_to_physical[KBD_DEV] = (uint8_t)phys;
        } else if (serial_eq(serial, "NEMOMOUSE")) {
            logical_to_physical[MOUSE_DEV] = (uint8_t)phys;
            read_abs_info((uint8_t)phys, ABS_X, &abs_min_x, &abs_max_x);
            read_abs_info((uint8_t)phys, ABS_Y, &abs_min_y, &abs_max_y);
        }
    }

    cur_mouse_x = (int32_t)(fb_width() / 2);
    cur_mouse_y = (int32_t)(fb_height() / 2);

    uart_puts("input: teclado y raton listos.\n");
    return true;
}

// -- traduccion de codigos de tecla (estilo Linux) a ASCII --
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

// Imprime un numero decimal pequeño (0-9999) por UART -- solo para
// depuracion del mapeo de teclado, no se usa en el funcionamiento
// normal del sistema.
static void uart_put_dec(uint16_t val) {
    char digits[5];
    int n = 0;
    if (val == 0) { uart_putc('0'); return; }
    while (val > 0 && n < 5) { digits[n++] = (char)('0' + (val % 10)); val /= 10; }
    while (n > 0) uart_putc(digits[--n]);
}

static void process_event(uint8_t logical_dev, struct virtio_input_event *ev) {
    if (logical_dev == KBD_DEV) {
        if (ev->type != EV_KEY) return;
        uint16_t code = ev->code;
        bool down = (ev->value != 0); // 1=pulsada, 2=repeticion, 0=soltada

        // -- DEPURACION DE TECLADO -- imprime cada tecla que llega,
        // para poder mapear un teclado fisico real sin adivinar.
        // Quitar estas 6 lineas cuando ya no haga falta.
        uart_puts("tecla: codigo=");
        uart_put_dec(code);
        uart_puts(down ? " ABAJO" : " arriba");
        uart_puts(" shift="); uart_putc(key_state[KEY_LEFTSHIFT] || key_state[KEY_RIGHTSHIFT] ? '1' : '0');
        uart_puts(" alt="); uart_putc(key_state[KEY_LEFTALT] || key_state[KEY_RIGHTALT] ? '1' : '0');
        uart_puts("\n");

        bool was_down = (code < 512) ? key_state[code] : false;
        if (code < 512) key_state[code] = down;

        if (down && !was_down && code < 512) {
            // Flanco de subida -- avisamos a KeyHit (contador por
            // tecla) y a la cola de scancodes (ya sin uso directo de
            // GetKey/WaitKey desde que estos pasaron a usar la cola de
            // caracteres ASCII, pero la dejamos por si hace falta mas
            // adelante).
            key_hit_counts[code]++;
            uint32_t next = (scan_queue_tail + 1) % SCAN_QUEUE_SIZE;
            if (next != scan_queue_head) { // si esta llena, descartamos en vez de bloquear
                scan_queue[scan_queue_tail] = code;
                scan_queue_tail = next;
            }
        }

        if (code == KEY_CAPSLOCK) {
            if (down && !was_down) { // solo al flanco de subida -- conmuta una vez por pulsacion
                caps_lock_on = !caps_lock_on;
            }
            return; // Caps Lock no genera ningun caracter propio
        }

        if (down && code != KEY_LEFTSHIFT && code != KEY_RIGHTSHIFT &&
            code != KEY_LEFTALT && code != KEY_RIGHTALT) {
            bool shift = key_state[KEY_LEFTSHIFT] || key_state[KEY_RIGHTSHIFT];
            bool alt = key_state[KEY_LEFTALT] || key_state[KEY_RIGHTALT];
            char c = keycode_to_ascii(code, shift, alt);
            if (c != 0) push_char(c);
        }
    } else if (logical_dev == MOUSE_DEV) {
        if (ev->type == EV_ABS) {
            if (ev->code == ABS_X) {
                int32_t range = abs_max_x - abs_min_x;
                if (range <= 0) range = 1;
                cur_mouse_x = (int32_t)(((int64_t)((int32_t)ev->value - abs_min_x) * (fb_width() - 1)) / range);
            } else if (ev->code == ABS_Y) {
                int32_t range = abs_max_y - abs_min_y;
                if (range <= 0) range = 1;
                cur_mouse_y = (int32_t)(((int64_t)((int32_t)ev->value - abs_min_y) * (fb_height() - 1)) / range);
            }
        } else if (ev->type == EV_KEY) {
            bool down = (ev->value != 0);
            if (ev->code == BTN_LEFT) {
                if (down && !cur_btn_left) mouse_hit_left_count++;
                cur_btn_left = down;
            } else if (ev->code == BTN_RIGHT) {
                if (down && !cur_btn_right) mouse_hit_right_count++;
                cur_btn_right = down;
            } else if (ev->code == BTN_MIDDLE) {
                if (down && !cur_btn_middle) mouse_hit_middle_count++;
                cur_btn_middle = down;
            }
        } else if (ev->type == EV_REL) {
            if (ev->code == REL_WHEEL) {
                cur_wheel_delta += (int32_t)ev->value; // acumulamos -- puede llegar mas de una muesca entre polls
                cur_wheel_total += (int32_t)ev->value;
            }
        }
    }
}

static void poll_one_dev(uint8_t logical_dev) {
    uint8_t phys = logical_to_physical[logical_dev];
    if (!dev_present[phys]) return;

    while (used_ring[phys].idx != last_used_idx[phys]) {
        uint16_t slot = last_used_idx[phys] % QUEUE_SIZE;
        uint32_t desc_id = used_ring[phys].ring[slot].id;

        process_event(logical_dev, &event_bufs[phys][desc_id]);

        last_used_idx[phys]++;

        // Devolvemos el mismo buffer a la cola de "disponibles" para
        // que el dispositivo lo pueda rellenar con el siguiente evento
        uint16_t avail_slot = avail_ring[phys].idx % QUEUE_SIZE;
        avail_ring[phys].ring[avail_slot] = (uint16_t)desc_id;
        __asm__ volatile("dsb sy" ::: "memory");
        avail_ring[phys].idx++;
        __asm__ volatile("dsb sy" ::: "memory");
        mmio_write(phys, REG_QUEUE_NOTIFY, 0);
    }
}

void input_poll(void) {
    poll_one_dev(KBD_DEV);
    poll_one_dev(MOUSE_DEV);
}

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
int32_t mouse_wheel_total(void) { return cur_wheel_total; } // MouseZ() -- NO se resetea al leerlo

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

void input_flush_keys(void) {
    input_flush_chars();
    scan_queue_head = scan_queue_tail;
    for (int i = 0; i < 512; i++) key_hit_counts[i] = 0;
    mouse_hit_left_count = 0;
    mouse_hit_right_count = 0;
    mouse_hit_middle_count = 0;
}

// FlushMouse -- BlitzPlus real lo trata como un comando SEPARADO de
// FlushKeys (solo limpia las pulsaciones de boton en cola, no toca
// nada del teclado).
void input_flush_mouse(void) {
    mouse_hit_left_count = 0;
    mouse_hit_right_count = 0;
    mouse_hit_middle_count = 0;
}
