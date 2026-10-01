// http.h -- Nemo OS
// Cliente HTTP minimo: pedir una cosa y recibirla.
//
// Encima de tcp_cliente.c, y con el mismo criterio: esto habla con el
// proxy de la red local (ver el diseno del navegador), no con internet
// en bruto. Eso permite recortar mucho y decirlo en voz alta:
//
// * GET y POST. El POST llego, para que un programa
//     pueda MANDAR datos --la lectura de un sensor del GPIO, por
//     ejemplo-- a un servidor de la red. Nada mas: ni PUT, ni DELETE,
//     ni cabeceras a la carta.
//   * Solo HTTP, nunca HTTPS. El cifrado lo pone el proxy, que es toda
//     la gracia del montaje: TLS dentro de Nemo OS serian meses de
//     trabajo y una responsabilidad de seguridad permanente.
//   * "Connection: close" en cada peticion. Con eso el servidor cierra
//     al terminar, y el cierre ES la senal de fin -- lo que evita
//     tener que entender la codificacion "chunked", que es la otra
//     forma de terminar una respuesta y no es poca cosa.
//   * Sin redirecciones, sin cookies, sin autenticacion, sin cache.
//     El proxy resuelve todo eso antes de contestarnos.
//
// Si llega una respuesta "chunked" se rechaza CON UN ERROR CLARO en vez
// de intentar leerla a medias: entregar un cuerpo con los tamanos de
// trozo mezclados dentro seria mucho peor que fallar.
#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <stdbool.h>

// Lo que cabe esperando a que la aplicacion lo consuma. No es la
// descarga: es el colchon entre lo que llega y lo que se lee. Cuando se
// llena, se deja de sacar de TCP y la ventana anunciada se encoge sola
// -- el freno llega hasta el otro extremo sin que nadie lo programe.
#define HTTP_CUERPO_BUF 8192

// Tope de cabeceras. Las de un proxy propio son cuatro lineas; 4 KB es
// de sobra, y tener tope evita que una respuesta rara coma memoria sin
// final.
#define HTTP_CAB_MAX 4096

typedef enum {
    HTTP_PARADO,
    HTTP_CONECTANDO,
    HTTP_PIDIENDO,     // mandando la peticion
    HTTP_CABECERAS,    // leyendo la respuesta, aun sin llegar al cuerpo
    HTTP_CUERPO,       // el cuerpo esta llegando
    HTTP_LISTO,        // termino, todo recibido
    HTTP_FALLO
} http_estado_t;

// Pide 'ruta' (con la barra inicial) al servidor. 'host' es lo que va
// en la cabecera Host: normalmente la direccion del proxy.
//
// NO lleva la hora a proposito. La llevaba, y era una trampa: quien
// llamaba tenia que pasar el MISMO reloj que luego recibe http_tick(),
// y pasar un 0 --lo mas natural del mundo al escribir la llamada--
// hacia que el plazo de diez segundos se considerara agotado en el
// primer tick y la descarga fallara antes de empezar. Ahora el plazo
// lo cuenta http_tick desde su primera vuelta, que es cuando de verdad
// empieza a correr el tiempo.
bool http_get(const uint8_t ip[4], uint16_t puerto,
              const char *ruta, const char *host);

// Lo mismo, pero MANDANDO 'cuerpo' (cuerpo_len bytes). Para eso sirve:
// un programa que lee un sensor y se lo cuenta a un servidor.
//
// El tipo de contenido va fijo a "text/plain": quien recoja esto al otro
// lado es un guion propio, y elegirlo caso por caso seria un argumento
// mas que nadie va a cambiar nunca. Si algun dia hace falta JSON de
// verdad, se anade entonces.
//
// La peticion entera (linea, cabeceras y cuerpo) tiene que caber en
// HTTP_PETICION_MAX. Si no cabe, devuelve false con un motivo claro en
// vez de mandarla cortada -- un cuerpo truncado con su Content-Length
// original deja al servidor esperando bytes que no van a llegar, y eso
// se manifiesta como una conexion colgada, no como un error.
#define HTTP_PETICION_MAX 2048
bool http_post(const uint8_t ip[4], uint16_t puerto,
               const char *ruta, const char *host,
               const uint8_t *cuerpo, uint32_t cuerpo_len);

// Llamar en cada vuelta.
void http_tick(uint64_t ms);

// Saca hasta 'max' bytes del CUERPO. Devuelve 0 si aun no hay nada.
// Hay que seguir llamando mientras el estado sea HTTP_CUERPO: es lo que
// libera sitio para que siga llegando.
uint32_t http_leer(uint8_t *out, uint32_t max);

http_estado_t http_estado(void);
const char   *http_estado_nombre(void);

// Codigo de la respuesta (200, 404...). 0 si todavia no se sabe.
uint32_t http_codigo(void);

// Lo que dijo Content-Length, o 0 si no lo dijo.
uint32_t http_largo_declarado(void);

// Bytes de cuerpo recibidos hasta ahora.
uint32_t http_recibido(void);

// Por que fallo, en una linea. Cadena vacia si no ha fallado.
const char *http_motivo(void);

// Corta la descarga y suelta la conexion.
void http_abandonar(void);

#endif
