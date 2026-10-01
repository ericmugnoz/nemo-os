// otros_programas.h -- la carpeta "otros programas" que viaja en la imagen.
//
// Programas escritos en Nemo Basic que se instalan como CODIGO FUENTE, cada
// uno en su carpeta, dentro de /otros programas de NemoFS. Se compilan desde
// el propio sistema (con nbc o desde el IDE), que es justo la gracia: el
// sistema trae ejemplos de verdad, con su fuente, listos para abrir y
// cambiar.
//
// Van como fuente y no compilados a proposito: son el material de estudio del
// sistema, no aplicaciones del menu.
#ifndef OTROS_PROGRAMAS_H
#define OTROS_PROGRAMAS_H

#include <stdint.h>

// Desempaqueta el paquete embebido en /otros programas, creando la carpeta y
// las de cada programa. Idempotente: en el segundo arranque solo reescribe lo
// que haya cambiado (nemofs_write_file_if_changed), asi que no toca la
// tarjeta si el usuario no ha tocado nada.
void otros_programas_instalar(void);

#endif
