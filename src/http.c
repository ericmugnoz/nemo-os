// http.c -- Nemo OS. Ver http.h para el alcance y los recortes.

#include "http.h"
#include "tcp_cliente.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);

static void put_dec(uint32_t v) {
    char d[12]; int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0 && n < 11) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) uart_putc(d[--n]);
}

// DOS plazos, porque son dos situaciones distintas.
//
// Habia uno solo, de diez segundos en total, y se agotaba pidiendo una
// pagina de Wikipedia. No porque la red fuera lenta: el proxy, antes de
// contestar una sola letra, tiene que bajar la pagina Y sus imagenes de
// internet y convertirlas una a una. Durante todo ese rato no llega
// nada, y con razon.
//
// Subir el numero seria aplazar la pregunta, y ademas dejaria sin
// proteccion el caso contrario: una descarga
// que se corta a la mitad y se queda callada para siempre.
//
// Asi que se separan:
//   * Esperando a que el servidor EMPIECE a contestar: generoso, porque
//     ahi hay trabajo de verdad al otro lado.
//   * Una vez que los bytes fluyen, un silencio largo si es un fallo.
#define PLAZO_PRIMER_BYTE_MS 45000
#define PLAZO_SIN_AVANCE_MS  10000

static http_estado_t estado = HTTP_PARADO;
static uint64_t ms_inicio;       // se fija en el PRIMER tick, no al pedir
static uint64_t ms_ultimo_dato;  // ultima vez que llego ALGO

static uint8_t  peticion[HTTP_PETICION_MAX];
static uint32_t pet_len, pet_enviado;
// Se pone a true cuando la peticion no cupo. Se mira DESPUES de montarla
// entera, porque anadir_texto y anadir_bytes se paran solos al llegar al
// tope y hay que distinguir "cabe justo" de "se corto".
static bool pet_desbordada;

static uint8_t  cab[HTTP_CAB_MAX];
static uint32_t cab_len;

static uint8_t  cuerpo[HTTP_CUERPO_BUF];
static uint32_t cue_ini, cue_fin;

static uint32_t codigo, largo_declarado, recibido;
// Por que fallo, en una linea. El motivo se imprimia por el UART, que
// no se ve desde la shell de red -- justo desde donde se lanza esto.
static const char *motivo = "";
static bool     largo_conocido, chunked;

// ---------------------------------------------------------------------
// Utilidades de texto. Sin biblioteca estandar: esto corre dentro del
// kernel.
// ---------------------------------------------------------------------
static uint32_t largo_cadena(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

static char minuscula(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Compara sin distinguir mayusculas. Los nombres de cabecera HTTP no
// las distinguen, y hay servidores que escriben "content-length" en
// minuscula entero -- buscarlo tal cual es un clasico de los fallos que
// solo aparecen contra el servidor de otro.
static bool empieza_por_sin_caso(const uint8_t *s, uint32_t largo, const char *pre) {
    uint32_t n = largo_cadena(pre);
    if (largo < n) return false;
    for (uint32_t i = 0; i < n; i++)
        if (minuscula((char)s[i]) != minuscula(pre[i])) return false;
    return true;
}

static uint32_t leer_numero(const uint8_t *s, uint32_t largo) {
    uint32_t v = 0, i = 0;
    while (i < largo && (s[i] == ' ' || s[i] == '\t')) i++;
    while (i < largo && s[i] >= '0' && s[i] <= '9') {
        // Tope para que un numero absurdo no de la vuelta en silencio,
        // que es el mismo fallo que el de los literales del compilador.
        if (v > 400000000u) return 0xFFFFFFFFu;
        v = v * 10 + (uint32_t)(s[i] - '0');
        i++;
    }
    return v;
}

// ---------------------------------------------------------------------
static void anadir_texto(const char *t) {
    uint32_t n = largo_cadena(t);
    for (uint32_t i = 0; i < n; i++) {
        if (pet_len >= sizeof peticion) { pet_desbordada = true; return; }
        peticion[pet_len++] = (uint8_t)t[i];
    }
}

static void anadir_bytes(const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (pet_len >= sizeof peticion) { pet_desbordada = true; return; }
        peticion[pet_len++] = b[i];
    }
}

static void anadir_numero(uint32_t v) {
    if (v == 0) { anadir_texto("0"); return; }
    char d[12];
    int n = 0;
    while (v > 0 && n < 11) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) {
        char c = d[--n];
        anadir_bytes((const uint8_t *)&c, 1);
    }
}

