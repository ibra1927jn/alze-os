/*
 * Anykernel OS — xHCI USB 3.x Host Controller (Minimal Detection)
 *
 * Minimal implementation:
 *   1. Find xHCI controller on PCI (class 0C, sub 03, progif 30)
 *   2. Map BAR0 MMIO
 *   3. Reset controller (HCRST)
 *   4. Enumerate ports and report connected devices
 *
 * Not implemented: device context, command ring, event ring,
 * transfers, HID protocol. Detection only.
 */

#include "xhci.h"
#include "pci.h"
#include "vmm.h"
#include "memory.h"
#include "log.h"

/* ── Helpers for reading xHCI MMIO registers ──────────────────── */

static volatile uint32_t *xhci_base = 0;   /* Mapped BAR0 */
static uint8_t xhci_cap_length = 0;        /* Capability regs length */

/* Read operational register (offset relative to operational base) */
static inline uint32_t xhci_op_read32(uint32_t offset) {
    volatile uint32_t *reg = (volatile uint32_t *)
        ((uint8_t *)xhci_base + xhci_cap_length + offset);
    return *reg;
}

/* Write operational register */
static inline void xhci_op_write32(uint32_t offset, uint32_t val) {
    volatile uint32_t *reg = (volatile uint32_t *)
        ((uint8_t *)xhci_base + xhci_cap_length + offset);
    *reg = val;
}

/* Operational register offsets */
#define XHCI_OP_USBCMD   0x00
#define XHCI_OP_USBSTS   0x04

/* Port Status register: each port occupies 0x10 bytes starting at offset 0x400 */
#define XHCI_PORT_REG_BASE   0x400
#define XHCI_PORT_REG_SIZE   0x10
#define XHCI_OP_PORTSC(n)  (XHCI_PORT_REG_BASE + ((n) * XHCI_PORT_REG_SIZE))

/* Timeout spin-loop limits for controller state transitions */
#define XHCI_TIMEOUT_HALT     100000   /* Iterations waiting for HCHalted     */
#define XHCI_TIMEOUT_RESET   1000000   /* Iterations waiting for HCRST clear  */
#define XHCI_TIMEOUT_READY   1000000   /* Iterations waiting for CNR clear    */

/* PORTSC speed field shift */
#define XHCI_PORTSC_SPEED_SHIFT  10

/* ── Human-readable speed string ───────────────────────────────── */

static const char *xhci_speed_str(uint32_t speed) {
    switch (speed) {
        case XHCI_SPEED_FULL:  return "Full-Speed (12 Mbps)";
        case XHCI_SPEED_LOW:   return "Low-Speed (1.5 Mbps)";
        case XHCI_SPEED_HIGH:  return "High-Speed (480 Mbps)";
        case XHCI_SPEED_SUPER: return "SuperSpeed (5 Gbps)";
        default:               return "Unknown speed";
    }
}

/* ── Controller reset ──────────────────────────────────────────── */

static int xhci_reset(void) {
    /* Step 1: stop the controller (clear Run/Stop bit) */
    uint32_t cmd = xhci_op_read32(XHCI_OP_USBCMD);
    cmd &= ~XHCI_CMD_RUN;
    xhci_op_write32(XHCI_OP_USBCMD, cmd);

    /* Wait for halt (HCHalted = 1) */
    {
        int timeout = XHCI_TIMEOUT_HALT;
        while (!(xhci_op_read32(XHCI_OP_USBSTS) & XHCI_STS_HCH) && --timeout > 0) {
            asm volatile("pause");
        }
        if (timeout == 0) {
            LOG_ERROR("xHCI: timeout waiting for controller halt");
            return -1;
        }
    }

    /* Step 2: reset (HCRST bit) */
    cmd = xhci_op_read32(XHCI_OP_USBCMD);
    cmd |= XHCI_CMD_HCRST;
    xhci_op_write32(XHCI_OP_USBCMD, cmd);

    /* Wait for reset to complete (HCRST clears itself) */
    {
        int timeout = XHCI_TIMEOUT_RESET;
        while ((xhci_op_read32(XHCI_OP_USBCMD) & XHCI_CMD_HCRST) && --timeout > 0) {
            asm volatile("pause");
        }
        if (timeout == 0) {
            LOG_ERROR("xHCI: timeout waiting for reset completion");
            return -1;
        }
    }

    /* Step 3: wait for Controller Not Ready = 0 */
    {
        int timeout = XHCI_TIMEOUT_READY;
        while ((xhci_op_read32(XHCI_OP_USBSTS) & XHCI_STS_CNR) && --timeout > 0) {
            asm volatile("pause");
        }
        if (timeout == 0) {
            LOG_ERROR("xHCI: timeout waiting for controller ready after reset");
            return -1;
        }
    }

    return 0;
}

