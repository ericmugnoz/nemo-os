// kernel.c — Nemo OS
// Punto de entrada en C. Inicializa el UART, las excepciones, la MMU,
// el GIC, el timer, el heap, el disco y NemoFS.

#include <stdint.h>
#include "uart.h"
#include "gic.h"
#include "timer.h"
#include "mmu.h"
#include "heap.h"
#include "disk.h"
#include "sound.h"
#include "nemofs.h"
#include "fat.h"
#include "loader.h"
#include "ramfb.h"
#include "text.h"
#include "input.h"
#include "wm.h"
#include "tasks.h"

#define UART0_BASE 0x09000000
#define UART0_DR   (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART0_FR   (*(volatile uint32_t *)(UART0_BASE + 0x18))
#define UART_FR_TXFF (1 << 5)

void uart_putc(char c) {
    while (UART0_FR & UART_FR_TXFF) {
        // busy wait
    }
    UART0_DR = c;
}

void uart_puts(const char *str) {
    while (*str) {
        if (*str == '\n') {
            uart_putc('\r');
        }
        uart_putc(*str);
        str++;
    }
}

// Definida en exceptions.s — registra nuestra tabla de vectores en VBAR_EL1
extern void exceptions_init(void);

// Inodo de la carpeta "PROGRAMAS" -- ahi viven todos los .pro, y ahi
// es donde el bucle principal busca los programas al lanzarlos desde
// un icono o el menu Start. Se rellena una vez en el arranque.
static uint32_t g_programas_dir = 0;

// Solo para depuracion.
static void uart_put_dec_signed(int32_t val) {
    if (val < 0) { uart_putc('-'); val = -val; }
    char digits[12];
    int n = 0;
    if (val == 0) { uart_putc('0'); return; }
    while (val > 0 && n < 12) { digits[n++] = (char)('0' + (val % 10)); val /= 10; }
    while (n > 0) uart_putc(digits[--n]);
}

static bool str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

// Busca una carpeta por nombre dentro de 'parent'; si no existe, la crea.
static int32_t nemofs_ensure_dir(uint32_t parent, const char *name) {
    int32_t idx = nemofs_find_child(parent, name);
    if (idx >= 0) return idx;
    return nemofs_create(parent, name, NEMOFS_TYPE_DIR);
}

static void test_nemofs(uint32_t documentos_dir) {
    // Creamos una carpeta y un archivo dentro de Documentos
    int32_t docs = nemofs_create(documentos_dir, "docs", NEMOFS_TYPE_DIR);
    if (docs < 0) {
        uart_puts("nemofs: fallo creando carpeta 'docs'\n");
        return;
    }
    uart_puts("nemofs: carpeta 'docs' creada.\n");

    int32_t hello = nemofs_create((uint32_t)docs, "hello.txt", NEMOFS_TYPE_FILE);
    if (hello < 0) {
        uart_puts("nemofs: fallo creando archivo 'hello.txt'\n");
        return;
    }
    uart_puts("nemofs: archivo 'hello.txt' creado dentro de 'docs'.\n");

    const char *content = "Hola desde NemoFS! Este archivo vive dentro de una carpeta anidada.\n";
    uint32_t len = 0;
    while (content[len] != '\0') len++;

    if (!nemofs_write_file((uint32_t)hello, content, len)) {
        uart_puts("nemofs: fallo escribiendo el archivo\n");
        return;
    }
    uart_puts("nemofs: contenido escrito.\n");

    static char read_buf[256];
    int32_t bytes_read = nemofs_read_file((uint32_t)hello, read_buf, sizeof(read_buf) - 1);
    if (bytes_read < 0) {
        uart_puts("nemofs: fallo leyendo el archivo\n");
        return;
    }
    read_buf[bytes_read] = '\0';
    uart_puts("nemofs: contenido leido -> ");
    uart_puts(read_buf);

    // Listamos el contenido de Documentos
    static nemofs_dirent_t entries[16];
    uint32_t count = nemofs_list_dir(documentos_dir, entries, 16);
    uart_puts("nemofs: contenido de Documentos (");
    if (count == 0) uart_putc('0');
    else {
        char digits[8]; int n = 0; uint32_t c = count;
        while (c > 0) { digits[n++] = '0' + (c % 10); c /= 10; }
        while (n > 0) uart_putc(digits[--n]);
    }
    uart_puts(" elementos):\n");
    for (uint32_t i = 0; i < count && i < 16; i++) {
        uart_puts("  - ");
        uart_puts(entries[i].name);
        uart_puts(entries[i].type == NEMOFS_TYPE_DIR ? " (carpeta)\n" : " (archivo)\n");
    }
}

