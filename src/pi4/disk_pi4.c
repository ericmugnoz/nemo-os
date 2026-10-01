// disk_pi4.c -- driver real de disco para Raspberry Pi 4 (EMMC2)
//
// IMPORTANTE -- particion compartida con el arranque:
// Esta misma tarjeta SD tiene, ademas de NemoFS, la particion de
// arranque FAT32 que el propio firmware de la Pi 4 necesita
// (config.txt, kernel8.img, el firmware, el DTB). NemoFS NO puede
// escribir desde el sector 0 absoluto -- eso destruiria la tabla
// de particiones y la propia particion de arranque (lo que paso
// exactamente en la primera prueba real de este driver).
//
// Solucion: la particion de arranque ocupa los primeros 512MB de
// la tarjeta (ver instrucciones de particionado en la bitacora).
// Todo acceso de NemoFS a "sector N" se traduce aqui, sumando
// SECTOR_OFFSET_NEMOFS, a la direccion fisica real "sector N +
// 512MB" -- NemoFS nunca sabe que no empieza en el sector 0 fisico
// de verdad, y la particion de arranque queda intacta siempre.
#define SECTOR_OFFSET_NEMOFS ((512ULL * 1024 * 1024) / 512)  // 1048576 sectores
//
// La pieza mas dificil de todo este port. A diferencia de mailbox,
// GIC, o MMU (donde una unica fuente clara resolvia el problema),
// aqui la informacion esta genuinamente dispersa y a veces
// contradictoria entre modelos de Raspberry Pi. Cada direccion y
// cada decision de este archivo esta verificada contra al menos
// dos fuentes independientes -- ver la bitacora del port para el
// detalle completo de esa investigacion.
//
// Verificado:
//   - EMMC2_BASE = 0xFE340000 (peripheral base 0xFE000000 + 0x340000)
//     Confirmado por: foro oficial de Raspberry Pi (desarrollador
//     real reportando su propio driver funcionando) Y el arbol de
//     dispositivos real volcado en OSDev Wiki
//     ("fe340000-fe3400ff : emmc2@7e340000").
//   - El layout de registros es SDHCI estandar (offsets 0x00-0xFE),
//     confirmado cruzando la cabecera real de U-Boot (sdhci.h) con
//     los nombres al estilo Broadcom (BLKSIZECNT, CMDTM, etc.) que
//     aparecen en drivers ya funcionando para modelos anteriores.
//   - La Pi 4 (a diferencia de la Pi 3) EXIGE activar 3.3V en el
//     registro de control de energia -- confirmado por un parche
//     real de barebox, con commit explicando que sin esto la
//     tarjeta no respondia en la Pi 4 aunque funcionara en la Pi 3.
//   - La frecuencia base del reloj de EMMC2 se pide por mailbox,
//     tag 0x00030002 (GET_CLOCK_RATE), clock_id=12 (especifico de
//     EMMC2, distinto del id=1 de la Pi 3) -- confirmado por un
//     commit real de U-Boot añadiendo esa constante explicitamente
//     para el soporte de Pi 4.
//   - Bug de firmware conocido: en firmwares recientes,
//     GET_CLOCK_RATE puede devolver 0 en vez de la frecuencia real
//     -- confirmado por un issue real y ya resuelto en el propio
//     repositorio de firmware de Raspberry Pi. Solucion aplicada
//     aqui igual que en U-Boot: si devuelve 0, pedir
//     GET_MAX_CLOCK_RATE (tag 0x00030004) como respaldo.
//
// Sin verificar con la misma certeza (primera candidata a fallar
// en la primera prueba real, como paso con el mailbox y con
// CPACR_EL1 antes):
//   - Si las lineas fisicas de la tarjeta SD necesitan
//     configuracion de GPIO (ALT function) como el UART, o si son
//     pines dedicados que no la necesitan. Este driver ASUME que
//     no hace falta, dado que ninguna fuente consultada la
//     menciona para EMMC2 especificamente (a diferencia del UART,
//     donde SI hacia falta) -- pero es una suposicion, no una
//     certeza confirmada.
//   - Los timings exactos de espera entre pasos de la
//     inicializacion SD -- se uso el orden estandar del protocolo
//     SD, pero sin poder verificar en hardware real que cada
//     espera sea suficiente para esta tarjeta/controlador concretos.

#include <stdint.h>
#include <stdbool.h>
#include "disk.h"
#include "fat.h"
#include "nemofs.h"   // para que NemoFS crezca al estirar
#include "xhci_pi4.h"
void uart_puts(const char *s);
void uart_putc(char c);

void uart_puts(const char *s);
static void uart_put_hex32(uint32_t val) {
    extern void uart_putc(char c);
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uint8_t nibble = (val >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10);
        uart_putc(c);
    }
}

// Las direcciones de los perifericos se pueden cambiar desde fuera al
// compilar. En la Pi 4 valen lo que dice el manual; en una prueba de
// escritorio se apuntan a un trozo de memoria normal, y asi la logica
// de la cache se puede comprobar sin tarjeta y sin hardware.
#ifndef PERIPHERAL_BASE
#define PERIPHERAL_BASE 0xFE000000UL
#endif
#ifndef EMMC2_BASE
#define EMMC2_BASE      0xFE340000UL
#endif
#define MAILBOX_BASE    (PERIPHERAL_BASE + 0xB880)

// Las tres operaciones de cache de datos que necesita el mailbox, con
// nombre propio. Fuera de ARM64 no existen esas instrucciones y quedan
// en nada: no hay mailbox que atender en una prueba de escritorio, y
// asi este archivo se puede compilar en el ordenador para comprobar la
// logica de la cache de sectores sin tocar la Pi.
#ifdef __aarch64__
#define LIMPIAR_CACHE(a) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory")
#define TIRAR_CACHE(a)   __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory")
#define BARRERA()        __asm__ volatile("dsb sy" ::: "memory")
#else
#define LIMPIAR_CACHE(a) ((void)(a))
#define TIRAR_CACHE(a)   ((void)(a))
#define BARRERA()        ((void)0)
#endif

// --- Registros del mailbox, mismos que en mailbox_pi4.c ---
#define MAILBOX_READ   (*(volatile uint32_t *)(MAILBOX_BASE + 0x00))
#define MAILBOX_STATUS (*(volatile uint32_t *)(MAILBOX_BASE + 0x18))
#define MAILBOX_WRITE  (*(volatile uint32_t *)(MAILBOX_BASE + 0x20))
#define MAILBOX_FULL   0x80000000
#define MAILBOX_EMPTY  0x40000000
#define MAILBOX_CH_PROP 8

#define TAG_GET_CLOCK_RATE     0x00030002
#define TAG_GET_MAX_CLOCK_RATE 0x00030004
#define CLOCK_ID_EMMC2         12

__attribute__((aligned(16)))
static volatile uint32_t msg_reloj[8];

// --- Registros EMMC2, offsets SDHCI estandar verificados ---
#define EMMC_ARG2         (*(volatile uint32_t *)(EMMC2_BASE + 0x00))
#define EMMC_BLKSIZECNT   (*(volatile uint32_t *)(EMMC2_BASE + 0x04))
#define EMMC_ARG1         (*(volatile uint32_t *)(EMMC2_BASE + 0x08))
#define EMMC_CMDTM        (*(volatile uint32_t *)(EMMC2_BASE + 0x0C))
#define EMMC_RESP0        (*(volatile uint32_t *)(EMMC2_BASE + 0x10))
#define EMMC_RESP1        (*(volatile uint32_t *)(EMMC2_BASE + 0x14))
#define EMMC_RESP2        (*(volatile uint32_t *)(EMMC2_BASE + 0x18))
#define EMMC_RESP3        (*(volatile uint32_t *)(EMMC2_BASE + 0x1C))
#define EMMC_DATA         (*(volatile uint32_t *)(EMMC2_BASE + 0x20))
#define EMMC_STATUS       (*(volatile uint32_t *)(EMMC2_BASE + 0x24))
#define EMMC_CONTROL0     (*(volatile uint32_t *)(EMMC2_BASE + 0x28))
#define EMMC_CONTROL1     (*(volatile uint32_t *)(EMMC2_BASE + 0x2C))
#define EMMC_INTERRUPT    (*(volatile uint32_t *)(EMMC2_BASE + 0x30))
#define EMMC_IRPT_MASK    (*(volatile uint32_t *)(EMMC2_BASE + 0x34))
#define EMMC_IRPT_EN      (*(volatile uint32_t *)(EMMC2_BASE + 0x38))
#define EMMC_CONTROL2     (*(volatile uint32_t *)(EMMC2_BASE + 0x3C))

