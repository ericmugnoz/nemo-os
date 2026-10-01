// gadgetdemo.c — Nemo OS
//
// Demo minima del sistema de gadgets: una barra de menu real (Archivo
// > Salir / Acerca de), un boton, un campo de texto y una lista --
// para comprobar que todo el sistema (dibujo automatico, clics,
// teclado, eventos) funciona de punta a punta antes de construir el
// IDE de verdad encima.

#include <stdint.h>
#include <stdbool.h>

#define SYS_PUMP             14
#define SYS_GET_WINDOW_SIZE  33
#define SYS_CREATE_BUTTON    100
#define SYS_CREATE_TEXTFIELD 102
#define SYS_CREATE_LISTBOX   103
#define SYS_GADGET_SET_TEXT  105
#define SYS_GADGET_EVENT     113
#define SYS_LISTBOX_ADD_ITEM 114
#define SYS_WINDOW_MENU      120
#define SYS_CREATE_MENU      121
#define SYS_MENU_GET_TAG     124

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

static int32_t btn_id, field_id, list_id;
static int32_t menu_exit_id, menu_about_id;
static bool running = true;

__attribute__((section(".text.start")))
void _start(void) {
    uint64_t size = syscall5(SYS_GET_WINDOW_SIZE, 0, 0, 0, 0, 0);
    int win_w = (int)(size >> 32);
    (void)win_w;

    // -- menu de verdad, como en la captura de BlitzPlus --
    int32_t root = (int32_t)syscall5(SYS_WINDOW_MENU, 0, 0, 0, 0, 0);
    int32_t menu_file = (int32_t)syscall5(SYS_CREATE_MENU, (uint64_t)"ARCHIVO", 0, (uint64_t)root, 0, 0);
    menu_exit_id = (int32_t)syscall5(SYS_CREATE_MENU, (uint64_t)"SALIR", 99, (uint64_t)menu_file, 0, 0);
    int32_t menu_help = (int32_t)syscall5(SYS_CREATE_MENU, (uint64_t)"AYUDA", 0, (uint64_t)root, 0, 0);
    menu_about_id = (int32_t)syscall5(SYS_CREATE_MENU, (uint64_t)"ACERCA DE", 1, (uint64_t)menu_help, 0, 0);

    // -- gadgets normales --
    uint64_t wh1 = ((uint64_t)90 << 16) | 24;
    btn_id = (int32_t)syscall5(SYS_CREATE_BUTTON, (uint64_t)"PULSAME", 10, 30, wh1, 0);

    uint64_t wh2 = ((uint64_t)160 << 16) | 20;
    field_id = (int32_t)syscall5(SYS_CREATE_TEXTFIELD, 10, 66, wh2, 0, 0);
    syscall5(SYS_GADGET_SET_TEXT, (uint64_t)field_id, (uint64_t)"ESCRIBE AQUI", 0, 0, 0);

    uint64_t wh3 = ((uint64_t)160 << 16) | 100;
    list_id = (int32_t)syscall5(SYS_CREATE_LISTBOX, 10, 96, wh3, 0, 0);
    syscall5(SYS_LISTBOX_ADD_ITEM, (uint64_t)list_id, (uint64_t)"ELEMENTO UNO", 0, 0, 0);
    syscall5(SYS_LISTBOX_ADD_ITEM, (uint64_t)list_id, (uint64_t)"ELEMENTO DOS", 0, 0, 0);
    syscall5(SYS_LISTBOX_ADD_ITEM, (uint64_t)list_id, (uint64_t)"ELEMENTO TRES", 0, 0, 0);

    int clicks = 0;

    while (running) {
        uint64_t r = syscall5(SYS_PUMP, 0, 0, 0, 0, 0);
        if ((int64_t)r < 0) break;

        int32_t ev = (int32_t)syscall5(SYS_GADGET_EVENT, 0, 0, 0, 0, 0);
        if (ev == btn_id) {
            clicks++;
            static char label[16];
            label[0] = 'C'; label[1] = 'L'; label[2] = 'I'; label[3] = 'C';
            label[4] = 'S'; label[5] = ':'; label[6] = ' ';
            int p = 7;
            if (clicks == 0) { label[p++] = '0'; }
            else {
                char digits[6]; int n = 0; int v = clicks;
                while (v > 0) { digits[n++] = (char)('0' + v % 10); v /= 10; }
                while (n > 0) label[p++] = digits[--n];
            }
            label[p] = '\0';
            syscall5(SYS_GADGET_SET_TEXT, (uint64_t)btn_id, (uint64_t)label, 0, 0, 0);
        } else if (ev == menu_exit_id) {
            running = false;
        } else if (ev == menu_about_id) {
            syscall5(SYS_GADGET_SET_TEXT, (uint64_t)field_id, (uint64_t)"NEMO OS - GADGETS DEMO", 0, 0, 0);
        }
    }
}
