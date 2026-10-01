// xhci_pi4.c -- Nemo OS, Raspberry Pi 4
// Driver xHCI para el VL805.
//   Paso 1: registros, reset del controlador, estado de los puertos.
//   Paso 2: estructuras en RAM (DCBAA, scratchpad, anillo de comandos,
//           anillo de eventos), arranque del controlador (RS=1), reset
//           del puerto con dispositivo, y primer comando (Enable Slot)
//           con su Command Completion Event -- la prueba de que el DMA
//           funciona en las dos direcciones.
//   Paso 3: Address Device y transferencias de control por EP0:
//           descriptores de dispositivo, cadena de producto,
//           configuracion con interfaces y endpoints.
//   Paso 4: hubs (clase 9): configurar, encender puertos, leer estado,
//           resetear el puerto con dispositivo, y direccionar el hijo
//           con route string + Transaction Translator (LS/FS detras de
//           un hub HS). El teclado Logitech de esta placa lleva un hub
//           VIA integrado, asi que este paso es obligatorio.
//   Paso 5: HID: SET_CONFIGURATION, SET_PROTOCOL(boot), SET_IDLE,
//           Configure Endpoint del EP de interrupcion IN, TRBs Normal
//           encolados y sondeo no bloqueante del anillo de eventos
//           desde input_poll(). Entrega informes de 8 bytes a input_pi4.
//
// Referencia: especificacion xHCI 1.x (Intel), capitulos 4 y 5.
// El controlador tiene cuatro bloques de registros, todos relativos
// al BAR0 (pcie_xhci_base()):
//   - Capability:  en 0. CAPLENGTH (byte 0) dice donde empieza el
//                  siguiente bloque.
//   - Operational: en CAPLENGTH. USBCMD, USBSTS, PAGESIZE, CRCR,
//                  DCBAAP, CONFIG, y a partir de +0x400 los PORTSC.
//   - Runtime:     en RTSOFF (interrupters).
//   - Doorbells:   en DBOFF.
//
// REGLA de esta placa: toda direccion de RAM entregada al VL805 lleva
// sumado pcie_dma_offset() (ver xhci_dma_addr). En este paso aun no
// se entrega ninguna.

#include "xhci_pi4.h"
#include "pcie_pi4.h"

void uart_puts(const char *s);
void uart_putc(char c);

static void put_hex32(uint32_t v) {
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uint8_t n = (v >> i) & 0xF;
        uart_putc(n < 10 ? (char)('0' + n) : (char)('a' + n - 10));
    }
}
static void put_dec(uint32_t v) {
    char b[11]; int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) uart_putc(b[--n]);
}

static void esperar_ms(uint32_t ms) {
    uint64_t f, t0, t;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
    uint64_t fin = t0 + (f * ms) / 1000;
    do { __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t)); } while (t < fin);
}

// ---------------------------------------------------------------------
// Mapa de registros (offsets de la especificacion)
// ---------------------------------------------------------------------
// Capability
#define CAP_CAPLENGTH   0x00   // byte
#define CAP_HCIVERSION  0x02   // 16 bits
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCSPARAMS3  0x0C
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18
// Operational
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTSC(n)    (0x400 + 0x10 * ((n) - 1))   // puertos 1..N

#define USBCMD_RS       (1u << 0)   // Run/Stop
#define USBCMD_HCRST    (1u << 1)   // Host Controller Reset
#define USBSTS_HCH      (1u << 0)   // HC Halted
#define USBSTS_CNR      (1u << 11)  // Controller Not Ready
#define PORTSC_CCS      (1u << 0)   // Current Connect Status
#define PORTSC_PED      (1u << 1)   // Port Enabled
#define PORTSC_PR       (1u << 4)   // Port Reset
#define PORTSC_PLS_MASK (0xFu << 5)
#define PORTSC_PP       (1u << 9)   // Port Power
#define PORTSC_SPEED(v) (((v) >> 10) & 0xF)
#define PORTSC_CSC      (1u << 17)  // Connect Status Change
#define PORTSC_PRC      (1u << 21)  // Port Reset Change
#define PORTSC_WPR      (1u << 31)  // Warm Port Reset (solo puertos USB 3)
// Bits RW1C de PORTSC (escribir 1 los LIMPIA): hay que ponerlos a 0 al
// hacer read-modify-write para no borrar cambios sin querer.
#define PORTSC_RW1C     ((1u << 17) | (1u << 18) | (1u << 19) | (1u << 20) | \
                         (1u << 21) | (1u << 22) | (1u << 23))
#define USBSTS_EINT     (1u << 3)   // Event Interrupt (aunque no usemos IRQ)

// Runtime: interrupter 0 esta en RTSOFF + 0x20
#define RT_IR0_IMAN     0x20
#define RT_IR0_IMOD     0x24
#define RT_IR0_ERSTSZ   0x28
#define RT_IR0_ERSTBA   0x30   // 64 bits
#define RT_IR0_ERDP     0x38   // 64 bits; bit 3 EHB (Event Handler Busy, RW1C)

// TRB: 4 dwords. dword3: bit 0 Cycle, bit 1 TC/ENT, bits 15:10 tipo.
#define TRB_CYCLE           (1u << 0)
#define TRB_TOGGLE_CYCLE    (1u << 1)
#define TRB_TYPE(t)         ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(d3)    (((d3) >> 10) & 0x3F)
#define TRB_TYPE_SETUP      2
#define TRB_TYPE_DATA       3
#define TRB_TYPE_STATUS     4
#define TRB_TYPE_LINK       6
#define TRB_TYPE_ENABLE_SLOT 9
#define TRB_TYPE_DISABLE_SLOT 10
#define TRB_TYPE_ADDRESS_DEVICE 11
#define TRB_TYPE_EVALUATE_CONTEXT 13
#define TRB_TYPE_TRANSFER_EVENT 32
#define TRB_TYPE_CMD_COMPLETION 33
#define TRB_TYPE_PORT_STATUS_CHANGE 34
#define TRB_IOC             (1u << 5)
#define TRB_IDT             (1u << 6)
#define TRB_DIR_IN          (1u << 16)
#define TRB_TRT_IN          (3u << 16)   // Setup: hay etapa de datos IN
#define TRB_TYPE_NORMAL     1
#define TRB_TYPE_CONFIGURE_EP 12
#define TRB_TYPE_RESET_EP      14
#define TRB_TYPE_SET_TR_DEQ    16
#define CC_USB_TRANSACTION_ERR 4
#define CC_STALL               6
#define CC_SPLIT_TRANSACTION_ERR 36
#define TRB_ISP             (1u << 2)   // Interrupt on Short Packet
// HID
#define HID_REQ_SET_IDLE     0x0A
#define HID_REQ_SET_PROTOCOL 0x0B
// Mass Storage, Bulk-Only Transport (USB MSC BOT 1.0)
#define BOT_CBW_SIGNATURE   0x43425355u   // "USBC"
#define BOT_CSW_SIGNATURE   0x53425355u   // "USBS"
#define BOT_FLAG_IN         0x80
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A
#define EP_TYPE_BULK_OUT     2
#define EP_TYPE_BULK_IN      6
#define TRB_TRT_OUT         (2u << 16)   // Setup: etapa de datos OUT
#define CC_SUCCESS          1
#define CC_SHORT_PACKET     13

// Hub (USB 2.0 cap. 11)
#define REQ_SET_CONFIGURATION 9
#define REQ_CLEAR_FEATURE     1
#define REQ_SET_FEATURE       3
#define REQ_GET_STATUS        0
#define DESC_HUB              0x29
#define HUB_FEAT_PORT_RESET   4
#define HUB_FEAT_PORT_POWER   8
#define HUB_FEAT_C_PORT_CONNECTION 16
#define HUB_FEAT_C_PORT_RESET 20
#define PS_CONNECTION   (1u << 0)
#define PS_ENABLE       (1u << 1)
#define PS_RESET        (1u << 4)
#define PS_POWER        (1u << 8)
#define PS_LOW_SPEED    (1u << 9)
#define PS_HIGH_SPEED   (1u << 10)
#define PC_CONNECTION   (1u << 0)
#define PC_RESET        (1u << 4)

// Peticiones estandar USB
#define REQ_GET_DESCRIPTOR  6
#define DESC_HID_REPORT     0x22
#define DESC_DEVICE         1
#define DESC_CONFIGURATION  2
#define DESC_STRING         3
#define DESC_INTERFACE      4
#define DESC_ENDPOINT       5

typedef struct { uint32_t d0, d1, d2, d3; } trb_t;

// Definido en el paso 5: si el evento es de un EP HID lo atiende (repone
// el TRB, recupera el EP si hace falta) y devuelve true. Lo usan tambien
// las esperas bloqueantes para no perder informes durante la enumeracion.
static bool procesar_evento_hid(const trb_t *ev);
// Conexion en caliente (definidas al final del archivo)
static void hotplug_sondeo(void);
static uint64_t ms_ahora(void);

// ---------------------------------------------------------------------
// Estructuras en RAM que el controlador lee/escribe por DMA.
// Todas estaticas y alineadas: 64 bytes minimo (spec), y las paginas de
// scratchpad a PAGESIZE (4KB). Un anillo de 1KB alineado a 4KB nunca
// cruza un limite de 64KB (requisito de la spec para los segmentos).
// ---------------------------------------------------------------------
#define MAX_SLOTS       32
#define NUM_SCRATCHPAD  31          // HCSPARAMS2 del VL805: 31 paginas
#define PAGINA          4096
#define CMD_RING_TRBS   64
#define EVT_RING_TRBS   64

__attribute__((aligned(64)))   static uint64_t dcbaa[MAX_SLOTS + 1];
__attribute__((aligned(64)))   static uint64_t scratchpad_array[NUM_SCRATCHPAD];
__attribute__((aligned(PAGINA))) static uint8_t scratchpad_pages[NUM_SCRATCHPAD][PAGINA];
__attribute__((aligned(PAGINA))) static trb_t cmd_ring[CMD_RING_TRBS];
__attribute__((aligned(PAGINA))) static trb_t evt_ring[EVT_RING_TRBS];
__attribute__((aligned(64)))   static uint64_t erst[2];   // 1 entrada: base(64) + tamaño(32)+reservado

// Contextos (CSZ=0 -> 32 bytes cada uno). Input Context = Input Control
// + Slot + 31 EP = 33 x 32 = 1056 B. Device Context (salida, la escribe
// el HC) = Slot + 31 EP = 1024 B. Alineados a pagina para no cruzar
// limites.
#define CTX_BYTES 32
#define MAX_DEV   8          // slots 1..7 (0 no se usa): hub + teclado + raton + margen
__attribute__((aligned(PAGINA))) static uint32_t input_ctx[33 * CTX_BYTES / 4];   // compartido
__attribute__((aligned(PAGINA))) static uint32_t device_ctx[MAX_DEV][32 * CTX_BYTES / 4];
__attribute__((aligned(PAGINA))) static trb_t ep0_ring[MAX_DEV][64];
__attribute__((aligned(PAGINA))) static uint8_t ctrl_buf[512];
static uint32_t ep0_enq[MAX_DEV], ep0_cycle[MAX_DEV];
static uint32_t dev_speed[MAX_DEV];   // velocidad (codificacion PORTSC) por slot

// Endpoints de interrupcion IN de dispositivos HID (teclados y ratones)
#define INT_RING_TRBS   64
#define NUM_INFORMES    8
#define MAX_HID         4
typedef struct {
    uint32_t slot, dci, mps, enq, cycle;
    int      tipo;                        // XHCI_HID_TECLADO / XHCI_HID_RATON
    uint32_t errores_seguidos;            // recuperaciones consecutivas sin un exito entre medias
    bool     abandonado;                  // demasiados errores seguidos: se deja de reintentar
    bool     activo;                      // entrada en uso (false = libre, reutilizable)
    uint32_t informe_de_trb[INT_RING_TRBS];
} hid_ep_t;
#define MAX_ERRORES_SEGUIDOS 5

// Contabilidad por slot, para poder deshacer en una desconexion.
typedef struct {
    bool     usado;
    bool     es_hub;
    uint32_t root_port;     // puerto raiz del que cuelga (directo o via hub)
    uint32_t hub_slot;      // 0 si va directo al puerto raiz
    uint32_t hub_port;      // puerto del hub padre (0 si directo)
    uint32_t nports;        // si es hub: numero de puertos
    // Route string ACUMULADO de este dispositivo, y en que
    // nivel de hubs cuelga (0 = directo al puerto raiz). Hacen falta
    // los dos para calcular el route de un hijo: ver hub_puerto_enumerar.
    uint32_t route;
    uint32_t nivel;
} slot_info_t;
static slot_info_t slots[MAX_DEV];
__attribute__((aligned(PAGINA))) static trb_t   hid_rings[MAX_HID][INT_RING_TRBS];
__attribute__((aligned(64)))     static uint8_t hid_bufs[MAX_HID][NUM_INFORMES][64];  // 1 informe por linea de cache
static hid_ep_t hid_eps[MAX_HID];
static uint32_t num_hid = 0;

static uint32_t cmd_enq = 0;      // indice de encolado en el anillo de comandos
static uint32_t cmd_cycle = 1;    // Producer Cycle State del anillo de comandos
static uint32_t evt_deq = 0;      // indice de desencolado del anillo de eventos
static uint32_t evt_cycle = 1;    // Consumer Cycle State del anillo de eventos
static bool     hc_corriendo = false;
static uint32_t puerto_con_dispositivo = 0;

