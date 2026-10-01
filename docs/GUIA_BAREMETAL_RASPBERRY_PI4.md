# Raspberry Pi 4 bare-metal — Guía de referencia

**Todas las direcciones y valores de este documento están verificados en hardware**
(Raspberry Pi 4 Model B, revisión `d03115` = 8GB rev 1.5, BCM2711, AArch64, modo
*low-peripheral*), durante el port de Nemo OS. Donde algo procede de la
especificación y no se ha ejercitado aún, se indica con *(spec)*.

Convención: `BASE + offset`. Todos los registros son de 32 bits salvo que se diga
lo contrario. Accesos siempre con punteros `volatile`.

---

## 1. Arranque

### 1.1 Qué hace el firmware antes que tú

1. El bootloader de la EEPROM lee `config.txt`, carga `start4.elf` + `fixup4.dat`.
2. `start4.elf` (VideoCore) configura reloj, RAM, HDMI; **inicializa PCIe y lee
   el xHCI del VL805** (verás `xHC0 ver: 256` en el UART) y luego hace
   `PCI0 reset` — te entrega el puente PCIe **en reset**.
3. Carga `bcm2711-rpi-4-b.dtb` y los overlays, y `kernel8.img` en **`0x80000`**.
4. Ejecuta el *armstub* de fábrica en EL3, que:
   - pone `SCR_EL3.NS=1` (mundo No Seguro), `RW=1` (AArch64), `HCE=1`;
   - activa `CPUECTLR_EL1.SMPEN` — **obligatorio antes de MMU/cachés en A72**;
   - aparca los núcleos 1-3 en una *spin table* (`0xD8`, `0xE0`, `0xE8`, `0xF0`);
   - salta a `0x80000` en **EL2**, con `x0` = dirección del DTB.

### 1.2 Lo que debe hacer tu `_start` (verificado)

```
mrs x0, mpidr_el1 ; and x0,x0,#0xFF ; cbnz x0, parar_nucleo  ; solo core 0
mrs x0, CurrentEL ; lsr x0,x0,#2                             ; 2 = EL2
msr hcr_el2, #(1<<31)                                        ; EL1 en AArch64
msr sctlr_el1, xzr                                           ; MMU/caches off
msr spsr_el2, #0x3c5   ; EL1h, DAIF enmascarado
adr x0, en_el1 ; msr elr_el2, x0 ; eret
en_el1:
mrs x0, cpacr_el1 ; orr x0,x0,#(3<<20) ; msr cpacr_el1,x0  ; FP/SIMD
ldr x0, =stack_top ; mov sp, x0
(limpiar .bss)  ->  bl kernel_main
```

### 1.3 `config.txt` mínimo (verificado)

```
arm_64bit=1
enable_uart=1
dtoverlay=disable-bt      # libera el PL011 (UART0) hacia GPIO 14/15
uart_2ndstage=1           # log del firmware por UART, muy útil
hdmi_force_hotplug=1      # solo si usas framebuffer sin monitor detectado
kernel=kernel8.img
```

**No** poner `otg_mode=1` (en la 4B activa otro xHCI en el USB-C, no el VL805).
**No** hace falta `armstub=`; si usas uno propio, debe activar `SMPEN`.

### 1.4 Script del enlazador (verificado)

```
ENTRY(_start)
SECTIONS {
  . = 0x80000;
  .text.boot : { KEEP(*(.text.boot)) }
  .text : { *(.text*) }  .rodata : { *(.rodata*) }  .data : { *(.data*) }
  . = ALIGN(16); __bss_start = .; .bss : { *(.bss*) *(COMMON) } __bss_end = .;
  . = ALIGN(16); . += 0x10000; stack_top = .;
}
```

Toolchain: `aarch64-elf-` o `aarch64-none-elf-` (GCC 15.x probado).
Flags: `-ffreestanding -mcpu=cortex-a72 -Wall -Wextra -O2`.

### 1.5 Código de revisión

`d03115`: bits 0-3 rev PCB (5), 4-11 tipo (`0x11` = 4B), 12-15 CPU (3 = BCM2711),
16-19 fabricante (0 = Sony UK), **20-22 RAM (5 = 8GB)**, bit 23 = formato nuevo.
Tabla RAM: 0=256MB 1=512MB 2=1GB 3=2GB 4=4GB 5=8GB.
Lo imprime el firmware: `board: boardrev d03115`.

---

## 2. Mapa de memoria (modo low-peripheral, el de fábrica)

| Rango (CPU)                   | Qué es                                   |
|-------------------------------|------------------------------------------|
| `0x0_0000_0000` – RAM         | SDRAM (hasta ~3,9 GB visibles bajo 4GB) |
| `0x0_0000_0000` – `0x0_0000_00FF` | armstub + spin table (no pisar)      |
| `0x0_0008_0000`               | carga de `kernel8.img`                   |
| `0x0_FC00_0000` – `0x0_FFFF_FFFF` | periféricos (64 MB)                  |
| `0x0_FD50_0000`               | controlador PCIe (BCM2711)               |
| `0x0_FE00_0000`               | `PERIPHERAL_BASE` (bloque principal)     |
| `0x0_FE10_0000`               | Power Management (watchdog, reset real)  |
| `0x0_FF84_1000` / `0x0_FF84_2000` | GIC-400 Distributor / CPU interface  |
| `0x1_0000_0000` en adelante   | resto de la RAM en placas > 4GB          |
| `0x6_0000_0000` – `0x6_03FF_FFFF` | **ventana de salida PCIe** (64 MB)   |

En modo *high-peripheral* (`arm_peri_high=1`) los periféricos se mueven a
`0x4_7C00_0000`; todo lo de esta guía asume **low**.

### 2.1 Cómo reparte Nemo OS la RAM (septiembre de 2026)

Todo estático, en `.bss` del kernel, sin asignación dinámica de páginas:

