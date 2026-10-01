// descarga.h -- Nemo OS
// Bajar algo por HTTP a un archivo de NemoFS.
//
// POR QUE ESTA APARTE. Esto vivia dentro de netshell.c,
// porque el unico que sabia bajar cosas era el comando 'bajar' de la
// shell de red. En cuanto los programas de usuario tambien pueden
// pedirlo --el navegador-- habria dos copias del mismo bucle: sacar de
// http.c, escribir en el disco, llevar la cuenta. Aqui hay una.
//
// UNA DESCARGA A LA VEZ, en todo el sistema. No es pereza: debajo hay
// un solo cliente HTTP y un solo cliente TCP, asi que permitir dos
// seria prometer algo que el kernel no puede cumplir. Quien pida una
// segunda mientras la primera esta en marcha recibe un no.
#ifndef DESCARGA_H
#define DESCARGA_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    DESCARGA_PARADA,
    DESCARGA_EN_MARCHA,
    DESCARGA_LISTA,
    DESCARGA_FALLO
} descarga_estado_t;

// Empieza. 'dir' es el inodo de la carpeta donde guardar y 'nombre' el
// archivo; con 'nombre' a NULL no se guarda nada y solo se puede mirar
// el principio con descarga_vista().
//
// Si el archivo ya existia se borra: una descarga SUSTITUYE. Pegarse
// detras de lo que quedo del intento anterior daria un archivo que
// parece correcto y esta corrupto por delante.
bool descarga_empezar(const uint8_t ip[4], uint16_t puerto, const char *ruta,
                      uint32_t dir, const char *nombre);

// Manda 'cuerpo' por POST y deja la respuesta EN MEMORIA, sin tocar el
// disco: se lee con descarga_vista(). Para eso sirve -- un programa que
// lee un sensor del GPIO y se lo cuenta a un servidor de la red, y que lo
// unico que quiere de vuelta es un "recibido".
//
// Con 'cuerpo' a NULL es un GET a memoria, que es como se pide algo
// pequeno (la hora, una consigna) sin crear un archivo para tirarlo.
//
// La respuesta se queda en los primeros DESCARGA_VISTA bytes. Lo que pase
// de ahi se descarta, pero se sigue contando en descarga_bytes(): asi se
// ve que vino mas de lo que se guardo en vez de creerse que eso era todo.
bool descarga_pedir(const uint8_t ip[4], uint16_t puerto, const char *ruta,
                    const uint8_t *cuerpo, uint32_t cuerpo_len);

// Llamar en cada vuelta del kernel. Hace avanzar el HTTP y --esto es lo
// importante-- VACIA lo que va llegando. Si nadie vacia, el buffer de
// http.c se llena, se deja de sacar de TCP, la ventana anunciada se
// encoge y la descarga se para sola. El consumidor es parte del
// mecanismo, no un espectador.
void descarga_tick(uint64_t ms);

descarga_estado_t descarga_estado(void);
uint32_t    descarga_bytes(void);      // recibidos hasta ahora
uint32_t    descarga_total(void);      // lo que dijo Content-Length, 0 si no lo dijo
uint32_t    descarga_codigo(void);     // 200, 404...
const char *descarga_motivo(void);     // por que fallo, o cadena vacia
int32_t     descarga_inodo(void);      // el archivo escrito, o -1

// Copia el principio de lo recibido (hasta donde se guardo). Sirve para
// dos cosas: mirar por encima una descarga a disco sin abrir el archivo,
// y --con descarga_pedir-- leer la respuesta entera, que es para lo que
// se subio de 600 a 2048 bytes. Una contestacion de servidor ("ok", un
// puñado de valores, un error) cabe de sobra; una pagina web no, y no es
// para eso.
#define DESCARGA_VISTA 2048
uint32_t descarga_vista(uint8_t *out, uint32_t max);

void descarga_abandonar(void);

#endif
