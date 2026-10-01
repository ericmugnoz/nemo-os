# Makefile -- Fase 3, con reglas EXPLICITAS para cada archivo
# (sin reglas de patron %.o:%.c, para descartar por completo
# cualquier comportamiento distinto de la version antigua de
# make que trae macOS de fabrica)

PREFIX = aarch64-elf-
CC = $(PREFIX)gcc
AS = $(PREFIX)as
LD = $(PREFIX)ld
OBJCOPY = $(PREFIX)objcopy

CFLAGS = -ffreestanding -mcpu=cortex-a72 -Wall -Wextra -O2 -Isrc -Isrc/pi4 -fno-jump-tables -fno-tree-switch-conversion

# Lua embebido: el interprete (lua.bin) y cada .lua como un blob. Ver
# embedded_lua.c para donde acaba cada uno en NemoFS. Para añadir un
# .lua nuevo: una linea aqui, una regla abajo (seccion "Lua embebido")
# y una linea en la tabla de embedded_lua.c.
LUA_LIBS = nemo_gui nemo_archivos nemo_sistema nemo_html nemo_md nemo_web nemo_gpio nemo_prueba aronnax_proyecto
LUA_APPS = particionador menued navegante navegador pintor shell editor calculadora gestor_tareas monitor_sistema reloj notas visor_imagenes visor dibujo prueba_memoria prueba_lectura gpio prueba_gpio prueba_pwm prueba_sonido prueba_i2c prueba_spi aronnax ayuda
# Paginas HTML de ejemplo (lua/html/) -> /DOCUMENTOS, las abre visor.lua
HTML_PAGES = bienvenida ayuda_html demo_lua
# La ayuda de usuario (lua/ayuda/) -> /DOCUMENTOS/AYUDA, la abre ayuda.lua.
# Los archivos se llaman ayuda_X.html en el proyecto para que el simbolo que
# genera objcopy (_binary_ayuda_X_html_start) no choque con nada, pero se
# instalan como X.html, que es como se enlazan entre ellas.
AYUDA_PAGES = indice primeros archivos programas ajustes teclas problemas programar
# Guias en Markdown (docs/ y lua/) -> /DOCUMENTOS, las abre visor.lua
# Carpeta donde objcopy deja los blobs. No viaja en el repositorio (solo
# contiene archivos generados), asi que cada regla que escribe ahi la pide
# como dependencia de orden --la barra |--: make la crea antes de la primera
# y no la vuelve a mirar. Sin esto, un arbol recien clonado falla en el
# primer objcopy con "No such file or directory".
BLOBS_DIR = src/pi4/blobs

GUIA_BLOBS = src/pi4/blobs/indice_manuales_blob.o src/pi4/blobs/guia_nb_blob.o src/pi4/blobs/guia_nb2_blob.o src/pi4/blobs/guia_nb3_blob.o src/pi4/blobs/guia_nb4_blob.o src/pi4/blobs/guia_nb5_blob.o src/pi4/blobs/ref_nb_blob.o src/pi4/blobs/ref_nb2_blob.o src/pi4/blobs/ref_nb3_blob.o src/pi4/blobs/ref_nb4_blob.o src/pi4/blobs/ref_nb5_blob.o src/pi4/blobs/fondo_blob.o src/pi4/blobs/menu_cfg_blob.o src/pi4/blobs/guia_nimg_blob.o src/pi4/blobs/guia_ambito_blob.o src/pi4/blobs/guia_iconos_blob.o src/pi4/blobs/guia_aronnax_blob.o src/pi4/blobs/ejemplo_aronnax_blob.o src/pi4/blobs/ejemplo_editor_blob.o src/pi4/blobs/guia_gpio_blob.o src/pi4/blobs/guia_red_blob.o src/pi4/blobs/guia_html_blob.o src/pi4/blobs/guia_lua_blob.o src/pi4/blobs/guia_lua_sys_blob.o src/pi4/blobs/guia_lua_sys2_blob.o
LUA_BLOBS = src/pi4/blobs/otros_blob.o src/pi4/blobs/lua_blob.o $(foreach f,$(LUA_LIBS) $(LUA_APPS),src/pi4/blobs/lua_$(f)_blob.o) $(foreach f,$(HTML_PAGES),src/pi4/blobs/html_$(f)_blob.o) $(foreach f,$(AYUDA_PAGES),src/pi4/blobs/ayuda_$(f)_blob.o) src/pi4/blobs/fuentes_blob.o $(GUIA_BLOBS)

OBJS = src/kernel.o src/bitacora.o src/wm.o src/gadgets.o src/icons_data.o src/heap.o src/timer.o src/exceptions.o src/tasks.o src/syscall.o src/text.o src/font5x7.o src/fat.o src/nemofs.o src/dialog.o src/net.o src/tcp.o src/tcp_comun.o src/tcp_cliente.o src/http.o src/descarga.o src/nmz.o src/nmz_fs.o src/udp.o src/udp_sock.o src/ip_texto.o src/dhcp.o src/arp.o src/netshell.o src/smp.o src/memoria.o src/gpio.o src/i2c.o src/spi.o src/bkl.o src/medir.o src/fonts.o src/pi4/power_pi4.o src/rtc.o src/loader.o src/tasks_switch.o src/exceptions_asm.o src/pi4/uart_pi4.o src/pi4/mailbox_pi4.o src/pi4/ramfb_pi4.o src/pi4/gic_pi4.o src/pi4/mmu_pi4.o src/pi4/disk_pi4.o src/pi4/input_pi4.o src/pi4/sound_pi4.o src/pi4/pcie_pi4.o src/pi4/xhci_pi4.o src/pi4/genet_pi4.o src/pi4/blobs/hello_blob.o src/pi4/blobs/syscall_test_blob.o src/pi4/blobs/shell_blob.o src/pi4/blobs/explorer_blob.o src/pi4/blobs/editor_blob.o src/pi4/blobs/ide_blob.o src/pi4/blobs/gadgetdemo_blob.o src/pi4/blobs/desktoped_blob.o src/pi4/blobs/screensettings_blob.o src/pi4/blobs/nbc_blob.o src/embedded_lua.o src/otros_programas.o src/iconos_nimg.o $(LUA_BLOBS) src/pi4/start_pi4.o

all: preparar_carpetas kernel8.img

preparar_carpetas:
	mkdir -p src/pi4/blobs programs nbc-selfhost

$(BLOBS_DIR):
	mkdir -p $(BLOBS_DIR)

src/kernel.o: src/kernel.c
	$(CC) $(CFLAGS) -c src/kernel.c -o src/kernel.o

src/wm.o: src/wm.c
	$(CC) $(CFLAGS) -c src/wm.c -o src/wm.o

src/gadgets.o: src/gadgets.c
	$(CC) $(CFLAGS) -c src/gadgets.c -o src/gadgets.o

src/icons_data.o: src/icons_data.c
	$(CC) $(CFLAGS) -c src/icons_data.c -o src/icons_data.o

src/heap.o: src/heap.c
	$(CC) $(CFLAGS) -c src/heap.c -o src/heap.o

src/bitacora.o: src/bitacora.c src/bitacora.h
	$(CC) $(CFLAGS) -c src/bitacora.c -o src/bitacora.o

src/timer.o: src/timer.c
	$(CC) $(CFLAGS) -c src/timer.c -o src/timer.o

src/exceptions.o: src/exceptions.c
	$(CC) $(CFLAGS) -c src/exceptions.c -o src/exceptions.o

src/tasks.o: src/tasks.c
	$(CC) $(CFLAGS) -c src/tasks.c -o src/tasks.o

src/syscall.o: src/syscall.c
	$(CC) $(CFLAGS) -c src/syscall.c -o src/syscall.o

src/text.o: src/text.c
	$(CC) $(CFLAGS) -c src/text.c -o src/text.o

src/font5x7.o: src/font5x7.c
	$(CC) $(CFLAGS) -c src/font5x7.c -o src/font5x7.o

src/fat.o: src/fat.c
	$(CC) $(CFLAGS) -c src/fat.c -o src/fat.o

src/nemofs.o: src/nemofs.c
	$(CC) $(CFLAGS) -c src/nemofs.c -o src/nemofs.o

src/net.o: src/net.c src/net.h src/tcp.h src/nic.h src/pi4/genet_pi4.h
	$(CC) $(CFLAGS) -c src/net.c -o src/net.o

src/tcp.o: src/tcp.c src/tcp.h
	$(CC) $(CFLAGS) -c src/tcp.c -o src/tcp.o

src/udp.o: src/udp.c src/udp.h
	$(CC) $(CFLAGS) -c src/udp.c -o src/udp.o

src/udp_sock.o: src/udp_sock.c src/udp_sock.h
	$(CC) $(CFLAGS) -c src/udp_sock.c -o src/udp_sock.o

src/ip_texto.o: src/ip_texto.c src/ip_texto.h
	$(CC) $(CFLAGS) -c src/ip_texto.c -o src/ip_texto.o

src/dhcp.o: src/dhcp.c src/dhcp.h
	$(CC) $(CFLAGS) -c src/dhcp.c -o src/dhcp.o

src/arp.o: src/arp.c src/arp.h
	$(CC) $(CFLAGS) -c src/arp.c -o src/arp.o

src/tcp_comun.o: src/tcp_comun.c src/tcp_comun.h
	$(CC) $(CFLAGS) -c src/tcp_comun.c -o src/tcp_comun.o

src/tcp_cliente.o: src/tcp_cliente.c src/tcp_cliente.h src/tcp_comun.h
	$(CC) $(CFLAGS) -c src/tcp_cliente.c -o src/tcp_cliente.o

src/http.o: src/http.c src/http.h src/tcp_cliente.h
	$(CC) $(CFLAGS) -c src/http.c -o src/http.o

src/descarga.o: src/descarga.c src/descarga.h src/http.h
	$(CC) $(CFLAGS) -c src/descarga.c -o src/descarga.o

src/nmz.o: src/nmz.c src/nmz.h
	$(CC) $(CFLAGS) -c src/nmz.c -o src/nmz.o

src/nmz_fs.o: src/nmz_fs.c src/nmz.h
	$(CC) $(CFLAGS) -c src/nmz_fs.c -o src/nmz_fs.o