| Qué | Tamaño | Dónde | Notas |
|---|---|---|---|
| Kernel (`.text`/`.rodata`/`.data`) | ~3 MB | desde `0x80000` | incluye 2 MB de fuentes pre-rasterizadas (`FUENTES.NFP`) y los programas embebidos |
| Heap del kernel (`kmalloc`/`kfree`) | 64 MB | `.bss` | listas libres segregadas; lo usa NemoFS para el bitmap de bloques, entre otros |
| Pilas de kernel de las tareas | 8 × 128 KB | `.bss` | una por hueco, para el código EL1 que atiende sus syscalls |
| Áreas de programa | 8 × 16 MB, **alineadas a 2 MB** | `.bss` | lo ÚNICO que una tarea ve desde EL0 (§4) |
| Tablas de traducción | 9 × (4 KB + 4 KB) | `.bss` | una pareja L1+L2 por hueco de tarea, más la del kernel |
| Framebuffer | según resolución | lo reserva el VideoCore (mailbox) | dirección de bus → `& 0x3FFFFFFF` |

---

## 3. Periféricos verificados

### 3.1 UART0 — PL011, `0xFE201000`

| Offset | Registro | Notas                                              |
|--------|----------|----------------------------------------------------|
| `0x00` | DR       | dato                                               |
| `0x18` | FR       | bit 5 TXFF (FIFO TX llena), bit 4 RXFE (RX vacía)  |
| `0x24` | IBRD     | 26 → 115200 a 48 MHz                               |
| `0x28` | FBRD     | 3                                                  |
| `0x2C` | LCRH     | `3<<5` = 8 bits                                    |
| `0x30` | CR       | bit0 UARTEN, bit8 TXE, bit9 RXE                    |
| `0x38` | IMSC     | máscara de interrupciones                          |
| `0x44` | ICR      | limpiar interrupciones                             |

Requiere `dtoverlay=disable-bt` en `config.txt`. El firmware ya lo deja
configurado si `enable_uart=1`; reprogramar IBRD/FBRD es opcional.

### 3.2 Mailbox (ARM ↔ VideoCore), `0xFE00B880`

| Offset | Registro | Notas                                      |
|--------|----------|--------------------------------------------|
| `0x00` | READ     |                                            |
| `0x18` | STATUS   | bit31 FULL, bit30 EMPTY                    |
| `0x20` | WRITE    | dirección del buffer (16-aligned) \| canal |

Canal 8 = interfaz de propiedades. Protocolo verificado:

```
buf[0] = tamaño total en bytes
buf[1] = 0 (petición)            -> respuesta: 0x80000000 OK / 0x80000001 error
buf[2] = tag
buf[3] = tamaño del buffer de valor
buf[4] = tamaño de la petición   -> respuesta: bit 31 puesto si el tag se atendió
buf[5..] = valores
buf[n] = 0 (fin)
```

Antes de escribir en WRITE: `dc cvac` sobre el buffer + `dsb sy`. Tras leer la
respuesta: `dc ivac` + `dsb sy`. La dirección funciona tanto física como con el
alias de bus `0xC0000000|física` (probadas ambas). **Comprueba siempre `buf[1]`
y el bit 31 de `buf[4]`**: el eco en READ vuelve igual aunque el tag se ignore.

Tags verificados:

| Tag          | Función                                  |
|--------------|------------------------------------------|
| `0x00048003` | set physical width/height                |
| `0x00048004` | set virtual width/height                 |
| `0x00048009` | set virtual offset                       |
| `0x00048005` | set depth (32 = ARGB)                    |
| `0x00048006` | set pixel order                          |
| `0x00040001` | allocate framebuffer (alineación en `buf[5]`, devuelve dirección de bus y tamaño) |
| `0x00040008` | get pitch                                |
| `0x00030058` | **NOTIFY_XHCI_RESET** (valor: `bus<<20 \| dev<<15 \| fn<<12` = `0x100000` para el VL805) |

La dirección de framebuffer devuelta es de bus (p.ej. `0xFE8FA000`); para la CPU
quita los dos bits altos (`& 0x3FFFFFFF`).

Tag útil que no es de vídeo: **`0x00010003` "Get board MAC address"** —
6 bytes de respuesta con la MAC grabada de fábrica. Es la única forma de
obtenerla (no hay registro del GENET que la tenga). Igual que el
framebuffer: buffer alineado a 16, `dc cvac` antes de la llamada y
`dc ivac` después, o el VideoCore ve basura.

### 3.3 GIC-400

| Bloque | Base         | Registros usados                                      |
|--------|--------------|-------------------------------------------------------|
| GICD   | `0xFF841000` | `0x000` CTLR, `0x100+4n` ISENABLERn, `0x400+n` IPRIORITYRn (byte) |
| GICC   | `0xFF842000` | `0x000` CTLR, `0x004` PMR, `0x00C` IAR, `0x010` EOIR   |

Interrupciones (ID GIC = SPI + 32; PPI = 16..31):

| ID GIC | Origen                                   |
|--------|------------------------------------------|
| 27     | **timer virtual** `CNTV` (PPI 11) — verificado |
| 179    | PCIe host (SPI 147) *(spec, DT)*         |
| 180    | PCIe MSI (SPI 148) *(spec, DT)*          |

### 3.4 Temporizador genérico de ARM (verificado)

`cntfrq_el0` frecuencia (Hz), `cntpct_el0` contador físico (para esperas),
`cntv_tval_el0` + `cntv_ctl_el0` (bit0 enable) para el timer virtual con IRQ 27.
**Nunca esperar con bucles vacíos**: a `-O0`/`-O2` su duración no significa nada.

### 3.5 EMMC2 (tarjeta SD), `0xFE340000`