// Lo comun a GET y POST: dejar todo a cero y montar la peticion. 'cuerpo'
// a NULL es un GET.
static bool pedir(const uint8_t ip[4], uint16_t puerto,
                  const char *ruta, const char *host,
                  const uint8_t *cuerpo, uint32_t cuerpo_len) {
    if (estado != HTTP_PARADO && estado != HTTP_LISTO && estado != HTTP_FALLO) return false;

    pet_len = pet_enviado = 0;
    pet_desbordada = false;
    cab_len = 0;
    cue_ini = cue_fin = 0;
    codigo = 0; largo_declarado = 0; recibido = 0;
    motivo = "";
    largo_conocido = false; chunked = false;
    ms_inicio = 0;      // sin empezar: lo pone el primer tick

    anadir_texto(cuerpo ? "POST " : "GET ");
    anadir_texto(ruta);
    anadir_texto(" HTTP/1.1\r\nHost: ");
    anadir_texto(host);
    // "Connection: close" no es cortesia: hace que el servidor cierre al
    // terminar, y ese cierre es la senal de fin cuando no hay
    // Content-Length. Sin esto habria que entender "chunked".
    anadir_texto("\r\nConnection: close\r\nUser-Agent: NemoOS\r\n");
    if (cuerpo) {
        // Content-Length es OBLIGATORIO en un POST: sin el, el servidor
        // no sabe donde acaba el cuerpo y se queda esperando. Y tiene que
        // coincidir con lo que se manda de verdad, de ahi que el cuerpo
        // se meta aqui mismo y no lo ponga quien llama.
        anadir_texto("Content-Type: text/plain\r\nContent-Length: ");
        anadir_numero(cuerpo_len);
        anadir_texto("\r\n\r\n");
        anadir_bytes(cuerpo, cuerpo_len);
    } else {
        anadir_texto("\r\n");
    }

    if (pet_desbordada) {
        estado = HTTP_FALLO;
        motivo = "la peticion no cabe"; uart_puts("http: la peticion no cabe\n");
        return false;
    }

    if (!tcpcli_conectar(ip, puerto, 0)) { estado = HTTP_FALLO; return false; }
    estado = HTTP_CONECTANDO;
    uart_puts(cuerpo ? "http: POST " : "http: GET "); uart_puts(ruta); uart_puts("\n");
    return true;
}

bool http_get(const uint8_t ip[4], uint16_t puerto,
              const char *ruta, const char *host) {
    return pedir(ip, puerto, ruta, host, 0, 0);
}

bool http_post(const uint8_t ip[4], uint16_t puerto,
               const char *ruta, const char *host,
               const uint8_t *cuerpo, uint32_t cuerpo_len) {
    // Un POST con cuerpo vacio es legal y tiene sentido ("avisa de que
    // esto ha pasado, sin datos"), asi que se pasa un puntero que no sea
    // NULL aunque la longitud sea cero: NULL es lo que distingue un GET.
    static const uint8_t nada = 0;
    return pedir(ip, puerto, ruta, host,
                 cuerpo ? cuerpo : &nada, cuerpo ? cuerpo_len : 0);
}

// Mira la primera linea: "HTTP/1.1 200 OK".
static bool leer_linea_de_estado(const uint8_t *l, uint32_t largo) {
    if (!empieza_por_sin_caso(l, largo, "HTTP/")) return false;
    uint32_t i = 0;
    while (i < largo && l[i] != ' ') i++;      // saltar "HTTP/1.1"
    codigo = leer_numero(l + i, largo - i);
    return codigo >= 100 && codigo < 600;
}

static void mirar_cabecera(const uint8_t *l, uint32_t largo) {
    if (empieza_por_sin_caso(l, largo, "content-length:")) {
        uint32_t v = leer_numero(l + 15, largo - 15);
        if (v != 0xFFFFFFFFu) { largo_declarado = v; largo_conocido = true; }
    } else if (empieza_por_sin_caso(l, largo, "transfer-encoding:")) {
        // No se sabe leer "chunked". Se marca y se falla con un motivo
        // claro, que es infinitamente mejor que entregar un cuerpo con
        // los tamanos de trozo mezclados dentro.
        for (uint32_t i = 18; i + 6 < largo; i++)
            if (empieza_por_sin_caso(l + i, largo - i, "chunked")) { chunked = true; break; }
    }
}

