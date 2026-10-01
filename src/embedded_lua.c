// embedded_lua.c — Nemo OS
// Ver embedded_lua.h. Los simbolos _binary_*_start/_end los genera
// objcopy a partir del NOMBRE del archivo de entrada (puntos y guiones
// pasan a '_'): nemo_gui.lua -> _binary_nemo_gui_lua_start.

#include <stdint.h>
#include <stdbool.h>
#include "embedded_lua.h"
#include "loader.h"
#ifdef NEMO_QEMU
#include "pro_sizes_qemu.h"
#else
#include "pi4/pro_sizes_pi4.h"
#endif
#include "nemofs.h"
#include "uart.h"

// --- el interprete ---
extern const uint8_t _binary_lua_bin_start[];
extern const uint8_t _binary_lua_bin_end[];

// --- librerias (SISTEMA) ---
extern const uint8_t _binary_nemo_gui_lua_start[],       _binary_nemo_gui_lua_end[];
extern const uint8_t _binary_nemo_archivos_lua_start[],  _binary_nemo_archivos_lua_end[];
extern const uint8_t _binary_nemo_sistema_lua_start[],   _binary_nemo_sistema_lua_end[];
extern const uint8_t _binary_nemo_html_lua_start[],      _binary_nemo_html_lua_end[];
extern const uint8_t _binary_nemo_md_lua_start[],        _binary_nemo_md_lua_end[];
extern const uint8_t _binary_nemo_web_lua_start[],       _binary_nemo_web_lua_end[];
extern const uint8_t _binary_nemo_gpio_lua_start[],      _binary_nemo_gpio_lua_end[];
extern const uint8_t _binary_gpio_lua_start[],           _binary_gpio_lua_end[];
extern const uint8_t _binary_aronnax_lua_start[],        _binary_aronnax_lua_end[];
extern const uint8_t _binary_pintor_lua_start[],         _binary_pintor_lua_end[];
extern const uint8_t _binary_navegante_lua_start[],      _binary_navegante_lua_end[];
extern const uint8_t _binary_menued_lua_start[],         _binary_menued_lua_end[];
extern const uint8_t _binary_aronnax_proyecto_lua_start[], _binary_aronnax_proyecto_lua_end[];
extern const uint8_t _binary_nemo_prueba_lua_start[],    _binary_nemo_prueba_lua_end[];
extern const uint8_t _binary_prueba_gpio_lua_start[],    _binary_prueba_gpio_lua_end[];
extern const uint8_t _binary_prueba_pwm_lua_start[],     _binary_prueba_pwm_lua_end[];
extern const uint8_t _binary_prueba_sonido_lua_start[], _binary_prueba_sonido_lua_end[];
extern const uint8_t _binary_prueba_i2c_lua_start[],     _binary_prueba_i2c_lua_end[];
extern const uint8_t _binary_prueba_spi_lua_start[],     _binary_prueba_spi_lua_end[];

// --- programas (ACCESORIOS) ---
extern const uint8_t _binary_shell_lua_start[],           _binary_shell_lua_end[];
extern const uint8_t _binary_editor_lua_start[],          _binary_editor_lua_end[];
extern const uint8_t _binary_calculadora_lua_start[],     _binary_calculadora_lua_end[];
extern const uint8_t _binary_gestor_tareas_lua_start[],   _binary_gestor_tareas_lua_end[];
extern const uint8_t _binary_particionador_lua_start[],  _binary_particionador_lua_end[];   // el reparto de la tarjeta
extern const uint8_t _binary_monitor_sistema_lua_start[], _binary_monitor_sistema_lua_end[];
extern const uint8_t _binary_reloj_lua_start[],           _binary_reloj_lua_end[];
extern const uint8_t _binary_dibujo_lua_start[],          _binary_dibujo_lua_end[];
extern const uint8_t _binary_prueba_memoria_lua_start[],  _binary_prueba_memoria_lua_end[];
extern const uint8_t _binary_prueba_lectura_lua_start[],  _binary_prueba_lectura_lua_end[];
extern const uint8_t _binary_notas_lua_start[],           _binary_notas_lua_end[];
extern const uint8_t _binary_visor_imagenes_lua_start[],  _binary_visor_imagenes_lua_end[];
extern const uint8_t _binary_visor_lua_start[],           _binary_visor_lua_end[];
extern const uint8_t _binary_navegador_lua_start[],       _binary_navegador_lua_end[];   // el navegador web
extern const uint8_t _binary_ayuda_lua_start[],           _binary_ayuda_lua_end[];   // abre la ayuda de usuario en el visor

