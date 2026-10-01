// iconos_nimg.h -- la biblioteca de iconos NIMG del sistema (tiras para la
// barra de herramientas e iconos de 32 px). Los datos los genera
// herramientas/iconos/crear_iconos.py en iconos_nimg.c; embedded_lua.c los
// instala en DOCUMENTOS, donde los buscan CreateToolBar, SetPanelImage y
// LoadImage.
#ifndef ICONOS_NIMG_H
#define ICONOS_NIMG_H
#include <stdint.h>
typedef struct { const uint8_t *datos; uint32_t tamano; const char *nombre; } icono_nimg_t;
extern const icono_nimg_t ICONOS_NIMG[];
extern const uint32_t ICONOS_NIMG_N;
#endif
