// netshell.c -- Nemo OS
// Shell remota minima, en C, dentro del kernel, servida por TCP (Fase
// 3b del roadmap de red). Implementa los ganchos tcp_app_* de tcp.h.
//
// Corre en el contexto de net_poll() -- es decir, dentro de
// task_yield(), el "tick" del sistema -- asi que cada llamada tiene
// que ser rapida y no bloquear nunca: acumular bytes hasta un salto de
// linea, ejecutar el comando, devolver la salida en el mismo turno.
// Nada de esperar a nada.
//
// Es DELIBERADAMENTE mas pequeña que shell.lua (la shell "de verdad"
// del sistema, que corre como tarea): comandos basicos de disco,
// estado del sistema y lanzar programas -- lo que hace falta para
// administrar la placa desde el Mac sin cable serie. No es telnet (no
// negocia opciones) ni SSH (no cifra): texto plano en la red de casa.
//
// Toda la salida de un comando tiene que caber en la ventana TCP (1024
// bytes): 'cat' y 'ls' se cortan si hace falta, y lo dicen. Con "un
// segmento entra, uno sale" no hay forma de mandar mas de una ventana
// por comando sin un mecanismo de colas que aqui no compensa.

#include <stdint.h>
#include <stdbool.h>
#include "tcp.h"
#include "nemofs.h"
#include "heap.h"
#include "tasks.h"
#include "wm.h"
#include "descarga.h"
#include "net.h"
#include "ip_texto.h"

// ---- utilidades de cadena/numero sobre un buffer de salida acotado ----
typedef struct { uint8_t *p; uint32_t len, max; bool cortado; } sal_t;

static void sal_c(sal_t *s, char c) { if (s->len < s->max) s->p[s->len++] = (uint8_t)c; else s->cortado = true; }
static void sal_s(sal_t *s, const char *t) { while (*t) sal_c(s, *t++); }
static void sal_u(sal_t *s, uint32_t v) {
    char d[12]; int n = 0;
    if (v == 0) { sal_c(s, '0'); return; }
    while (v > 0 && n < 11) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) sal_c(s, d[--n]);
}
static void sal_bytes(sal_t *s, uint64_t b) {
    if (b >= 1024ULL * 1024 * 1024) { sal_u(s, (uint32_t)(b / (1024 * 1024 * 1024))); sal_s(s, " GB"); }
    else if (b >= 1024 * 1024) { sal_u(s, (uint32_t)(b / (1024 * 1024))); sal_s(s, " MB"); }
    else if (b >= 1024) { sal_u(s, (uint32_t)(b / 1024)); sal_s(s, " KB"); }
    else { sal_u(s, (uint32_t)b); sal_s(s, " B"); }
}

static bool str_eq(const char *a, const char *b) { while (*a && *b) { if (*a != *b) return false; a++; b++; } return *a == *b; }

// ---- estado de la sesion ----
#define MAX_PROF 8
static uint32_t ruta_inodos[MAX_PROF];       // pila de carpetas, [0] = raiz
static char ruta_nombres[MAX_PROF][NEMOFS_MAX_NAME + 1];
static int prof = 0;

static char linea[256];
static uint32_t linea_len = 0;

static void reiniciar_sesion(void) {
    prof = 0;
    ruta_inodos[0] = NEMOFS_ROOT_INODE;
    ruta_nombres[0][0] = '\0';
    linea_len = 0;
}

static void prompt(sal_t *s) {
    sal_s(s, "nemo:/");
    for (int i = 1; i <= prof; i++) { sal_s(s, ruta_nombres[i]); if (i < prof) sal_c(s, '/'); }
    sal_s(s, "> ");
}

// ---- comandos ----
static void cmd_help(sal_t *s) {
    sal_s(s, "Comandos:\r\n"
             "  ls            lista la carpeta actual\r\n"
             "  cd <carpeta>  entra ('cd ..' sube, 'cd /' a la raiz)\r\n"
             "  pwd           carpeta actual\r\n"
             "  cat <archivo> muestra un archivo de texto (hasta 900 bytes)\r\n"
             "  run <prog> [arg]  lanza un programa (.pro o .lua) en el escritorio\r\n"
             "  disco         uso del disco NemoFS\r\n"
             "  mem           memoria del kernel, tareas y memoria de tareas\r\n"
             "  tareas        tareas en marcha\r\n"
             "  bajar <ip> <puerto> <ruta> [archivo]  baja algo por HTTP\r\n"
             "  ver           como va la descarga\r\n"
             "  ip            la configuracion de red de esta maquina\r\n"
             "  exit          cierra la conexion\r\n");
}