// --- paginas HTML de ejemplo (DOCUMENTOS) -- las abre visor.lua ---
extern const uint8_t _binary_bienvenida_html_start[],     _binary_bienvenida_html_end[];
extern const uint8_t _binary_ayuda_html_html_start[],     _binary_ayuda_html_html_end[];
extern const uint8_t _binary_demo_lua_html_start[],       _binary_demo_lua_html_end[];

// --- guias del sistema en Markdown (DOCUMENTOS) -- las abre visor.lua via nemo_md ---
extern const uint8_t _binary_GUIA_RED_NEMO_OS_md_start[],  _binary_GUIA_RED_NEMO_OS_md_end[];
extern const uint8_t _binary_GUIA_HTML_NEMO_OS_md_start[], _binary_GUIA_HTML_NEMO_OS_md_end[];
extern const uint8_t _binary_GUIA_PROGRAMACION_LUA_NEMO_OS_md_start[], _binary_GUIA_PROGRAMACION_LUA_NEMO_OS_md_end[];
extern const uint8_t _binary_GUIA_GPIO_NEMO_OS_md_start[], _binary_GUIA_GPIO_NEMO_OS_md_end[];
extern const uint8_t _binary_GUIA_ARONNAX_NEMO_OS_md_start[], _binary_GUIA_ARONNAX_NEMO_OS_md_end[];
extern const uint8_t _binary_GUIA_ICONOS_NEMO_OS_md_start[], _binary_GUIA_ICONOS_NEMO_OS_md_end[];
extern const uint8_t _binary_GUIA_IMAGENES_NIMG_md_start[], _binary_GUIA_IMAGENES_NIMG_md_end[];
extern const uint8_t _binary_MENU_CFG_start[], _binary_MENU_CFG_end[];
extern const uint8_t _binary_nautilus_1280_nimg_start[], _binary_nautilus_1280_nimg_end[];   // el fondo de pantalla
extern const uint8_t _binary_GUIA_AMBITO_NEMO_BASIC_md_start[], _binary_GUIA_AMBITO_NEMO_BASIC_md_end[];
extern const uint8_t _binary_INDICE_MANUALES_md_start[], _binary_INDICE_MANUALES_md_end[];
extern const uint8_t _binary_GUIA_NEMO_BASIC_md_start[], _binary_GUIA_NEMO_BASIC_md_end[];
extern const uint8_t _binary_GUIA_NEMO_BASIC_2_md_start[], _binary_GUIA_NEMO_BASIC_2_md_end[];
extern const uint8_t _binary_GUIA_NEMO_BASIC_3_md_start[], _binary_GUIA_NEMO_BASIC_3_md_end[];
extern const uint8_t _binary_GUIA_NEMO_BASIC_4_md_start[], _binary_GUIA_NEMO_BASIC_4_md_end[];
extern const uint8_t _binary_GUIA_NEMO_BASIC_5_md_start[], _binary_GUIA_NEMO_BASIC_5_md_end[];
extern const uint8_t _binary_REFERENCIA_NEMO_BASIC_md_start[], _binary_REFERENCIA_NEMO_BASIC_md_end[];
extern const uint8_t _binary_REFERENCIA_NEMO_BASIC_2_md_start[], _binary_REFERENCIA_NEMO_BASIC_2_md_end[];
extern const uint8_t _binary_REFERENCIA_NEMO_BASIC_3_md_start[], _binary_REFERENCIA_NEMO_BASIC_3_md_end[];
extern const uint8_t _binary_REFERENCIA_NEMO_BASIC_4_md_start[], _binary_REFERENCIA_NEMO_BASIC_4_md_end[];
extern const uint8_t _binary_REFERENCIA_NEMO_BASIC_5_md_start[], _binary_REFERENCIA_NEMO_BASIC_5_md_end[];
extern const uint8_t _binary_panel_nautilus_anx_start[], _binary_panel_nautilus_anx_end[];
extern const uint8_t _binary_editor_texto_anx_start[], _binary_editor_texto_anx_end[];   // el editor de texto
extern const uint8_t _binary_GUIA_LUA_SYSCALLS_md_start[], _binary_GUIA_LUA_SYSCALLS_md_end[];
extern const uint8_t _binary_GUIA_LUA_SYSCALLS_2_md_start[], _binary_GUIA_LUA_SYSCALLS_2_md_end[];

