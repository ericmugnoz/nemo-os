// mailbox_pi4.c -- comunicacion con el VideoCore por mailbox
//
// En QEMU, virtio-gpu/ramfb nos daba un framebuffer casi gratis.
// En hardware real no hay virtio de ningun tipo -- hay que
// pedirle un framebuffer al propio VideoCore (el procesador
// grafico de la Pi 4) a traves de un mecanismo de correo interno
// llamado mailbox: escribimos un mensaje en un buffer de memoria
// compartida, avisamos al VideoCore de que hay un mensaje nuevo,
// y esperamos su respuesta en el mismo buffer -- el VideoCore la
// escribe encima de la peticion original.
//
// Direcciones y formato de mensaje verificados contra el
// tutorial bare-metal especifico de Raspberry Pi 4 de rpi4os.com
// (isometimes/rpi4-osdev, parte 5), no contra documentacion
// generica de modelos anteriores con un mapa de memoria distinto.

#include <stdint.h>
#include "mailbox_pi4.h"

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

#define PERIPHERAL_BASE   0xFE000000UL
#define MAILBOX_BASE      (PERIPHERAL_BASE + 0xB880)

#define MAILBOX_READ      (*(volatile uint32_t *)(MAILBOX_BASE + 0x00))
#define MAILBOX_STATUS    (*(volatile uint32_t *)(MAILBOX_BASE + 0x18))
#define MAILBOX_WRITE     (*(volatile uint32_t *)(MAILBOX_BASE + 0x20))

#define MAILBOX_FULL      0x80000000
#define MAILBOX_EMPTY     0x40000000

#define MAILBOX_CH_PROP   8   // canal de la interfaz de propiedades

// El buffer del mensaje: debe estar alineado a 16 bytes, porque
// el mailbox solo transmite los 28 bits superiores de la
// direccion -- los 4 bits inferiores se usan para el numero de
// canal.
volatile uint32_t mensaje[36] __attribute__((aligned(16)));

// Etiquetas de la interfaz de propiedades que necesitamos para
// configurar un framebuffer basico
#define TAG_SET_ANCHO_ALTO_FISICO    0x00048003
#define TAG_SET_ANCHO_ALTO_VIRTUAL   0x00048004
#define TAG_SET_DESPLAZAMIENTO       0x00048009
#define TAG_SET_PROFUNDIDAD          0x00048005
#define TAG_SET_ORDEN_PIXEL          0x00048006
#define TAG_GET_FRAMEBUFFER          0x00040001
#define TAG_GET_PITCH                0x00040008
#define TAG_GET_MAC_ADDRESS           0x00010003
#define TAG_GET_REVISION_PLACA        0x00010002
#define TAG_GET_RELOJ                 0x00030002
#define TAG_FIN                      0x00000000

#define PETICION                      0x00000000
#define RESPUESTA_OK                  0x80000000

