// dialog.c — Nemo OS
//
// Dialogo comun de "Abrir" y "Guardar como", al estilo de los common
// dialogs clasicos de Windows: barra de titulo, "Buscar en:" con la
// ruta desplegable, barra de lugares a la izquierda, lista con iconos
// y barra de desplazamiento, caja de "Nombre de archivo", filtro de
// tipo y botones Abrir/Guardar y Cancelar.
//
// Reutiliza directamente las funciones del kernel (nemofs_*,
// wm_content_*, input_*) sin pasar por la frontera de syscalls -- al
// fin y al cabo, ya estamos dentro del kernel cuando esto se ejecuta.
//
// ---------------------------------------------------------------
// Por que se reescribio
// ---------------------------------------------------------------
// La version anterior era una lista pelada de seis filas, y ademas:
//
//   1. REPINTABA ENTERO EN CADA VUELTA del bucle, aunque no hubiera
//      pasado NADA: borraba toda la ventana a negro, redibujaba las
//      filas y llamaba a wm_request_redraw(). Con el dialogo abierto,
//      el escritorio entero se recomponia decenas de veces por
//      segundo sin motivo. Ahora hay banderas de "sucio": si el raton
//      no se movio y no se pulso nada, la vuelta del bucle no dibuja
//      NI UN PIXEL, solo cede el turno. Esto es lo que lo hace
//      liviano de verdad, mas que cualquier truco de dibujo.
//
//   2. BORRABA EL CONTENIDO DEL PROGRAMA. Ahora se guarda el trozo de
//      lienzo que tapa el dialogo (wm_content_save_rect) y se
//      restaura al salir: el texto del editor sigue ahi detras, como
//      en cualquier dialogo modal de verdad.
//
//   3. USABA ESTATICOS COMPARTIDOS (entries[], pila_tmp[]) entre las
//      dos funciones. Dos programas con un dialogo abierto a la vez
//      -- perfectamente posible, el sistema es multitarea y aqui
//      dentro se llama a task_yield() -- se pisaban la lista de
//      archivos el uno al otro. Ahora todo el estado vive en un
//      bloque del monton, uno por dialogo abierto.
//
//   4. ERA CODIGO DUPLICADO: abrir y guardar eran dos funciones casi
//      identicas de ~90 lineas cada una. Ahora hay una sola rutina y
//      un booleano.

#include "dialog.h"
#include "nemofs.h"
#include "wm.h"
#include "input.h"
#include "tasks.h"
#include "icons_data.h"
#include "heap.h"
#include "timer.h"

// ---------------------------------------------------------------
// Medidas
// ---------------------------------------------------------------
#define DLG_MAX_CARGA   256 // archivos que se cargan como maximo de una carpeta
#define DLG_PILA_MAX    8   // profundidad maxima de carpetas
#define DLG_MAX_EXT     6   // filtros de tipo distintos que se ofrecen
#define DLG_MAX_LUGARES 6   // atajos de la barra de la izquierda

#define DLG_ANCHO_PREF  520
#define DLG_ALTO_PREF   350
#define DLG_ANCHO_MIN   280
#define DLG_ALTO_MIN    170

#define TIT_H      22   // barra de titulo del dialogo
#define FILA_H     18   // alto de cada fila de la lista
#define ICO        16   // los iconos del sistema son de 24; aqui se reducen a 16
#define BARRA_W    100  // barra de lugares (Raiz, DOCUMENTOS...): 100 es lo
                        // justo para que "DOCUMENTOS" quepa entero a 12px
#define BOTON_W    84
#define BOTON_H    22
#define SCROLL_W   14
#define MARGEN     8

// ---------------------------------------------------------------
// Colores -- paleta clasica, la misma del gestor de ventanas
// ---------------------------------------------------------------
#define C_CARA        0x00D4D0C8
#define C_LUZ         0x00FFFFFF
#define C_SOMBRA      0x00808080
#define C_OSCURO      0x00000000
#define C_TEXTO       0x00000000
#define C_TEXTO_APAG  0x00808080
#define C_CAMPO       0x00FFFFFF
#define C_SEL         0x00000080
#define C_SEL_TEXTO   0x00FFFFFF
#define C_TIT         0x00000080
#define C_TIT_TEXTO   0x00FFFFFF
#define C_LUGARES     0x00505A73
#define C_LUGAR_SEL   0x00707D9E
#define C_LUGAR_TEXTO 0x00FFFFFF
#define C_CERRAR      0x00CC3333

// Partes que hay que repintar (mascara de bits)
#define P_NADA    0
#define P_LISTA   1
#define P_NOMBRE  2
#define P_BOTONES 4
#define P_RUTA    8
#define P_LUGARES 16
#define P_TODO    31

// Zonas sensibles al raton
#define Z_NINGUNA   0
#define Z_LISTA     1
#define Z_ABRIR     2
#define Z_CANCELAR  3
#define Z_SUBIR     4
#define Z_RUTA      5
#define Z_FILTRO    6
#define Z_RUTA_ITEM 12  // una linea del desplegable de "Buscar en:"
#define Z_TIPO_ITEM 13  // una linea del desplegable de "Tipo:"
#define Z_CERRAR    7
#define Z_SCR_ARR   8
#define Z_SCR_ABA   9
#define Z_SCR_POMO  10
#define Z_SCR_PISTA 11
#define Z_LUGAR     100 // Z_LUGAR + i

// Desplegables (campo 'desplegado')
#define D_NADA 0
#define D_RUTA 1
#define D_TIPO 2

// Codigos de tecla (los mismos de input.c, que no los exporta en su
// cabecera). Solo hacen falta las que no llegan como caracter.
#define T_ARRIBA   103
#define T_ABAJO    108
#define T_REPAG    104
#define T_AVPAG    109
#define T_INICIO   102
#define T_FIN      107

typedef struct {
    char nombre[NEMOFS_MAX_NAME + 1];
    uint32_t tam;
    uint8_t tipo;
} dlg_ent_t;

typedef struct {
    int32_t  inodo;
    char     etiqueta[NEMOFS_MAX_NAME + 1];
    int      icono;
} dlg_lugar_t;

// Todo el estado de UN dialogo abierto. Vive en el monton: asi dos
// programas pueden tener uno abierto a la vez sin pisarse (ver la
// nota 3 de la cabecera).
typedef struct {
    int32_t win;
    bool    guardar;

    nemofs_dirent_t bruto[DLG_MAX_CARGA]; // lo que devuelve el sistema de archivos
    dlg_ent_t ent[DLG_MAX_CARGA];         // ya copiado y con el tamaño
    uint16_t  orden[DLG_MAX_CARGA];       // indices de ent[], ya filtrados y ordenados
    int       n_ent, n_vista;

    uint32_t dir;                              // carpeta actual
    uint32_t pila[DLG_PILA_MAX];               // cadena de carpetas hasta la raiz
    char     nom[DLG_PILA_MAX + 1][NEMOFS_MAX_NAME + 1];
    int      prof;                             // cuantas carpetas hay en 'pila'

    dlg_lugar_t lugar[DLG_MAX_LUGARES];
    int      n_lugares;

    int      sel;      // fila seleccionada dentro de orden[], -1 si ninguna
    int      top;      // primera fila visible
    int      filas;    // filas visibles (se recalcula con el tamaño)

    char     ext[DLG_MAX_EXT][8]; // extensiones presentes; ext[0] = "" (todos)
    int      n_ext, filtro;

    char     nombre[NEMOFS_MAX_NAME + 1];
    int      n_nombre;

    int      desplegado;   // cual de los dos desplegables esta abierto
    int      pulsada;      // zona con el boton izquierdo pulsado dentro
    bool     arrastra;     // arrastrando el pomo de la barra
    int      arr_dy;       // desplazamiento dentro del pomo al empezar a arrastrar

    uint64_t t_clic;       // para detectar el doble clic
    int      fila_clic;

    uint8_t *bajo;         // copia del lienzo que tapamos
    uint32_t bajo_w, bajo_h, bajo_x, bajo_y;
} dlg_t;

