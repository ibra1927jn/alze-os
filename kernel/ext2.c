/*
 * Anykernel OS — ext2 Filesystem (Read-Only) Implementation
 *
 * Lectura basica de ext2 sobre ramdisk. Soporta:
 *   - Parseo de superblock y group descriptor table
 *   - Lectura de inodes por numero
 *   - Listado de directorios
 *   - Lectura de archivos (bloques directos unicamente)
 *
 * El ramdisk se carga como modulo de Limine al boot.
 * Se registra como filesystem en el VFS.
 */

#include "ext2.h"
#include "string.h"
#include "kprintf.h"
#include "log.h"
#include "errno.h"
#include "vfs.h"

/* ── Estado global del filesystem ───────────────────────────────── */

static struct ext2_fs fs;
static bool ext2_mounted = false;

/* Forward declaration — definida al final del archivo */
static void ext2_register_vfs(void);

/* ── Helpers internos ───────────────────────────────────────────── */

/* Obtener puntero a un bloque dentro del ramdisk */
static inline void *ext2_block_ptr(uint32_t block_num) {
    uint64_t offset = (uint64_t)block_num * fs.block_size;
    if (offset + fs.block_size > fs.disk_size) {
        return 0; /* Fuera de rango */
    }
    return fs.disk + offset;
}

/* ── Inicializacion ─────────────────────────────────────────────── */

int ext2_init(void *ramdisk_base, uint64_t ramdisk_size) {
    if (!ramdisk_base || ramdisk_size < EXT2_SUPERBLOCK_OFFSET + sizeof(struct ext2_superblock)) {
        LOG_ERROR("ext2: ramdisk too small or NULL");
        return -EINVAL;
    }

    fs.disk = (uint8_t *)ramdisk_base;
    fs.disk_size = ramdisk_size;

    /* Leer superblock desde offset 1024 */
    memcpy(&fs.sb, fs.disk + EXT2_SUPERBLOCK_OFFSET, sizeof(struct ext2_superblock));

    /* Validar magic number */
    if (fs.sb.s_magic != EXT2_SUPER_MAGIC) {
        LOG_ERROR("ext2: bad magic 0x%x (expected 0x%x)",
                  fs.sb.s_magic, EXT2_SUPER_MAGIC);
        return -EINVAL;
    }

    /* Calcular tamano de bloque (valid range: 0-6, i.e. 1K-64K) */
    if (fs.sb.s_log_block_size > 6) {
        LOG_ERROR("ext2: invalid s_log_block_size %u", fs.sb.s_log_block_size);
        return -EINVAL;
    }
    fs.block_size = EXT2_BASE_BLOCK_SIZE << fs.sb.s_log_block_size;

    /* Tamano de inode: rev 0 usa 128 bytes fijo, rev >= 1 usa s_inode_size */
    if (fs.sb.s_rev_level >= 1 && fs.sb.s_inode_size > 0) {
        fs.inode_size = fs.sb.s_inode_size;
    } else {
        fs.inode_size = EXT2_REV0_INODE_SIZE;
    }

    /* Numero de block groups */
    fs.groups_count = (fs.sb.s_blocks_count + fs.sb.s_blocks_per_group - 1)
                      / fs.sb.s_blocks_per_group;

    /* La GDT esta en el bloque siguiente al superblock.
     * Si block_size == 1024, superblock esta en bloque 1, GDT en bloque 2.
     * Si block_size >= 2048, superblock esta en bloque 0 (offset 1024), GDT en bloque 1. */
    uint32_t gdt_block;
    if (fs.block_size == EXT2_BASE_BLOCK_SIZE) {
        gdt_block = 2;
    } else {
        gdt_block = 1;
    }

    fs.gdt = (struct ext2_group_desc *)ext2_block_ptr(gdt_block);
    if (!fs.gdt) {
        LOG_ERROR("ext2: GDT block out of range");
        return -EIO;
    }

    ext2_mounted = true;

    LOG_OK("ext2: mounted (magic=0x%x, block_size=%u, inodes=%u, blocks=%u, groups=%u)",
           fs.sb.s_magic, fs.block_size,
           fs.sb.s_inodes_count, fs.sb.s_blocks_count, fs.groups_count);

    /* Registrar en VFS como dispositivo de bloque */
    ext2_register_vfs();

    return 0;
}

