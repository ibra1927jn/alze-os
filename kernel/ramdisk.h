/*
 * Anykernel OS — Ramdisk Driver (Boot Module Backed)
 *
 * Carga un modulo de Limine como dispositivo de bloque en memoria.
 * Usado como backing store para ext2 sin necesitar AHCI/disco real.
 *
 * El modulo se especifica en limine.conf como:
 *   module_path: boot():/boot/ramdisk.img
 *
 * El ramdisk es read-only (la imagen viene del bootloader).
 */

#ifndef RAMDISK_H
#define RAMDISK_H

#include <stdint.h>
#include <stdbool.h>

/* Estado del ramdisk */
struct ramdisk {
    void     *base;   /* Direccion virtual del modulo cargado */
    uint64_t  size;   /* Tamano en bytes */
    bool      loaded; /* true si se encontro y cargo un modulo */
};

/*
 * Initialize ramdisk from Limine boot modules.
 * Busca el primer modulo disponible y lo expone como ramdisk.
 * Si tiene formato ext2, inicializa el filesystem automaticamente.
 */
void ramdisk_init(void);

/*
 * Get ramdisk base address and size.
 * Returns NULL if no ramdisk loaded.
 */
void *ramdisk_get_base(void);
uint64_t ramdisk_get_size(void);

/*
 * Read bytes from ramdisk at given offset.
 * Returns bytes read, or negative errno.
 */
int64_t ramdisk_read(uint64_t offset, void *buf, uint64_t count);

#endif /* RAMDISK_H */
