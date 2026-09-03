// loader.c — Nemo OS
//
// El formato de ejecutable propio de Nemo OS: ".pro" (Nemo PROgram).
// Deliberadamente simple -- una cabecera fija seguida del código máquina
// puro, sin secciones, sin tabla de símbolos, sin nada que un formato
// como ELF necesita para casos mucho más generales que el nuestro.
//
//   Offset 0:  magic[4]      = "NEXE"
//   Offset 4:  version       (u32)
//   Offset 8:  entry_offset  (u32) -- desde donde empieza a ejecutarse
//   Offset 12: code_size     (u32)
//   Offset 16: ... código máquina puro ...
//
// DETALLE IMPORTANTE (y muy real): tras COPIAR código nuevo a memoria,
// hay que decirle explícitamente a la CPU "oye, hay instrucciones
// nuevas aquí". En ARM, la caché de datos y la caché de instrucciones
// pueden estar completamente desincronizadas -- sin este paso, el
// programa cargado podría ejecutar código "viejo" o corrupto de forma
// intermitente.
//
// Desde que existe el planificador de tareas (tasks.c), cada programa
// vivo necesita su PROPIA area de codigo -- ya no vale compartir una
// unica zona de 64KB como cuando solo corria un programa a la vez.
// loader_load_into() es la version reutilizable: cualquiera (incluido
// tasks.c) le puede pasar SU PROPIO buffer de destino.

#include "loader.h"
#include "nemofs.h"
#include "uart.h"
#include "wm.h"

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint32_t version;
    uint32_t entry_offset;
    uint32_t code_size;
} nexe_header_t;

// Solo la usa el camino "sincrono" antiguo (loader_run_in_window),
// que hoy en dia unicamente emplea el test de arranque hello.pro --
// todo lo demas pasa por tasks.c con su propia area por tarea.
#define PROGRAM_AREA_SIZE (64 * 1024)
__attribute__((aligned(4096))) static uint8_t program_area[PROGRAM_AREA_SIZE];

extern const uint8_t _binary_hello_bin_start[];
extern const uint8_t _binary_hello_bin_end[];

static void uart_put_dec(uint32_t value) {
    if (value == 0) {
        uart_putc('0');
        return;
    }
    char digits[10];
    int n = 0;
    while (value > 0) {
        digits[n++] = '0' + (value % 10);
        value /= 10;
    }
    while (n > 0) {
        uart_putc(digits[--n]);
    }
}

// Limpia la cache de datos y refresca la de instrucciones para el
// rango [addr, addr+size). Necesario despues de copiar codigo nuevo a
// memoria y antes de saltar a ejecutarlo.
static void sync_icache(void *addr, uint32_t size) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));

    uint32_t dline = 4u << ((ctr >> 16) & 0xF);
    uint32_t iline = 4u << (ctr & 0xF);

    uint64_t start = (uint64_t)addr;
    uint64_t end = start + size;

    for (uint64_t a = start & ~((uint64_t)dline - 1); a < end; a += dline) {
        __asm__ volatile("dc cvau, %0" :: "r"(a));
    }
    __asm__ volatile("dsb ish");

    for (uint64_t a = start & ~((uint64_t)iline - 1); a < end; a += iline) {
        __asm__ volatile("ic ivau, %0" :: "r"(a));
    }
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}

// Antes este buffer era de 8KB, de sobra para cualquier .pro normal
// -- pero desde que el hueco de memoria de cada tarea paso a ser de
// 6MB (para poder correr el compilador autohospedado), un programa
// grande de verdad ya no cabe ahi, y se estaba truncando en
// silencio. Este buffer vive en el propio kernel (no por tarea), asi
// que solo pagamos este coste una vez.
#define LOADER_FILE_BUF_SIZE (6 * 1024 * 1024 + 4096)