// Bits de EMMC_STATUS (PRESENT_STATE)
#define ST_CMD_INHIBIT    (1 << 0)
#define ST_DAT_INHIBIT    (1 << 1)
#define ST_DAT_ACTIVE     (1 << 2)

// Bits de EMMC_INTERRUPT
#define INT_CMD_DONE   (1 << 0)
#define INT_DATA_DONE  (1 << 1)
#define INT_WRITE_RDY  (1 << 4)
#define INT_READ_RDY   (1 << 5)
#define INT_ERROR_MASK 0xFFFF0000

// Tipos de respuesta de comando, para el campo CMDTM
#define CMD_RESP_NONE       (0 << 16)
#define CMD_RESP_136        (1 << 16)
#define CMD_RESP_48         (2 << 16)
#define CMD_RESP_48_BUSY    (3 << 16)
#define CMD_CRCCHK_EN       (1 << 19)
#define CMD_IXCHK_EN        (1 << 20)
#define CMD_ISDATA          (1 << 21)
#define TM_DAT_DIR_READ     (1 << 4)
#define TM_BLKCNT_EN        (1 << 1)
#define TM_AUTO_CMD12       (1 << 2)   // el controlador manda CMD12 al acabar
#define TM_MULTI_BLOQUE     (1 << 5)   // varios bloques con un solo comando

// Bit 1 de CONTROL0: el controlador habla por CUATRO lineas de datos en
// vez de una. La tarjeta arranca siempre en una sola (asi manda la
// norma SD, para que cualquier controlador pueda entenderse con ella
// antes de negociar nada) y hay que pedirle el cambio con ACMD6. Los
// dos lados tienen que moverse a la vez: si el anfitrion cambia y la
// tarjeta no, lo que se lee es basura. Por eso este bit solo se toca
// si la tarjeta ya ha aceptado.
#define HCTL_ANCHO_4BITS    (1 << 1)

// Bit 2 de CONTROL0: modo de alta velocidad. Cambia en que flanco del
// reloj muestrea el controlador, y hace falta al pasar de 25 a 50 MHz.
// Igual que el anterior: solo se toca si la tarjeta ya ha cambiado.
#define HCTL_ALTA_VELOCIDAD (1 << 2)

static bool esperar_bit_status(uint32_t bit, uint32_t timeout_iter) {
    while (timeout_iter--) {
        if (!(EMMC_STATUS & bit)) return true;
    }
    return false;
}

// Guarda el ultimo estado de interrupcion/error visto, ANTES de
// limpiarlo -- para poder diagnosticar despues de que
// emmc_comando() ya devolvio false y el registro real ya se borro.
static uint32_t g_ultimo_interrupt_error = 0;
static bool g_ultimo_fue_timeout = false;

static bool esperar_interrupt(uint32_t bit, uint32_t timeout_iter) {
    while (timeout_iter--) {
        uint32_t irpt = EMMC_INTERRUPT;
        if (irpt & INT_ERROR_MASK) {
            g_ultimo_interrupt_error = irpt;
            g_ultimo_fue_timeout = false;
            EMMC_INTERRUPT = irpt;  // limpiar, escribiendo 1 en los bits que estaban activos
            return false;
        }
        if (irpt & bit) {
            EMMC_INTERRUPT = bit;
            return true;
        }
    }
    g_ultimo_interrupt_error = 0;
    g_ultimo_fue_timeout = true;
    return false;
}

// Envia un comando SD y espera su finalizacion. 'arg' es el
// argumento de 32 bits del comando; 'flags' combina el numero de
// comando con el tipo de respuesta esperada (CMD_RESP_*).
static bool emmc_comando(uint32_t indice, uint32_t flags, uint32_t arg) {
    if (!esperar_bit_status(ST_CMD_INHIBIT, 1000000)) return false;

    EMMC_INTERRUPT = 0xFFFFFFFF;  // limpiar cualquier interrupcion pendiente
    EMMC_ARG1 = arg;
    EMMC_CMDTM = (indice << 24) | flags;

    return esperar_interrupt(INT_CMD_DONE, 1000000);
}

// --- Consulta de la frecuencia base del reloj de EMMC2 por mailbox ---
static uint32_t pedir_frecuencia_reloj(uint32_t tag) {
    msg_reloj[0] = 8 * 4;
    msg_reloj[1] = 0;
    msg_reloj[2] = tag;
    msg_reloj[3] = 8;
    msg_reloj[4] = 8;
    msg_reloj[5] = CLOCK_ID_EMMC2;
    msg_reloj[6] = 0;
    msg_reloj[7] = 0;

    uintptr_t base = (uintptr_t)&msg_reloj;
    uintptr_t fin = base + sizeof(msg_reloj);
    for (uintptr_t a = base & ~63UL; a < fin; a += 64) {
        LIMPIAR_CACHE(a);
    }
    BARRERA();

    uint32_t direccion = ((uint32_t)base & ~0xF) | MAILBOX_CH_PROP;
    while (MAILBOX_STATUS & MAILBOX_FULL) {}
    MAILBOX_WRITE = direccion;
    while (1) {
        while (MAILBOX_STATUS & MAILBOX_EMPTY) {}
        if (MAILBOX_READ == direccion) break;
    }

    for (uintptr_t a = base & ~63UL; a < fin; a += 64) {
        TIRAR_CACHE(a);
    }
    BARRERA();

    return msg_reloj[6];  // rate_hz
}

static uint32_t obtener_frecuencia_base(void) {
    uint32_t freq = pedir_frecuencia_reloj(TAG_GET_CLOCK_RATE);
    if (freq == 0) {
        // Bug de firmware conocido (issue real de Raspberry Pi):
        // en firmwares recientes GET_CLOCK_RATE puede devolver 0.
        // Respaldo: pedir la frecuencia maxima, igual que hace
        // U-Boot para el mismo problema.
        freq = pedir_frecuencia_reloj(TAG_GET_MAX_CLOCK_RATE);
    }
    return freq;
}

// Configura el divisor de reloj para alcanzar (aproximadamente) la
// frecuencia deseada, a partir de la frecuencia base real obtenida
// por mailbox -- nunca asumida fija, a diferencia de un primer
// intento ingenuo.
static void establecer_reloj(uint32_t freq_base, uint32_t freq_deseada) {
    uint32_t divisor = 2;
    while ((freq_base / divisor) > freq_deseada && divisor < 0x3FF) {
        divisor <<= 1;
    }
    divisor >>= 1;
    if (divisor == 0) divisor = 1;

    // Parar el reloj HACIA LA TARJETA antes de cambiar el
    // divisor. Cuando esta funcion solo se llamaba al arrancar daba
    // igual, porque el reloj aun no estaba en marcha; ahora tambien se
    // llama para subir de 25 a 50 MHz con la tarjeta funcionando, y
    // cambiar el divisor en caliente puede colarle medio pulso. La
    // norma SDHCI manda pararlo, cambiarlo y volver a arrancarlo.
    EMMC_CONTROL1 &= ~(1u << 2);

    uint32_t control1 = ((divisor & 0xFF) << 8) | (((divisor >> 8) & 0x3) << 6);
    control1 |= (1 << 0);  // habilitar reloj interno
    EMMC_CONTROL1 = control1;

    // Esperar a que el reloj interno se estabilice (bit 1)
    uint32_t intentos = 1000000;
    while (!(EMMC_CONTROL1 & (1 << 1)) && intentos--) {}

    EMMC_CONTROL1 |= (1 << 2);  // habilitar el reloj hacia la tarjeta
}

// Variables de estado, rellenas por disk_init()
static uint32_t g_rca = 0;
static bool g_es_sdhc = false;
static bool g_disco_listo = false;
static uint64_t g_capacidad_sectores = 0;
// Donde empieza y acaba NemoFS, sacado de la tabla de particiones
typedef struct {
    uint8_t  tipo;
    uint32_t lba;
    uint32_t sectores;
} particion_t;

