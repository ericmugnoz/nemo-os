// icons_data.h — Nemo OS
#ifndef ICONS_DATA_H
#define ICONS_DATA_H

#include <stdint.h>

#define ICON_FOLDER 0
#define ICON_TXT    1
#define ICON_CODE   2

#define ICON_SIZE 24 // todos los iconos embebidos son de 24x24, formato RGBA

// Devuelve un puntero a ICON_SIZE*ICON_SIZE*4 bytes RGBA, o NULL si el
// id no es valido.
const uint8_t *icon_get_rgba(int icon_id);

#endif
