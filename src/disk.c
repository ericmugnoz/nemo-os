// disk.c — Nemo OS
//
// Driver de disco virtio-blk con soporte para varios discos. Cada disco
// tiene su propia cola virtio independiente.
//
// IMPORTANTE (lección real, de las que vale la pena documentar en el
// libro): QEMU NO garantiza que el orden en que se declaran los discos
// en la línea de comandos (-drive/-device) coincida con el orden en que
// el kernel los "descubre" al escanear los slots de virtio-mmio. Para
// no depender de esa suposición, identificamos cada disco por un NUMERO
// DE SERIE (una cadena que le asignamos nosotros mismos al arrancar
// QEMU con 'serial=...') en vez de por su posición. Esto usa la
// petición estándar VIRTIO_BLK_T_GET_ID del protocolo virtio.

#include "disk.h"
#include "uart.h"

#define VIRTIO_MMIO_BASE   0x0a000000UL
#define VIRTIO_MMIO_STRIDE 0x200UL
#define VIRTIO_MMIO_SLOTS  32

#define VIRTIO_MAGIC       0x74726976UL
#define VIRTIO_DEVICE_ID_BLOCK 2

#define REG_MAGIC             0x000
#define REG_VERSION           0x004
#define REG_DEVICE_ID         0x008
#define REG_DRIVER_FEATURES   0x020
#define REG_DRIVER_FEATURES_SEL 0x024
#define REG_QUEUE_SEL         0x030
#define REG_QUEUE_NUM_MAX     0x034
#define REG_QUEUE_NUM         0x038
#define REG_QUEUE_READY       0x044
#define REG_QUEUE_NOTIFY      0x050
#define REG_STATUS            0x070
#define REG_QUEUE_DESC_LOW    0x080
#define REG_QUEUE_DESC_HIGH   0x084
#define REG_QUEUE_DRIVER_LOW  0x090
#define REG_QUEUE_DRIVER_HIGH 0x094
#define REG_QUEUE_DEVICE_LOW  0x0a0
#define REG_QUEUE_DEVICE_HIGH 0x0a4
#define REG_CONFIG            0x100

#define STATUS_ACKNOWLEDGE 1
#define STATUS_DRIVER      2
#define STATUS_DRIVER_OK   4
#define STATUS_FEATURES_OK 8

#define VIRTIO_F_VERSION_1_BIT 0

#define QUEUE_SIZE 8

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#define VIRTIO_BLK_T_IN     0
#define VIRTIO_BLK_T_OUT    1
#define VIRTIO_BLK_T_GET_ID 8

#define VIRTIO_BLK_ID_BYTES 20

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[QUEUE_SIZE];
};

struct virtio_blk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

__attribute__((aligned(4096))) static struct virtq_desc desc_table[MAX_DISKS][QUEUE_SIZE];
__attribute__((aligned(4096))) static struct virtq_avail avail_ring[MAX_DISKS];
__attribute__((aligned(4096))) static struct virtq_used used_ring[MAX_DISKS];

static struct virtio_blk_req_header req_header[MAX_DISKS];
static uint8_t req_status[MAX_DISKS];

// Todo esto esta indexado por "indice fisico" (el orden en que QEMU nos
// deja encontrar los dispositivos al escanear slots) -- NO es
// necesariamente el orden logico que le importa al resto del kernel.
static uint64_t mmio_base[MAX_DISKS];
static bool disk_ready[MAX_DISKS];
static uint16_t last_used_idx[MAX_DISKS];
static uint32_t disks_found = 0;

// Traduce "indice logico" (el que usa el resto del kernel, basado en el
// numero de serie) a "indice fisico" (el que realmente hay que usar
// para tocar los registros MMIO correctos).
static uint8_t logical_to_physical[MAX_DISKS];

static void uart_put_dec(uint64_t value) {
    if (value == 0) {
        uart_putc('0');
        return;
    }
    char digits[20];
    int n = 0;
    while (value > 0) {
        digits[n++] = '0' + (value % 10);
        value /= 10;
    }
    while (n > 0) {
        uart_putc(digits[--n]);
    }
}

static inline uint32_t mmio_read(uint8_t phys, uint32_t offset) {
    return *(volatile uint32_t *)(mmio_base[phys] + offset);
}
static inline void mmio_write(uint8_t phys, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(mmio_base[phys] + offset) = value;
}