static particion_t g_tabla[4];      // la tabla de particiones leida, para el particionador
static bool g_tabla_leida = false;
static uint64_t g_nfs_inicio = SECTOR_OFFSET_NEMOFS;
static uint64_t g_nfs_sectores = 0;
static bool g_nfs_de_tabla = false;

static void guardar_rca_y_tipo(uint32_t rca, bool es_sdhc) {
    g_rca = rca;
    g_es_sdhc = es_sdhc;
    g_disco_listo = true;
}

// Lee y decodifica el registro CSD (CMD9) para obtener la
// capacidad real de la tarjeta. Debe llamarse mientras la tarjeta
// esta en estado "stand-by" (tras CMD3, antes de CMD7) -- llamarla
// despues de seleccionar la tarjeta no esta garantizado por el
// protocolo SD.
static uint64_t leer_capacidad_csd(uint32_t rca) {
    if (!emmc_comando(9, CMD_RESP_136, rca)) {
        uart_puts("leer_capacidad_csd: CMD9 fallo -- ");
        if (g_ultimo_fue_timeout) {
            uart_puts("timeout (ninguna respuesta llego a tiempo)\n");
        } else {
            uart_puts("error real, INTERRUPT=");
            uart_put_hex32(g_ultimo_interrupt_error);
            uart_puts("\n");
        }
        return 0;
    }

    // Convencion estandar en controladores SDHCI: la respuesta de
    // 128 bits utiles del CSD (sin el CRC final, que el propio
    // controlador ya descarta) queda repartida como
    // RESP3=[127:96], RESP2=[95:64], RESP1=[63:32], RESP0=[31:0].
    uint32_t r0 = EMMC_RESP0;
    uint32_t r1 = EMMC_RESP1;
    uint32_t r2 = EMMC_RESP2;
    uint32_t r3 = EMMC_RESP3;

    uart_puts("leer_capacidad_csd: r0=");
    uart_put_hex32(r0);
    uart_puts(" r1=");
    uart_put_hex32(r1);
    uart_puts(" r2=");
    uart_put_hex32(r2);
    uart_puts(" r3=");
    uart_put_hex32(r3);
    uart_puts("\n");

    // CSD_STRUCTURE: bits [127:126] -> dentro de r3, bits locales
    // [31:30] (global 96+30=126 hasta 96+31=127)
    // CORREGIDO: desplazamiento de 8 bits (ver nota completa mas
    // abajo, junto al calculo de C_SIZE) -- verificado con datos
    // reales, csd_version=1 (SDHC) coincide con esta tarjeta.
    uint32_t csd_version = (r3 >> 22) & 0x3;
    uart_puts("leer_capacidad_csd: csd_version=");
    uart_put_hex32(csd_version);
    uart_puts("\n");

    if (csd_version == 1) {
        // CSD version 2.0 -- tarjetas SDHC/SDXC (lo esperable para
        // cualquier tarjeta de mas de 2GB). Formula simple:
        // capacidad = (C_SIZE + 1) * 512 KB.
        //
        // CORREGIDO tras depuracion con datos reales: este
        // controlador (Arasan EMMC2 de la Pi 4) entrega la
        // respuesta de 136 bits SIN el byte de CRC final, lo que
        // desplaza todo el contenido 8 bits mas abajo de lo que
        // la convencion "estandar" SDHCI sugeriria -- confirmado
        // por un desarrollador real con el mismo sintoma exacto en
        // un foro de Raspberry Pi ("everything is 8 bits lower
        // than expected because there is no CRC byte included at
        // the end"), y verificado aqui mismo contra el tamaño real
        // de la tarjeta de prueba (15.6GB, coincide exacto).
        //
        // Con la correccion (CSD_bit(n) = physical_bit(n-8)),
        // C_SIZE [69:48] (22 bits) cae entero dentro de r1, en sus
        // bits locales [29:8] -- no repartido entre r1 y r2 como
        // en la formula original sin corregir.
        uint32_t c_size = (r1 >> 8) & 0x3FFFFF;

        uart_puts("leer_capacidad_csd: c_size=");
        uart_put_hex32(c_size);
        uart_puts("\n");

        // (C_SIZE+1) * 524288 bytes / 512 bytes por sector
        // = (C_SIZE+1) * 1024 sectores
        return (uint64_t)(c_size + 1) * 1024ULL;
    } else {
        // CSD version 1.0 -- tarjetas SDSC antiguas (menores de 2GB).
        // Misma correccion de 8 bits aplicada aqui (sin verificar
        // con una tarjeta real de este tipo, pero con la misma
        // logica ya confirmada arriba):
        //   READ_BL_LEN [83:80] -> physical[75:72] -> r2 bits[11:8]
        //   C_SIZE [73:62] -> physical[65:54] -> r2 bits[1:0] + r1 bits[31:22]
        //   C_SIZE_MULT [49:47] -> physical[41:39] -> r1 bits[9:7]
        uint32_t read_bl_len = (r2 >> 8) & 0xF;
        uint32_t c_size = ((r2 & 0x3) << 10) | ((r1 >> 22) & 0x3FF);
        uint32_t c_size_mult = (r1 >> 7) & 0x7;

        uint64_t capacidad_bytes = (uint64_t)(c_size + 1)
                                  * (1ULL << (c_size_mult + 2))
                                  * (1ULL << read_bl_len);
        return capacidad_bytes / 512ULL;
    }
}

static void localizar_particiones(void);
static bool sd_leer_fisico(uint64_t sector_fisico, void *buf);
static void sd_cache_vaciar(void);
static bool es_tipo_fat(uint8_t t);   // adelantada: la usa la decision de particiones

// ---------------------------------------------------------------------
// Tabla de particiones (MBR): lectura y decision, en funciones PURAS
// (no tocan hardware) para poder probarlas con tablas de mentira.
//
// Antes NemoFS empezaba SIEMPRE en el sector 1048576 (512
// MB), escrito a mano. Eso ataba el tamaño de la particion de arranque
// para siempre: agrandarla habria hecho que NemoFS escribiera ENCIMA de
// ella. Ahora el principio y el final de NemoFS salen de la tabla.
// ---------------------------------------------------------------------

// Devuelve cuantas entradas no vacias hay (0 si el MBR no es valido).
int disk_mbr_leer(const uint8_t *mbr, particion_t salida[4]) {
    for (int i = 0; i < 4; i++) { salida[i].tipo = 0; salida[i].lba = 0; salida[i].sectores = 0; }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;
    int n = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = &mbr[446 + 16 * i];
        salida[i].tipo = e[4];
        salida[i].lba  = (uint32_t)e[8]  | ((uint32_t)e[9] << 8)  | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        salida[i].sectores = (uint32_t)e[12] | ((uint32_t)e[13] << 8) | ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        if (salida[i].tipo != 0 && salida[i].sectores != 0) n++;
    }
    return n;
}