/* ── Lectura de inode ───────────────────────────────────────────── */

int ext2_read_inode(uint32_t ino, struct ext2_inode *out) {
    if (!ext2_mounted) return -EIO;
    if (ino == 0 || ino > fs.sb.s_inodes_count) return -EINVAL;

    /* Los inodes se numeran desde 1. Calcular grupo y offset. */
    uint32_t group = (ino - 1) / fs.sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % fs.sb.s_inodes_per_group;

    if (group >= fs.groups_count) return -EINVAL;

    /* La inode table de este grupo empieza en bg_inode_table */
    uint32_t inode_table_block = fs.gdt[group].bg_inode_table;
    uint64_t inode_offset = (uint64_t)inode_table_block * fs.block_size
                          + (uint64_t)index * fs.inode_size;

    if (inode_offset + sizeof(struct ext2_inode) > fs.disk_size) {
        return -EIO;
    }

    memcpy(out, fs.disk + inode_offset, sizeof(struct ext2_inode));
    return 0;
}

/* ── Listado de directorio ──────────────────────────────────────── */

int ext2_list_dir(uint32_t dir_ino, ext2_dir_callback callback, void *ctx) {
    if (!ext2_mounted) return -EIO;
    if (!callback) return -EINVAL;

    struct ext2_inode inode;
    int ret = ext2_read_inode(dir_ino, &inode);
    if (ret < 0) return ret;

    /* Verificar que es un directorio */
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        return -EINVAL;
    }

    uint32_t size = inode.i_size;
    uint32_t bytes_read = 0;

    /* Recorrer bloques directos del directorio */
    for (int i = 0; i < EXT2_NDIR_BLOCKS && bytes_read < size; i++) {
        uint32_t block = inode.i_block[i];
        if (block == 0) continue;

        uint8_t *data = (uint8_t *)ext2_block_ptr(block);
        if (!data) return -EIO;

        uint32_t offset = 0;
        uint32_t remaining = size - bytes_read;
        if (remaining > fs.block_size) remaining = fs.block_size;

        while (offset < remaining) {
            struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(data + offset);

            /* Validacion basica para evitar loops infinitos */
            if (entry->rec_len == 0 || entry->rec_len > remaining - offset) {
                break;
            }

            if (entry->inode != 0 && entry->name_len > 0) {
                callback(entry->name, entry->name_len,
                         entry->inode, entry->file_type, ctx);
            }

            offset += entry->rec_len;
        }

        bytes_read += remaining;
    }

    return 0;
}

/* ── Lectura de archivo ─────────────────────────────────────────── */

int64_t ext2_read_file(uint32_t ino, void *buf, uint64_t offset, uint64_t count) {
    if (!ext2_mounted) return -EIO;
    if (!buf) return -EINVAL;

    struct ext2_inode inode;
    int ret = ext2_read_inode(ino, &inode);
    if (ret < 0) return ret;

    /* Solo archivos regulares */
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) {
        return -EINVAL;
    }

    uint32_t file_size = inode.i_size;

    /* Ajustar offset y count a los limites del archivo */
    if (offset >= file_size) return 0;
    if (count > file_size - offset) {
        count = file_size - offset;
    }

    uint8_t *dst = (uint8_t *)buf;
    uint64_t bytes_read = 0;

    while (bytes_read < count) {
        /* Calcular en que bloque logico estamos */
        uint64_t current_pos = offset + bytes_read;
        uint32_t block_index = (uint32_t)(current_pos / fs.block_size);
        uint32_t block_offset = (uint32_t)(current_pos % fs.block_size);

        /* Solo bloques directos por ahora */
        if (block_index >= EXT2_NDIR_BLOCKS) {
            LOG_WARN("ext2: file read hit indirect block limit (block %u)", block_index);
            break;
        }

        uint32_t block_num = inode.i_block[block_index];
        if (block_num == 0) {
            /* Bloque sparse (agujero): llenar con ceros */
            uint32_t chunk = fs.block_size - block_offset;
            if (chunk > count - bytes_read) chunk = (uint32_t)(count - bytes_read);
            memset(dst + bytes_read, 0, chunk);
            bytes_read += chunk;
            continue;
        }

        uint8_t *block_data = (uint8_t *)ext2_block_ptr(block_num);
        if (!block_data) return -EIO;

        /* Copiar datos de este bloque */
        uint32_t chunk = fs.block_size - block_offset;
        if (chunk > count - bytes_read) chunk = (uint32_t)(count - bytes_read);

        memcpy(dst + bytes_read, block_data + block_offset, chunk);
        bytes_read += chunk;
    }

    return (int64_t)bytes_read;
}

