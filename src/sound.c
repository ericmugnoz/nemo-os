// sound.c — Nemo OS
//
// Driver de audio virtio-sound. Mismo transporte virtio-mmio que
// disk.c (escaneo por 'magic', negociacion de features, colas
// descriptor/avail/used) pero con CUATRO colas en vez de una sola
// (control/evento/tx/rx, en ese orden fijo segun el protocolo) --
// solo usamos activamente 'control' (para configurar el stream) y
// 'tx' (para enviar las muestras a reproducir); 'evento'/'rx' se
// configuran igual (el dispositivo las exige) pero no se usan.
//
// Los valores exactos de la especificacion (indices de VIRTIO_SND_R_*,
// VIRTIO_SND_PCM_FMT_*, VIRTIO_SND_PCM_RATE_*, tamaños de estructura)
// se verificaron contra el header oficial 'virtio_snd.h' del kernel
// Linux antes de escribir esto -- un borrador antiguo de la
// especificacion tenia el enum de frecuencias SIN 'RATE_5512' al
// principio, lo que habria desplazado todos los indices y roto la
// negociacion en silencio.
//
// LIMITACION DOCUMENTADA (V1): reproduccion SINCRONA -- sound_play_
// blocking() no vuelve hasta que el sonido entero termina de sonar,
// asi que de momento NO hay polifonia real (dos sonidos a la vez) ni
// el programa puede seguir haciendo otras cosas mientras suena. Para
// tener canales de verdad (el modelo ChannelXxx de BlitzPlus) haria
// falta reescribir esto sobre envios NO bloqueantes a la cola TX,
// consultando el anillo 'used' desde el bucle principal en vez de
// esperar aqui -- una ampliacion real, no solo un ajuste.

#include "sound.h"
#include "uart.h"

#define VIRTIO_MMIO_BASE   0x0a000000UL
#define VIRTIO_MMIO_STRIDE 0x200UL
#define VIRTIO_MMIO_SLOTS  32

#define VIRTIO_MAGIC            0x74726976UL
#define VIRTIO_DEVICE_ID_SOUND  25

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

#define STATUS_ACKNOWLEDGE 1
#define STATUS_DRIVER      2
#define STATUS_DRIVER_OK   4
#define STATUS_FEATURES_OK 8

#define VIRTIO_F_VERSION_1_BIT 0

#define QUEUE_SIZE 8

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

// -- indices de cola fijos segun la especificacion virtio-sound --
#define VIRTIO_SND_VQ_CONTROL 0
#define VIRTIO_SND_VQ_EVENT   1
#define VIRTIO_SND_VQ_TX      2
#define VIRTIO_SND_VQ_RX      3
#define VIRTIO_SND_VQ_COUNT   4

// -- codigos de peticion/respuesta de control (verificados contra
// virtio_snd.h oficial) --
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101
#define VIRTIO_SND_R_PCM_PREPARE    0x0102
#define VIRTIO_SND_R_PCM_START      0x0104

#define VIRTIO_SND_S_OK 0x8000

#define VIRTIO_SND_PCM_FMT_S16      5  // verificado: indice 5 del enum de formatos
#define VIRTIO_SND_PCM_RATE_44100   6  // verificado: indice 6 del enum de frecuencias (RATE_5512 es el 0)

// Tamaños de buffer/periodo que le anunciamos al dispositivo (no son
// un limite estricto por envio, son una pista de cuanto quiere
// almacenar internamente) -- generosos para no limitar demasiado la
// duracion de los sonidos que podemos mandar de una vez.
#define SOUND_BUFFER_BYTES (1024u * 1024u)
#define SOUND_PERIOD_BYTES (256u * 1024u)

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

// -- estructuras del protocolo de control virtio-sound (layout exacto
// verificado contra include/uapi/linux/virtio_snd.h) --
struct virtio_snd_hdr {
    uint32_t code;
};
struct virtio_snd_pcm_hdr {
    struct virtio_snd_hdr hdr;
    uint32_t stream_id;
};
struct virtio_snd_pcm_set_params {
    struct virtio_snd_pcm_hdr hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t channels;
    uint8_t format;
    uint8_t rate;
    uint8_t padding;
};
struct virtio_snd_pcm_xfer {
    uint32_t stream_id;
};
struct virtio_snd_pcm_status {
    uint32_t status;
    uint32_t latency_bytes;
};