// La ayuda de usuario: ocho paginas enlazadas entre
// ellas, en DOCUMENTOS/AYUDA. Se llaman ayuda_X.html en el proyecto
// para que el simbolo que genera objcopy no choque con
// ayuda_html.html, que ya existia; se instalan como X.html, que es
// como se enlazan unas con otras.
extern const uint8_t _binary_ayuda_indice_html_start[],    _binary_ayuda_indice_html_end[];
extern const uint8_t _binary_ayuda_primeros_html_start[],  _binary_ayuda_primeros_html_end[];
extern const uint8_t _binary_ayuda_archivos_html_start[],  _binary_ayuda_archivos_html_end[];
extern const uint8_t _binary_ayuda_programas_html_start[], _binary_ayuda_programas_html_end[];
extern const uint8_t _binary_ayuda_ajustes_html_start[],   _binary_ayuda_ajustes_html_end[];
extern const uint8_t _binary_ayuda_teclas_html_start[],    _binary_ayuda_teclas_html_end[];
extern const uint8_t _binary_ayuda_problemas_html_start[], _binary_ayuda_problemas_html_end[];
extern const uint8_t _binary_ayuda_programar_html_start[], _binary_ayuda_programar_html_end[];

// EN_RAIZ: para MENU.CFG, que va en la raiz porque es lo
// que el usuario abre para tocar su menu de Inicio -- enterrarlo en
// SISTEMA seria esconderlo.
//
// EN_MANUALES: la carpeta MANUALES de la raiz. Antes TODA la
// documentacion caia en DOCUMENTOS, que es la carpeta del USUARIO: veinte
// guias .md la dejaban inservible para lo que es. Ahora los manuales del
// sistema tienen su sitio y DOCUMENTOS vuelve a ser del usuario.
//
// EN_AYUDA: MANUALES/AYUDA. La ayuda de
// usuario tambien es documentacion, asi que va con el resto.
typedef enum { EN_SISTEMA, EN_ACCESORIOS, EN_DOCUMENTOS, EN_RAIZ, EN_IMAGENES,
               EN_MANUALES, EN_AYUDA } destino_t;

typedef struct {
    const uint8_t *start, *end;
    const char *nombre;
    destino_t destino;
} lua_file_t;