src/netshell.o: src/netshell.c src/tcp.h src/nemofs.h src/tasks.h src/wm.h
	$(CC) $(CFLAGS) -c src/netshell.c -o src/netshell.o

# Varios nucleos (SMP). En la Pi 4, de momento solo compila y dice que
# despertar los nucleos esta pendiente (fase 3b).
src/smp.o: src/smp.c src/smp.h src/cpu.h src/mmu.h
	$(CC) $(CFLAGS) -c src/smp.c -o src/smp.o

src/memoria.o: src/memoria.c src/memoria.h src/pi4/mailbox_pi4.h
	$(CC) $(CFLAGS) -c src/memoria.c -o src/memoria.o

src/gpio.o: src/gpio.c src/gpio.h src/tasks.h
	$(CC) $(CFLAGS) -c src/gpio.c -o src/gpio.o
# La biblioteca de iconos NIMG (la genera herramientas/iconos/crear_iconos.py;
# el .c generado se guarda en el proyecto: no hace falta Pillow para compilar)
src/iconos_nimg.o: src/iconos_nimg.c src/iconos_nimg.h
	$(CC) $(CFLAGS) -c src/iconos_nimg.c -o src/iconos_nimg.o

src/i2c.o: src/i2c.c src/i2c.h src/gpio.h src/pi4/mailbox_pi4.h
	$(CC) $(CFLAGS) -c src/i2c.c -o src/i2c.o

src/spi.o: src/spi.c src/spi.h src/gpio.h src/pi4/mailbox_pi4.h
	$(CC) $(CFLAGS) -c src/spi.c -o src/spi.o

# Candado grande del kernel (SMP, fase 4).
src/bkl.o: src/bkl.c src/bkl.h src/spinlock.h src/cpu.h
	$(CC) $(CFLAGS) -c src/bkl.c -o src/bkl.o

# Medicion de rendimiento del SMP (diagnostico; ver medir.h).
src/medir.o: src/medir.c src/medir.h src/cpu.h
	$(CC) $(CFLAGS) -c src/medir.c -o src/medir.o


src/fonts.o: src/fonts.c src/fonts.h
	$(CC) $(CFLAGS) -c src/fonts.c -o src/fonts.o

# Paquete de fuentes proporcionales (fuentes/FUENTES.NFP, generado con
# herramientas/nfnt_pack.py a partir de DejaVu -- se guarda generado en
# el repositorio para no exigir Pillow ni las TTF en la maquina de
# compilacion). objcopy: FUENTES.NFP -> _binary_FUENTES_NFP_start/_end.
# --- "otros programas": codigo fuente en Nemo Basic, uno por carpeta ---
#
# Va todo en UN paquete en vez de un blob por archivo, por tres razones que
# conviene no olvidar (estan largas en herramientas/empaquetar_otros.py):
# cada programa necesita sus piezas juntas porque usa Include con nombres
# relativos; los nombres llevan espacios y make parte las dependencias por
# espacios; y asi añadir un programa nuevo no toca ni el Makefile ni el C.
#
# El paquete se rehace en cada 'make' a proposito: la carpeta no puede ser
# dependencia de make (los espacios parten la lista), asi que la unica forma
# de que un programa nuevo entre sin tener que acordarse de nada es
# reconstruirlo siempre. Cuesta un reenlace del kernel por compilacion --
# unos segundos-- y a cambio no hay forma de generar una imagen a la que le
# falte algo que ya esta en la carpeta.
OTROS_DIR = otros programas
OTROS_PAQUETE = build_otros/otros.npak

$(OTROS_PAQUETE): herramientas/empaquetar_otros.py
	python3 herramientas/empaquetar_otros.py "$(OTROS_DIR)" $(OTROS_PAQUETE)

# Rehacer el paquete en cada 'make': la carpeta no se puede poner como
# dependencia (espacios), asi que se fuerza aqui. El script no toca el
# archivo si no ha cambiado nada.
.PHONY: otros-paquete
otros-paquete:
	@python3 herramientas/empaquetar_otros.py "$(OTROS_DIR)" $(OTROS_PAQUETE)

src/pi4/blobs/otros_blob.o: $(OTROS_PAQUETE) | $(BLOBS_DIR)
	cd build_otros && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 otros.npak ../src/pi4/blobs/otros_blob.o

src/pi4/blobs/fuentes_blob.o: fuentes/FUENTES.NFP | $(BLOBS_DIR)
	cd fuentes && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 FUENTES.NFP ../src/pi4/blobs/fuentes_blob.o

# Regenerar el paquete (opcional; necesita python3 + Pillow + fuentes DejaVu)
fuentes: 
	python3 herramientas/nfnt_pack.py fuentes/FUENTES.NFP

src/dialog.o: src/dialog.c
	$(CC) $(CFLAGS) -c src/dialog.c -o src/dialog.o

# power.c (PSCI/HVC) es la version para QEMU; en la Pi 4 real no hay
# firmware EL3 que atienda esa llamada, asi que se usa power_pi4.c
# (watchdog de hardware del BCM2711) en su lugar -- ver la nota en el
# propio archivo.
src/pi4/power_pi4.o: src/pi4/power_pi4.c
	$(CC) $(CFLAGS) -c src/pi4/power_pi4.c -o src/pi4/power_pi4.o

src/rtc.o: src/rtc.c
	$(CC) $(CFLAGS) -c src/rtc.c -o src/rtc.o

# Memoria REAL que necesita cada programa del sistema, leida de su ELF.
#
# El instalador del kernel (loader.c) la usa para escribir cabeceras
# NEXE version 3, asi cada programa recibe su tamaño y no 16MB a
# ciegas. Medido: la shell necesita 80KB y el editor medio MB, pero
# antes ocho programas del sistema se comian los 128MB de la reserva.
#
# El dato solo existe en el ELF -- al empaquetar con objcopy se pierde
# el .bss -- por eso se calcula aqui, al compilar, y no a mano: una
# tabla escrita a mano se quedaria vieja al primer cambio.
PRO_SIZES_PI4 = src/pi4/pro_sizes_pi4.h
$(PRO_SIZES_PI4): programs/shell.elf programs/explorer.elf programs/desktoped.elf programs/screensettings.elf programs/editor.elf programs/gadgetdemo.elf programs/ide.elf programs/syscall_test.elf nbc-selfhost/nbc.elf lua/build/nemo/lua.bin herramientas/pro_sizes.py
	python3 herramientas/pro_sizes.py -o $@ shell=programs/shell.elf explorer=programs/explorer.elf desktoped=programs/desktoped.elf screensettings=programs/screensettings.elf editor=programs/editor.elf gadgetdemo=programs/gadgetdemo.elf ide=programs/ide.elf syscall_test=programs/syscall_test.elf nbc=nbc-selfhost/nbc.elf lua=lua/build/nemo/lua.elf

src/loader.o: src/loader.c $(PRO_SIZES_PI4)
	$(CC) $(CFLAGS) -c src/loader.c -o src/loader.o

src/tasks_switch.o: src/tasks_switch.s
	$(AS) -mcpu=cortex-a72 src/tasks_switch.s -o src/tasks_switch.o

src/exceptions_asm.o: src/exceptions.s
	$(AS) -mcpu=cortex-a72 src/exceptions.s -o src/exceptions_asm.o

src/pi4/uart_pi4.o: src/pi4/uart_pi4.c
	$(CC) $(CFLAGS) -c src/pi4/uart_pi4.c -o src/pi4/uart_pi4.o

src/pi4/mailbox_pi4.o: src/pi4/mailbox_pi4.c
	$(CC) $(CFLAGS) -c src/pi4/mailbox_pi4.c -o src/pi4/mailbox_pi4.o

src/pi4/ramfb_pi4.o: src/pi4/ramfb_pi4.c
	$(CC) $(CFLAGS) -c src/pi4/ramfb_pi4.c -o src/pi4/ramfb_pi4.o

src/pi4/gic_pi4.o: src/pi4/gic_pi4.c
	$(CC) $(CFLAGS) -c src/pi4/gic_pi4.c -o src/pi4/gic_pi4.o

src/pi4/mmu_pi4.o: src/pi4/mmu_pi4.c
	$(CC) $(CFLAGS) -c src/pi4/mmu_pi4.c -o src/pi4/mmu_pi4.o

src/pi4/disk_pi4.o: src/pi4/disk_pi4.c
	$(CC) $(CFLAGS) -c src/pi4/disk_pi4.c -o src/pi4/disk_pi4.o

src/pi4/pcie_pi4.o: src/pi4/pcie_pi4.c src/pi4/pcie_pi4.h
	$(CC) $(CFLAGS) -c src/pi4/pcie_pi4.c -o src/pi4/pcie_pi4.o

src/pi4/xhci_pi4.o: src/pi4/xhci_pi4.c src/pi4/xhci_pi4.h src/pi4/pcie_pi4.h
	$(CC) $(CFLAGS) -c src/pi4/xhci_pi4.c -o src/pi4/xhci_pi4.o

src/pi4/genet_pi4.o: src/pi4/genet_pi4.c src/pi4/genet_pi4.h src/pi4/mailbox_pi4.h
	$(CC) $(CFLAGS) -c src/pi4/genet_pi4.c -o src/pi4/genet_pi4.o

src/pi4/input_pi4.o: src/pi4/input_pi4.c src/input.h src/pi4/pcie_pi4.h src/pi4/xhci_pi4.h
	$(CC) $(CFLAGS) -c src/pi4/input_pi4.c -o src/pi4/input_pi4.o

# El sonido de la Pi 4: PWM1 al jack de 3,5 mm. Sustituye al stub vacio que
# habia aqui (sound_pi4_stub.c, ya borrado).
src/pi4/sound_pi4.o: src/pi4/sound_pi4.c
	$(CC) $(CFLAGS) -c src/pi4/sound_pi4.c -o src/pi4/sound_pi4.o

src/pi4/start_pi4.o: src/pi4/start_pi4.S
	$(AS) -mcpu=cortex-a72 src/pi4/start_pi4.S -o src/pi4/start_pi4.o