| Offset | Registro    | Offset | Registro   |
|--------|-------------|--------|------------|
| `0x00` | ARG2        | `0x28` | CONTROL0   |
| `0x04` | BLKSIZECNT  | `0x2C` | CONTROL1 (bit24 reset, bit2 clk en, bit1 clk stable) |
| `0x08` | ARG1        | `0x30` | INTERRUPT  |
| `0x0C` | CMDTM       | `0x34` | IRPT_MASK  |
| `0x10`–`0x1C` | RESP0–3 | `0x38` | IRPT_EN  |
|        |             | `0x3C` | CONTROL2   |

### 3.6 Power Management / Watchdog (reset real), `0xFE100000`

**PSCI por HVC (`hvc #0` con `PSCI_SYSTEM_RESET`/`PSCI_SYSTEM_OFF`) NO
funciona en la Pi 4 real** — solo responde en QEMU, que emula un firmware
EL3 atendiendo esa llamada. En bare metal sin ARM Trusted Firmware, `hvc`
no tiene nada al otro lado; la instrucción no hace nada observable (no
crashea, simplemente no reinicia). Verificado por prueba directa: 0 efecto
en hardware, funcionamiento normal en QEMU.

El reinicio real en un BCM2711 se hace por el bloque PM (Power Management),
usando su watchdog de hardware:

| Offset | Registro | Notas                                                      |
|--------|----------|-------------------------------------------------------------|
| `0x1C` | PM_RSTC  | control de reset; bits 5:0 = WRCFG, `0x20` = full reset      |
| `0x24` | PM_WDOG  | valor del watchdog, unidades de ~1/16 µs                     |

Todas las escrituras a este bloque exigen la "contraseña" `0x5A000000` en el
byte alto — sin ella, el hardware ignora la escritura por completo (mismo
mecanismo de protección que en el resto de la familia BCM2835/2711).

Secuencia verificada:
```
PM_WDOG = 0x5A000000 | 1                                    ; plazo minimo
PM_RSTC = 0x5A000000 | (PM_RSTC actual & 0xFFFFFFCF) | 0x20  ; WRCFG=full reset
```
El watchdog dispara el reset casi al instante (plazo de 1 unidad). No hay
forma verificada, en bare metal sin hablar con el chip de gestión de
energía externo, de hacer un apagado completo (corte de los 5V) solo desde
el núcleo ARM — como mucho, detener el procesador (`wfe` con IRQ
enmascaradas) y dejar la placa a la espera de desenchufar a mano.

---

## 4. MMU (verificado) — mapeo de identidad, una tabla por tarea

**Cambió en septiembre de 2026** (Semanas 10-11 de la bitácora). La
versión anterior de esta guía describía una única tabla L1 de bloques de
1 GB compartida por todo el sistema; eso valió mientras todo corría en
EL1. Ahora los programas corren en **EL0**, cada uno con su propia tabla,
y el kernel tiene la suya.

### 4.1 Estructura

Cada juego de tablas es una pareja alineada a 4 KB:

- **L1**: 512 entradas de 1 GB. Entrada 0 → *descriptor de tabla* que
  apunta a la L2. Entrada 3 = `0xC0000000`–`0xFFFFFFFF`, bloque
  Device-nGnRE (periféricos, GIC, PCIe). Entrada 24 =
  `0x600000000`–`0x63FFFFFFF`, bloque Device (ventana PCIe / BAR0 del VL805).
- **L2**: 512 entradas de **2 MB** que cubren el primer GB (toda la RAM
  que usa Nemo OS). Cada bloque es "solo kernel" o "usuario".

Hay 9 parejas: la del kernel (ningún bloque de usuario) y una por hueco
de tarea, en la que **solo su área de 16 MB** es de usuario — el resto
(kernel y las otras siete áreas) es solo EL1. Las 9 se construyen **una
vez** en el arranque y no se tocan nunca más: con mapeo de identidad, la
tabla del hueco *i* es la misma sea cual sea el programa que corra en él.

### 4.2 Descriptores

```
Device:  PA | 0x1 | (0<<2) | AF | nG | AP=00 | UXN | PXN
Kernel:  PA | 0x1 | (1<<2) | AF | nG | SH=3<<8 | AP=00 | UXN
Usuario: PA | 0x1 | (1<<2) | AF | nG | SH=3<<8 | AP=01 | PXN
Tabla:   dirección_L2 | 0x3
```

- `AP=00` (bits 7:6) = EL1 lee/escribe, EL0 nada. `AP=01` = los dos
  leen/escriben.
- `UXN` (bit 54) = EL0 no ejecuta; `PXN` (bit 53) = EL1 no ejecuta. El
  kernel nunca ejecuta desde un área de programa, ni un programa desde
  el kernel.
- **`nG` (bit 11) en TODAS las entradas, también las del kernel.** Ver 4.4.
- `MAIR_EL1`: índice 0 = `0x04` (Device-nGnRE), índice 1 = `0xFF` (Normal WB).
- `TCR_EL1` = `25 | 1<<8 | 1<<10 | 3<<12 | (PARange<<32)` — T0SZ=25,
  tablas cacheables, TG0 4 KB, `AS=0` (ASID de 8 bits), `A1=0` (el ASID
  viene de TTBR0).

### 4.3 Cambio de tarea: dos instrucciones, sin TLBI

`TTBR0_EL1 = dirección_L1 | (ASID << 48)`, con ASID 0 para el kernel e
`i+1` para el hueco *i*. `task_yield()` escribe `TTBR0_EL1` y hace `isb`
antes de `task_switch` — nada más. El TLB conserva las traducciones de
cada tarea etiquetadas por su ASID; no hay que vaciarlo al cambiar, ni al
reutilizar un hueco (mismo mapeo de identidad, misma tabla). Todo lo que
corre entre el `msr` y el `eret` a la tarea es código y datos del kernel,
idénticos en todas las tablas.

### 4.4 La trampa que hay que conocer antes de que exista

