// icons_data.h — Nemo OS
#ifndef ICONS_DATA_H
#define ICONS_DATA_H

#include <stdint.h>

#define ICON_FOLDER   0
#define ICON_TXT      1
#define ICON_CODE     2
#define ICON_PROGRAM  3   // programa generico (triangulo de "play")
#define ICON_TERMINAL 4   // shell/terminal
#define ICON_SETTINGS 5   // ajustes (engranaje)
#define ICON_TRASH    6   // papelera
#define ICON_DISK     7   // disco/pendrive
#define ICON_NETWORK  8   // red (faro)
#define ICON_LOCK     9   // candado/privacidad
#define ICON_MAIL     10  // correo/mensajes
#define ICON_COMPASS  11  // brujula -- logo de Nemo OS
#define ICON_ANCHOR   12  // ancla -- marcador generico de sistema

// Añadidos para el escritorio. El editor de escritorio
// los ofrece solos: recorre de 0 a icon_catalog_count()-1.
#define ICON_PAINT     13  // paleta de pintor
#define ICON_DESIGN    14  // ventana en construccion -- Aronnax
#define ICON_LUA       15  // la luna de Lua
#define ICON_IMAGE     16  // una foto
#define ICON_GAME      17  // mando de juego
#define ICON_MUSIC     18  // nota musical
#define ICON_CLOCK     19  // reloj
#define ICON_CALC      20  // calculadora
#define ICON_HELP      21  // interrogacion -- ayuda
#define ICON_INFO      22  // informacion
#define ICON_BOOK      23  // libro abierto -- guias
#define ICON_NAUTILUS  24  // el Nautilus

#define ICON_SIZE 24 // todos los iconos embebidos son de 24x24, formato RGBA

// Devuelve un puntero a ICON_SIZE*ICON_SIZE*4 bytes RGBA, o NULL si el
// id no es valido.
const uint8_t *icon_get_rgba(int icon_id);
uint32_t icon_catalog_count(void);

#endif