# ============================================================
# Programas reales embebidos (sustituyen los blobs placeholder
# de 4 bytes usados en la integracion inicial del kernel)
# ============================================================

PROGRAMS_DIR = programs
NBC_DIR = nbc-selfhost

# --- hello.pro: version Pi4, toca el UART real directamente ---
programs/hello_pi4.o: $(PROGRAMS_DIR)/hello_pi4.s
	$(AS) -mcpu=cortex-a72 $(PROGRAMS_DIR)/hello_pi4.s -o programs/hello_pi4.o

programs/hello.elf: programs/hello_pi4.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o programs/hello.elf programs/hello_pi4.o

programs/hello.bin: programs/hello.elf
	$(OBJCOPY) -O binary programs/hello.elf programs/hello.bin

src/pi4/blobs/hello_blob.o: programs/hello.bin | $(BLOBS_DIR)
	cd programs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 hello.bin ../src/pi4/blobs/hello_blob.o

# --- syscall_test.pro: identico a QEMU, solo usa syscalls ---
programs/syscall_test.o: $(PROGRAMS_DIR)/syscall_test.s
	$(AS) -mcpu=cortex-a72 $(PROGRAMS_DIR)/syscall_test.s -o programs/syscall_test.o

programs/syscall_test.elf: programs/syscall_test.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o programs/syscall_test.elf programs/syscall_test.o

programs/syscall_test.bin: programs/syscall_test.elf
	$(OBJCOPY) -O binary programs/syscall_test.elf programs/syscall_test.bin

src/pi4/blobs/syscall_test_blob.o: programs/syscall_test.bin | $(BLOBS_DIR)
	cd programs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 syscall_test.bin ../src/pi4/blobs/syscall_test_blob.o

# --- shell.pro, explorer.pro, editor.pro, ide.pro, gadgetdemo.pro:
#     identicos a QEMU, solo usan syscalls ---
programs/shell.o: $(PROGRAMS_DIR)/shell.c
	$(CC) $(CFLAGS) -c $(PROGRAMS_DIR)/shell.c -o programs/shell.o

programs/shell.elf: programs/shell.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o programs/shell.elf programs/shell.o

programs/shell.bin: programs/shell.elf
	$(OBJCOPY) -O binary programs/shell.elf programs/shell.bin

src/pi4/blobs/shell_blob.o: programs/shell.bin | $(BLOBS_DIR)
	cd programs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 shell.bin ../src/pi4/blobs/shell_blob.o

programs/explorer.o: $(PROGRAMS_DIR)/explorer.c $(PROGRAMS_DIR)/barra.h
	$(CC) $(CFLAGS) -c $(PROGRAMS_DIR)/explorer.c -o programs/explorer.o

programs/explorer.elf: programs/explorer.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o programs/explorer.elf programs/explorer.o

programs/explorer.bin: programs/explorer.elf
	$(OBJCOPY) -O binary programs/explorer.elf programs/explorer.bin

src/pi4/blobs/explorer_blob.o: programs/explorer.bin | $(BLOBS_DIR)
	cd programs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 explorer.bin ../src/pi4/blobs/explorer_blob.o

programs/desktoped.o: $(PROGRAMS_DIR)/desktoped.c $(PROGRAMS_DIR)/barra.h
	$(CC) $(CFLAGS) -c $(PROGRAMS_DIR)/desktoped.c -o programs/desktoped.o

programs/desktoped.elf: programs/desktoped.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o programs/desktoped.elf programs/desktoped.o

programs/desktoped.bin: programs/desktoped.elf
	$(OBJCOPY) -O binary programs/desktoped.elf programs/desktoped.bin

src/pi4/blobs/desktoped_blob.o: programs/desktoped.bin | $(BLOBS_DIR)
	cd programs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 desktoped.bin ../src/pi4/blobs/desktoped_blob.o

programs/screensettings.o: $(PROGRAMS_DIR)/screensettings.c
	$(CC) $(CFLAGS) -c $(PROGRAMS_DIR)/screensettings.c -o programs/screensettings.o

programs/screensettings.elf: programs/screensettings.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o programs/screensettings.elf programs/screensettings.o

programs/screensettings.bin: programs/screensettings.elf
	$(OBJCOPY) -O binary programs/screensettings.elf programs/screensettings.bin

src/pi4/blobs/screensettings_blob.o: programs/screensettings.bin | $(BLOBS_DIR)
	cd programs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 screensettings.bin ../src/pi4/blobs/screensettings_blob.o

programs/editor.o: $(PROGRAMS_DIR)/editor.c
	$(CC) $(CFLAGS) -c $(PROGRAMS_DIR)/editor.c -o programs/editor.o

programs/editor.elf: programs/editor.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o programs/editor.elf programs/editor.o

programs/editor.bin: programs/editor.elf
	$(OBJCOPY) -O binary programs/editor.elf programs/editor.bin

src/pi4/blobs/editor_blob.o: programs/editor.bin | $(BLOBS_DIR)
	cd programs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 editor.bin ../src/pi4/blobs/editor_blob.o

programs/ide.o: $(PROGRAMS_DIR)/ide.c
	$(CC) $(CFLAGS) -c $(PROGRAMS_DIR)/ide.c -o programs/ide.o

programs/ide.elf: programs/ide.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o programs/ide.elf programs/ide.o

programs/ide.bin: programs/ide.elf
	$(OBJCOPY) -O binary programs/ide.elf programs/ide.bin

src/pi4/blobs/ide_blob.o: programs/ide.bin | $(BLOBS_DIR)
	cd programs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ide.bin ../src/pi4/blobs/ide_blob.o

programs/gadgetdemo.o: $(PROGRAMS_DIR)/gadgetdemo.c
	$(CC) $(CFLAGS) -c $(PROGRAMS_DIR)/gadgetdemo.c -o programs/gadgetdemo.o

programs/gadgetdemo.elf: programs/gadgetdemo.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o programs/gadgetdemo.elf programs/gadgetdemo.o

programs/gadgetdemo.bin: programs/gadgetdemo.elf
	$(OBJCOPY) -O binary programs/gadgetdemo.elf programs/gadgetdemo.bin

src/pi4/blobs/gadgetdemo_blob.o: programs/gadgetdemo.bin | $(BLOBS_DIR)
	cd programs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 gadgetdemo.bin ../src/pi4/blobs/gadgetdemo_blob.o

# --- nbc.pro: el compilador de Nemo Basic AUTOHOSPEDADO ---
#
# Diez archivos: el punto de entrada (nbc_main.c, que lee el .nb y
# escribe el .pro con las syscalls de Nemo OS) y las nueve piezas del
# compilador, las MISMAS que usa la version del Mac (nbc_driver.c).
#
# EL MONTON AMPLIADO NO ES OPCIONAL: nb_alloc.c cumple dos papeles a
# la vez -- es la memoria de trabajo del propio compilador Y el codigo
# que se incrusta como runtime dentro de cada .pro que genera. Los 2MB
# por defecto son los buenos para lo segundo (van dentro de cada
# programa compilado), pero al compilador se le quedan cortos: al
# compilar algo con cadenas o graficos mete el bloque de runtime
# entero dentro de su propio buffer de codigo. 8MB caben de sobra en
# los 16MB que Nemo OS da a cada tarea.
# -fno-jump-tables -fno-tree-switch-conversion: como en QEMU. nbc.pro se
# enlaza en 0 y se carga sin reubicar punteros de DATOS, y con -O2 gcc
# convierte algunos switch en tablas de punteros (CSWTCH) que dentro de
# Nemo OS apuntarian a direcciones de enlace. Por la misma razon, el codigo
# del compilador no usa tablas { const char *... }. Se puede comprobar que
# no quede ninguno con: objdump -r sobre cada .o del compilador, contando
# las reubicaciones en secciones de DATOS (tienen que ser cero).
NBC_CFLAGS = -ffreestanding -mcpu=cortex-a72 -Wall -Wextra -O2 -fno-jump-tables -fno-tree-switch-conversion \
             -DNB_ALLOC_POOL_SIZE='(8u*1024u*1024u)' -I$(NBC_DIR)

NBC_SRCS = nbc_main.c nb_lexer.c nb_ast.c nb_parser.c nb_codegen.c nb_symtab.c nb_codebuf.c nb_encode.c nb_string.c nb_alloc.c nb_include.c
NBC_OBJS = $(patsubst %.c,$(NBC_DIR)/%.o,$(NBC_SRCS))

$(NBC_DIR)/%.o: $(NBC_DIR)/%.c
	$(CC) $(NBC_CFLAGS) -c $< -o $@

nbc-selfhost/nbc.elf: $(NBC_OBJS) $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o nbc-selfhost/nbc.elf $(NBC_OBJS)

nbc-selfhost/nbc.bin: nbc-selfhost/nbc.elf
	$(OBJCOPY) -O binary nbc-selfhost/nbc.elf nbc-selfhost/nbc.bin

src/pi4/blobs/nbc_blob.o: nbc-selfhost/nbc.bin | $(BLOBS_DIR)
	cd nbc-selfhost && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 nbc.bin ../src/pi4/blobs/nbc_blob.o

# --- El runtime que nbc.pro INCRUSTA en cada .pro que compila ---
#
# nb_runtime_blob.h es codigo maquina ARM64 ya resuelto (nb_alloc +
# nb_string + nb_convert + nb_math), listo para copiarlo dentro de
# cada programa compilado. Lo genera tools/nb_elf_extract.py, que hace
# de enlazador en miniatura: combina los .o, coloca .text/.rodata/.bss
# y resuelve las reubicaciones.
#
# ESTO SE REGENERA SOLO, a proposito. Antes era un paso manual "porque
# solo hace falta al tocar el runtime" -- pero 'make clean' borra los
# .o, asi que al rehacerlo era facil quedarse con una cabecera vieja
# compilada dentro del kernel. Paso justo eso: un fallo en el
# compilador dificil de rastrear, causado por una cabecera
# desactualizada. Con estas reglas, tocar cualquiera de los cuatro
# fuentes del runtime regenera la cabecera sin que nadie tenga que
# acordarse.
#
# OJO: estos .o son SOLO para el bloque incrustado, y se compilan con
# el monton por defecto (2MB) -- que es el que viaja dentro de cada
# .pro. No confundir con los .o del propio compilador, que llevan
# NB_ALLOC_POOL_SIZE ampliado (ver arriba).
NBRT_DIR = $(NBC_DIR)
NBRT_SRCS = nb_alloc.c nb_string.c nb_convert.c nb_math.c
NBRT_OBJS = $(patsubst %.c,$(NBRT_DIR)/rt_%.o,$(NBRT_SRCS))
NBRT_CFLAGS = -ffreestanding -mcpu=cortex-a72 -O2