static const lua_file_t archivos[] = {
    { _binary_nemo_gui_lua_start,       _binary_nemo_gui_lua_end,       "nemo_gui.lua",       EN_SISTEMA },
    { _binary_nemo_archivos_lua_start,  _binary_nemo_archivos_lua_end,  "nemo_archivos.lua",  EN_SISTEMA },
    { _binary_nemo_sistema_lua_start,   _binary_nemo_sistema_lua_end,   "nemo_sistema.lua",   EN_SISTEMA },
    { _binary_nemo_html_lua_start,      _binary_nemo_html_lua_end,      "nemo_html.lua",      EN_SISTEMA },
    { _binary_nemo_md_lua_start,        _binary_nemo_md_lua_end,        "nemo_md.lua",        EN_SISTEMA },
    { _binary_nemo_web_lua_start,       _binary_nemo_web_lua_end,       "nemo_web.lua",       EN_SISTEMA },
    { _binary_nemo_gpio_lua_start,      _binary_nemo_gpio_lua_end,      "nemo_gpio.lua",      EN_SISTEMA },
    { _binary_aronnax_proyecto_lua_start, _binary_aronnax_proyecto_lua_end, "aronnax_proyecto.lua", EN_SISTEMA },
    { _binary_nemo_prueba_lua_start,    _binary_nemo_prueba_lua_end,    "nemo_prueba.lua",    EN_SISTEMA },
    { _binary_shell_lua_start,           _binary_shell_lua_end,           "shell.lua",           EN_ACCESORIOS },
    { _binary_editor_lua_start,          _binary_editor_lua_end,          "editor.lua",          EN_ACCESORIOS },
    { _binary_calculadora_lua_start,     _binary_calculadora_lua_end,     "calculadora.lua",     EN_ACCESORIOS },
    { _binary_gestor_tareas_lua_start,   _binary_gestor_tareas_lua_end,   "gestor_tareas.lua",   EN_ACCESORIOS },
    { _binary_particionador_lua_start,  _binary_particionador_lua_end,  "particionador.lua",  EN_ACCESORIOS },
    { _binary_monitor_sistema_lua_start, _binary_monitor_sistema_lua_end, "monitor_sistema.lua", EN_ACCESORIOS },
    { _binary_reloj_lua_start,           _binary_reloj_lua_end,           "reloj.lua",           EN_ACCESORIOS },
    { _binary_dibujo_lua_start,          _binary_dibujo_lua_end,          "dibujo.lua",          EN_ACCESORIOS },
    { _binary_prueba_memoria_lua_start,  _binary_prueba_memoria_lua_end,  "prueba_memoria.lua",  EN_ACCESORIOS },
    { _binary_prueba_lectura_lua_start,  _binary_prueba_lectura_lua_end,  "prueba_lectura.lua",  EN_ACCESORIOS },
    { _binary_gpio_lua_start,           _binary_gpio_lua_end,           "gpio.lua",           EN_ACCESORIOS },
    { _binary_aronnax_lua_start,        _binary_aronnax_lua_end,        "aronnax.lua",        EN_ACCESORIOS },
    { _binary_pintor_lua_start,         _binary_pintor_lua_end,         "pintor.lua",         EN_ACCESORIOS },
    { _binary_navegante_lua_start,      _binary_navegante_lua_end,      "navegante.lua",      EN_ACCESORIOS },
    { _binary_menued_lua_start,         _binary_menued_lua_end,         "menued.lua",         EN_ACCESORIOS },
    { _binary_prueba_gpio_lua_start,    _binary_prueba_gpio_lua_end,    "prueba_gpio.lua",    EN_ACCESORIOS },
    { _binary_prueba_pwm_lua_start,     _binary_prueba_pwm_lua_end,     "prueba_pwm.lua",     EN_ACCESORIOS },
    { _binary_prueba_sonido_lua_start, _binary_prueba_sonido_lua_end, "prueba_sonido.lua",  EN_ACCESORIOS },
    { _binary_prueba_i2c_lua_start,     _binary_prueba_i2c_lua_end,     "prueba_i2c.lua",     EN_ACCESORIOS },
    { _binary_prueba_spi_lua_start,     _binary_prueba_spi_lua_end,     "prueba_spi.lua",     EN_ACCESORIOS },
    { _binary_notas_lua_start,           _binary_notas_lua_end,           "notas.lua",           EN_ACCESORIOS },
    { _binary_visor_imagenes_lua_start,  _binary_visor_imagenes_lua_end,  "visor_imagenes.lua",  EN_ACCESORIOS },
    { _binary_visor_lua_start,           _binary_visor_lua_end,           "visor.lua",           EN_ACCESORIOS },
    { _binary_navegador_lua_start,       _binary_navegador_lua_end,       "navegador.lua",       EN_ACCESORIOS },
    { _binary_ayuda_lua_start,           _binary_ayuda_lua_end,           "ayuda.lua",           EN_ACCESORIOS },
    // Las ocho paginas de la ayuda de usuario -> DOCUMENTOS/AYUDA
    { _binary_ayuda_indice_html_start,    _binary_ayuda_indice_html_end,    "indice.html",    EN_AYUDA },
    { _binary_ayuda_primeros_html_start,  _binary_ayuda_primeros_html_end,  "primeros.html",  EN_AYUDA },
    { _binary_ayuda_archivos_html_start,  _binary_ayuda_archivos_html_end,  "archivos.html",  EN_AYUDA },
    { _binary_ayuda_programas_html_start, _binary_ayuda_programas_html_end, "programas.html", EN_AYUDA },
    { _binary_ayuda_ajustes_html_start,   _binary_ayuda_ajustes_html_end,   "ajustes.html",   EN_AYUDA },
    { _binary_ayuda_teclas_html_start,    _binary_ayuda_teclas_html_end,    "teclas.html",    EN_AYUDA },
    { _binary_ayuda_problemas_html_start, _binary_ayuda_problemas_html_end, "problemas.html", EN_AYUDA },
    { _binary_ayuda_programar_html_start, _binary_ayuda_programar_html_end, "programar.html", EN_AYUDA },
    { _binary_bienvenida_html_start,     _binary_bienvenida_html_end,     "bienvenida.html",     EN_DOCUMENTOS },
    { _binary_ayuda_html_html_start,     _binary_ayuda_html_html_end,     "ayuda_html.html",     EN_DOCUMENTOS },
    { _binary_demo_lua_html_start,       _binary_demo_lua_html_end,       "demo_lua.html",       EN_DOCUMENTOS },
    { _binary_GUIA_RED_NEMO_OS_md_start,  _binary_GUIA_RED_NEMO_OS_md_end,  "GUIA_RED.md",         EN_MANUALES },
    { _binary_GUIA_HTML_NEMO_OS_md_start, _binary_GUIA_HTML_NEMO_OS_md_end, "GUIA_HTML.md",        EN_MANUALES },
    { _binary_GUIA_PROGRAMACION_LUA_NEMO_OS_md_start, _binary_GUIA_PROGRAMACION_LUA_NEMO_OS_md_end, "GUIA_LUA.md", EN_MANUALES },
    { _binary_GUIA_GPIO_NEMO_OS_md_start, _binary_GUIA_GPIO_NEMO_OS_md_end, "GUIA_GPIO.md", EN_MANUALES },
    { _binary_GUIA_ARONNAX_NEMO_OS_md_start, _binary_GUIA_ARONNAX_NEMO_OS_md_end, "GUIA_ARONNAX.md", EN_MANUALES },
    { _binary_GUIA_ICONOS_NEMO_OS_md_start, _binary_GUIA_ICONOS_NEMO_OS_md_end, "GUIA_ICONOS.md", EN_MANUALES },
    { _binary_GUIA_IMAGENES_NIMG_md_start, _binary_GUIA_IMAGENES_NIMG_md_end, "GUIA_NIMG.md", EN_MANUALES },
    { _binary_MENU_CFG_start, _binary_MENU_CFG_end, "MENU.CFG", EN_RAIZ },
    { _binary_GUIA_AMBITO_NEMO_BASIC_md_start, _binary_GUIA_AMBITO_NEMO_BASIC_md_end, "GUIA_AMBITO.md", EN_MANUALES },
    { _binary_INDICE_MANUALES_md_start, _binary_INDICE_MANUALES_md_end, "INDICE.md", EN_MANUALES },
    { _binary_GUIA_NEMO_BASIC_md_start, _binary_GUIA_NEMO_BASIC_md_end, "GUIA_NEMO_BASIC.md", EN_MANUALES },
    { _binary_GUIA_NEMO_BASIC_2_md_start, _binary_GUIA_NEMO_BASIC_2_md_end, "GUIA_NEMO_BASIC_2.md", EN_MANUALES },
    { _binary_GUIA_NEMO_BASIC_3_md_start, _binary_GUIA_NEMO_BASIC_3_md_end, "GUIA_NEMO_BASIC_3.md", EN_MANUALES },
    { _binary_GUIA_NEMO_BASIC_4_md_start, _binary_GUIA_NEMO_BASIC_4_md_end, "GUIA_NEMO_BASIC_4.md", EN_MANUALES },
    { _binary_GUIA_NEMO_BASIC_5_md_start, _binary_GUIA_NEMO_BASIC_5_md_end, "GUIA_NEMO_BASIC_5.md", EN_MANUALES },
    { _binary_REFERENCIA_NEMO_BASIC_md_start, _binary_REFERENCIA_NEMO_BASIC_md_end, "REFERENCIA_NEMO_BASIC.md", EN_MANUALES },
    { _binary_REFERENCIA_NEMO_BASIC_2_md_start, _binary_REFERENCIA_NEMO_BASIC_2_md_end, "REFERENCIA_NEMO_BASIC_2.md", EN_MANUALES },
    { _binary_REFERENCIA_NEMO_BASIC_3_md_start, _binary_REFERENCIA_NEMO_BASIC_3_md_end, "REFERENCIA_NEMO_BASIC_3.md", EN_MANUALES },
    { _binary_REFERENCIA_NEMO_BASIC_4_md_start, _binary_REFERENCIA_NEMO_BASIC_4_md_end, "REFERENCIA_NEMO_BASIC_4.md", EN_MANUALES },
    { _binary_REFERENCIA_NEMO_BASIC_5_md_start, _binary_REFERENCIA_NEMO_BASIC_5_md_end, "REFERENCIA_NEMO_BASIC_5.md", EN_MANUALES },
    { _binary_panel_nautilus_anx_start, _binary_panel_nautilus_anx_end, "panel_nautilus.anx", EN_DOCUMENTOS },
    { _binary_editor_texto_anx_start, _binary_editor_texto_anx_end, "editor_texto.anx", EN_DOCUMENTOS },
    // La referencia de syscalls, aparte: la guia entera no cabia en los 4 MB
    // de memoria del visor de Lua (medido: tocaba el techo solo con el motor)
    { _binary_GUIA_LUA_SYSCALLS_md_start, _binary_GUIA_LUA_SYSCALLS_md_end, "GUIA_LUA_SYSCALLS.md", EN_MANUALES },
    { _binary_GUIA_LUA_SYSCALLS_2_md_start, _binary_GUIA_LUA_SYSCALLS_2_md_end, "GUIA_LUA_SYSCALLS_2.md", EN_MANUALES },
};

