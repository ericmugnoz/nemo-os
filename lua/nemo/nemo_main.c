// nemo_main.c — punto de entrada comun del interprete: coge el nombre
// del script del argumento de lanzamiento, lo carga y lo ejecuta.
// Identico en Nemo OS y en el host; lo que cambia es nemo_platform.
#include "lua.h"
#include "lauxlib.h"
#include "nemo_platform.h"
#include "include/string.h"
#include "include/stdlib.h"

void *nemo_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize);
void nemo_openlibs(lua_State *L);

static void out(const char *s) { nemo_plat_write(1, s, strlen(s)); }
static void err(const char *s) { nemo_plat_write(2, s, strlen(s)); }

// Al registro del sistema, con dos trozos. Lo que sale por err() va a la
// consola del programa, y un programa lanzado desde el escritorio no tiene
// consola que nadie mire: sus fallos se perdian y LUA.PRO parecia "salir
// ella misma" sin motivo. Los tres finales silenciosos de nemo_lua_main
// pasan tambien por aqui.
static void log2(const char *a, const char *b) {
    char l[256]; size_t k = 0;
    for (size_t j = 0; a[j] && k < sizeof l - 1; j++) l[k++] = a[j];
    for (size_t j = 0; b && b[j] && k < sizeof l - 1; j++) l[k++] = b[j];
    l[k] = 0;
    nemo_plat_log(l);
}

static int msghandler(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (msg == NULL) msg = "(error sin mensaje)";
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// El argumento de lanzamiento es "script.lua palabra palabra...":
// el kernel lo construye asi al redirigir un .lua a LUA.PRO. Se separa
// en el nombre del script y el resto, que se expone como la tabla
// global 'arg' (arg[0]=script, arg[1..]=palabras), como en Lua estandar.
static void push_args(lua_State *L, const char *script, const char *resto) {
    lua_newtable(L);
    lua_pushstring(L, script); lua_rawseti(L, -2, 0);
    int i = 1;
    while (*resto) {
        while (*resto == ' ') resto++;
        if (!*resto) break;
        const char *ini = resto;
        while (*resto && *resto != ' ') resto++;
        lua_pushlstring(L, ini, (size_t)(resto - ini)); lua_rawseti(L, -2, i++);
    }
    lua_setglobal(L, "arg");
}

int nemo_lua_main(void) {
    char linea[192];
    int n = nemo_plat_get_arg(linea, sizeof linea);
    if (n <= 0) {
        out("uso: run LUA.PRO ARCHIVO.lua [argumentos]\n       o directamente: run ARCHIVO.lua\n");
        log2("lua: lanzada sin argumento: no hay script que ejecutar", 0);
        return 1;
    }
    // El kernel antepone la carpeta ORIGINAL del script como
    // "INODO:script.lua argumentos" (ver el bug real corregido junto
    // a task_spawn_from_file, src/tasks.c) -- si no hay ':' (por
    // ejemplo alguien construyendo el argumento a mano sin ese
    // prefijo), se asume 0 (raiz), igual que el comportamiento de
    // siempre.
    uint32_t carpeta_script = 0;
    char *resto_linea = linea;
    {
        char *dos_puntos = strchr(linea, ':');
        if (dos_puntos) {
            uint32_t v = 0;
            char *p = linea;
            while (p < dos_puntos) { v = v * 10 + (uint32_t)(*p - '0'); p++; }
            carpeta_script = v;
            resto_linea = dos_puntos + 1;
        }
    }
    // separar "script.lua" del resto
    char script[128]; int i = 0;
    while (resto_linea[i] && resto_linea[i] != ' ' && i < (int)sizeof script - 1) { script[i] = resto_linea[i]; i++; }
    script[i] = 0;
    const char *resto = resto_linea + i;

    unsigned char *src; size_t size;
    if (nemo_plat_read_file_en(carpeta_script, script, &src, &size) != 0) {
        err("lua: no se puede abrir "); err(script); err("\n");
        log2("lua: no se puede abrir ", script);
        return 1;
    }
    // Un script de CERO bytes se compila y se ejecuta sin error: el programa
    // salia con exito y sin una sola linea, que es justo lo que se ve cuando
    // el archivo no estaba donde se buscaba (abrirlo lo crea vacio).
    if (size == 0) {
        err("lua: "); err(script); err(" esta vacio\n");
        log2("lua: script vacio (0 bytes): ", script);
        free(src);
        return 1;
    }

    lua_State *L = lua_newstate(nemo_lua_alloc, NULL, 0);
    if (!L) {
        err("lua: sin memoria para crear el estado\n");
        log2("lua: sin memoria para crear el estado", 0);
        return 1;
    }
    nemo_openlibs(L);
    push_args(L, script, resto);

    lua_pushcfunction(L, msghandler);
    int status = luaL_loadbuffer(L, (const char *)src, size, script);
    free(src);
    if (status == LUA_OK) status = lua_pcall(L, 0, 0, -2);
    if (status != LUA_OK) {
        const char *m = lua_tostring(L, -1);
        err("lua: "); err(m ? m : "error"); err("\n");
        // y tambien al registro del sistema: un programa lanzado
        // desde el escritorio o el explorador escribe en una consola que nadie
        // muestra, y su error se perdia (LUA.PRO "salia el mismo", sin mas)
        char linea[512]; size_t k = 0;
        const char *pre = "lua: ";
        for (size_t j = 0; pre[j] && k < sizeof linea - 1; j++) linea[k++] = pre[j];
        for (size_t j = 0; m && m[j] && k < sizeof linea - 1; j++) linea[k++] = m[j];
        linea[k] = 0;
        nemo_plat_log(linea);
    }
    lua_close(L);
    return status == LUA_OK ? 0 : 1;
}
