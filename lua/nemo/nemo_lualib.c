// nemo_lualib.c — la libreria 'nemo' para Lua: el puente entre un
// script y el kernel. Deliberadamente minima en C: nemo.syscall() da
// acceso a TODAS las syscalls de golpe, y los envoltorios comodos
// (ventanas, gadgets, dibujo...) se escriben en Lua encima, donde son
// mucho mas faciles de cambiar sin recompilar.

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "nemo_platform.h"
#include "include/string.h"
#include "include/stdlib.h"

void nemo_writestringerror(const char *fmt, const char *p) {
    // solo se usa con "%s\n" y similares desde lauxlib
    char buf[512]; int n = 0;
    for (const char *f = fmt; *f && n < 500; f++) {
        if (f[0] == '%' && f[1] == 's') { for (const char *q = p; *q && n < 500; q++) buf[n++] = *q; f++; }
        else if (f[0] == '%' && f[1] == '%') { buf[n++] = '%'; f++; }
        else buf[n++] = *f;
    }
    nemo_plat_write(2, buf, (size_t)n);
}

// nemo.syscall(num, a0, a1, a2, a3, a4) -> entero
// Los argumentos pueden ser enteros o cadenas (se pasa el puntero al
// texto; valido durante la llamada, que es lo que el kernel necesita).
static uint64_t arg_to_u64(lua_State *L, int idx) {
    if (lua_isnoneornil(L, idx)) return 0;
    if (lua_type(L, idx) == LUA_TSTRING) return (uint64_t)(uintptr_t)lua_tostring(L, idx);
    if (lua_type(L, idx) == LUA_TBOOLEAN) return (uint64_t)lua_toboolean(L, idx);
    return (uint64_t)luaL_checkinteger(L, idx);
}
// ---------------------------------------------------------------------
// Ceder el turno sin que el programa se acuerde
//
// EL PROBLEMA. El planificador de Nemo OS es COOPERATIVO: input_poll()
// --quien lee el raton-- solo corre en el turno del kernel, y ese turno
// solo llega cuando la tarea que corre lo cede. Un programa de Lua metido
// en su bucle de dibujo no cede por su cuenta, y durante ese rato NADIE
// lee el raton. El sintoma es el que se veia: con un programa de Lua
// abierto, el puntero va a rafagas.
//
// POR QUE NO SE HACE CON UN GANCHO DE lua_sethook, que era el plan.
// Se probo y se MIDIO, y sale carisimo: con el gancho puesto, un bucle
// cerrado pasa de 88 ms a 215 ms. Y no es el trabajo del gancho --con la
// cuenta a UN MILLON, o sea veinte llamadas en toda la ejecucion, cuesta
// exactamente lo mismo-- sino que tener instalado cualquier gancho de
// cuenta pone a la maquina virtual de Lua en un camino de despacho mas
// lento. Serian 2,3 veces mas lento TODO el Lua del sistema para que el
// raton vaya fino. Mal negocio.
//
// DONDE SE PONE ENTONCES. Aqui: en el puente por el que Lua entra al
// kernel. Un programa que dibuja hace cientos de syscalls por fotograma,
// asi que pasa por aqui constantemente y la cesion sale gratis --ya
// estabamos cruzando la frontera--. Un programa que solo calcula no pasa,
// y tampoco lo necesita: no esta pintando nada que el raton tape.
//
// LAS DOS CIFRAS:
//   * Se mira el reloj cada CADA_CUANTAS_LLAMADAS syscalls y no en todas,
//     porque leer el reloj es a su vez una syscall y saldria a una de
//     cada dos.
//   * Se cede solo si ha cambiado el LATIDO (10 ms), que es el ritmo al
//     que el kernel hace su trabajo de todas formas. Ceder mas a menudo
//     no gana nada y cuesta: cada SYS_PUMP coge el candado grande.
//
// En el host (lua_host) nemo_plat_pump() no hace nada.
#define CADA_CUANTAS_LLAMADAS 64