// Elige que particion es la FAT de arranque y cual la de NemoFS.
// Reglas, pensadas para no estropear ninguna tarjeta existente:
//   - la FAT es la primera de tipo FAT con sectores;
//   - NemoFS es la primera que NO es FAT, con sectores, y que empieza
//     detras de la FAT;
//   - si no hay ninguna asi, se usa el reparto de siempre (512 MB) para
//     que las tarjetas hechas antes de este cambio sigan funcionando;
//   - nada puede pasarse de la capacidad real de la tarjeta, y las dos
//     particiones no pueden solaparse: ante la duda, no se expone la FAT.
// Devuelve true si la informacion viene de la tabla; false si es el reparto de siempre.
bool disk_elegir_particiones(const particion_t *p, int cuantas, uint64_t capacidad,
                             particion_t *fat, particion_t *nfs) {
    fat->tipo = 0; fat->lba = 0; fat->sectores = 0;
    nfs->tipo = 0; nfs->lba = 0; nfs->sectores = 0;
    if (cuantas > 0) {
        for (int i = 0; i < 4; i++) {
            if (p[i].sectores == 0 || p[i].tipo == 0) continue;
            if ((uint64_t)p[i].lba + p[i].sectores > capacidad) continue;   // no cabe en la tarjeta
            if (fat->sectores == 0 && es_tipo_fat(p[i].tipo)) { *fat = p[i]; continue; }
            if (nfs->sectores == 0 && !es_tipo_fat(p[i].tipo)) *nfs = p[i];
        }
        if (nfs->sectores && fat->sectores && nfs->lba < fat->lba + fat->sectores) {
            nfs->sectores = 0;               // se solapan: no fiarse de la tabla
        }
        if (nfs->sectores) {
            if (fat->sectores && (uint64_t)fat->lba + fat->sectores > nfs->lba) fat->sectores = 0;
            return true;
        }
    }
    // Reparto de siempre: NemoFS desde los 512 MB hasta el final
    nfs->tipo = 0x7F;
    nfs->lba = (uint32_t)SECTOR_OFFSET_NEMOFS;
    nfs->sectores = (capacidad > SECTOR_OFFSET_NEMOFS)
                  ? (uint32_t)(capacidad - SECTOR_OFFSET_NEMOFS) : 0;
    if (fat->sectores && (uint64_t)fat->lba + fat->sectores > SECTOR_OFFSET_NEMOFS) fat->sectores = 0;
    return false;
}


// ---------------------------------------------------------------------
// MODO DE ALTA VELOCIDAD
//
// Una tarjeta SD arranca en "velocidad normal": 25 MHz como maximo.
// Casi todas admiten ademas "alta velocidad", que es el doble, 50 MHz,
// y se pide con CMD6 (SWITCH_FUNC). Duplicar el reloj duplica lo que
// se lee por segundo, sin cambiar nada mas.
//
// El peligro esta claro: si subimos el reloj del anfitrion y la
// tarjeta no ha cambiado de verdad, lo que se lee es basura -- y
// basura que nadie comprueba, porque un sector de datos no lleva
// firma. Por eso aqui hay TRES cierres, no uno:
//
//   1. Se pregunta primero (CMD6 en modo consulta, que no cambia
//      nada) si la funcion existe en esta tarjeta.
//   2. Se pide el cambio y se mira la respuesta: la tarjeta dice en
//      que modo se ha quedado, y solo si dice "alta velocidad" se
//      toca el reloj.
//   3. Y aun asi, se LEE UN SECTOR Y SE COMPARA con lo que ese mismo
//      sector daba a 25 MHz. Si no coincide, se vuelve atras.
//
// El tercero es el que de verdad importa: convierte "la tarjeta deja
// de responder" en "sigue a 25 MHz y lo dice por la UART".
//
// Si alguna tarjeta diera guerra, poner esto a 0 y recompilar deja
// todo como estaba, sin tocar nada mas.
#define SD_ALTA_VELOCIDAD 1

static bool g_alta_velocidad = false;

// CMD6 (SWITCH_FUNC) devuelve 64 bytes por las lineas de datos.
// consulta=true solo pregunta; consulta=false cambia de verdad.
// Ojo: esto es CMD6 A SECAS. El CMD6 precedido de CMD55 (el ACMD6 de
// mas arriba) es otro comando distinto, el del ancho del bus.
static bool sd_switch_func(bool consulta, uint8_t estado[64]) {
    uint32_t arg = (consulta ? 0x00000000u : 0x80000000u)  // bit 31: 0 preguntar, 1 cambiar
                 | 0x00FFFFF1u;                            // grupo 1 = 1 (alta velocidad); el resto, sin tocar
    EMMC_BLKSIZECNT = (1 << 16) | 64;
    if (!emmc_comando(6, CMD_RESP_48 | CMD_ISDATA | TM_DAT_DIR_READ, arg)) return false;
    if (!esperar_interrupt(INT_READ_RDY, 1000000)) return false;
    uint32_t *p = (uint32_t *)estado;
    for (int i = 0; i < 16; i++) p[i] = EMMC_DATA;
    return esperar_interrupt(INT_DATA_DONE, 1000000);
}

// Las dos decisiones que deciden si se toca el reloj, aparte y sin
// hardware de por medio, para poder comprobarlas con respuestas
// fabricadas. Equivocarse en el byte o en el bit no daria un error
// visible: daria una tarjeta corriendo al doble sin haber cambiado,
// que es justo lo que no puede pasar.
//
// Los 64 bytes de CMD6 llegan con el bit 511 de la norma primero. Las
// cuentas salen asi:
//   - "que admite el grupo 1" son los bits 415..400 -> bytes 12 y 13,
//     y dentro del 13 el bit 1 es la alta velocidad.
//   - "en que se ha quedado el grupo 1" son los bits 379..376 -> el
//     nibble bajo del byte 16; vale 1 si cambio.
static bool sd_hs_ofrecida(const uint8_t estado[64]) {
    return (estado[13] & 0x02) != 0;
}
static bool sd_hs_aceptada(const uint8_t estado[64]) {
    return (estado[16] & 0x0F) == 0x01;
}

static void intentar_alta_velocidad(uint32_t freq_base) {
#if !SD_ALTA_VELOCIDAD
    (void)freq_base;
    uart_puts("disk_pi4: alta velocidad desactivada al compilar; 25 MHz.\n");
#else
    __attribute__((aligned(8))) static uint8_t estado[64];
    __attribute__((aligned(8))) static uint8_t testigo[512];
    __attribute__((aligned(8))) static uint8_t comprobar[512];

    // El testigo: un sector leido a 25 MHz, que ya sabemos bueno.
    if (!sd_leer_fisico(0, testigo)) {
        uart_puts("disk_pi4: no se pudo leer el sector de referencia; me quedo a 25 MHz.\n");
        return;
    }

    // 1. Preguntar. De los 64 bytes, el 13 dice que funciones admite
    //    el grupo 1; el bit 1 es la alta velocidad.
    if (!sd_switch_func(true, estado) || !sd_hs_ofrecida(estado)) {
        uart_puts("disk_pi4: la tarjeta no ofrece alta velocidad; 25 MHz.\n");
        return;
    }

    // 2. Pedir el cambio. La tarjeta contesta en el nibble bajo del
    //    byte 16 en que funcion del grupo 1 se ha quedado: 1 = alta
    //    velocidad. Cualquier otra cosa significa que NO cambio.
    if (!sd_switch_func(false, estado) || !sd_hs_aceptada(estado)) {
        uart_puts("disk_pi4: la tarjeta no acepto el cambio; 25 MHz.\n");
        return;
    }

    // Ahora si: el anfitrion al mismo paso que la tarjeta.
    EMMC_CONTROL0 |= HCTL_ALTA_VELOCIDAD;
    establecer_reloj(freq_base, 50000000);

    // 3. La prueba de verdad: releer el mismo sector y compararlo.
    //    Se va al disco a proposito, sin pasar por la cache.
    bool bien = sd_leer_fisico(0, comprobar);
    if (bien) {
        for (int i = 0; i < 512; i++) {
            if (comprobar[i] != testigo[i]) { bien = false; break; }
        }
    }
    if (!bien) {
        // Marcha atras: reloj y bit como estaban. Mejor lento que mal.
        establecer_reloj(freq_base, 25000000);
        EMMC_CONTROL0 &= ~HCTL_ALTA_VELOCIDAD;
        sd_cache_vaciar();          // por si algo entro mal mientras tanto
        uart_puts("disk_pi4: a 50 MHz la tarjeta lee mal; vuelvo a 25 MHz.\n");
        return;
    }

    g_alta_velocidad = true;
    uart_puts("disk_pi4: alta velocidad, 50 MHz.\n");
#endif
}