__attribute__((aligned(4096))) static struct virtq_desc desc_table[VIRTIO_SND_VQ_COUNT][QUEUE_SIZE];
__attribute__((aligned(4096))) static struct virtq_avail avail_ring[VIRTIO_SND_VQ_COUNT];
__attribute__((aligned(4096))) static struct virtq_used used_ring[VIRTIO_SND_VQ_COUNT];
static uint16_t last_used_idx[VIRTIO_SND_VQ_COUNT];

static uint64_t g_mmio_base = 0;
static bool g_device_ready = false;

static inline uint32_t mmio_read(uint32_t offset) {
    return *(volatile uint32_t *)(g_mmio_base + offset);
}
static inline void mmio_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(g_mmio_base + offset) = value;
}

static bool find_sound_device(void) {
    for (int i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
        uint64_t base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_STRIDE;
        uint32_t magic = *(volatile uint32_t *)(base + REG_MAGIC);
        if (magic != VIRTIO_MAGIC) continue;
        uint32_t device_id = *(volatile uint32_t *)(base + REG_DEVICE_ID);
        if (device_id == VIRTIO_DEVICE_ID_SOUND) {
            g_mmio_base = base;
            return true;
        }
    }
    return false;
}

static bool init_queue(uint32_t qidx) {
    mmio_write(REG_QUEUE_SEL, qidx);
    if (mmio_read(REG_QUEUE_NUM_MAX) == 0) return false;
    mmio_write(REG_QUEUE_NUM, QUEUE_SIZE);

    uint64_t desc_addr = (uint64_t)desc_table[qidx];
    uint64_t avail_addr = (uint64_t)&avail_ring[qidx];
    uint64_t used_addr = (uint64_t)&used_ring[qidx];

    mmio_write(REG_QUEUE_DESC_LOW,  (uint32_t)(desc_addr & 0xFFFFFFFF));
    mmio_write(REG_QUEUE_DESC_HIGH, (uint32_t)(desc_addr >> 32));
    mmio_write(REG_QUEUE_DRIVER_LOW,  (uint32_t)(avail_addr & 0xFFFFFFFF));
    mmio_write(REG_QUEUE_DRIVER_HIGH, (uint32_t)(avail_addr >> 32));
    mmio_write(REG_QUEUE_DEVICE_LOW,  (uint32_t)(used_addr & 0xFFFFFFFF));
    mmio_write(REG_QUEUE_DEVICE_HIGH, (uint32_t)(used_addr >> 32));

    mmio_write(REG_QUEUE_READY, 1);
    last_used_idx[qidx] = 0;
    return true;
}

// Envio SINCRONO generico de una peticion (la cadena de descriptores
// ya debe estar rellenada en desc_table[q], empezando en el indice 0)
// por la cola 'q' -- espera (bloqueando) a que el dispositivo
// termine, con un limite de intentos para no colgarse para siempre
// si algo va mal.
static bool submit_and_wait(uint32_t q, uint64_t max_attempts) {
    __asm__ volatile("dsb sy" ::: "memory");
    uint16_t slot = avail_ring[q].idx % QUEUE_SIZE;
    avail_ring[q].ring[slot] = 0; // siempre empezamos la cadena en el descriptor 0
    __asm__ volatile("dsb sy" ::: "memory");
    avail_ring[q].idx++;
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_write(REG_QUEUE_NOTIFY, q);

    uint64_t attempts = 0;
    while (used_ring[q].idx == last_used_idx[q]) {
        __asm__ volatile("" ::: "memory");
        attempts++;
        if (attempts >= max_attempts) {
            uart_puts("sound: TIMEOUT esperando al dispositivo\n");
            return false;
        }
    }
    last_used_idx[q] = used_ring[q].idx;
    return true;
}

// Peticion de control generica: escribe 'req' (req_len bytes) y lee
// 'resp' (resp_len bytes) -- 2 descriptores en la cola de control.
static bool control_request(void *req, uint32_t req_len, void *resp, uint32_t resp_len) {
    if (!g_device_ready) return false;
    uint32_t q = VIRTIO_SND_VQ_CONTROL;

    desc_table[q][0].addr = (uint64_t)req;
    desc_table[q][0].len = req_len;
    desc_table[q][0].flags = VIRTQ_DESC_F_NEXT;
    desc_table[q][0].next = 1;

    desc_table[q][1].addr = (uint64_t)resp;
    desc_table[q][1].len = resp_len;
    desc_table[q][1].flags = VIRTQ_DESC_F_WRITE;
    desc_table[q][1].next = 0;

    return submit_and_wait(q, 20000000);
}