/* ── Diagnostico ────────────────────────────────────────────────── */

void ext2_dump_info(void) {
    if (!ext2_mounted) {
        LOG_WARN("ext2: not mounted");
        return;
    }

    kprintf("\n--- ext2 Filesystem Info ---\n");
    kprintf("  Block size:      %u bytes\n", fs.block_size);
    kprintf("  Inode size:      %u bytes\n", fs.inode_size);
    kprintf("  Total inodes:    %u\n", fs.sb.s_inodes_count);
    kprintf("  Total blocks:    %u\n", fs.sb.s_blocks_count);
    kprintf("  Free blocks:     %u\n", fs.sb.s_free_blocks_count);
    kprintf("  Free inodes:     %u\n", fs.sb.s_free_inodes_count);
    kprintf("  Blocks/group:    %u\n", fs.sb.s_blocks_per_group);
    kprintf("  Inodes/group:    %u\n", fs.sb.s_inodes_per_group);
    kprintf("  Block groups:    %u\n", fs.groups_count);
    kprintf("  First data blk:  %u\n", fs.sb.s_first_data_block);
}

/* ── Callback para listar root ──────────────────────────────────── */

static void print_dir_entry(const char *name, uint32_t name_len,
                            uint32_t inode, uint8_t file_type, void *ctx) {
    (void)ctx;
    /* Imprimir nombre con longitud conocida (no null-terminated en disco) */
    const char *type_str;
    switch (file_type) {
        case EXT2_FT_REG_FILE: type_str = "FILE"; break;
        case EXT2_FT_DIR:      type_str = "DIR";  break;
        case EXT2_FT_SYMLINK:  type_str = "LINK"; break;
        default:               type_str = "????"; break;
    }

    kprintf("  [%4s] inode=%u  ", type_str, inode);
    /* Imprimir nombre caracter por caracter (no es null-terminated) */
    for (uint32_t i = 0; i < name_len; i++) {
        kprintf("%c", name[i]);
    }
    kprintf("\n");
}

void ext2_list_root(void) {
    if (!ext2_mounted) {
        LOG_WARN("ext2: not mounted, cannot list root");
        return;
    }

    kprintf("\n--- ext2 Root Directory (inode %u) ---\n", EXT2_ROOT_INO);
    int ret = ext2_list_dir(EXT2_ROOT_INO, print_dir_entry, 0);
    if (ret < 0) {
        LOG_ERROR("ext2: failed to list root (err=%d)", ret);
    }
}

/* ── VFS integration: file_ops para archivos ext2 ───────────────── */

static int64_t ext2_vfs_read(struct vnode *vn, void *buf, uint64_t count) {
    if (!vn || !vn->private_data) return -EINVAL;
    /* Usamos private_data como puntero al numero de inode (cast directo) */
    uint32_t ino = (uint32_t)(uint64_t)vn->private_data;
    /* Lectura desde offset 0 por simplicidad (VFS todavia no tiene seek) */
    return ext2_read_file(ino, buf, 0, count);
}

static struct file_ops ext2_file_ops = {
    .open  = 0,
    .close = 0,
    .read  = ext2_vfs_read,
    .write = 0,  /* Solo lectura */
    .ioctl = 0,
};

/* Registrar el filesystem ext2 como dispositivo de bloque en VFS */
static void ext2_register_vfs(void) {
    vfs_register_device("ext2", VN_BLOCKDEV, &ext2_file_ops,
                        (void *)(uint64_t)EXT2_ROOT_INO);
    LOG_OK("ext2: registered as VFS device 'ext2'");
}