// ---------------------------------------------------------------------
// Coherencia de cache. El DMA de PCIe en la BCM2711 NO es coherente con
// las caches de la CPU: antes de que el HC LEA una estructura hay que
// limpiarla (dc cvac) y antes de que la CPU LEA algo que el HC ESCRIBIO
// hay que invalidarla (dc ivac). Todas las estructuras de arriba ocupan
// lineas de cache enteras y exclusivas, asi que invalidar es seguro.
// ---------------------------------------------------------------------
static void cache_limpiar(const void *p, uintptr_t len) {
    uintptr_t a = (uintptr_t)p & ~63UL, fin = (uintptr_t)p + len;
    for (; a < fin; a += 64) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void cache_invalidar(const void *p, uintptr_t len) {
    uintptr_t a = (uintptr_t)p & ~63UL, fin = (uintptr_t)p + len;
    for (; a < fin; a += 64) __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static volatile uint8_t  *cap_base = 0;
static volatile uint32_t *op_base  = 0;
static volatile uint32_t *rt_base  = 0;
static volatile uint32_t *db_base  = 0;
static uint32_t num_ports = 0, num_slots = 0;

static inline uint32_t cap32(uint32_t off) { return *(volatile uint32_t *)(cap_base + off); }
static inline uint32_t op32(uint32_t off)  { return *(volatile uint32_t *)((volatile uint8_t *)op_base + off); }
static inline void op32_w(uint32_t off, uint32_t v) { *(volatile uint32_t *)((volatile uint8_t *)op_base + off) = v; }
static inline uint32_t rt32(uint32_t off)  { return *(volatile uint32_t *)((volatile uint8_t *)rt_base + off); }
static inline void rt32_w(uint32_t off, uint32_t v) { *(volatile uint32_t *)((volatile uint8_t *)rt_base + off) = v; }
static inline void op64_w(uint32_t off, uint64_t v) {
    op32_w(off, (uint32_t)v); op32_w(off + 4, (uint32_t)(v >> 32));
}
static inline void rt64_w(uint32_t off, uint64_t v) {
    rt32_w(off, (uint32_t)v); rt32_w(off + 4, (uint32_t)(v >> 32));
}
static inline void doorbell(uint32_t n, uint32_t target) { db_base[n] = target; }

// ---------------------------------------------------------------------
uint64_t xhci_dma_addr(const void *p) {
    return (uint64_t)(uintptr_t)p + pcie_dma_offset();
}

uint32_t xhci_num_ports(void) { return num_ports; }

bool xhci_port_connected(uint32_t puerto) {
    if (!op_base || puerto < 1 || puerto > num_ports) return false;
    return (op32(OP_PORTSC(puerto)) & PORTSC_CCS) != 0;
}

static const char *nombre_velocidad(uint32_t s) {
    switch (s) {
        case 1: return "Full (12 Mb/s)";
        case 2: return "Low (1.5 Mb/s)";
        case 3: return "High (480 Mb/s)";
        case 4: return "Super (5 Gb/s)";
        default: return "?";
    }
}

// ---------------------------------------------------------------------
// Anillo de comandos (productor: nosotros; consumidor: el HC)
// ---------------------------------------------------------------------
static void cmd_ring_init(void) {
    for (uint32_t i = 0; i < CMD_RING_TRBS; i++) cmd_ring[i] = (trb_t){0, 0, 0, 0};
    // Ultimo TRB = Link al principio, con Toggle Cycle.
    uint64_t base = xhci_dma_addr(cmd_ring);
    trb_t *link = &cmd_ring[CMD_RING_TRBS - 1];
    link->d0 = (uint32_t)base;
    link->d1 = (uint32_t)(base >> 32);
    link->d2 = 0;
    link->d3 = TRB_TYPE(TRB_TYPE_LINK) | TRB_TOGGLE_CYCLE;   // cycle=0 al principio
    cmd_enq = 0; cmd_cycle = 1;
    cache_limpiar(cmd_ring, sizeof cmd_ring);
}

// Encola un TRB de comando y devuelve su direccion DMA (para casar el
// evento de completado). No toca el doorbell.
static uint64_t cmd_encolar(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3_sin_cycle) {
    trb_t *t = &cmd_ring[cmd_enq];
    t->d0 = d0; t->d1 = d1; t->d2 = d2;
    t->d3 = (d3_sin_cycle & ~TRB_CYCLE) | (cmd_cycle ? TRB_CYCLE : 0);
    cache_limpiar(t, sizeof *t);
    uint64_t dma = xhci_dma_addr(t);

    cmd_enq++;
    if (cmd_enq == CMD_RING_TRBS - 1) {
        // Hemos llegado al Link TRB: darle nuestro cycle y dar la vuelta.
        trb_t *link = &cmd_ring[CMD_RING_TRBS - 1];
        link->d3 = (link->d3 & ~TRB_CYCLE) | (cmd_cycle ? TRB_CYCLE : 0);
        cache_limpiar(link, sizeof *link);
        cmd_enq = 0;
        cmd_cycle ^= 1;
    }
    return dma;
}

// ---------------------------------------------------------------------
// Anillo de eventos (productor: el HC; consumidor: nosotros). Sin IRQ:
// se sondea. Un solo segmento.
// ---------------------------------------------------------------------
static void evt_ring_init(void) {
    for (uint32_t i = 0; i < EVT_RING_TRBS; i++) evt_ring[i] = (trb_t){0, 0, 0, 0};
    cache_limpiar(evt_ring, sizeof evt_ring);
    erst[0] = xhci_dma_addr(evt_ring);
    erst[1] = EVT_RING_TRBS;          // dword2 = tamaño en TRBs, dword3 reservado
    cache_limpiar(erst, sizeof erst);
    evt_deq = 0; evt_cycle = 1;

    rt32_w(RT_IR0_ERSTSZ, 1);
    rt64_w(RT_IR0_ERDP, xhci_dma_addr(evt_ring));
    rt64_w(RT_IR0_ERSTBA, xhci_dma_addr(erst));   // ultimo: activa el anillo
}

// Mira si hay un evento nuevo. Si lo hay lo copia, avanza y devuelve
// true. No bloquea.
static bool evt_intentar(trb_t *out) {
    trb_t *e = &evt_ring[evt_deq];
    cache_invalidar(e, sizeof *e);
    if (((e->d3 & TRB_CYCLE) != 0) != (evt_cycle != 0)) return false;
    *out = *e;
    evt_deq++;
    if (evt_deq == EVT_RING_TRBS) { evt_deq = 0; evt_cycle ^= 1; }
    // Avisar al HC de hasta donde hemos leido (y limpiar EHB).
    rt64_w(RT_IR0_ERDP, xhci_dma_addr(&evt_ring[evt_deq]) | (1u << 3));
    return true;
}

// Espera (sondeando, con timeout en ms) al siguiente evento.
static bool evt_esperar(trb_t *out, uint32_t timeout_ms) {
    uint64_t f, t0, t;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
    uint64_t fin = t0 + (f * timeout_ms) / 1000;
    for (;;) {
        if (evt_intentar(out)) return true;
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
        if (t >= fin) return false;
    }
}

// Envia un comando y espera su Command Completion Event. Devuelve el
// codigo de completado (1 = exito) y, si se pide, el slot del evento.
static uint32_t cmd_ejecutar(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
                             uint32_t *slot_out, uint32_t timeout_ms) {
    uint64_t dma = cmd_encolar(d0, d1, d2, d3);
    doorbell(0, 0);   // doorbell del host controller, target 0 = command ring
    trb_t ev;
    while (evt_esperar(&ev, timeout_ms)) {
        uint32_t tipo = TRB_GET_TYPE(ev.d3);
        if (tipo == TRB_TYPE_CMD_COMPLETION) {
            uint64_t ptr = ((uint64_t)ev.d1 << 32) | ev.d0;
            if (ptr != dma) continue;          // completado de otro comando
            if (slot_out) *slot_out = ev.d3 >> 24;
            return ev.d2 >> 24;                // completion code
        }
        if (tipo == TRB_TYPE_TRANSFER_EVENT) procesar_evento_hid(&ev);
        // Otros eventos (p.ej. Port Status Change): se ignoran.
    }
    return 0;   // timeout
}

// ---------------------------------------------------------------------
// Que clase de puerto es cada puerto raiz
//
// No todos los puertos son USB 2.0, y la diferencia no es cosmetica:
// un puerto USB 3 con un aparato dentro
// entrena el enlace SOLO, sin que el host le mande un reset, y se
// habilita el solo. Mandarle el reset de USB 2 no lo arregla -- como
// mucho no hace nada, y el driver se queda esperando un cambio que no
// va a llegar.
//
// La respuesta no hay que adivinarla: el propio controlador la lleva
// escrita en una lista de capacidades extendidas (xECP), donde la de
// ID 2 ("Supported Protocol") dice, por cada tramo de puertos, que
// version de USB habla. Se lee una vez al arrancar.
#define XECP_ID_PROTOCOLO 2

static uint8_t puerto_rev[32 + 1];   // version USB mayor por puerto (2 o 3)

static void protocolos_leer(uint32_t hcc1) {
    for (uint32_t i = 0; i <= 32; i++) puerto_rev[i] = 0;

    uint32_t off = (hcc1 >> 16) & 0xFFFF;   // en palabras de 32 bits
    if (off == 0) return;                   // sin lista: nada que leer

    uint32_t recorridos = 0;
    volatile uint8_t *cap = cap_base + off * 4;
    while (recorridos++ < 64) {             // tope: una lista circular no nos cuelga
        uint32_t d0 = *(volatile uint32_t *)cap;
        uint32_t id = d0 & 0xFF;
        uint32_t siguiente = (d0 >> 8) & 0xFF;

        if (id == XECP_ID_PROTOCOLO) {
            uint32_t mayor = (d0 >> 24) & 0xFF;
            uint32_t menor = (d0 >> 16) & 0xFF;
            uint32_t d2 = *(volatile uint32_t *)(cap + 8);
            uint32_t primero = d2 & 0xFF;
            uint32_t cuantos = (d2 >> 8) & 0xFF;

            uart_puts("xhci: protocolo USB "); put_dec(mayor);
            uart_putc('.'); if (menor < 10) uart_putc('0'); put_dec(menor);
            uart_puts(": puertos "); put_dec(primero);
            if (cuantos > 1) { uart_puts(" a "); put_dec(primero + cuantos - 1); }
            uart_puts("\n");

            for (uint32_t i = 0; i < cuantos; i++) {
                uint32_t p = primero + i;
                if (p >= 1 && p <= 32) puerto_rev[p] = (uint8_t)mayor;
            }
        }

        if (siguiente == 0) break;
        cap += siguiente * 4;
    }
}

// true si el puerto raiz p habla USB 3. Si la lista no dijo nada de el
// (controlador raro, o lista ausente), se trata como USB 2, que es el
// comportamiento de siempre.
static bool puerto_es_usb3(uint32_t p) {
    return p <= 32 && puerto_rev[p] >= 3;
}

// ---------------------------------------------------------------------
// Puerto: reset y lectura de velocidad (USB 2.0 la necesita)
// ---------------------------------------------------------------------
// Manda un reset (PR = el normal, WPR = el "en caliente" de USB 3) y
// espera a que el puerto avise de que termino.
static bool reset_con(uint32_t p, uint32_t bit) {
    uint32_t sc = op32(OP_PORTSC(p));
    sc &= ~PORTSC_RW1C;      // no limpiar cambios sin querer
    sc &= ~PORTSC_PED;       // escribir 1 en PED DESHABILITA el puerto
    op32_w(OP_PORTSC(p), sc | bit);

    uint32_t t = 0;
    while (!(op32(OP_PORTSC(p)) & PORTSC_PRC) && t++ < 1000) esperar_ms(1);
    if (!(op32(OP_PORTSC(p)) & PORTSC_PRC)) return false;

    // Limpiar PRC (RW1C) sin tocar nada mas.
    sc = op32(OP_PORTSC(p));
    op32_w(OP_PORTSC(p), (sc & ~PORTSC_RW1C & ~PORTSC_PED) | PORTSC_PRC);
    esperar_ms(10);
    return (op32(OP_PORTSC(p)) & PORTSC_PED) != 0;
}

static bool puerto_reset(uint32_t p) {
    // Un puerto USB 3 con el enlace ya entrenado esta habilitado el
    // solo: no hay nada que resetear. Mandarselo de todas formas seria
    // tirarle un enlace que ya funcionaba.
    // Un puerto USB 3 con el enlace ya entrenado esta habilitado el
    // solo: no hay nada que resetear, y mandarselo seria tirarle un
    // enlace que ya funcionaba.
    if (puerto_es_usb3(p) && (op32(OP_PORTSC(p)) & PORTSC_PED)) return true;

    // Camino de siempre, intacto: el reset normal. Todo lo que
    // funcionaba antes de esta funcion sigue pasando por aqui y por
    // ningun sitio mas.
    if (reset_con(p, PORTSC_PR)) return true;

    // Solo si ese ha fracasado, y solo en un puerto USB 3, se prueba
    // el reset "en caliente", que reentrena el enlace desde cero. Es
    // lo que hace Linux cuando un aparato SuperSpeed se queda a medias
    // al enchufarlo. Añadido, nunca sustituto.
    if (puerto_es_usb3(p)) return reset_con(p, PORTSC_WPR);
    return false;
}

// ---------------------------------------------------------------------
// Paso 3: Address Device y transferencias de control por EP0 (por slot)
// ---------------------------------------------------------------------
static void ep0_ring_init(uint32_t slot) {
    trb_t *r = ep0_ring[slot];
    for (uint32_t i = 0; i < 64; i++) r[i] = (trb_t){0, 0, 0, 0};
    uint64_t base = xhci_dma_addr(r);
    r[63].d0 = (uint32_t)base;
    r[63].d1 = (uint32_t)(base >> 32);
    r[63].d3 = TRB_TYPE(TRB_TYPE_LINK) | TRB_TOGGLE_CYCLE;
    ep0_enq[slot] = 0; ep0_cycle[slot] = 1;
    cache_limpiar(r, 64 * sizeof(trb_t));
}

static uint64_t ep0_encolar(uint32_t slot, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    trb_t *r = ep0_ring[slot];
    trb_t *t = &r[ep0_enq[slot]];
    t->d0 = d0; t->d1 = d1; t->d2 = d2;
    t->d3 = (d3 & ~TRB_CYCLE) | (ep0_cycle[slot] ? TRB_CYCLE : 0);
    cache_limpiar(t, sizeof *t);
    uint64_t dma = xhci_dma_addr(t);
    if (++ep0_enq[slot] == 63) {
        r[63].d3 = (r[63].d3 & ~TRB_CYCLE) | (ep0_cycle[slot] ? TRB_CYCLE : 0);
        cache_limpiar(&r[63], sizeof(trb_t));
        ep0_enq[slot] = 0; ep0_cycle[slot] ^= 1;
    }
    return dma;
}

static uint32_t ep0_mps_inicial(uint32_t speed) {
    switch (speed) { case 1: case 2: return 8; case 3: return 64; default: return 512; }
}

// Rellena el Input Context (Control + Slot + EP0) y envia Address Device.
//   route: route string (0 si va directo al root hub; puerto del hub si
//          cuelga de uno, 4 bits por nivel)
//   tt_slot/tt_port: slot del hub HS y puerto en el, para dispositivos
//          LS/FS detras de un hub HS (0/0 si no aplica)
static bool slot_address_device(uint32_t slot, uint32_t root_port, uint32_t speed,
                                uint32_t route, uint32_t tt_slot, uint32_t tt_port) {
    for (uint32_t i = 0; i < sizeof input_ctx / 4; i++) input_ctx[i] = 0;
    for (uint32_t i = 0; i < 32 * CTX_BYTES / 4; i++) device_ctx[slot][i] = 0;
    ep0_ring_init(slot);
    dev_speed[slot] = speed;

    uint32_t *icc  = &input_ctx[0];
    uint32_t *sctx = &input_ctx[CTX_BYTES / 4];
    uint32_t *ep0  = &input_ctx[2 * CTX_BYTES / 4];

    icc[1] = (1u << 0) | (1u << 1);              // Add A0 (slot) + A1 (EP0)

    sctx[0] = (1u << 27) | (speed << 20) | (route & 0xFFFFF);
    sctx[1] = (root_port << 16);
    sctx[2] = (tt_slot & 0xFF) | ((tt_port & 0xFF) << 8);   // TT Hub Slot ID, TT Port
    sctx[3] = 0;

    uint32_t mps = ep0_mps_inicial(speed);
    ep0[1] = (3u << 1) | (4u << 3) | (mps << 16);
    uint64_t deq = xhci_dma_addr(ep0_ring[slot]) | 1u;
    ep0[2] = (uint32_t)deq;
    ep0[3] = (uint32_t)(deq >> 32);
    ep0[4] = 8;

    cache_limpiar(input_ctx, sizeof input_ctx);
    cache_limpiar(device_ctx[slot], 32 * CTX_BYTES);
    dcbaa[slot] = xhci_dma_addr(device_ctx[slot]);
    cache_limpiar(dcbaa, sizeof dcbaa);

    uint64_t ictx = xhci_dma_addr(input_ctx);
    uint32_t cc = cmd_ejecutar((uint32_t)ictx, (uint32_t)(ictx >> 32), 0,
                               TRB_TYPE(TRB_TYPE_ADDRESS_DEVICE) | (slot << 24), 0, 2000);
    if (cc != CC_SUCCESS) {
        uart_puts("xhci: Address Device FALLO (cc="); put_dec(cc); uart_puts(")\n");
        return false;
    }
    cache_invalidar(device_ctx[slot], 32 * CTX_BYTES);
    uart_puts("xhci: Address Device OK (slot "); put_dec(slot);
    uart_puts("). Direccion USB="); put_dec(device_ctx[slot][3] & 0xFF);
    uart_puts("\n");
    return true;
}

// Marca el slot de un hub como hub (bit Hub, numero de puertos, TTT) con
// Configure Endpoint: sin esto el HC no sabe enrutar hacia sus hijos.
static bool slot_configure_hub(uint32_t slot, uint32_t nports, uint32_t ttt) {
    for (uint32_t i = 0; i < sizeof input_ctx / 4; i++) input_ctx[i] = 0;
    uint32_t *icc  = &input_ctx[0];
    uint32_t *sctx = &input_ctx[CTX_BYTES / 4];
    cache_invalidar(device_ctx[slot], 32 * CTX_BYTES);
    // Partimos del slot context actual (lo escribio el HC en Address Device)
    for (uint32_t i = 0; i < CTX_BYTES / 4; i++) sctx[i] = device_ctx[slot][i];
    sctx[0] = (sctx[0] & ~(0x1Fu << 27)) | (1u << 27) | (1u << 26);   // Context Entries=1, Hub=1
    sctx[1] = (sctx[1] & 0x00FFFFFFu) | (nports << 24);              // Number of Ports
    sctx[2] = (sctx[2] & ~(3u << 16)) | ((ttt & 3) << 16);            // TT Think Time
    sctx[3] = 0;                                                       // campos del HC: a 0 en la entrada
    icc[1] = (1u << 0);                                                // Add A0 (solo slot)
    cache_limpiar(input_ctx, sizeof input_ctx);

    uint64_t ictx = xhci_dma_addr(input_ctx);
    uint32_t cc = cmd_ejecutar((uint32_t)ictx, (uint32_t)(ictx >> 32), 0,
                               TRB_TYPE(TRB_TYPE_CONFIGURE_EP) | (slot << 24), 0, 2000);
    if (cc != CC_SUCCESS) {
        uart_puts("xhci: Configure Endpoint (hub) FALLO (cc="); put_dec(cc); uart_puts(")\n");
        return false;
    }
    return true;
}

// Espera el Transfer Event del TRB de Status. Devuelve true si exito.
// Los eventos de EPs HID que lleguen mientras tanto NO se tiran: se
// atienden, para que sus anillos no se vacien durante la enumeracion.
static bool esperar_transfer_ok(uint64_t status_dma) {
    trb_t ev;
    while (evt_esperar(&ev, 2000)) {
        if (TRB_GET_TYPE(ev.d3) != TRB_TYPE_TRANSFER_EVENT) continue;
        if (procesar_evento_hid(&ev)) continue;
        uint64_t ptr = ((uint64_t)ev.d1 << 32) | ev.d0;
        uint32_t cc = ev.d2 >> 24;
        if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
            uart_puts("xhci: transferencia de control fallo (cc="); put_dec(cc); uart_puts(")\n");
            return false;
        }
        if (ptr == status_dma) return true;
    }
    uart_puts("xhci: transferencia de control: timeout\n");
    return false;
}

// Control IN con datos. Devuelve bytes pedidos o -1.
static int control_in(uint32_t slot, uint8_t bmRequestType, uint8_t bRequest,
                      uint16_t wValue, uint16_t wIndex, void *buf, uint16_t wLength) {
    cache_invalidar(buf, wLength);
    uint64_t bdma = xhci_dma_addr(buf);
    ep0_encolar(slot, bmRequestType | ((uint32_t)bRequest << 8) | ((uint32_t)wValue << 16),
                wIndex | ((uint32_t)wLength << 16), 8,
                TRB_TYPE(TRB_TYPE_SETUP) | TRB_IDT | TRB_TRT_IN);
    ep0_encolar(slot, (uint32_t)bdma, (uint32_t)(bdma >> 32), wLength,
                TRB_TYPE(TRB_TYPE_DATA) | TRB_DIR_IN);
    uint64_t st = ep0_encolar(slot, 0, 0, 0, TRB_TYPE(TRB_TYPE_STATUS) | TRB_IOC);  // Status OUT
    doorbell(slot, 1);
    if (!esperar_transfer_ok(st)) return -1;
    cache_invalidar(buf, wLength);
    return (int)wLength;
}

// Control sin datos (SET_CONFIGURATION, SET/CLEAR_FEATURE...).
static bool control_nodata(uint32_t slot, uint8_t bmRequestType, uint8_t bRequest,
                           uint16_t wValue, uint16_t wIndex) {
    ep0_encolar(slot, bmRequestType | ((uint32_t)bRequest << 8) | ((uint32_t)wValue << 16),
                wIndex, 8, TRB_TYPE(TRB_TYPE_SETUP) | TRB_IDT);          // TRT = 0: sin datos
    uint64_t st = ep0_encolar(slot, 0, 0, 0, TRB_TYPE(TRB_TYPE_STATUS) | TRB_IOC | TRB_DIR_IN);
    doorbell(slot, 1);
    return esperar_transfer_ok(st);
}

static void put_str_utf16(const uint8_t *d) {
    for (uint32_t i = 2; i + 1 < d[0]; i += 2) uart_putc(d[i + 1] == 0 && d[i] < 128 ? (char)d[i] : '?');
}

// Interfaces HID boot encontradas al leer los descriptores de UN dispositivo
typedef struct { int tipo; uint32_t interfaz, ep, mps, intervalo; } hid_if_t;
static hid_if_t hid_ifs[4];
static uint32_t num_hid_ifs = 0;
static uint32_t config_value = 1;   // bConfigurationValue del ultimo dispositivo leido

// Interfaz Mass Storage (clase 8, subclase 6 SCSI, protocolo 0x50 BOT)
// encontrada en el ultimo dispositivo leido, con sus dos EPs bulk.
static bool     msc_if_encontrada = false;
static uint32_t msc_if_num = 0, msc_ep_in = 0, msc_ep_out = 0, msc_mps_in = 0, msc_mps_out = 0;

// ---------------------------------------------------------------------
// TAMANO DE PAQUETE DEL ENDPOINT 0
//
// Al crear un dispositivo hay que darle al controlador un tamano
// maximo de paquete para el endpoint de control, y ANTES de hablar con
// el aparato solo se puede suponer. La suposicion es segura en dos de
// las tres velocidades:
//
//   High Speed : el MPS0 es SIEMPRE 64. Acertamos por norma.
//   Low Speed  : el MPS0 es SIEMPRE 8.  Acertamos por norma.
//   Full Speed : puede ser 8, 16, 32 O 64. Aqui se puede fallar.
//
// Si el controlador cree que son 8 y el aparato contesta con 64, ve
// mas datos de los que esperaba y aborta con Babble (cc=3). Ese fallo
// estuvo latente desde siempre y no se vio nunca porque los teclados
// son Low Speed y los hubs High Speed -- hizo falta un aparato Full
// Speed (el tactil de una pantalla) para destaparlo.
//
// El procedimiento correcto, que es el que hace cualquier sistema
// operativo: pedir solo los OCHO primeros bytes del descriptor (que
// caben en el paquete mas pequeno posible, asi que nunca desbordan),
// mirar el byte 7 --que es el MPS0 de verdad-- y, si no coincide con
// lo supuesto, corregirlo con un Evaluate Context antes de seguir.
static bool ep0_ajustar_mps(uint32_t slot, uint32_t mps_real) {
    for (uint32_t i = 0; i < sizeof input_ctx / 4; i++) input_ctx[i] = 0;
    uint32_t *icc = &input_ctx[0];
    uint32_t *ep0 = &input_ctx[2 * CTX_BYTES / 4];

    // Evaluate Context solo mira los contextos marcados en A1. Se toca
    // EP0 y nada mas: el resto del dispositivo se queda como esta.
    icc[1] = (1u << 1);
    ep0[1] = (3u << 1) | (4u << 3) | (mps_real << 16);

    cache_limpiar(input_ctx, sizeof input_ctx);
    uint64_t ictx = xhci_dma_addr(input_ctx);
    uint32_t cc = cmd_ejecutar((uint32_t)ictx, (uint32_t)(ictx >> 32), 0,
                               TRB_TYPE(TRB_TYPE_EVALUATE_CONTEXT) | (slot << 24), 0, 2000);
    if (cc != CC_SUCCESS) {
        uart_puts("xhci: Evaluate Context FALLO (cc="); put_dec(cc); uart_puts(")\n");
        return false;
    }
    cache_invalidar(device_ctx[slot], 32 * CTX_BYTES);
    return true;
}

// ---------------------------------------------------------------------
// PANTALLA TACTIL: leer el Report Descriptor
//
// Un teclado o un raton de arranque tienen formato FIJO por norma, y
// por eso se pueden leer sin preguntar nada. Un digitalizador no: cada
// panel coloca sus datos donde quiere, y para saber donde estan hay que
// leer su Report Descriptor, que es la descripcion que el propio
// aparato da de sus informes.
//
// Esto NO es un analizador completo de HID -- eso es un proyecto en si
// mismo. Busca exactamente tres cosas, que es todo lo que hace falta
// para mover un puntero con un dedo:
//
//   X          pagina 0x01 (escritorio generico), uso 0x30
//   Y          pagina 0x01, uso 0x31
//   Tip Switch pagina 0x0D (digitalizador), uso 0x42 -- el dedo apoyado
//
// Se queda con el PRIMERO de cada uno que encuentra. En un panel
// multitactil eso es el primer contacto, que es justo el dedo que
// queremos seguir.
//
// El formato de los elementos (HID 1.11, capitulo 6.2.2): cada uno
// empieza por un byte  (etiqueta<<4) | (tipo<<2) | tamano , seguido de
// 0, 1, 2 o 4 bytes de datos en orden little-endian. Tamano 3 significa
// CUATRO bytes, no tres: es la trampa clasica de este formato.
#define HID_TIPO_MAIN    0
#define HID_TIPO_GLOBAL  1
#define HID_TIPO_LOCAL   2

typedef struct {
    bool     valido;
    uint32_t id;            // Report ID, o 0 si el aparato no usa
    uint32_t x_bit,  x_bits;
    uint32_t y_bit,  y_bits;
    uint32_t tip_bit;
    bool     hay_tip;
    int32_t  x_max, y_max;  // maximo logico, para escalar
} tactil_fmt_t;

// Uno solo, a proposito: se sigue UN dedo de UNA pantalla. Si hubiera
// dos paneles conectados, el segundo pisaria el formato del primero.
// Cuando eso deje de ser una rareza, este formato pasa a hid_ep_t.
static tactil_fmt_t tactil_fmt;

static uint32_t hid_item_valor(const uint8_t *p, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static bool tactil_analizar(const uint8_t *d, uint32_t largo, tactil_fmt_t *f) {
    for (uint32_t i = 0; i < sizeof(tactil_fmt_t); i++) ((uint8_t *)f)[i] = 0;

    uint32_t pagina = 0, tam_campo = 0, num_campos = 0, id_actual = 0;
    int32_t  log_max = 0;
    uint32_t bit = 0;
    uint32_t usos[16]; uint32_t n_usos = 0;

    for (uint32_t i = 0; i < largo; ) {
        uint8_t pre = d[i];
        if (pre == 0xFE) break;                 // elemento largo: no se usa en paneles
        uint32_t tam = pre & 3;
        if (tam == 3) tam = 4;                  // 3 significa CUATRO bytes
        uint32_t tipo = (pre >> 2) & 3;
        uint32_t tag  = (pre >> 4) & 0xF;
        if (i + 1 + tam > largo) break;
        uint32_t val = hid_item_valor(&d[i + 1], tam);
        i += 1 + tam;

        if (tipo == HID_TIPO_GLOBAL) {
            switch (tag) {
                case 0: pagina = val; break;                 // Usage Page
                case 2: log_max = (int32_t)val; break;       // Logical Maximum
                case 7: tam_campo = val; break;              // Report Size
                case 8:                                      // Report ID
                    // Un Report ID nuevo empieza su propio informe:
                    // las posiciones vuelven a cero, y en el informe
                    // que llega por el cable el primer byte sera el ID.
                    id_actual = val; bit = 0; break;
                case 9: num_campos = val; break;             // Report Count
                default: break;
            }
        } else if (tipo == HID_TIPO_LOCAL) {
            if (tag == 0 && n_usos < 16) usos[n_usos++] = val;   // Usage
        } else if (tipo == HID_TIPO_MAIN) {
            if (tag == 8) {                                   // Input
                // Cada campo de este elemento ocupa tam_campo bits. Se
                // recorren uno a uno mirando que uso le toca: si hay
                // menos usos que campos, los sobrantes repiten el
                // ultimo (asi lo define la norma).
                for (uint32_t c = 0; c < num_campos; c++) {
                    uint32_t uso = 0;
                    if (n_usos > 0) uso = (c < n_usos) ? usos[c] : usos[n_usos - 1];
                    uint32_t aqui = bit + c * tam_campo;

                    if (pagina == 0x01 && uso == 0x30 && f->x_bits == 0) {
                        f->x_bit = aqui; f->x_bits = tam_campo; f->x_max = log_max;
                        f->id = id_actual;
                    } else if (pagina == 0x01 && uso == 0x31 && f->y_bits == 0) {
                        f->y_bit = aqui; f->y_bits = tam_campo; f->y_max = log_max;
                    } else if (pagina == 0x0D && uso == 0x42 && !f->hay_tip) {
                        f->tip_bit = aqui; f->hay_tip = true;
                    }
                }
                bit += tam_campo * num_campos;
            } else if (tag == 9 || tag == 11) {
                // Output y Feature tambien ocupan sitio en SUS propios
                // informes, no en el de entrada: no mueven 'bit'.
            }
            n_usos = 0;   // los usos son locales: se agotan con cada Main
        }
    }

    // Sin X e Y no hay nada que seguir. El dedo apoyado es opcional:
    // si el panel no lo declara, se da por apoyado siempre que mande
    // un informe, que es como se comportan los paneles mas simples.
    f->valido = (f->x_bits > 0 && f->y_bits > 0);
    if (f->x_max <= 0) f->x_max = 4095;   // valores de cordura por si el
    if (f->y_max <= 0) f->y_max = 4095;   // descriptor no los declara
    return f->valido;
}

// Saca 'nbits' empezando en 'bit' de un informe, en little-endian.
static uint32_t tactil_campo(const uint8_t *r, uint32_t len, uint32_t bit, uint32_t nbits) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < nbits && i < 32; i++) {
        uint32_t b = bit + i;
        if (b / 8 >= len) break;
        if (r[b / 8] & (1u << (b % 8))) v |= (1u << i);
    }
    return v;
}

