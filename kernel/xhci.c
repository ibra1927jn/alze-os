/*
 * Anykernel OS — xHCI USB 3.x Host Controller (Minimal Detection)
 *
 * Implementacion minima:
 *   1. Buscar controlador xHCI en PCI (class 0C, sub 03, progif 30)
 *   2. Mapear BAR0 MMIO
 *   3. Resetear controlador (HCRST)
 *   4. Enumerar puertos y reportar dispositivos conectados
 *
 * No implementa: device context, command ring, event ring,
 * transfers, HID protocol. Solo deteccion de presencia.
 */

#include "xhci.h"
#include "pci.h"
#include "vmm.h"
#include "memory.h"
#include "kprintf.h"
#include "log.h"
#include "string.h"

/* ── Helpers para leer registros MMIO del xHCI ──────────────────── */

static volatile uint32_t *xhci_base = 0;   /* BAR0 mapeado */
static uint8_t xhci_cap_length = 0;        /* Longitud de capability regs */

/* Leer registro operacional (offset relativo a operational base) */
static inline uint32_t xhci_op_read32(uint32_t offset) {
    volatile uint32_t *reg = (volatile uint32_t *)
        ((uint8_t *)xhci_base + xhci_cap_length + offset);
    return *reg;
}

/* Escribir registro operacional */
static inline void xhci_op_write32(uint32_t offset, uint32_t val) {
    volatile uint32_t *reg = (volatile uint32_t *)
        ((uint8_t *)xhci_base + xhci_cap_length + offset);
    *reg = val;
}

/* Offsets de registros operacionales */
#define XHCI_OP_USBCMD   0x00
#define XHCI_OP_USBSTS   0x04
#define XHCI_OP_PAGESIZE  0x08
#define XHCI_OP_DNCTRL   0x14
#define XHCI_OP_CRCR     0x18
#define XHCI_OP_DCBAAP   0x30
#define XHCI_OP_CONFIG   0x38

/* Port Status register: cada puerto ocupa 0x10 bytes empezando en offset 0x400 */
#define XHCI_PORT_REG_BASE   0x400
#define XHCI_PORT_REG_SIZE   0x10
#define XHCI_OP_PORTSC(n)  (XHCI_PORT_REG_BASE + ((n) * XHCI_PORT_REG_SIZE))

/* PCI BAR decoding */
#define PCI_BAR_IO_BIT         0x01   /* Bit 0: 1=I/O, 0=MMIO          */
#define PCI_BAR_ADDR_MASK_32   0xFFFFFFF0U  /* Bits 31:4 for 32-bit BAR */
#define PCI_BAR_TYPE_MASK      0x06   /* Bits 2:1: BAR type             */
#define PCI_BAR_TYPE_64BIT     0x04   /* Type 10b: 64-bit BAR           */

/* PCI Command register bits */
#define PCI_CMD_MEMORY_SPACE   (1 << 1)   /* Enable memory-mapped access */
#define PCI_CMD_BUS_MASTER     (1 << 2)   /* Enable bus mastering        */

/* PORTSC speed field shift */
#define XHCI_PORTSC_SPEED_SHIFT  10

/* ── Nombre legible del speed ───────────────────────────────────── */

static const char *xhci_speed_str(uint32_t speed) {
    switch (speed) {
        case XHCI_SPEED_FULL:  return "Full-Speed (12 Mbps)";
        case XHCI_SPEED_LOW:   return "Low-Speed (1.5 Mbps)";
        case XHCI_SPEED_HIGH:  return "High-Speed (480 Mbps)";
        case XHCI_SPEED_SUPER: return "SuperSpeed (5 Gbps)";
        default:               return "Unknown speed";
    }
}

/* ── Reset del controlador ──────────────────────────────────────── */

