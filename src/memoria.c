// memoria.c -- Nemo OS: cuanta RAM hay y donde esta. Ver memoria.h.
#include <stdint.h>
#include <stdbool.h>
#include "memoria.h"

void uart_puts(const char *s);
void uart_putc(char c);

#ifndef NEMO_QEMU
#include "mailbox_pi4.h"
#endif

#define MB (1024ULL * 1024ULL)
#define GB (1024ULL * MB)

// Fin del GB donde vive el kernel: la RAM por debajo ya se usaba antes
// de este cambio; la de encima es la nueva, la que comprueba
// memoria_probar_alta.
#ifdef NEMO_QEMU
#define FIN_GB_KERNEL 0x80000000ULL      // RAM de QEMU virt desde 0x40000000
#else
#define FIN_GB_KERNEL 0x40000000ULL      // RAM de la Pi desde 0
#endif

static memoria_region_t regiones[MEMORIA_MAX_REGIONES];
static int num_regiones = 0;
static uint64_t total = 0;
#ifndef NEMO_QEMU
static uint32_t revision = 0;
static bool revision_ok = false;
#endif

static void anadir(uint64_t base, uint64_t fin) {
    if (fin <= base || num_regiones >= MEMORIA_MAX_REGIONES) return;
    regiones[num_regiones].base = base;
    regiones[num_regiones].tam = fin - base;
    num_regiones++;
}

void memoria_detectar(void) {
    num_regiones = 0;
#ifdef NEMO_QEMU
    // El Makefile pasa el mismo valor a QEMU (-m) y aqui (-DQEMU_RAM_MB).
#ifndef QEMU_RAM_MB
#define QEMU_RAM_MB 512
#endif
    total = (uint64_t)QEMU_RAM_MB * MB;
    anadir(0x40000000ULL, 0x40000000ULL + total);
#else
    // Revision de la placa, formato nuevo (bit 23 a 1): los bits 20-22
    // son el tamaño de la RAM: 0=256MB 1=512MB 2=1GB 3=2GB 4=4GB 5=8GB.
    // Si el mailbox falla o el formato es el antiguo, nos quedamos con
    // 1 GB: exactamente lo que se mapeaba antes, sin riesgo.
    total = 1 * GB;
    revision_ok = mailbox_revision_placa_pi4(&revision);
    if (revision_ok && (revision & (1u << 23))) {
        uint32_t codigo = (revision >> 20) & 7u;
        if (codigo <= 5) total = (256ULL * MB) << codigo;
    }
    // Mapa de la Pi 4 con los perifericos "bajos" (firmware por defecto):
    // la RAM por encima de 1 GB llega hasta 0xFC000000, donde empiezan
    // los perifericos; la de las placas de 8 GB sigue a partir de 4 GB.
    anadir(0, total < 1 * GB ? total : 1 * GB);
    if (total > 1 * GB) anadir(1 * GB, total < 0xFC000000ULL ? total : 0xFC000000ULL);
    if (total > 4 * GB) anadir(4 * GB, total);
#endif
}

int memoria_num_regiones(void) { return num_regiones; }
const memoria_region_t *memoria_region(int i) {
    return (i >= 0 && i < num_regiones) ? &regiones[i] : 0;
}
uint64_t memoria_total(void) { return total; }
uint64_t memoria_fin_gb_kernel(void) { return FIN_GB_KERNEL; }

bool memoria_es_ram(uint64_t pa, uint64_t tam) {
    for (int i = 0; i < num_regiones; i++) {
        uint64_t b = regiones[i].base, f = regiones[i].base + regiones[i].tam;
        if (pa >= b && pa + tam <= f) return true;
    }
    return false;
}

// ---- Informe por la terminal ----

static void poner_dec(uint64_t v) {
    char t[24]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) uart_putc(t[--k]);
}
static void poner_hex(uint64_t v, int cifras) {
    uart_puts("0x");
    for (int i = (cifras - 1) * 4; i >= 0; i -= 4) {
        uint32_t n = (uint32_t)(v >> i) & 0xF;
        uart_putc((char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
}

void memoria_informe(void) {
    uart_puts("memoria: ");
    poner_dec(total / MB);
    uart_puts(" MB de RAM");
#ifdef NEMO_QEMU
    uart_puts(" (QEMU, -m del Makefile)\n");
#else
    if (revision_ok) { uart_puts(" (revision de placa "); poner_hex(revision, 8); uart_puts(")\n"); }
    else uart_puts(" (el mailbox no respondio: se usa 1 GB, como antes)\n");
#endif
    for (int i = 0; i < num_regiones; i++) {
        uart_puts("  region "); poner_dec((uint64_t)i); uart_puts(": ");
        poner_hex(regiones[i].base, 9); uart_puts(" - ");
        poner_hex(regiones[i].base + regiones[i].tam, 9);
        uart_puts("  ("); poner_dec(regiones[i].tam / MB); uart_puts(" MB)\n");
    }
}

// ---- Comprobacion de la RAM nueva ----

// Escribe un patron, lo expulsa de la cache hasta la RAM (dc civac), y lo
// relee: lo leido viene de la RAM de verdad, no de una copia en cache. Y
// otra vez con el patron invertido, para no dar por buena una celda que
// casualmente ya tuviera el valor.
static bool probar_punto(uint64_t pa) {
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)pa;
    uint64_t patron = pa ^ 0x5AA5C33C0FF0A55AULL;
    *p = patron;
    __asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(pa) : "memory");
    uint64_t a = *p;
    *p = ~patron;
    __asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(pa) : "memory");
    uint64_t b = *p;
    return a == patron && b == ~patron;
}