#include "iconos_nimg.h"

// Un numero en decimal por el UART. uart.h solo sabe de cadenas, y aqui hace
// falta contar archivos.
static void uart_dec(uint32_t v) {
    char b[11];
    int i = 0;
    if (v == 0) { uart_puts("0"); return; }
    while (v > 0 && i < (int)sizeof b - 1) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    char s[12];
    int k = 0;
    while (i > 0) s[k++] = b[--i];
    s[k] = '\0';
    uart_puts(s);
}

// Escribe un archivo de texto tal cual (sin cabecera) en NemoFS,
// creandolo o sobreescribiendolo.
static bool install_raw(const uint8_t *start, const uint8_t *end, uint32_t parent, const char *nombre) {
    uint32_t size = (uint32_t)(end - start);
    int32_t inode = nemofs_find_child(parent, nombre);
    if (inode < 0) {
        inode = nemofs_create(parent, nombre, NEMOFS_TYPE_FILE);
        if (inode < 0) { uart_puts("lua: fallo creando "); uart_puts(nombre); uart_puts("\n"); return false; }
    }
    if (!nemofs_write_file_if_changed((uint32_t)inode, start, size)) {
        uart_puts("lua: fallo escribiendo "); uart_puts(nombre); uart_puts("\n");
        return false;
    }
    return true;
}

