/*
 * Anykernel OS — Ramdisk Driver Implementation
 *
 * Searches for Limine modules at boot, exposes the first one as a ramdisk.
 * If it contains a valid ext2 image, it is mounted automatically.
 *
 * Limine module protocol (revision 0):
 *   - The request has an array of modules (struct limine_file).
 *   - Each module has: address (virtual), size, path, cmdline.
 *
 * In limine.conf, add:
 *   module_path: boot():/boot/ramdisk.img
 */

#include "ramdisk.h"
#include "ext2.h"
#include "string.h"
#include "log.h"
#include "errno.h"
#include "vfs.h"

#include <stdint.h>
#include <stdbool.h>

/* Include limine protocol for module request */
#include "limine.h"

/* ── Limine module request ──────────────────────────────────────── */

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

/* ── Global state ──────────────────────────────────────────────── */

struct ramdisk {
    void     *base;   /* Virtual address of the loaded module */
    uint64_t  size;   /* Size in bytes */
    bool      loaded; /* true if a module was found and loaded */
};

static struct ramdisk rd = {
    .base   = 0,
    .size   = 0,
    .loaded = false,
};

/* ── VFS file_ops for ramdisk ──────────────────────────────────── */

static int64_t ramdisk_vfs_read(struct vnode *vn, void *buf, uint64_t count) {
    (void)vn;
    if (!rd.loaded) return -EIO;
    if (!buf) return -EINVAL;
    if (count > rd.size) count = rd.size;
    memcpy(buf, rd.base, count);
    return (int64_t)count;
}

static struct file_ops ramdisk_ops = {
    .open  = 0,
    .close = 0,
    .read  = ramdisk_vfs_read,
    .write = 0,  /* Read-only */
    .ioctl = 0,
};

/* ── Initialization ─────────────────────────────────────────────── */

void ramdisk_init(void) {
    if (!module_request.response) {
        LOG_INFO("ramdisk: no Limine module response (no modules loaded)");
        return;
    }

    uint64_t count = module_request.response->module_count;
    if (count == 0) {
        LOG_INFO("ramdisk: 0 boot modules found");
        return;
    }

    LOG_INFO("ramdisk: %lu boot module(s) found", count);

    /* Use the first module as ramdisk */
    struct limine_file *mod = module_request.response->modules[0];
    if (!mod || !mod->address || mod->size == 0) {
        LOG_WARN("ramdisk: first module is empty or invalid");
        return;
    }

    rd.base = mod->address;
    rd.size = mod->size;
    rd.loaded = true;

    LOG_OK("ramdisk: loaded module '%s' (%lu bytes at %p)",
           mod->path ? (const char *)mod->path : "(unknown)",
           rd.size, rd.base);

    /* Register as a device in VFS */
    vfs_register_device("ramdisk", VN_BLOCKDEV, &ramdisk_ops, 0);

    /* Try to mount as ext2 */
    int ret = ext2_init(rd.base, rd.size);
    if (ret == 0) {
        ext2_dump_info();
        ext2_list_root();
    } else {
        LOG_INFO("ramdisk: module is not ext2 (or corrupt), skipping fs mount");
    }
}
