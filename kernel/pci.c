/*
 * Anykernel OS — PCI Configuration Space Implementation
 *
 * Uses mechanism 1 (I/O ports 0xCF8/0xCFC) to access the
 * PCI configuration space of any device.
 *
 * Scans buses 0-255, devices 0-31, functions 0-7.
 * In practice, most systems only use bus 0.
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

/* ── PCI config space read/write ────────────────────────────────── */

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

void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t dword = pci_read32(bus, dev, func, offset & PCI_REG_ALIGN_MASK);
    int shift = (offset & 2) * 8;
    dword &= ~(0xFFFF << shift);
    dword |= (uint32_t)val << shift;
    pci_write32(bus, dev, func, offset & PCI_REG_ALIGN_MASK, dword);
}

void pci_write8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint8_t val) {
    uint32_t dword = pci_read32(bus, dev, func, offset & PCI_REG_ALIGN_MASK);
    int shift = (offset & 3) * 8;
    dword &= ~(0xFF << shift);
    dword |= (uint32_t)val << shift;
    pci_write32(bus, dev, func, offset & PCI_REG_ALIGN_MASK, dword);
}

/* ── Common bus scan — callback returns 0 to continue, non-zero to stop ── */

typedef int (*pci_scan_cb)(uint8_t bus, uint8_t dev, uint8_t func,
                           uint16_t vendor, void *ctx);

static int pci_scan(pci_scan_cb cb, void *ctx) {
    for (uint32_t bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; dev++) {
            for (uint8_t func = 0; func < PCI_MAX_FUNCTION; func++) {
                uint16_t vendor = pci_read16((uint8_t)bus, dev, func, PCI_VENDOR_ID);
                if (vendor == PCI_VENDOR_INVALID || vendor == 0x0000) {
                    if (func == 0) break;
                    continue;
                }

                int ret = cb((uint8_t)bus, dev, func, vendor, ctx);
                if (ret != 0) return ret;

                if (func == 0) {
                    uint8_t hdr = pci_read8((uint8_t)bus, dev, func, PCI_HEADER_TYPE);
                    if (!(hdr & PCI_HEADER_MULTIFUNC)) break;
                }
            }
        }
    }
    return 0;
}

/* ── Device search by class ────────────────────────────────────── */

struct pci_find_ctx {
    uint8_t class_code, subclass, prog_if;
    struct pci_device *out;
};

static int pci_find_cb(uint8_t bus, uint8_t dev, uint8_t func,
                       uint16_t vendor, void *ctx) {
    struct pci_find_ctx *f = (struct pci_find_ctx *)ctx;

    uint8_t cls = pci_read8(bus, dev, func, PCI_CLASS);
    uint8_t sub = pci_read8(bus, dev, func, PCI_SUBCLASS);
    uint8_t pif = pci_read8(bus, dev, func, PCI_PROG_IF);

    if (cls != f->class_code || sub != f->subclass || pif != f->prog_if)
        return 0;

    if (f->out) {
        f->out->bus         = bus;
        f->out->device      = dev;
        f->out->function    = func;
        f->out->vendor_id   = vendor;
        f->out->device_id   = pci_read16(bus, dev, func, PCI_DEVICE_ID);
        f->out->class_code  = cls;
        f->out->subclass    = sub;
        f->out->prog_if     = pif;
        f->out->header_type = pci_read8(bus, dev, func, PCI_HEADER_TYPE);

        for (int i = 0; i < 6; i++) {
            f->out->bar[i] = pci_read32(bus, dev, func,
                                        PCI_BAR0 + (uint8_t)(i * 4));
        }
    }
    return 1;  /* Found — stop scanning */
}

int pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                    struct pci_device *out) {
    struct pci_find_ctx ctx = { class_code, subclass, prog_if, out };
    return pci_scan(pci_find_cb, &ctx) ? 0 : -ENOENT;
}

/* ── Full enumeration (diagnostic) ──────────────────────────────── */

static int pci_enum_cb(uint8_t bus, uint8_t dev, uint8_t func,
                       uint16_t vendor, void *ctx) {
    uint32_t *count = (uint32_t *)ctx;

    uint16_t device_id = pci_read16(bus, dev, func, PCI_DEVICE_ID);
    uint8_t cls = pci_read8(bus, dev, func, PCI_CLASS);
    uint8_t sub = pci_read8(bus, dev, func, PCI_SUBCLASS);
    uint8_t pif = pci_read8(bus, dev, func, PCI_PROG_IF);

    kprintf("  %02x:%02x.%x  %04x:%04x  class=%02x sub=%02x pif=%02x\n",
            bus, dev, func, vendor, device_id, cls, sub, pif);
    (*count)++;
    return 0;
}

void pci_enumerate(void) {
    kprintf("\n--- PCI Device Enumeration ---\n");
    uint32_t count = 0;
    pci_scan(pci_enum_cb, &count);
    kprintf("  Total: %u devices\n", count);
}