void embedded_lua_install(uint32_t programas_dir, uint32_t sistema_dir, uint32_t accesorios_dir, uint32_t documentos_dir) {
    // El interprete de Lua es, con diferencia, el programa que mas memoria
    // necesita (~13MB, casi todo su monton). Su tamaño real se mide al
    // compilar, igual que el de los demas programas del sistema.
    loader_install_pro_blob(_binary_lua_bin_start, _binary_lua_bin_end, programas_dir, "LUA.PRO", "LUA.PRO", PRO_MEM_LUA);

    // MANUALES, en la raiz. Antes toda la documentacion iba a
    // DOCUMENTOS, que es la carpeta del USUARIO, y veinte guias la dejaban
    // sin sitio para lo suyo.
    int32_t manuales_dir = nemofs_find_child(NEMOFS_ROOT_INODE, "MANUALES");
    if (manuales_dir < 0) manuales_dir = nemofs_create(NEMOFS_ROOT_INODE, "MANUALES", NEMOFS_TYPE_DIR);
    if (manuales_dir < 0) uart_puts("lua: no se pudo crear MANUALES: los manuales no se instalan\n");

    // MANUALES/AYUDA, antes del bucle: las paginas de la ayuda necesitan su
    // carpeta ya creada. Si no se puede crear, sus entradas se saltan (mejor
    // eso que soltarlas en ACCESORIOS).
    int32_t ayuda_dir = -1;
    if (manuales_dir >= 0) {
        ayuda_dir = nemofs_find_child((uint32_t)manuales_dir, "AYUDA");
        if (ayuda_dir < 0) ayuda_dir = nemofs_create((uint32_t)manuales_dir, "AYUDA", NEMOFS_TYPE_DIR);
    }
    if (ayuda_dir < 0) uart_puts("lua: no se pudo crear MANUALES/AYUDA: la ayuda no se instala\n");

    int ok = 0, total = (int)(sizeof(archivos) / sizeof(archivos[0]));
    for (int i = 0; i < total; i++) {
        if (archivos[i].destino == EN_AYUDA && ayuda_dir < 0) continue;
        if (archivos[i].destino == EN_MANUALES && manuales_dir < 0) continue;
        uint32_t parent = (archivos[i].destino == EN_SISTEMA) ? sistema_dir
                        : (archivos[i].destino == EN_DOCUMENTOS) ? documentos_dir
                        : (archivos[i].destino == EN_RAIZ) ? (uint32_t)NEMOFS_ROOT_INODE
                        : (archivos[i].destino == EN_MANUALES) ? (uint32_t)manuales_dir
                        : (archivos[i].destino == EN_AYUDA) ? (uint32_t)ayuda_dir : accesorios_dir;
        if (install_raw(archivos[i].start, archivos[i].end, parent, archivos[i].nombre)) ok++;
    }
    // La biblioteca de iconos NIMG (tiras de la barra de herramientas e
    // iconos de 32 px), en DOCUMENTOS/IMAGENES (antes, sueltos en
    // DOCUMENTOS; el kernel busca ahora las imagenes tambien en IMAGENES).
    // Las copias que dejo en DOCUMENTOS la instalacion anterior se retiran.
    int32_t imagenes_dir = nemofs_find_child(documentos_dir, "IMAGENES");
    if (imagenes_dir < 0) imagenes_dir = nemofs_create(documentos_dir, "IMAGENES", NEMOFS_TYPE_DIR);
    if (imagenes_dir >= 0) {
        // El fondo de pantalla, junto a los iconos. Y si el
        // usuario todavia no ha elegido ninguno, se deja puesto este: se crea
        // FONDO.CFG solo si NO existe, para no pisar el que haya elegido el.
        install_raw(_binary_nautilus_1280_nimg_start, _binary_nautilus_1280_nimg_end,
                    (uint32_t)imagenes_dir, "nautilus.nimg");
        if (nemofs_find_child(NEMOFS_ROOT_INODE, "FONDO.CFG") < 0) {
            int32_t cfg = nemofs_create(NEMOFS_ROOT_INODE, "FONDO.CFG", NEMOFS_TYPE_FILE);
            if (cfg >= 0) {
                // un byte de modo (0 = estirar a la pantalla) y el nombre
                const uint8_t contenido[] = { 0, 'n','a','u','t','i','l','u','s','.','n','i','m','g' };
                if (nemofs_write_file((uint32_t)cfg, contenido, sizeof contenido))
                    uart_puts("lua: fondo de pantalla por defecto puesto (nautilus.nimg)\n");
            }
        }

        for (uint32_t i = 0; i < ICONOS_NIMG_N; i++) {
            install_raw(ICONOS_NIMG[i].datos, ICONOS_NIMG[i].datos + ICONOS_NIMG[i].tamano, (uint32_t)imagenes_dir, ICONOS_NIMG[i].nombre);
            if (nemofs_find_child(documentos_dir, ICONOS_NIMG[i].nombre) >= 0) nemofs_delete(documentos_dir, ICONOS_NIMG[i].nombre);
        }
    } else {
        uart_puts("lua: no se pudo crear DOCUMENTOS/IMAGENES: los iconos NIMG no se instalan\n");
    }

    // ---- MANUALES: retirar las copias viejas de DOCUMENTOS ----
    //
    // En una tarjeta que ya venia de antes, las veinte guias siguen en
    // DOCUMENTOS aunque ahora se instalen en MANUALES: quedarian duplicadas y
    // el problema que motivo el cambio (DOCUMENTOS lleno) seguiria igual en
    // las tarjetas existentes, arreglado solo en las nuevas.
    //
    // Se borran SOLO los nombres que instala este mismo kernel, y SOLO
    // despues de haber escrito la copia nueva: si algo fallo al instalar en
    // MANUALES, la de DOCUMENTOS se queda donde estaba. Mismo criterio que se
    // uso al mover los iconos NIMG a DOCUMENTOS/IMAGENES.
    if (manuales_dir >= 0) {
        for (int i = 0; i < total; i++) {
            if (archivos[i].destino != EN_MANUALES) continue;
            if (nemofs_find_child((uint32_t)manuales_dir, archivos[i].nombre) < 0) continue;  // la nueva no esta: no se toca la vieja
            if (nemofs_find_child(documentos_dir, archivos[i].nombre) >= 0)
                nemofs_delete(documentos_dir, archivos[i].nombre);
        }
        // Y la AYUDA vieja, que estaba en DOCUMENTOS/AYUDA. La carpeta solo
        // se borra si queda vacia: nemofs_delete no borra carpetas con
        // contenido, asi que si el usuario dejo algo dentro, se respeta.
        int32_t vieja = nemofs_find_child(documentos_dir, "AYUDA");
        if (vieja >= 0 && ayuda_dir >= 0) {
            for (int i = 0; i < total; i++) {
                if (archivos[i].destino != EN_AYUDA) continue;
                if (nemofs_find_child((uint32_t)ayuda_dir, archivos[i].nombre) < 0) continue;
                if (nemofs_find_child((uint32_t)vieja, archivos[i].nombre) >= 0)
                    nemofs_delete((uint32_t)vieja, archivos[i].nombre);
            }
            nemofs_delete(documentos_dir, "AYUDA");
        }
    }

    uart_puts("lua: LUA.PRO en PROGRAMAS, librerias en SISTEMA, programas en ACCESORIOS,\n");
    uart_puts("     paginas HTML y ejemplos en DOCUMENTOS, iconos NIMG en DOCUMENTOS/IMAGENES,\n");
    uart_puts("     guias .md y la ayuda en MANUALES.\n");
    // La cuenta, siempre. Antes este mensaje decia "instalados" pasara lo que
    // pasara, y 'ok' se tiraba: una tarjeta a la que le faltaban archivos
    // arrancaba con el mismo texto que una completa.
    uart_puts("lua: ");
    uart_dec((uint32_t)ok);
    uart_puts(" de ");
    uart_dec((uint32_t)total);
    uart_puts(" archivos instalados");
    uart_puts((ok == total) ? " (todos).\n" : " -- FALTAN ARCHIVOS.\n");
}
