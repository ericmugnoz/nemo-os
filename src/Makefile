# Makefile del kernel
#
# Requiere el toolchain cruzado aarch64-elf, instalable en Mac con:
#   brew install aarch64-elf-gcc aarch64-elf-binutils qemu
#
# Si tu toolchain se llama distinto (por ejemplo aarch64-none-elf-*),
# cambia el prefijo CROSS abajo.

CROSS   = aarch64-elf-
CC      = $(CROSS)gcc
AS      = $(CROSS)as
LD      = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

CFLAGS  = -Wall -Wextra -ffreestanding -nostdlib -nostartfiles -mcpu=cortex-a53 -mgeneral-regs-only -O2 -fno-jump-tables -fno-tree-switch-conversion
ASFLAGS = -mcpu=cortex-a53

# nbc-selfhost (nbc.pro) usa coma flotante REAL en su lexer/generador
# de codigo (para parsear literales decimales como "3.14" a un double
# de verdad) -- a diferencia del RESTO del kernel, esto es SEGURO
# aqui: nbc.pro corre como una tarea de usuario NORMAL (cooperativa,
# via task_switch, que YA preserva d8-d15 con seguridad entre
# cambios de tarea -- ver la nota en tasks_switch.s), nunca como
# parte de la cadena de manejo de interrupciones (exceptions.s solo
# llama a handle_irq/handle_sync -> syscall_dispatch, TODOS
# compilados CON -mgeneral-regs-only, asi que NUNCA tocan registros
# de coma flotante). Si una interrupcion del temporizador llega a
# mitad de un calculo de nbc.pro, esos registros simplemente quedan
# intactos (nadie mas los toca) y el calculo continua bien al volver.
NBC_CFLAGS = -Wall -Wextra -ffreestanding -nostdlib -nostartfiles -mcpu=cortex-a53 -O2 -fno-jump-tables -fno-tree-switch-conversion

SRC_DIR = src
BUILD_DIR = build

OBJS = $(BUILD_DIR)/boot.o \
       $(BUILD_DIR)/exceptions.o \
       $(BUILD_DIR)/exceptions_c.o \
       $(BUILD_DIR)/gic.o \
       $(BUILD_DIR)/timer.o \
       $(BUILD_DIR)/mmu.o \
       $(BUILD_DIR)/heap.o \
       $(BUILD_DIR)/disk.o \
       $(BUILD_DIR)/sound.o \
       $(BUILD_DIR)/rtc.o \
       $(BUILD_DIR)/nemofs.o \
       $(BUILD_DIR)/fat.o \
       $(BUILD_DIR)/loader.o \
       $(BUILD_DIR)/hello_blob.o \
       $(BUILD_DIR)/syscall.o \
       $(BUILD_DIR)/syscall_test_blob.o \
       $(BUILD_DIR)/shell_blob.o \
       $(BUILD_DIR)/explorer_blob.o \
       $(BUILD_DIR)/editor_blob.o \
       $(BUILD_DIR)/ide_blob.o \
       $(BUILD_DIR)/gadgetdemo_blob.o \
       $(BUILD_DIR)/nbc_blob.o \
       $(BUILD_DIR)/fwcfg.o \
       $(BUILD_DIR)/ramfb.o \
       $(BUILD_DIR)/font5x7.o \
       $(BUILD_DIR)/text.o \
       $(BUILD_DIR)/input.o \
       $(BUILD_DIR)/wm.o \
       $(BUILD_DIR)/power.o \
       $(BUILD_DIR)/icons_data.o \
       $(BUILD_DIR)/dialog.o \
       $(BUILD_DIR)/gadgets.o \
       $(BUILD_DIR)/tasks_switch.o \
       $(BUILD_DIR)/tasks.o \
       $(BUILD_DIR)/kernel.o

PROGRAMS_DIR = programs