$(NBRT_DIR)/rt_%.o: $(NBRT_DIR)/%.c
	$(CC) $(NBRT_CFLAGS) -c $< -o $@

$(NBRT_DIR)/nb_runtime_blob.h: $(NBRT_OBJS) $(NBRT_DIR)/tools/nb_elf_extract.py
	python3 $(NBRT_DIR)/tools/nb_elf_extract.py $(NBRT_OBJS) -o $@

# Todo el compilador depende de la cabecera: si el runtime cambia, se
# recompila entero.
$(NBC_OBJS): $(NBRT_DIR)/nb_runtime_blob.h
# (la dependencia equivalente para el build de QEMU va mas abajo,
# donde QEMU_NBC_OBJS ya esta definida -- make expande las variables
# de una regla en el momento de leerla, asi que aqui estaria vacia)


# ============================================================
# Lua embebido -- ver embedded_lua.c
# ============================================================

src/embedded_lua.o: src/embedded_lua.c src/embedded_lua.h $(PRO_SIZES_PI4)
	$(CC) $(CFLAGS) -c src/embedded_lua.c -o src/embedded_lua.o

src/otros_programas.o: src/otros_programas.c src/otros_programas.h
	$(CC) $(CFLAGS) -c src/otros_programas.c -o src/otros_programas.o

# El interprete se construye con su propio Makefile (lua/Makefile), que
# lleva su propia libc y sus propias banderas. Se pide el .bin CRUDO,
# no lua.pro: la cabecera NEXE la añade el instalador del kernel, y
# pro_wrap tambien la añadiria -> cabecera doble. Se delega siempre en
# el sub-make (FORCE): el decide si hay algo que recompilar.
lua/build/nemo/lua.bin: FORCE
	$(MAKE) -C lua build/nemo/lua.bin

FORCE:

src/pi4/blobs/lua_blob.o: lua/build/nemo/lua.bin | $(BLOBS_DIR)
	cd lua/build/nemo && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 lua.bin ../../../src/pi4/blobs/lua_blob.o

# Un blob por .lua. objcopy nombra los simbolos por el archivo de
# entrada (nemo_gui.lua -> _binary_nemo_gui_lua_start), por eso el
# "cd" al directorio: asi el nombre no arrastra la ruta.
src/pi4/blobs/lua_nemo_gui_blob.o: lua/ejemplos/nemo_gui.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 nemo_gui.lua ../../src/pi4/blobs/lua_nemo_gui_blob.o

src/pi4/blobs/lua_nemo_archivos_blob.o: lua/ejemplos/nemo_archivos.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 nemo_archivos.lua ../../src/pi4/blobs/lua_nemo_archivos_blob.o

src/pi4/blobs/lua_nemo_web_blob.o: lua/ejemplos/nemo_web.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 nemo_web.lua ../../src/pi4/blobs/lua_nemo_web_blob.o

src/pi4/blobs/lua_navegador_blob.o: lua/ejemplos/navegador.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 navegador.lua ../../src/pi4/blobs/lua_navegador_blob.o

src/pi4/blobs/lua_nemo_sistema_blob.o: lua/ejemplos/nemo_sistema.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 nemo_sistema.lua ../../src/pi4/blobs/lua_nemo_sistema_blob.o

src/pi4/blobs/lua_menued_blob.o: lua/ejemplos/menued.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 menued.lua ../../src/pi4/blobs/lua_menued_blob.o
src/pi4/blobs/lua_navegante_blob.o: lua/ejemplos/navegante.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 navegante.lua ../../src/pi4/blobs/lua_navegante_blob.o
src/pi4/blobs/lua_pintor_blob.o: lua/ejemplos/pintor.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 pintor.lua ../../src/pi4/blobs/lua_pintor_blob.o
src/pi4/blobs/lua_shell_blob.o: lua/ejemplos/shell.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 shell.lua ../../src/pi4/blobs/lua_shell_blob.o

src/pi4/blobs/lua_editor_blob.o: lua/ejemplos/editor.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 editor.lua ../../src/pi4/blobs/lua_editor_blob.o

src/pi4/blobs/lua_calculadora_blob.o: lua/ejemplos/calculadora.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 calculadora.lua ../../src/pi4/blobs/lua_calculadora_blob.o

src/pi4/blobs/lua_gestor_tareas_blob.o: lua/ejemplos/gestor_tareas.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 gestor_tareas.lua ../../src/pi4/blobs/lua_gestor_tareas_blob.o

src/pi4/blobs/lua_particionador_blob.o: lua/ejemplos/particionador.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 particionador.lua ../../src/pi4/blobs/lua_particionador_blob.o

src/pi4/blobs/lua_monitor_sistema_blob.o: lua/ejemplos/monitor_sistema.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 monitor_sistema.lua ../../src/pi4/blobs/lua_monitor_sistema_blob.o

src/pi4/blobs/lua_reloj_blob.o: lua/ejemplos/reloj.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 reloj.lua ../../src/pi4/blobs/lua_reloj_blob.o
src/pi4/blobs/lua_dibujo_blob.o: lua/ejemplos/dibujo.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 dibujo.lua ../../src/pi4/blobs/lua_dibujo_blob.o
src/pi4/blobs/lua_prueba_memoria_blob.o: lua/ejemplos/prueba_memoria.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 prueba_memoria.lua ../../src/pi4/blobs/lua_prueba_memoria_blob.o
src/pi4/blobs/lua_prueba_lectura_blob.o: lua/ejemplos/prueba_lectura.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 prueba_lectura.lua ../../src/pi4/blobs/lua_prueba_lectura_blob.o

src/pi4/blobs/lua_notas_blob.o: lua/ejemplos/notas.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 notas.lua ../../src/pi4/blobs/lua_notas_blob.o

src/pi4/blobs/lua_visor_imagenes_blob.o: lua/ejemplos/visor_imagenes.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 visor_imagenes.lua ../../src/pi4/blobs/lua_visor_imagenes_blob.o

src/pi4/blobs/lua_nemo_html_blob.o: lua/ejemplos/nemo_html.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 nemo_html.lua ../../src/pi4/blobs/lua_nemo_html_blob.o

src/pi4/blobs/lua_nemo_md_blob.o: lua/ejemplos/nemo_md.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 nemo_md.lua ../../src/pi4/blobs/lua_nemo_md_blob.o
src/pi4/blobs/lua_nemo_gpio_blob.o: lua/ejemplos/nemo_gpio.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 nemo_gpio.lua ../../src/pi4/blobs/lua_nemo_gpio_blob.o
src/pi4/blobs/lua_nemo_prueba_blob.o: lua/ejemplos/nemo_prueba.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 nemo_prueba.lua ../../src/pi4/blobs/lua_nemo_prueba_blob.o
src/pi4/blobs/lua_prueba_gpio_blob.o: lua/ejemplos/prueba_gpio.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 prueba_gpio.lua ../../src/pi4/blobs/lua_prueba_gpio_blob.o
src/pi4/blobs/lua_prueba_pwm_blob.o: lua/ejemplos/prueba_pwm.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 prueba_pwm.lua ../../src/pi4/blobs/lua_prueba_pwm_blob.o

src/pi4/blobs/lua_prueba_sonido_blob.o: lua/ejemplos/prueba_sonido.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 prueba_sonido.lua ../../src/pi4/blobs/lua_prueba_sonido_blob.o
src/pi4/blobs/lua_prueba_i2c_blob.o: lua/ejemplos/prueba_i2c.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 prueba_i2c.lua ../../src/pi4/blobs/lua_prueba_i2c_blob.o
src/pi4/blobs/lua_prueba_spi_blob.o: lua/ejemplos/prueba_spi.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 prueba_spi.lua ../../src/pi4/blobs/lua_prueba_spi_blob.o
src/pi4/blobs/lua_aronnax_blob.o: lua/ejemplos/aronnax.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 aronnax.lua ../../src/pi4/blobs/lua_aronnax_blob.o
src/pi4/blobs/lua_aronnax_proyecto_blob.o: lua/ejemplos/aronnax_proyecto.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 aronnax_proyecto.lua ../../src/pi4/blobs/lua_aronnax_proyecto_blob.o
src/pi4/blobs/lua_gpio_blob.o: lua/ejemplos/gpio.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 gpio.lua ../../src/pi4/blobs/lua_gpio_blob.o

# Guias: el simbolo sale del nombre del archivo (GUIA_RED_NEMO_OS.md -> _binary_GUIA_RED_NEMO_OS_md_start)
src/pi4/blobs/fondo_blob.o: docs/nautilus_1280.nimg | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 nautilus_1280.nimg ../src/pi4/blobs/fondo_blob.o

src/pi4/blobs/menu_cfg_blob.o: docs/MENU.CFG | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 MENU.CFG ../src/pi4/blobs/menu_cfg_blob.o
src/pi4/blobs/guia_nimg_blob.o: docs/GUIA_IMAGENES_NIMG.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_IMAGENES_NIMG.md ../src/pi4/blobs/guia_nimg_blob.o
src/pi4/blobs/guia_ambito_blob.o: docs/GUIA_AMBITO_NEMO_BASIC.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_AMBITO_NEMO_BASIC.md ../src/pi4/blobs/guia_ambito_blob.o

# El indice de MANUALES: veinte manuales sin indice son un muro.
src/pi4/blobs/indice_manuales_blob.o: docs/INDICE_MANUALES.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 INDICE_MANUALES.md ../src/pi4/blobs/indice_manuales_blob.o