// Geometria del dialogo, recalculada en cada vuelta (es barata y asi
// el dialogo se adapta solo si la ventana cambia de tamaño).
typedef struct {
    int wx, wy;           // esquina del area de dibujo, en pantalla
    int x, y, w, h;       // dialogo, en coordenadas de la ventana
    bool barra, filtro;   // ¿cabe la barra de lugares? ¿y la fila de filtro?
    int lx, ly, lw, lh;   // lista (por dentro del marco)
    int ruta_x, ruta_y, ruta_w, ruta_h;
    int subir_x, subir_y;
    int caja_x, caja_y, caja_w, caja_h;
    int filtro_x, filtro_y, filtro_w, filtro_h;
    int btn_x, btn1_y, btn2_y;
    int lug_x, lug_y, lug_h;
} geo_t;

// ---------------------------------------------------------------
// Utilidades menudas (aqui no hay libc)
// ---------------------------------------------------------------
static int scopia(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

static char mayus(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

// Compara sin distinguir mayusculas: <0, 0, >0
static int cmp_nom(const char *a, const char *b) {
    while (*a && *b) {
        char ca = mayus(*a), cb = mayus(*b);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static bool seq(const char *a, const char *b) { return cmp_nom(a, b) == 0; }

static int num_a_texto(uint32_t v, char *out) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    return n;
}

// "1 KB", "12 KB", "340 B" -- sin decimales, como el explorador clasico
static void texto_tam(uint32_t bytes, char *out) {
    if (bytes < 1024) {
        int n = num_a_texto(bytes, out);
        scopia(out + n, " B", 4);
    } else {
        int n = num_a_texto((bytes + 1023) / 1024, out);
        scopia(out + n, " KB", 5);
    }
}

// Extension en MAYUSCULAS (sin el punto) de un nombre; "" si no tiene
static void extension(const char *nombre, char *out, int max) {
    int punto = -1, i = 0;
    while (nombre[i]) { if (nombre[i] == '.') punto = i; i++; }
    out[0] = '\0';
    if (punto < 0 || nombre[punto + 1] == '\0') return;
    int j = 0;
    for (i = punto + 1; nombre[i] && j < max - 1; i++) out[j++] = mayus(nombre[i]);
    out[j] = '\0';
}

// Copia 'src' en 'dst' recortandolo con puntos suspensivos si no cabe
// en 'ancho' pixeles. Sin esto los nombres largos se salen del marco:
// wm_content_ui_text no recorta por su cuenta.
static void recortar(char *dst, int max, const char *src, int ancho) {
    int n = scopia(dst, src, max);
    if ((int)wm_ui_text_width(dst) <= ancho) return;
    while (n > 1) {
        n--;
        dst[n] = '\0';
        if (n >= 2) { dst[n - 1] = '.'; dst[n - 2] = '.'; }
        if ((int)wm_ui_text_width(dst) <= ancho) return;
    }
}

// ---------------------------------------------------------------
// Dibujo: biseles al estilo clasico (dos pixeles, cuatro tonos)
// ---------------------------------------------------------------
static void linea_h(int32_t w, int x, int y, int largo, uint32_t c) {
    if (largo > 0) wm_content_fill_rect(w, (uint32_t)x, (uint32_t)y, (uint32_t)largo, 1, c);
}
static void linea_v(int32_t w, int x, int y, int largo, uint32_t c) {
    if (largo > 0) wm_content_fill_rect(w, (uint32_t)x, (uint32_t)y, 1, (uint32_t)largo, c);
}

static void marco(int32_t w, int x, int y, int an, int al,
                  uint32_t ext_ai, uint32_t int_ai, uint32_t int_bd, uint32_t ext_bd) {
    linea_h(w, x, y, an, ext_ai);
    linea_v(w, x, y, al, ext_ai);
    linea_h(w, x + 1, y + 1, an - 2, int_ai);
    linea_v(w, x + 1, y + 1, al - 2, int_ai);
    linea_h(w, x + 1, y + al - 2, an - 2, int_bd);
    linea_v(w, x + an - 2, y + 1, al - 2, int_bd);
    linea_h(w, x, y + al - 1, an, ext_bd);
    linea_v(w, x + an - 1, y, al, ext_bd);
}

// Saliente (botones, fondo del dialogo) / hundido (campos, lista)
static void saliente(int32_t w, int x, int y, int an, int al) {
    marco(w, x, y, an, al, C_LUZ, C_CARA, C_SOMBRA, C_OSCURO);
}
static void hundido(int32_t w, int x, int y, int an, int al) {
    marco(w, x, y, an, al, C_SOMBRA, C_OSCURO, C_CARA, C_LUZ);
}

// Boton completo: cara + bisel + texto centrado. 'pulsado' lo hunde y
// desplaza el texto un pixel, como los de toda la vida.
static void boton(int32_t w, int x, int y, int an, int al, const char *texto, bool pulsado, bool marcado) {
    wm_content_fill_rect(w, (uint32_t)x, (uint32_t)y, (uint32_t)an, (uint32_t)al, C_CARA);
    if (pulsado) hundido(w, x, y, an, al); else saliente(w, x, y, an, al);
    if (marcado && !pulsado) { // el boton por defecto lleva un borde negro extra
        linea_h(w, x, y, an, C_OSCURO); linea_h(w, x, y + al - 1, an, C_OSCURO);
        linea_v(w, x, y, al, C_OSCURO); linea_v(w, x + an - 1, y, al, C_OSCURO);
    }
    int tw = (int)wm_ui_text_width(texto);
    int th = (int)wm_ui_text_height();
    int tx = x + (an - tw) / 2 + (pulsado ? 1 : 0);
    int ty = y + (al - th) / 2 + (pulsado ? 1 : 0);
    wm_content_ui_text(w, (uint32_t)tx, (uint32_t)ty, texto, C_TEXTO);
}

// Triangulito de los desplegables y de la barra de desplazamiento.
// dir: 0 abajo, 1 arriba, 2 izquierda, 3 derecha
static void flecha(int32_t w, int cx, int cy, int dir, uint32_t c) {
    for (int i = 0; i < 4; i++) {
        int largo = 7 - i * 2;
        if (dir == 0)      linea_h(w, cx - 3 + i, cy - 2 + i, largo, c);
        else if (dir == 1) linea_h(w, cx - 3 + i, cy + 2 - i, largo, c);
        else if (dir == 2) linea_v(w, cx + 2 - i, cy - 3 + i, largo, c);
        else               linea_v(w, cx - 2 + i, cy - 3 + i, largo, c);
    }
}

// Flecha de "subir un nivel": mas grande que las de los desplegables,
// con base, para que se lea de un vistazo a 12px.
static void flecha_subir(int32_t w, int cx, int cy, uint32_t c) {
    for (int i = 0; i < 5; i++) linea_h(w, cx - i, cy - 4 + i, 1 + i * 2, c);
    wm_content_fill_rect(w, (uint32_t)(cx - 1), (uint32_t)(cy + 1), 3, 3, c);
    linea_h(w, cx - 4, cy + 5, 9, c);
}

// ---------------------------------------------------------------
// Iconos a 16x16
// ---------------------------------------------------------------
// Los iconos embebidos son de 24x24 y solo hay ampliacion entera
// (wm_content_blit_icon_scaled). Una lista con filas de 24 pixeles se
// ve enorme y cabe la mitad de archivos, asi que aqui se reducen a 16
// con media de area (cada pixel de salida = media de 1 o 2 de
// entrada, alternando: 8*(1+2) = 24). Se hace UNA vez por icono y se
// guarda en cache: dibujar la lista despues es solo copiar.
static uint8_t cache16[8][ICO * ICO * 4];
static int     cache_id[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };

static const uint8_t *icono16(int id) {
    for (int i = 0; i < 8; i++) if (cache_id[i] == id) return cache16[i];
    const uint8_t *src = icon_get_rgba(id);
    if (!src) return 0;
    int hueco = 0;
    for (int i = 0; i < 8; i++) if (cache_id[i] < 0) { hueco = i; break; }
    uint8_t *dst = cache16[hueco];
    for (int oy = 0; oy < ICO; oy++) {
        int y0 = oy * 3 / 2, y1 = (oy + 1) * 3 / 2;
        for (int ox = 0; ox < ICO; ox++) {
            int x0 = ox * 3 / 2, x1 = (ox + 1) * 3 / 2;
            // Se acumula con el alfa premultiplicado: si no, los
            // bordes transparentes tiñen de negro el contorno.
            uint32_t ar = 0, ag = 0, ab = 0, aa = 0, n = 0;
            for (int sy = y0; sy < y1; sy++) {
                for (int sx = x0; sx < x1; sx++) {
                    const uint8_t *p = &src[((uint32_t)sy * ICON_SIZE + (uint32_t)sx) * 4];
                    ar += (uint32_t)p[0] * p[3];
                    ag += (uint32_t)p[1] * p[3];
                    ab += (uint32_t)p[2] * p[3];
                    aa += p[3];
                    n++;
                }
            }
            uint8_t *o = &dst[((uint32_t)oy * ICO + (uint32_t)ox) * 4];
            if (aa == 0 || n == 0) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            o[0] = (uint8_t)(ar / aa);
            o[1] = (uint8_t)(ag / aa);
            o[2] = (uint8_t)(ab / aa);
            o[3] = (uint8_t)(aa / n);
        }
    }
    cache_id[hueco] = id;
    return dst;
}

// Pega un icono de 16x16 mezclandolo sobre un color liso conocido
// (blanco de la lista, azul de la seleccion...). No hace falta leer el
// lienzo: sabemos lo que hay debajo.
static void pintar_icono(int32_t w, int x, int y, int id, uint32_t fondo) {
    const uint8_t *px = icono16(id);
    if (!px) return;
    uint8_t fr = (uint8_t)(fondo >> 16), fg = (uint8_t)(fondo >> 8), fb = (uint8_t)fondo;
    for (int iy = 0; iy < ICO; iy++) {
        for (int ix = 0; ix < ICO; ix++) {
            const uint8_t *p = &px[((uint32_t)iy * ICO + (uint32_t)ix) * 4];
            uint8_t a = p[3];
            if (a == 0) continue;
            uint8_t r = (uint8_t)((p[0] * a + fr * (255 - a)) / 255);
            uint8_t g = (uint8_t)((p[1] * a + fg * (255 - a)) / 255);
            uint8_t b = (uint8_t)((p[2] * a + fb * (255 - a)) / 255);
            wm_content_put_pixel(w, (uint32_t)(x + ix), (uint32_t)(y + iy),
                                 ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
        }
    }
}

static int icono_de(const dlg_ent_t *e) {
    if (e->tipo == NEMOFS_TYPE_DIR) return ICON_FOLDER;
    char ext[8];
    extension(e->nombre, ext, sizeof(ext));
    if (seq(ext, "PRO")) return ICON_PROGRAM;
    if (seq(ext, "NB") || seq(ext, "LUA") || seq(ext, "C") || seq(ext, "H") || seq(ext, "BIT")) return ICON_CODE;
    if (seq(ext, "NIMG") || seq(ext, "BMP")) return ICON_IMAGE;
    if (seq(ext, "NFP") || seq(ext, "CFG")) return ICON_SETTINGS;
    if (seq(ext, "HTML") || seq(ext, "MD")) return ICON_BOOK;
    return ICON_TXT;
}

// ---------------------------------------------------------------
// Carpetas: cadena hasta la raiz, carga, filtro y orden
// ---------------------------------------------------------------
// El dialogo arranca en la carpeta que pide el programa (DOCUMENTOS
// casi siempre) SIN saber por donde se llega a ella, asi que hay que
// reconstruir la cadena buscandola desde la raiz. El buffer de la
// busqueda es temporal y se pide al monton: son ~90KB que antes
// estaban reservados para siempre en el kernel.
typedef struct { nemofs_dirent_t e[DLG_PILA_MAX][DLG_MAX_CARGA]; } busca_t;

static bool buscar_camino(busca_t *b, dlg_t *d, uint32_t carpeta, uint32_t objetivo, int nivel) {
    if (nivel >= DLG_PILA_MAX) return false;
    uint32_t n = nemofs_list_dir(carpeta, b->e[nivel], DLG_MAX_CARGA);
    if (n > DLG_MAX_CARGA) n = DLG_MAX_CARGA;
    for (uint32_t i = 0; i < n; i++) {
        if (b->e[nivel][i].type != NEMOFS_TYPE_DIR) continue;
        d->pila[nivel] = carpeta;
        if (b->e[nivel][i].inode == objetivo) {
            scopia(d->nom[nivel + 1], b->e[nivel][i].name, NEMOFS_MAX_NAME + 1);
            d->prof = nivel + 1;
            return true;
        }
        scopia(d->nom[nivel + 1], b->e[nivel][i].name, NEMOFS_MAX_NAME + 1);
        if (buscar_camino(b, d, b->e[nivel][i].inode, objetivo, nivel + 1)) return true;
    }
    return false;
}

static void cadena_hasta(dlg_t *d, uint32_t dir) {
    d->prof = 0;
    scopia(d->nom[0], "Ra\xC3\xADz", NEMOFS_MAX_NAME + 1);
    if (dir == NEMOFS_ROOT_INODE) return;
    busca_t *b = (busca_t *)kmalloc(sizeof(busca_t));
    if (!b) return; // sin memoria: se navega igual, solo que sin poder subir
    if (!buscar_camino(b, d, NEMOFS_ROOT_INODE, dir, 0)) d->prof = 0;
    kfree(b);
}

// Rehace orden[] a partir de ent[]: quita lo que no pasa el filtro,
// pone las carpetas primero y ordena por nombre (insercion; con 256
// entradas como mucho es instantaneo y no gasta memoria extra).
static void rehacer_vista(dlg_t *d) {
    d->n_vista = 0;
    for (int i = 0; i < d->n_ent; i++) {
        if (d->ent[i].tipo != NEMOFS_TYPE_DIR && d->filtro > 0) {
            char ext[8];
            extension(d->ent[i].nombre, ext, sizeof(ext));
            if (!seq(ext, d->ext[d->filtro])) continue;
        }
        d->orden[d->n_vista++] = (uint16_t)i;
    }
    for (int i = 1; i < d->n_vista; i++) {
        uint16_t v = d->orden[i];
        int j = i - 1;
        while (j >= 0) {
            const dlg_ent_t *a = &d->ent[d->orden[j]], *bb = &d->ent[v];
            bool a_dir = (a->tipo == NEMOFS_TYPE_DIR), b_dir = (bb->tipo == NEMOFS_TYPE_DIR);
            bool mayor = (a_dir != b_dir) ? (b_dir && !a_dir) : (cmp_nom(a->nombre, bb->nombre) > 0);
            if (!mayor) break;
            d->orden[j + 1] = d->orden[j];
            j--;
        }
        d->orden[j + 1] = v;
    }
}

// Recoge las extensiones presentes para el desplegable "Tipo". Asi el
// filtro siempre ofrece algo util sin que el programa tenga que
// pasarlo por la syscall (que no tiene hueco para ello).
static void recoger_ext(dlg_t *d) {
    d->n_ext = 1;
    d->ext[0][0] = '\0';
    for (int i = 0; i < d->n_ent && d->n_ext < DLG_MAX_EXT; i++) {
        if (d->ent[i].tipo == NEMOFS_TYPE_DIR) continue;
        char ext[8];
        extension(d->ent[i].nombre, ext, sizeof(ext));
        if (!ext[0]) continue;
        bool ya = false;
        for (int j = 1; j < d->n_ext; j++) if (seq(d->ext[j], ext)) { ya = true; break; }
        if (!ya) scopia(d->ext[d->n_ext++], ext, 8);
    }
    if (d->filtro >= d->n_ext) d->filtro = 0;
}

static void cargar(dlg_t *d, uint32_t dir) {
    d->dir = dir;
    uint32_t total = nemofs_list_dir(dir, d->bruto, DLG_MAX_CARGA);
    if (total > DLG_MAX_CARGA) total = DLG_MAX_CARGA;
    d->n_ent = (int)total;
    for (int i = 0; i < d->n_ent; i++) {
        scopia(d->ent[i].nombre, d->bruto[i].name, NEMOFS_MAX_NAME + 1);
        d->ent[i].tipo = (uint8_t)d->bruto[i].type;
        d->ent[i].tam  = d->bruto[i].size;
    }
    recoger_ext(d);
    rehacer_vista(d);
    d->sel = -1;
    d->top = 0;
    d->fila_clic = -1;
}

static void entrar(dlg_t *d, uint32_t hija, const char *nombre) {
    if (d->prof < DLG_PILA_MAX) {
        d->pila[d->prof] = d->dir;
        scopia(d->nom[d->prof + 1], nombre, NEMOFS_MAX_NAME + 1);
        d->prof++;
    }
    cargar(d, hija);
}

static void subir(dlg_t *d) {
    if (d->prof <= 0) return;
    d->prof--;
    cargar(d, d->pila[d->prof]);
}

// Salta a una carpeta cualquiera de la cadena (desplegable "Buscar
// en:") o a un atajo de la barra de lugares.
static void ir_a(dlg_t *d, uint32_t dir) {
    cadena_hasta(d, dir);
    cargar(d, dir);
}

static void montar_lugares(dlg_t *d) {
    d->n_lugares = 0;
    d->lugar[0].inodo = (int32_t)NEMOFS_ROOT_INODE;
    scopia(d->lugar[0].etiqueta, "Ra\xC3\xADz", NEMOFS_MAX_NAME + 1);
    d->lugar[0].icono = ICON_DISK;
    d->n_lugares = 1;

    nemofs_dirent_t *raiz = (nemofs_dirent_t *)kmalloc(sizeof(nemofs_dirent_t) * DLG_MAX_CARGA);
    if (!raiz) return;
    uint32_t n = nemofs_list_dir(NEMOFS_ROOT_INODE, raiz, DLG_MAX_CARGA);
    if (n > DLG_MAX_CARGA) n = DLG_MAX_CARGA;
    for (uint32_t i = 0; i < n && d->n_lugares < DLG_MAX_LUGARES; i++) {
        if (raiz[i].type != NEMOFS_TYPE_DIR) continue;
        d->lugar[d->n_lugares].inodo = (int32_t)raiz[i].inode;
        scopia(d->lugar[d->n_lugares].etiqueta, raiz[i].name, NEMOFS_MAX_NAME + 1);
        d->lugar[d->n_lugares].icono = ICON_FOLDER;
        d->n_lugares++;
    }
    kfree(raiz);
}

// ---------------------------------------------------------------
// Geometria
// ---------------------------------------------------------------
static bool medir(dlg_t *d, geo_t *g) {
    int32_t wx, wy;
    uint32_t ww, wh;
    if (!wm_get_window_client_rect(d->win, &wx, &wy, &ww, &wh)) return false;
    g->wx = wx; g->wy = wy;

    g->w = DLG_ANCHO_PREF;
    g->h = DLG_ALTO_PREF;
    if (g->w > (int)ww - 8) g->w = (int)ww - 8;
    if (g->h > (int)wh - 8) g->h = (int)wh - 8;
    if (g->w < DLG_ANCHO_MIN) g->w = DLG_ANCHO_MIN;
    if (g->h < DLG_ALTO_MIN)  g->h = DLG_ALTO_MIN;
    g->x = ((int)ww - g->w) / 2; if (g->x < 0) g->x = 0;
    g->y = ((int)wh - g->h) / 2; if (g->y < 0) g->y = 0;

    // En ventanas estrechas se cae la barra de lugares, y en las bajas
    // la fila del filtro: el dialogo sigue siendo usable en una
    // ventana pequeña en vez de salirse por los bordes.
    g->barra  = (g->w >= 420);
    g->filtro = (g->h >= 260);

    int x0 = g->x + MARGEN;
    int ancho_util = g->w - MARGEN * 2;

    // Fila de arriba: "Buscar en:" + ruta + boton de subir
    int et_w = (int)wm_ui_text_width("Buscar en:") + 6;
    g->ruta_h = 22;
    g->ruta_y = g->y + TIT_H + 8;
    g->ruta_x = x0 + et_w;
    g->subir_y = g->ruta_y;
    g->subir_x = g->x + g->w - MARGEN - 24;
    g->ruta_w = g->subir_x - 6 - g->ruta_x;

    // Fila(s) de abajo
    int filas_abajo = g->filtro ? 2 : 1;
    int alto_abajo = filas_abajo * (BOTON_H + 6) + 4;
    g->btn_x  = g->x + g->w - MARGEN - BOTON_W;
    g->btn1_y = g->y + g->h - MARGEN - alto_abajo + 4;
    g->btn2_y = g->btn1_y + BOTON_H + 6;

    int cx = x0 + (int)wm_ui_text_width("Nombre:") + 6;
    if (g->barra) cx = x0 + BARRA_W + 6 + (int)wm_ui_text_width("Nombre:") + 6;
    g->caja_x = cx;
    g->caja_y = g->btn1_y;
    // Sin fila de filtro, "Cancelar" se pone al lado de "Abrir": la
    // caja del nombre tiene que terminar antes, no debajo.
    g->caja_w = (g->filtro ? g->btn_x : (g->btn_x - BOTON_W - 6)) - 8 - cx;
    if (g->caja_w < 40) g->caja_w = 40;
    g->caja_h = BOTON_H;
    g->filtro_x = cx;
    g->filtro_y = g->btn2_y;
    g->filtro_w = g->caja_w;
    g->filtro_h = BOTON_H;

    // Barra de lugares y lista, entre una cosa y la otra
    int arriba = g->ruta_y + g->ruta_h + 6;
    int abajo  = g->btn1_y - 8;
    g->lug_x = x0;
    g->lug_y = arriba;
    g->lug_h = abajo - arriba;

    int lx = g->barra ? (x0 + BARRA_W + 6) : x0;
    g->lx = lx + 2;
    g->ly = arriba + 2;
    g->lw = (x0 + ancho_util) - lx - 4;
    g->lh = abajo - arriba - 4;
    if (g->lh < FILA_H) g->lh = FILA_H;

    d->filas = g->lh / FILA_H;
    if (d->filas < 1) d->filas = 1;
    return true;
}

static int max_scroll(const dlg_t *d) {
    int m = d->n_vista - d->filas;
    return (m > 0) ? m : 0;
}

static void ajustar_scroll(dlg_t *d) {
    if (d->top > max_scroll(d)) d->top = max_scroll(d);
    if (d->top < 0) d->top = 0;
}

// Deja visible la fila seleccionada (para las flechas del teclado)
static void ver_seleccion(dlg_t *d) {
    if (d->sel < 0) return;
    if (d->sel < d->top) d->top = d->sel;
    if (d->sel >= d->top + d->filas) d->top = d->sel - d->filas + 1;
    ajustar_scroll(d);
}

// ---------------------------------------------------------------
// Pintado
// ---------------------------------------------------------------
static void etiqueta_filtro(const dlg_t *d, int i, char *out, int max) {
    if (i <= 0) { scopia(out, "Todos los archivos (*.*)", max); return; }
    int n = scopia(out, "Archivos *.", max);
    scopia(out + n, d->ext[i], max - n);
}

// Cuantas lineas tiene cada desplegable y donde cae su recuadro. El
// de la ruta se abre hacia abajo; el del tipo hacia ARRIBA, porque
// esta en la ultima fila del dialogo y hacia abajo no hay sitio.
static int desplegable_items(const dlg_t *d, int cual) {
    return (cual == D_RUTA) ? d->prof + 1 : d->n_ext;
}

static void desplegable_rect(const dlg_t *d, const geo_t *g, int cual, int *x, int *y, int *w, int *h) {
    int n = desplegable_items(d, cual);
    if (n < 1) n = 1;
    *h = n * FILA_H + 4;
    if (cual == D_RUTA) {
        *x = g->ruta_x; *w = g->ruta_w; *y = g->ruta_y + g->ruta_h;
    } else {
        *x = g->filtro_x; *w = g->filtro_w; *y = g->filtro_y - *h;
        int techo = g->y + TIT_H;
        if (*y < techo) *y = techo;
    }
}

static void pintar_scroll(dlg_t *d, const geo_t *g) {
    int sx = g->lx + g->lw - SCROLL_W;
    int sy = g->ly, sh = g->lh;
    if (d->n_vista <= d->filas) { // sin desbordar: pista lisa y sin pomo
        wm_content_fill_rect(d->win, (uint32_t)sx, (uint32_t)sy, SCROLL_W, (uint32_t)sh, C_CARA);
        return;
    }
    wm_content_fill_rect(d->win, (uint32_t)sx, (uint32_t)sy, SCROLL_W, (uint32_t)sh, 0x00E8E4DC);
    // Botones de los extremos
    wm_content_fill_rect(d->win, (uint32_t)sx, (uint32_t)sy, SCROLL_W, SCROLL_W, C_CARA);
    if (d->pulsada == Z_SCR_ARR) hundido(d->win, sx, sy, SCROLL_W, SCROLL_W); else saliente(d->win, sx, sy, SCROLL_W, SCROLL_W);
    flecha(d->win, sx + SCROLL_W / 2, sy + SCROLL_W / 2, 1, C_TEXTO);
    int by = sy + sh - SCROLL_W;
    wm_content_fill_rect(d->win, (uint32_t)sx, (uint32_t)by, SCROLL_W, SCROLL_W, C_CARA);
    if (d->pulsada == Z_SCR_ABA) hundido(d->win, sx, by, SCROLL_W, SCROLL_W); else saliente(d->win, sx, by, SCROLL_W, SCROLL_W);
    flecha(d->win, sx + SCROLL_W / 2, by + SCROLL_W / 2, 0, C_TEXTO);
    // Pomo
    int pista = sh - SCROLL_W * 2;
    if (pista < 8) return;
    int alto = pista * d->filas / d->n_vista;
    if (alto < 12) alto = 12;
    if (alto > pista) alto = pista;
    int desp = (max_scroll(d) > 0) ? (pista - alto) * d->top / max_scroll(d) : 0;
    int py = sy + SCROLL_W + desp;
    wm_content_fill_rect(d->win, (uint32_t)sx, (uint32_t)py, SCROLL_W, (uint32_t)alto, C_CARA);
    saliente(d->win, sx, py, SCROLL_W, alto);
}

static void pintar_lista(dlg_t *d, const geo_t *g) {
    int ancho_lista = g->lw - SCROLL_W;
    wm_content_fill_rect(d->win, (uint32_t)g->lx, (uint32_t)g->ly, (uint32_t)ancho_lista, (uint32_t)g->lh, C_CAMPO);

    int th = (int)wm_ui_text_height();
    char buf[NEMOFS_MAX_NAME + 8];
    char tam[16];

    for (int f = 0; f < d->filas; f++) {
        int i = d->top + f;
        if (i >= d->n_vista) break;
        const dlg_ent_t *e = &d->ent[d->orden[i]];
        int fy = g->ly + f * FILA_H;
        bool sel = (i == d->sel);
        uint32_t fondo = sel ? C_SEL : C_CAMPO;
        uint32_t tinta = sel ? C_SEL_TEXTO : C_TEXTO;
        if (sel) wm_content_fill_rect(d->win, (uint32_t)g->lx, (uint32_t)fy, (uint32_t)ancho_lista, FILA_H, C_SEL);

        pintar_icono(d->win, g->lx + 3, fy + (FILA_H - ICO) / 2, icono_de(e), fondo);

        // El tamaño va pegado a la derecha, como en la vista de
        // detalles del explorador; el nombre se recorta si hace falta.
        int col_tam = 0;
        if (e->tipo != NEMOFS_TYPE_DIR) {
            texto_tam(e->tam, tam);
            col_tam = (int)wm_ui_text_width(tam);
        }
        int ancho_nom = ancho_lista - (ICO + 8) - (col_tam ? col_tam + 12 : 6);
        recortar(buf, sizeof(buf), e->nombre, ancho_nom);
        wm_content_ui_text(d->win, (uint32_t)(g->lx + ICO + 7), (uint32_t)(fy + (FILA_H - th) / 2), buf, tinta);
        if (col_tam) {
            wm_content_ui_text(d->win, (uint32_t)(g->lx + ancho_lista - col_tam - 6),
                               (uint32_t)(fy + (FILA_H - th) / 2), tam,
                               sel ? C_SEL_TEXTO : C_TEXTO_APAG);
        }
    }
    if (d->n_vista == 0) {
        wm_content_ui_text(d->win, (uint32_t)(g->lx + 6), (uint32_t)(g->ly + 5),
                           "(no hay archivos aqu\xC3\xAD)", C_TEXTO_APAG);
    }
    pintar_scroll(d, g);
}

static void pintar_ruta(dlg_t *d, const geo_t *g) {
    char buf[NEMOFS_MAX_NAME + 8];
    int th = (int)wm_ui_text_height();
    wm_content_fill_rect(d->win, (uint32_t)g->ruta_x, (uint32_t)g->ruta_y, (uint32_t)g->ruta_w, (uint32_t)g->ruta_h, C_CAMPO);
    hundido(d->win, g->ruta_x, g->ruta_y, g->ruta_w, g->ruta_h);
    pintar_icono(d->win, g->ruta_x + 3, g->ruta_y + (g->ruta_h - ICO) / 2,
                 (d->prof == 0) ? ICON_DISK : ICON_FOLDER, C_CAMPO);
    recortar(buf, sizeof(buf), d->nom[d->prof], g->ruta_w - ICO - 34);
    wm_content_ui_text(d->win, (uint32_t)(g->ruta_x + ICO + 7), (uint32_t)(g->ruta_y + (g->ruta_h - th) / 2), buf, C_TEXTO);
    // Boton del desplegable, pegado por dentro al borde derecho
    int bx = g->ruta_x + g->ruta_w - 18, by = g->ruta_y + 2;
    wm_content_fill_rect(d->win, (uint32_t)bx, (uint32_t)by, 16, (uint32_t)(g->ruta_h - 4), C_CARA);
    if (d->desplegado == D_RUTA) hundido(d->win, bx, by, 16, g->ruta_h - 4); else saliente(d->win, bx, by, 16, g->ruta_h - 4);
    flecha(d->win, bx + 8, by + (g->ruta_h - 4) / 2, 0, C_TEXTO);
}

static void pintar_nombre(dlg_t *d, const geo_t *g) {
    int th = (int)wm_ui_text_height();
    char buf[NEMOFS_MAX_NAME + 8];
    wm_content_fill_rect(d->win, (uint32_t)g->caja_x, (uint32_t)g->caja_y, (uint32_t)g->caja_w, (uint32_t)g->caja_h, C_CAMPO);
    hundido(d->win, g->caja_x, g->caja_y, g->caja_w, g->caja_h);
    recortar(buf, sizeof(buf), d->nombre, g->caja_w - 14);
    int tx = g->caja_x + 5, ty = g->caja_y + (g->caja_h - th) / 2;
    wm_content_ui_text(d->win, (uint32_t)tx, (uint32_t)ty, buf, C_TEXTO);
    // Se puede teclear tambien al abrir (no solo al guardar), como en
    // cualquier dialogo de estos: escribir el nombre y pulsar Enter.
    int cx = tx + (int)wm_ui_text_width(buf) + 1;
    if (cx < g->caja_x + g->caja_w - 4) linea_v(d->win, cx, ty, th, C_TEXTO);
}

static void pintar_botones(dlg_t *d, const geo_t *g) {
    boton(d->win, g->btn_x, g->btn1_y, BOTON_W, BOTON_H, d->guardar ? "Guardar" : "Abrir",
          d->pulsada == Z_ABRIR, true);
    if (g->filtro) {
        boton(d->win, g->btn_x, g->btn2_y, BOTON_W, BOTON_H, "Cancelar", d->pulsada == Z_CANCELAR, false);
        char buf[48], rec[48];
        etiqueta_filtro(d, d->filtro, buf, sizeof(buf));
        wm_content_fill_rect(d->win, (uint32_t)g->filtro_x, (uint32_t)g->filtro_y, (uint32_t)g->filtro_w, (uint32_t)g->filtro_h, C_CAMPO);
        hundido(d->win, g->filtro_x, g->filtro_y, g->filtro_w, g->filtro_h);
        int th = (int)wm_ui_text_height();
        recortar(rec, sizeof(rec), buf, g->filtro_w - 28);
        wm_content_ui_text(d->win, (uint32_t)(g->filtro_x + 5), (uint32_t)(g->filtro_y + (g->filtro_h - th) / 2), rec, C_TEXTO);
        int bx = g->filtro_x + g->filtro_w - 18, by = g->filtro_y + 2;
        wm_content_fill_rect(d->win, (uint32_t)bx, (uint32_t)by, 16, (uint32_t)(g->filtro_h - 4), C_CARA);
        if (d->desplegado == D_TIPO) hundido(d->win, bx, by, 16, g->filtro_h - 4);
        else saliente(d->win, bx, by, 16, g->filtro_h - 4);
        flecha(d->win, bx + 8, by + (g->filtro_h - 4) / 2, 0, C_TEXTO);
    } else {
        // Sin sitio para dos filas: Cancelar va al lado de Abrir
        boton(d->win, g->btn_x - BOTON_W - 6, g->btn1_y, BOTON_W, BOTON_H, "Cancelar", d->pulsada == Z_CANCELAR, false);
    }
}

static void pintar_lugares(dlg_t *d, const geo_t *g) {
    if (!g->barra) return;
    wm_content_fill_rect(d->win, (uint32_t)g->lug_x, (uint32_t)g->lug_y, BARRA_W, (uint32_t)g->lug_h, C_LUGARES);
    hundido(d->win, g->lug_x, g->lug_y, BARRA_W, g->lug_h);
    char buf[NEMOFS_MAX_NAME + 8];
    for (int i = 0; i < d->n_lugares; i++) {
        int y = g->lug_y + 4 + i * 38;
        if (y + 36 > g->lug_y + g->lug_h) break;
        bool aqui = ((uint32_t)d->lugar[i].inodo == d->dir);
        uint32_t fondo = aqui ? C_LUGAR_SEL : C_LUGARES;
        if (aqui) wm_content_fill_rect(d->win, (uint32_t)(g->lug_x + 2), (uint32_t)y, BARRA_W - 4, 36, C_LUGAR_SEL);
        pintar_icono(d->win, g->lug_x + (BARRA_W - ICO) / 2, y + 2, d->lugar[i].icono, fondo);
        recortar(buf, sizeof(buf), d->lugar[i].etiqueta, BARRA_W - 8);
        int tw = (int)wm_ui_text_width(buf);
        wm_content_ui_text(d->win, (uint32_t)(g->lug_x + (BARRA_W - tw) / 2), (uint32_t)(y + 20),
                           buf, C_LUGAR_TEXTO);
    }
}

static void pintar_todo(dlg_t *d, const geo_t *g) {
    int th = (int)wm_ui_text_height();
    // Cuerpo y barra de titulo
    wm_content_fill_rect(d->win, (uint32_t)g->x, (uint32_t)g->y, (uint32_t)g->w, (uint32_t)g->h, C_CARA);
    saliente(d->win, g->x, g->y, g->w, g->h);
    wm_content_fill_rect(d->win, (uint32_t)(g->x + 3), (uint32_t)(g->y + 3), (uint32_t)(g->w - 6), TIT_H - 4, C_TIT);
    wm_content_ui_text(d->win, (uint32_t)(g->x + 8), (uint32_t)(g->y + 3 + (TIT_H - 4 - th) / 2),
                       d->guardar ? "Guardar como" : "Abrir", C_TIT_TEXTO);
    int cx = g->x + g->w - 3 - 18, cy = g->y + 5;
    wm_content_fill_rect(d->win, (uint32_t)cx, (uint32_t)cy, 16, 14, C_CERRAR);
    if (d->pulsada == Z_CERRAR) hundido(d->win, cx, cy, 16, 14); else saliente(d->win, cx, cy, 16, 14);
    wm_content_ui_text(d->win, (uint32_t)(cx + 4), (uint32_t)(cy + (14 - th) / 2), "\xC3\x97", C_TIT_TEXTO);

    // Etiquetas
    wm_content_ui_text(d->win, (uint32_t)(g->x + MARGEN), (uint32_t)(g->ruta_y + (g->ruta_h - th) / 2), "Buscar en:", C_TEXTO);
    int et_x = g->caja_x - (int)wm_ui_text_width("Nombre:") - 6;
    wm_content_ui_text(d->win, (uint32_t)et_x, (uint32_t)(g->caja_y + (g->caja_h - th) / 2), "Nombre:", C_TEXTO);
    if (g->filtro) {
        int tx = g->filtro_x - (int)wm_ui_text_width("Tipo:") - 6;
        wm_content_ui_text(d->win, (uint32_t)tx, (uint32_t)(g->filtro_y + (g->filtro_h - th) / 2), "Tipo:", C_TEXTO);
    }

    // Boton de subir un nivel
    wm_content_fill_rect(d->win, (uint32_t)g->subir_x, (uint32_t)g->subir_y, 24, (uint32_t)g->ruta_h, C_CARA);
    if (d->pulsada == Z_SUBIR) hundido(d->win, g->subir_x, g->subir_y, 24, g->ruta_h);
    else saliente(d->win, g->subir_x, g->subir_y, 24, g->ruta_h);
    flecha_subir(d->win, g->subir_x + 12, g->subir_y + g->ruta_h / 2 - 2,
                 d->prof > 0 ? C_TEXTO : C_SOMBRA);

    hundido(d->win, g->lx - 2, g->ly - 2, g->lw + 4, g->lh + 4);
    pintar_ruta(d, g);
    pintar_lugares(d, g);
    pintar_lista(d, g);
    pintar_nombre(d, g);
    pintar_botones(d, g);
}

// Los dos desplegables. El de "Buscar en:" muestra la cadena de
// carpetas desde la raiz, con sangria; el de "Tipo:" la lista de
// extensiones presentes en la carpeta. La linea actual va marcada.
static void pintar_desplegable(dlg_t *d, const geo_t *g) {
    int cual = d->desplegado;
    if (cual == D_NADA) return;
    int x, y, an, alto;
    desplegable_rect(d, g, cual, &x, &y, &an, &alto);
    int n = desplegable_items(d, cual);
    int actual = (cual == D_RUTA) ? d->prof : d->filtro;

    wm_content_fill_rect(d->win, (uint32_t)x, (uint32_t)y, (uint32_t)an, (uint32_t)alto, C_CAMPO);
    hundido(d->win, x, y, an, alto);
    int th = (int)wm_ui_text_height();
    char buf[64];
    for (int i = 0; i < n; i++) {
        int fy = y + 2 + i * FILA_H;
        bool sel = (i == actual);
        if (sel) wm_content_fill_rect(d->win, (uint32_t)(x + 2), (uint32_t)fy, (uint32_t)(an - 4), FILA_H, C_SEL);
        int sangria = (cual == D_RUTA) ? (4 + i * 10) : 4;
        if (cual == D_RUTA) {
            pintar_icono(d->win, x + sangria, fy + (FILA_H - ICO) / 2, i == 0 ? ICON_DISK : ICON_FOLDER,
                         sel ? C_SEL : C_CAMPO);
            recortar(buf, sizeof(buf), d->nom[i], an - sangria - ICO - 10);
            wm_content_ui_text(d->win, (uint32_t)(x + sangria + ICO + 4), (uint32_t)(fy + (FILA_H - th) / 2),
                               buf, sel ? C_SEL_TEXTO : C_TEXTO);
        } else {
            char et[48];
            etiqueta_filtro(d, i, et, sizeof(et));
            recortar(buf, sizeof(buf), et, an - 12);
            wm_content_ui_text(d->win, (uint32_t)(x + sangria), (uint32_t)(fy + (FILA_H - th) / 2),
                               buf, sel ? C_SEL_TEXTO : C_TEXTO);
        }
    }
}

static void pintar(dlg_t *d, const geo_t *g, int partes) {
    if (partes == P_TODO) { pintar_todo(d, g); }
    else {
        if (partes & P_LISTA)   pintar_lista(d, g);
        if (partes & P_NOMBRE)  pintar_nombre(d, g);
        if (partes & P_BOTONES) pintar_botones(d, g);
        if (partes & P_RUTA)    pintar_ruta(d, g);
        if (partes & P_LUGARES) pintar_lugares(d, g);
    }
    if (d->desplegado != D_NADA) pintar_desplegable(d, g);
    wm_request_redraw();
}

// ---------------------------------------------------------------
// Raton: en que zona esta el puntero
// ---------------------------------------------------------------
static bool dentro(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int zona(dlg_t *d, const geo_t *g, int x, int y, int *fila) {
    *fila = -1;
    // Un desplegable abierto se queda con todo lo que caiga dentro de
    // el, por encima de lo que haya debajo.
    if (d->desplegado != D_NADA) {
        int dx, dy, dw, dh;
        desplegable_rect(d, g, d->desplegado, &dx, &dy, &dw, &dh);
        if (dentro(x, y, dx, dy, dw, dh)) {
            int n = desplegable_items(d, d->desplegado);
            *fila = (y - dy - 2) / FILA_H;
            if (*fila < 0) *fila = 0;
            if (*fila >= n) *fila = n - 1;
            return (d->desplegado == D_RUTA) ? Z_RUTA_ITEM : Z_TIPO_ITEM;
        }
    }
    if (dentro(x, y, g->x + g->w - 21, g->y + 5, 16, 14)) return Z_CERRAR;
    if (dentro(x, y, g->subir_x, g->subir_y, 24, g->ruta_h)) return Z_SUBIR;
    if (dentro(x, y, g->ruta_x, g->ruta_y, g->ruta_w, g->ruta_h)) return Z_RUTA;
    if (dentro(x, y, g->btn_x, g->btn1_y, BOTON_W, BOTON_H)) return Z_ABRIR;
    if (g->filtro) {
        if (dentro(x, y, g->btn_x, g->btn2_y, BOTON_W, BOTON_H)) return Z_CANCELAR;
        if (dentro(x, y, g->filtro_x, g->filtro_y, g->filtro_w, g->filtro_h)) return Z_FILTRO;
    } else if (dentro(x, y, g->btn_x - BOTON_W - 6, g->btn1_y, BOTON_W, BOTON_H)) return Z_CANCELAR;

    if (g->barra && dentro(x, y, g->lug_x, g->lug_y, BARRA_W, g->lug_h)) {
        int i = (y - g->lug_y - 4) / 38;
        if (i >= 0 && i < d->n_lugares) { *fila = i; return Z_LUGAR + i; }
        return Z_NINGUNA;
    }

    int sx = g->lx + g->lw - SCROLL_W;
    if (dentro(x, y, sx, g->ly, SCROLL_W, g->lh)) {
        if (d->n_vista <= d->filas) return Z_NINGUNA;
        if (y < g->ly + SCROLL_W) return Z_SCR_ARR;
        if (y >= g->ly + g->lh - SCROLL_W) return Z_SCR_ABA;
        int pista = g->lh - SCROLL_W * 2;
        int alto = pista * d->filas / d->n_vista;
        if (alto < 12) alto = 12;
        int desp = (max_scroll(d) > 0) ? (pista - alto) * d->top / max_scroll(d) : 0;
        int py = g->ly + SCROLL_W + desp;
        if (y >= py && y < py + alto) { *fila = y - py; return Z_SCR_POMO; }
        return Z_SCR_PISTA;
    }
    if (dentro(x, y, g->lx, g->ly, g->lw - SCROLL_W, g->lh)) {
        int f = d->top + (y - g->ly) / FILA_H;
        if (f >= 0 && f < d->n_vista) *fila = f;
        return Z_LISTA;
    }
    return Z_NINGUNA;
}

// Mueve el pomo arrastrando: convierte la posicion del raton en fila
static void arrastrar(dlg_t *d, const geo_t *g, int y) {
    int pista = g->lh - SCROLL_W * 2;
    int alto = pista * d->filas / d->n_vista;
    if (alto < 12) alto = 12;
    int recorrido = pista - alto;
    if (recorrido <= 0) return;
    int rel = y - (g->ly + SCROLL_W) - d->arr_dy;
    if (rel < 0) rel = 0;
    if (rel > recorrido) rel = recorrido;
    d->top = rel * max_scroll(d) / recorrido;
    ajustar_scroll(d);
}

// ---------------------------------------------------------------
// Confirmar la eleccion
// ---------------------------------------------------------------
// Devuelve 1 si hay que terminar (resultado en *res), 0 si no.
// Un nombre que resulta ser una carpeta -- lo este por seleccion o
// escrito a mano -- no termina el dialogo: entra en ella, igual que
// el "Abrir" de toda la vida.
static int confirmar(dlg_t *d, int64_t *res, char *out, uint32_t out_max, int *partes) {
    if (d->n_nombre == 0) return 0;

    int32_t inodo = nemofs_find_child(d->dir, d->nombre);
    if (inodo >= 0 && nemofs_type_by_inode((uint32_t)inodo) == NEMOFS_TYPE_DIR) {
        char nombre[NEMOFS_MAX_NAME + 1];
        scopia(nombre, d->nombre, sizeof(nombre));
        entrar(d, (uint32_t)inodo, nombre);
        d->nombre[0] = '\0';
        d->n_nombre = 0;
        *partes = P_TODO;
        return 0;
    }
    if (inodo < 0) {
        if (!d->guardar) return 0;  // abrir algo que no existe: no se hace nada
        inodo = nemofs_create(d->dir, d->nombre, NEMOFS_TYPE_FILE);
        if (inodo < 0) return 0;
    }
    scopia(out, d->nombre, (int)out_max);
    *res = ((int64_t)(uint32_t)d->dir << 32) | (uint32_t)inodo;
    return 1;
}

// Al marcar una fila, su nombre pasa a la caja: asi "Abrir" y Enter
// trabajan siempre sobre lo mismo que se ve escrito, se haya elegido
// con el raton, con las flechas o tecleando.
static void seleccionar(dlg_t *d, int fila) {
    d->sel = fila;
    if (fila < 0 || fila >= d->n_vista) return;
    d->n_nombre = scopia(d->nombre, d->ent[d->orden[fila]].nombre, NEMOFS_MAX_NAME + 1);
}

// ---------------------------------------------------------------
// El dialogo
// ---------------------------------------------------------------
static int64_t dialogo(int32_t win, uint32_t dir_inicial, char *out, uint32_t out_max, bool guardar) {
    if (!out || out_max == 0) return -1;
    out[0] = '\0';

    dlg_t *d = (dlg_t *)kmalloc(sizeof(dlg_t));
    if (!d) return -1;   // sin memoria: se cancela, mejor que pintar medio dialogo
    for (uint32_t i = 0; i < sizeof(dlg_t); i++) ((uint8_t *)d)[i] = 0;
    d->win = win;
    d->guardar = guardar;
    d->sel = -1;
    d->pulsada = Z_NINGUNA;
    d->fila_clic = -1;

    montar_lugares(d);
    cadena_hasta(d, dir_inicial);
    cargar(d, dir_inicial);

    geo_t g;
    if (!medir(d, &g)) { kfree(d); return -1; }

    // Copia de lo que tapamos, para devolverlo al salir (ver nota 2)
    d->bajo_x = (uint32_t)g.x; d->bajo_y = (uint32_t)g.y;
    d->bajo_w = (uint32_t)g.w; d->bajo_h = (uint32_t)g.h;
    d->bajo = (uint8_t *)kmalloc((size_t)g.w * (size_t)g.h * 4);
    if (d->bajo && !wm_content_save_rect(win, d->bajo_x, d->bajo_y, d->bajo_w, d->bajo_h, d->bajo)) {
        kfree(d->bajo);
        d->bajo = 0;
    }

    // Que no entre en el dialogo un clic o una tecla de antes
    input_flush_keys();
    input_flush_mouse();

    int64_t res = -1;
    bool terminar = false;
    int partes = P_TODO;
    bool ultimo_izq = false;
    int ultimo_ancho = g.w, ultimo_alto = g.h;

    while (!terminar) {
        if (partes != P_NADA) {
            pintar(d, &g, partes);
            partes = P_NADA;
        }

        task_yield();

        geo_t ng;
        if (!medir(d, &ng)) break;       // ventana cerrada: se cancela
        if (ng.w != ultimo_ancho || ng.h != ultimo_alto || ng.x != g.x || ng.y != g.y) {
            // La ventana cambio de tamaño: el dialogo se recoloca. La
            // copia de debajo ya no vale para la zona nueva, asi que
            // se suelta (el programa repinta al volver, como siempre).
            if (d->bajo) { kfree(d->bajo); d->bajo = 0; }
            ultimo_ancho = ng.w; ultimo_alto = ng.h;
            partes = P_TODO;
        }
        g = ng;
        ajustar_scroll(d);

        if (win != wm_get_focused_window()) continue;

        int mx = mouse_x() - g.wx;
        int my = mouse_y() - g.wy;
        int fila = -1;
        int z = zona(d, &g, mx, my, &fila);

        // --- rueda ---
        int32_t rueda = mouse_wheel_delta();
        if (rueda != 0 && d->n_vista > d->filas) {
            d->top -= rueda * 3;
            ajustar_scroll(d);
            partes |= P_LISTA;
        }

        // --- raton ---
        bool izq = mouse_left_down();

        if (d->arrastra) {
            if (izq) {
                int antes = d->top;
                arrastrar(d, &g, my);
                if (antes != d->top) partes |= P_LISTA;
            } else {
                d->arrastra = false;
                d->pulsada = Z_NINGUNA;
                partes |= P_LISTA;
            }
        } else if (izq && !ultimo_izq) {
            // --- pulsacion ---
            if (z == Z_RUTA_ITEM) {                // elegir carpeta de la ruta
                d->desplegado = D_NADA;
                if (fila >= 0 && fila < d->prof) { d->prof = fila; cargar(d, d->pila[fila]); }
                partes = P_TODO;
            } else if (z == Z_TIPO_ITEM) {         // elegir tipo de archivo
                d->desplegado = D_NADA;
                if (fila >= 0 && fila < d->n_ext && fila != d->filtro) {
                    d->filtro = fila;
                    rehacer_vista(d);
                    d->sel = -1;
                    d->top = 0;
                }
                partes = P_TODO;
            } else if (d->desplegado != D_NADA) {  // clic fuera: se cierra
                d->desplegado = D_NADA;
                partes = P_TODO;
            } else if (z == Z_RUTA) {
                d->desplegado = D_RUTA;
                partes = P_RUTA;
            } else if (z == Z_FILTRO) {
                d->desplegado = D_TIPO;
                partes = P_BOTONES;
            } else if (z == Z_CERRAR || z == Z_SUBIR) {
                d->pulsada = z;                    // se resuelve al soltar
                partes = P_TODO;                   // se dibujan en el marco general
            } else if (z == Z_CANCELAR || z == Z_ABRIR) {
                d->pulsada = z;
                partes |= P_BOTONES;
            } else if (z == Z_SCR_ARR || z == Z_SCR_ABA) {
                d->pulsada = z;
                partes |= P_LISTA;
            } else if (z == Z_SCR_POMO) {
                d->arrastra = true;
                d->arr_dy = fila;                  // desfase dentro del pomo
            } else if (z == Z_SCR_PISTA) {
                d->top += (my < g.ly + g.lh / 2) ? -d->filas : d->filas;
                ajustar_scroll(d);
                partes |= P_LISTA;
            } else if (z >= Z_LUGAR) {
                int i = z - Z_LUGAR;
                if (i < d->n_lugares && d->lugar[i].inodo >= 0) {
                    ir_a(d, (uint32_t)d->lugar[i].inodo);
                    partes = P_TODO;
                }
            } else if (z == Z_LISTA) {
                uint64_t ahora = timer_micros();
                bool doble = (fila >= 0 && fila == d->fila_clic && (ahora - d->t_clic) < 400000ULL);
                d->t_clic = ahora;
                d->fila_clic = fila;
                seleccionar(d, fila);
                partes |= P_LISTA | P_NOMBRE;
                if (doble && fila >= 0 && confirmar(d, &res, out, out_max, &partes)) terminar = true;
            }
        } else if (!izq && ultimo_izq) {
            // --- soltar ---
            int p = d->pulsada;
            d->pulsada = Z_NINGUNA;
            if (p != Z_NINGUNA) {
                if (p == z) {
                    if (p == Z_CERRAR || p == Z_CANCELAR) { res = -1; terminar = true; }
                    else if (p == Z_ABRIR) {
                        if (confirmar(d, &res, out, out_max, &partes)) terminar = true;
                    } else if (p == Z_SUBIR) { subir(d); partes = P_TODO; }
                    else if (p == Z_SCR_ARR) { d->top--; ajustar_scroll(d); }
                    else if (p == Z_SCR_ABA) { d->top++; ajustar_scroll(d); }
                }
                if (!terminar && partes != P_TODO) partes |= P_BOTONES | P_LISTA;
            }
        }
        ultimo_izq = izq;

        if (terminar) break;

        // --- teclado: moverse por la lista ---
        int mover = 0;
        if (key_was_hit(T_ARRIBA)) mover = -1;
        if (key_was_hit(T_ABAJO))  mover = 1;
        if (key_was_hit(T_REPAG))  mover = -d->filas;
        if (key_was_hit(T_AVPAG))  mover = d->filas;
        if (key_was_hit(T_INICIO)) mover = -d->n_vista;
        if (key_was_hit(T_FIN))    mover = d->n_vista;
        if (mover != 0 && d->n_vista > 0) {
            int nueva = (d->sel < 0) ? (mover > 0 ? 0 : d->n_vista - 1) : d->sel + mover;
            if (nueva < 0) nueva = 0;
            if (nueva >= d->n_vista) nueva = d->n_vista - 1;
            seleccionar(d, nueva);
            ver_seleccion(d);
            partes |= P_LISTA | P_NOMBRE;
        }

        char c;
        while (input_read_char(&c)) {
            if (c == 27) {                         // Esc
                if (d->desplegado != D_NADA) { d->desplegado = D_NADA; partes = P_TODO; }
                else { res = -1; terminar = true; }
                break;
            } else if (c == '\n') {
                if (confirmar(d, &res, out, out_max, &partes)) { terminar = true; break; }
                partes |= P_LISTA | P_NOMBRE;
            } else if (c == '\b') {
                // Retroceso con la caja vacia sube un nivel, como en el
                // explorador; si hay texto, borra una letra.
                if (d->n_nombre > 0) {
                    d->nombre[--d->n_nombre] = '\0';
                    partes |= P_NOMBRE;
                } else {
                    subir(d);
                    partes = P_TODO;
                }
            } else if (c >= 32 && c < 127 && d->n_nombre < NEMOFS_MAX_NAME) {
                d->nombre[d->n_nombre++] = mayus(c);
                d->nombre[d->n_nombre] = '\0';
                // Lo tecleado manda sobre lo marcado en la lista
                if (d->sel >= 0) { d->sel = -1; partes |= P_LISTA; }
                partes |= P_NOMBRE;
            }
        }
    }

    // Devolver el lienzo tal y como estaba
    if (d->bajo) {
        wm_content_restore_rect(win, d->bajo_x, d->bajo_y, d->bajo_w, d->bajo_h, d->bajo);
        wm_request_redraw();
        kfree(d->bajo);
    }
    kfree(d);
    return res;
}

int64_t dialog_open_file(int32_t win, uint32_t start_dir, char *out_name, uint32_t out_name_max) {
    return dialogo(win, start_dir, out_name, out_name_max, false);
}

int64_t dialog_save_file(int32_t win, uint32_t start_dir, char *out_name, uint32_t out_name_max) {
    return dialogo(win, start_dir, out_name, out_name_max, true);
}