// Traduce el informe CRUDO del panel al informe normalizado de 5 bytes
// que describe xhci_pi4.h. Todo lo que tiene de raro cada pantalla se
// queda aqui dentro: quien recibe el informe ve siempre lo mismo.
//
//   salida[0]    1 si hay un dedo apoyado
//   salida[1..2] X de 0 a 32767     salida[3..4] Y de 0 a 32767
//
// Devuelve false si el informe no es del formato que analizamos --por
// ejemplo, un panel multitactil manda ademas informes con otro Report
// ID (configuracion, numero de contactos) que no nos valen.
static bool tactil_normalizar(const uint8_t *crudo, uint32_t len, uint8_t *salida) {
    if (!tactil_fmt.valido || len == 0) return false;

    const uint8_t *r = crudo;
    if (tactil_fmt.id != 0) {
        // El primer byte es el Report ID; las posiciones que calculo el
        // analizador van desde el byte siguiente.
        if (crudo[0] != (uint8_t)tactil_fmt.id) return false;
        r = crudo + 1;
        len--;
        if (len == 0) return false;
    }

    uint32_t x = tactil_campo(r, len, tactil_fmt.x_bit, tactil_fmt.x_bits);
    uint32_t y = tactil_campo(r, len, tactil_fmt.y_bit, tactil_fmt.y_bits);

    // Escalado a 0..32767. Se hace en 64 bits porque x * 32767 con X de
    // 16 bits ya se sale de 32 bits con holgura.
    uint32_t xmax = (uint32_t)tactil_fmt.x_max, ymax = (uint32_t)tactil_fmt.y_max;
    if (x > xmax) x = xmax;
    if (y > ymax) y = ymax;
    uint32_t xn = (uint32_t)(((uint64_t)x * 32767u) / xmax);
    uint32_t yn = (uint32_t)(((uint64_t)y * 32767u) / ymax);

    salida[0] = tactil_fmt.hay_tip
              ? (uint8_t)tactil_campo(r, len, tactil_fmt.tip_bit, 1)
              : 1;   // panel que no declara el dedo: si manda informe, esta apoyado
    salida[1] = (uint8_t)(xn & 0xFF);
    salida[2] = (uint8_t)(xn >> 8);
    salida[3] = (uint8_t)(yn & 0xFF);
    salida[4] = (uint8_t)(yn >> 8);
    return true;
}