# La GUIA de Nemo Basic, tambien en cinco partes. Hasta hoy NO
# viajaba dentro del sistema: son 51 KB de una pieza y el visor no los abre
# con el monton de 4 MB de su tarea, asi que la guia del lenguaje propio era
# justo la que no se podia leer desde dentro.
src/pi4/blobs/guia_nb_blob.o: docs/GUIA_NEMO_BASIC.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_NEMO_BASIC.md ../src/pi4/blobs/guia_nb_blob.o
src/pi4/blobs/guia_nb2_blob.o: docs/GUIA_NEMO_BASIC_2.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_NEMO_BASIC_2.md ../src/pi4/blobs/guia_nb2_blob.o
src/pi4/blobs/guia_nb3_blob.o: docs/GUIA_NEMO_BASIC_3.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_NEMO_BASIC_3.md ../src/pi4/blobs/guia_nb3_blob.o
src/pi4/blobs/guia_nb4_blob.o: docs/GUIA_NEMO_BASIC_4.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_NEMO_BASIC_4.md ../src/pi4/blobs/guia_nb4_blob.o
src/pi4/blobs/guia_nb5_blob.o: docs/GUIA_NEMO_BASIC_5.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_NEMO_BASIC_5.md ../src/pi4/blobs/guia_nb5_blob.o

# Referencia de Nemo Basic, en cinco partes enlazadas.
# Va partida porque el visor carga el documento entero en el monton de 4 MB
# de su tarea: de una pieza no cabe, igual que la guia de Lua.
src/pi4/blobs/ref_nb_blob.o: docs/REFERENCIA_NEMO_BASIC.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 REFERENCIA_NEMO_BASIC.md ../src/pi4/blobs/ref_nb_blob.o
src/pi4/blobs/ref_nb2_blob.o: docs/REFERENCIA_NEMO_BASIC_2.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 REFERENCIA_NEMO_BASIC_2.md ../src/pi4/blobs/ref_nb2_blob.o
src/pi4/blobs/ref_nb3_blob.o: docs/REFERENCIA_NEMO_BASIC_3.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 REFERENCIA_NEMO_BASIC_3.md ../src/pi4/blobs/ref_nb3_blob.o
src/pi4/blobs/ref_nb4_blob.o: docs/REFERENCIA_NEMO_BASIC_4.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 REFERENCIA_NEMO_BASIC_4.md ../src/pi4/blobs/ref_nb4_blob.o
src/pi4/blobs/ref_nb5_blob.o: docs/REFERENCIA_NEMO_BASIC_5.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 REFERENCIA_NEMO_BASIC_5.md ../src/pi4/blobs/ref_nb5_blob.o
src/pi4/blobs/guia_iconos_blob.o: docs/GUIA_ICONOS_NEMO_OS.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_ICONOS_NEMO_OS.md ../src/pi4/blobs/guia_iconos_blob.o
src/pi4/blobs/guia_aronnax_blob.o: docs/GUIA_ARONNAX_NEMO_OS.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_ARONNAX_NEMO_OS.md ../src/pi4/blobs/guia_aronnax_blob.o
src/pi4/blobs/ejemplo_aronnax_blob.o: docs/panel_nautilus.anx | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 panel_nautilus.anx ../src/pi4/blobs/ejemplo_aronnax_blob.o

src/pi4/blobs/ejemplo_editor_blob.o: docs/editor_texto.anx | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 editor_texto.anx ../src/pi4/blobs/ejemplo_editor_blob.o
src/pi4/blobs/guia_red_blob.o: docs/GUIA_RED_NEMO_OS.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_RED_NEMO_OS.md ../src/pi4/blobs/guia_red_blob.o

src/pi4/blobs/guia_html_blob.o: docs/GUIA_HTML_NEMO_OS.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_HTML_NEMO_OS.md ../src/pi4/blobs/guia_html_blob.o

src/pi4/blobs/guia_gpio_blob.o: docs/GUIA_GPIO_NEMO_OS.md | $(BLOBS_DIR)
	cd docs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_GPIO_NEMO_OS.md ../src/pi4/blobs/guia_gpio_blob.o

src/pi4/blobs/guia_lua_blob.o: lua/GUIA_PROGRAMACION_LUA_NEMO_OS.md | $(BLOBS_DIR)
	cd lua && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_PROGRAMACION_LUA_NEMO_OS.md ../src/pi4/blobs/guia_lua_blob.o
src/pi4/blobs/guia_lua_sys_blob.o: lua/GUIA_LUA_SYSCALLS.md | $(BLOBS_DIR)
	cd lua && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_LUA_SYSCALLS.md ../src/pi4/blobs/guia_lua_sys_blob.o
src/pi4/blobs/guia_lua_sys2_blob.o: lua/GUIA_LUA_SYSCALLS_2.md | $(BLOBS_DIR)
	cd lua && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 GUIA_LUA_SYSCALLS_2.md ../src/pi4/blobs/guia_lua_sys2_blob.o

src/pi4/blobs/lua_visor_blob.o: lua/ejemplos/visor.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 visor.lua ../../src/pi4/blobs/lua_visor_blob.o

src/pi4/blobs/lua_ayuda_blob.o: lua/ejemplos/ayuda.lua | $(BLOBS_DIR)
	cd lua/ejemplos && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ayuda.lua ../../src/pi4/blobs/lua_ayuda_blob.o

# objcopy nombra los simbolos por el archivo: bienvenida.html -> _binary_bienvenida_html_start
src/pi4/blobs/html_bienvenida_blob.o: lua/html/bienvenida.html | $(BLOBS_DIR)
	cd lua/html && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 bienvenida.html ../../src/pi4/blobs/html_bienvenida_blob.o

src/pi4/blobs/html_ayuda_html_blob.o: lua/html/ayuda_html.html | $(BLOBS_DIR)
	cd lua/html && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ayuda_html.html ../../src/pi4/blobs/html_ayuda_html_blob.o

src/pi4/blobs/html_demo_lua_blob.o: lua/html/demo_lua.html | $(BLOBS_DIR)
	cd lua/html && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 demo_lua.html ../../src/pi4/blobs/html_demo_lua_blob.o

# La ayuda de usuario (lua/ayuda/) -- una regla por pagina, como todo lo
# demas aqui: nada de reglas de patron (ver la cabecera del Makefile).
src/pi4/blobs/ayuda_indice_blob.o: lua/ayuda/ayuda_indice.html | $(BLOBS_DIR)
	cd lua/ayuda && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ayuda_indice.html ../../src/pi4/blobs/ayuda_indice_blob.o

src/pi4/blobs/ayuda_primeros_blob.o: lua/ayuda/ayuda_primeros.html | $(BLOBS_DIR)
	cd lua/ayuda && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ayuda_primeros.html ../../src/pi4/blobs/ayuda_primeros_blob.o

src/pi4/blobs/ayuda_archivos_blob.o: lua/ayuda/ayuda_archivos.html | $(BLOBS_DIR)
	cd lua/ayuda && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ayuda_archivos.html ../../src/pi4/blobs/ayuda_archivos_blob.o

src/pi4/blobs/ayuda_programas_blob.o: lua/ayuda/ayuda_programas.html | $(BLOBS_DIR)
	cd lua/ayuda && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ayuda_programas.html ../../src/pi4/blobs/ayuda_programas_blob.o

src/pi4/blobs/ayuda_ajustes_blob.o: lua/ayuda/ayuda_ajustes.html | $(BLOBS_DIR)
	cd lua/ayuda && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ayuda_ajustes.html ../../src/pi4/blobs/ayuda_ajustes_blob.o

src/pi4/blobs/ayuda_teclas_blob.o: lua/ayuda/ayuda_teclas.html | $(BLOBS_DIR)
	cd lua/ayuda && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ayuda_teclas.html ../../src/pi4/blobs/ayuda_teclas_blob.o

src/pi4/blobs/ayuda_problemas_blob.o: lua/ayuda/ayuda_problemas.html | $(BLOBS_DIR)
	cd lua/ayuda && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ayuda_problemas.html ../../src/pi4/blobs/ayuda_problemas_blob.o

src/pi4/blobs/ayuda_programar_blob.o: lua/ayuda/ayuda_programar.html | $(BLOBS_DIR)
	cd lua/ayuda && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ayuda_programar.html ../../src/pi4/blobs/ayuda_programar_blob.o


kernel8.elf: otros-paquete $(OBJS) src/pi4/pi4.ld
	$(LD) -T src/pi4/pi4.ld $(OBJS) -o kernel8.elf

kernel8.img: kernel8.elf
	$(OBJCOPY) -O binary kernel8.elf kernel8.img
	@echo ""
	@echo "Listo: kernel8.img generado."