Con ASIDs, una traducción *global* (`nG=0`, compartida entre ASIDs) y una
*no global* para la **misma dirección virtual** conviviendo en el TLB es
un conflicto que ARM declara impredecible. El diseño ingenuo cae justo
ahí: el área de la tarea 1 sería "usuario, nG=1" en su tabla y "kernel,
global" en las otras ocho. Por eso **ninguna entrada es global**: todo va
etiquetado por ASID, incluidas las del kernel y los periféricos. El
precio son entradas de TLB duplicadas para el kernel; con 9 contextos, nada.

### 4.5 Lo que se mantiene

- `SCTLR_EL1 |= M | C | I` tras `dsb sy`; `isb` después.
- **No apagar la MMU** con la D-cache encendida sin limpiarla: cuelgue silencioso.
- `CPUECTLR_EL1.SMPEN` antes de activar la MMU (ver 1.2).
- Cortex-A72 es ARMv8.0: **no hay PAN**. El kernel, atendiendo una
  syscall, puede leer y escribir cualquier dirección de la tabla activa
  — incluida la de otras tareas (`AP=00` da a EL1 acceso completo). Por
  eso la validación de punteros de syscall es por software (§4b).

### 4.6 Pruebas que lo confirmaron

`crash_test.pro` escribe en `0x80000` desde EL0 → `ESR_EL1 = 0x9200004e`
(EC=0x24 Data Abort desde EL0; DFSC=0x0e *permission fault, level 2*;
WnR=1), `FAR_EL1 = 0x80000`, la tarea se retira y el shell sigue.
`crash_test3.pro` lee el área del hueco 0 desde el hueco 1 → mismo fallo
con WnR=0. Antes de la Fase 1, la primera escritura habría corrompido el
kernel en silencio.

---

## 4b. Tareas en EL0 (verificado)

- **Vectores**: las cuatro entradas "lower EL, AArch64" de `VBAR_EL1`
  (`+0x400`…`+0x580`) apuntan a los mismos manejadores que las de EL1
  (`sync/irq/fiq/serror`). Sin esto, la primera syscall desde EL0 cae en
  un vector vacío.
- **Arranque**: `task_trampoline` copia un stub de 12 bytes
  (`mov x8,#0 ; svc #0 ; b .`) al final del área, enmascara IRQ, y hace
  `eret` con `ELR_EL1 = entry`, `SPSR_EL1 = 0` (EL0t) y `SP_EL0 = stub`.
  El `ret` final de cualquier `_start` cae en el stub → `SYS_EXIT`.
- **Marco de contexto**: `task_switch` guarda y restaura también `SP_EL0`
  (marco de 176 bytes). Olvidarlo mezcla las pilas de usuario de dos
  tareas.
- **`CPACR_EL1.FPEN = 0b11`** para que EL0 pueda usar FP/NEON (Lua lo
  necesita). En la Pi 4 lo pone `start_pi4.S`.
- **Manejador de excepciones**: si `SPSR_EL1.M[3:0] == 0` (venía de EL0)
  → `task_exit_current()`, el sistema sigue; si venía de EL1 → fallo del
  kernel, `wfe` infinito. Imprime programa, desplazamiento dentro del
  programa, `ELR/ESR/FAR`, EC decodificado y `x0..x2`.
- **Validación de punteros de syscall**: cada syscall que recibe un
  puntero comprueba que el rango (o la cadena hasta su `\0`, byte a
  byte) está dentro del área de 16 MB de quien llama. Sin esto, un
  puntero a memoria del kernel pasado a `SYS_WRITE_STRING` lo leería EL1
  sin que la MMU dijera nada.

---

## 5. PCIe (BCM2711), base `0xFD500000` — todo verificado

### 5.1 Registros del controlador

| Offset   | Registro                          | Uso                                              |
|----------|-----------------------------------|--------------------------------------------------|
| `0x0000` | cabecera PCI del puente (bus 0)   | config del RC accesible directamente en base+reg |
| `0x0004` | Comando del puente                | Mem\|Master\|Parity\|SERR → `0x00100146` (solo acepta tras 5.3) |
| `0x0018` | buses                             | `0x00010100` (pri 0 / sec 1 / sub 1)             |
| `0x0020` | **MEMORY_BASE/LIMIT** del puente  | 16+16 bits, en unidades de 64KB (valor = addr>>16). Reset: `0xbff08000` = `0x80000000`–`0xBFFFFFFF` |
| `0x003C` | INT_LINE/PIN + BRIDGE_CONTROL     | `|= 1<<16` paridad                               |
| `0x00C8` | PCIe cap RTCTL                    | `|= 0x10` CRSSVE                                 |
| `0x0188` | VENDOR_SPECIFIC_REG1              | `&= ~0xC` endian BAR2 little                     |
| `0x043C` | PRIV1_ID_VAL3                     | clase `0x060400` (puente PCI-PCI)                |
| `0x04DC` | PRIV1_LINK_CAPABILITY             | ASPM bits 11:10 = `2` (solo L1)                  |
| `0x4008` | MISC_CTRL                         | `0x1000` SCB_ACCESS_EN, `0x2000` CFG_READ_UR_MODE, bits 21:20 burst (0=128B), bits 31:27 SCB0_SIZE |
| `0x400C` | WIN0_LO                           | dirección PCI baja de la ventana de salida        |
| `0x4010` | WIN0_HI                           | dirección PCI alta                                |
| `0x402C` | RC_BAR1_CONFIG_LO                 | `&= ~0x1F` (apagada)                              |
| `0x4034` | RC_BAR2_CONFIG_LO                 | bits 4:0 tamaño (`0x11` = 4GB) \| offset PCI bajo  |
| `0x4038` | RC_BAR2_CONFIG_HI                 | offset PCI alto                                   |
| `0x403C` | RC_BAR3_CONFIG_LO                 | `&= ~0x1F` (apagada)                              |
| `0x4068` | STATUS                            | bit5 DL_ACTIVE, bit4 PHYLINKUP, bit7 modo RC (`0xb0` = enlace OK, RC) |
| `0x4070` | WIN0_BASE_LIMIT                   | bits 15:4 base (MB, 12 bits bajos), bits 31:20 límite |
| `0x4080` | WIN0_BASE_HI                      | base (MB) >> 12                                   |
| `0x4084` | WIN0_LIMIT_HI                     | límite (MB) >> 12                                 |
| `0x4204` | HARD_PCIE_HARD_DEBUG              | bit27 SERDES_IDDQ (a 0), bit1 CLKREQ_DEBUG_ENABLE (a 1) |
| `0x4308` | INTR2_CPU_CLEAR                   | `0xFFFFFFFF` al arrancar                          |
| `0x4310` | INTR2_CPU_MASK_SET                | `0xFFFFFFFF` (no gestionamos aún)                 |
| `0x6014` | OUTB_ERR_CFG_CAUSE                | diagnóstico (0 = sin error registrado)            |
| `0x8000` | EXT_CFG_DATA                      | config de bus ≠ 0: leer/escribir en `0x8000 + reg` tras fijar INDEX |
| `0x9000` | EXT_CFG_INDEX                     | `bus<<20 \| dev<<15 \| fn<<12 \| (reg & ~3)`     |
| `0x9210` | RGR1_SW_INIT_1                    | **bit0 PERST#**, bit1 reset del puente             |