// Lee y muestra los descriptores. Devuelve la clase de dispositivo (o
// la de la primera interfaz si la del dispositivo es 0) en *clase_out.
static bool leer_descriptores(uint32_t slot, uint8_t *clase_out) {
    uint8_t *d = ctrl_buf;

    // Primero OCHO bytes: es lo unico que cabe seguro en el paquete
    // mas pequeno que puede tener un endpoint de control. Ver
    // ep0_ajustar_mps.
    if (control_in(slot, 0x80, REQ_GET_DESCRIPTOR, (DESC_DEVICE << 8), 0, d, 8) < 0) return false;
    uint32_t mps_real = d[7];
    // Un MPS0 valido es 8, 16, 32 o 64 (y 9 en SuperSpeed, donde el
    // byte guarda el exponente: 2^9 = 512). Cualquier otra cosa es un
    // descriptor corrupto, y hacerle caso seria peor que ignorarlo.
    if (mps_real == 9) mps_real = 512;
    if (mps_real == 8 || mps_real == 16 || mps_real == 32 || mps_real == 64 || mps_real == 512) {
        if (mps_real != ep0_mps_inicial(dev_speed[slot])) {
            uart_puts("xhci:   MPS0 real "); put_dec(mps_real);
            uart_puts(" (se supuso "); put_dec(ep0_mps_inicial(dev_speed[slot]));
            uart_puts("), corrigiendo\n");
            if (!ep0_ajustar_mps(slot, mps_real)) return false;
        }
    }

    // Ahora si, el descriptor entero.
    if (control_in(slot, 0x80, REQ_GET_DESCRIPTOR, (DESC_DEVICE << 8), 0, d, 18) < 0) return false;
    uint32_t vid = d[8] | (d[9] << 8), pid = d[10] | (d[11] << 8);
    uint8_t clase = d[4];
    uart_puts("xhci: DISPOSITIVO (slot "); put_dec(slot); uart_puts(")  USB "); put_hex32(d[3] << 8 | d[2]);
    uart_puts("  VID="); put_hex32(vid); uart_puts(" PID="); put_hex32(pid);
    uart_puts("  clase="); put_dec(d[4]); uart_puts("/"); put_dec(d[5]); uart_puts("/"); put_dec(d[6]);
    uart_puts("  MPS0="); put_dec(d[7]); uart_puts("\n");
    uint8_t iProduct = d[15];
    if (iProduct && control_in(slot, 0x80, REQ_GET_DESCRIPTOR, (DESC_STRING << 8) | iProduct, 0x0409, d, 64) >= 0) {
        uart_puts("xhci:   producto: \""); put_str_utf16(d); uart_puts("\"\n");
    }
    if (control_in(slot, 0x80, REQ_GET_DESCRIPTOR, (DESC_CONFIGURATION << 8), 0, d, 9) < 0) return false;
    uint16_t total = d[2] | (d[3] << 8);
    if (total > sizeof ctrl_buf) total = sizeof ctrl_buf;
    if (control_in(slot, 0x80, REQ_GET_DESCRIPTOR, (DESC_CONFIGURATION << 8), 0, d, total) < 0) return false;
    config_value = d[5];
    uart_puts("xhci:   configuracion "); put_dec(d[5]); uart_puts(": "); put_dec(d[4]);
    uart_puts(" interfaz(es), "); put_dec(total); uart_puts(" bytes\n");
    num_hid_ifs = 0;
    msc_if_encontrada = false; msc_ep_in = msc_ep_out = 0;
    bool en_msc = false;
    int tipo_if = 0;   // tipo HID boot de la interfaz actual (0 = ninguno)
    for (uint32_t i = 0; i + 1 < total; i += d[i] ? d[i] : 1) {
        if (d[i + 1] == DESC_INTERFACE) {
            if (clase == 0) clase = d[i + 5];
            tipo_if = 0;
            en_msc = (d[i + 5] == 8 && d[i + 6] == 6 && d[i + 7] == 0x50 && !msc_if_encontrada);
            if (en_msc) { msc_if_encontrada = true; msc_if_num = d[i + 2]; }
            if (d[i + 5] == 3 && d[i + 6] == 1 && (d[i + 7] == 1 || d[i + 7] == 2) && num_hid_ifs < 4) {
                tipo_if = d[i + 7] == 1 ? XHCI_HID_TECLADO : XHCI_HID_RATON;
                hid_ifs[num_hid_ifs] = (hid_if_t){ tipo_if, d[i + 2], 0, 0, 0 };
            } else if (d[i + 5] == 3 && num_hid_ifs < 4) {
                // HID que NO es teclado ni raton de
                // arranque: candidato a pantalla tactil. Aqui todavia
                // no se puede saber -- hace falta leer su Report
                // Descriptor, y eso ocurre al configurarla. Si resulta
                // no tener X e Y, se descarta alli.
                tipo_if = XHCI_HID_TACTIL;
                hid_ifs[num_hid_ifs] = (hid_if_t){ tipo_if, d[i + 2], 0, 0, 0 };
            }
            uart_puts("xhci:     interfaz "); put_dec(d[i + 2]);
            uart_puts(": clase "); put_dec(d[i + 5]); uart_puts("/"); put_dec(d[i + 6]); uart_puts("/"); put_dec(d[i + 7]);
            if (d[i + 5] == 3) uart_puts(d[i + 7] == 1 ? "  (HID teclado boot)" : d[i + 7] == 2 ? "  (HID raton boot)" : "  (HID)");
            else if (d[i + 5] == 9) uart_puts("  (hub)");
            else if (d[i + 5] == 8) uart_puts(d[i + 7] == 0x50 ? "  (Mass Storage, Bulk-Only)" : "  (Mass Storage, otro protocolo)");
            uart_puts("\n");
        } else if (d[i + 1] == DESC_ENDPOINT) {
            if (en_msc && (d[i + 3] & 3) == 2) {   // bulk
                uint32_t mps = d[i + 4] | (d[i + 5] << 8);
                if (d[i + 2] & 0x80) { if (!msc_ep_in)  { msc_ep_in  = d[i + 2]; msc_mps_in  = mps; } }
                else                 { if (!msc_ep_out) { msc_ep_out = d[i + 2]; msc_mps_out = mps; } }
            }
            if (tipo_if && hid_ifs[num_hid_ifs].ep == 0 && (d[i + 2] & 0x80) && (d[i + 3] & 3) == 3) {
                hid_ifs[num_hid_ifs].ep = d[i + 2];
                hid_ifs[num_hid_ifs].mps = d[i + 4] | (d[i + 5] << 8);
                hid_ifs[num_hid_ifs].intervalo = d[i + 6];
                num_hid_ifs++;
                tipo_if = 0;
            }
            uart_puts("xhci:       endpoint "); put_hex32(d[i + 2]);
            uart_puts(d[i + 2] & 0x80 ? " IN" : " OUT");
            uart_puts((d[i + 3] & 3) == 3 ? " interrupt" : (d[i + 3] & 3) == 2 ? " bulk" : " otro");
            uart_puts(", MPS="); put_dec(d[i + 4] | (d[i + 5] << 8));
            uart_puts(", intervalo="); put_dec(d[i + 6]); uart_puts("\n");
        }
    }
    if (clase_out) *clase_out = clase;
    return true;
}

