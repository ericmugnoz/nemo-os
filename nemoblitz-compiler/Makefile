# Makefile — compilador Nemo-Blitz
#
# Este compilador corre en TU ORDENADOR (gcc normal), no dentro de
# Nemo OS -- produce ensamblador AArch64 como texto, que despues se
# ensambla con el toolchain aarch64-elf-* del propio proyecto Nemo OS.

CC = gcc
CFLAGS = -Wall -Wextra -O2

SRCS = lexer.c ast.c parser.c codegen.c main.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean test

all: nemoblitz

nemoblitz: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Compila el ejemplo de prueba a ensamblador (no lo ensambla -- eso
# necesita el toolchain aarch64-elf-* de Nemo OS)
test: nemoblitz
	./nemoblitz ejemplo.bb ejemplo.s
	@echo "Generado ejemplo.s -- ensamblalo desde la carpeta de Nemo OS con:"
	@echo "  aarch64-elf-as -o ejemplo.o ejemplo.s"
	@echo "  aarch64-elf-ld -T programs/hello_linker.ld -o ejemplo.elf ejemplo.o"

clean:
	rm -f $(OBJS) nemoblitz *.s *.o *.elf test_lexer test_parser test_codegen
