# Makefile — Lua 5.5 para Nemo OS (y prueba en host sin libc)
#
# Dos objetivos:
#   make host   -> lua_host   (x86-64 Linux, SIN glibc: solo syscalls;
#                              misma libc minima que en Nemo OS)
#   make        -> lua.pro    (ARM64, para copiar a NemoFS)

.DEFAULT_GOAL := lua.pro

LUA_CORE = lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes \
           lparser lstate lstring ltable ltm lundump lvm lzio
LUA_LIB  = lauxlib lbaselib lcorolib ldblib lmathlib loadlib lstrlib ltablib lutf8lib
NEMO_C   = nemo_alloc nemo_printf nemo_string nemo_math nemo_stdio nemo_lualib nemo_main nemo_reloc

GCC_INC := $(shell $(CC) -print-file-name=include)
ifeq ($(wildcard $(GCC_INC)),)
$(warning No se encontro la carpeta de cabeceras internas de $(CC) -- stdint.h/stddef.h/stdarg.h podrian fallar)
GCC_INC_FLAG =
else
GCC_INC_FLAG = -I$(GCC_INC)
endif
COMMON_CFLAGS = -O2 -ffreestanding -fno-stack-protector -fno-builtin -nostdinc -fPIE \
    $(GCC_INC_FLAG) -Inemo/include -Isrc -Inemo -DNEMO_OS_BUILD -Wall -Wextra -Wno-unused-parameter

# ---------- host (prueba) ----------
HOST_CC = gcc
HOST_OBJ = $(addprefix build/host/,$(LUA_CORE:=.o) $(LUA_LIB:=.o) $(NEMO_C:=.o)) \
           build/host/nemo_platform_hostlinux.o build/host/nemo_setjmp_x86_64.o

host: lua_host
lua_host: $(HOST_OBJ)
	ld -pie --no-dynamic-linker -T nemo/host.ld -o $@ $(HOST_OBJ) $(shell $(HOST_CC) -print-libgcc-file-name)
build/host/%.o: src/%.c src/luaconf.h nemo/include/*.h
	@mkdir -p build/host
	$(HOST_CC) $(COMMON_CFLAGS) -DNEMO_ALLOC_POOL_SIZE='(64u*1024u*1024u)' -c $< -o $@
build/host/%.o: nemo/%.c
	@mkdir -p build/host
	$(HOST_CC) $(COMMON_CFLAGS) -DNEMO_ALLOC_POOL_SIZE='(64u*1024u*1024u)' -c $< -o $@
build/host/%.o: nemo/%.S
	@mkdir -p build/host
	$(HOST_CC) -c $< -o $@

# ---------- Nemo OS (ARM64) ----------
PREFIX  = aarch64-elf-
XCC     = $(PREFIX)gcc
XLD     = $(PREFIX)ld
XOBJCOPY= $(PREFIX)objcopy
XGCC_INC := $(shell $(XCC) -print-file-name=include 2>/dev/null)
ifeq ($(wildcard $(XGCC_INC)),)
$(warning No se encontro la carpeta de cabeceras internas de $(XCC) -- stdint.h/stddef.h/stdarg.h podrian fallar. Revisa que $(XCC) este en el PATH.)
XGCC_INC_FLAG =
else
XGCC_INC_FLAG = -I$(XGCC_INC)
endif
# Montón de Lua en Nemo OS: 4 MB (antes 12).
#
# Es el mismo para TODAS las apps en Lua, porque todas usan el mismo
# interprete. Con 12 MB cada una ocupaba una region de 16 MB y siete de
# ellas (reloj, calculadora, notas, monitor...) se comian 112 de los
# 128 MB de la reserva de tareas: no cabia ni una mas aunque hubiera
# huecos libres. Con 4 MB cada una ocupa 8 MB y caben 16, justo el
# numero de huecos.
#
# (fase 4): DE NUEVO 4 MB, pero ahora solo es el monton
# INICIAL: cuando se acaba, nemo_alloc.c pide otra zona al kernel
# (SYS_MEM_PEDIR) y el monton crece lo que haga falta. Un reloj ocupa
# poco; el visor con la guia de Lua entera pide una zona de 8 MB.
#
# (Historia:): SUBIDO A 16 MB. Con la memoria de programas sacada de la
# RAM detectada (unos 3 GB en QEMU -m 4G y en una Pi de 4 GB, ver
# src/tasks.c), los 4 MB ya no tenian razon de ser, y se quedaban cortos:
# la guia de Lua entera no cabia en el visor. Con 16 MB cada app ocupa
# unos 20 MB: las 16 de los huecos caben de sobra, y si el kernel tuviera
# que usar la reserva vieja de 128 MB aun cabrian seis.
#
# Por que basta: las imagenes NO viven en este monton -- cargar_imagen
# devuelve solo un identificador, los pixeles los guarda el kernel. Y si
# una app se pasara, Lua lanza un error limpio "not enough memory", no
# un cuelgue. Si alguna app lo necesita de verdad, es aqui donde se sube.
X_CFLAGS = -O2 -ffreestanding -fno-stack-protector -fno-builtin -nostdinc -fPIE -mcpu=cortex-a72 \
    -fno-jump-tables -fno-tree-switch-conversion \
    $(XGCC_INC_FLAG) -Inemo/include -Isrc -Inemo -DNEMO_OS_BUILD \
    -DNEMO_ALLOC_POOL_SIZE='(4u*1024u*1024u)' -Wall -Wextra -Wno-unused-parameter
X_OBJ = $(addprefix build/nemo/,$(LUA_CORE:=.o) $(LUA_LIB:=.o) $(NEMO_C:=.o)) \
        build/nemo/nemo_platform_nemo.o build/nemo/nemo_setjmp.o

lua.pro: build/nemo/lua.bin build/pro_wrap
	build/pro_wrap build/nemo/lua.bin lua.pro
build/pro_wrap: ../herramientas/pro_wrap.c
	@mkdir -p build
	gcc -O2 -o $@ $<
build/nemo/lua.bin: build/nemo/lua.elf
	$(XOBJCOPY) -O binary $< $@
build/nemo/lua.elf: $(X_OBJ) nemo/lua.ld
	$(XLD) -pie --no-dynamic-linker -T nemo/lua.ld -o $@ $(X_OBJ) $(shell $(XCC) -print-libgcc-file-name)
build/nemo/%.o: src/%.c src/luaconf.h nemo/include/*.h Makefile
	@mkdir -p build/nemo
	$(XCC) $(X_CFLAGS) -c $< -o $@
# Tambien dependen de este Makefile: si cambia una opcion (como el
# tamaño del monton, NEMO_ALLOC_POOL_SIZE), se recompilan solos.
build/nemo/%.o: nemo/%.c Makefile
	@mkdir -p build/nemo
	$(XCC) $(X_CFLAGS) -c $< -o $@
build/nemo/%.o: nemo/%.S
	@mkdir -p build/nemo
	$(XCC) -c $< -o $@

clean:
	rm -rf build lua_host lua.pro
.PHONY: host clean all
all: lua.pro