clean:
	# BUG REAL CORREGIDO: esto solo borraba $(OBJS) (los .o finales,
	# incluidos los blobs) pero NUNCA los intermedios de cada programa
	# (programs/*.o .elf .bin) ni de nbc-selfhost (su .o .elf .bin
	# propios). "make" decide si recompilar un .c comparando fechas de
	# modificacion contra su .o -- si al copiar un archivo nuevo (p.ej.
	# nbc_main.c) su fecha no queda claramente mas reciente que el .o
	# viejo que ya habia, "make clean && make" se saltaba esa
	# recompilacion EN SILENCIO: el blob final se regeneraba, pero a
	# partir del binario antiguo. Sintoma real sufrido: cambios en
	# nbc_main.c que nunca llegaban a ejecutarse pese a "make clean &&
	# make" repetido.
	rm -f $(OBJS) kernel8.elf kernel8.img
	rm -f programs/*.o programs/*.elf programs/*.bin
	rm -f nbc-selfhost/*.o nbc-selfhost/*.elf nbc-selfhost/*.bin
	rm -f src/pi4/pro_sizes_pi4.h   # generada a partir de los ELF (ver PRO_SIZES_PI4)
	rm -f src/pi4/blobs/fondo_blob.o src/pi4/blobs/menu_cfg_blob.o src/pi4/blobs/lua_*_blob.o src/pi4/blobs/html_*_blob.o src/pi4/blobs/ayuda_*_blob.o src/pi4/blobs/guia_*_blob.o src/pi4/blobs/ejemplo_*_blob.o src/pi4/blobs/fuentes_blob.o src/pi4/blobs/lua_blob.o src/embedded_lua.o
	-$(MAKE) -C lua clean
	# Y lo de QEMU, para que "make clean" deje las dos plataformas limpias.
	rm -rf $(QEMU_BUILD_DIR)

.PHONY: all clean distclean run qemu preparar_carpetas FORCE fuentes imagen imagen-xz

# ============================================================
# QEMU (emulado) -- "make run" compila y arranca esta version.
# No interfiere con la Pi 4 de arriba: usa sus PROPIAS variables
# (QEMU_*) y su propia carpeta de objetos (build/), asi que "make"
# a secas sigue haciendo exactamente lo de siempre (kernel para la
# Pi 4 real). Unica excepcion: los blobs de Lua ($(LUA_BLOBS)) se
# comparten, porque son datos y no dependen de la placa.
# ============================================================

# Makefile del kernel
#
# Requiere el toolchain cruzado aarch64-elf, instalable en Mac con:
#   brew install aarch64-elf-gcc aarch64-elf-binutils qemu
#
# Si tu toolchain se llama distinto (por ejemplo aarch64-none-elf-*),
# cambia el prefijo CROSS abajo.

# RAM de la maquina de QEMU, en MB. La MISMA variable va a QEMU (-m) y al
# kernel (-DQEMU_RAM_MB, ver src/memoria.c), asi que no pueden discrepar.
# Hasta 4096 (las tablas de src/mmu.c cubren 4 GB). Si se cambia, make clean.
QEMU_MEM_MB ?= 4096
QEMU_CFLAGS  = -DQEMU_RAM_MB=$(QEMU_MEM_MB) -Wall -Wextra -ffreestanding -nostdlib -nostartfiles -mcpu=cortex-a53 -mgeneral-regs-only -O2 -fno-jump-tables -fno-tree-switch-conversion -DNEMO_QEMU
QEMU_ASFLAGS = -mcpu=cortex-a53

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
QEMU_NBC_CFLAGS = -Wall -Wextra -ffreestanding -nostdlib -nostartfiles -mcpu=cortex-a53 -O2 -fno-jump-tables -fno-tree-switch-conversion

QEMU_SRC_DIR = src
QEMU_BUILD_DIR = build

QEMU_OBJS = $(QEMU_BUILD_DIR)/boot.o \
       $(QEMU_BUILD_DIR)/uart.o \
       $(QEMU_BUILD_DIR)/bitacora.o \
       $(QEMU_BUILD_DIR)/exceptions.o \
       $(QEMU_BUILD_DIR)/exceptions_c.o \
       $(QEMU_BUILD_DIR)/gic.o \
       $(QEMU_BUILD_DIR)/timer.o \
       $(QEMU_BUILD_DIR)/mmu.o \
       $(QEMU_BUILD_DIR)/heap.o \
       $(QEMU_BUILD_DIR)/disk.o \
       $(QEMU_BUILD_DIR)/sound.o \
       $(QEMU_BUILD_DIR)/rtc.o \
       $(QEMU_BUILD_DIR)/nemofs.o \
       $(QEMU_BUILD_DIR)/fat.o \
       $(QEMU_BUILD_DIR)/loader.o \
       $(QEMU_BUILD_DIR)/hello_blob.o \
       $(QEMU_BUILD_DIR)/syscall.o \
       $(QEMU_BUILD_DIR)/syscall_test_blob.o \
       $(QEMU_BUILD_DIR)/shell_blob.o \
       $(QEMU_BUILD_DIR)/explorer_blob.o \
       $(QEMU_BUILD_DIR)/editor_blob.o \
       $(QEMU_BUILD_DIR)/ide_blob.o \
       $(QEMU_BUILD_DIR)/gadgetdemo_blob.o \
       $(QEMU_BUILD_DIR)/desktoped_blob.o \
       $(QEMU_BUILD_DIR)/screensettings_blob.o \
       $(QEMU_BUILD_DIR)/nbc_blob.o \
       $(QEMU_BUILD_DIR)/fwcfg.o \
       $(QEMU_BUILD_DIR)/ramfb.o \
       $(QEMU_BUILD_DIR)/font5x7.o \
       $(QEMU_BUILD_DIR)/text.o \
       $(QEMU_BUILD_DIR)/input.o \
       $(QEMU_BUILD_DIR)/wm.o \
       $(QEMU_BUILD_DIR)/power.o \
       $(QEMU_BUILD_DIR)/icons_data.o \
       $(QEMU_BUILD_DIR)/dialog.o \
       $(QEMU_BUILD_DIR)/gadgets.o \
       $(QEMU_BUILD_DIR)/tasks_switch.o \
       $(QEMU_BUILD_DIR)/tasks.o \
       $(QEMU_BUILD_DIR)/embedded_lua.o \
       $(QEMU_BUILD_DIR)/otros_programas.o \
       $(QEMU_BUILD_DIR)/iconos_nimg.o \
       $(LUA_BLOBS) \
       $(QEMU_BUILD_DIR)/virtio_net.o \
       $(QEMU_BUILD_DIR)/net.o \
       $(QEMU_BUILD_DIR)/tcp.o \
       $(QEMU_BUILD_DIR)/udp.o \
       $(QEMU_BUILD_DIR)/udp_sock.o \
       $(QEMU_BUILD_DIR)/ip_texto.o \
       $(QEMU_BUILD_DIR)/dhcp.o \
       $(QEMU_BUILD_DIR)/arp.o \
       $(QEMU_BUILD_DIR)/tcp_comun.o \
       $(QEMU_BUILD_DIR)/tcp_cliente.o \
       $(QEMU_BUILD_DIR)/http.o \
       $(QEMU_BUILD_DIR)/descarga.o \
       $(QEMU_BUILD_DIR)/nmz.o \
       $(QEMU_BUILD_DIR)/nmz_fs.o \
       $(QEMU_BUILD_DIR)/netshell.o \
       $(QEMU_BUILD_DIR)/smp.o \
       $(QEMU_BUILD_DIR)/memoria.o \
       $(QEMU_BUILD_DIR)/gpio.o \
       $(QEMU_BUILD_DIR)/i2c.o \
       $(QEMU_BUILD_DIR)/spi.o \
       $(QEMU_BUILD_DIR)/bkl.o \
       $(QEMU_BUILD_DIR)/medir.o \
       $(QEMU_BUILD_DIR)/fonts.o \
       $(QEMU_BUILD_DIR)/kernel.o

# -- nbc.pro: el compilador de Nemo Basic AUTOHOSPEDADO, corriendo
# dentro de Nemo OS. A diferencia de shell/editor/etc (un solo .c),
# son diez archivos que hay que compilar y enlazar juntos -- mismo
# empaquetado (elf -> bin -> blob.o embebido en el kernel), con una
# regla de compilacion generica en vez de una por archivo.
#
# Ver la nota del monton ampliado en la seccion equivalente de Pi 4:
# el -D de NB_ALLOC_POOL_SIZE no es opcional.
NBC_DIR = nbc-selfhost
QEMU_NBC_SRCS = nbc_main.c nb_lexer.c nb_ast.c nb_parser.c nb_codegen.c nb_symtab.c nb_codebuf.c nb_encode.c nb_string.c nb_alloc.c nb_include.c
QEMU_NBC_OBJS = $(patsubst %.c,$(QEMU_BUILD_DIR)/nbc_%.o,$(QEMU_NBC_SRCS))

# Igual que en el build de Pi 4: si el runtime incrustado cambia, el
# compilador se recompila entero.
$(QEMU_NBC_OBJS): $(NBRT_DIR)/nb_runtime_blob.h

$(QEMU_BUILD_DIR)/nbc_%.o: $(NBC_DIR)/%.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_NBC_CFLAGS) -DNB_ALLOC_POOL_SIZE='(8u*1024u*1024u)' -I$(NBC_DIR) -c -o $@ $<

$(QEMU_BUILD_DIR)/nbc.elf: $(QEMU_NBC_OBJS) $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(QEMU_NBC_OBJS)

$(QEMU_BUILD_DIR)/nbc.bin: $(QEMU_BUILD_DIR)/nbc.elf
	$(OBJCOPY) -O binary $< $@

$(QEMU_BUILD_DIR)/nbc_blob.o: $(QEMU_BUILD_DIR)/nbc.bin
	cd $(QEMU_BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 nbc.bin nbc_blob.o

DISK_IMG = disk.img
DISK_SIZE = 64M
FAT_IMG = fat.img
# 512 MB, como la particion de arranque de la Pi (fase 5b: archivos
# grandes por partes). La imagen es dispersa: en el Mac solo ocupa lo que
# se escriba. Solo se aplica al CREAR fat.img: tras cambiarlo, borrar
# fat.img (NO disk.img, que es NemoFS con los archivos de siempre).
FAT_SIZE = 512M

qemu: $(QEMU_BUILD_DIR)/kernel.elf

$(QEMU_BUILD_DIR):
	mkdir -p $(QEMU_BUILD_DIR)

$(DISK_IMG):
	qemu-img create -f raw $(DISK_IMG) $(DISK_SIZE)

$(FAT_IMG):
	qemu-img create -f raw $(FAT_IMG) $(FAT_SIZE)

$(QEMU_BUILD_DIR)/boot.o: $(QEMU_SRC_DIR)/boot.s | $(QEMU_BUILD_DIR)
	$(AS) $(QEMU_ASFLAGS) -o $@ $<

$(QEMU_BUILD_DIR)/uart.o: $(QEMU_SRC_DIR)/uart.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/bitacora.o: $(QEMU_SRC_DIR)/bitacora.c $(QEMU_SRC_DIR)/bitacora.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/exceptions.o: $(QEMU_SRC_DIR)/exceptions.s | $(QEMU_BUILD_DIR)
	$(AS) $(QEMU_ASFLAGS) -o $@ $<

$(QEMU_BUILD_DIR)/exceptions_c.o: $(QEMU_SRC_DIR)/exceptions.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/gic.o: $(QEMU_SRC_DIR)/gic.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/timer.o: $(QEMU_SRC_DIR)/timer.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/mmu.o: $(QEMU_SRC_DIR)/mmu.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/heap.o: $(QEMU_SRC_DIR)/heap.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/disk.o: $(QEMU_SRC_DIR)/disk.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/sound.o: $(QEMU_SRC_DIR)/sound.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/rtc.o: $(QEMU_SRC_DIR)/rtc.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/nemofs.o: $(QEMU_SRC_DIR)/nemofs.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/fat.o: $(QEMU_SRC_DIR)/fat.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

# Igual que en la Pi 4: tamaños reales de cada programa, de sus ELF.
# Van a build/ porque los binarios de QEMU no son identicos a los de la
# Pi (distinto -mcpu), y loader.c incluye el suyo segun NEMO_QEMU.
PRO_SIZES_QEMU = $(QEMU_BUILD_DIR)/pro_sizes_qemu.h
$(PRO_SIZES_QEMU): $(QEMU_BUILD_DIR)/shell.elf $(QEMU_BUILD_DIR)/explorer.elf $(QEMU_BUILD_DIR)/desktoped.elf $(QEMU_BUILD_DIR)/screensettings.elf $(QEMU_BUILD_DIR)/editor.elf $(QEMU_BUILD_DIR)/gadgetdemo.elf $(QEMU_BUILD_DIR)/ide.elf $(QEMU_BUILD_DIR)/syscall_test.elf $(QEMU_BUILD_DIR)/nbc.elf lua/build/nemo/lua.bin herramientas/pro_sizes.py | $(QEMU_BUILD_DIR)
	python3 herramientas/pro_sizes.py -o $@ shell=$(QEMU_BUILD_DIR)/shell.elf explorer=$(QEMU_BUILD_DIR)/explorer.elf desktoped=$(QEMU_BUILD_DIR)/desktoped.elf screensettings=$(QEMU_BUILD_DIR)/screensettings.elf editor=$(QEMU_BUILD_DIR)/editor.elf gadgetdemo=$(QEMU_BUILD_DIR)/gadgetdemo.elf ide=$(QEMU_BUILD_DIR)/ide.elf syscall_test=$(QEMU_BUILD_DIR)/syscall_test.elf nbc=$(QEMU_BUILD_DIR)/nbc.elf lua=lua/build/nemo/lua.elf

$(QEMU_BUILD_DIR)/loader.o: $(QEMU_SRC_DIR)/loader.c $(PRO_SIZES_QEMU) | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -I$(QEMU_BUILD_DIR) -c -o $@ $<

# -- Programa de prueba para el loader: se ensambla y enlaza aparte,
# se extrae como binario plano, y se empaqueta como un objeto ELF con
# simbolos (_binary_hello_bin_start/_end) para poder enlazarlo
# directamente dentro del kernel.
$(QEMU_BUILD_DIR)/hello.o: $(PROGRAMS_DIR)/hello.s | $(QEMU_BUILD_DIR)
	$(AS) $(QEMU_ASFLAGS) -o $@ $<

$(QEMU_BUILD_DIR)/hello.elf: $(QEMU_BUILD_DIR)/hello.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(QEMU_BUILD_DIR)/hello.o

$(QEMU_BUILD_DIR)/hello.bin: $(QEMU_BUILD_DIR)/hello.elf
	$(OBJCOPY) -O binary $< $@

$(QEMU_BUILD_DIR)/hello_blob.o: $(QEMU_BUILD_DIR)/hello.bin
	cd $(QEMU_BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 hello.bin hello_blob.o

$(QEMU_BUILD_DIR)/syscall.o: $(QEMU_SRC_DIR)/syscall.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

# -- Programa de prueba de syscalls: mismo proceso que hello.s --
$(QEMU_BUILD_DIR)/syscall_test.o: $(PROGRAMS_DIR)/syscall_test.s | $(QEMU_BUILD_DIR)
	$(AS) $(QEMU_ASFLAGS) -o $@ $<

$(QEMU_BUILD_DIR)/syscall_test.elf: $(QEMU_BUILD_DIR)/syscall_test.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(QEMU_BUILD_DIR)/syscall_test.o

$(QEMU_BUILD_DIR)/syscall_test.bin: $(QEMU_BUILD_DIR)/syscall_test.elf
	$(OBJCOPY) -O binary $< $@

$(QEMU_BUILD_DIR)/syscall_test_blob.o: $(QEMU_BUILD_DIR)/syscall_test.bin
	cd $(QEMU_BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 syscall_test.bin syscall_test_blob.o

# -- Shell: escrita en C (freestanding, mismas flags que el kernel),
# usando solo syscalls. Mismo proceso de empaquetado que los programas
# en ensamblador, salvo que se compila con gcc en vez de ensamblarse.
$(QEMU_BUILD_DIR)/shell.o: $(PROGRAMS_DIR)/shell.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/shell.elf: $(QEMU_BUILD_DIR)/shell.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(QEMU_BUILD_DIR)/shell.o

$(QEMU_BUILD_DIR)/shell.bin: $(QEMU_BUILD_DIR)/shell.elf
	$(OBJCOPY) -O binary $< $@

$(QEMU_BUILD_DIR)/shell_blob.o: $(QEMU_BUILD_DIR)/shell.bin
	cd $(QEMU_BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 shell.bin shell_blob.o

# -- Explorador de archivos: mismo proceso que la shell --
$(QEMU_BUILD_DIR)/explorer.o: $(PROGRAMS_DIR)/explorer.c $(PROGRAMS_DIR)/barra.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/explorer.elf: $(QEMU_BUILD_DIR)/explorer.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(QEMU_BUILD_DIR)/explorer.o

$(QEMU_BUILD_DIR)/explorer.bin: $(QEMU_BUILD_DIR)/explorer.elf
	$(OBJCOPY) -O binary $< $@

$(QEMU_BUILD_DIR)/explorer_blob.o: $(QEMU_BUILD_DIR)/explorer.bin
	cd $(QEMU_BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 explorer.bin explorer_blob.o

# -- Editor de texto: mismo proceso que la shell y el explorador --
$(QEMU_BUILD_DIR)/editor.o: $(PROGRAMS_DIR)/editor.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/editor.elf: $(QEMU_BUILD_DIR)/editor.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(QEMU_BUILD_DIR)/editor.o

$(QEMU_BUILD_DIR)/editor.bin: $(QEMU_BUILD_DIR)/editor.elf
	$(OBJCOPY) -O binary $< $@

$(QEMU_BUILD_DIR)/editor_blob.o: $(QEMU_BUILD_DIR)/editor.bin
	cd $(QEMU_BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 editor.bin editor_blob.o

# -- IDE (varias pestañas + compilar/ejecutar): mismo proceso --
$(QEMU_BUILD_DIR)/ide.o: $(PROGRAMS_DIR)/ide.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/ide.elf: $(QEMU_BUILD_DIR)/ide.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(QEMU_BUILD_DIR)/ide.o

$(QEMU_BUILD_DIR)/ide.bin: $(QEMU_BUILD_DIR)/ide.elf
	$(OBJCOPY) -O binary $< $@

$(QEMU_BUILD_DIR)/ide_blob.o: $(QEMU_BUILD_DIR)/ide.bin
	cd $(QEMU_BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 ide.bin ide_blob.o

# -- Demo del sistema de gadgets: mismo proceso que los demas --
$(QEMU_BUILD_DIR)/gadgetdemo.o: $(PROGRAMS_DIR)/gadgetdemo.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/gadgetdemo.elf: $(QEMU_BUILD_DIR)/gadgetdemo.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(QEMU_BUILD_DIR)/gadgetdemo.o

$(QEMU_BUILD_DIR)/gadgetdemo.bin: $(QEMU_BUILD_DIR)/gadgetdemo.elf
	$(OBJCOPY) -O binary $< $@

$(QEMU_BUILD_DIR)/gadgetdemo_blob.o: $(QEMU_BUILD_DIR)/gadgetdemo.bin
	cd $(QEMU_BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 gadgetdemo.bin gadgetdemo_blob.o

# -- Editor de escritorio: mismo proceso que la shell y el explorador --
$(QEMU_BUILD_DIR)/desktoped.o: $(PROGRAMS_DIR)/desktoped.c $(PROGRAMS_DIR)/barra.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/desktoped.elf: $(QEMU_BUILD_DIR)/desktoped.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(QEMU_BUILD_DIR)/desktoped.o

$(QEMU_BUILD_DIR)/desktoped.bin: $(QEMU_BUILD_DIR)/desktoped.elf
	$(OBJCOPY) -O binary $< $@

$(QEMU_BUILD_DIR)/desktoped_blob.o: $(QEMU_BUILD_DIR)/desktoped.bin
	cd $(QEMU_BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 desktoped.bin desktoped_blob.o

# -- Configurar pantalla: mismo proceso --
$(QEMU_BUILD_DIR)/screensettings.o: $(PROGRAMS_DIR)/screensettings.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/screensettings.elf: $(QEMU_BUILD_DIR)/screensettings.o $(PROGRAMS_DIR)/hello_linker.ld
	$(LD) -T $(PROGRAMS_DIR)/hello_linker.ld -o $@ $(QEMU_BUILD_DIR)/screensettings.o

$(QEMU_BUILD_DIR)/screensettings.bin: $(QEMU_BUILD_DIR)/screensettings.elf
	$(OBJCOPY) -O binary $< $@

$(QEMU_BUILD_DIR)/screensettings_blob.o: $(QEMU_BUILD_DIR)/screensettings.bin
	cd $(QEMU_BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 screensettings.bin screensettings_blob.o

$(QEMU_BUILD_DIR)/fwcfg.o: $(QEMU_SRC_DIR)/fwcfg.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/ramfb.o: $(QEMU_SRC_DIR)/ramfb.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/font5x7.o: $(QEMU_SRC_DIR)/font5x7.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/text.o: $(QEMU_SRC_DIR)/text.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/input.o: $(QEMU_SRC_DIR)/input.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/wm.o: $(QEMU_SRC_DIR)/wm.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/power.o: $(QEMU_SRC_DIR)/power.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/icons_data.o: $(QEMU_SRC_DIR)/icons_data.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/dialog.o: $(QEMU_SRC_DIR)/dialog.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/gadgets.o: $(QEMU_SRC_DIR)/gadgets.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/tasks_switch.o: $(QEMU_SRC_DIR)/tasks_switch.s | $(QEMU_BUILD_DIR)
	$(AS) $(QEMU_ASFLAGS) -o $@ $<

$(QEMU_BUILD_DIR)/tasks.o: $(QEMU_SRC_DIR)/tasks.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/kernel.o: $(QEMU_SRC_DIR)/kernel.c | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

# -- Lua embebido: mismo embedded_lua.c y los MISMOS blobs que la Pi 4
# ($(LUA_BLOBS), en src/pi4/blobs/): son datos aarch64, valen para las
# dos placas. lua.bin va compilado -mcpu=cortex-a72; en QEMU (a53)
# corre igual, es la misma ISA -- solo cambia el ajuste.
$(QEMU_BUILD_DIR)/embedded_lua.o: $(QEMU_SRC_DIR)/embedded_lua.c $(QEMU_SRC_DIR)/embedded_lua.h $(PRO_SIZES_QEMU) | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -I$(QEMU_BUILD_DIR) -c -o $@ $<

$(QEMU_BUILD_DIR)/otros_programas.o: $(QEMU_SRC_DIR)/otros_programas.c $(QEMU_SRC_DIR)/otros_programas.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -I$(QEMU_BUILD_DIR) -c -o $@ $<

# -- Red en QEMU: la MISMA pila (net.c, tcp.c, netshell.c) que la Pi 4,
# con una tarjeta virtio-net debajo en vez del GENET (ver nic.h).
$(QEMU_BUILD_DIR)/virtio_net.o: $(QEMU_SRC_DIR)/virtio_net.c $(QEMU_SRC_DIR)/virtio_net.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/net.o: $(QEMU_SRC_DIR)/net.c $(QEMU_SRC_DIR)/net.h $(QEMU_SRC_DIR)/nic.h $(QEMU_SRC_DIR)/tcp.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/tcp.o: $(QEMU_SRC_DIR)/tcp.c $(QEMU_SRC_DIR)/tcp.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/udp.o: $(QEMU_SRC_DIR)/udp.c $(QEMU_SRC_DIR)/udp.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/udp_sock.o: $(QEMU_SRC_DIR)/udp_sock.c $(QEMU_SRC_DIR)/udp_sock.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/ip_texto.o: $(QEMU_SRC_DIR)/ip_texto.c $(QEMU_SRC_DIR)/ip_texto.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/dhcp.o: $(QEMU_SRC_DIR)/dhcp.c $(QEMU_SRC_DIR)/dhcp.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/arp.o: $(QEMU_SRC_DIR)/arp.c $(QEMU_SRC_DIR)/arp.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/tcp_comun.o: $(QEMU_SRC_DIR)/tcp_comun.c $(QEMU_SRC_DIR)/tcp_comun.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/tcp_cliente.o: $(QEMU_SRC_DIR)/tcp_cliente.c $(QEMU_SRC_DIR)/tcp_cliente.h $(QEMU_SRC_DIR)/tcp_comun.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/http.o: $(QEMU_SRC_DIR)/http.c $(QEMU_SRC_DIR)/http.h $(QEMU_SRC_DIR)/tcp_cliente.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/descarga.o: $(QEMU_SRC_DIR)/descarga.c $(QEMU_SRC_DIR)/descarga.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/nmz.o: $(QEMU_SRC_DIR)/nmz.c $(QEMU_SRC_DIR)/nmz.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/nmz_fs.o: $(QEMU_SRC_DIR)/nmz_fs.c $(QEMU_SRC_DIR)/nmz.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/netshell.o: $(QEMU_SRC_DIR)/netshell.c $(QEMU_SRC_DIR)/tcp.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

# Varios nucleos (SMP): despierta los nucleos 1-3 con PSCI. Necesita
# que QEMU se lance con -smp 4 (ver la regla run).
$(QEMU_BUILD_DIR)/smp.o: $(QEMU_SRC_DIR)/smp.c $(QEMU_SRC_DIR)/smp.h $(QEMU_SRC_DIR)/cpu.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

# Depende tambien del Makefile: si cambia QEMU_MEM_MB, se recompila solo.
$(QEMU_BUILD_DIR)/spi.o: $(QEMU_SRC_DIR)/spi.c $(QEMU_SRC_DIR)/spi.h $(QEMU_SRC_DIR)/gpio.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/i2c.o: $(QEMU_SRC_DIR)/i2c.c $(QEMU_SRC_DIR)/i2c.h $(QEMU_SRC_DIR)/gpio.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/iconos_nimg.o: $(QEMU_SRC_DIR)/iconos_nimg.c $(QEMU_SRC_DIR)/iconos_nimg.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<
$(QEMU_BUILD_DIR)/gpio.o: $(QEMU_SRC_DIR)/gpio.c $(QEMU_SRC_DIR)/gpio.h $(QEMU_SRC_DIR)/tasks.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/memoria.o: $(QEMU_SRC_DIR)/memoria.c $(QEMU_SRC_DIR)/memoria.h Makefile | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

# Candado grande del kernel (SMP, fase 4).
$(QEMU_BUILD_DIR)/bkl.o: $(QEMU_SRC_DIR)/bkl.c $(QEMU_SRC_DIR)/bkl.h $(QEMU_SRC_DIR)/spinlock.h $(QEMU_SRC_DIR)/cpu.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

# Medicion de rendimiento del SMP (diagnostico; ver medir.h).
$(QEMU_BUILD_DIR)/medir.o: $(QEMU_SRC_DIR)/medir.c $(QEMU_SRC_DIR)/medir.h $(QEMU_SRC_DIR)/cpu.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<


$(QEMU_BUILD_DIR)/fonts.o: $(QEMU_SRC_DIR)/fonts.c $(QEMU_SRC_DIR)/fonts.h | $(QEMU_BUILD_DIR)
	$(CC) $(QEMU_CFLAGS) -c -o $@ $<

$(QEMU_BUILD_DIR)/kernel.elf: otros-paquete $(QEMU_OBJS) $(QEMU_SRC_DIR)/linker.ld
	$(LD) -T $(QEMU_SRC_DIR)/linker.ld -o $@ $(QEMU_OBJS)

run: $(QEMU_BUILD_DIR)/kernel.elf $(DISK_IMG) $(FAT_IMG)
	qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 4 -m $(QEMU_MEM_MB)M \
		-serial stdio -display cocoa,zoom-to-fit=on \
		-global virtio-mmio.force-legacy=false \
		-kernel $(QEMU_BUILD_DIR)/kernel.elf \
		-drive file=$(DISK_IMG),if=none,format=raw,id=hd0 \
		-device virtio-blk-device,drive=hd0,serial=NEMOSYS \
		-drive file=$(FAT_IMG),if=none,format=raw,id=hd1 \
		-device virtio-blk-device,drive=hd1,serial=NEMOFAT \
		-device ramfb \
		-device virtio-keyboard-device,serial=NEMOKBD \
		-device virtio-tablet-device,serial=NEMOMOUSE \
		-audiodev coreaudio,id=audio0 \
		-device virtio-sound-device,audiodev=audio0 \
		-netdev user,id=net0,hostfwd=tcp::2323-:2323 \
		-device virtio-net-device,netdev=net0

# --- La imagen de tarjeta, lista para el programa de Raspberry Pi ---
#
# Se monta con python3 y nada mas: ni mtools, ni permisos de administrador.
# Reparto: 1 GB de arranque en FAT32 + 2 GB de NemoFS a ceros (el sistema lo
# formatea en el primer arranque, reservando el mapa para la tarjeta ENTERA,
# de modo que luego se pueda estirar desde el Particionador).
#
#   make imagen ARRANQUE=/ruta/a/la/carpeta/con/el/firmware
#
# La carpeta debe llevar el firmware de la Pi (start4.elf, fixup4.dat, el .dtb
# y overlays/), el config.txt y el kernel8.img recien compilado.
IMAGEN = nemo-os-$(VERSION_NEMO).img
VERSION_NEMO = 1.0.1
# La carpeta con el firmware de arranque de la Raspberry Pi (start4.elf,
# fixup4.dat, el .dtb y overlays/). NO viaja en este repositorio: es de la
# Fundacion Raspberry Pi, con su propia licencia, y hay que bajarlo una vez
# (el README explica de donde). El kernel y el config.txt los pone el
# Makefile, recien hechos.
#
# Si la carpeta esta en otro sitio:  make imagen ARRANQUE=/ruta/a/arranque
ARRANQUE ?= arranque

imagen: kernel8.img
	@test -d "$(ARRANQUE)" || (echo "falta la carpeta '$(ARRANQUE)' con el firmware de arranque de la Pi. Mira 'La carpeta de arranque' en el README; si la tienes en otro sitio: make imagen ARRANQUE=/ruta"; exit 1)
	@test -f "$(ARRANQUE)/start4.elf" || (echo "en '$(ARRANQUE)' falta el firmware de la Pi: start4.elf, fixup4.dat, bcm2711-rpi-4-b.dtb y overlays/. Mira 'La carpeta de arranque' en el README"; exit 1)
	# El kernel y el config.txt SIEMPRE los pone el Makefile, recien hechos:
	# asi no hay forma de generar una imagen con una copia vieja de ninguno.
	cp kernel8.img "$(ARRANQUE)/kernel8.img"
	cp config.txt "$(ARRANQUE)/config.txt"
	python3 herramientas/imagen/crear_imagen.py --arranque "$(ARRANQUE)" --salida $(IMAGEN) --version $(VERSION_NEMO)
	python3 herramientas/imagen/verificar_imagen.py $(IMAGEN) "$(ARRANQUE)"
	@echo ""
	@echo "  Listo: $(IMAGEN)"
	@echo "  Comprimela con 'make imagen-xz' y escribela con el programa de Raspberry Pi"
	@echo "  (opcion de imagen personalizada; cuando pregunte por la personalizacion, responde que NO)."

# Y comprimida, que es como se descarga (el programa de Raspberry Pi lee .xz)
imagen-xz: imagen
	rm -f $(IMAGEN).xz
	xz -9 -T0 -k $(IMAGEN)
	@ls -la $(IMAGEN).xz

distclean: clean
	rm -f $(DISK_IMG) $(FAT_IMG)
