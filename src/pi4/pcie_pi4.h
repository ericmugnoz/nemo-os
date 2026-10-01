// pcie_pi4.h -- Nemo OS, Raspberry Pi 4
// Bring-up del controlador PCIe (BCM2711) y del VL805 (xHCI, USB-A).

#ifndef PCIE_PI4_H
#define PCIE_PI4_H

#include <stdint.h>
#include <stdbool.h>

// Inicializa PCIe y el VL805. Al volver true, el bloque de registros
// xHCI responde en pcie_xhci_base().
bool pcie_init_pi4(void);

// Direccion (lado CPU, mapeo de identidad) del BAR0 del VL805: base
// de los registros xHCI.
uintptr_t pcie_xhci_base(void);

// Offset que hay que SUMAR a toda direccion de RAM que se entregue al
// VL805 para DMA (DCBAA, anillos, scratchpad...). RC_BAR2 -- la
// ventana de entrada PCIe->RAM -- esta colocada en la direccion PCI
// 0x1_00000000, no en 0, para no solapar la ventana de salida.
uint64_t pcie_dma_offset(void);

#endif