// Recorre las cabeceras acumuladas buscando la linea en blanco que las
// separa del cuerpo. Devuelve el desplazamiento del primer byte del
// cuerpo, o 0 si todavia no han terminado de llegar.
static uint32_t fin_de_cabeceras(void) {
    for (uint32_t i = 0; i + 3 < cab_len; i++)
        if (cab[i] == '\r' && cab[i+1] == '\n' && cab[i+2] == '\r' && cab[i+3] == '\n')
            return i + 4;
    return 0;
}

static void procesar_cabeceras(uint32_t fin) {
    uint32_t ini = 0;
    bool primera = true;
    for (uint32_t i = 0; i + 1 < fin; i++) {
        if (cab[i] == '\r' && cab[i+1] == '\n') {
            uint32_t largo = i - ini;
            if (largo == 0) break;             // la linea en blanco: se acabo
            if (primera) {
                if (!leer_linea_de_estado(cab + ini, largo)) {
                    estado = HTTP_FALLO;
                    motivo = "la respuesta no parece HTTP"; uart_puts("http: la respuesta no parece HTTP\n");
                    return;
                }
                primera = false;
            } else {
                mirar_cabecera(cab + ini, largo);
            }
            ini = i + 2;
            i++;
        }
    }
}

static void guardar_en_cuerpo(const uint8_t *d, uint32_t n) {
    for (uint32_t i = 0; i < n && cue_fin < HTTP_CUERPO_BUF; i++) cuerpo[cue_fin++] = d[i];
    recibido += n;
}