bool disk_init(void) {
    // 1. Frecuencia base real, por mailbox -- nunca asumida.
    uint32_t freq_base = obtener_frecuencia_base();
    if (freq_base == 0) {
        return false;  // sin frecuencia, no hay forma de continuar con seguridad
    }

    // 2. Reset por software del controlador completo.
    EMMC_CONTROL0 = 0;
    EMMC_CONTROL1 |= (1 << 24);  // reset completo
    uint32_t intentos = 1000000;
    while ((EMMC_CONTROL1 & (1 << 24)) && intentos--) {}
    if (intentos == 0) return false;

    // 3. Reloj de identificacion, lento (400 KHz), para la
    //    negociacion inicial con la tarjeta -- el mismo valor
    //    estandar que usa cualquier controlador SD/MMC del mundo
    //    para esta fase, independiente del modelo de Pi.
    establecer_reloj(freq_base, 400000);

    // 4. Activar 3.3V explicitamente -- el detalle real que la
    //    Pi 4 exige y los modelos anteriores no (confirmado por
    //    el parche de barebox referenciado arriba).
    EMMC_CONTROL0 |= (0x0F << 8);

    // 5. Habilitar las interrupciones que usamos, enmascarando el
    //    resto -- sondeamos por registro, no por IRQ real todavia.
    EMMC_IRPT_EN = 0xFFFFFFFF;
    EMMC_IRPT_MASK = 0xFFFFFFFF;

    // --- Secuencia de inicializacion SD estandar ---
    // CMD0: GO_IDLE_STATE -- reiniciar la tarjeta a su estado inicial
    if (!emmc_comando(0, CMD_RESP_NONE, 0)) return false;

    // CMD8: SEND_IF_COND -- comprueba tension y confirma que es una
    // tarjeta SD version 2.0 o posterior (0x1AA = 3.3V + patron de verificacion)
    bool es_sdv2 = emmc_comando(8, CMD_RESP_48 | CMD_CRCCHK_EN, 0x1AA);
    if (es_sdv2 && (EMMC_RESP0 & 0xFFF) != 0x1AA) {
        return false;  // tarjeta respondio pero con un patron distinto -- algo raro
    }

    // ACMD41 (CMD55 + CMD41): negociacion de tension y espera a que
    // la tarjeta este lista -- puede tardar, se reintenta en bucle.
    uint32_t ocr = 0;
    bool lista = false;
    for (int i = 0; i < 1000 && !lista; i++) {
        if (!emmc_comando(55, CMD_RESP_48, 0)) continue;  // CMD_APP
        uint32_t arg_acmd41 = 0x00FF8000;  // rango de voltaje 3.2-3.4V
        if (es_sdv2) arg_acmd41 |= (1 << 30);  // HCS: admite tarjetas SDHC/SDXC
        if (!emmc_comando(41, CMD_RESP_48, arg_acmd41)) continue;
        ocr = EMMC_RESP0;
        if (ocr & 0x80000000) lista = true;  // bit de "busy" a 0 = lista
    }
    if (!lista) return false;

    bool es_sdhc = (ocr & 0x40000000) != 0;

    // CMD2: ALL_SEND_CID -- identificador unico de la tarjeta (no lo usamos, solo lo pedimos)
    if (!emmc_comando(2, CMD_RESP_136, 0)) return false;

    // CMD3: SEND_RELATIVE_ADDR -- la tarjeta nos asigna su RCA (direccion en el bus)
    if (!emmc_comando(3, CMD_RESP_48, 0)) return false;
    uint32_t rca = EMMC_RESP0 & 0xFFFF0000;

    // CMD9: SEND_CSD -- leemos la capacidad AQUI, mientras la
    // tarjeta sigue en estado "stand-by" (tras CMD3, antes de
    // CMD7). Segun el protocolo SD, CMD9 se espera en este estado,
    // no despues de seleccionar la tarjeta con CMD7 -- de ahi que
    // esta lectura ocurra aqui y se guarde en cache, en vez de
    // pedirse mas tarde bajo demanda.
    g_capacidad_sectores = leer_capacidad_csd(rca);

    // CORRECCION DE SEGURIDAD: si no se pudo leer la capacidad,
    // detener la inicializacion aqui mismo -- devolver el disco
    // como "listo" con capacidad 0 o incorrecta es un modo de
    // fallo peligroso: podria dejar que NemoFS formatee o escriba
    // sin ningun limite real conocido. Mejor fallar limpiamente
    // (disk_init devuelve false) y que kernel.c se salte todo el
    // bloque de NemoFS con total seguridad, tal como ya hace si no
    // hay disco en absoluto.
    if (g_capacidad_sectores == 0) {
        uart_puts("disk_pi4: fallo leyendo CSD (CMD9) -- "
                  "abortando inicializacion por seguridad.\n");
        return false;
    }

    // CMD7: SELECT_CARD -- seleccionar esta tarjeta para operaciones posteriores
    if (!emmc_comando(7, CMD_RESP_48_BUSY, rca)) return false;

    // ACMD6: SET_BUS_WIDTH -- pasar a cuatro lineas de datos.
    //
    // Hasta aqui la tarjeta venia hablando por UNA sola linea, que es
    // como arranca toda tarjeta SD por norma. El conector tiene cuatro,
    // y usarlas cuesta dos comandos: se lo pedimos a la tarjeta y, solo
    // si acepta, movemos el anfitrion al mismo ancho. Si algo falla nos
    // quedamos en una linea -- mas lento, pero correcto; lo que no se
    // puede es que uno de los dos cambie y el otro no.
    //
    // Esto faltaba, y era el motivo principal de que la Pi 4 fuera
    // cuatro veces mas lenta de lo que le toca leyendo de la tarjeta.
    bool cuatro_bits = false;
    if (emmc_comando(55, CMD_RESP_48, rca) &&       // CMD_APP, dirigido a esta tarjeta
        emmc_comando(6, CMD_RESP_48, 0x02)) {       // 0x02 = bus de 4 bits
        EMMC_CONTROL0 |= HCTL_ANCHO_4BITS;
        cuatro_bits = true;
    }
    uart_puts(cuatro_bits ? "disk_pi4: bus de datos de 4 bits.\n"
                          : "disk_pi4: la tarjeta no acepto 4 bits; sigo con 1.\n");

    // Subir la velocidad de reloj a algo mas razonable ahora que la
    // tarjeta esta inicializada -- 25 MHz, el maximo del modo por
    // defecto (no de alta velocidad).
    establecer_reloj(freq_base, 25000000);

    guardar_rca_y_tipo(rca, es_sdhc);

    // Y, si la tarjeta lo admite, el doble: 50 MHz. Va despues de
    // guardar_rca_y_tipo() a proposito, porque la comprobacion de
    // seguridad de ahi dentro necesita poder leer sectores, y para eso
    // el disco tiene que estar dado por listo.
    intentar_alta_velocidad(freq_base);

    localizar_particiones();
    return true;
}

// ---------------------------------------------------------------------
// Acceso fisico (sector absoluto de la tarjeta)
// ---------------------------------------------------------------------
static bool sd_leer_fisico(uint64_t sector_fisico, void *buf) {
    uint32_t direccion = g_es_sdhc ? (uint32_t)sector_fisico
                                     : (uint32_t)(sector_fisico * 512);
    EMMC_BLKSIZECNT = (1 << 16) | 512;
    if (!emmc_comando(17, CMD_RESP_48 | CMD_ISDATA | TM_DAT_DIR_READ, direccion)) return false;
    if (!esperar_interrupt(INT_READ_RDY, 1000000)) return false;
    uint32_t *destino = (uint32_t *)buf;
    for (int i = 0; i < 128; i++) destino[i] = EMMC_DATA;
    return esperar_interrupt(INT_DATA_DONE, 1000000);
}

// Varios sectores SEGUIDOS con un solo comando (CMD18, READ_MULTIPLE).
//
// La diferencia con llamar n veces a sd_leer_fisico() no son los datos
// -- esos viajan igual -- sino el ida y vuelta: un CMD17 es mandar el
// comando, esperar su respuesta, esperar el bloque y esperar el fin,
// cuatro sondeos por cada 512 bytes. Con CMD18 eso se paga UNA vez y
// luego la tarjeta va soltando bloque tras bloque.
//
// TM_AUTO_CMD12 hace que el propio controlador mande el CMD12 de
// "para" al llegar al ultimo bloque. Es importante que lo haga el
// hardware: si la tarjeta se queda esperando a que alguien la pare,
// bloquea la linea de datos y el siguiente comando no entra.
#define SD_MAX_BLOQUES 64      // 32 KB por comando; mas no compensa

