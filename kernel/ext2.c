/*
 * Anykernel OS — ext2 Filesystem (Read-Only) Implementation
 *
 * Basic ext2 reading over ramdisk. Supports:
 *   - Parsing of superblock and group descriptor table
 *   - Reading inodes by number
 *   - Directory listing
 *   - File reading (direct blocks only)
 *
 * The ramdisk is loaded as a Limine module at boot.
 * It is registered as a filesystem in the VFS.
 */

#include "ext2.h"
#include "string.h"
#include "kprintf.h"
#include "log.h"
#include "errno.h"
#include "vfs.h"

/* ── Global filesystem state ───────────────────────────────────── */

static struct ext2_fs fs;
static bool ext2_mounted = false;

/* Forward declaration — defined at the end of the file */
static void ext2_register_vfs(void);

/* ── Internal helpers ───────────────────────────────────────────── */

/* Get a pointer to a block within the ramdisk */
static inline void *ext2_block_ptr(uint32_t block_num) {
    uint64_t offset = (uint64_t)block_num * fs.block_size;
    if (offset + fs.block_size > fs.disk_size) {
        return 0; /* Out of range */
    }
    return fs.disk + offset;
}

/* ── Initialization ─────────────────────────────────────────────── */

int ext2_init(void *ramdisk_base, uint64_t ramdisk_size) {
    if (!ramdisk_base || ramdisk_size < EXT2_SUPERBLOCK_OFFSET + sizeof(struct ext2_superblock)) {
        LOG_ERROR("ext2: ramdisk too small or NULL");
        return -EINVAL;
    }

    fs.disk = (uint8_t *)ramdisk_base;
    fs.disk_size = ramdisk_size;

    /* Read superblock from offset 1024 */
    memcpy(&fs.sb, fs.disk + EXT2_SUPERBLOCK_OFFSET, sizeof(struct ext2_superblock));

    /* Validate magic number */
    if (fs.sb.s_magic != EXT2_SUPER_MAGIC) {
        LOG_ERROR("ext2: bad magic 0x%x (expected 0x%x)",
                  fs.sb.s_magic, EXT2_SUPER_MAGIC);
        return -EINVAL;
    }

    /* Calculate block size (valid range: 0-6, i.e. 1K-64K) */
    if (fs.sb.s_log_block_size > EXT2_MAX_LOG_BLOCK_SIZE) {
        LOG_ERROR("ext2: invalid s_log_block_size %u", fs.sb.s_log_block_size);
        return -EINVAL;
    }
    fs.block_size = EXT2_BASE_BLOCK_SIZE << fs.sb.s_log_block_size;

    /* Inode size: rev 0 uses fixed 128 bytes, rev >= 1 uses s_inode_size */
    if (fs.sb.s_rev_level >= 1 && fs.sb.s_inode_size > 0) {
        fs.inode_size = fs.sb.s_inode_size;
    } else {
        fs.inode_size = EXT2_REV0_INODE_SIZE;
    }

    /* Number of block groups */
    fs.groups_count = (fs.sb.s_blocks_count + fs.sb.s_blocks_per_group - 1)
                      / fs.sb.s_blocks_per_group;

    /* The GDT is in the block following the superblock.
     * If block_size == 1024, superblock is in block 1, GDT in block 2.
     * If block_size >= 2048, superblock is in block 0 (offset 1024), GDT in block 1. */
    uint32_t gdt_block;
    if (fs.block_size == EXT2_BASE_BLOCK_SIZE) {
        gdt_block = EXT2_GDT_BLOCK_1K;
    } else {
        gdt_block = EXT2_GDT_BLOCK_LARGE;
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

    /* Register in VFS as a block device */
    ext2_register_vfs();

    return 0;
}

/* ── Inode reading ───────────────────────────────────────────────── */

int ext2_read_inode(uint32_t ino, struct ext2_inode *out) {
    if (!ext2_mounted) return -EIO;
    if (ino == 0 || ino > fs.sb.s_inodes_count) return -EINVAL;

    /* Inodes are numbered from 1. Calculate group and offset. */
    uint32_t group = (ino - 1) / fs.sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % fs.sb.s_inodes_per_group;

    if (group >= fs.groups_count) return -EINVAL;

    /* The inode table for this group starts at bg_inode_table */
    uint32_t inode_table_block = fs.gdt[group].bg_inode_table;
    uint64_t inode_offset = (uint64_t)inode_table_block * fs.block_size
                          + (uint64_t)index * fs.inode_size;

    if (inode_offset + sizeof(struct ext2_inode) > fs.disk_size) {
        return -EIO;
    }

    memcpy(out, fs.disk + inode_offset, sizeof(struct ext2_inode));
    return 0;
}

/* ── Directory listing ──────────────────────────────────────────── */

int ext2_list_dir(uint32_t dir_ino, ext2_dir_callback callback, void *ctx) {
    if (!ext2_mounted) return -EIO;
    if (!callback) return -EINVAL;

    struct ext2_inode inode;
    int ret = ext2_read_inode(dir_ino, &inode);
    if (ret < 0) return ret;

    /* Verify that it is a directory */
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        return -EINVAL;
    }

    uint32_t size = inode.i_size;
    uint32_t bytes_read = 0;

    /* Iterate over direct blocks of the directory */
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

            /* Basic validation to avoid infinite loops */
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

/* ── File reading ──────────────────────────────────────────────── */

int64_t ext2_read_file(uint32_t ino, void *buf, uint64_t offset, uint64_t count) {
    if (!ext2_mounted) return -EIO;
    if (!buf) return -EINVAL;

    struct ext2_inode inode;
    int ret = ext2_read_inode(ino, &inode);
    if (ret < 0) return ret;

    /* Regular files only */
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) {
        return -EINVAL;
    }

    uint32_t file_size = inode.i_size;

    /* Adjust offset and count to file boundaries */
    if (offset >= file_size) return 0;
    if (count > file_size - offset) {
        count = file_size - offset;
    }

    uint8_t *dst = (uint8_t *)buf;
    uint64_t bytes_read = 0;

    while (bytes_read < count) {
        /* Calculate which logical block we are in */
        uint64_t current_pos = offset + bytes_read;
        uint32_t block_index = (uint32_t)(current_pos / fs.block_size);
        uint32_t block_offset = (uint32_t)(current_pos % fs.block_size);

        /* Direct blocks only for now */
        if (block_index >= EXT2_NDIR_BLOCKS) {
            LOG_WARN("ext2: file read hit indirect block limit (block %u)", block_index);
            break;
        }

        uint32_t block_num = inode.i_block[block_index];
        if (block_num == 0) {
            /* Sparse block (hole): fill with zeros */
            uint32_t chunk = fs.block_size - block_offset;
            if (chunk > count - bytes_read) chunk = (uint32_t)(count - bytes_read);
            memset(dst + bytes_read, 0, chunk);
            bytes_read += chunk;
            continue;
        }

        uint8_t *block_data = (uint8_t *)ext2_block_ptr(block_num);
        if (!block_data) return -EIO;

        /* Copy data from this block */
        uint32_t chunk = fs.block_size - block_offset;
        if (chunk > count - bytes_read) chunk = (uint32_t)(count - bytes_read);

        memcpy(dst + bytes_read, block_data + block_offset, chunk);
        bytes_read += chunk;
    }

    return (int64_t)bytes_read;
}