### 5.2 Ventanas (los tres números que hay que tener claros)

| Ventana                      | CPU                     | PCI            | Tamaño |
|------------------------------|-------------------------|----------------|--------|
| Salida (CPU → dispositivo)   | `0x6_0000_0000`         | `0xF800_0000`  | 64 MB  |
| Entrada RC_BAR2 (DMA → RAM)  | RAM (identidad)         | `0x1_0000_0000`| 4 GB   |
| Reenvío del puente (0x20)    | —                       | `0xF800_0000`–`0xFBFF_FFFF` | 64 MB |

Codificación verificada de la ventana de salida para CPU `0x600000000`, PCI
`0xf8000000`, 64MB: `WIN0_LO=0xf8000000 WIN0_HI=0 BASE_LIMIT=0x03f00000
BASE_HI=0x06 LIMIT_HI=0x06`. Puente: `0x20 = 0xfbf0f800` (los 4 bits bajos del
límite son de solo lectura).

**RC_BAR2 no puede estar en offset PCI 0**: con 4GB de tamaño solaparía
`0xf8000000`. Consecuencia: **toda dirección de RAM que se le pase al
dispositivo para DMA lleva sumado `0x1_0000_0000`**.

### 5.3 Secuencia de inicialización (la que funciona, en orden)

```
 1. RGR1 = 0x3        puente en reset + PERST# asertado; 1 ms
 2. RGR1 = 0x1        puente activo, PERST# sigue asertado
 3. HARD_DEBUG &= ~SERDES_IDDQ; 1 ms
 4. leer VID del puente (0x0000) == 0x14E4
 5. INTR2 CLEAR = MASK_SET = 0xFFFFFFFF
 6. MISC_CTRL |= SCB_ACCESS_EN | CFG_READ_UR_MODE; burst = 128B
 7. RC_BAR2 LO = 0x11, HI = 0x1; RC_BAR1/3 off; MISC_CTRL.SCB0_SIZE = 0x11
 8. LINK_CAP (0x4DC) ASPM = solo L1
 9. ID_VAL3 (0x43C) clase = 0x060400
10. VENDOR_SPECIFIC_REG1 (0x188) &= ~0xC
11. RGR1 = 0x0        PERST# liberado UNA vez; 100 ms; esperar STATUS & 0x30 == 0x30
12. WIN0 (0x400C/0x4010/0x4070/0x4080/0x4084)
13. HARD_DEBUG |= CLKREQ_DEBUG_ENABLE
14. puente: 0x18 = 0x00010100; 0x20 = MEMORY_BASE/LIMIT; 0x3C |= 1<<16;
    0xC8 |= 0x10; 0x04 |= 0x146
15. mailbox NOTIFY_XHCI_RESET (0x30058, valor 0x100000); comprobar respuesta; 300 ms
16. VL805 (bus1,dev0,fn0): VID 0x1106 DID 0x3483; 0x0C=16; 0x10=0xf8000000; 0x14=0;
    0x04 |= 0x146
17. leer 0x600000000: CAPLENGTH/HCIVERSION = 0x01000020
```

