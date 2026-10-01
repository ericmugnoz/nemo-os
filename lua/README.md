# Lua 5.5 para Nemo OS

Intérprete Lua completo corriendo dentro de Nemo OS como programa de
usuario (`LUA.PRO`), sobre una libc mínima propia. Sin glibc, sin
Nemo-Blitz, sin Nemo-AS.

## Compilar (en el Mac, con el toolchain del kernel)

    cd lua
    make            # -> lua.pro  (ARM64, listo para NemoFS)
    make host       # -> lua_host (prueba en Linux x86-64, sin libc)

## Usar

Estructura de carpetas en NemoFS:

    /PROGRAMAS/LUA.PRO                -- el interprete (build/nemo/lua.pro)
    /SISTEMA/nemo_gui.lua             -- ventanas, gadgets, eventos, teclado
    /SISTEMA/nemo_archivos.lua        -- archivos y carpetas
    /tu_script.lua                    -- donde quieras

Desde la shell:

    run hola.lua
    run ventana.lua

`require("modulo")` busca sola dentro de `SISTEMA` (instalado
automaticamente al arrancar el interprete, ver `nemo_openlibs()` en
`nemo_lualib.c`) -- no hace falta copiar las librerias junto a cada
script.

## Estructura

    src/     fuente original de Lua 5.5.1 (solo luaconf.h tocado, bloque NEMO_OS_BUILD al principio)
    nemo/    la capa de Nemo OS:
      include/*.h          cabeceras de libc propias (se compila con -nostdinc)
      nemo_alloc.c         allocator con free real, listas segregadas, 12 MB
      nemo_printf.c        snprintf con %g/%f/%e (probado contra glibc)
      nemo_string.c        string.h, ctype, strtod, locale
      nemo_math.c          libm de 18 funciones (medida en ULP contra glibc)
      nemo_stdio.c         FILE en memoria + salida por syscall
      nemo_setjmp.S        setjmp/longjmp ARM64
      nemo_reloc.c         AUTORREUBICACION al arrancar (ver abajo)
      nemo_lualib.c        libreria 'nemo' + apertura de librerias estandar
      nemo_main.c          punto de entrada comun
      nemo_platform_nemo.c syscalls de Nemo OS + _start ARM64 (pila propia 1 MB)
      nemo_platform_hostlinux.c / nemo_setjmp_x86_64.S / host.ld   solo para 'make host'
      lua.ld               linker script del .pro
    ejemplos/  hola.lua, ventana.lua, nemo_gui.lua

## La libreria 'nemo' desde Lua

    nemo.syscall(num, a0, a1, a2, a3, a4)  -- cualquier syscall; cadenas se pasan como puntero
    nemo.escribir(s)      nemo.ticks()      nemo.pump()
    nemo.leer_archivo(ruta)   nemo.memoria()
    nemo.buffer(n) / nemo.direccion(ud) / nemo.cadena(ud)   -- para syscalls que escriben en memoria

Los envoltorios comodos (ventanas, eventos) van en Lua: ver ejemplos/nemo_gui.lua.

## Autorreubicacion (importante para TODO programa C futuro)

Un .pro se enlaza en 0 y el loader lo copia a otro sitio sin reubicar:
cualquier puntero guardado como dato queda roto (la LECCION IMPORTANTE
de programas-nemo-os). Lua tiene cientos de tablas de punteros. Solucion:
se compila con -fPIE, se enlaza con -pie, y _start aplica las
reubicaciones de .rela.dyn antes de nada (nemo_reloc.c). Verificado en
el host bajo ASLR: cargado en tres direcciones aleatorias distintas,
todo funciona. El mismo esquema sirve para cualquier otro programa.

## Verificado en el host (make host): 58 comprobaciones OK
aritmetica entera/flotante, tostring/tonumber, string.format, patrones,
tablas, metatablas, clausuras, corrutinas, pcall/error, GC de 300.000
objetos, load, goto, utf8.

## Pendiente de probar en la Pi 4 (no hay cross-compilador aqui)
_start ARM64, setjmp ARM64, long double por software (libgcc), la
lectura de archivos por syscall. Si algo falla, pegar la excepcion.

## Limitaciones conocidas
- Sin 'io' ni 'os' (POSIX). Salida solo a la consola de la shell.
- fopen solo lectura, archivos enteros a memoria (solo raiz de NemoFS).
- %g: ~0,3% de valores con el 14o-17o digito +-1 respecto a glibc (casi-empates).
- Sin recursion en C profunda: LUAI_MAXCCALLS=100, LUAI_MAXSTACK=60000.