/* ── Diagnostics ────────────────────────────────────────────────── */

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

/* ── Callback for listing root ──────────────────────────────────── */

static void print_dir_entry(const char *name, uint32_t name_len,
                            uint32_t inode, uint8_t file_type, void *ctx) {
    (void)ctx;
    /* Print name with known length (not null-terminated on disk) */
    const char *type_str;
    switch (file_type) {
        case EXT2_FT_REG_FILE: type_str = "FILE"; break;
        case EXT2_FT_DIR:      type_str = "DIR";  break;
        case EXT2_FT_SYMLINK:  type_str = "LINK"; break;
        default:               type_str = "????"; break;
    }

    kprintf("  [%4s] inode=%u  ", type_str, inode);
    /* Print name character by character (not null-terminated) */
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

/* ── VFS integration: file_ops for ext2 files ───────────────────── */

static int64_t ext2_vfs_read(struct vnode *vn, void *buf, uint64_t count) {
    if (!vn || !vn->private_data) return -EINVAL;
    /* We use private_data as a pointer to the inode number (direct cast) */
    uint32_t ino = (uint32_t)(uint64_t)vn->private_data;
    /* Read from offset 0 for simplicity (VFS does not have seek yet) */
    return ext2_read_file(ino, buf, 0, count);
}

static struct file_ops ext2_file_ops = {
    .open  = 0,
    .close = 0,
    .read  = ext2_vfs_read,
    .write = 0,  /* Read-only */
    .ioctl = 0,
};

/* Register the ext2 filesystem as a block device in VFS */
static void ext2_register_vfs(void) {
    vfs_register_device("ext2", VN_BLOCKDEV, &ext2_file_ops,
                        (void *)(uint64_t)EXT2_ROOT_INO);
    LOG_OK("ext2: registered as VFS device 'ext2'");
}