static void cmd_ls(sal_t *s) {
    static nemofs_dirent_t ents[64];
    uint32_t n = nemofs_list_dir(ruta_inodos[prof], ents, 64);
    uint32_t mostrar = n < 64 ? n : 64;
    for (uint32_t i = 0; i < mostrar; i++) {
        if (ents[i].type == NEMOFS_TYPE_DIR) { sal_s(s, "  [DIR]  "); sal_s(s, ents[i].name); }
        else { sal_s(s, "         "); sal_s(s, ents[i].name); sal_s(s, "  ("); sal_bytes(s, ents[i].size); sal_c(s, ')'); }
        sal_s(s, "\r\n");
        if (s->cortado) break;
    }
    if (n > 64) { sal_s(s, "  ... y "); sal_u(s, n - 64); sal_s(s, " mas\r\n"); }
    if (n == 0) sal_s(s, "  (vacia)\r\n");
}

static void cmd_cd(sal_t *s, const char *arg) {
    if (!arg[0] || str_eq(arg, "/")) { prof = 0; return; }
    if (str_eq(arg, "..")) { if (prof > 0) prof--; return; }
    if (prof + 1 >= MAX_PROF) { sal_s(s, "demasiada profundidad\r\n"); return; }
    int32_t ino = nemofs_find_child(ruta_inodos[prof], arg);
    if (ino < 0) { sal_s(s, "no existe: "); sal_s(s, arg); sal_s(s, "\r\n"); return; }
    if (nemofs_type_by_inode((uint32_t)ino) != NEMOFS_TYPE_DIR) { sal_s(s, "no es una carpeta: "); sal_s(s, arg); sal_s(s, "\r\n"); return; }
    prof++;
    ruta_inodos[prof] = (uint32_t)ino;
    uint32_t i = 0; while (arg[i] && i < NEMOFS_MAX_NAME) { ruta_nombres[prof][i] = arg[i]; i++; } ruta_nombres[prof][i] = '\0';
}

static void cmd_cat(sal_t *s, const char *arg) {
    if (!arg[0]) { sal_s(s, "uso: cat <archivo>\r\n"); return; }
    int32_t ino = nemofs_find_child(ruta_inodos[prof], arg);
    if (ino < 0) { sal_s(s, "no existe: "); sal_s(s, arg); sal_s(s, "\r\n"); return; }
    if (nemofs_type_by_inode((uint32_t)ino) != NEMOFS_TYPE_FILE) { sal_s(s, "no es un archivo\r\n"); return; }
    static uint8_t buf[900];
    int32_t n = nemofs_read_file((uint32_t)ino, buf, sizeof(buf));
    if (n < 0) { sal_s(s, "error leyendo\r\n"); return; }
    for (int32_t i = 0; i < n; i++) {
        uint8_t c = buf[i];
        if (c == '\n') sal_s(s, "\r\n");
        else if (c >= 32 && c < 127) sal_c(s, (char)c);
        else if (c == '\t') sal_c(s, ' ');
        else sal_c(s, '.');   // binario: un punto por byte, sin volver loco al terminal
    }
    if (n == (int32_t)sizeof(buf)) sal_s(s, "\r\n[... cortado a 900 bytes]");
    sal_s(s, "\r\n");
}

static void cmd_run(sal_t *s, char *arg) {
    if (!arg[0]) { sal_s(s, "uso: run <programa> [argumento]\r\n"); return; }
    char *a = arg; while (*a && *a != ' ') a++;
    if (*a == ' ') { *a = '\0'; a++; }
    // Misma cola que usa la shell del escritorio: el bucle principal del
    // kernel lo recoge en su siguiente vuelta y abre la ventana. Busca
    // en la carpeta actual y en PROGRAMAS/ACCESORIOS como siempre.
    wm_request_launch(arg, a, -1, ruta_inodos[prof]);
    sal_s(s, "lanzado: "); sal_s(s, arg); sal_s(s, "\r\n");
}

