// xhci_pi4.h -- Nemo OS, Raspberry Pi 4
// Driver xHCI (USB 3 host) para el VL805. Fase C del subproyecto USB.

#ifndef XHCI_PI4_H
#define XHCI_PI4_H

#include <stdint.h>
#include <stdbool.h>

// Fase C completa para HID: registros, reset del controlador, DCBAA/
// scratchpad/anillos, arranque, enumeracion de TODOS los puertos raiz y
// de los hubs que cuelguen de ellos (route strings + Transaction
// Translator), Address Device, descriptores, y configuracion de cada
// interfaz HID boot (teclado 3/1/1, raton 3/1/2) con su EP de
// interrupcion IN leyendo. Devuelve true si hay al menos una interfaz
// HID configurada.
bool xhci_init_pi4(void);

// Numero de puertos del root hub (0 si xhci_init_pi4 no se ha ejecutado).
uint32_t xhci_num_ports(void);

// true si hay un dispositivo conectado en el puerto raiz (1..N).
bool xhci_port_connected(uint32_t puerto);

// Tipos de interfaz HID boot
#define XHCI_HID_TECLADO 1
#define XHCI_HID_RATON   2
// Pantalla tactil, un dedo. El informe que se entrega NO
// es el crudo del aparato --cada panel tiene el suyo-- sino uno ya
// normalizado de 5 bytes, siempre igual:
//   [0] 1 si hay un dedo apoyado, 0 si no
//   [1..2] X, y [3..4] Y, de 0 a 32767 sobre el ancho/alto del panel
// La traduccion del formato propio de cada panel a este se hace en
// xhci_pi4.c, leyendo su Report Descriptor. Quien recibe esto no
// necesita saber nada del aparato.
#define XHCI_HID_TACTIL  3

// Sondeo no bloqueante del anillo de eventos. Cada informe HID recibido
// se entrega a `cb` con su tipo: teclado = 8 bytes (modificadores,
// reservado, 6 teclas); raton = 3-4 bytes (botones, dx, dy[, rueda]).
// Cada 250 ms revisa ademas conexiones/desconexiones en los puertos
// raiz y en los hubs (conexion en caliente): al retirar un dispositivo
// entrega un informe "todo suelto" de su tipo antes de olvidarlo.
// Llamar desde input_poll().
void xhci_poll(void (*cb)(int tipo, const uint8_t *informe, uint32_t len));

// Pendrive (Mass Storage, Bulk-Only Transport, SCSI). Un dispositivo a
// la vez. Sectores de 512 bytes, LBA absoluto del dispositivo.
bool     xhci_msd_presente(void);
uint64_t xhci_msd_sectores(void);
bool     xhci_msd_leer(uint64_t lba, void *buf);
bool     xhci_msd_escribir(uint64_t lba, const void *buf);

// Igual, pero hasta XHCI_MSD_MAX_SECTORES sectores CONTIGUOS en una
// sola transaccion BOT (mucho mas rapido que uno a uno: un pendrive
// tipico hace ~3 transacciones USB por comando SCSI, asi que leer 64
// sectores de golpe en vez de sector a sector es ~64 veces menos
// trafico). `buf` debe tener sitio para count*512 bytes.
#define XHCI_MSD_MAX_SECTORES 64
bool xhci_msd_leer_n(uint64_t lba, uint32_t count, void *buf);
bool xhci_msd_escribir_n(uint64_t lba, uint32_t count, const void *buf);
// Evento de cambio desde la ultima llamada: 0 nada, 1 conectado,
// 2 retirado. Lo consume la capa de disco para (re)montar la FAT.
int      xhci_msd_cambio(void);

// Traduce una direccion de RAM (lado CPU) a la direccion que debe ver
// el VL805 para DMA. Toda direccion escrita en un registro o
// estructura del xHCI pasa por aqui.
uint64_t xhci_dma_addr(const void *p);

#endif
