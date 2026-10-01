// ip_texto.h -- Nemo OS
// Leer una direccion IPv4 escrita por una persona: "192.168.1.40".
//
// Veinte lineas y archivo propio, por lo mismo que nmz_fs.c: es texto que
// viene de FUERA. Lo escribe alguien a mano en la barra de un programa, o
// llega en un archivo de configuracion, y lo que hay ahi puede no ser una
// IP: "999.1.1.1", "1.2.3", "1.2.3.4.5", "localhost", nada.
//
// Y equivocarse aqui no da ningun error. Una IP mal leida manda los
// paquetes a otra maquina --o a ninguna-- y el sintoma es "no llega", que
// puede ser mil cosas: el cable, el router, el cortafuegos, el programa
// del otro lado. Se depura fatal en una placa y se ve en un segundo aqui.
//
// Existe tambien para que no haya que empaquetar la IP en el lenguaje que
// llama. Esa conversion la hacia el navegador en Lua y necesito seis
// comprobaciones solo para el orden de los bytes: es justo el tipo de
// cuenta que se escribe al reves sin enterarse.
#ifndef IP_TEXTO_H
#define IP_TEXTO_H

#include <stdint.h>
#include <stdbool.h>

// Lee "a.b.c.d" y deja los cuatro numeros en 'out'. Devuelve false si el
// texto no es una IPv4, y entonces 'out' no se toca.
//
// Se acepta lo que de verdad es una direccion y nada mas: cuatro numeros
// de 0 a 255 separados por puntos, con espacios alrededor si hace falta.
// Ni nombres de maquina (para eso hace falta DNS), ni notaciones cortas
// ("10.1" por "10.0.0.1"), ni ceros por delante ("010" en una IP es octal
// para algunas herramientas y decimal para otras, y esa ambiguedad se
// ha usado para colar direcciones).
bool ip_desde_texto(const char *s, uint8_t out[4]);

// El camino de vuelta: escribe "a.b.c.d" en 'out' y devuelve su largo sin
// contar el cero final. Hacen falta 16 bytes en el peor caso
// ("255.255.255.255" son 15 letras y el cero). Con menos sitio no escribe
// nada y devuelve 0 -- cortar una IP por la mitad daria una direccion
// distinta y valida, que es la peor forma de fallar.
#define IP_TEXTO_MAX 16
uint32_t ip_a_texto(const uint8_t ip[4], char *out, uint32_t max);

#endif