static int xhci_reset(void) {
    /* Paso 1: detener el controlador (limpiar Run/Stop bit) */
    uint32_t cmd = xhci_op_read32(XHCI_OP_USBCMD);
    cmd &= ~XHCI_CMD_RUN;
    xhci_op_write32(XHCI_OP_USBCMD, cmd);

    /* Esperar a que se detenga (HCHalted = 1) */
    {
        int timeout = 100000;
        while (!(xhci_op_read32(XHCI_OP_USBSTS) & XHCI_STS_HCH) && --timeout > 0) {
            asm volatile("pause");
        }
        if (timeout == 0) {
            LOG_ERROR("xHCI: timeout waiting for controller halt");
            return -1;
        }
    }

    /* Paso 2: reset (HCRST bit) */
    cmd = xhci_op_read32(XHCI_OP_USBCMD);
    cmd |= XHCI_CMD_HCRST;
    xhci_op_write32(XHCI_OP_USBCMD, cmd);

    /* Esperar a que el reset termine (HCRST se limpia solo) */
    {
        int timeout = 1000000;
        while ((xhci_op_read32(XHCI_OP_USBCMD) & XHCI_CMD_HCRST) && --timeout > 0) {
            asm volatile("pause");
        }
        if (timeout == 0) {
            LOG_ERROR("xHCI: timeout waiting for reset completion");
            return -1;
        }
    }

    /* Paso 3: esperar Controller Not Ready = 0 */
    {
        int timeout = 1000000;
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

/* ── Enumeracion de puertos ─────────────────────────────────────── */

static void xhci_enumerate_ports(uint32_t max_ports) {
    uint32_t devices_found = 0;

    for (uint32_t port = 0; port < max_ports; port++) {
        uint32_t portsc = xhci_op_read32(XHCI_OP_PORTSC(port));

        /* Verificar si hay un dispositivo conectado */
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

/* ── Inicializacion principal ───────────────────────────────────── */

void xhci_init(void) {
    struct pci_device pci_dev;

    /* Buscar controlador xHCI en PCI */
    int ret = pci_find_device(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                              PCI_PROGIF_XHCI, &pci_dev);
    if (ret < 0) {
        LOG_INFO("No xHCI controller found on PCI bus");
        return;
    }

    LOG_OK("xHCI controller found: PCI %02x:%02x.%x  vendor=%04x device=%04x",
           pci_dev.bus, pci_dev.device, pci_dev.function,
           pci_dev.vendor_id, pci_dev.device_id);

    /* Obtener BAR0 (MMIO base address) */
    uint32_t bar0 = pci_dev.bar[0];

    /* Verificar que es MMIO (bit 0 = 0) */
    if (bar0 & PCI_BAR_IO_BIT) {
        LOG_ERROR("xHCI: BAR0 is I/O mapped, expected MMIO");
        return;
    }

    /* Extraer direccion base (bits 31:4 para 32-bit, o usar BAR1 para 64-bit) */
    uint64_t mmio_phys = bar0 & PCI_BAR_ADDR_MASK_32;

    /* Si BAR0 indica 64-bit (bits 2:1 == 10), combinar con BAR1 */
    if ((bar0 & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64BIT) {
        mmio_phys |= (uint64_t)pci_dev.bar[1] << 32;
    }

    if (mmio_phys == 0) {
        LOG_ERROR("xHCI: BAR0 address is NULL");
        return;
    }

    LOG_INFO("xHCI: BAR0 MMIO at physical 0x%lx", mmio_phys);

    /* Habilitar Bus Master y Memory Space en PCI command register */
    uint16_t cmd = pci_read16(pci_dev.bus, pci_dev.device,
                              pci_dev.function, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER;
    pci_write32(pci_dev.bus, pci_dev.device, pci_dev.function,
                PCI_COMMAND, cmd);

    /* Mapear BAR0 en espacio virtual via HHDM.
     * MMIO de xHCI tipicamente usa ~64KB (depende del controlador).
     * Mapeamos paginas con cache deshabilitado (PTE_PCD | PTE_PWT). */
    xhci_base = (volatile uint32_t *)PHYS2VIRT(mmio_phys);

    /* Mapear las primeras paginas MMIO con cache disabled.
     * En un kernel con HHDM completo, la region ya esta mapeada,
     * pero necesitamos asegurar que el mapeo tiene flags correctos.
     * Por ahora asumimos que HHDM cubre esta region. */
    /* TODO: vmm_map_range con PTE_PCD | PTE_PWT para MMIO correcto */

    /* Leer capability registers */
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

    /* Resetear el controlador */
    if (xhci_reset() < 0) {
        LOG_ERROR("xHCI: reset failed, aborting");
        return;
    }

    LOG_OK("xHCI: controller reset OK");

    /* Enumerar puertos para detectar dispositivos */
    xhci_enumerate_ports(max_ports);
}