// ---------------------------------------------------------------------
// Paso 5: endpoints de interrupcion IN de dispositivos HID
// ---------------------------------------------------------------------
static void hid_ring_init(hid_ep_t *h, uint32_t idx) {
    trb_t *r = hid_rings[idx];
    for (uint32_t i = 0; i < INT_RING_TRBS; i++) r[i] = (trb_t){0, 0, 0, 0};
    uint64_t base = xhci_dma_addr(r);
    r[INT_RING_TRBS - 1].d0 = (uint32_t)base;
    r[INT_RING_TRBS - 1].d1 = (uint32_t)(base >> 32);
    r[INT_RING_TRBS - 1].d3 = TRB_TYPE(TRB_TYPE_LINK) | TRB_TOGGLE_CYCLE;
    h->enq = 0; h->cycle = 1;
    cache_limpiar(r, INT_RING_TRBS * sizeof(trb_t));
}

// Encola un TRB Normal que lee un informe en hid_bufs[idx][buf].
static void hid_encolar_lectura(uint32_t idx, uint32_t buf) {
    hid_ep_t *h = &hid_eps[idx];
    trb_t *r = hid_rings[idx];
    cache_invalidar(hid_bufs[idx][buf], 64);
    uint64_t b = xhci_dma_addr(hid_bufs[idx][buf]);
    trb_t *t = &r[h->enq];
    t->d0 = (uint32_t)b; t->d1 = (uint32_t)(b >> 32);
    t->d2 = h->mps;
    t->d3 = TRB_TYPE(TRB_TYPE_NORMAL) | TRB_IOC | TRB_ISP | (h->cycle ? TRB_CYCLE : 0);
    h->informe_de_trb[h->enq] = buf;
    cache_limpiar(t, sizeof *t);
    if (++h->enq == INT_RING_TRBS - 1) {
        trb_t *l = &r[INT_RING_TRBS - 1];
        l->d3 = (l->d3 & ~TRB_CYCLE) | (h->cycle ? TRB_CYCLE : 0);
        cache_limpiar(l, sizeof *l);
        h->enq = 0; h->cycle ^= 1;
    }
}

// Interval del EP context: 2^Interval * 125us >= bInterval (ms en LS/FS)
static uint32_t intervalo_ep(uint32_t speed, uint32_t bInterval) {
    if (speed == 3) return bInterval ? (bInterval - 1 > 15 ? 15 : bInterval - 1) : 0;  // HS: 2^(bInterval-1) uframes
    uint32_t uframes = bInterval * 8, e = 0;
    while ((1u << (e + 1)) <= uframes && e < 15) e++;   // floor(log2)
    return e < 3 ? 3 : e;
}

// Configura una interfaz HID boot (teclado o raton) del slot: protocolo
// boot, Configure Endpoint de su EP IN, y lecturas en marcha.
static bool hid_configurar(uint32_t slot, const hid_if_t *hif) {
    // Buscar una entrada libre (reutilizar la de un dispositivo quitado)
    uint32_t idx_libre = num_hid;
    for (uint32_t i = 0; i < num_hid; i++) if (!hid_eps[i].activo) { idx_libre = i; break; }
    if (idx_libre >= MAX_HID) return false;
    // Protocolo boot para teclado y raton (subclase 1 = ambos lo
    // soportan): informes de formato fijo -- teclado 8 bytes, raton
    // botones/dx/dy[/rueda]. Si un raton lo ignorase y siguiera en
    // report protocol, input_pi4.c reconoce tambien el formato de 12
    // bits con report ID que devuelve el nuestro.
    if (hif->tipo != XHCI_HID_TACTIL) {
        // Solo teclados y ratones tienen protocolo de arranque. A un
        // digitalizador no se le puede pedir: no lo tiene, y la
        // peticion puede fallar o dejarlo en un estado raro.
        if (!control_nodata(slot, 0x21, HID_REQ_SET_PROTOCOL, 0 /* boot */, hif->interfaz)) return false;
    }
    // Idle 0 para todos (como Linux): informar solo cuando algo cambia.
    control_nodata(slot, 0x21, HID_REQ_SET_IDLE, 0, hif->interfaz);

    // Leer el descriptor de informe HID (peticion estandar a la
    // INTERFAZ, 0x81). Linux lo hace siempre; hay dispositivos baratos
    // que no empiezan a enviar informes hasta que el host lo ha leido.
    // No lo interpretamos (usamos los formatos boot), solo lo pedimos.
    // Para el tactil se piden los 512 del buffer entero y no 256: el
    // descriptor de un panel multitactil pasa de 256 bytes con
    // facilidad, y si se corta justo antes de X o de Y el analisis
    // falla sin que nada lo delate.
    int32_t largo_rd = control_in(slot, 0x81, REQ_GET_DESCRIPTOR, (DESC_HID_REPORT << 8),
                                 hif->interfaz, ctrl_buf,
                                 hif->tipo == XHCI_HID_TACTIL ? sizeof(ctrl_buf) : 256);

    // Para una pantalla tactil ese descriptor NO es un tramite: es la
    // unica forma de saber donde vienen las coordenadas. Si no se
    // puede leer, o no declara X e Y, no es un tactil que sepamos usar
    // y se deja pasar en vez de enganchar algo que no entendemos.
    if (hif->tipo == XHCI_HID_TACTIL) {
        if (largo_rd <= 0 || !tactil_analizar(ctrl_buf, (uint32_t)largo_rd, &tactil_fmt)) {
            // No es un fallo, y conviene que no lo parezca. Casi
            // siempre es la SEGUNDA interfaz de un teclado normal: la
            // de las teclas de volumen y multimedia, que es HID 3/0/0
            // igual que un digitalizador y por eso llega hasta aqui.
            // No la usamos todavia; el teclado de verdad ya quedo
            // configurado por su interfaz boot.
            uart_puts("xhci:   interfaz HID sin X/Y (teclas de medios u otra): sin usar.\n");
            return false;
        }
        uart_puts("xhci:   TACTIL: X en bit "); put_dec(tactil_fmt.x_bit);
        uart_puts(" ("); put_dec(tactil_fmt.x_bits); uart_puts(" bits, max ");
        put_dec((uint32_t)tactil_fmt.x_max); uart_puts("), Y en bit ");
        put_dec(tactil_fmt.y_bit); uart_puts(", dedo ");
        if (tactil_fmt.hay_tip) { uart_puts("en bit "); put_dec(tactil_fmt.tip_bit); }
        else uart_puts("no declarado (se dara por apoyado)");
        uart_puts("\n");
    }

    uint32_t idx = idx_libre;
    hid_ep_t *h = &hid_eps[idx];
    uint32_t dci = ((hif->ep & 0xF) << 1) | ((hif->ep & 0x80) ? 1 : 0);
    h->slot = slot; h->dci = dci; h->mps = hif->mps ? hif->mps : 8; h->tipo = hif->tipo;
    h->errores_seguidos = 0; h->abandonado = false; h->activo = false;
    hid_ring_init(h, idx);

    // Configure Endpoint: Add A0 (slot) + A<dci>. Context Entries = DCI
    // mas alto configurado hasta ahora en este slot.
    for (uint32_t i = 0; i < sizeof input_ctx / 4; i++) input_ctx[i] = 0;
    uint32_t *icc  = &input_ctx[0];
    uint32_t *sctx = &input_ctx[CTX_BYTES / 4];
    uint32_t *epc  = &input_ctx[(1 + dci) * CTX_BYTES / 4];
    cache_invalidar(device_ctx[slot], 32 * CTX_BYTES);
    for (uint32_t i = 0; i < CTX_BYTES / 4; i++) sctx[i] = device_ctx[slot][i];
    uint32_t entries = sctx[0] >> 27; if (dci > entries) entries = dci;
    sctx[0] = (sctx[0] & ~(0x1Fu << 27)) | (entries << 27);
    sctx[3] = 0;
    icc[1] = (1u << 0) | (1u << dci);

    uint32_t interval = intervalo_ep(dev_speed[slot], hif->intervalo ? hif->intervalo : 10);
    epc[0] = interval << 16;
    epc[1] = (3u << 1) | (7u << 3) | (h->mps << 16);      // CErr=3, Interrupt IN, MPS
    uint64_t deq = xhci_dma_addr(hid_rings[idx]) | 1u;
    epc[2] = (uint32_t)deq; epc[3] = (uint32_t)(deq >> 32);
    epc[4] = h->mps | (h->mps << 16);                       // Average TRB Length | Max ESIT Payload
    cache_limpiar(input_ctx, sizeof input_ctx);

    uint64_t ictx = xhci_dma_addr(input_ctx);
    uint32_t cc = cmd_ejecutar((uint32_t)ictx, (uint32_t)(ictx >> 32), 0,
                               TRB_TYPE(TRB_TYPE_CONFIGURE_EP) | (slot << 24), 0, 2000);
    if (cc != CC_SUCCESS) {
        uart_puts("xhci: Configure Endpoint (HID) FALLO (cc="); put_dec(cc); uart_puts(")\n");
        return false;
    }
    h->activo = true;
    if (idx == num_hid) num_hid++;
    for (uint32_t i = 0; i < NUM_INFORMES; i++) hid_encolar_lectura(idx, i);
    doorbell(slot, dci);
    uart_puts(hif->tipo == XHCI_HID_TECLADO ? "xhci: TECLADO configurado (slot "
            : hif->tipo == XHCI_HID_TACTIL  ? "xhci: TACTIL configurado (slot "
                                            : "xhci: RATON configurado (slot ");
    put_dec(slot); uart_puts(", EP DCI "); put_dec(dci); uart_puts(", MPS "); put_dec(h->mps);
    uart_puts(", interval="); put_dec(interval); uart_puts("). Leyendo informes.\n");
    return true;
}

static void (*hid_callback)(int tipo, const uint8_t *informe, uint32_t len) = 0;

// Recupera un EP HID que el HC ha dejado en estado Halted tras un error
// (USB Transaction Error, Stall, Split Transaction Error): Reset
// Endpoint, Set TR Dequeue Pointer al TRB siguiente al fallido, y
// doorbell para que vuelva a correr. Es lo mismo que hace Linux
// (xhci_cleanup_halted_endpoint).
static void hid_recuperar(uint32_t idx, uint32_t trb_fallido) {
    hid_ep_t *h = &hid_eps[idx];
    trb_t *r = hid_rings[idx];

    uint32_t cc = cmd_ejecutar(0, 0, 0,
        TRB_TYPE(TRB_TYPE_RESET_EP) | (h->slot << 24) | (h->dci << 16), 0, 1000);
    if (cc != CC_SUCCESS) {
        uart_puts("xhci: Reset Endpoint FALLO (cc="); put_dec(cc); uart_puts(")\n");
        return;
    }
    uint32_t sig = trb_fallido + 1;
    if (sig >= INT_RING_TRBS - 1) sig = 0;             // saltar el Link TRB
    cache_invalidar(&r[sig], sizeof(trb_t));
    uint64_t deq = xhci_dma_addr(&r[sig]) | (r[sig].d3 & TRB_CYCLE);   // DCS = cycle del TRB destino
    cc = cmd_ejecutar((uint32_t)deq, (uint32_t)(deq >> 32), 0,
        TRB_TYPE(TRB_TYPE_SET_TR_DEQ) | (h->slot << 24) | (h->dci << 16), 0, 1000);
    if (cc != CC_SUCCESS) {
        uart_puts("xhci: Set TR Dequeue FALLO (cc="); put_dec(cc); uart_puts(")\n");
        return;
    }
    doorbell(h->slot, h->dci);
    uart_puts("xhci: EP HID recuperado (slot "); put_dec(h->slot); uart_puts(")\n");
}