static bool pcm_set_params(uint32_t stream_id, uint8_t channels, uint8_t format, uint8_t rate) {
    struct virtio_snd_pcm_set_params req;
    req.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    req.hdr.stream_id = stream_id;
    req.buffer_bytes = SOUND_BUFFER_BYTES;
    req.period_bytes = SOUND_PERIOD_BYTES;
    req.features = 0;
    req.channels = channels;
    req.format = format;
    req.rate = rate;
    req.padding = 0;

    struct virtio_snd_hdr resp;
    resp.code = 0xFFFFFFFF;
    if (!control_request(&req, sizeof(req), &resp, sizeof(resp))) return false;
    return resp.code == VIRTIO_SND_S_OK;
}

static bool pcm_simple_cmd(uint32_t stream_id, uint32_t code) {
    struct virtio_snd_pcm_hdr req;
    req.hdr.code = code;
    req.stream_id = stream_id;

    struct virtio_snd_hdr resp;
    resp.code = 0xFFFFFFFF;
    if (!control_request(&req, sizeof(req), &resp, sizeof(resp))) return false;
    return resp.code == VIRTIO_SND_S_OK;
}

bool sound_init(void) {
    g_device_ready = false;

    if (!find_sound_device()) {
        uart_puts("sound: no se encontro ningun dispositivo virtio-sound\n");
        return false;
    }

    mmio_write(REG_STATUS, 0);
    mmio_write(REG_STATUS, STATUS_ACKNOWLEDGE);
    mmio_write(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    mmio_write(REG_DRIVER_FEATURES_SEL, 0);
    mmio_write(REG_DRIVER_FEATURES, 0);
    mmio_write(REG_DRIVER_FEATURES_SEL, 1);
    mmio_write(REG_DRIVER_FEATURES, 1 << VIRTIO_F_VERSION_1_BIT);

    mmio_write(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!(mmio_read(REG_STATUS) & STATUS_FEATURES_OK)) {
        uart_puts("sound: el dispositivo rechazo la negociacion de features\n");
        return false;
    }

    for (uint32_t q = 0; q < VIRTIO_SND_VQ_COUNT; q++) {
        if (!init_queue(q)) {
            uart_puts("sound: fallo al configurar una de las 4 colas\n");
            return false;
        }
    }

    mmio_write(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    g_device_ready = true;

    if (!pcm_set_params(0, 2, VIRTIO_SND_PCM_FMT_S16, VIRTIO_SND_PCM_RATE_44100)) {
        uart_puts("sound: SET_PARAMS del stream 0 fallo\n");
        g_device_ready = false;
        return false;
    }
    if (!pcm_simple_cmd(0, VIRTIO_SND_R_PCM_PREPARE)) {
        uart_puts("sound: PREPARE del stream 0 fallo\n");
        g_device_ready = false;
        return false;
    }
    if (!pcm_simple_cmd(0, VIRTIO_SND_R_PCM_START)) {
        uart_puts("sound: START del stream 0 fallo\n");
        g_device_ready = false;
        return false;
    }

    uart_puts("sound: listo (stream 0, 44100Hz 16bit estereo)\n");
    return true;
}

bool sound_available(void) {
    return g_device_ready;
}

static struct virtio_snd_pcm_xfer tx_xfer_hdr;
static struct virtio_snd_pcm_status tx_status;

void sound_play_blocking(const int16_t *samples, uint32_t frame_count) {
    if (!g_device_ready || frame_count == 0) return;
    uint32_t q = VIRTIO_SND_VQ_TX;
    uint32_t data_bytes = frame_count * 2 * (uint32_t)sizeof(int16_t); // 2 canales

    tx_xfer_hdr.stream_id = 0;
    tx_status.status = 0xFFFFFFFF;

    desc_table[q][0].addr = (uint64_t)&tx_xfer_hdr;
    desc_table[q][0].len = sizeof(tx_xfer_hdr);
    desc_table[q][0].flags = VIRTQ_DESC_F_NEXT;
    desc_table[q][0].next = 1;

    desc_table[q][1].addr = (uint64_t)samples;
    desc_table[q][1].len = data_bytes;
    desc_table[q][1].flags = VIRTQ_DESC_F_NEXT;
    desc_table[q][1].next = 2;

    desc_table[q][2].addr = (uint64_t)&tx_status;
    desc_table[q][2].len = sizeof(tx_status);
    desc_table[q][2].flags = VIRTQ_DESC_F_WRITE;
    desc_table[q][2].next = 0;

    // limite de intentos mas alto que en control_request -- un sonido
    // de verdad puede tardar bastante en terminar de sonar.
    submit_and_wait(q, 2000000000ULL);
}
