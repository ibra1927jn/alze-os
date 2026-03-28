/*
 * Anykernel OS — Virtual File System (VFS)
 *
 * The Unix paradigm: "Everything is a file."
 *
 * Any device, pipe, or resource in the kernel exposes a uniform
 * interface: open, read, write, close, ioctl. This means user code
 * doesn't need to know whether it's talking to a serial port, a
 * framebuffer, a pipe, or a disk — the same API works for all.
 *
 * Architecture:
 *
 *   struct file_ops  — Function pointer table (like a C++ vtable)
 *   struct vnode     — Represents a file-like object in the kernel
 *   struct file      — An open file descriptor (fd + current offset)
 *
 *   Devices register vnodes at paths like "/dev/serial", "/dev/console".
 *   Tasks open vnodes to get file descriptors, then read/write via fd.
 *
 * Inspired by: Unix VFS, Plan 9 (9P), macOS VFS layer.
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Forward declarations ────────────────────────────────────── */

struct vnode;
struct file;

/* ── File operations (vtable) ────────────────────────────────── */

/*
 * Every device implements this interface. NULL entries mean
 * "operation not supported" and return -1 (ENOTSUP).
 */
struct file_ops {
    int     (*open)(struct vnode *vn, int flags);
    int     (*close)(struct vnode *vn);
    int64_t (*read)(struct vnode *vn, void *buf, uint64_t count);
    int64_t (*write)(struct vnode *vn, const void *buf, uint64_t count);
    int     (*ioctl)(struct vnode *vn, uint32_t cmd, void *arg);
};

/* ── Vnode types ─────────────────────────────────────────────── */

enum vnode_type {
    VN_NONE = 0,
    VN_CHARDEV,   /* Character device (UART, keyboard, console) */
    VN_BLOCKDEV,  /* Block device (disk — future) */
    VN_PIPE,      /* In-kernel pipe (future) */
    VN_FILE,      /* Regular file (future, needs filesystem) */
    VN_DIR,       /* Directory (future) */
};

/* ── Vnode (virtual node) ────────────────────────────────────── */

#define VN_NAME_MAX 32

struct vnode {
    char              name[VN_NAME_MAX]; /* e.g. "serial", "console" */
    enum vnode_type   type;
    struct file_ops  *ops;               /* Operation vtable */
    void             *private_data;      /* Device-specific state */
    uint32_t          refcount;          /* Number of open fds */
};

/* ── File descriptor ─────────────────────────────────────────── */

#define VFS_O_RDONLY  0x01
#define VFS_O_WRONLY  0x02
#define VFS_O_RDWR    0x03
#define VFS_O_APPEND  0x04

struct file {
    struct vnode *vnode;      /* Backing vnode */
    int           flags;     /* Open mode (O_RDONLY, O_WRONLY, etc.) */
    uint64_t      offset;    /* Current position (for seekable devices) */
    bool          in_use;    /* Is this fd slot taken? */
};

/* ── Per-task file descriptor table ──────────────────────────── */

#define VFS_MAX_FDS       16   /* Max open fds per task */
#define VFS_MAX_DEVICES   16   /* Max registered devices */

/* Well-known file descriptors */
#define FD_STDIN   0
#define FD_STDOUT  1
#define FD_STDERR  2

/* ── API ─────────────────────────────────────────────────────── */

/* Initialize the VFS subsystem */
void vfs_init(void);

/* Register a device in the global device table */
int vfs_register_device(const char *name, enum vnode_type type,
                        struct file_ops *ops, void *private_data);

/* Open a device by name, returns fd or -1 */
int vfs_open(const char *path, int flags);

/* Close a file descriptor */
int vfs_close(int fd);

/* Read from fd into buffer, returns bytes read or -1 */
int64_t vfs_read(int fd, void *buf, uint64_t count);

/* Write buffer to fd, returns bytes written or -1 */
int64_t vfs_write(int fd, const void *buf, uint64_t count);

/* Device control */
int vfs_ioctl(int fd, uint32_t cmd, void *arg);

/* Lookup a device vnode by name */
struct vnode *vfs_lookup(const char *name);

#endif /* VFS_H */
