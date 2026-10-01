// kernel.c — Nemo OS
// Punto de entrada en C. Inicializa el UART, las excepciones, la MMU,
// el GIC, el timer, el heap, el disco y NemoFS.

#include "gpio.h"
#include "memoria.h"
#include <stdint.h>
#include "smp.h"
#include "bkl.h"
#include "kernel.h"
#include "embedded_lua.h"
#include "otros_programas.h"
#include "bitacora.h"
#include "uart.h"
#include "nic.h"
#include "net.h"
#include "fonts.h"
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
void fondo_cargar_guardado(void);   // syscall.c: el fondo guardado en FONDO.CFG
#include "icons_data.h"
#include "tasks.h"

// Definida en exceptions.s — registra nuestra tabla de vectores en VBAR_EL1
extern void exceptions_init(void);

// Inodo de la carpeta "PROGRAMAS" -- ahi viven todos los .pro, y ahi
// es donde el bucle principal busca los programas al lanzarlos desde
// un icono o el menu Start. Se rellena una vez en el arranque.
static uint32_t g_programas_dir = 0;
static uint32_t g_sistema_dir = 0;     // /SISTEMA: librerias de Lua
static uint32_t g_accesorios_dir = 0;  // /ACCESORIOS: programas escritos en Lua
static uint32_t g_documentos_dir = 0;  // /DOCUMENTOS: paginas HTML de ejemplo, notas del usuario

// Accesor para otros archivos (tasks.c) -- ver kernel.h.
uint32_t kernel_get_programas_dir(void) { return g_programas_dir; }
uint32_t kernel_get_accesorios_dir(void) { return g_accesorios_dir; }

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

// Lo mismo para un archivo. Mismo motivo: en el segundo arranque ya existe,
// y crearlo a secas devuelve error siempre -- un error falso en el log del
// arranque, que es justo donde menos sitio hay para errores falsos.
static int32_t nemofs_ensure_file(uint32_t parent, const char *name) {
    int32_t idx = nemofs_find_child(parent, name);
    if (idx >= 0) return idx;
    return nemofs_create(parent, name, NEMOFS_TYPE_FILE);
}