static void cmd_disco(sal_t *s) {
    uint32_t total, usados;
    nemofs_disk_usage(&total, &usados);
    sal_s(s, "NemoFS: "); sal_bytes(s, (uint64_t)usados * 512); sal_s(s, " usados de ");
    sal_bytes(s, (uint64_t)total * 512); sal_s(s, " ("); sal_bytes(s, (uint64_t)(total - usados) * 512); sal_s(s, " libres)\r\n");
}

static void cmd_mem(sal_t *s) {
    sal_s(s, "heap del kernel: "); sal_bytes(s, kheap_used()); sal_s(s, " usados, ");
    sal_bytes(s, kheap_free()); sal_s(s, " libres\r\n");
    sal_s(s, "tareas: "); sal_u(s, task_count_used()); sal_s(s, " de "); sal_u(s, MAX_TASKS); sal_s(s, "\r\n");
    {
        uint32_t usado, total;
        task_pool_usage_kb(&usado, &total);
        sal_s(s, "memoria de tareas: "); sal_u(s, usado / 1024);
        sal_s(s, " MB de "); sal_u(s, total / 1024); sal_s(s, " MB\r\n");
    }
}

static void cmd_tareas(sal_t *s) {
    static uint8_t buf[48 * MAX_TASKS];
    uint32_t n = task_list_dump(buf, MAX_TASKS);
    if (n == 0) { sal_s(s, "  (ninguna)\r\n"); return; }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = buf + i * 48;   // slot(4) ventana(4) turnos(4) nombre(36) -- ver task_list_dump
        sal_s(s, "  slot "); sal_u(s, e[0]);
        sal_s(s, "  turnos "); sal_u(s, (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24));
        sal_s(s, "  "); sal_s(s, (const char *)(e + 12)); sal_s(s, "\r\n");
    }
}

// ---- descarga por HTTP --------------------------------
//
// Cierra el circuito de red entero --ARP, IP, TCP cliente, HTTP-- contra
// un servidor de verdad, que es lo unico que las pruebas del Mac no
// pueden comprobar: alli cada capa se probo con las de abajo fingidas.
//
// La descarga NO cabe en un comando. Un comando de esta shell lee una
// linea, hace lo suyo y contesta EN EL MISMO TURNO, y una peticion HTTP
// tarda muchas vueltas del kernel. Asi que 'bajar' solo arranca, y 'ver'
// cuenta como va -- que ademas es lo que hace falta para mirar una
// descarga larga sin quedarse sin saber nada.
// Saca una IP "a.b.c.d" del texto y deja el puntero detras.
static bool leer_ip(char **t, uint8_t ip[4]) {
    char *p = *t;
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0; int cifras = 0;
        while (*p >= '0' && *p <= '9' && cifras < 3) { v = v * 10 + (uint32_t)(*p - '0'); p++; cifras++; }
        if (cifras == 0 || v > 255) return false;
        ip[i] = (uint8_t)v;
        if (i < 3) { if (*p != '.') return false; p++; }
    }
    *t = p;
    return true;
}

// Se guarda solo para ensenarlo en 'ver'; la descarga la lleva
// descarga.c, que es quien la comparte con los programas de usuario.
static char destino_nombre[NEMOFS_MAX_NAME + 1];