/* ── Port enumeration ─────────────────────────────────────────── */

static void xhci_enumerate_ports(uint32_t max_ports) {
    uint32_t devices_found = 0;

    for (uint32_t port = 0; port < max_ports; port++) {
        uint32_t portsc = xhci_op_read32(XHCI_OP_PORTSC(port));

        /* Check if a device is connected */
        if (portsc & XHCI_PORTSC_CCS) {
            uint32_t speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
            LOG_OK("USB device detected on port %u — %s", port + 1,
                   xhci_speed_str(speed));
            devices_found++;
        }
    }

    if (devices_found == 0) {
        LOG_INFO("xHCI: no USB devices connected on any port");
    } else {
        LOG_OK("xHCI: %u USB device(s) detected", devices_found);
    }
}

/* ── Main initialization ───────────────────────────────────────── */

void xhci_init(void) {
    struct pci_device pci_dev;

    /* Find xHCI controller on PCI */
    int ret = pci_find_device(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                              PCI_PROGIF_XHCI, &pci_dev);
    if (ret < 0) {
        LOG_INFO("No xHCI controller found on PCI bus");
        return;
    }

    LOG_OK("xHCI controller found: PCI %02x:%02x.%x  vendor=%04x device=%04x",
           pci_dev.bus, pci_dev.device, pci_dev.function,
           pci_dev.vendor_id, pci_dev.device_id);

    /* Get BAR0 (MMIO base address) */
    uint32_t bar0 = pci_dev.bar[0];

    /* Verify it's MMIO (bit 0 = 0) */
    if (bar0 & PCI_BAR_IO_BIT) {
        LOG_ERROR("xHCI: BAR0 is I/O mapped, expected MMIO");
        return;
    }

    /* Extract base address (bits 31:4 for 32-bit, or use BAR1 for 64-bit) */
    uint64_t mmio_phys = bar0 & PCI_BAR_ADDR_MASK_32;

    /* If BAR0 indicates 64-bit (bits 2:1 == 10), combine with BAR1 */
    if ((bar0 & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64BIT) {
        mmio_phys |= (uint64_t)pci_dev.bar[1] << 32;
    }

    if (mmio_phys == 0) {
        LOG_ERROR("xHCI: BAR0 address is NULL");
        return;
    }

    LOG_INFO("xHCI: BAR0 MMIO at physical 0x%lx", mmio_phys);

    /* Enable Bus Master and Memory Space in PCI command register */
    uint16_t cmd = pci_read16(pci_dev.bus, pci_dev.device,
                              pci_dev.function, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER;
    pci_write16(pci_dev.bus, pci_dev.device, pci_dev.function,
                PCI_COMMAND, cmd);

    /* Map BAR0 into virtual space via HHDM.
     * xHCI MMIO typically uses ~64KB (depends on controller).
     * Pages should be mapped with cache disabled (PTE_PCD | PTE_PWT). */
    xhci_base = (volatile uint32_t *)PHYS2VIRT(mmio_phys);

    /* Map first MMIO pages with cache disabled.
     * With a full HHDM the region is already mapped, but we need
     * to ensure the mapping has correct flags.
     * For now we assume HHDM covers this region. */
    /* TODO: vmm_map_range with PTE_PCD | PTE_PWT for proper MMIO */

    /* Read capability registers */
    volatile struct xhci_cap_regs *caps = (volatile struct xhci_cap_regs *)xhci_base;
    xhci_cap_length = caps->caplength;

    uint16_t version = caps->hci_version;
    uint32_t hcsparams1 = caps->hcsparams1;

    uint32_t max_slots = XHCI_MAX_SLOTS(hcsparams1);
    uint32_t max_intrs = XHCI_MAX_INTRS(hcsparams1);
    uint32_t max_ports = XHCI_MAX_PORTS(hcsparams1);

    LOG_INFO("xHCI: version %x.%x, cap_length=%u",
             (version >> 8) & 0xFF, version & 0xFF, xhci_cap_length);
    LOG_INFO("xHCI: max_slots=%u, max_intrs=%u, max_ports=%u",
             max_slots, max_intrs, max_ports);

    /* Reset the controller */
    if (xhci_reset() < 0) {
        LOG_ERROR("xHCI: reset failed, aborting");
        return;
    }

    LOG_OK("xHCI: controller reset OK");

    /* Enumerate ports to detect devices */
    xhci_enumerate_ports(max_ports);
}
