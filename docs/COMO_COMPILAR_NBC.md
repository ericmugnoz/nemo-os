# Cómo construir `nbc.pro` (el compilador autohospedado)

`nbc_main.c` es el compilador de Nemo Basic corriendo **dentro** de
Nemo OS. Misma cadena de piezas que la versión del Mac
(`nbc_driver.c`): lexer → parser → generador → cabecera NEXE. Lo
único que cambia es de dónde sale el texto y a dónde va el binario:
syscalls de Nemo OS en vez de `fopen`/`fwrite`.

## Archivos que lo componen

```
nbc_main.c     nb_lexer.c   nb_ast.c    nb_parser.c
nb_codegen.c   nb_symtab.c  nb_codebuf.c  nb_encode.c
nb_string.c    nb_alloc.c
```

Más las cabeceras, incluida `nb_runtime_blob.h` (generada por
`tools/nb_elf_extract.py`) y `nb_syscalls.h`.

## Memoria de trabajo: el detalle que importa

`nb_alloc.c` cumple **dos papeles a la vez**: es el montón que usa el
propio compilador para su trabajo interno (el árbol, el búfer de
código, la tabla de símbolos) **y** es el código que se incrusta como
runtime dentro de cada `.pro` que genera.

El valor por defecto (2 MB) es el bueno para el segundo papel — el
que viaja dentro de los programas compilados. Pero al compilador le
queda corto: cuando compila un programa con cadenas o gráficos,
incrusta el bloque de runtime entero dentro de su propio búfer de
código.

Por eso `nbc.pro` se compila con el montón ampliado:

```
-DNB_ALLOC_POOL_SIZE='(8u*1024u*1024u)'
```

8 MB, que caben de sobra en los 16 MB que Nemo OS da a cada tarea.
El `#ifndef` que protege esa constante permite cambiarla sin tocar
`nb_alloc.c`, así que el binario **embebido** en los `.pro` sigue
usando 2 MB, sin relación con esto.

## Reglas del Makefile

Sustituir el bloque del `nbc.pro` viejo (el de `nbc_main.c` +
`lexer.c` + `codegen.c` + `asm_lexer.c` + `assembler.c`) por estos
diez archivos. Las banderas son las mismas que usan los demás
programas del sistema, más el `-D` del montón:

```makefile
NBC_DIR = nbc-selfhost
NBC_CFLAGS = -ffreestanding -mcpu=cortex-a72 -Wall -Wextra -O2 \
             -DNB_ALLOC_POOL_SIZE='(8u*1024u*1024u)' -I$(NBC_DIR)

NBC_SRCS = nbc_main.c nb_lexer.c nb_ast.c nb_parser.c nb_codegen.c \
           nb_symtab.c nb_codebuf.c nb_encode.c nb_string.c nb_alloc.c
NBC_OBJS = $(patsubst %.c,$(NBC_DIR)/%.o,$(NBC_SRCS))

$(NBC_DIR)/%.o: $(NBC_DIR)/%.c
	$(CC) $(NBC_CFLAGS) -c $< -o $@

$(NBC_DIR)/nbc.elf: $(NBC_OBJS) $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(NBC_OBJS)

$(NBC_DIR)/nbc.bin: $(NBC_DIR)/nbc.elf
	$(OBJCOPY) -O binary $< $@
```

Y volver a poner `nbc_blob.o` en las dos listas de objetos (`OBJS`
para Pi 4, `QEMU_OBJS` para QEMU), además de restaurar
`loader_install_embedded_nbc()` en `src/loader.c` (quedó como no-op
al desactivar el compilador viejo).

## Uso, desde la shell de Nemo OS

```
run nbc.pro miprograma.nb
```

Admite también `run nbc.pro 5:miprograma.nb`, donde `5` es el inodo
de la carpeta — así el explorador y el IDE pueden compilar un archivo
allí donde vive, sin copiarlo a la raíz. El `.pro` resultante se deja
**en la misma carpeta** que el fuente.

## Comprobación hecha

Los dos compiladores (el del Mac y el autohospedado) se ejecutaron
sobre los mismos once programas `.nb`, y los `.pro` resultantes
salieron **idénticos byte a byte** en los once casos. Es el mismo
código de generación detrás: lo único distinto es la entrada/salida
de archivos.