Lo que rompe la secuencia si se altera: escribir `0` en RGR1 como primer paso
(libera PERST# con el puente vacío; además dispara un SError con la MMU activa),
esperar con bucles en vez de milisegundos reales, omitir el paso 14 (el Comando
del puente no acepta la escritura), o notificar el firmware sin esperar.

### 5.4 El VL805 (xHCI de los puertos USB-A)

| Dato                | Valor                                  |
|---------------------|----------------------------------------|
| Bus/dev/fn          | 1 / 0 / 0                              |
| VID / DID           | `0x1106` / `0x3483`                    |
| BAR0                | 4 KB, 64 bits (`0xfffff004` al sondear)|
| Firmware            | **Sin EEPROM en la 8GB rev 1.5**: lo carga VideoCore en cada reset vía NOTIFY_XHCI_RESET. Sin él, la configuración responde (hardware) pero los registros xHCI devuelven relleno |

### 5.5 Diagnóstico

`0xdeaddead` = relleno del bus interno de Broadcom para accesos que no llegan a
nadie (**no** es un valor del dispositivo). `OUTB_ERR_CFG_CAUSE (0x6014) = 0`
significa que el controlador no vio ningún error — el problema está en un
eslabón que no reenvía (ventana, puente, BAR2) o en un dispositivo sin firmware.
Árbol de decisión completo: `DIAGRAMA_DIAGNOSTICO_PCIE_PI4.mermaid`.

---

## 6. xHCI (VL805) — registros verificados

Base = `0x600000000` (BAR0 en el lado CPU).

**Capability** (en base):

| Offset | Registro   | Valor leído  | Notas                                              |
|--------|------------|--------------|----------------------------------------------------|
| `0x00` | CAPLENGTH  | `0x20`       | byte; los operacionales empiezan en base+0x20      |
| `0x02` | HCIVERSION | `0x0100`     | 16 bits, xHCI 1.0                                  |
| `0x04` | HCSPARAMS1 | `0x05000420` | slots=32 (7:0), interrupters=4 (18:8), **puertos=5** (31:24) |
| `0x08` | HCSPARAMS2 | `0xfc000031` | scratchpad buffers en bits 31:27 y 25:21 *(spec)*  |
| `0x0C` | HCSPARAMS3 |              |                                                    |
| `0x10` | HCCPARAMS1 | `0x002841eb` | bit0 **AC64=1** (punteros 64 bits), bit2 CSZ=0 (contextos de 32 bytes), xECP en 31:16 |
| `0x14` | DBOFF      |              | offset de los doorbells                            |
| `0x18` | RTSOFF     |              | offset de los registros runtime                    |

**Operational** (en base + CAPLENGTH):

| Offset | Registro  | Notas                                                     |
|--------|-----------|-----------------------------------------------------------|
| `0x00` | USBCMD    | bit0 RS, bit1 **HCRST**, bit2 INTE                        |
| `0x04` | USBSTS    | bit0 HCH, bit2 HSE, bit3 EINT, bit11 **CNR**; `0x11` tras reset = parado, listo |
| `0x08` | PAGESIZE  | `1` = páginas de 4 KB                                     |
| `0x18` | CRCR      | anillo de comandos (64 bits), bit0 RCS *(spec)*           |
| `0x30` | DCBAAP    | DCBAA (64 bits, alineado a 64) *(spec)*                    |
| `0x38` | CONFIG    | MaxSlotsEn *(spec)*                                        |
| `0x400 + 0x10·(n-1)` | PORTSC del puerto n | bit0 CCS, bit1 PED, bit4 PR, bits 8:5 PLS, bit9 PP, bits 13:10 velocidad (1 Full, 2 Low, 3 High, 4 Super), bit17 CSC, bit21 PRC |

Lectura real con un teclado en el puerto 1: `PORTSC=0x400202e1` → CCS=1,
PED=0, PLS=7 (Polling), PP=1, CSC=1, velocidad=0 → **dispositivo USB 2.0 a la
espera del reset del puerto** (la velocidad se conoce después). Puertos vacíos:
`0x000002a0`.

**Regla DMA**: cada dirección escrita en CRCR, DCBAAP, ERSTBA, en la DCBAA o en
cualquier TRB es `dirección CPU + 0x1_0000_0000`, y la estructura debe
limpiarse de la caché (`dc cvac` + `dsb sy`) antes de que el controlador la lea;
lo que el controlador escribe (anillo de eventos, buffers IN) se invalida
(`dc ivac`) antes de leerlo. El DMA de PCIe no es coherente con las cachés.

**Topología USB de la Pi 4 (verificada)**: el VL805 expone 5 puertos raíz. Todo
dispositivo **USB 2.0**, en cualquiera de los cuatro conectores, aparece detrás
del **hub interno** `2109:3431` "USB2.0 Hub" (VIA Labs, 4 puertos, TTT=3,
alimentación en 100 ms) en el puerto raíz 1 a High Speed. Un teclado y un
ratón LS comparten por tanto un mismo Transaction Translator: es el caso
normal. Los dispositivos USB 3.0 van a los puertos raíz 2-5.

**Secuencia que funciona para un dispositivo HID** (todo verificado):
Enable Slot → Address Device (Slot: route string = puerto del hub en bits 3:0,
TT Hub Slot ID + TT Port si es LS/FS tras hub HS; EP0 MPS 8 para LS/FS, 64 HS)
→ GET_DESCRIPTOR device/config/string → **SET_CONFIGURATION** (sin él solo
existe EP0: los EP de interrupción no responden) → SET_PROTOCOL(boot) →
SET_IDLE(0) → GET_DESCRIPTOR(report, 0x22) → Configure Endpoint (Add A0 + A_dci,
Context Entries = DCI máximo, EP type 7, Interval 6 para bInterval 10 ms en LS)
→ TRBs Normal con IOC|ISP → doorbell(slot, dci). Hub: además Configure Endpoint
del slot del hub con Hub=1, Number of Ports y TTT antes de direccionar hijos;
SET_PORT_FEATURE(POWER) + espera; GET_PORT_STATUS (0xA3/0); reset del puerto
(SET_FEATURE 4, esperar C_PORT_RESET, CLEAR_FEATURE 20); velocidad de los bits
9 (LS) / 10 (HS) del estado.

Recuperación de un EP tras cc 4/6/36: Reset Endpoint (TRB 14) + Set TR Dequeue
Pointer (TRB 16, DCS = cycle del TRB destino) + doorbell.

**Informes HID vistos**: teclado boot `[mod][0][k1..k6]` (8 B, MPS 8). Ratón
boot `[botones][dx][dy]` (3 B). Ratón genérico `0000:3825` en report protocol:
`[01][botones][X:12][Y:12][rueda]` (6 B, MPS 6).

---

## 6b. GENET — Ethernet integrado (verificado, Gigabit)

Controlador **GENET v5** del BCM2711, base **`0xFD580000`** (dentro del
bloque de periféricos ya mapeado como Device). RGMII hacia un PHY externo
**BCM54213PE** en la dirección MDIO 1. Referencia: el driver de U-Boot
(`drivers/net/bcmgenet.c`) — no hay documentación pública completa del
chip; todo offset de aquí viene de ahí y está probado.

### 6b.1 Registros (desplazamientos desde la base)

| Offset | Registro | Notas |
|---|---|---|
| `0x000` | `SYS_REV_CTRL` | `(v>>24)&0xF` debe ser **6** (= v5, así lo codifica el chip) |
| `0x004` | `SYS_PORT_CTRL` | `3` = PORT_MODE_EXT_GPHY (RGMII) |
| `0x008` | `SYS_RBUF_FLUSH_CTRL` | pulso del bit 1 al resetear |
| `0x08c` | `EXT_RGMII_OOB_CTRL` | tras negociar: `\|= RGMII_LINK(4) \| RGMII_MODE_EN(6) \| ID_MODE_DIS(16)`, `&= ~OOB_DISABLE(5)` |
| `0x300` | `RBUF_CTRL` | `\|= RBUF_ALIGN_2B(1)`: 2 bytes de relleno antes de cada trama recibida |
| `0x3b4` | `RBUF_TBUF_SIZE_CTRL` | `= 1` |
| `0x808` | `UMAC_CMD` | `TX_EN(0)`, `RX_EN(1)`, velocidad en bits 3:2 (`0`=10, `1`=100, `2`=1000), `SW_RESET(13)`, `LCL_LOOP_EN(15)` |
| `0x80c`/`0x810` | `UMAC_MAC0/1` | MAC: `b0<<24\|b1<<16\|b2<<8\|b3` y `b4<<8\|b5` |
| `0x814` | `UMAC_MAX_FRAME_LEN` | `1536` |
| `0xb34` | `UMAC_TX_FLUSH` | pulso `1` → `0` al deshabilitar DMA |
| `0xd80` | `UMAC_MIB_CTRL` | reset de contadores |
| `0xe14` | `MDIO_CMD` | `START_BUSY(29)`, `READ_FAIL(28)`, `RD=2<<26`, `WR=1<<26`, PHY en `[25:21]`, registro en `[20:16]`, dato en `[15:0]` |
| `0x2000` | descriptores RX | 256 × 12 bytes: `LENGTH_STATUS`, `ADDR_LO`, `ADDR_HI` — **viven en el espacio de registros**, no en RAM |
| `0x4000` | descriptores TX | ídem |
| `0x2C00`/`0x4C00` | registros de anillos RDMA/TDMA | 17 anillos × `0x40`; se usa solo el **16** (cola por defecto) |
| `0x3040`/`0x5040` | `RDMA/TDMA_REG_BASE` | `DMA_RING_CFG(0x00) = 1<<16`, `DMA_CTRL(0x04) = (1<<17) \| DMA_EN`, `SCB_BURST_SIZE(0x0c) = 8` |

Anillo 16 (`RING_REG_BASE` = `REG_OFF + 16*0x40`): `READ_PTR +0x00`,
`CONS_INDEX +0x08`, `PROD_INDEX +0x0c`, `RING_BUF_SIZE +0x10` (=
`256<<16 | 2048`), `START_ADDR +0x14` (=0), `END_ADDR +0x1c` (=
`256*12/4-1`), `MBUF_DONE_THRESH +0x24` (=1, TX), `FLOW_PERIOD +0x28`
(=0 TX; `XON_XOFF_THRESH` RX = `5<<16 | 16`), `WRITE_PTR +0x2c`.

### 6b.2 Secuencia que funciona

1. Leer `SYS_REV_CTRL`, comprobar major 6. 2. MAC por mailbox
(`0x00010003`). 3. `SYS_PORT_CTRL = 3`. 4. `UMAC_CMD = SW_RESET|LCL_LOOP_EN`,
2 µs, `= 0`; reset de MIB; `MAX_FRAME_LEN`; `RBUF_ALIGN_2B`. 5. Escribir la
MAC. 6. Deshabilitar DMA (`DMA_CTRL &= ~EN`, pulso `TX_FLUSH`). 7. Anillos:
RX primero — `CONS_INDEX = PROD_INDEX` leído (el PROD no se puede poner a
0), descriptores con `LENGTH_STATUS = 2048<<16 | DMA_OWN(0x8000)` —, luego
TX. 8. Habilitar DMA. 9. **PHY** (6b.3). 10. `EXT_RGMII_OOB_CTRL`, velocidad
en `UMAC_CMD`, `TX_EN|RX_EN`.

### 6b.3 El PHY — la lección que costó una vuelta

`CMD_SW_RESET` resetea el lado MAC del GENET, **nunca el PHY externo**. Sin
un reset explícito del PHY, el chip sigue con lo que tuviera configurado
del arranque anterior (otro sistema operativo) — negocia con un switch
indulgente, pero **no con un ordenador conectado en directo**. Secuencia
IEEE 802.3 estándar por MDIO (Clause 22):

1. `BMCR (0) = 0x8000` (reset), esperar a que el bit se autolimpie.
2. `ADVERTISE (4) = 0x01E1` (10/100 half/full + IEEE 802.3),
   `CTRL1000 (9) = 0x0200` (**Gigabit full**: sin este anuncio aparte, el
   PHY nunca ofrece Gigabit).
3. `BMCR = 0x1200` (ANENABLE|ANRESTART); sondear `BMSR (1)` bit 2 (link)
   hasta 5 s.
4. Velocidad negociada: registro **`0x19`** (Broadcom *Auxiliary Status
   Summary*), bits `[10:8]`: `0-1`=10, `2-3`=100, `≥4`=1000. Específico
   de Broadcom — con otro PHY, esto es lo primero a revisar.

Si `BMSR` lee `0xFFFF`, el PHY no responde por MDIO (dirección
equivocada o fallo del driver): no es "falta de cable".

### 6b.4 Enviar y recibir

- **TX**: `cache_limpiar` (`dc cvac`) del buffer del llamador; descriptor
  `ADDR_LO/HI` + `LENGTH_STATUS = len<<16 | 0x3F<<7 | APPEND_CRC(0x40) | SOP(0x2000) | EOP(0x4000)`
  — **los bits QTAG (`0x3F<<7`) son obligatorios**, sin ellos el árbitro
  descarta la trama; `PROD_INDEX++`; esperar `CONS_INDEX >= PROD` (16 bits).
- **RX**: `prod = RDMA_PROD_INDEX & 0xFFFF` — **solo los 16 bits bajos
  son el índice; los altos son un contador de descartes**: sin la máscara,
  al primer descarte `prod` nunca vuelve a igualar `cons` y se releen
  descriptores viejos para siempre. Si `prod != cons`: `dc ivac` del
  buffer, longitud en `LENGTH_STATUS[27:16] & 0xFFF`, trama a partir del
  byte 2 (relleno de `RBUF_ALIGN_2B`), `CONS_INDEX++`.
- Los descriptores no necesitan mantenimiento de caché (memoria de
  dispositivo); los buffers de paquetes sí, exactamente como el DMA de
  xHCI (§6).

Confirmado en hardware: enlace a 1000 Mbps conectado directo a un Mac,
`ping` con 5-20 ms, TCP. La pila (ARP/IPv4/ICMP/TCP y una shell remota)
está en `src/net.c`, `src/tcp.c`, `src/netshell.c` — ver `GUIA_RED_NEMO_OS.md`.

---

## 7. Trampas que cuestan horas (todas sufridas)

- **Comparar valores de registros no basta**: importan el orden y los tiempos.
- El driver del SoC de Linux (`pcie-brcmstb.c`) no escribe `MEMORY_BASE/LIMIT`;
  lo hace el núcleo genérico de PCI. Comparar solo el driver lo oculta.
- Un `SError` real puede ser síntoma secundario (aquí, del orden de reset).
- `#if RASPPI == 4` / `#if RASPPI >= 5`: dos copias del mismo paso; editar la
  que compila para tu placa.
- `otg_mode=1` no tiene que ver con el VL805.
- `EXT_CFG_DATA` es `0x8000`, no `0x9004`.
- El registro de numeración de buses (`0x18`) es imprescindible: sin él el
  puente no reenvía nada a bus 1.
- Sin `dc cvac`/`dc ivac` alrededor del buffer del mailbox, VideoCore ve basura.
- Apagar la MMU con la D-cache encendida cuelga la CPU en silencio.
- Un armstub propio sin `CPUECTLR_EL1.SMPEN` cuelga al activar la MMU.
- Entradas globales y no globales para la misma VA en el TLB (con ASIDs):
  comportamiento impredecible. `nG=1` en todo, también el kernel (§4.4).
- Los vectores "lower EL" de `VBAR_EL1` sin conectar: la primera syscall
  desde EL0 cae en un vector vacío (§4b).
- `SW_RESET` del GENET no toca el PHY: sin reset del PHY por MDIO, un
  Mac conectado en directo no negocia nunca (§6b.3).
- `RDMA_PROD_INDEX` sin enmascarar a 16 bits: tras el primer descarte del
  hardware, recepción muerta para siempre (§6b.4).
- Bits QTAG (`0x3F<<7`) ausentes en el descriptor TX: la trama se
  descarta sin error.
- `x19` sin guardar en un `_start` a mano (el runtime de Lua): el kernel
  lo usa como callee-saved y el programa vuelve con basura. Guardar
  `x19-x28` como marca la ABI.
- El `tick` por UART cada segundo tapa el log; captura a fichero o quítalo.
- La terminal serie sin *scrollback* hace perder arranques: `screen -L`.
- **`SET_CONFIGURATION` a cada dispositivo**, no solo al hub: sin él los
  endpoints de interrupción no existen (descriptores OK, informes nunca; el
  teclado LS tras el TT da Split Transaction Error 36).
- Las esperas bloqueantes que consumen el anillo de eventos deben **atender**
  los eventos ajenos (reponer sus TRBs), no descartarlos.
- **`WFE` con IRQ enmascarada duerme para siempre** en hardware (QEMU no lo
  muestra). Dentro de un manejador de excepción la CPU enmascara IRQ: si una
  syscall cede el control con `wfe`, habilitar IRQ en el stub de `svc` y
  enmascararla antes de restaurar `elr/spsr`.
- Un ratón barato puede usar un informe de 12 bits con report ID en report
  protocol; pedir protocolo boot lo simplifica.
- **`PSCI`/`hvc` para reset/apagado solo funciona en QEMU**: en hardware
  real, sin ARM Trusted Firmware en EL3, la instrucción no hace nada — ni
  falla ni avisa, simplemente no reinicia. Usar el watchdog del bloque PM
  (§3.6) para un reset de verdad en la Pi 4.
- Las funciones de framebuffer (`fb_put_pixel`/`fb_get_pixel`/`fb_present`)
  deben direccionar cada fila con el ANCHO REAL de la resolución activa
  (guardado tras `fb_init_pi4()`), no con una constante de compilación: si
  alguna vez se admite más de una resolución, usar el ancho fijo de
  compilación desalinea la imagen en diagonal — invisible mientras solo
  exista una resolución posible.
- **Ninguna dirección de puntero a texto debe grabarse en una tabla de
  datos estática** (array de struct, o incluso una función con
  `switch`/`if` de pocos casos densos) dentro de un ejecutable `.pro`:
  estos binarios cargan sin ningún paso de reubicación, y el propio
  compilador optimizador puede convertir esa selección indexada en una
  tabla de punteros interna — grabada con la dirección de ENLAZADO, no
  válida si el programa carga en otra zona de memoria en tiempo real.
  Cada texto debe ir literal, directamente en su propia llamada.

---

## 8. Archivos de firmware en la SD

`start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`, `overlays/disable-bt.dtbo`,
`config.txt`, `kernel8.img`. Firmware oficial de Raspberry Pi (probado con la
revisión de agosto de 2025). `bootcode.bin` no hace falta en la Pi 4 (está en
la EEPROM). Con `uart_2ndstage=1` el firmware imprime su propio log — la línea
`arm_loader: Starting ARM with NNNMB` marca el momento en que tu código empieza.
