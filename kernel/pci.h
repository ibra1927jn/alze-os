/*
 * Anykernel OS — PCI Configuration Space Access
 *
 * Acceso al espacio de configuracion PCI via I/O ports (mecanismo 1).
 * PCI usa puertos 0xCF8 (address) y 0xCFC (data) para leer/escribir
 * registros de configuracion de cualquier dispositivo en el bus.
 *
 * Direccionamiento: Bus (8 bits) | Device (5 bits) | Function (3 bits) | Offset (8 bits)
 */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* PCI I/O ports (mecanismo 1) */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* Offsets de registros de configuracion estandar */
#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_STATUS       0x06
#define PCI_REVISION     0x08
#define PCI_PROG_IF      0x09
#define PCI_SUBCLASS     0x0A
#define PCI_CLASS        0x0B
#define PCI_HEADER_TYPE  0x0E
#define PCI_BAR0         0x10
#define PCI_BAR1         0x14
#define PCI_BAR2         0x18
#define PCI_BAR3         0x1C
#define PCI_BAR4         0x20
#define PCI_BAR5         0x24
#define PCI_IRQ_LINE     0x3C
#define PCI_IRQ_PIN      0x3D

/* PCI classes relevantes */
#define PCI_CLASS_SERIAL_BUS     0x0C
#define PCI_SUBCLASS_USB         0x03
#define PCI_PROGIF_XHCI          0x30  /* xHCI (USB 3.x) */
#define PCI_PROGIF_EHCI          0x20  /* EHCI (USB 2.0) */
#define PCI_PROGIF_OHCI          0x10  /* OHCI (USB 1.1) */
#define PCI_PROGIF_UHCI          0x00  /* UHCI (USB 1.0) */

/* Vendor/Device ID invalidos */
#define PCI_VENDOR_INVALID  0xFFFF

/* Resultado de un scan PCI */
struct pci_device {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint32_t bar[6];
};

/*
 * Build PCI config address for mechanism 1.
 */
static inline uint32_t pci_config_addr(uint8_t bus, uint8_t dev,
                                       uint8_t func, uint8_t offset) {
    return (uint32_t)(
        (1U << 31)                |  /* Enable bit */
        ((uint32_t)bus << 16)     |
        ((uint32_t)(dev & 0x1F) << 11) |
        ((uint32_t)(func & 0x07) << 8) |
        (offset & 0xFC)              /* Alineado a 4 bytes */
    );
}

/*
 * Read 32-bit value from PCI config space.
 */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

/*
 * Read 16-bit value from PCI config space.
 */
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

/*
 * Read 8-bit value from PCI config space.
 */
uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

/*
 * Write 32-bit value to PCI config space.
 */
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);

/*
 * Scan PCI bus for a device matching class/subclass/progif.
 * Fills `out` with the first match found.
 * Returns 0 if found, -ENOENT if not found.
 */
int pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                    struct pci_device *out);

/*
 * Enumerate and print all PCI devices to serial (diagnostic).
 */
void pci_enumerate(void);

#endif /* PCI_H */