// Si el evento es de un EP HID conocido lo atiende y devuelve true:
// entrega el informe (si hay callback), repone el TRB, y si el EP quedo
// Halted por un error lo recupera.
static bool procesar_evento_hid(const trb_t *ev) {
    if (TRB_GET_TYPE(ev->d3) != TRB_TYPE_TRANSFER_EVENT) return false;
    uint32_t slot = ev->d3 >> 24, dci = (ev->d3 >> 16) & 0x1F;
    uint32_t idx;
    for (idx = 0; idx < num_hid; idx++)
        if (hid_eps[idx].activo && hid_eps[idx].slot == slot && hid_eps[idx].dci == dci) break;
    if (idx == num_hid) return false;

    hid_ep_t *h = &hid_eps[idx];
    uint32_t cc = ev->d2 >> 24;
    uint64_t ptr = ((uint64_t)ev->d1 << 32) | ev->d0;
    uint32_t i = (uint32_t)((ptr - xhci_dma_addr(hid_rings[idx])) / sizeof(trb_t));
    if (i >= INT_RING_TRBS - 1) return true;
    uint32_t buf = h->informe_de_trb[i];

    if (h->abandonado) return true;
    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
        h->errores_seguidos = 0;
        uint32_t len = h->mps - (ev->d2 & 0xFFFFFF);
        cache_invalidar(hid_bufs[idx][buf], 64);
        if (hid_callback && len) {
            if (h->tipo == XHCI_HID_TACTIL) {
                // El panel manda SU formato; arriba solo sube el nuestro.
                uint8_t norm[5];
                if (tactil_normalizar(hid_bufs[idx][buf], len, norm))
                    hid_callback(h->tipo, norm, 5);
            } else {
                hid_callback(h->tipo, hid_bufs[idx][buf], len);
            }
        }
        hid_encolar_lectura(idx, buf);
        doorbell(slot, dci);
    } else {
        if (++h->errores_seguidos > MAX_ERRORES_SEGUIDOS) {
            h->abandonado = true;
            uart_puts("xhci: EP HID (slot "); put_dec(slot);
            uart_puts(") abandonado tras errores repetidos (cc="); put_dec(cc); uart_puts(")\n");
            return true;
        }
        uart_puts("xhci: error en EP HID (slot "); put_dec(slot); uart_puts(", cc=");
        put_dec(cc); uart_puts(") -- recuperando\n");
        hid_encolar_lectura(idx, buf);
        if (cc == CC_USB_TRANSACTION_ERR || cc == CC_STALL || cc == CC_SPLIT_TRANSACTION_ERR)
            hid_recuperar(idx, i);
        else
            doorbell(slot, dci);
    }
    return true;
}

// Sondeo no bloqueante: procesa los eventos pendientes y entrega cada
// informe HID recibido a `cb` con su tipo.
void xhci_poll(void (*cb)(int tipo, const uint8_t *informe, uint32_t len)) {
    hid_callback = cb;
    trb_t ev;
    while (evt_intentar(&ev)) {
        procesar_evento_hid(&ev);   // Port Status Change Events: se detectan por sondeo (abajo)
    }
    // Conexion en caliente: cada 250 ms
    static uint64_t ultimo_ms = 0;
    uint64_t ahora = ms_ahora();
    if (ahora - ultimo_ms >= 250) {
        ultimo_ms = ahora;
        hotplug_sondeo();
    }
}

// ---------------------------------------------------------------------
// Mass Storage (pendrives): Bulk-Only Transport sobre dos EPs bulk, y
// los cinco comandos SCSI que hacen falta para leer/escribir sectores.
// Un solo dispositivo a la vez.
// ---------------------------------------------------------------------
#define BULK_RING_TRBS 64
__attribute__((aligned(PAGINA))) static trb_t bulk_in_ring[BULK_RING_TRBS];
__attribute__((aligned(PAGINA))) static trb_t bulk_out_ring[BULK_RING_TRBS];
__attribute__((aligned(PAGINA))) static uint8_t msc_datos[4096];   // bounce: nunca cruza 64KB
__attribute__((aligned(64)))     static uint8_t msc_cbw[64];       // 31 bytes usados
__attribute__((aligned(64)))     static uint8_t msc_csw[64];       // 13 bytes usados

static struct {
    bool     presente;
    uint32_t slot, dci_in, dci_out, mps_in, mps_out;
    uint32_t enq_in, cyc_in, enq_out, cyc_out;
    uint32_t tag;
    uint64_t sectores;
    uint32_t tam_sector;
    int      cambio;          // 0 nada, 1 conectado, 2 retirado (lo consume disk_pi4)
} msc;

static void bulk_ring_init(trb_t *r, uint32_t *enq, uint32_t *cyc) {
    for (uint32_t i = 0; i < BULK_RING_TRBS; i++) r[i] = (trb_t){0, 0, 0, 0};
    uint64_t base = xhci_dma_addr(r);
    r[BULK_RING_TRBS - 1].d0 = (uint32_t)base;
    r[BULK_RING_TRBS - 1].d1 = (uint32_t)(base >> 32);
    r[BULK_RING_TRBS - 1].d3 = TRB_TYPE(TRB_TYPE_LINK) | TRB_TOGGLE_CYCLE;
    *enq = 0; *cyc = 1;
    cache_limpiar(r, BULK_RING_TRBS * sizeof(trb_t));
}

static uint64_t bulk_encolar(trb_t *r, uint32_t *enq, uint32_t *cyc,
                             const void *buf, uint32_t len) {
    uint64_t b = xhci_dma_addr(buf);
    trb_t *t = &r[*enq];
    t->d0 = (uint32_t)b; t->d1 = (uint32_t)(b >> 32);
    t->d2 = len;
    t->d3 = TRB_TYPE(TRB_TYPE_NORMAL) | TRB_IOC | (*cyc ? TRB_CYCLE : 0);
    cache_limpiar(t, sizeof *t);
    uint64_t dma = xhci_dma_addr(t);
    if (++*enq == BULK_RING_TRBS - 1) {
        trb_t *l = &r[BULK_RING_TRBS - 1];
        l->d3 = (l->d3 & ~TRB_CYCLE) | (*cyc ? TRB_CYCLE : 0);
        cache_limpiar(l, sizeof *l);
        *enq = 0; *cyc ^= 1;
    }
    return dma;
}

// Recupera un EP bulk en Halted (STALL tipico del BOT) y devuelve el
// anillo al TRB siguiente al fallido.
static void bulk_recuperar(uint32_t dci, trb_t *r, uint64_t trb_dma) {
    cmd_ejecutar(0, 0, 0, TRB_TYPE(TRB_TYPE_RESET_EP) | (msc.slot << 24) | (dci << 16), 0, 1000);
    uint32_t i = (uint32_t)((trb_dma - xhci_dma_addr(r)) / sizeof(trb_t)) + 1;
    if (i >= BULK_RING_TRBS - 1) i = 0;
    cache_invalidar(&r[i], sizeof(trb_t));
    uint64_t deq = xhci_dma_addr(&r[i]) | (r[i].d3 & TRB_CYCLE);
    cmd_ejecutar((uint32_t)deq, (uint32_t)(deq >> 32), 0,
                 TRB_TYPE(TRB_TYPE_SET_TR_DEQ) | (msc.slot << 24) | (dci << 16), 0, 1000);
    doorbell(msc.slot, dci);
}

// Transferencia bulk sincrona. Devuelve el codigo de completado del
// HC (1 exito, 13 short packet, 6 stall...) y deja en *hecho los
// bytes transferidos.
static uint32_t bulk_transferir(bool in, void *buf, uint32_t len, uint32_t *hecho) {
    trb_t *r = in ? bulk_in_ring : bulk_out_ring;
    uint32_t *enq = in ? &msc.enq_in : &msc.enq_out, *cyc = in ? &msc.cyc_in : &msc.cyc_out;
    uint32_t dci = in ? msc.dci_in : msc.dci_out;
    if (in) cache_invalidar(buf, len); else cache_limpiar(buf, len);
    uint64_t dma = bulk_encolar(r, enq, cyc, buf, len);
    doorbell(msc.slot, dci);
    trb_t ev;
    while (evt_esperar(&ev, 3000)) {
        if (TRB_GET_TYPE(ev.d3) != TRB_TYPE_TRANSFER_EVENT) continue;
        if (procesar_evento_hid(&ev)) continue;
        uint64_t ptr = ((uint64_t)ev.d1 << 32) | ev.d0;
        if (ptr != dma) continue;
        uint32_t cc = ev.d2 >> 24;
        if (hecho) *hecho = len - (ev.d2 & 0xFFFFFF);
        if (in) cache_invalidar(buf, len);
        if (cc == CC_STALL || cc == CC_USB_TRANSACTION_ERR) bulk_recuperar(dci, r, dma);
        return cc;
    }
    if (hecho) *hecho = 0;
    return 0;   // timeout
}

// Un comando BOT completo: CBW (OUT) -> datos (IN/OUT) -> CSW (IN).
// Devuelve true si el CSW dice "bueno".
static bool bot_comando(const uint8_t *cdb, uint32_t cdb_len, void *datos, uint32_t datos_len, bool in) {
    uint32_t tag = ++msc.tag;
    for (int i = 0; i < 31; i++) msc_cbw[i] = 0;
    *(uint32_t *)&msc_cbw[0] = BOT_CBW_SIGNATURE;
    *(uint32_t *)&msc_cbw[4] = tag;
    *(uint32_t *)&msc_cbw[8] = datos_len;
    msc_cbw[12] = in ? BOT_FLAG_IN : 0;
    msc_cbw[13] = 0;               // LUN 0
    msc_cbw[14] = (uint8_t)cdb_len;
    for (uint32_t i = 0; i < cdb_len && i < 16; i++) msc_cbw[15 + i] = cdb[i];

    uint32_t hecho, cc;
    cc = bulk_transferir(false, msc_cbw, 31, &hecho);
    if (cc != CC_SUCCESS) return false;

    if (datos_len) {
        cc = bulk_transferir(in, datos, datos_len, &hecho);
        if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET && cc != CC_STALL) return false;
        // Un STALL en datos es legal (el dispositivo no tenia tanto); ya se
        // recupero el EP: seguimos al CSW.
    }

    cc = bulk_transferir(true, msc_csw, 13, &hecho);
    if (cc == CC_STALL) cc = bulk_transferir(true, msc_csw, 13, &hecho);   // reintento tras STALL
    if ((cc != CC_SUCCESS && cc != CC_SHORT_PACKET) || hecho < 13) return false;
    if (*(uint32_t *)&msc_csw[0] != BOT_CSW_SIGNATURE || *(uint32_t *)&msc_csw[4] != tag) return false;
    return msc_csw[12] == 0;
}

static bool scsi_test_unit_ready(void) {
    uint8_t cdb[6] = {SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0};
    return bot_comando(cdb, 6, 0, 0, true);
}
static bool scsi_request_sense(void) {
    uint8_t cdb[6] = {SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0};
    return bot_comando(cdb, 6, msc_datos, 18, true);
}
static bool scsi_inquiry(void) {
    uint8_t cdb[6] = {SCSI_INQUIRY, 0, 0, 0, 36, 0};
    if (!bot_comando(cdb, 6, msc_datos, 36, true)) return false;
    uart_puts("xhci: MSC INQUIRY: \"");
    for (int i = 8; i < 36; i++) uart_putc(msc_datos[i] >= 32 && msc_datos[i] < 127 ? (char)msc_datos[i] : ' ');
    uart_puts("\"\n");
    return true;
}
// Lee/escribe hasta XHCI_MSD_MAX_SECTORES sectores CONTIGUOS en una sola
// transaccion BOT (un CBW + una transferencia de datos + un CSW), en vez
// de un comando por cada 512 bytes. `buf` recibe/entrega los datos
// DIRECTAMENTE (sin pasar por msc_datos): debe tener sitio para
// count*512 bytes; el mantenimiento de cache lo hace bulk_transferir.
bool xhci_msd_leer_n(uint64_t lba, uint32_t count, void *buf) {
    if (!msc.presente || count == 0 || count > XHCI_MSD_MAX_SECTORES || lba + count > msc.sectores) return false;
    uint8_t cdb[10] = {SCSI_READ10, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                       0, (uint8_t)(count >> 8), (uint8_t)count, 0};
    for (int intento = 0; intento < 3; intento++) {
        if (bot_comando(cdb, 10, buf, count * 512, true)) return true;
        scsi_request_sense();
    }
    return false;
}

bool xhci_msd_escribir_n(uint64_t lba, uint32_t count, const void *buf) {
    if (!msc.presente || count == 0 || count > XHCI_MSD_MAX_SECTORES || lba + count > msc.sectores) return false;
    uint8_t cdb[10] = {SCSI_WRITE10, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                       0, (uint8_t)(count >> 8), (uint8_t)count, 0};
    for (int intento = 0; intento < 3; intento++) {
        if (bot_comando(cdb, 10, (void *)buf, count * 512, false)) return true;
        scsi_request_sense();
    }
    return false;
}