# -- nbc.pro: el compilador + ensamblador Nemo-Blitz AUTOHOSPEDADO,
# corriendo dentro de Nemo OS. A diferencia de shell/editor/etc (un
# solo archivo .c), nbc-selfhost/ tiene VARIOS archivos fuente que hay
# que compilar y enlazar juntos -- mismo patron de empaquetado
# (elf -> bin -> blob.o embebido en el kernel), pero con una regla de
# compilacion generica en vez de una por archivo.
NBC_DIR = nbc-selfhost
NBC_SRCS = nbc_main.c lexer.c ast.c parser.c codegen.c asm_lexer.c asm_encode.c assembler.c nblibc.c nb_output.c nb_runtime_nemo.c
NBC_OBJS = $(patsubst %.c,$(BUILD_DIR)/nbc_%.o,$(NBC_SRCS))

$(BUILD_DIR)/nbc_%.o: $(NBC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(NBC_CFLAGS) -I$(NBC_DIR) -c -o $@ $<

$(BUILD_DIR)/nbc.elf: $(NBC_OBJS) $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(NBC_OBJS)

$(BUILD_DIR)/nbc.bin: $(BUILD_DIR)/nbc.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/nbc_blob.o: $(BUILD_DIR)/nbc.bin
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 nbc.bin nbc_blob.o

DISK_IMG = disk.img
DISK_SIZE = 64M
FAT_IMG = fat.img
FAT_SIZE = 64M

.PHONY: all clean distclean run

all: $(BUILD_DIR)/kernel.elf

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DISK_IMG):
	qemu-img create -f raw $(DISK_IMG) $(DISK_SIZE)

$(FAT_IMG):
	qemu-img create -f raw $(FAT_IMG) $(FAT_SIZE)

$(BUILD_DIR)/boot.o: $(SRC_DIR)/boot.s | $(BUILD_DIR)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD_DIR)/exceptions.o: $(SRC_DIR)/exceptions.s | $(BUILD_DIR)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD_DIR)/exceptions_c.o: $(SRC_DIR)/exceptions.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/gic.o: $(SRC_DIR)/gic.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/timer.o: $(SRC_DIR)/timer.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/mmu.o: $(SRC_DIR)/mmu.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/heap.o: $(SRC_DIR)/heap.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/disk.o: $(SRC_DIR)/disk.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/sound.o: $(SRC_DIR)/sound.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/rtc.o: $(SRC_DIR)/rtc.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/nemofs.o: $(SRC_DIR)/nemofs.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/fat.o: $(SRC_DIR)/fat.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/loader.o: $(SRC_DIR)/loader.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# -- Programa de prueba para el loader: se ensambla y enlaza aparte,
# se extrae como binario plano, y se empaqueta como un objeto ELF con
# simbolos (_binary_hello_bin_start/_end) para poder enlazarlo
# directamente dentro del kernel.
$(BUILD_DIR)/hello.o: $(PROGRAMS_DIR)/hello.s | $(BUILD_DIR)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD_DIR)/hello.elf: $(BUILD_DIR)/hello.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(BUILD_DIR)/hello.o

$(BUILD_DIR)/hello.bin: $(BUILD_DIR)/hello.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/hello_blob.o: $(BUILD_DIR)/hello.bin
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 hello.bin hello_blob.o

$(BUILD_DIR)/syscall.o: $(SRC_DIR)/syscall.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# -- Programa de prueba de syscalls: mismo proceso que hello.s --
$(BUILD_DIR)/syscall_test.o: $(PROGRAMS_DIR)/syscall_test.s | $(BUILD_DIR)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD_DIR)/syscall_test.elf: $(BUILD_DIR)/syscall_test.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(BUILD_DIR)/syscall_test.o

$(BUILD_DIR)/syscall_test.bin: $(BUILD_DIR)/syscall_test.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/syscall_test_blob.o: $(BUILD_DIR)/syscall_test.bin
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 syscall_test.bin syscall_test_blob.o

# -- Shell: escrita en C (freestanding, mismas flags que el kernel),
# usando solo syscalls. Mismo proceso de empaquetado que los programas
# en ensamblador, salvo que se compila con gcc en vez de ensamblarse.
$(BUILD_DIR)/shell.o: $(PROGRAMS_DIR)/shell.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/shell.elf: $(BUILD_DIR)/shell.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(BUILD_DIR)/shell.o