static void test_fat(void) {
    const char *content = "Este archivo se puede leer desde el Mac montando fat.img.\n";
    uint32_t len = 0;
    while (content[len] != '\0') len++;

    if (!fat_create_file("HELLO.TXT", content, len)) {
        uart_puts("fat: fallo creando HELLO.TXT (¿ya existia de una ejecucion anterior?)\n");
    } else {
        uart_puts("fat: HELLO.TXT creado y escrito.\n");
    }

    fat_dirent_t entry;
    if (fat_find_root("HELLO.TXT", &entry)) {
        static char read_buf[256];
        uint32_t bytes_read = 0;
        if (fat_read_file(&entry, read_buf, sizeof(read_buf) - 1, &bytes_read)) {
            read_buf[bytes_read] = '\0';
            uart_puts("fat: contenido leido -> ");
            uart_puts(read_buf);
        }
    }

    static fat_dirent_t entries[16];
    uint32_t count = fat_list_root(entries, 16);
    uart_puts("fat: contenido del directorio raiz:\n");
    for (uint32_t i = 0; i < count && i < 16; i++) {
        uart_puts("  - ");
        uart_puts(entries[i].name);
        uart_puts(entries[i].is_dir ? " (carpeta)\n" : " (archivo)\n");
    }
}

void kernel_main(void) {
    uart_puts("Nemo OS - kernel ARM64\n");
    uart_puts("Arrancado correctamente en QEMU/UTM (maquina 'virt').\n");

    exceptions_init();

    mmu_init();
    uart_puts("MMU activada (identity mapping, bloques de 1GB).\n");

    void *test = kmalloc(128);
    if (test) {
        uart_puts("kmalloc: 128 bytes asignados correctamente.\n");
    }

    gic_init();
    timer_init();
    __asm__ volatile("msr daifclr, #2");
    uart_puts("Interrupciones y timer activados.\n");

    if (disk_init()) {
        if (nemofs_mount()) {
            int32_t programas_dir = nemofs_ensure_dir(NEMOFS_ROOT_INODE, "PROGRAMAS");
            int32_t documentos_dir = nemofs_ensure_dir(NEMOFS_ROOT_INODE, "DOCUMENTOS");
            int32_t sistema_dir = nemofs_ensure_dir(NEMOFS_ROOT_INODE, "SISTEMA");
            uart_puts("nemofs: carpetas PROGRAMAS/DOCUMENTOS/SISTEMA listas.\n");

            g_programas_dir = (programas_dir >= 0) ? (uint32_t)programas_dir : NEMOFS_ROOT_INODE;

            test_nemofs((documentos_dir >= 0) ? (uint32_t)documentos_dir : NEMOFS_ROOT_INODE);

            if (loader_install_embedded_test()) {
                loader_run_from_nemofs("hello.pro", NEMOFS_ROOT_INODE);
            }
        }
        if (disk_count() >= 2) {
            if (fat_mount(1)) {
                test_fat();
            }
        } else {
            uart_puts("fat: solo se encontro un disco, no se puede probar FAT\n");
        }
    }

    bool fb_ok = ramfb_init();

    uart_puts("checkpoint A: antes de input_init\n");
    bool input_ok = input_init();
    uart_puts("checkpoint B: input_init devolvio ");
    uart_puts(input_ok ? "true\n" : "false\n");

    // sound_init() es independiente de disco/nemofs -- si no
    // encuentra el dispositivo virtio-sound, el resto del driver
    // queda como no-op seguro (sound_available() devuelve false).
    sound_init();

    if (fb_ok) {
        wm_init();

        wm_add_desktop_icon("FILES", "explorer.pro", 30, 30);
        wm_add_desktop_icon("SHELL", "shell.pro", 30, 90);
        wm_add_desktop_icon("EDIT", "editor.pro", 30, 150);
        wm_add_desktop_icon("IDE", "ide.pro", 30, 210);
        loader_install_embedded_explorer(g_programas_dir);
        loader_install_embedded_editor(g_programas_dir);
        loader_install_embedded_gadgetdemo(g_programas_dir);
        loader_install_embedded_nbc(g_programas_dir); // listo desde el primer arranque, para 'run nbc.pro archivo.bb'

        tasks_init();
    }

    if (input_ok) {
        uart_puts("Mueve el raton, haz doble clic en un icono del escritorio,\n");
        uart_puts("o usa el boton START para abrir un programa.\n");
    }

    uart_puts("checkpoint C: entrando al bucle principal\n");

    while (1) {
        char launch_name[32];
        char launch_arg[32];
        int32_t requesting_window = -1;
        uint32_t search_dir = 0xFFFFFFFF;
        if (wm_consume_launch_request(launch_name, sizeof(launch_name), launch_arg, sizeof(launch_arg),
                                       &requesting_window, &search_dir)) {
            if (str_eq(launch_name, "shell.pro")) {
                int32_t win = wm_create_window(300, 100, 380, 260, "SHELL");
                if (win >= 0 && loader_install_embedded_shell(g_programas_dir)) {
                    task_spawn_from_file("shell.pro", g_programas_dir, win, launch_arg, -1);
                }
            } else if (str_eq(launch_name, "explorer.pro")) {
                int32_t win = wm_create_window(260, 100, 480, 300, "FILES");
                if (win >= 0 && loader_install_embedded_explorer(g_programas_dir)) {
                    task_spawn_from_file("explorer.pro", g_programas_dir, win, 0, -1);
                }
            } else if (str_eq(launch_name, "editor.pro")) {
                int32_t win = wm_create_window(280, 120, 420, 300, "EDITOR");
                if (win >= 0 && loader_install_embedded_editor(g_programas_dir)) {
                    task_spawn_from_file("editor.pro", g_programas_dir, win, launch_arg, -1);
                }
            } else if (str_eq(launch_name, "ide.pro")) {
                int32_t win = wm_create_window(240, 90, 520, 360, "IDE");
                if (win >= 0 && loader_install_embedded_ide(g_programas_dir)) {
                    task_spawn_from_file("ide.pro", g_programas_dir, win, launch_arg, -1);
                }
            } else if (str_eq(launch_name, "gadgetdemo.pro")) {
                int32_t win = wm_create_window(300, 140, 260, 260, "GADGETS DEMO");
                if (win >= 0 && loader_install_embedded_gadgetdemo(g_programas_dir)) {
                    task_spawn_from_file("gadgetdemo.pro", g_programas_dir, win, 0, -1);
                }
            } else {
                // Cualquier otro .pro -- por ejemplo, uno recien
                // copiado desde el disco FAT, compilado a mano, o
                // pedido con 'run' desde una carpeta cualquiera. Lo
                // buscamos primero en la carpeta que nos digan
                // (search_dir -- tipicamente la carpeta actual de
                // quien pidio el lanzamiento), luego en la raiz
                // (donde suelen aterrizar las copias desde FAT), y
                // si tampoco, en PROGRAMAS.
                int32_t parent;
                if (search_dir != 0xFFFFFFFF && nemofs_find_child(search_dir, launch_name) >= 0) {
                    parent = (int32_t)search_dir;
                } else if (nemofs_find_child(NEMOFS_ROOT_INODE, launch_name) >= 0) {
                    parent = NEMOFS_ROOT_INODE;
                } else {
                    parent = (int32_t)g_programas_dir;
                }

                if (requesting_window >= 0) {
                    // Lanzado con 'run' desde un programa con ventana
                    // (tipicamente la shell) -- arranca en "modo
                    // consola": SIN ventana propia todavia. Si el
                    // programa nunca llama a nada grafico, se queda
                    // asi para siempre y su Print va a quien lo lanzo.
                    // Si en algun momento SI llama a algo grafico, el
                    // propio kernel le crea una ventana de verdad en
                    // ese instante (ver task_ensure_window).
                    int32_t new_task_id = task_spawn_from_file(launch_name, parent, -1, launch_arg, requesting_window);
                    uart_puts("kernel: task_spawn_from_file devolvio id=");
                    uart_put_dec_signed(new_task_id);
                    uart_puts("\n");
                } else {
                    // Lanzado sin "padre" (icono, menu Start...) --
                    // ventana inmediata, como siempre.
                    int32_t win = wm_create_window(300, 150, 400, 280, launch_name);
                    if (win >= 0) {
                        if (task_spawn_from_file(launch_name, parent, win, launch_arg, -1) < 0) {
                            // El archivo no existe o no se pudo cargar
                            // -- no dejamos una ventana vacia flotando.
                            wm_destroy_window(win);
                        }
                    }
                }
            }
        }

        task_yield();
    }
}
