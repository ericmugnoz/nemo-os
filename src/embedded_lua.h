// embedded_lua.h — Nemo OS
//
// Lua y sus programas, embebidos en el kernel e instalados en NemoFS
// al arrancar -- igual que shell.pro, explorer.pro, etc:
//   /PROGRAMAS/LUA.PRO              el interprete (lua/build/nemo/lua.bin + cabecera NEXE)
//   /SISTEMA/nemo_*.lua             las librerias (require() las busca ahi)
//   /ACCESORIOS/*.lua               los programas escritos en Lua
//   /DOCUMENTOS/*.html              paginas de ejemplo para el visor (visor.lua)
// Se sobreescriben en cada arranque, para que lo que hay en disco sea
// siempre lo que se compilo. Añadir un .lua nuevo = una linea en la
// tabla de embedded_lua.c y dos reglas en el Makefile.
#ifndef EMBEDDED_LUA_H
#define EMBEDDED_LUA_H

#include <stdint.h>

void embedded_lua_install(uint32_t programas_dir, uint32_t sistema_dir, uint32_t accesorios_dir, uint32_t documentos_dir);

#endif