void http_tick(uint64_t ms) {
    if (estado == HTTP_PARADO || estado == HTTP_LISTO || estado == HTTP_FALLO) return;

    if (ms_inicio == 0) { ms_inicio = ms; ms_ultimo_dato = 0; }   // el reloj arranca aqui

    bool vencido = (ms_ultimo_dato == 0)
                 ? (ms - ms_inicio      > PLAZO_PRIMER_BYTE_MS)
                 : (ms - ms_ultimo_dato > PLAZO_SIN_AVANCE_MS);
    if (vencido) {
        motivo = (ms_ultimo_dato == 0) ? "el servidor no contesto (45 s)"
                                       : "la descarga se quedo parada (10 s)";
        uart_puts("http: se agoto el plazo\n");
        tcpcli_abandonar();
        estado = HTTP_FALLO;
        return;
    }

    if (tcpcli_estado() == TCPCLI_FALLO) {
        motivo = "no se pudo conectar (servidor apagado?)"; uart_puts("http: no se pudo conectar\n");
        estado = HTTP_FALLO;
        return;
    }

    if (estado == HTTP_CONECTANDO) {
        if (tcpcli_estado() != TCPCLI_ABIERTA) return;
        estado = HTTP_PIDIENDO;
    }

    if (estado == HTTP_PIDIENDO) {
        // Puede que no salga a la primera: falta resolver la MAC por
        // ARP. Se reintenta en la vuelta siguiente con lo que quede.
        while (pet_enviado < pet_len) {
            uint32_t n = tcpcli_enviar(peticion + pet_enviado, pet_len - pet_enviado, ms);
            if (n == 0) return;
            pet_enviado += n;
        }
        estado = HTTP_CABECERAS;
    }

    // Sacar de TCP lo que quepa. Cuando el cuerpo esta lleno se deja de
    // sacar, y la ventana que anuncia tcp_cliente se encoge sola: el
    // freno llega al otro extremo sin que nadie lo programe.
    uint8_t tmp[1500];
    for (;;) {
        uint32_t sitio = (estado == HTTP_CABECERAS)
                       ? sizeof tmp
                       : (HTTP_CUERPO_BUF - cue_fin);
        if (sitio == 0) break;
        if (sitio > sizeof tmp) sitio = sizeof tmp;
        uint32_t n = tcpcli_leer(tmp, sitio);
        if (n == 0) break;
        ms_ultimo_dato = ms;   // hay avance: el plazo vuelve a empezar

        if (estado == HTTP_CABECERAS) {
            for (uint32_t i = 0; i < n; i++) {
                if (cab_len < HTTP_CAB_MAX) cab[cab_len++] = tmp[i];
                else {
                    motivo = "cabeceras demasiado largas"; uart_puts("http: cabeceras demasiado largas\n");
                    tcpcli_abandonar();
                    estado = HTTP_FALLO;
                    return;
                }
                uint32_t fin = fin_de_cabeceras();
                if (fin) {
                    procesar_cabeceras(fin);
                    if (estado == HTTP_FALLO) { tcpcli_abandonar(); return; }
                    if (chunked) {
                        motivo = "respuesta 'chunked', no la sabemos leer"; uart_puts("http: respuesta 'chunked', que no sabemos leer\n");
                        tcpcli_abandonar();
                        estado = HTTP_FALLO;
                        return;
                    }
                    uart_puts("http: codigo "); put_dec(codigo);
                    if (largo_conocido) { uart_puts(", "); put_dec(largo_declarado); uart_puts(" bytes"); }
                    else uart_puts(", largo desconocido (se lee hasta que cierre)");
                    uart_puts("\n");
                    estado = HTTP_CUERPO;
                    // Lo que venia detras de la linea en blanco YA es
                    // cuerpo, y puede ser bastante: cabe entero en el
                    // mismo segmento que las cabeceras.
                    guardar_en_cuerpo(cab + fin, cab_len - fin);
                    // Y lo que quede de este bloque, tambien.
                    if (i + 1 < n) guardar_en_cuerpo(tmp + i + 1, n - i - 1);
                    break;
                }
            }
        } else {
            guardar_en_cuerpo(tmp, n);
        }

        if (estado == HTTP_CUERPO && largo_conocido && recibido >= largo_declarado) break;
    }

    if (estado == HTTP_CUERPO) {
        bool completo = largo_conocido ? (recibido >= largo_declarado)
                                       : (tcpcli_fin_recibido() && tcpcli_pendiente() == 0);
        if (completo) {
            uart_puts("http: recibido entero ("); put_dec(recibido); uart_puts(" bytes)\n");
            estado = HTTP_LISTO;
            tcpcli_cerrar(ms);
        }
    }

    // El otro extremo cerro antes de tiempo. Con Content-Length sabemos
    // que falta algo y hay que decirlo: media pagina que parece entera
    // es peor que un error.
    if (estado == HTTP_CABECERAS && tcpcli_fin_recibido() && tcpcli_pendiente() == 0) {
        motivo = "se corto antes de contestar"; uart_puts("http: se corto antes de las cabeceras\n");
        estado = HTTP_FALLO;
    }
}

uint32_t http_leer(uint8_t *out, uint32_t max) {
    uint32_t hay = cue_fin - cue_ini;
    uint32_t n = hay < max ? hay : max;
    for (uint32_t i = 0; i < n; i++) out[i] = cuerpo[cue_ini + i];
    cue_ini += n;
    if (cue_ini == cue_fin) cue_ini = cue_fin = 0;   // vacio: al principio otra vez
    else if (cue_ini > HTTP_CUERPO_BUF / 2) {        // compactar para no quedarnos sin sitio
        uint32_t q = cue_fin - cue_ini;
        for (uint32_t i = 0; i < q; i++) cuerpo[i] = cuerpo[cue_ini + i];
        cue_ini = 0; cue_fin = q;
    }
    return n;
}

http_estado_t http_estado(void) { return estado; }
uint32_t http_codigo(void) { return codigo; }
uint32_t http_largo_declarado(void) { return largo_declarado; }
uint32_t http_recibido(void) { return recibido; }
const char *http_motivo(void) { return motivo; }

void http_abandonar(void) {
    tcpcli_abandonar();
    estado = HTTP_PARADO;
    cue_ini = cue_fin = 0;
    cab_len = 0;
}

const char *http_estado_nombre(void) {
    switch (estado) {
        case HTTP_PARADO:     return "parado";
        case HTTP_CONECTANDO: return "conectando";
        case HTTP_PIDIENDO:   return "pidiendo";
        case HTTP_CABECERAS:  return "leyendo cabeceras";
        case HTTP_CUERPO:     return "recibiendo";
        case HTTP_LISTO:      return "listo";
        default:              return "fallo";
    }
}