// ---- Lo que probar_punto NO puede ver: el eco ----
//
// Escribir y releer LA MISMA direccion no demuestra que esa RAM exista. Si
// la placa tiene menos memoria de la que decimos, las direcciones altas
// hacen ECO sobre las bajas: escribir en 4 GB + X cae en X, releer 4 GB + X
// devuelve X, y la prueba pasa -- mientras acaba de machacar lo que hubiera
// en X, que es donde viven el kernel y las tareas. El sistema arranca
// diciendo que todo esta bien y luego se cae a pedazos de forma distinta en
// cada arranque, que es justo lo mas caro de diagnosticar.
//
// Para verlo hay que escribir en LOS DOS sitios y comprobar que no se
// estorban. Se guarda lo que hubiera en la direccion baja y se devuelve tal
// cual: aqui todavia no hay tareas y solo corre un nucleo, asi que nadie mas
// mira esa palabra mientras dura la prueba.
static bool hay_eco(uint64_t alta, uint64_t baja) {
    volatile uint64_t *pa = (volatile uint64_t *)(uintptr_t)alta;
    volatile uint64_t *pb = (volatile uint64_t *)(uintptr_t)baja;

    uint64_t guardado = *pb;                       // lo que hubiera ahi

    uint64_t marca_a = 0xA5A5A5A5DEADBEEFULL ^ alta;
    uint64_t marca_b = ~marca_a;                   // distinta a la fuerza

    *pa = marca_a;
    __asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(alta) : "memory");
    *pb = marca_b;
    __asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(baja) : "memory");

    uint64_t leido_alta = *pa;
    __asm__ volatile("dsb sy" ::: "memory");

    *pb = guardado;                                // devolver lo de antes
    __asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(baja) : "memory");

    // Si escribir abajo cambio lo de arriba, son la misma celda: hay eco.
    return leido_alta != marca_a;
}

int32_t memoria_probar_alta(void) {
    int32_t puntos = 0;
    for (int i = 0; i < num_regiones; i++) {
        uint64_t b = regiones[i].base, f = regiones[i].base + regiones[i].tam;
        if (b < FIN_GB_KERNEL) b = FIN_GB_KERNEL;     // solo la RAM nueva
        if (b >= f) continue;
        for (uint64_t pa = b; pa < f; pa += 64 * MB) {
            if (!probar_punto(pa)) return -1;
            puntos++;
        }
        if (!probar_punto(f - 8)) return -1;           // y la ultima palabra
        puntos++;
    }
    return puntos;
}

// Busca eco en la RAM que decimos tener por encima de 4 GB. Devuelve la RAM
// que de verdad hay, que puede ser menos de la que dijo la revision de la
// placa: si una de 4 GB se toma por una de 8, el tramo de arriba es un
// espejismo y todo lo que se ponga ahi pisa la memoria baja.
//
// Se mira en varios puntos porque un eco puede no ser exacto: algunas placas
// repiten cada 1 GB, otras cada 4.
uint64_t memoria_comprobar_eco(void) {
    if (total <= 4 * GB) return total;                 // nada que comprobar

    const uint64_t desplazamientos[] = { 4 * GB, 2 * GB, 1 * GB };
    uint64_t base_alta = 4 * GB;

    for (uint64_t pa = base_alta; pa < total; pa += 512 * MB) {
        for (int d = 0; d < 3; d++) {
            uint64_t baja = pa - desplazamientos[d];
            if (baja < 1 * MB) continue;               // no tocar el primer MB
            if (baja >= base_alta) continue;           // tiene que caer abajo
            if (hay_eco(pa, baja)) {
                uart_puts("memoria: ECO detectado -- la RAM de ");
                poner_hex(pa, 9);
                uart_puts(" es la misma que la de ");
                poner_hex(baja, 9);
                uart_puts("\n         Esta placa NO tiene la memoria que dice su revision.\n");
                return 4 * GB;                          // solo lo que se ha podido confirmar
            }
        }
    }
    return total;
}

// Recorta la RAM a lo comprobado y rehace los tramos. Se llama despues de
// memoria_comprobar_eco(): sin esto, el reparto de memoria a los programas
// seguiria entregando un tramo que no existe.
void memoria_recortar(uint64_t nueva_total) {
    if (nueva_total >= total) return;
    total = nueva_total;
    num_regiones = 0;
    anadir(0, total < 1 * GB ? total : 1 * GB);
    if (total > 1 * GB) anadir(1 * GB, total < 0xFC000000ULL ? total : 0xFC000000ULL);
    if (total > 4 * GB) anadir(4 * GB, total);
    uart_puts("memoria: recortada a lo que se ha podido comprobar de verdad.\n");
}
