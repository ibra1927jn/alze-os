/*
 * Anykernel OS — xHCI USB 3.x Host Controller (Minimal Detection)
 *
 * Minimal driver: detects xHCI controller via PCI, maps BAR0,
 * resets the controller, and enumerates ports to detect
 * connected USB devices.
 *
 * Does NOT implement full HID protocol — detection only.
 *
 * xHCI spec: https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf
 */

#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>
#include <stdbool.h>

/* ── xHCI Capability Registers (offsets from BAR0) ──────────────── */

struct xhci_cap_regs {
    uint8_t  caplength;       /* Offset 0x00: capability regs length */
    uint8_t  reserved;
    uint16_t hci_version;     /* Offset 0x02: interface version (BCD) */
    uint32_t hcsparams1;      /* Offset 0x04: structural parameters 1 */
    uint32_t hcsparams2;      /* Offset 0x08: structural parameters 2 */
    uint32_t hcsparams3;      /* Offset 0x0C: structural parameters 3 */
    uint32_t hccparams1;      /* Offset 0x10: capability parameters 1 */
    uint32_t dboff;           /* Offset 0x14: doorbell offset */
    uint32_t rtsoff;          /* Offset 0x18: runtime registers offset */
    uint32_t hccparams2;      /* Offset 0x1C: capability parameters 2 */
} __attribute__((packed));

/* Extract fields from HCSPARAMS1 */
#define XHCI_MAX_SLOTS(p1)  ((p1) & 0xFF)
#define XHCI_MAX_INTRS(p1)  (((p1) >> 8) & 0x7FF)
#define XHCI_MAX_PORTS(p1)  (((p1) >> 24) & 0xFF)

/* ── xHCI Operational Registers (offset = BAR0 + caplength) ─────── */

/* USBCMD register bits */
#define XHCI_CMD_RUN    (1U << 0)   /* Run/Stop */
#define XHCI_CMD_HCRST  (1U << 1)   /* Host Controller Reset */

/* USBSTS register bits */
#define XHCI_STS_HCH   (1U << 0)   /* HCHalted */
#define XHCI_STS_CNR   (1U << 11)  /* Controller Not Ready */

/* Port Status and Control register bits */
#define XHCI_PORTSC_CCS   (1U << 0)   /* Current Connect Status */
#define XHCI_PORTSC_PED   (1U << 1)   /* Port Enabled/Disabled */
#define XHCI_PORTSC_PR    (1U << 4)   /* Port Reset */
#define XHCI_PORTSC_PLS_MASK  (0xFU << 5)  /* Port Link State */
#define XHCI_PORTSC_PP    (1U << 9)   /* Port Power */
#define XHCI_PORTSC_SPEED_MASK (0xFU << 10) /* Port Speed */

/* Port speed values */
#define XHCI_SPEED_FULL   1  /* USB 1.1 Full-Speed (12 Mbps) */
#define XHCI_SPEED_LOW    2  /* USB 1.0 Low-Speed (1.5 Mbps) */
#define XHCI_SPEED_HIGH   3  /* USB 2.0 High-Speed (480 Mbps) */
#define XHCI_SPEED_SUPER  4  /* USB 3.0 SuperSpeed (5 Gbps) */

/* ── API ────────────────────────────────────────────────────────── */

/*
 * Probe for xHCI controller on PCI bus.
 * If found: map BAR0, reset controller, enumerate ports.
 * Prints detection results to serial.
 */
void xhci_init(void);

#endif /* XHCI_H */
