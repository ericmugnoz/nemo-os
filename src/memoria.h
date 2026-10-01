// memoria.h -- Nemo OS: cuanta RAM hay y donde esta
//
// Antes, las dos MMU mapeaban solo el primer GB de RAM, sin preguntar
// cuanta habia: en una Pi 4 de 4 GB, Nemo OS veia una cuarta parte. Este
// modulo averigua los rangos de direcciones que son RAM DE VERDAD, antes
// de encender la MMU, para que las tablas los mapeen todos -- y SOLO
// esos: mapear como memoria normal una direccion donde no hay RAM es
// peligroso en hardware real (la CPU lee por adelantado de lo que cree
// que es memoria, y eso puede acabar en errores imposibles de depurar).
//
//   QEMU (virt): una sola region desde 0x40000000, del tamaño que fija el
//     Makefile con la MISMA variable que se le pasa a QEMU en -m
//     (QEMU_RAM_MB), asi que no pueden discrepar.
//   Pi 4: el tamaño sale de la revision de la placa (mailbox), y las
//     regiones del mapa fijo de la Pi 4 en modo de perifericos "bajos"
//     (el que usa su firmware por defecto): [0, 1 GB), [1 GB, min(total,
//     0xFC000000)) y, en las de 8 GB, [4 GB, total). Los ultimos 64 MB
//     por debajo de 4 GB son los perifericos, no RAM.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MEMORIA_MAX_REGIONES 4

typedef struct { uint64_t base, tam; } memoria_region_t;

// Llamar ANTES de mmu_init: las tablas se construyen con lo que diga.
void memoria_detectar(void);

int memoria_num_regiones(void);
const memoria_region_t *memoria_region(int i);
uint64_t memoria_total(void);              // RAM total de la placa, en bytes

// true si TODO [pa, pa+tam) cae dentro de una region de RAM.
bool memoria_es_ram(uint64_t pa, uint64_t tam);

// Donde acaba el GB en el que vive el kernel (0x80000000 en QEMU,
// 0x40000000 en la Pi). La RAM por encima es la que se reparte a los
// programas (tasks.c).
uint64_t memoria_fin_gb_kernel(void);

// Por la terminal: el tamaño detectado y las regiones.
void memoria_informe(void);

// Comprobacion tras encender la MMU: escribe y relee (pasando por la RAM,
// no por la cache) una palabra cada 64 MB de toda la RAM que antes no se
// usaba. Si alguna region estuviera mal, aqui saltaria una excepcion en
// vez de mas tarde en un programa. Devuelve cuantos puntos comprobo, o
// -1 si alguno no devolvio lo escrito.
int32_t memoria_probar_alta(void);

// Comprueba si la RAM por encima de 4 GB es real o es un ECO de la memoria
// baja (una placa de 4 GB tomada por una de 8). Devuelve la RAM que de
// verdad hay. Escribir y releer la misma direccion NO lo detecta: el eco
// pasa esa prueba mientras machaca la memoria baja.
uint64_t memoria_comprobar_eco(void);

// Recorta la RAM total a lo comprobado y rehace los tramos.
void memoria_recortar(uint64_t nueva_total);