static void limpiar_cache_mensaje(void) {
    // "Limpia" (vuelca a RAM real) cada linea de cache que cubra el
    // buffer 'mensaje' -- necesario antes de que el VideoCore (que
    // lee RAM fisica directamente, sin pasar por nuestra cache) vea
    // la peticion que acabamos de escribir.
    uintptr_t base = (uintptr_t)&mensaje;
    uintptr_t fin = base + sizeof(mensaje);
    for (uintptr_t addr = base & ~63UL; addr < fin; addr += 64) {
        __asm__ volatile("dc cvac, %0" :: "r"(addr) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void invalidar_cache_mensaje(void) {
    // Descarta cualquier copia en cache de 'mensaje' -- necesario
    // para que la siguiente lectura traiga de verdad lo que el
    // VideoCore escribio en RAM fisica, no una copia nuestra ya
    // desactualizada.
    uintptr_t base = (uintptr_t)&mensaje;
    uintptr_t fin = base + sizeof(mensaje);
    for (uintptr_t addr = base & ~63UL; addr < fin; addr += 64) {
        __asm__ volatile("dc ivac, %0" :: "r"(addr) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t mailbox_llamar(uint8_t canal) {
    // Empaquetar: 28 bits de direccion + 4 bits de canal. El
    // buffer ya viene alineado a 16, asi que sus 4 bits bajos
    // estan garantizados en cero antes de esta operacion.
    uint32_t direccion = ((uint32_t)(uintptr_t)&mensaje & ~0xF) |
                          (canal & 0xF);

    limpiar_cache_mensaje();

    while (MAILBOX_STATUS & MAILBOX_FULL) {
        // esperar a que haya sitio para escribir
    }
    MAILBOX_WRITE = direccion;

    while (1) {
        while (MAILBOX_STATUS & MAILBOX_EMPTY) {
            // esperar a que llegue una respuesta
        }
        // Comprobar que la respuesta es para nuestro mensaje
        // concreto -- el mailbox es compartido, podria en
        // principio traer respuestas de otros canales
        if (MAILBOX_READ == direccion) {
            invalidar_cache_mensaje();
            return mensaje[1] == RESPUESTA_OK;
        }
    }
}

// Resultado de configurar el framebuffer, relleno por fb_init_pi4
// (el tipo framebuffer_t viene de mailbox_pi4.h)

framebuffer_t fb_init_pi4(uint32_t ancho, uint32_t alto) {
    framebuffer_t resultado = {0};

    mensaje[0] = 35 * 4;   // longitud total del mensaje en bytes
    mensaje[1] = PETICION;

    mensaje[2] = TAG_SET_ANCHO_ALTO_FISICO;
    mensaje[3] = 8;
    mensaje[4] = 8;
    mensaje[5] = ancho;
    mensaje[6] = alto;

    mensaje[7] = TAG_SET_ANCHO_ALTO_VIRTUAL;
    mensaje[8] = 8;
    mensaje[9] = 8;
    mensaje[10] = ancho;
    mensaje[11] = alto;

    mensaje[12] = TAG_SET_DESPLAZAMIENTO;
    mensaje[13] = 8;
    mensaje[14] = 8;
    mensaje[15] = 0;
    mensaje[16] = 0;

    mensaje[17] = TAG_SET_PROFUNDIDAD;
    mensaje[18] = 4;
    mensaje[19] = 4;
    mensaje[20] = 32;      // 32 bits por pixel (ARGB)

    mensaje[21] = TAG_SET_ORDEN_PIXEL;
    mensaje[22] = 4;
    mensaje[23] = 4;
    mensaje[24] = 1;       // 1 = RGB

    mensaje[25] = TAG_GET_FRAMEBUFFER;
    mensaje[26] = 8;
    mensaje[27] = 8;
    mensaje[28] = 4096;    // alineacion pedida
    mensaje[29] = 0;

    mensaje[30] = TAG_GET_PITCH;
    mensaje[31] = 4;
    mensaje[32] = 4;
    mensaje[33] = 0;

    mensaje[34] = TAG_FIN;

    uint32_t llamada_ok = mailbox_llamar(MAILBOX_CH_PROP);

    // Diagnostico detallado: cual de las tres condiciones fallo
    // exactamente, para no tener que adivinar a distancia.
    uart_puts("mailbox: llamada=");
    uart_puts(llamada_ok ? "OK" : "FALLO");
    uart_puts(" profundidad=");
    uart_put_hex32(mensaje[20]);
    uart_puts(" direccion_fb=");
    uart_put_hex32(mensaje[28]);
    uart_puts("\n");

    // ---- QUE ORDEN DE PIXEL CONCEDIO DE VERDAD ----
    //
    // Arriba se PIDE RGB (mensaje[24] = 1). El firmware contesta en ese mismo
    // hueco lo que ha concedido, y nadie lo leia nunca: se daba por hecho que
    // era BGR y fb_present cambia rojo por azul en cada pixel de cada
    // fotograma. Eso costaba 6,75 ms de los 13,4 de una composicion -- la
    // mitad -- asi que si el firmware concedio RGB, ese trabajo sobra entero.
    //
    // Se informa en vez de decidir solo: cambiar fb_present a ciegas y
    // equivocarse deja la pantalla con los colores cambiados, que es un fallo
    // evidente pero molesto de diagnosticar a distancia. Con este dato en el
    // arranque se decide con conocimiento.
    resultado.orden_pixel = mensaje[24];
    uart_puts("mailbox: orden de pixel pedido=1 (RGB), concedido=");
    uart_put_hex32(mensaje[24]);
    uart_puts(mensaje[24] == 1 ? "  -> ES RGB: la conversion de fb_present SOBRA\n"
                               : "  -> es BGR: la conversion hace falta\n");

    if (llamada_ok && mensaje[20] == 32 && mensaje[28] != 0) {
        // El VideoCore devuelve una direccion de "bus", no una
        // direccion fisica ARM directamente utilizable -- hay
        // que enmascarar los bits altos para convertirla.
        // Confirmado con codigo real y probado en Pi 4, no
        // asumido por analogia con modelos anteriores.
        mensaje[28] &= 0x3FFFFFFF;

        resultado.ancho = mensaje[10];
        resultado.alto = mensaje[11];
        resultado.pitch = mensaje[33];
        resultado.buffer = (uint8_t *)(uintptr_t)mensaje[28];
        resultado.ok = 1;
    }

    return resultado;
}

bool mailbox_get_mac_pi4(uint8_t mac[6]) {
    mensaje[0] = 8 * 4;   // longitud total del mensaje en bytes
    mensaje[1] = PETICION;

    mensaje[2] = TAG_GET_MAC_ADDRESS;
    mensaje[3] = 6;   // tamaño del buffer de respuesta
    mensaje[4] = 6;   // tamaño de la respuesta real
    mensaje[5] = 0;   // los 6 bytes de la MAC ocupan mensaje[5] y la
    mensaje[6] = 0;   // mitad baja de mensaje[6] -- se leen como bytes,
                      // no como enteros, para no depender del orden
    mensaje[7] = TAG_FIN;

    if (!mailbox_llamar(MAILBOX_CH_PROP)) return false;

    const uint8_t *bytes = (const uint8_t *)(uintptr_t)&mensaje[5];
    bool todo_cero = true;
    for (int i = 0; i < 6; i++) { mac[i] = bytes[i]; if (bytes[i] != 0) todo_cero = false; }
    return !todo_cero;
}


// Revision de la placa (un entero de 32 bits). En el formato nuevo (bit
// 23 a 1), los bits 20-22 dicen cuanta RAM tiene: lo usa memoria.c para
// saber que mapear. Se llama ANTES de encender la MMU: con la MMU apagada
// la cache de datos tampoco esta activa, y las operaciones de cache de
// mailbox_llamar no hacen ningun daño.
bool mailbox_revision_placa_pi4(uint32_t *revision) {
    mensaje[0] = 7 * 4;   // longitud total del mensaje en bytes
    mensaje[1] = PETICION;
    mensaje[2] = TAG_GET_REVISION_PLACA;
    mensaje[3] = 4;       // tamaño del buffer de respuesta
    mensaje[4] = 0;
    mensaje[5] = 0;       // aqui llega la revision
    mensaje[6] = TAG_FIN;
    if (!mailbox_llamar(MAILBOX_CH_PROP)) return false;
    *revision = mensaje[5];
    return mensaje[5] != 0;
}


// Frecuencia de un reloj del chip, en Hz (0 si el VideoCore no contesta).
// La usa el I2C (i2c.c) para calcular su divisor: el bus BSC cuenta con el
// reloj del NUCLEO (id 4), que en la Pi 4 suele ir a 500 MHz, pero el
// firmware puede cambiarlo.
uint32_t mailbox_reloj_pi4(uint32_t id) {
    mensaje[0] = 8 * 4;
    mensaje[1] = PETICION;
    mensaje[2] = TAG_GET_RELOJ;
    mensaje[3] = 8;       // tamaño del buffer de respuesta
    mensaje[4] = 0;
    mensaje[5] = id;      // que reloj
    mensaje[6] = 0;       // aqui llega la frecuencia
    mensaje[7] = TAG_FIN;
    if (!mailbox_llamar(MAILBOX_CH_PROP)) return 0;
    return mensaje[6];
}