static bool scsi_read_capacity(void) {
    uint8_t cdb[10] = {SCSI_READ_CAPACITY10, 0,0,0,0,0,0,0,0,0};
    if (!bot_comando(cdb, 10, msc_datos, 8, true)) return false;
    uint32_t ultimo = ((uint32_t)msc_datos[0] << 24) | (msc_datos[1] << 16) | (msc_datos[2] << 8) | msc_datos[3];
    uint32_t tam    = ((uint32_t)msc_datos[4] << 24) | (msc_datos[5] << 16) | (msc_datos[6] << 8) | msc_datos[7];
    msc.sectores = (uint64_t)ultimo + 1; msc.tam_sector = tam;
    return tam == 512;
}

// Configura un pendrive BOT en el slot: EPs bulk, y lo deja listo.
static bool msc_configurar(uint32_t slot) {
    if (msc.presente) { uart_puts("xhci: ya hay un pendrive; se ignora el segundo.\n"); return false; }
    if (!msc_ep_in || !msc_ep_out) return false;
    msc.slot = slot;
    msc.dci_in  = ((msc_ep_in  & 0xF) << 1) | 1;
    msc.dci_out = ((msc_ep_out & 0xF) << 1);
    msc.mps_in = msc_mps_in ? msc_mps_in : 512;
    msc.mps_out = msc_mps_out ? msc_mps_out : 512;
    bulk_ring_init(bulk_in_ring, &msc.enq_in, &msc.cyc_in);
    bulk_ring_init(bulk_out_ring, &msc.enq_out, &msc.cyc_out);

    // Configure Endpoint: A0 + los dos bulk
    for (uint32_t i = 0; i < sizeof input_ctx / 4; i++) input_ctx[i] = 0;
    uint32_t *icc = &input_ctx[0], *sctx = &input_ctx[CTX_BYTES / 4];
    cache_invalidar(device_ctx[slot], 32 * CTX_BYTES);
    for (uint32_t i = 0; i < CTX_BYTES / 4; i++) sctx[i] = device_ctx[slot][i];
    uint32_t maxdci = msc.dci_in > msc.dci_out ? msc.dci_in : msc.dci_out;
    sctx[0] = (sctx[0] & ~(0x1Fu << 27)) | (maxdci << 27);
    sctx[3] = 0;
    icc[1] = (1u << 0) | (1u << msc.dci_in) | (1u << msc.dci_out);

    uint32_t *ein = &input_ctx[(1 + msc.dci_in) * CTX_BYTES / 4];
    ein[1] = (3u << 1) | (EP_TYPE_BULK_IN << 3) | (msc.mps_in << 16);
    uint64_t dq = xhci_dma_addr(bulk_in_ring) | 1u;
    ein[2] = (uint32_t)dq; ein[3] = (uint32_t)(dq >> 32);
    ein[4] = 3072;   // Average TRB Length (bulk, como Linux)

    uint32_t *eout = &input_ctx[(1 + msc.dci_out) * CTX_BYTES / 4];
    eout[1] = (3u << 1) | (EP_TYPE_BULK_OUT << 3) | (msc.mps_out << 16);
    dq = xhci_dma_addr(bulk_out_ring) | 1u;
    eout[2] = (uint32_t)dq; eout[3] = (uint32_t)(dq >> 32);
    eout[4] = 3072;
    cache_limpiar(input_ctx, sizeof input_ctx);

    uint64_t ictx = xhci_dma_addr(input_ctx);
    uint32_t cc = cmd_ejecutar((uint32_t)ictx, (uint32_t)(ictx >> 32), 0,
                               TRB_TYPE(TRB_TYPE_CONFIGURE_EP) | (slot << 24), 0, 2000);
    if (cc != CC_SUCCESS) { uart_puts("xhci: Configure Endpoint (MSC) FALLO (cc="); put_dec(cc); uart_puts(")\n"); return false; }

    // SCSI: identificar, esperar a que este listo, capacidad.
    scsi_inquiry();
    bool listo = false;
    for (int i = 0; i < 30 && !listo; i++) {
        listo = scsi_test_unit_ready();
        if (!listo) { scsi_request_sense(); esperar_ms(100); }
    }
    if (!listo) { uart_puts("xhci: MSC no responde a TEST UNIT READY.\n"); return false; }
    if (!scsi_read_capacity()) { uart_puts("xhci: MSC READ CAPACITY fallo o sector != 512.\n"); return false; }
    uart_puts("xhci: PENDRIVE listo (slot "); put_dec(slot); uart_puts("): ");
    put_dec((uint32_t)(msc.sectores / 2048)); uart_puts(" MB, sectores de 512.\n");
    msc.presente = true; msc.cambio = 1;
    return true;
}

// ---- API publica del pendrive ----
bool xhci_msd_presente(void) { return msc.presente; }
uint64_t xhci_msd_sectores(void) { return msc.presente ? msc.sectores : 0; }
int xhci_msd_cambio(void) { int c = msc.cambio; msc.cambio = 0; return c; }

bool xhci_msd_leer(uint64_t lba, void *buf) {
    return xhci_msd_leer_n(lba, 1, buf);
}

bool xhci_msd_escribir(uint64_t lba, const void *buf) {
    return xhci_msd_escribir_n(lba, 1, buf);
}

// ---------------------------------------------------------------------
// Enumeracion completa de UN dispositivo ya reseteado en su puerto:
// slot, Address Device, descriptores, y luego o bien sus interfaces
// HID, o bien -- si es un hub -- todos sus puertos con dispositivo.
// ---------------------------------------------------------------------
static uint32_t dispositivos_encontrados = 0;

static bool hub_enumerar_todos(uint32_t hub_slot);
static bool hub_puerto_enumerar(uint32_t hub_slot, uint32_t p);
static void slot_quitar(uint32_t slot);

static bool dispositivo_enumerar(uint32_t root_port, uint32_t speed,
                                 uint32_t route, uint32_t tt_slot, uint32_t tt_port,
                                 uint32_t hub_slot, uint32_t hub_port) {
    uint32_t slot = 0;
    uint32_t cc = cmd_ejecutar(0, 0, 0, TRB_TYPE(TRB_TYPE_ENABLE_SLOT), &slot, 1000);
    if (cc != CC_SUCCESS || slot == 0 || slot >= MAX_DEV) {
        uart_puts("xhci: Enable Slot FALLO (cc="); put_dec(cc); uart_puts(")\n");
        return false;
    }
    slots[slot] = (slot_info_t){ true, false, root_port, hub_slot, hub_port, 0,
                                 route, hub_slot ? slots[hub_slot].nivel + 1 : 0 };
    if (!slot_address_device(slot, root_port, speed, route, tt_slot, tt_port)) {
        // Enable Slot triunfo pero Address Device fallo (tipico si el
        // puerto no tiene corriente suficiente, como un hub de teclado
        // con un pendrive hambriento): sin esto el slot quedaba marcado
        // "en uso" para siempre, agotando los 8 disponibles poco a poco.
        slot_quitar(slot);
        return false;
    }
    uint8_t clase = 0;
    if (!leer_descriptores(slot, &clase)) return false;
    dispositivos_encontrados++;
    if (clase == 9) { slots[slot].es_hub = true; return hub_enumerar_todos(slot); }
    if (msc_if_encontrada) {
        if (!control_nodata(slot, 0x00, REQ_SET_CONFIGURATION, config_value, 0)) return false;
        return msc_configurar(slot);
    }
    if (num_hid_ifs == 0) return true;

    // SET_CONFIGURATION: pasa el dispositivo de Addressed a Configured.
    // Sin esto solo existe EP0: los endpoints de interrupcion no
    // responden.
    if (!control_nodata(slot, 0x00, REQ_SET_CONFIGURATION, config_value, 0)) {
        uart_puts("xhci: SET_CONFIGURATION FALLO\n");
        return false;
    }
    hid_if_t ifs[4]; uint32_t n = num_hid_ifs;    // copia local: hid_ifs es compartido
    for (uint32_t i = 0; i < n; i++) ifs[i] = hid_ifs[i];
    for (uint32_t i = 0; i < n; i++)
        if (ifs[i].ep) hid_configurar(slot, &ifs[i]);
    return true;
}

// Un puerto de hub con algo conectado: reset, velocidad, y enumerar al hijo.
static bool hub_puerto_enumerar(uint32_t hub_slot, uint32_t p) {
    uint8_t *d = ctrl_buf;
    uart_puts("xhci:   hub "); put_dec(hub_slot); uart_puts(" puerto "); put_dec(p);
    uart_puts(": CONECTADO, reseteando...\n");
    control_nodata(hub_slot, 0x23, REQ_CLEAR_FEATURE, HUB_FEAT_C_PORT_CONNECTION, p);
    if (!control_nodata(hub_slot, 0x23, REQ_SET_FEATURE, HUB_FEAT_PORT_RESET, p)) return false;
    uint32_t status = 0, change = 0, t = 0;
    do {
        esperar_ms(10);
        if (control_in(hub_slot, 0xA3, REQ_GET_STATUS, 0, p, d, 4) < 0) return false;
        status = d[0] | (d[1] << 8); change = d[2] | (d[3] << 8);
    } while (!(change & PC_RESET) && ++t < 50);
    if (!(change & PC_RESET)) { uart_puts("xhci:   el puerto no completo el reset.\n"); return false; }
    control_nodata(hub_slot, 0x23, REQ_CLEAR_FEATURE, HUB_FEAT_C_PORT_RESET, p);
    esperar_ms(10);

    uint32_t speed = (status & PS_LOW_SPEED) ? 2 : (status & PS_HIGH_SPEED) ? 3 : 1;
    uart_puts("xhci:   velocidad "); uart_puts(nombre_velocidad(speed)); uart_puts("\n");
    uint32_t tt_slot = (speed != 3 && dev_speed[hub_slot] == 3) ? hub_slot : 0;

    // ROUTE STRING ACUMULADO.
    //
    // El route string de xHCI son 20 bits, CUATRO POR CADA NIVEL de
    // hub: los bits 3..0 dicen por que puerto del primer hub se baja,
    // los 7..4 por que puerto del segundo, y asi hasta cinco niveles.
    // El controlador lo usa para saber a donde mandar cada paquete.
    //
    // Aqui antes se mandaba solo 'p', el puerto del hub actual. Con UN
    // hub funcionaba de casualidad, porque el nivel 1 son justo los
    // bits de abajo. Con dos hubs encadenados --que es lo que lleva
    // dentro cualquier caja con puertos propios-- el controlador
    // entendia "puerto p del PRIMER hub", donde no hay nada: el
    // Address Device salia bien (no usa el route para nada) y la
    // primera transferencia de control moria con Babble.
    //
    // Se heredan los niveles del padre y se anade el propio.
    uint32_t nivel_hijo = slots[hub_slot].nivel;          // 0 si el hub cuelga del raiz
    uint32_t route_hijo = slots[hub_slot].route;
    if (nivel_hijo < 5) {
        route_hijo |= (p & 0xF) << (4 * nivel_hijo);
    }

    return dispositivo_enumerar(slots[hub_slot].root_port, speed, route_hijo,
                                tt_slot, tt_slot ? p : 0, hub_slot, p);
}

// Hub: configurar, encender puertos, y enumerar CADA puerto con algo.
static bool hub_enumerar_todos(uint32_t hub_slot) {
    uint8_t *d = ctrl_buf;
    if (!control_nodata(hub_slot, 0x00, REQ_SET_CONFIGURATION, 1, 0)) return false;
    if (control_in(hub_slot, 0xA0, REQ_GET_DESCRIPTOR, (DESC_HUB << 8), 0, d, 9) < 0) return false;
    uint32_t nports = d[2], caract = d[3] | (d[4] << 8), pwr_ms = d[5] * 2, ttt = (caract >> 5) & 3;
    uart_puts("xhci: HUB (slot "); put_dec(hub_slot); uart_puts("): "); put_dec(nports);
    uart_puts(" puertos, alimentacion en "); put_dec(pwr_ms); uart_puts(" ms, TTT="); put_dec(ttt); uart_puts("\n");
    if (nports == 0 || nports > 15) return false;
    if (!slot_configure_hub(hub_slot, nports, ttt)) return false;
    slots[hub_slot].nports = nports;

    for (uint32_t p = 1; p <= nports; p++)
        control_nodata(hub_slot, 0x23, REQ_SET_FEATURE, HUB_FEAT_PORT_POWER, p);
    esperar_ms(pwr_ms + 100);

    for (uint32_t p = 1; p <= nports; p++) {
        if (control_in(hub_slot, 0xA3, REQ_GET_STATUS, 0, p, d, 4) < 0) continue;
        if (d[0] & PS_CONNECTION) hub_puerto_enumerar(hub_slot, p);
    }
    return true;
}

// ---------------------------------------------------------------------
// Conexion en caliente
// ---------------------------------------------------------------------