static bool sd_leer_fisico_n(uint64_t sector_fisico, uint32_t n, void *buf) {
    if (n == 0) return true;
    if (n == 1) return sd_leer_fisico(sector_fisico, buf);
    if (n > SD_MAX_BLOQUES) n = SD_MAX_BLOQUES;

    uint32_t direccion = g_es_sdhc ? (uint32_t)sector_fisico
                                   : (uint32_t)(sector_fisico * 512);
    EMMC_BLKSIZECNT = (n << 16) | 512;
    uint32_t flags = CMD_RESP_48 | CMD_ISDATA | TM_DAT_DIR_READ
                   | TM_BLKCNT_EN | TM_MULTI_BLOQUE | TM_AUTO_CMD12;
    if (!emmc_comando(18, flags, direccion)) return false;

    uint32_t *destino = (uint32_t *)buf;
    for (uint32_t b = 0; b < n; b++) {
        if (!esperar_interrupt(INT_READ_RDY, 1000000)) return false;
        for (int i = 0; i < 128; i++) destino[b * 128 + i] = EMMC_DATA;
    }
    return esperar_interrupt(INT_DATA_DONE, 1000000);
}

// ---------------------------------------------------------------------
// Cache de lectura de la tarjeta SD
//
// Por que de SECTORES SUELTOS y no por lotes, como la del pendrive:
// se midio. Al abrir una carpeta de imagenes, el Navegante pide UNA
// FILA de cada imagen por miniatura, y cada peticion vuelve a leer el
// inodo y el bloque de indices del archivo. De 4.826 lecturas, 1.842
// eran sectores leidos hacia un instante -- siempre los mismos pocos.
// Una cache por lotes de 8 arreglaba eso pero traia 8 sectores para
// usar 2, y acababa moviendo el doble de datos: mas lenta que no tener
// nada. Guardando sectores sueltos se quitan esas 1.842 sin traer ni
// un byte de mas.
//
// Se echa fuera el menos usado hace mas tiempo, no el mas viejo: el
// sector del inodo se pide en cada fila, asi que tiene que sobrevivir
// aunque entre medias pasen cientos de sectores de pixeles.
//
// Son 16 huecos = 8 KB. Con 8 ya se lograba casi todo el ahorro; 16
// deja margen para que el explorador y una aplicacion trabajen a la
// vez sin echarse los sectores el uno al otro.
#define SD_CACHE_VIAS 16

__attribute__((aligned(8)))
static uint8_t  sd_cache_datos[SD_CACHE_VIAS][512];
static uint64_t sd_cache_sector[SD_CACHE_VIAS];
static bool     sd_cache_valido[SD_CACHE_VIAS];
static uint8_t  sd_cache_orden[SD_CACHE_VIAS];   // orden[0] = el usado mas recientemente
static bool     sd_cache_lista = false;

static void sd_cache_vaciar(void) {
    for (int i = 0; i < SD_CACHE_VIAS; i++) {
        sd_cache_valido[i] = false;
        sd_cache_orden[i] = (uint8_t)i;
    }
    sd_cache_lista = true;
}

// Mueve un hueco al frente de la lista de uso. La lista tiene 16
// elementos: moverla entera es mas barato que cualquier estructura
// mas lista, y no hay que reservar memoria.
static void sd_cache_al_frente(int hueco) {
    int donde = 0;
    for (int i = 0; i < SD_CACHE_VIAS; i++) {
        if (sd_cache_orden[i] == (uint8_t)hueco) { donde = i; break; }
    }
    for (int i = donde; i > 0; i--) sd_cache_orden[i] = sd_cache_orden[i - 1];
    sd_cache_orden[0] = (uint8_t)hueco;
}

static int sd_cache_buscar(uint64_t sector) {
    if (!sd_cache_lista) { sd_cache_vaciar(); return -1; }
    for (int i = 0; i < SD_CACHE_VIAS; i++) {
        if (sd_cache_valido[i] && sd_cache_sector[i] == sector) return i;
    }
    return -1;
}

static void sd_cache_meter(uint64_t sector, const void *datos) {
    if (!sd_cache_lista) sd_cache_vaciar();
    int hueco = sd_cache_orden[SD_CACHE_VIAS - 1];   // el menos usado
    sd_cache_sector[hueco] = sector;
    sd_cache_valido[hueco] = true;
    const uint32_t *o = (const uint32_t *)datos;
    uint32_t *d = (uint32_t *)sd_cache_datos[hueco];
    for (int i = 0; i < 128; i++) d[i] = o[i];
    sd_cache_al_frente(hueco);
}

// Una escritura deja lo guardado obsoleto. Como ya tenemos los datos
// nuevos en la mano, se refresca el hueco en vez de tirarlo: una
// relectura inmediata (que es lo normal -- NemoFS lee el sector del
// inodo justo despues de escribirlo) sale de memoria y no del disco.
static void sd_cache_refrescar(uint64_t sector, const void *datos) {
    int hueco = sd_cache_buscar(sector);
    if (hueco < 0) return;
    const uint32_t *o = (const uint32_t *)datos;
    uint32_t *d = (uint32_t *)sd_cache_datos[hueco];
    for (int i = 0; i < 128; i++) d[i] = o[i];
    sd_cache_al_frente(hueco);
}

// De donde saca la cache los sectores que no tiene. En la Pi 4, de la
// tarjeta. Una prueba de escritorio lo apunta a un disco de mentira y
// asi puede comprobar que la cache devuelve el sector correcto y echa
// fuera al que toca, sin necesidad de hardware ni de tarjeta.
#ifndef SD_LEER_UNO
#define SD_LEER_UNO(sector, buf)        sd_leer_fisico((sector), (buf))
#endif
#ifndef SD_LEER_TIRADA
#define SD_LEER_TIRADA(sector, n, buf)  sd_leer_fisico_n((sector), (n), (buf))
#endif
#ifndef SD_ESCRIBIR_UNO
#define SD_ESCRIBIR_UNO(sector, buf)    sd_escribir_fisico((sector), (buf))
#endif

static bool sd_leer_cacheado(uint64_t sector, void *buf) {
    int hueco = sd_cache_buscar(sector);
    if (hueco >= 0) {
        const uint32_t *o = (const uint32_t *)sd_cache_datos[hueco];
        uint32_t *d = (uint32_t *)buf;
        for (int i = 0; i < 128; i++) d[i] = o[i];
        sd_cache_al_frente(hueco);
        return true;
    }
    if (!SD_LEER_UNO(sector, buf)) return false;
    sd_cache_meter(sector, buf);
    return true;
}

// Lectura de una tirada de sectores seguidos. Va por CMD18 y NO pasa
// por la cache a proposito: son los pixeles de un archivo, que se leen
// una vez y no se vuelven a pedir. Meterlos echaria fuera justo los
// sectores de inodos e indices que si se repiten.
static bool sd_leer_varios(uint64_t sector, uint32_t n, void *buf) {
    uint8_t *dst = (uint8_t *)buf;
    while (n > 0) {
        uint32_t trozo = (n > SD_MAX_BLOQUES) ? SD_MAX_BLOQUES : n;
        // Un sector suelto sí pasa por la cache: suele ser el final de
        // una tirada, y con un solo bloque el CMD18 no aporta nada.
        if (trozo > 1) {
            if (!SD_LEER_TIRADA(sector, trozo, dst)) return false;
        } else {
            if (!sd_leer_cacheado(sector, dst)) return false;
        }
        sector += trozo; dst += trozo * 512; n -= trozo;
    }
    return true;
}

static bool sd_escribir_fisico(uint64_t sector_fisico, const void *buf) {
    uint32_t direccion = g_es_sdhc ? (uint32_t)sector_fisico
                                     : (uint32_t)(sector_fisico * 512);
    EMMC_BLKSIZECNT = (1 << 16) | 512;
    if (!emmc_comando(24, CMD_RESP_48 | CMD_ISDATA, direccion)) return false;
    if (!esperar_interrupt(INT_WRITE_RDY, 1000000)) return false;
    const uint32_t *origen = (const uint32_t *)buf;
    for (int i = 0; i < 128; i++) EMMC_DATA = origen[i];
    return esperar_interrupt(INT_DATA_DONE, 1000000);
}