// Carga un archivo .pro desde NemoFS en el buffer 'dest' (de tamaño
// 'dest_size'), y devuelve en '*out_entry' un puntero a funcion listo
// para llamar. No ejecuta nada -- eso lo decide quien la llama
// (tasks.c crea una tarea con ese punto de entrada).
bool loader_load_into(const char *filename, uint32_t parent_inode,
                       uint8_t *dest, uint32_t dest_size,
                       void (**out_entry)(void)) {
    int32_t inode = nemofs_find_child(parent_inode, filename);
    if (inode < 0) {
        uart_puts("loader: archivo no encontrado: ");
        uart_puts(filename);
        uart_puts("\n");
        return false;
    }

    static uint8_t file_buf[LOADER_FILE_BUF_SIZE];
    int32_t bytes = nemofs_read_file((uint32_t)inode, file_buf, sizeof(file_buf));
    if (bytes < (int32_t)sizeof(nexe_header_t)) {
        uart_puts("loader: archivo demasiado pequeño para ser un ejecutable valido\n");
        return false;
    }

    nexe_header_t *hdr = (nexe_header_t *)file_buf;
    if (hdr->magic[0] != 'N' || hdr->magic[1] != 'E' || hdr->magic[2] != 'X' || hdr->magic[3] != 'E') {
        uart_puts("loader: firma NEXE no encontrada\n");
        return false;
    }

    if (hdr->code_size > dest_size) {
        uart_puts("loader: el programa no cabe en el area reservada\n");
        return false;
    }

    const uint8_t *code_src = file_buf + sizeof(nexe_header_t);
    for (uint32_t i = 0; i < hdr->code_size; i++) {
        dest[i] = code_src[i];
    }

    sync_icache(dest, hdr->code_size);

    *out_entry = (void (*)(void))(dest + hdr->entry_offset);
    return true;
}

bool loader_install_embedded_test(void) {
    uint32_t code_size = (uint32_t)(_binary_hello_bin_end - _binary_hello_bin_start);

    static uint8_t file_buf[4096];
    if (sizeof(nexe_header_t) + code_size > sizeof(file_buf)) {
        uart_puts("loader: el programa de prueba embebido es demasiado grande\n");
        return false;
    }

    nexe_header_t *hdr = (nexe_header_t *)file_buf;
    hdr->magic[0] = 'N'; hdr->magic[1] = 'E'; hdr->magic[2] = 'X'; hdr->magic[3] = 'E';
    hdr->version = 1;
    hdr->entry_offset = 0;
    hdr->code_size = code_size;

    for (uint32_t i = 0; i < code_size; i++) {
        file_buf[sizeof(nexe_header_t) + i] = _binary_hello_bin_start[i];
    }

    int32_t inode = nemofs_find_child(NEMOFS_ROOT_INODE, "hello.pro");
    if (inode < 0) {
        inode = nemofs_create(NEMOFS_ROOT_INODE, "hello.pro", NEMOFS_TYPE_FILE);
        if (inode < 0) {
            uart_puts("loader: fallo creando hello.pro\n");
            return false;
        }
    }

    if (!nemofs_write_file((uint32_t)inode, file_buf, sizeof(nexe_header_t) + code_size)) {
        uart_puts("loader: fallo escribiendo hello.pro\n");
        return false;
    }

    uart_puts("loader: hello.pro instalado en NemoFS (");
    uart_put_dec(code_size);
    uart_puts(" bytes de codigo).\n");
    return true;
}

extern const uint8_t _binary_syscall_test_bin_start[];
extern const uint8_t _binary_syscall_test_bin_end[];

extern const uint8_t _binary_shell_bin_start[];
extern const uint8_t _binary_shell_bin_end[];

