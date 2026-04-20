/*
 * Anykernel OS — Ramdisk Driver (Boot Module Backed)
 *
 * Loads a Limine module as an in-memory block device.
 * Used as backing store for ext2 without requiring AHCI/real disk.
 *
 * The module is specified in limine.conf as:
 *   module_path: boot():/boot/ramdisk.img
 *
 * The ramdisk is read-only (the image comes from the bootloader).
 */

#ifndef RAMDISK_H
#define RAMDISK_H

#include <stdint.h>
#include <stdbool.h>

/* Ramdisk state */
struct ramdisk {
    void     *base;   /* Virtual address of the loaded module */
    uint64_t  size;   /* Size in bytes */
    bool      loaded; /* true if a module was found and loaded */
};

/*
 * Initialize ramdisk from Limine boot modules.
 * Searches for the first available module and exposes it as a ramdisk.
 * If it contains an ext2 image, initializes the filesystem automatically.
 */
void ramdisk_init(void);

#endif /* RAMDISK_H */