// ---------------------------------------------------------------------
// Dos discos logicos sobre la misma tarjeta:
//   disco 0 = NemoFS, en sectores crudos desde SECTOR_OFFSET_NEMOFS
//   disco 1 = la particion FAT de arranque (la misma que ve el Mac),
//             localizada en la tabla de particiones (MBR, sector 0)
// ---------------------------------------------------------------------
static bool     g_fat_ok = false;
static uint64_t g_fat_inicio = 0;      // LBA del primer sector de la particion
static uint64_t g_fat_sectores = 0;

static bool es_tipo_fat(uint8_t t) {
    return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C || t == 0x0E;
}

// Lee el MBR y elige la primera particion FAT como disco 1, siempre que
// termine antes de la zona de NemoFS (si no, no se expone: proteger
// los datos vale mas que tener el segundo disco).
static void localizar_particiones(void) {
    static uint8_t mbr[512] __attribute__((aligned(16)));
    particion_t tabla[4], fat, nfs;
    int cuantas = 0;

    if (!sd_leer_fisico(0, mbr)) {
        uart_puts("disk_pi4: no se pudo leer el MBR\n");
    } else {
        cuantas = disk_mbr_leer(mbr, tabla);
        for (int k = 0; k < 4; k++) g_tabla[k] = tabla[k];
        g_tabla_leida = (cuantas > 0);
        if (cuantas == 0) uart_puts("disk_pi4: MBR sin firma 55AA o sin particiones\n");
        for (int i2 = 0; i2 < 4; i2++) {
            if (tabla[i2].tipo == 0 || tabla[i2].sectores == 0) continue;
            uart_puts("disk_pi4: particion "); uart_putc((char)('1' + i2));
            uart_puts(": tipo="); uart_put_hex32(tabla[i2].tipo);
            uart_puts(" inicio="); uart_put_hex32(tabla[i2].lba);
            uart_puts(" sectores="); uart_put_hex32(tabla[i2].sectores);
            uart_puts("\n");
        }
    }

    g_nfs_de_tabla = disk_elegir_particiones(tabla, cuantas, g_capacidad_sectores, &fat, &nfs);
    g_nfs_inicio = nfs.lba;
    g_nfs_sectores = nfs.sectores;
    if (fat.sectores) { g_fat_inicio = fat.lba; g_fat_sectores = fat.sectores; g_fat_ok = true; }

    uart_puts(g_nfs_de_tabla ? "disk_pi4: NemoFS segun la tabla de particiones -- inicio="
                             : "disk_pi4: NemoFS con el reparto de siempre (512MB) -- inicio=");
    uart_put_hex32((uint32_t)g_nfs_inicio);
    uart_puts(" sectores="); uart_put_hex32((uint32_t)g_nfs_sectores);
    uart_puts(" ("); uart_put_hex32((uint32_t)(g_nfs_sectores / 2048)); uart_puts(" MB)\n");
    if (g_fat_ok) {
        uart_puts("disk_pi4: disco 1 = particion FAT de la SD (");
        uart_put_hex32((uint32_t)(g_fat_sectores / 2048));
        uart_puts(" MB).\n");
    } else {
        uart_puts("disk_pi4: sin particion FAT utilizable en la SD\n");
    }
}

// ---------------------------------------------------------------------
// Disco 2 = pendrive USB (xhci_msd_*). La FAT del sistema es una sola:
// si hay pendrive se monta en el; si no, en la particion de la SD.
// ---------------------------------------------------------------------
static bool     g_usb_ok = false;
static uint64_t g_usb_inicio = 0, g_usb_sectores = 0;

// Cache de lectura adelantada para el pendrive: la mayoria de accesos
// de FAT son secuenciales o muy locales (leer un archivo, recorrer la
// cadena de clusters, listar un directorio), asi que traer de golpe
// XHCI_MSD_MAX_SECTORES sectores en vez de leer uno a uno acelera
// mucho sin tocar fat.c. Solo lectura: las escrituras van directas al
// dispositivo (write-through) y refrescan la parte de la cache que
// coincida, para no servir datos viejos si se relee justo despues.
__attribute__((aligned(4096))) static uint8_t usb_cache[XHCI_MSD_MAX_SECTORES * 512];
static uint64_t usb_cache_inicio = 0;   // LBA absoluto (del dispositivo) del primer sector cacheado
static bool     usb_cache_valido = false;

static bool usb_leer_cacheado(uint64_t abs_lba, void *buf) {
    if (usb_cache_valido && abs_lba >= usb_cache_inicio && abs_lba < usb_cache_inicio + XHCI_MSD_MAX_SECTORES) {
        const uint8_t *src = usb_cache + (abs_lba - usb_cache_inicio) * 512;
        for (int i = 0; i < 512; i++) ((uint8_t *)buf)[i] = src[i];
        return true;
    }
    uint32_t n = XHCI_MSD_MAX_SECTORES;
    uint64_t total = xhci_msd_sectores();
    if (abs_lba + n > total) n = (uint32_t)(total - abs_lba);
    if (n == 0) return false;
    if (!xhci_msd_leer_n(abs_lba, n, usb_cache)) {
        usb_cache_valido = false;
        return xhci_msd_leer(abs_lba, buf);   // el lote fallo: probar sector suelto antes de rendirse
    }
    usb_cache_inicio = abs_lba;
    usb_cache_valido = true;
    for (int i = 0; i < 512; i++) ((uint8_t *)buf)[i] = usb_cache[i];
    return true;
}

static bool usb_escribir_directo(uint64_t abs_lba, const void *buf) {
    if (!xhci_msd_escribir(abs_lba, buf)) return false;
    // Write-through: si el sector escrito cae dentro de la ventana ya
    // cacheada, refrescar esa parte para que una lectura inmediata
    // posterior (p.ej. "escribir y releer para comprobar") no sirva el
    // contenido antiguo desde la cache.
    if (usb_cache_valido && abs_lba >= usb_cache_inicio && abs_lba < usb_cache_inicio + XHCI_MSD_MAX_SECTORES) {
        uint8_t *dst = usb_cache + (abs_lba - usb_cache_inicio) * 512;
        for (int i = 0; i < 512; i++) dst[i] = ((const uint8_t *)buf)[i];
    }
    return true;
}

// Los pendrives suelen traer MBR + una particion FAT; algunos son una
// FAT "a pelo" desde el sector 0. Se distinguen mirando el sector 0.
static void localizar_fat_usb(void) {
    static uint8_t s0[512] __attribute__((aligned(16)));
    g_usb_ok = false;
    if (!xhci_msd_leer(0, s0)) { uart_puts("disk_pi4: pendrive: no se pudo leer el sector 0\n"); return; }
    if (s0[510] != 0x55 || s0[511] != 0xAA) { uart_puts("disk_pi4: pendrive: sector 0 sin firma 55AA\n"); return; }
    // ¿Tabla de particiones con alguna entrada plausible?
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = &s0[446 + 16 * i];
        uint8_t tipo = e[4];
        uint32_t lba = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t n   = e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24);
        if (tipo && es_tipo_fat(tipo) && lba && n && (uint64_t)lba + n <= xhci_msd_sectores()) {
            g_usb_inicio = lba; g_usb_sectores = n; g_usb_ok = true;
            uart_puts("disk_pi4: pendrive: particion FAT en LBA "); uart_put_hex32(lba);
            uart_puts(", "); uart_put_hex32((uint32_t)(n / 2048)); uart_puts(" MB\n");
            return;
        }
    }
    // Sin tabla valida: ¿es un sector de arranque FAT directamente?
    if ((s0[0] == 0xEB || s0[0] == 0xE9) && (s0[11] | (s0[12] << 8)) == 512) {
        g_usb_inicio = 0; g_usb_sectores = xhci_msd_sectores(); g_usb_ok = true;
        uart_puts("disk_pi4: pendrive: FAT sin tabla de particiones\n");
        return;
    }
    uart_puts("disk_pi4: pendrive: sin particion FAT reconocible (exFAT/NTFS? formatealo en FAT32)\n");
}