static void ceder_si_toca(void) {
    static uint32_t cuenta = 0;
    static int64_t ultimo_latido = -1;
    if (++cuenta < CADA_CUANTAS_LLAMADAS) return;
    cuenta = 0;
    int64_t t = nemo_plat_ticks();
    if (t == ultimo_latido) return;
    ultimo_latido = t;
    nemo_plat_pump();
}

static int l_syscall(lua_State *L) {
    uint64_t num = (uint64_t)luaL_checkinteger(L, 1);
    uint64_t r = nemo_plat_syscall(num, arg_to_u64(L, 2), arg_to_u64(L, 3), arg_to_u64(L, 4), arg_to_u64(L, 5), arg_to_u64(L, 6), arg_to_u64(L, 7));
    ceder_si_toca();
    lua_pushinteger(L, (lua_Integer)r);
    return 1;
}
// nemo.escribir(s): salida sin salto de linea
static int l_escribir(lua_State *L) { size_t n; const char *s = luaL_checklstring(L, 1, &n); nemo_plat_write(1, s, n); return 0; }
// nemo.ticks(): milisegundos del sistema
static int l_ticks(lua_State *L) { lua_pushinteger(L, nemo_plat_ticks()); return 1; }
// nemo.pump(): ceder el procesador al resto del sistema (SYS_PUMP)
static int l_pump(lua_State *L) { (void)L; nemo_plat_pump(); return 0; }
// nemo.leer_archivo(ruta) -> contenido o nil
static int l_leer_archivo(lua_State *L) {
    const char *p = luaL_checkstring(L, 1); unsigned char *buf; size_t n;
    if (nemo_plat_read_file(p, &buf, &n) != 0) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, (const char *)buf, n); free(buf); return 1;
}
// nemo.memoria() -> bytes en uso del pool
uint32_t nemo_alloc_used(void);
static int l_memoria(lua_State *L) { lua_pushinteger(L, nemo_alloc_used()); return 1; }
// nemo.buffer(n) -> userdata de n bytes, para syscalls que escriben en memoria
static int l_buffer(lua_State *L) { lua_Integer n = luaL_checkinteger(L, 1); void *p = lua_newuserdatauv(L, (size_t)n, 0); memset(p, 0, (size_t)n); return 1; }
// nemo.direccion(ud) -> entero con la direccion de un buffer
static int l_direccion(lua_State *L) { void *p = lua_touserdata(L, 1); lua_pushinteger(L, (lua_Integer)(uintptr_t)p); return 1; }
// nemo.cadena(ud[, n]) -> la cadena C que hay dentro de un buffer
static int l_cadena(lua_State *L) { const char *p = (const char *)lua_touserdata(L, 1); if (!p) { lua_pushnil(L); return 1; } lua_pushstring(L, p); return 1; }
// nemo.bytes(ud, desde, n) -> exactamente n bytes del buffer, desde el
// desplazamiento 'desde', como cadena de Lua -- CEROS INCLUIDOS (a
// diferencia de nemo.cadena, que se para en el primero). Para datos
// binarios leidos por partes con SYS_FILE_READ_AT. Error si se sale del
// buffer.
static int l_bytes(lua_State *L) {
    const uint8_t *p = (const uint8_t *)lua_touserdata(L, 1);
    lua_Integer desde = luaL_checkinteger(L, 2), n = luaL_checkinteger(L, 3);
    size_t tam = lua_rawlen(L, 1);
    if (!p || desde < 0 || n < 0 || (size_t)desde > tam || (size_t)n > tam - (size_t)desde)
        return luaL_error(L, "nemo.bytes: fuera del buffer");
    lua_pushlstring(L, (const char *)p + desde, (size_t)n);
    return 1;
}
// nemo.leer_i32(ud, offset) -> entero de 32 bits sin signo, little-endian,
// leido en el desplazamiento 'offset' (en bytes) dentro de un buffer --
// para interpretar resultados con formato binario mixto texto+enteros,
// como SYS_FILE_LIST (ver GUIA_PROGRAMACION_LUA_NEMO_OS.md), y para leer
// los pixeles de una fila traida con SYS_FILA_LEER.
//
// NO COMPROBABA LIMITES: un offset pasado de largo leia
// fuera del userdata sin decir nada, y un offset negativo leia hacia
// atras. nemo.bytes ya comprobaba; esta se habia quedado sin hacerlo.
static int l_leer_i32(lua_State *L) {
    const uint8_t *p = (const uint8_t *)lua_touserdata(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);
    size_t tam = lua_rawlen(L, 1);
    if (!p || off < 0 || (size_t)off + 4 > tam)
        return luaL_error(L, "nemo.leer_i32: fuera del buffer");
    uint32_t v = (uint32_t)p[off] | ((uint32_t)p[off+1] << 8) | ((uint32_t)p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}
// nemo.escribir_i32(ud, offset, v) -- el simetrico de leer_i32: mete un
// entero de 32 bits little-endian en el buffer. Hace falta para las
// syscalls que LEEN de un buffer del programa, como SYS_FILA_ESCRIBIR
// (antes solo habia forma de leer lo que el kernel habia dejado, no de
// prepararle datos pixel a pixel).
static int l_escribir_i32(lua_State *L) {
    uint8_t *p = (uint8_t *)lua_touserdata(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);
    uint32_t v = (uint32_t)luaL_checkinteger(L, 3);
    size_t tam = lua_rawlen(L, 1);
    if (!p || off < 0 || (size_t)off + 4 > tam)
        return luaL_error(L, "nemo.escribir_i32: fuera del buffer");
    p[off]   = (uint8_t)v;
    p[off+1] = (uint8_t)(v >> 8);
    p[off+2] = (uint8_t)(v >> 16);
    p[off+3] = (uint8_t)(v >> 24);
    return 0;
}
// nemo.cadena_en(ud, offset) -> cadena C que empieza en 'offset' dentro
// de un buffer (para el campo de nombre de una entrada de FILE_LIST).
static int l_cadena_en(lua_State *L) {
    const char *p = (const char *)lua_touserdata(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);
    lua_pushstring(L, p + off);
    return 1;
}

static const luaL_Reg nemolib[] = {
    {"syscall", l_syscall}, {"escribir", l_escribir}, {"ticks", l_ticks}, {"pump", l_pump},
    {"leer_archivo", l_leer_archivo}, {"memoria", l_memoria},
    {"buffer", l_buffer}, {"direccion", l_direccion}, {"cadena", l_cadena},
    {"leer_i32", l_leer_i32}, {"escribir_i32", l_escribir_i32},
    {"bytes", l_bytes}, {"cadena_en", l_cadena_en},
    {NULL, NULL}
};
int luaopen_nemo(lua_State *L) { luaL_newlib(L, nemolib); return 1; }

// linit propio: las librerias estandar que tienen sentido dentro de
// Nemo OS (sin 'io' ni 'os', que dependen de un sistema POSIX).
static const luaL_Reg loadedlibs[] = {
    {LUA_GNAME, luaopen_base},
    {LUA_LOADLIBNAME, luaopen_package},
    {LUA_COLIBNAME, luaopen_coroutine},
    {LUA_TABLIBNAME, luaopen_table},
    {LUA_STRLIBNAME, luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8},
    {LUA_DBLIBNAME, luaopen_debug},
    {"nemo", luaopen_nemo},
    {NULL, NULL}
};
// Arranque: instala un "buscador" mas para require(), que mira dentro
// de la carpeta /SISTEMA de NemoFS -- ahi viven nemo_gui.lua,
// nemo_archivos.lua, y cualquier otra libreria del sistema, en vez de
// sueltas en la raiz junto a los programas del usuario.
//
// Usa syscalls crudas (20=FILE_OPEN, 21=FILE_READ, 23=FILE_LIST) en
// vez de nemo_archivos.lua a proposito -- si usara require() para
// cargar SU PROPIA logica de busqueda, seria circular: la primera vez
// que alguien pida require("nemo_archivos"), este mismo buscador
// necesitaria que nemo_archivos ya estuviera cargado para encontrarlo.
static const char *BOOTSTRAP_SISTEMA =
"local FILE_READ, FILE_LIST = 21, 23\n"
"local RAIZ, VOL_NEMOFS = 0, 0\n"
"local inodo_sistema = nil\n"
"-- Una carpeta, entrada a entrada: devuelve inodo y tamaño de 'nombre', o nil\n"
"local function buscar_en(carpeta, nombre)\n"
"  local MAXE = 256\n"
"  local buf = nemo.buffer(MAXE * 40)\n"
"  local n = nemo.syscall(FILE_LIST, carpeta, nemo.direccion(buf), MAXE, VOL_NEMOFS)\n"
"  for i = 0, n - 1 do\n"
"    local off = i * 40\n"
"    if nemo.cadena_en(buf, off + 12) == nombre then return nemo.leer_i32(buf, off), nemo.leer_i32(buf, off + 8) end\n"
"  end\n"
"  return nil\n"
"end\n"
"local function encontrar_sistema()\n"
"  if not inodo_sistema then inodo_sistema = buscar_en(RAIZ, 'SISTEMA') end\n"
"  return inodo_sistema\n"
"end\n"
"-- El buscador de SISTEMA va el SEGUNDO, antes de la ruta\n"
"-- estandar ('?.lua', que el kernel busca en la raiz y en DOCUMENTOS): antes\n"
"-- iba el ultimo, y una copia antigua suelta de una biblioteca del sistema la\n"
"-- tapaba (Aronnax cargaba un aronnax_proyecto.lua viejo). Y busca el archivo\n"
"-- en la LISTA de SISTEMA: antes usaba FILE_OPEN, que CREA el archivo si no\n"
"-- existe -- un modulo inexistente se convertia en un archivo vacio en\n"
"-- SISTEMA y en un modulo vacio, sin ningun error. Lee el archivo entero (antes,\n"
"-- como mucho 64 KB).\n"
"table.insert(package.searchers, 2, function(modname)\n"
"  local ino = encontrar_sistema()\n"
"  if not ino then return \"\\n\\tno se encontro la carpeta SISTEMA en la raiz\" end\n"
"  local nombre_archivo = modname .. '.lua'\n"
"  local id, tam = buscar_en(ino, nombre_archivo)\n"
"  if not id then return \"\\n\\tno esta en SISTEMA: \" .. nombre_archivo end\n"
"  local buf = nemo.buffer(tam + 1)\n"
"  local got = nemo.syscall(FILE_READ, id, nemo.direccion(buf), tam + 1, VOL_NEMOFS)\n"
"  if got < 0 then return \"\\n\\terror leyendo SISTEMA/\" .. nombre_archivo end\n"
"  local codigo = nemo.cadena(buf):sub(1, got)\n"
"  local chunk, err = load(codigo, '@SISTEMA/' .. nombre_archivo)\n"
"  if not chunk then return \"\\n\\terror de sintaxis en \" .. nombre_archivo .. \": \" .. tostring(err) end\n"
"  return chunk\n"
"end)\n";

void nemo_openlibs(lua_State *L) {
    for (const luaL_Reg *lib = loadedlibs; lib->func; lib++) { luaL_requiref(L, lib->name, lib->func, 1); lua_pop(L, 1); }
    if (luaL_dostring(L, BOOTSTRAP_SISTEMA) != LUA_OK) {
        nemo_plat_write(2, "aviso: no se pudo instalar el buscador de SISTEMA: ", 51);
        const char *msg = lua_tostring(L, -1);
        if (msg) nemo_plat_write(2, msg, strlen(msg));
        nemo_plat_write(2, "\n", 1);
        lua_pop(L, 1);
    }
}