static void cmd_bajar(sal_t *s, char *arg) {
    while (*arg == ' ') arg++;
    uint8_t ip[4];
    if (!leer_ip(&arg, ip)) {
        sal_s(s, "uso: bajar <ip> <puerto> <ruta> [archivo]\r\n"
                 "ej.: bajar 10.0.2.2 8000 /\r\n"
                 "     bajar 10.0.2.2 8000 /foto.nimg foto.nimg\r\n");
        return;
    }
    while (*arg == ' ') arg++;
    uint32_t puerto = 0;
    while (*arg >= '0' && *arg <= '9') { puerto = puerto * 10 + (uint32_t)(*arg - '0'); arg++; }
    if (puerto == 0 || puerto > 65535) { sal_s(s, "puerto no valido\r\n"); return; }
    while (*arg == ' ') arg++;
    char *ruta = (*arg) ? arg : (char *)"/";

    // Separar la ruta del nombre de archivo opcional.
    char *nombre = ruta;
    while (*nombre && *nombre != ' ') nombre++;
    if (*nombre == ' ') { *nombre = 0; nombre++; while (*nombre == ' ') nombre++; }

    destino_nombre[0] = 0;
    if (*nombre) {
        uint32_t k = 0;
        while (nombre[k] && k < NEMOFS_MAX_NAME) { destino_nombre[k] = nombre[k]; k++; }
        destino_nombre[k] = 0;
    }
    if (!descarga_empezar(ip, (uint16_t)puerto, ruta, ruta_inodos[prof],
                          *nombre ? nombre : 0)) {
        sal_s(s, "no se pudo empezar: "); sal_s(s, descarga_motivo()); sal_s(s, "\r\n");
        return;
    }

    sal_s(s, "bajando de "); 
    for (int i = 0; i < 4; i++) { sal_u(s, ip[i]); if (i < 3) sal_c(s, '.'); }
    sal_c(s, ':'); sal_u(s, puerto); sal_c(s, ' '); sal_s(s, ruta);
    if (destino_nombre[0]) { sal_s(s, "  ->  "); sal_s(s, destino_nombre); }
    sal_s(s, "\r\nusa 'ver' para mirar como va.\r\n");
}

static void cmd_ver(sal_t *s) {
    static const char *nombres[] = { "parada", "en marcha", "lista", "fallo" };
    sal_s(s, "estado: "); sal_s(s, nombres[descarga_estado()]);
    if (descarga_codigo()) { sal_s(s, "   codigo "); sal_u(s, descarga_codigo()); }
    if (descarga_motivo()[0]) { sal_s(s, "\r\nmotivo: "); sal_s(s, descarga_motivo()); }
    sal_s(s, "\r\nrecibido: "); sal_bytes(s, descarga_bytes());
    if (descarga_total()) { sal_s(s, " de "); sal_bytes(s, descarga_total()); }
    if (destino_nombre[0]) { sal_s(s, "\r\nguardando en: "); sal_s(s, destino_nombre); }
    sal_s(s, "\r\n");

    uint8_t vista[600];
    uint32_t n = descarga_vista(vista, sizeof vista);
    if (n == 0) { sal_s(s, "(aun no hay nada que ensenar)\r\n"); return; }
    sal_s(s, "---- primeros bytes ----\r\n");
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = vista[i];
        // Lo que no es texto se ensena como un punto: un .nimg por el
        // terminal dejaria la consola inservible.
        sal_c(s, (c == '\n' || c == '\r' || c == '\t' || (c >= 32 && c < 127)) ? (char)c : '.');
    }
    sal_s(s, "\r\n------------------------\r\n");
}

// Ejecuta una linea ya completa (sin el salto). Devuelve true si la
// aplicacion quiere cerrar la conexion.
// La configuracion de red.
//
// Desde fuera ya sabes la IP --has tenido que teclearla para llegar
// aqui-- pero no la mascara, ni la pasarela, ni el DNS. Y son las tres
// cosas que hacen falta cuando algo no sale de la red: con la mascara
// mal, la placa cree que el router es de otra subred; sin pasarela no
// se sale de la red local; sin DNS no hay nombres.
static void cmd_ip(sal_t *s) {
    if (!net_ready()) {
        sal_s(s, "la red no esta lista (sin enlace, o todavia sin direccion)\r\n");
        return;
    }
    uint8_t v[4];
    char t[IP_TEXTO_MAX];
    net_get_ip(v);       ip_a_texto(v, t, sizeof t); sal_s(s, "IP:       "); sal_s(s, t); sal_s(s, "\r\n");
    net_get_mascara(v);  ip_a_texto(v, t, sizeof t); sal_s(s, "mascara:  "); sal_s(s, t); sal_s(s, "\r\n");
    net_get_pasarela(v); ip_a_texto(v, t, sizeof t); sal_s(s, "pasarela: "); sal_s(s, t); sal_s(s, "\r\n");
    net_get_dns(v);      ip_a_texto(v, t, sizeof t); sal_s(s, "DNS:      "); sal_s(s, t); sal_s(s, "\r\n");
    // De donde salio la direccion. Importa para saber que esperar: por
    // DHCP hay router y se sale a internet; de reserva no, y la
    // direccion se calculo de la MAC para no chocar con otra placa.
    sal_s(s, net_ip_por_dhcp() ? "origen:   DHCP (hay router)\r\n"
                               : "origen:   reserva, sacada de la MAC (no hay router)\r\n");
}