// Quita un dispositivo: sus EPs HID (avisando con un informe "todo
// suelto"), sus hijos si era un hub, y el slot en el HC (Disable Slot).
static void slot_quitar(uint32_t slot) {
    if (slot >= MAX_DEV || !slots[slot].usado) return;
    if (slots[slot].es_hub)
        for (uint32_t h = 1; h < MAX_DEV; h++)
            if (slots[h].usado && slots[h].hub_slot == slot) slot_quitar(h);
    static const uint8_t suelto[8] = {0};
    for (uint32_t i = 0; i < num_hid; i++) {
        if (!hid_eps[i].activo || hid_eps[i].slot != slot) continue;
        if (hid_callback) {
            uint32_t largo = hid_eps[i].tipo == XHCI_HID_TECLADO ? 8
                           : hid_eps[i].tipo == XHCI_HID_TACTIL  ? 5   // dedo levantado en 0,0
                                                                 : 3;
            hid_callback(hid_eps[i].tipo, suelto, largo);
        }
        hid_eps[i].activo = false;
    }
    if (msc.presente && msc.slot == slot) { msc.presente = false; msc.cambio = 2; uart_puts("xhci: pendrive retirado.\n"); }
    cmd_ejecutar(0, 0, 0, TRB_TYPE(TRB_TYPE_DISABLE_SLOT) | (slot << 24), 0, 1000);
    dcbaa[slot] = 0;
    cache_limpiar(dcbaa, sizeof dcbaa);
    slots[slot] = (slot_info_t){0};
    uart_puts("xhci: dispositivo del slot "); put_dec(slot); uart_puts(" retirado.\n");
}

static uint32_t slot_en(uint32_t root_port, uint32_t hub_slot, uint32_t hub_port) {
    for (uint32_t i = 1; i < MAX_DEV; i++)
        if (slots[i].usado && slots[i].root_port == root_port &&
            slots[i].hub_slot == hub_slot && slots[i].hub_port == hub_port) return i;
    return 0;
}

static uint64_t ms_ahora(void) {
    uint64_t f, t;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
    return (t * 1000) / f;
}

// Revisa puertos raiz y de hubs en busca de cambios de conexion.
static void hotplug_sondeo(void) {
    // Puertos raiz: bit CSC de PORTSC
    for (uint32_t p = 1; p <= num_ports; p++) {
        uint32_t sc = op32(OP_PORTSC(p));
        if (!(sc & PORTSC_CSC)) continue;
        op32_w(OP_PORTSC(p), (sc & ~PORTSC_RW1C & ~PORTSC_PED) | PORTSC_CSC);   // limpiar CSC
        uint32_t viejo = slot_en(p, 0, 0);
        if (viejo) slot_quitar(viejo);
        if (sc & PORTSC_CCS) {
            uart_puts("xhci: puerto raiz "); put_dec(p); uart_puts(": conectado, reseteando...\n");
            if (puerto_reset(p)) {
                uint32_t s2 = op32(OP_PORTSC(p));
                dispositivo_enumerar(p, PORTSC_SPEED(s2), 0, 0, 0, 0, 0);
            }
        } else {
            uart_puts("xhci: puerto raiz "); put_dec(p); uart_puts(": desconectado.\n");
        }
    }
    // Hubs: C_PORT_CONNECTION por puerto
    uint8_t *d = ctrl_buf;
    for (uint32_t hs = 1; hs < MAX_DEV; hs++) {
        if (!slots[hs].usado || !slots[hs].es_hub) continue;
        for (uint32_t p = 1; p <= slots[hs].nports; p++) {
            if (control_in(hs, 0xA3, REQ_GET_STATUS, 0, p, d, 4) < 0) continue;
            uint32_t status = d[0] | (d[1] << 8), change = d[2] | (d[3] << 8);
            if (!(change & PC_CONNECTION)) continue;
            control_nodata(hs, 0x23, REQ_CLEAR_FEATURE, HUB_FEAT_C_PORT_CONNECTION, p);
            uint32_t viejo = slot_en(slots[hs].root_port, hs, p);
            if (viejo) slot_quitar(viejo);
            if (status & PS_CONNECTION) hub_puerto_enumerar(hs, p);
            else { uart_puts("xhci:   hub "); put_dec(hs); uart_puts(" puerto "); put_dec(p); uart_puts(": desconectado.\n"); }
        }
    }
}

bool xhci_init_pi4(void) {
    cap_base = (volatile uint8_t *)pcie_xhci_base();

    uint32_t caplength  = cap_base[CAP_CAPLENGTH];
    uint32_t hciversion = *(volatile uint16_t *)(cap_base + CAP_HCIVERSION);
    uint32_t hcs1 = cap32(CAP_HCSPARAMS1);
    uint32_t hcc1 = cap32(CAP_HCCPARAMS1);
    uint32_t dboff  = cap32(CAP_DBOFF)  & ~0x3u;
    uint32_t rtsoff = cap32(CAP_RTSOFF) & ~0x1Fu;

    if (hciversion < 0x0090 || hciversion > 0x0200 || caplength < 0x20) {
        uart_puts("xhci: registros de capability no plausibles.\n");
        return false;
    }

    op_base = (volatile uint32_t *)(cap_base + caplength);
    rt_base = (volatile uint32_t *)(cap_base + rtsoff);
    db_base = (volatile uint32_t *)(cap_base + dboff);

    num_slots = hcs1 & 0xFF;
    num_ports = (hcs1 >> 24) & 0xFF;

    uart_puts("xhci: version "); put_hex32(hciversion);
    uart_puts(", slots="); put_dec(num_slots);
    uart_puts(", puertos="); put_dec(num_ports);
    uart_puts(", AC64="); uart_putc((hcc1 & 1) ? '1' : '0');
    uart_puts(", CSZ="); uart_putc((hcc1 & (1u << 2)) ? '1' : '0');
    uart_puts("\n");

    // Que version de USB habla cada puerto. Se lee del propio
    // controlador en vez de suponerlo (ver protocolos_leer).
    protocolos_leer(hcc1);

    // 1. Detener el controlador si esta corriendo (el firmware de la
    //    Pi lo dejo en "XHCI-STOP", pero no damos nada por hecho).
    if (!(op32(OP_USBSTS) & USBSTS_HCH)) {
        op32_w(OP_USBCMD, op32(OP_USBCMD) & ~USBCMD_RS);
        uint32_t t = 0;
        while (!(op32(OP_USBSTS) & USBSTS_HCH) && t++ < 100) esperar_ms(1);
        if (!(op32(OP_USBSTS) & USBSTS_HCH)) {
            uart_puts("xhci: el controlador no se detiene.\n");
            return false;
        }
    }

    // 2. Reset del controlador y espera a que termine (HCRST vuelve a
    //    0) y a que este listo (CNR a 0).
    op32_w(OP_USBCMD, op32(OP_USBCMD) | USBCMD_HCRST);
    {
        uint32_t t = 0;
        while ((op32(OP_USBCMD) & USBCMD_HCRST) && t++ < 500) esperar_ms(1);
        if (op32(OP_USBCMD) & USBCMD_HCRST) {
            uart_puts("xhci: HCRST no termina.\n");
            return false;
        }
        t = 0;
        while ((op32(OP_USBSTS) & USBSTS_CNR) && t++ < 500) esperar_ms(1);
        if (op32(OP_USBSTS) & USBSTS_CNR) {
            uart_puts("xhci: CNR no baja tras el reset.\n");
            return false;
        }
    }
    uart_puts("xhci: controlador reseteado. USBSTS=");
    put_hex32(op32(OP_USBSTS));
    uart_puts(" PAGESIZE=");
    put_hex32(op32(OP_PAGESIZE));
    uart_puts("\n");

    // 3. Estado de los puertos del root hub. Tras el reset los puertos
    //    tienen alimentacion (PP) y, si hay algo enchufado, CCS=1.
    //    Damos un momento a que se asiente la deteccion.
    esperar_ms(100);
    uint32_t conectados = 0;
    for (uint32_t p = 1; p <= num_ports; p++) {
        uint32_t sc = op32(OP_PORTSC(p));
        uart_puts("xhci: puerto "); put_dec(p);
        uart_puts(": PORTSC="); put_hex32(sc);
        if (sc & PORTSC_CCS) {
            conectados++;
            uart_puts("  CONECTADO");
            if (PORTSC_SPEED(sc) != 0) {
                // USB3: la velocidad se conoce al detectar el dispositivo.
                uart_puts(", velocidad ");
                uart_puts(nombre_velocidad(PORTSC_SPEED(sc)));
            } else if (puerto_es_usb3(p)) {
                // Conector USB 3 con algo dentro pero sin velocidad: el
                // aparato es USB 2 y se enumerara por su puerto gemelo.
                uart_puts(" (conector USB 3, enlace sin entrenar)");
            } else {
                // USB2: el puerto queda en Polling (PLS=7) y la velocidad
                // solo se conoce tras el reset del puerto (paso 2).
                uart_puts(" (USB 2.0, velocidad tras el reset del puerto)");
            }
        } else {
            uart_puts("  (vacio)");
        }
        uart_puts("\n");
    }
    uart_puts("xhci: dispositivos conectados: "); put_dec(conectados); uart_puts("\n");

    // ================= Paso 2: estructuras, arranque, comando =========

    // 4. Estructuras en RAM. Todas las direcciones que ve el HC pasan
    //    por xhci_dma_addr() (offset PCI de RC_BAR2).
    for (uint32_t i = 0; i <= MAX_SLOTS; i++) dcbaa[i] = 0;
    for (uint32_t i = 0; i < NUM_SCRATCHPAD; i++) {
        for (uint32_t j = 0; j < PAGINA; j += 8) *(uint64_t *)&scratchpad_pages[i][j] = 0;
        scratchpad_array[i] = xhci_dma_addr(scratchpad_pages[i]);
    }
    dcbaa[0] = xhci_dma_addr(scratchpad_array);   // entrada 0 = scratchpad
    cache_limpiar(scratchpad_pages, sizeof scratchpad_pages);
    cache_limpiar(scratchpad_array, sizeof scratchpad_array);
    cache_limpiar(dcbaa, sizeof dcbaa);

    op32_w(OP_CONFIG, MAX_SLOTS);                  // MaxSlotsEn
    op64_w(OP_DCBAAP, xhci_dma_addr(dcbaa));
    cmd_ring_init();
    op64_w(OP_CRCR, xhci_dma_addr(cmd_ring) | TRB_CYCLE);   // RCS = 1
    evt_ring_init();
    uart_puts("xhci: DCBAA, scratchpad (31 pag.), anillo de comandos y de eventos listos.\n");

    // 5. Arrancar el controlador.
    op32_w(OP_USBCMD, op32(OP_USBCMD) | USBCMD_RS);
    {
        uint32_t t = 0;
        while ((op32(OP_USBSTS) & USBSTS_HCH) && t++ < 100) esperar_ms(1);
        if (op32(OP_USBSTS) & USBSTS_HCH) {
            uart_puts("xhci: el controlador no arranca (HCH sigue a 1).\n");
            return false;
        }
    }
    hc_corriendo = true;
    uart_puts("xhci: controlador EN MARCHA. USBSTS=");
    put_hex32(op32(OP_USBSTS)); uart_puts("\n");

    // 6. Cada puerto raiz con dispositivo: reset, y enumeracion completa
    //    (si es un hub, tambien todo lo que cuelgue de el).
    for (uint32_t p = 1; p <= num_ports; p++) {
        if (!(op32(OP_PORTSC(p)) & PORTSC_CCS)) continue;
        uart_puts("xhci: puerto raiz "); put_dec(p);
        uart_puts(puerto_es_usb3(p) ? " (USB 3): reset...\n" : " (USB 2): reset...\n");
        if (!puerto_reset(p)) {
            // Caso normal y no preocupante: un aparato USB 2 metido en
            // un conector USB 3 aparece en los DOS puertos que el
            // controlador dedica a ese conector. El de USB 2 lo
            // enumera; el de USB 3 se queda con "algo conectado" y sin
            // enlace que entrenar, para siempre. No es un fallo.
            if (puerto_es_usb3(p))
                uart_puts("xhci:   sin enlace USB 3 (normal: el aparato de ese conector es USB 2).\n");
            else
                uart_puts("xhci:   no completo el reset.\n");
            continue;
        }
        uint32_t sc = op32(OP_PORTSC(p));
        uart_puts("xhci:   habilitado, velocidad "); uart_puts(nombre_velocidad(PORTSC_SPEED(sc))); uart_puts("\n");
        if (!puerto_con_dispositivo) puerto_con_dispositivo = p;
        dispositivo_enumerar(p, PORTSC_SPEED(sc), 0, 0, 0, 0, 0);
    }

    // Limpiar los bits de cambio (CSC, PEC, WRC, OCC, PRC, PLC, CEC) que los
    // puertos raiz traen pendientes desde el arranque: si no, el primer
    // sondeo de conexion en caliente los tomaria por cambios nuevos y
    // desmontaria y reenumeraria todo.
    for (uint32_t p = 1; p <= num_ports; p++) {
        uint32_t sc = op32(OP_PORTSC(p));
        op32_w(OP_PORTSC(p), (sc & ~PORTSC_RW1C & ~PORTSC_PED) | (sc & PORTSC_RW1C));
    }

    uart_puts("xhci: "); put_dec(dispositivos_encontrados); uart_puts(" dispositivo(s), ");
    put_dec(num_hid); uart_puts(" interfaz(es) HID configurada(s).\n");
    return num_hid > 0 || msc.presente;
}