static void find_block_devices(void) {
    for (int i = 0; i < VIRTIO_MMIO_SLOTS && disks_found < MAX_DISKS; i++) {
        uint64_t base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_STRIDE;
        uint32_t magic = *(volatile uint32_t *)(base + REG_MAGIC);
        if (magic != VIRTIO_MAGIC) continue;

        uint32_t device_id = *(volatile uint32_t *)(base + REG_DEVICE_ID);
        if (device_id == VIRTIO_DEVICE_ID_BLOCK) {
            mmio_base[disks_found] = base;
            disks_found++;
        }
    }
}

static bool init_one_disk(uint8_t phys) {
    mmio_write(phys, REG_STATUS, 0);
    mmio_write(phys, REG_STATUS, STATUS_ACKNOWLEDGE);
    mmio_write(phys, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    mmio_write(phys, REG_DRIVER_FEATURES_SEL, 0);
    mmio_write(phys, REG_DRIVER_FEATURES, 0);
    mmio_write(phys, REG_DRIVER_FEATURES_SEL, 1);
    mmio_write(phys, REG_DRIVER_FEATURES, 1 << VIRTIO_F_VERSION_1_BIT);

    mmio_write(phys, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!(mmio_read(phys, REG_STATUS) & STATUS_FEATURES_OK)) {
        uart_puts("disk: dispositivo rechazo la negociacion de features\n");
        return false;
    }

    mmio_write(phys, REG_QUEUE_SEL, 0);
    if (mmio_read(phys, REG_QUEUE_NUM_MAX) == 0) {
        uart_puts("disk: cola no disponible\n");
        return false;
    }
    mmio_write(phys, REG_QUEUE_NUM, QUEUE_SIZE);

    uint64_t desc_addr = (uint64_t)desc_table[phys];
    uint64_t avail_addr = (uint64_t)&avail_ring[phys];
    uint64_t used_addr = (uint64_t)&used_ring[phys];

    mmio_write(phys, REG_QUEUE_DESC_LOW,  (uint32_t)(desc_addr & 0xFFFFFFFF));
    mmio_write(phys, REG_QUEUE_DESC_HIGH, (uint32_t)(desc_addr >> 32));
    mmio_write(phys, REG_QUEUE_DRIVER_LOW,  (uint32_t)(avail_addr & 0xFFFFFFFF));
    mmio_write(phys, REG_QUEUE_DRIVER_HIGH, (uint32_t)(avail_addr >> 32));
    mmio_write(phys, REG_QUEUE_DEVICE_LOW,  (uint32_t)(used_addr & 0xFFFFFFFF));
    mmio_write(phys, REG_QUEUE_DEVICE_HIGH, (uint32_t)(used_addr >> 32));

    mmio_write(phys, REG_QUEUE_READY, 1);
    mmio_write(phys, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);

    disk_ready[phys] = true;
    last_used_idx[phys] = 0;
    return true;
}

// Version generica de la peticion virtio: sirve tanto para leer/escribir
// un sector (512 bytes) como para GET_ID (20 bytes) -- solo cambia el
// tamaño del buffer de datos y el tipo de peticion.
static bool submit_request(uint8_t phys, uint64_t sector, void *buf, uint32_t len, uint32_t type) {
    if (phys >= disks_found || !disk_ready[phys]) {
        return false;
    }

    req_header[phys].type = type;
    req_header[phys].reserved = 0;
    req_header[phys].sector = sector;
    req_status[phys] = 0xFF;

    desc_table[phys][0].addr  = (uint64_t)&req_header[phys];
    desc_table[phys][0].len   = sizeof(struct virtio_blk_req_header);
    desc_table[phys][0].flags = VIRTQ_DESC_F_NEXT;
    desc_table[phys][0].next  = 1;

    desc_table[phys][1].addr  = (uint64_t)buf;
    desc_table[phys][1].len   = len;
    desc_table[phys][1].flags = VIRTQ_DESC_F_NEXT | ((type == VIRTIO_BLK_T_OUT) ? 0 : VIRTQ_DESC_F_WRITE);
    desc_table[phys][1].next  = 2;

    desc_table[phys][2].addr  = (uint64_t)&req_status[phys];
    desc_table[phys][2].len   = 1;
    desc_table[phys][2].flags = VIRTQ_DESC_F_WRITE;
    desc_table[phys][2].next  = 0;

    __asm__ volatile("dsb sy" ::: "memory");

    uint16_t avail_slot = avail_ring[phys].idx % QUEUE_SIZE;
    avail_ring[phys].ring[avail_slot] = 0;

    __asm__ volatile("dsb sy" ::: "memory");
    avail_ring[phys].idx++;
    __asm__ volatile("dsb sy" ::: "memory");

    mmio_write(phys, REG_QUEUE_NOTIFY, 0);

    uint32_t attempts = 0;
    const uint32_t MAX_ATTEMPTS = 20000000;
    while (used_ring[phys].idx == last_used_idx[phys]) {
        __asm__ volatile("" ::: "memory");
        attempts++;
        if (attempts >= MAX_ATTEMPTS) {
            uart_puts("disk: TIMEOUT esperando respuesta del dispositivo\n");
            return false;
        }
    }
    last_used_idx[phys] = used_ring[phys].idx;

    return req_status[phys] == 0;
}

// Pide al dispositivo su numero de serie (20 bytes, relleno con ceros
// si es mas corto). Es una peticion estandar de virtio-blk, pensada
// exactamente para este caso: identificar discos sin depender del
// orden de deteccion.
static bool get_serial(uint8_t phys, char out[VIRTIO_BLK_ID_BYTES]) {
    for (int i = 0; i < VIRTIO_BLK_ID_BYTES; i++) out[i] = 0;
    return submit_request(phys, 0, out, VIRTIO_BLK_ID_BYTES, VIRTIO_BLK_T_GET_ID);
}

static bool serial_eq(const char *a, const char *b) {
    for (int i = 0; i < VIRTIO_BLK_ID_BYTES; i++) {
        char ca = a[i], cb = b[i];
        if (ca != cb) return false;
        if (ca == '\0' && cb == '\0') return true;
    }
    return true;
}

bool disk_init(void) {
    find_block_devices();
    if (disks_found == 0) {
        uart_puts("disk: no se encontro ningun dispositivo virtio-blk\n");
        return false;
    }

    for (uint32_t i = 0; i < disks_found; i++) {
        init_one_disk((uint8_t)i);
        logical_to_physical[i] = (uint8_t)i; // orden por defecto, por si algo falla abajo
    }

    // Preguntamos el numero de serie de cada disco fisico encontrado, y
    // construimos la traduccion "logico -> fisico": el disco logico 0
    // sera siempre 'NEMOSYS' (el disco de sistema de NemoFS) y el
    // logico 1 sera siempre 'NEMOFAT', sin importar en que orden los
    // haya encontrado QEMU.
    char serial[VIRTIO_BLK_ID_BYTES];
    for (uint32_t phys = 0; phys < disks_found; phys++) {
        if (!get_serial((uint8_t)phys, serial)) continue;

        uart_puts("disk: disco fisico ");
        uart_put_dec(phys);
        uart_puts(" -> serie '");
        uart_puts(serial);
        uart_puts("', capacidad = ");
        uint32_t cap_low  = mmio_read((uint8_t)phys, REG_CONFIG + 0);
        uint32_t cap_high = mmio_read((uint8_t)phys, REG_CONFIG + 4);
        uart_put_dec(((uint64_t)cap_high << 32) | cap_low);
        uart_puts(" sectores\n");

        if (serial_eq(serial, "NEMOSYS")) {
            logical_to_physical[0] = (uint8_t)phys;
        } else if (serial_eq(serial, "NEMOFAT")) {
            logical_to_physical[1] = (uint8_t)phys;
        }
    }

    return true;
}

uint32_t disk_count(void) {
    return disks_found;
}

uint64_t disk_capacity_sectors_n(uint8_t disk) {
    uint8_t phys = (disk < MAX_DISKS) ? logical_to_physical[disk] : disk;
    if (phys >= disks_found) return 0;
    uint32_t low  = mmio_read(phys, REG_CONFIG + 0);
    uint32_t high = mmio_read(phys, REG_CONFIG + 4);
    return ((uint64_t)high << 32) | low;
}

bool disk_read_sector_n(uint8_t disk, uint64_t sector, void *buf) {
    uint8_t phys = (disk < MAX_DISKS) ? logical_to_physical[disk] : disk;
    return submit_request(phys, sector, buf, SECTOR_SIZE, VIRTIO_BLK_T_IN);
}

bool disk_write_sector_n(uint8_t disk, uint64_t sector, const void *buf) {
    uint8_t phys = (disk < MAX_DISKS) ? logical_to_physical[disk] : disk;
    return submit_request(phys, sector, (void *)buf, SECTOR_SIZE, VIRTIO_BLK_T_OUT);
}