static void test_nemofs(uint32_t documentos_dir) {
    // Creamos una carpeta y un archivo dentro de Documentos.
    // En el segundo arranque la carpeta ya existe, asi que la reutilizamos:
    // crearla a secas fallaria siempre y llenaria el log de errores falsos.
    int32_t docs = nemofs_ensure_dir(documentos_dir, "docs");
    if (docs < 0) {
        uart_puts("nemofs: fallo creando carpeta 'docs'\n");
        return;
    }
    uart_puts("nemofs: carpeta 'docs' lista.\n");

    int32_t hello = nemofs_ensure_file((uint32_t)docs, "hello.txt");
    if (hello < 0) {
        uart_puts("nemofs: fallo creando archivo 'hello.txt'\n");
        return;
    }
    uart_puts("nemofs: archivo 'hello.txt' listo dentro de 'docs'.\n");

    const char *content = "Hola desde NemoFS! Este archivo vive dentro de una carpeta anidada.\n";
    uint32_t len = 0;
    while (content[len] != '\0') len++;

    if (!nemofs_write_file_if_changed((uint32_t)hello, content, len)) {
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

    // Igual que en NemoFS: del segundo arranque en adelante ya existe, y
    // crearlo a secas dejaba una linea de "fallo" en un arranque que va bien.
    // Se pregunta primero.
    fat_dirent_t ya;
    if (fat_find_root("HELLO.TXT", &ya)) {
        uart_puts("fat: HELLO.TXT ya estaba, se reutiliza.\n");
    } else if (fat_create_file("HELLO.TXT", content, len)) {
        uart_puts("fat: HELLO.TXT creado y escrito.\n");
    } else {
        uart_puts("fat: fallo creando HELLO.TXT\n");
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
    // Un mismo fuente, dos maquinas: decirlo bien. Esta linea anunciaba
    // "QEMU/UTM" tambien en la Pi, o sea que el log de la placa empezaba
    // con una mentira -- y el log de arranque es lo primero que se lee
    // cuando algo va mal.
    #ifdef NEMO_QEMU
    uart_puts("Arrancado correctamente en QEMU/UTM (maquina 'virt').\n");
    #else
    uart_puts("Arrancado correctamente en Raspberry Pi 4 (BCM2711).\n");
    #endif

    exceptions_init();

    // Diagnostico: comprobar si ya hay un SError pendiente desde
    // el punto mas temprano posible, antes de la MMU, el disco, o
    // cualquier otra cosa.
    uart_puts("kernel: comprobando SError pendiente lo mas "
              "temprano posible...\n");
    __asm__ volatile("msr daifclr, #4");
    __asm__ volatile("isb");
    uart_puts("kernel: sin disparo -- nada pendiente tan "
              "temprano. Continuando arranque normal...\n");

    // Cuanta RAM hay y donde (memoria.c): ANTES de construir las tablas de
    // la MMU, que mapean exactamente eso. En la Pi pregunta al VideoCore
    // por el mailbox con la MMU todavia apagada.
    memoria_detectar();

    mmu_init();

    // Candado grande del kernel (bkl.c, fase 4 del SMP): desde aqui este
    // nucleo esta ejecutando kernel, asi que lo tiene. Lo soltara cuando
    // salte a la primera tarea, y lo recuperara cada vez que un programa
    // entre en el kernel.
    //
    // Justo DESPUES de mmu_init y no antes: el candado usa las
    // instrucciones exclusivas (ldaxr/stxr), y con la MMU apagada la
    // memoria se trata como de dispositivo, donde el monitor exclusivo
    // no esta garantizado -- el stxr podria fallar siempre y el nucleo
    // se quedaria intentandolo para siempre.
    bkl_tomar();
    uart_puts("MMU activada (identity mapping, toda la RAM detectada).\n");
    memoria_informe();
    gpio_init();
    {
        // Escribir y releer (desde la RAM, no la cache) un punto cada 64 MB
        // de la RAM que antes no se usaba: si el mapa estuviera mal, que
        // salte aqui y no dentro de un programa.
        int32_t puntos = memoria_probar_alta();
        if (puntos < 0) uart_puts("memoria: FALLO al comprobar la RAM alta\n");
        else if (puntos == 0) uart_puts("memoria: no hay RAM por encima del GB del kernel\n");
        else uart_puts("memoria: RAM alta comprobada, lectura y escritura correctas\n");

        // Y ahora lo que esa prueba NO puede ver: si la RAM de arriba es un
        // ECO de la de abajo. Escribir y releer la misma direccion pasa aunque
        // la memoria no exista, porque la escritura cae en la copia baja. Si
        // la placa tiene menos RAM de la que dice su revision, esto lo caza
        // AQUI -- antes de que se reparta un solo byte a un programa.
        uint64_t real = memoria_comprobar_eco();
        if (real < memoria_total()) {
            memoria_recortar(real);
            memoria_informe();
        }
    }

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
            int32_t accesorios_dir = nemofs_ensure_dir(NEMOFS_ROOT_INODE, "ACCESORIOS");
            uart_puts("nemofs: carpetas PROGRAMAS/DOCUMENTOS/SISTEMA/ACCESORIOS listas.\n");

            g_programas_dir = (programas_dir >= 0) ? (uint32_t)programas_dir : NEMOFS_ROOT_INODE;
            g_sistema_dir = (sistema_dir >= 0) ? (uint32_t)sistema_dir : NEMOFS_ROOT_INODE;
            g_accesorios_dir = (accesorios_dir >= 0) ? (uint32_t)accesorios_dir : NEMOFS_ROOT_INODE;
            g_documentos_dir = (documentos_dir >= 0) ? (uint32_t)documentos_dir : NEMOFS_ROOT_INODE;

            test_nemofs((documentos_dir >= 0) ? (uint32_t)documentos_dir : NEMOFS_ROOT_INODE);

            if (loader_install_embedded_test()) {
                loader_run_from_nemofs("hello.pro", NEMOFS_ROOT_INODE);
            }
            // syscall_test.pro: existia la funcion de instalacion
            // pero nunca se llamaba desde ningun sitio del arranque
            // -- por eso nunca aparecia en NemoFS. Se instala en la
            // carpeta de programas, igual que shell/explorer/editor,
            // para poder lanzarlo despues con 'run syscall_test.pro'
            // desde el shell.
            loader_install_embedded_syscall_test(g_programas_dir);
        }
        if (disk_count() >= 2) {
            if (fat_mount(1)) {
                test_fat();
            }
        } else {
            uart_puts("fat: solo se encontro un disco, no se puede probar FAT\n");
        }
    }

    // Resolucion de pantalla: se guarda en SCREEN.CFG (raiz de
    // NemoFS), elegible desde Ajustes -> Pantalla. Si no hay nada
    // guardado (primer arranque), se usa la segura de siempre (0,0
    // le dice a ramfb_init que use su valor por defecto) y se deja
    // constancia en disco para que Ajustes tenga algo concreto que
    // mostrar la primera vez.
    uint32_t want_w = 0, want_h = 0;
    {
        int32_t scr_idx = nemofs_find_child(NEMOFS_ROOT_INODE, "SCREEN.CFG");
        if (scr_idx >= 0) {
            uint8_t buf[8];
            if (nemofs_read_file((uint32_t)scr_idx, buf, sizeof(buf)) == 8) {
                want_w = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
                want_h = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
            }
        }
    }

    bool fb_ok = ramfb_init(want_w, want_h);

    // Si no habia SCREEN.CFG, deja constancia en disco de la
    // resolucion con la que realmente se arranco (la segura, si la
    // pedida no existia o fallo) -- asi Ajustes -> Pantalla siempre
    // tiene un valor guardado y coherente que mostrar.
    if (fb_ok && want_w == 0) {
        int32_t scr_idx = nemofs_find_child(NEMOFS_ROOT_INODE, "SCREEN.CFG");
        if (scr_idx < 0) scr_idx = nemofs_create(NEMOFS_ROOT_INODE, "SCREEN.CFG", NEMOFS_TYPE_FILE);
        if (scr_idx >= 0) {
            uint8_t buf[8];
            uint32_t rw = fb_width(), rh = fb_height();
            for (int b = 0; b < 4; b++) buf[b] = (uint8_t)(rw >> (b * 8));
            for (int b = 0; b < 4; b++) buf[4 + b] = (uint8_t)(rh >> (b * 8));
            nemofs_write_file_if_changed((uint32_t)scr_idx, buf, sizeof(buf));
        }
    }


    // Fuentes proporcionales: el paquete FUENTES.NFP viaja embebido en
    // el kernel (fuentes/FUENTES.NFP -> blob) y se lee en el sitio, sin
    // copiar. Si faltara, LoadFont cae a la 5x7 de siempre.
    {
        extern const uint8_t _binary_FUENTES_NFP_start[], _binary_FUENTES_NFP_end[];
        uint32_t n = fonts_init(_binary_FUENTES_NFP_start, (uint32_t)(_binary_FUENTES_NFP_end - _binary_FUENTES_NFP_start));
        uart_puts("fonts: "); { char d[8]; int i = 0; uint32_t v = n; if (v == 0) d[i++] = '0'; while (v) { d[i++] = (char)('0' + v % 10); v /= 10; } while (i) uart_putc(d[--i]); }
        uart_puts(" caras de fuente cargadas (sans/serif/mono, con antialiasing).\n");
    }

    uart_puts("checkpoint A: antes de input_init\n");
    bool input_ok = input_init();
    uart_puts("checkpoint B: input_init devolvio ");
    uart_puts(input_ok ? "true\n" : "false\n");

    // Red. La tarjeta la elige nic.h segun el build: el GENET integrado
    // en la Pi 4 (genet_pi4.c) o una virtio-net en QEMU (virtio_net.c).
    // Encima, la misma pila en los dos: ARP+IPv4+ICMP (net.c), TCP
    // (tcp.c) y la shell remota (netshell.c). Si no hay cable, o el
    // enlace tarda en subir, el sistema sigue arrancando igual -- la red
    // es opcional, nunca un requisito para llegar al escritorio.
    uart_puts("checkpoint B2: antes de nic_init\n");
    bool red_ok = nic_init();
    uart_puts("checkpoint B3: nic_init devolvio ");
    uart_puts(red_ok ? "true\n" : "false\n");
    if (red_ok) net_init();

    // sound_init() es independiente de disco/nemofs -- si no
    // encuentra el dispositivo virtio-sound, el resto del driver
    // queda como no-op seguro (sound_available() devuelve false).
    sound_init();

    // El arranque ya ha terminado de diagnosticar: disco,
    // particiones, USB (dentro de input_init) y red. Se escribe todo
    // lo dicho por el UART en NEMO.LOG de la FAT de arranque, para
    // poder leerlo sacando la tarjeta cuando no hay cable serie a
    // mano -- ver bitacora.h. Va AQUI y no junto a fat_mount porque
    // el USB y la red hablan despues, y son justo lo que interesa.
    if (bitacora_volcar()) {
        uart_puts("bitacora: arranque guardado en NEMO.LOG de la FAT\n");
    }

    if (fb_ok) {
        wm_init();

        // El escritorio se guarda en disco (DESKTOP.CFG) para que lo
        // que el usuario coloque con el editor de escritorio sobreviva
        // al reinicio. Si no hay nada guardado (primer arranque, o SD
        // nueva), se crean los 4 iconos de siempre y se guardan de una
        // vez, para que el PROXIMO arranque ya cargue desde aqui.
        if (!wm_load_desktop_icons()) {
            // Separacion de 100px: con los iconos ahora al doble de
            // tamaño (ver DESKTOP_ICON_SCALE en wm.c), la de antes
            // (60px) se quedaria corta y se solaparian.
            wm_add_desktop_icon_ex("FILES", "explorer.pro", ICON_FOLDER, 30, 30);
            wm_add_desktop_icon_ex("SHELL", "shell.pro", ICON_TERMINAL, 30, 130);
            wm_add_desktop_icon_ex("EDIT", "editor.pro", ICON_TXT, 30, 230);
            wm_add_desktop_icon_ex("IDE", "ide.pro", ICON_CODE, 30, 330);
            wm_add_desktop_icon_ex("AJUSTES", "desktoped.pro", ICON_SETTINGS, 30, 430);
            // Programa aparte, a proposito: la resolucion de pantalla
            // no tiene relacion con colocar iconos, y separarlos deja
            // cada programa mas simple y facil de razonar.
            wm_add_desktop_icon_ex("PANTALLA", "screensettings.pro", ICON_NETWORK, 30, 530);
            // Segunda columna: las aplicaciones de ACCESORIOS escritas en
            // Lua. El kernel redirige un .lua a LUA.PRO al lanzarlo.
            wm_add_desktop_icon_ex("ARONNAX",   "aronnax.lua",   ICON_DESIGN,  150, 30);
            wm_add_desktop_icon_ex("PINTOR",    "pintor.lua",    ICON_PAINT,   150, 130);
            wm_add_desktop_icon_ex("LUA",       "shell.lua",     ICON_LUA,     150, 230);
            wm_add_desktop_icon_ex("NAVEGANTE", "navegante.lua", ICON_COMPASS, 150, 330);
            // La ayuda de usuario. Va en el escritorio, y no solo en el
            // menu, porque es lo primero que busca quien enciende esto
            // por primera vez sin nadie al lado que se lo explique.
            wm_add_desktop_icon_ex("AYUDA",     "ayuda.lua",     ICON_HELP,    150, 430);
            wm_save_desktop_icons();
        } else {
            // Ya habia un escritorio guardado: se le añaden los iconos que
            // el sistema haya empezado a traer desde que se guardo.
            wm_migrar_escritorio();
        }
        // El fondo de pantalla guardado, si lo hay. Va DESPUES de los
        // iconos porque necesita que la pantalla ya tenga su tamaño
        // definitivo: el fondo se escala una vez, al cargarlo.
        fondo_cargar_guardado();
        wm_menu_cargar();   // el menu de Inicio, de MENU.CFG
        loader_install_embedded_explorer(g_programas_dir);
        loader_install_embedded_editor(g_programas_dir);
        loader_install_embedded_gadgetdemo(g_programas_dir);
        loader_install_embedded_desktoped(g_programas_dir);
        loader_install_embedded_screensettings(g_programas_dir);
        loader_install_embedded_nbc(g_programas_dir); // listo desde el primer arranque, para 'run nbc.pro archivo.bb'
        // Lua: interprete en PROGRAMAS, librerias en SISTEMA, programas
        // en ACCESORIOS -- ver embedded_lua.c. Con esto 'run shell.lua',
        // 'run reloj.lua', etc. funcionan desde el primer arranque.
        embedded_lua_install(g_programas_dir, g_sistema_dir, g_accesorios_dir, g_documentos_dir);

        // "otros programas": el codigo fuente en Nemo Basic de los ejemplos
        // grandes (tetris, arkanoid, buscaminas, el juego de plataformas,
        // Timonel, el monitor), cada uno en su carpeta porque usan Include
        // con nombres relativos. Van como FUENTE a proposito: se compilan
        // desde el propio sistema, que es la gracia. Ver otros_programas.h.
        otros_programas_instalar();

        tasks_init();
    }

    if (input_ok) {
        uart_puts("Mueve el raton, haz doble clic en un icono del escritorio,\n");
        uart_puts("o usa el boton START para abrir un programa.\n");
    }

    // Despertar los nucleos 1-3 (SMP, fase 3). Al final del arranque, con
    // todo ya inicializado: los secundarios encienden su MMU apuntando a
    // las tablas del kernel, que tienen que existir ya. En esta fase solo
    // se presentan y se quedan dormidos; no reciben trabajo.
    smp_arrancar();

    uart_puts("checkpoint C: entrando al bucle principal\n");

    while (1) {
        // La memoria de los programas que ya terminaron, de vuelta a la reserva.
        tasks_recoger_terminadas();

        char launch_name[32];
        char launch_arg[TASK_LAUNCH_ARG_MAX];
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
