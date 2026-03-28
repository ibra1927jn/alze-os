/*
 * Anykernel OS — Device Filesystem (devfs)
 *
 * Registers kernel devices as file-like objects in the VFS:
 *
 *   /dev/serial   — UART COM1 (read/write: serial port I/O)
 *   /dev/console  — Framebuffer console (write-only: renders text)
 *   /dev/keyboard — PS/2 keyboard (read-only: returns ASCII chars)
 *   /dev/null     — Discard all writes, reads return 0
 *   /dev/zero     — Reads return zero bytes
 *
 * Each device implements file_ops with read/write callbacks
 * that delegate to the actual hardware driver.
 */

#include "vfs.h"
#include "uart.h"
#include "console.h"
#include "kb.h"
#include "string.h"
#include "kprintf.h"
#include "log.h"

/* ── /dev/serial (UART COM1) ─────────────────────────────────── */

static int64_t serial_write(struct vnode *vn, const void *buf, uint64_t count) {
    (void)vn;
    const char *p = (const char *)buf;
    for (uint64_t i = 0; i < count; i++) {
        uart_putc(p[i]);
    }
    return (int64_t)count;
}

static int64_t serial_read(struct vnode *vn, void *buf, uint64_t count) {
    (void)vn;
    char *p = (char *)buf;
    uint64_t read = 0;
    for (uint64_t i = 0; i < count; i++) {
        char c = uart_getc();
        if (c == 0) break;
        p[i] = c;
        read++;
    }
    return (int64_t)read;
}

static struct file_ops serial_ops = {
    .open  = 0,
    .close = 0,
    .read  = serial_read,
    .write = serial_write,
    .ioctl = 0,
};

/* ── /dev/console (framebuffer) ──────────────────────────────── */

static int64_t console_write_vfs(struct vnode *vn, const void *buf, uint64_t count) {
    (void)vn;
    const char *p = (const char *)buf;
    for (uint64_t i = 0; i < count; i++) {
        console_putchar(p[i]);
    }
    return (int64_t)count;
}

static struct file_ops console_ops = {
    .open  = 0,
    .close = 0,
    .read  = 0,
    .write = console_write_vfs,
    .ioctl = 0,
};

/* ── /dev/keyboard (PS/2) ────────────────────────────────────── */

static int64_t keyboard_read(struct vnode *vn, void *buf, uint64_t count) {
    (void)vn;
    char *p = (char *)buf;
    uint64_t read = 0;

    for (uint64_t i = 0; i < count; i++) {
        char c = kb_getchar();
        if (c == 0) break;  /* No more input available */
        p[i] = c;
        read++;
    }
    return (int64_t)read;
}

static struct file_ops keyboard_ops = {
    .open  = 0,
    .close = 0,
    .read  = keyboard_read,
    .write = 0,
    .ioctl = 0,
};

/* ── /dev/null ───────────────────────────────────────────────── */

static int64_t null_write(struct vnode *vn, const void *buf, uint64_t count) {
    (void)vn; (void)buf;
    return (int64_t)count;  /* Silently discard */
}

static int64_t null_read(struct vnode *vn, void *buf, uint64_t count) {
    (void)vn; (void)buf; (void)count;
    return 0;  /* EOF immediately */
}

static struct file_ops null_ops = {
    .open  = 0,
    .close = 0,
    .read  = null_read,
    .write = null_write,
    .ioctl = 0,
};

/* ── /dev/zero ───────────────────────────────────────────────── */

static int64_t zero_read(struct vnode *vn, void *buf, uint64_t count) {
    (void)vn;
    memset(buf, 0, count);
    return (int64_t)count;
}

static struct file_ops zero_ops = {
    .open  = 0,
    .close = 0,
    .read  = zero_read,
    .write = null_write, /* Same as null — discard */
    .ioctl = 0,
};

/* ── Register all devices ────────────────────────────────────── */

void devfs_init(void) {
    int r = 0;
    r |= vfs_register_device("serial",   VN_CHARDEV, &serial_ops,   0);
    r |= vfs_register_device("console",  VN_CHARDEV, &console_ops,  0);
    r |= vfs_register_device("keyboard", VN_CHARDEV, &keyboard_ops, 0);
    r |= vfs_register_device("null",     VN_CHARDEV, &null_ops,     0);
    r |= vfs_register_device("zero",     VN_CHARDEV, &zero_ops,     0);
    if (r < 0) {
        LOG_FAIL("devfs: failed to register one or more devices!");
    }
    LOG_OK("devfs: 5 devices registered (serial, console, keyboard, null, zero)");
}