$(BUILD_DIR)/shell.bin: $(BUILD_DIR)/shell.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/shell_blob.o: $(BUILD_DIR)/shell.bin
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 shell.bin shell_blob.o

# -- Explorador de archivos: mismo proceso que la shell --
$(BUILD_DIR)/explorer.o: $(PROGRAMS_DIR)/explorer.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/explorer.elf: $(BUILD_DIR)/explorer.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(BUILD_DIR)/explorer.o

$(BUILD_DIR)/explorer.bin: $(BUILD_DIR)/explorer.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/explorer_blob.o: $(BUILD_DIR)/explorer.bin
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 explorer.bin explorer_blob.o

# -- Editor de texto: mismo proceso que la shell y el explorador --
$(BUILD_DIR)/editor.o: $(PROGRAMS_DIR)/editor.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/editor.elf: $(BUILD_DIR)/editor.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(BUILD_DIR)/editor.o

$(BUILD_DIR)/editor.bin: $(BUILD_DIR)/editor.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/editor_blob.o: $(BUILD_DIR)/editor.bin
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 editor.bin editor_blob.o

# -- IDE (varias pestañas + compilar/ejecutar): mismo proceso --
$(BUILD_DIR)/ide.o: $(PROGRAMS_DIR)/ide.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/ide.elf: $(BUILD_DIR)/ide.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(BUILD_DIR)/ide.o

$(BUILD_DIR)/ide.bin: $(BUILD_DIR)/ide.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/ide_blob.o: $(BUILD_DIR)/ide.bin
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ide.bin ide_blob.o

# -- Demo del sistema de gadgets: mismo proceso que los demas --
$(BUILD_DIR)/gadgetdemo.o: $(PROGRAMS_DIR)/gadgetdemo.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/gadgetdemo.elf: $(BUILD_DIR)/gadgetdemo.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(BUILD_DIR)/gadgetdemo.o

$(BUILD_DIR)/gadgetdemo.bin: $(BUILD_DIR)/gadgetdemo.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/gadgetdemo_blob.o: $(BUILD_DIR)/gadgetdemo.bin
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 gadgetdemo.bin gadgetdemo_blob.o

$(BUILD_DIR)/fwcfg.o: $(SRC_DIR)/fwcfg.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/ramfb.o: $(SRC_DIR)/ramfb.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/font5x7.o: $(SRC_DIR)/font5x7.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/text.o: $(SRC_DIR)/text.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/input.o: $(SRC_DIR)/input.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/wm.o: $(SRC_DIR)/wm.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/power.o: $(SRC_DIR)/power.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/icons_data.o: $(SRC_DIR)/icons_data.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/dialog.o: $(SRC_DIR)/dialog.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/gadgets.o: $(SRC_DIR)/gadgets.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/tasks_switch.o: $(SRC_DIR)/tasks_switch.s | $(BUILD_DIR)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD_DIR)/tasks.o: $(SRC_DIR)/tasks.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/kernel.o: $(SRC_DIR)/kernel.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/kernel.elf: $(OBJS) $(SRC_DIR)/linker.ld
	$(LD) -T $(SRC_DIR)/linker.ld -o $@ $(OBJS)

run: $(BUILD_DIR)/kernel.elf $(DISK_IMG) $(FAT_IMG)
	qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -m 512M \
		-serial stdio -display cocoa,zoom-to-fit=on \
		-global virtio-mmio.force-legacy=false \
		-kernel $(BUILD_DIR)/kernel.elf \
		-drive file=$(DISK_IMG),if=none,format=raw,id=hd0 \
		-device virtio-blk-device,drive=hd0,serial=NEMOSYS \
		-drive file=$(FAT_IMG),if=none,format=raw,id=hd1 \
		-device virtio-blk-device,drive=hd1,serial=NEMOFAT \
		-device ramfb \
		-device virtio-keyboard-device,serial=NEMOKBD \
		-device virtio-tablet-device,serial=NEMOMOUSE \
		-audiodev coreaudio,id=audio0 \
		-device virtio-sound-device,audiodev=audio0

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -f $(DISK_IMG) $(FAT_IMG)
