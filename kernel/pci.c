/*
 * Anykernel OS — PCI Configuration Space Implementation
 *
 * Usa mecanismo 1 (I/O ports 0xCF8/0xCFC) para acceder al
 * espacio de configuracion PCI de cualquier dispositivo.
 *
 * Escanea buses 0-255, devices 0-31, functions 0-7.
 * En la practica, la mayoria de sistemas solo usan bus 0.
 */

#include "pci.h"
#include "io.h"
#include "kprintf.h"
#include "log.h"
#include "errno.h"

/* Header type bit 7: multi-function device */
#define PCI_HEADER_MULTIFUNC  0x80

/* PCI topology limits */
#define PCI_MAX_BUS       256
#define PCI_MAX_DEVICE    32
#define PCI_MAX_FUNCTION  8

/* Config space register alignment mask (dword-aligned access) */
#define PCI_REG_ALIGN_MASK  0xFC

/* ── Lectura/escritura PCI config space ─────────────────────────── */

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_config_addr(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read32(bus, dev, func, offset & PCI_REG_ALIGN_MASK);
    return (uint16_t)(val >> ((offset & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read32(bus, dev, func, offset & PCI_REG_ALIGN_MASK);
    return (uint8_t)(val >> ((offset & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    outl(PCI_CONFIG_ADDR, pci_config_addr(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

/* ── Busqueda de dispositivo por clase ──────────────────────────── */

int pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                    struct pci_device *out) {
    /* Escaneo brute-force: bus 0-255, device 0-31, function 0-7 */
    for (uint32_t bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; dev++) {
            for (uint8_t func = 0; func < PCI_MAX_FUNCTION; func++) {
                uint16_t vendor = pci_read16((uint8_t)bus, dev, func, PCI_VENDOR_ID);
                if (vendor == PCI_VENDOR_INVALID || vendor == 0x0000) {
                    if (func == 0) break;  /* No hay device, saltar al siguiente */
                    continue;
                }

                uint8_t cls  = pci_read8((uint8_t)bus, dev, func, PCI_CLASS);
                uint8_t sub  = pci_read8((uint8_t)bus, dev, func, PCI_SUBCLASS);
                uint8_t pif  = pci_read8((uint8_t)bus, dev, func, PCI_PROG_IF);

                if (cls == class_code && sub == subclass && pif == prog_if) {
                    if (out) {
                        out->bus       = (uint8_t)bus;
                        out->device    = dev;
                        out->function  = func;
                        out->vendor_id = vendor;
                        out->device_id = pci_read16((uint8_t)bus, dev, func, PCI_DEVICE_ID);
                        out->class_code = cls;
                        out->subclass   = sub;
                        out->prog_if    = pif;
                        out->header_type = pci_read8((uint8_t)bus, dev, func, PCI_HEADER_TYPE);

                        /* Leer los 6 BARs */
                        for (int i = 0; i < 6; i++) {
                            out->bar[i] = pci_read32((uint8_t)bus, dev, func,
                                                     PCI_BAR0 + (uint8_t)(i * 4));
                        }
                    }
                    return 0;
                }

                /* Si no es multi-function, no seguir con func > 0 */
                if (func == 0) {
                    uint8_t hdr = pci_read8((uint8_t)bus, dev, func, PCI_HEADER_TYPE);
                    if (!(hdr & PCI_HEADER_MULTIFUNC)) break;  /* Bit 7: multi-function */
                }
            }
        }
    }

    return -ENOENT;
}

/* ── Enumeracion completa (diagnostic) ──────────────────────────── */

void pci_enumerate(void) {
    kprintf("\n--- PCI Device Enumeration ---\n");
    uint32_t count = 0;

    for (uint32_t bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; dev++) {
            for (uint8_t func = 0; func < PCI_MAX_FUNCTION; func++) {
                uint16_t vendor = pci_read16((uint8_t)bus, dev, func, PCI_VENDOR_ID);
                if (vendor == PCI_VENDOR_INVALID || vendor == 0x0000) {
                    if (func == 0) break;
                    continue;
                }

                uint16_t device_id = pci_read16((uint8_t)bus, dev, func, PCI_DEVICE_ID);
                uint8_t cls  = pci_read8((uint8_t)bus, dev, func, PCI_CLASS);
                uint8_t sub  = pci_read8((uint8_t)bus, dev, func, PCI_SUBCLASS);
                uint8_t pif  = pci_read8((uint8_t)bus, dev, func, PCI_PROG_IF);

                kprintf("  %02x:%02x.%x  %04x:%04x  class=%02x sub=%02x pif=%02x\n",
                        bus, dev, func, vendor, device_id, cls, sub, pif);
                count++;

                if (func == 0) {
                    uint8_t hdr = pci_read8((uint8_t)bus, dev, func, PCI_HEADER_TYPE);
                    if (!(hdr & PCI_HEADER_MULTIFUNC)) break;
                }
            }
        }
    }

    kprintf("  Total: %u devices\n", count);
}