// Llamar periodicamente (desde input_poll): reacciona a conexion o
// retirada del pendrive remontando la FAT donde corresponda.
void disk_pi4_actualizar_usb(void) {
    int c = xhci_msd_cambio();
    if (c != 0) usb_cache_valido = false;   // dispositivo distinto: la cache vieja ya no vale
    if (c == 1) {
        localizar_fat_usb();
        if (g_usb_ok) {
            if (fat_mount(2)) uart_puts("disk_pi4: FAT montada en el PENDRIVE (disco 2).\n");
            else { uart_puts("disk_pi4: fat_mount(2) fallo; la FAT sigue en la SD.\n"); g_usb_ok = false; }
        }
    } else if (c == 2) {
        if (g_usb_ok) {
            g_usb_ok = false;
            if (g_fat_ok && fat_mount(1)) uart_puts("disk_pi4: pendrive retirado: FAT de vuelta a la SD (disco 1).\n");
        }
    }
}

uint32_t disk_count(void) {
    if (!g_disco_listo) return 0;
    return (g_fat_ok ? 2 : 1) + (g_usb_ok ? 1 : 0);
}

bool disk_read_sector_n(uint8_t disk, uint64_t sector, void *buf) {
    if (!g_disco_listo) return false;
    if (disk == 0) return (sector < g_nfs_sectores) && sd_leer_cacheado(sector + g_nfs_inicio, buf);
    if (disk == 1 && g_fat_ok && sector < g_fat_sectores) return sd_leer_cacheado(g_fat_inicio + sector, buf);
    if (disk == 2 && g_usb_ok && sector < g_usb_sectores) return usb_leer_cacheado(g_usb_inicio + sector, buf);
    return false;
}

// Varios sectores seguidos de una vez. El sistema de
// archivos la usa cuando ve que los bloques de un archivo van
// corridos, que es lo normal: asi un archivo entero entra en unos
// pocos comandos en vez de uno por cada 512 bytes.
bool disk_read_sectors_n(uint8_t disk, uint64_t sector, uint32_t n, void *buf) {
    if (!g_disco_listo || n == 0) return false;
    if (disk == 0)
        return (sector + n <= g_nfs_sectores) && sd_leer_varios(sector + g_nfs_inicio, n, buf);
    if (disk == 1 && g_fat_ok && sector + n <= g_fat_sectores)
        return sd_leer_varios(g_fat_inicio + sector, n, buf);
    if (disk == 2 && g_usb_ok && sector + n <= g_usb_sectores) {
        // El pendrive ya tiene lo suyo: su cache lee por lotes.
        uint8_t *dst = (uint8_t *)buf;
        for (uint32_t i = 0; i < n; i++) {
            if (!usb_leer_cacheado(g_usb_inicio + sector + i, dst + i * 512)) return false;
        }
        return true;
    }
    return false;
}

bool disk_write_sector_n(uint8_t disk, uint64_t sector, const void *buf) {
    if (!g_disco_listo) return false;
    // Tras escribir se refresca lo guardado: una relectura inmediata
    // (NemoFS lee el sector del inodo justo despues de escribirlo)
    // sale de memoria, y sobre todo NO puede devolver lo de antes.
    if (disk == 0) {
        if (sector >= g_nfs_sectores) return false;
        uint64_t s = sector + g_nfs_inicio;
        if (!SD_ESCRIBIR_UNO(s, buf)) return false;
        sd_cache_refrescar(s, buf);
        return true;
    }
    if (disk == 1 && g_fat_ok && sector < g_fat_sectores) {
        uint64_t s = g_fat_inicio + sector;
        if (!SD_ESCRIBIR_UNO(s, buf)) return false;
        sd_cache_refrescar(s, buf);
        return true;
    }
    if (disk == 2 && g_usb_ok && sector < g_usb_sectores) return usb_escribir_directo(g_usb_inicio + sector, buf);
    return false;
}

bool disk_particion(int indice, uint8_t *tipo, uint64_t *inicio, uint64_t *sectores) {
    if (indice < 0 || indice > 3 || !g_tabla_leida) return false;
    if (g_tabla[indice].tipo == 0 || g_tabla[indice].sectores == 0) return false;
    if (tipo) *tipo = g_tabla[indice].tipo;
    if (inicio) *inicio = g_tabla[indice].lba;
    if (sectores) *sectores = g_tabla[indice].sectores;
    return true;
}

uint64_t disk_capacidad_fisica(void) { return g_capacidad_sectores; }

// ---- Estirar la particion de NemoFS ----
//
// Esto ESCRIBE en el sector 0, que es el unico sitio del disco donde un
// error deja la tarjeta inservible. De ahi las comprobaciones: la
// particion de arranque no se toca NUNCA, no se hace nada si hay algo
// detras de NemoFS, y antes de escribir se guarda una copia del sector 0
// en la propia particion de arranque (MBR.BAK).
int disk_estirar_nemofs(uint64_t *sectores_nuevos) {
    static uint8_t mbr[512] __attribute__((aligned(16)));
    if (sectores_nuevos) *sectores_nuevos = 0;
    if (!g_nfs_de_tabla || g_nfs_sectores == 0) return 2;       // sin tabla fiable, no se toca
    if (!sd_leer_fisico(0, mbr)) return 4;

    particion_t tabla[4];
    if (disk_mbr_leer(mbr, tabla) == 0) return 2;

    int cual = -1;
    for (int i = 0; i < 4; i++) {
        if (tabla[i].sectores == 0) continue;
        if (tabla[i].lba == (uint32_t)g_nfs_inicio) cual = i;    // la de NemoFS
        else if ((uint64_t)tabla[i].lba >= g_nfs_inicio + g_nfs_sectores) return 3;  // hay algo detras
    }
    if (cual < 0) return 2;

    uint64_t fin = g_nfs_inicio + g_nfs_sectores;
    if (g_capacidad_sectores <= fin + 2048) return 1;            // menos de 1 MB: no merece la pena
    uint64_t nuevos = g_capacidad_sectores - g_nfs_inicio;
    if (nuevos > 0xFFFFFFFFULL) nuevos = 0xFFFFFFFFULL;

    uint32_t bloques = 0;
    if (!nemofs_puede_crecer((uint32_t)nuevos, &bloques)) return 5;

    // la copia de seguridad del sector 0, en la particion de arranque
    if (g_fat_ok) {
        if (!fat_write_file("MBR.BAK", mbr, 512))
            uart_puts("disk_pi4: aviso -- no se pudo guardar MBR.BAK\n");
    }

    uint8_t *e = &mbr[446 + 16 * cual];                          // solo esta entrada
    e[12] = (uint8_t)(nuevos & 0xFF);
    e[13] = (uint8_t)((nuevos >> 8) & 0xFF);
    e[14] = (uint8_t)((nuevos >> 16) & 0xFF);
    e[15] = (uint8_t)((nuevos >> 24) & 0xFF);
    if (!sd_escribir_fisico(0, mbr)) return 4;

    // La tabla acaba de cambiar bajo los pies de la cache,
    // y encima esta escritura no ha pasado por disk_write_sector_n, que
    // es quien refresca lo guardado. Se vacia entera: a partir de aqui
    // el reparto del disco es otro y nada de lo de antes vale.
    sd_cache_vaciar();

    g_nfs_sectores = nuevos;
    g_tabla[cual].sectores = (uint32_t)nuevos;
    if (nemofs_crecer((uint32_t)nuevos) == 0) {
        uart_puts("disk_pi4: la tabla se estiro pero NemoFS no pudo crecer\n");
        return 5;
    }
    if (sectores_nuevos) *sectores_nuevos = nuevos;
    uart_puts("disk_pi4: particion de NemoFS estirada hasta el final de la tarjeta\n");
    return 0;
}

void disk_rango_nemofs(uint64_t *inicio, uint64_t *sectores, bool *de_tabla) {
    if (inicio) *inicio = g_nfs_inicio;
    if (sectores) *sectores = g_nfs_sectores;
    if (de_tabla) *de_tabla = g_nfs_de_tabla;
}

uint64_t disk_capacity_sectors_n(uint8_t disk) {
    if (!g_disco_listo) return 0;
    if (disk == 0) return g_nfs_sectores;   // lo que dice la tabla, ni un sector mas
    if (disk == 1 && g_fat_ok) return g_fat_sectores;
    if (disk == 2 && g_usb_ok) return g_usb_sectores;
    return 0;
}
