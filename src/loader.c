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
// VERSION 3 -- cabecera ampliada a 24 bytes:
//   Offset 16: mem_size      (u32) -- memoria TOTAL que necesita al
//                                     ejecutarse: codigo + datos +
//                                     .bss. 0 = desconocida.
//   Offset 20: flags         (u32) -- bit 0: aplicacion de CONSOLA
//   Offset 24: ... código máquina puro ...
//
// Existe porque el .bss de un programa no viaja en el archivo (no
// tiene contenido: son ceros), asi que sin este campo el cargador no
// podia saber si el programa CABE de verdad. Solo comprobaba el
// codigo, y un programa que se pasara daba un Data Abort generico en
// vez de un mensaje claro.
//
// Versiones 1 y 2 (cabecera de 16 bytes) siguen siendo validas: 1 es
// escritorio, 2 es consola, y su mem_size se trata como desconocido.
// Asi todos los .pro ya compilados siguen cargando igual.
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

// Tamaños REALES de los programas del sistema, generados al compilar
// por herramientas/pro_sizes.py a partir de sus ELF. Cada build tiene
// los suyos (los binarios de QEMU y de la Pi 4 no son identicos).
#ifdef NEMO_QEMU
#include "pro_sizes_qemu.h"
#else
#include "pi4/pro_sizes_pi4.h"
#endif
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
void sync_icache(void *addr, uint32_t size) {
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
// Cuanta memoria dice necesitar un programa, leyendo SOLO su cabecera.
//
// Hace falta para reservar la region de la tarea ANTES de cargarla:
// el tamaño esta en la cabecera, pero la carga copia el codigo
// directamente en la region, asi que hay que conocerlo primero.
//
// Devuelve el mem_size de una cabecera version 3, o 0 si el programa
// es de version 1/2 (no lo declara) o no se pudo leer. Con 0, quien
// llama usa el tamaño por defecto.
uint32_t loader_required_size(const char *filename, uint32_t parent_inode) {
    int32_t inode = nemofs_find_child(parent_inode, filename);
    if (inode < 0) return 0;
    uint8_t cab[24];
    int32_t bytes = nemofs_read_file((uint32_t)inode, cab, sizeof(cab));
    if (bytes < 24) return 0;
    if (cab[0] != 'N' || cab[1] != 'E' || cab[2] != 'X' || cab[3] != 'E') return 0;
    uint32_t version = *(const uint32_t *)(cab + 4);
    if (version < 3) return 0;
    return *(const uint32_t *)(cab + 16);
}

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
        uart_puts("loader: archivo demasiado pequeno para ser un ejecutable valido\n");
        return false;
    }

    nexe_header_t *hdr = (nexe_header_t *)file_buf;
    if (hdr->magic[0] != 'N' || hdr->magic[1] != 'E' || hdr->magic[2] != 'X' || hdr->magic[3] != 'E') {
        uart_puts("loader: firma NEXE no encontrada\n");
        return false;
    }

    // Tamaño de la cabecera segun la version: 24 bytes desde la 3,
    // 16 en las anteriores.
    uint32_t header_size = (hdr->version >= 3) ? 24u : (uint32_t)sizeof(nexe_header_t);
    uint32_t mem_size = 0;
    if (hdr->version >= 3) {
        if (bytes < (int32_t)header_size) {
            uart_puts("loader: cabecera version 3 incompleta\n");
            return false;
        }
        mem_size = *(const uint32_t *)(file_buf + 16);
    }

    if (hdr->code_size > dest_size) {
        uart_puts("loader: el programa no cabe en el area reservada\n");
        return false;
    }

    // La comprobacion que faltaba: la memoria TOTAL, no solo el
    // codigo. Con numeros, para que se entienda cuanto se pasa.
    if (mem_size > dest_size) {
        uart_puts("loader: el programa necesita ");
        uart_put_dec(mem_size / 1024);
        uart_puts(" KB y el area de cada tarea es de ");
        uart_put_dec(dest_size / 1024);
        uart_puts(" KB\n");
        return false;
    }

    const uint8_t *code_src = file_buf + header_size;
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

    if (!nemofs_write_file_if_changed((uint32_t)inode, file_buf, sizeof(nexe_header_t) + code_size)) {
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

// mem_size: memoria total que necesita el programa (codigo + datos +
// .bss), medida al compilar. Con ella se escribe una cabecera version 3
// y el programa recibe exactamente su tamaño en vez de 16MB a ciegas.
// Con 0 se escribe la version 1 de siempre.
static bool install_embedded_pro(const uint8_t *code_start, const uint8_t *code_end,
                                  uint32_t parent_inode, const char *filename, const char *label,
                                  uint32_t mem_size) {
    uint32_t code_size = (uint32_t)(code_end - code_start);
    uint32_t header_size = mem_size ? 24u : (uint32_t)sizeof(nexe_header_t);

    // NemoFS v2 admite hasta 8656896 bytes por archivo (doble
    // indireccion). Este buffer NO llega a tanto a proposito: los
    // programas embebidos en el kernel son pequeños (el mayor, lua.bin,
    // ~245KB), y 8MB de .bss solo para esto seria un despilfarro. 1MB
    // da 4x de margen sobre lua.bin; si algun dia hace falta mas, la
    // opcion limpia es una escritura por partes (cabecera + cuerpo),
    // no agrandar este buffer.
    static uint8_t file_buf[1024 * 1024];
    if (header_size + code_size > sizeof(file_buf)) {
        uart_puts("loader: ");
        uart_puts(label);
        uart_puts(" es demasiado grande\n");
        return false;
    }

    nexe_header_t *hdr = (nexe_header_t *)file_buf;
    hdr->magic[0] = 'N'; hdr->magic[1] = 'E'; hdr->magic[2] = 'X'; hdr->magic[3] = 'E';
    hdr->version = mem_size ? 3u : 1u;
    hdr->entry_offset = 0;
    hdr->code_size = code_size;
    if (mem_size) {
        uint32_t flags = 0;   // los del sistema se lanzan directos
        *(uint32_t *)(file_buf + 16) = mem_size;
        *(uint32_t *)(file_buf + 20) = flags;
    }

    for (uint32_t i = 0; i < code_size; i++) {
        file_buf[header_size + i] = code_start[i];
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

    if (!nemofs_write_file_if_changed((uint32_t)inode, file_buf, header_size + code_size)) {
        uart_puts("loader: fallo escribiendo ");
        uart_puts(filename);
        uart_puts(" (");
        uart_put_dec(code_size);
        uart_puts(" bytes de codigo -- supera el tamano maximo de archivo?)\n");
        return false;
    }
    return true;
}

// Version publica de install_embedded_pro, para blobs declarados fuera
// de este archivo (embedded_lua.c: lua.pro). Misma logica, mismo
// buffer.
bool loader_install_pro_blob(const uint8_t *code_start, const uint8_t *code_end,
                             uint32_t parent_inode, const char *filename, const char *label,
                             uint32_t mem_size) {
    return install_embedded_pro(code_start, code_end, parent_inode, filename, label, mem_size);
}

bool loader_install_embedded_shell(uint32_t parent_inode) {
    return install_embedded_pro(_binary_shell_bin_start, _binary_shell_bin_end, parent_inode, "shell.pro", "shell.pro", PRO_MEM_SHELL);
}

extern const uint8_t _binary_explorer_bin_start[];
extern const uint8_t _binary_explorer_bin_end[];

bool loader_install_embedded_explorer(uint32_t parent_inode) {
    return install_embedded_pro(_binary_explorer_bin_start, _binary_explorer_bin_end, parent_inode, "explorer.pro", "explorer.pro", PRO_MEM_EXPLORER);
}

extern const uint8_t _binary_desktoped_bin_start[];
extern const uint8_t _binary_desktoped_bin_end[];

bool loader_install_embedded_desktoped(uint32_t parent_inode) {
    return install_embedded_pro(_binary_desktoped_bin_start, _binary_desktoped_bin_end, parent_inode, "desktoped.pro", "desktoped.pro", PRO_MEM_DESKTOPED);
}

extern const uint8_t _binary_screensettings_bin_start[];
extern const uint8_t _binary_screensettings_bin_end[];

bool loader_install_embedded_screensettings(uint32_t parent_inode) {
    return install_embedded_pro(_binary_screensettings_bin_start, _binary_screensettings_bin_end, parent_inode, "screensettings.pro", "screensettings.pro", PRO_MEM_SCREENSETTINGS);
}

extern const uint8_t _binary_editor_bin_start[];
extern const uint8_t _binary_editor_bin_end[];

bool loader_install_embedded_editor(uint32_t parent_inode) {
    return install_embedded_pro(_binary_editor_bin_start, _binary_editor_bin_end, parent_inode, "editor.pro", "editor.pro", PRO_MEM_EDITOR);
}

extern const uint8_t _binary_gadgetdemo_bin_start[];
extern const uint8_t _binary_gadgetdemo_bin_end[];

bool loader_install_embedded_gadgetdemo(uint32_t parent_inode) {
    return install_embedded_pro(_binary_gadgetdemo_bin_start, _binary_gadgetdemo_bin_end, parent_inode, "gadgetdemo.pro", "gadgetdemo.pro", PRO_MEM_GADGETDEMO);
}

extern const uint8_t _binary_ide_bin_start[];
extern const uint8_t _binary_ide_bin_end[];

bool loader_install_embedded_ide(uint32_t parent_inode) {
    return install_embedded_pro(_binary_ide_bin_start, _binary_ide_bin_end, parent_inode, "ide.pro", "ide.pro", PRO_MEM_IDE);
}

extern const uint8_t _binary_nbc_bin_start[];
extern const uint8_t _binary_nbc_bin_end[];

bool loader_install_embedded_nbc(uint32_t parent_inode) {
    return install_embedded_pro(_binary_nbc_bin_start, _binary_nbc_bin_end, parent_inode, "nbc.pro", "nbc.pro", PRO_MEM_NBC);
}

bool loader_install_embedded_syscall_test(uint32_t parent_inode) {
    return install_embedded_pro(_binary_syscall_test_bin_start, _binary_syscall_test_bin_end,
                                 parent_inode, "syscall_test.pro", "syscall_test.pro", PRO_MEM_SYSCALL_TEST);
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