static bool install_embedded_pro(const uint8_t *code_start, const uint8_t *code_end,
                                  uint32_t parent_inode, const char *filename, const char *label) {
    uint32_t code_size = (uint32_t)(code_end - code_start);

    // NemoFS admite hasta (12 + 128*4) * 512 = 268288 bytes por
    // archivo (12 bloques directos + 4 niveles de bloque indirecto,
    // ver la nota junto a nemofs_inode_t) -- este buffer debe cubrir
    // TODA esa capacidad, o se convertiria en un limite artificial
    // mas estricto que el del sistema de archivos real.
    static uint8_t file_buf[268288];
    if (sizeof(nexe_header_t) + code_size > sizeof(file_buf)) {
        uart_puts("loader: ");
        uart_puts(label);
        uart_puts(" es demasiado grande\n");
        return false;
    }

    nexe_header_t *hdr = (nexe_header_t *)file_buf;
    hdr->magic[0] = 'N'; hdr->magic[1] = 'E'; hdr->magic[2] = 'X'; hdr->magic[3] = 'E';
    hdr->version = 1;
    hdr->entry_offset = 0;
    hdr->code_size = code_size;

    for (uint32_t i = 0; i < code_size; i++) {
        file_buf[sizeof(nexe_header_t) + i] = code_start[i];
    }

    int32_t inode = nemofs_find_child(parent_inode, filename);
    if (inode < 0) {
        inode = nemofs_create(parent_inode, filename, NEMOFS_TYPE_FILE);
        if (inode < 0) {
            uart_puts("loader: fallo creando ");
            uart_puts(filename);
            uart_puts("\n");
            return false;
        }
    }

    if (!nemofs_write_file((uint32_t)inode, file_buf, sizeof(nexe_header_t) + code_size)) {
        uart_puts("loader: fallo escribiendo ");
        uart_puts(filename);
        uart_puts(" (");
        uart_put_dec(code_size);
        uart_puts(" bytes de codigo -- ¿supera el tamaño maximo de archivo?)\n");
        return false;
    }
    return true;
}

bool loader_install_embedded_shell(uint32_t parent_inode) {
    return install_embedded_pro(_binary_shell_bin_start, _binary_shell_bin_end, parent_inode, "shell.pro", "shell.pro");
}

extern const uint8_t _binary_explorer_bin_start[];
extern const uint8_t _binary_explorer_bin_end[];

bool loader_install_embedded_explorer(uint32_t parent_inode) {
    return install_embedded_pro(_binary_explorer_bin_start, _binary_explorer_bin_end, parent_inode, "explorer.pro", "explorer.pro");
}

extern const uint8_t _binary_editor_bin_start[];
extern const uint8_t _binary_editor_bin_end[];

bool loader_install_embedded_editor(uint32_t parent_inode) {
    return install_embedded_pro(_binary_editor_bin_start, _binary_editor_bin_end, parent_inode, "editor.pro", "editor.pro");
}

extern const uint8_t _binary_gadgetdemo_bin_start[];
extern const uint8_t _binary_gadgetdemo_bin_end[];

bool loader_install_embedded_gadgetdemo(uint32_t parent_inode) {
    return install_embedded_pro(_binary_gadgetdemo_bin_start, _binary_gadgetdemo_bin_end, parent_inode, "gadgetdemo.pro", "gadgetdemo.pro");
}

extern const uint8_t _binary_ide_bin_start[];
extern const uint8_t _binary_ide_bin_end[];

bool loader_install_embedded_ide(uint32_t parent_inode) {
    return install_embedded_pro(_binary_ide_bin_start, _binary_ide_bin_end, parent_inode, "ide.pro", "ide.pro");
}

extern const uint8_t _binary_nbc_bin_start[];
extern const uint8_t _binary_nbc_bin_end[];

bool loader_install_embedded_nbc(uint32_t parent_inode) {
    return install_embedded_pro(_binary_nbc_bin_start, _binary_nbc_bin_end, parent_inode, "nbc.pro", "nbc.pro");
}

bool loader_install_embedded_syscall_test(uint32_t parent_inode) {
    return install_embedded_pro(_binary_syscall_test_bin_start, _binary_syscall_test_bin_end,
                                 parent_inode, "syscall_test.pro", "syscall_test.pro");
}

// Camino "sincrono" antiguo -- se queda solo para el test de arranque
// mas temprano (hello.pro), que corre ANTES de que existan ventanas o
// tareas. Todo lo demas usa tasks.c.
bool loader_run_in_window(const char *filename, uint32_t parent_inode, int32_t window_idx) {
    void (*entry)(void) = 0;
    if (!loader_load_into(filename, parent_inode, program_area, PROGRAM_AREA_SIZE, &entry)) {
        return false;
    }

    uart_puts("loader: ejecutando '");
    uart_puts(filename);
    uart_puts("'...\n");

    entry();

    uart_puts("loader: el programa termino, control devuelto al kernel.\n");
    return true;
}

bool loader_run_from_nemofs(const char *filename, uint32_t parent_inode) {
    return loader_run_in_window(filename, parent_inode, -1);
}