static bool ejecutar(char *cmd, sal_t *s) {
    // recortar espacios al principio y al final
    while (*cmd == ' ') cmd++;
    uint32_t n = 0; while (cmd[n]) n++;
    while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\r')) cmd[--n] = '\0';
    if (!cmd[0]) return false;

    char *arg = cmd; while (*arg && *arg != ' ') arg++;
    if (*arg == ' ') { *arg = '\0'; arg++; while (*arg == ' ') arg++; }

    if (str_eq(cmd, "help") || str_eq(cmd, "?")) cmd_help(s);
    else if (str_eq(cmd, "ls") || str_eq(cmd, "dir")) cmd_ls(s);
    else if (str_eq(cmd, "cd")) cmd_cd(s, arg);
    else if (str_eq(cmd, "pwd")) { sal_s(s, "/"); for (int i = 1; i <= prof; i++) { sal_s(s, ruta_nombres[i]); if (i < prof) sal_c(s, '/'); } sal_s(s, "\r\n"); }
    else if (str_eq(cmd, "cat") || str_eq(cmd, "type")) cmd_cat(s, arg);
    else if (str_eq(cmd, "run")) cmd_run(s, arg);
    else if (str_eq(cmd, "disco") || str_eq(cmd, "df")) cmd_disco(s);
    else if (str_eq(cmd, "mem") || str_eq(cmd, "free")) cmd_mem(s);
    else if (str_eq(cmd, "tareas") || str_eq(cmd, "ps")) cmd_tareas(s);
    else if (str_eq(cmd, "bajar") || str_eq(cmd, "get")) cmd_bajar(s, arg);
    else if (str_eq(cmd, "ver")) cmd_ver(s);
    else if (str_eq(cmd, "ip") || str_eq(cmd, "red")) cmd_ip(s);
    else if (str_eq(cmd, "exit") || str_eq(cmd, "quit") || str_eq(cmd, "salir")) { sal_s(s, "hasta luego.\r\n"); return true; }
    else { sal_s(s, "comando desconocido: "); sal_s(s, cmd); sal_s(s, "  (prueba 'help')\r\n"); }
    return false;
}

// ---- ganchos de TCP ----
uint32_t tcp_app_on_connect(uint8_t *salida, uint32_t max) {
    reiniciar_sesion();
    sal_t s = { salida, 0, max, false };
    sal_s(&s, "\r\nNemo OS -- shell remota (Raspberry Pi 4)\r\n'help' para ver los comandos.\r\n\r\n");
    prompt(&s);
    return s.len;
}

uint32_t tcp_app_on_data(const uint8_t *datos, uint32_t len, uint8_t *salida, uint32_t max, bool *cerrar) {
    sal_t s = { salida, 0, max, false };
    *cerrar = false;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = datos[i];
        if (c == '\n') {
            linea[linea_len] = '\0';
            linea_len = 0;
            if (ejecutar(linea, &s)) { *cerrar = true; return s.len; }
            if (s.cortado) sal_s(&s, "\r\n[salida cortada: no cabe en un segmento]");
            prompt(&s);
        } else if (c == 8 || c == 127) {           // retroceso
            if (linea_len > 0) linea_len--;
        } else if (c >= 32 && linea_len < sizeof(linea) - 1) {
            linea[linea_len++] = (char)c;
        }
        // \\r y bytes de control se ignoran; nc manda \\n, telnet \\r\\n
    }
    // Sin salto de linea todavia (cliente que manda byte a byte): nada
    // que responder; TCP se encarga de confirmar los bytes igual.
    return s.len;
}
