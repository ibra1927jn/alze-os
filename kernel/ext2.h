/*
 * Anykernel OS — ext2 Filesystem (Read-Only)
 *
 * Basic read-only ext2 support over ramdisk (boot module).
 * Implements: superblock, group descriptors, inodes, directories,
 * file reading (direct blocks only for now).
 *
 * The backing store is a ramdisk loaded as a Limine module.
 * Does not require AHCI or a real disk.
 */

#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── ext2 constants ────────────────────────────────────────────── */

#define EXT2_SUPER_MAGIC       0xEF53
#define EXT2_SUPERBLOCK_OFFSET 1024   /* Superblock starts at byte 1024 */
#define EXT2_BASE_BLOCK_SIZE   1024   /* block_size = EXT2_BASE_BLOCK_SIZE << s_log_block_size */
#define EXT2_REV0_INODE_SIZE   128    /* Default inode size for revision 0 */
#define EXT2_MAX_LOG_BLOCK_SIZE 6    /* max shift: 1024 << 6 = 64KB      */
#define EXT2_GDT_BLOCK_1K      2    /* GDT block when block_size == 1024 */
#define EXT2_GDT_BLOCK_LARGE   1    /* GDT block when block_size >= 2048 */

/* Inode types (i_mode field, bits 12-15) */
#define EXT2_S_IFREG  0x8000  /* Regular file */
#define EXT2_S_IFDIR  0x4000  /* Directory */
#define EXT2_S_IFMT   0xF000  /* File type mask */

/* Special inode: root directory is always inode 2 */
#define EXT2_ROOT_INO  2

/* Direct blocks per inode */
#define EXT2_NDIR_BLOCKS  12
#define EXT2_IND_BLOCK    12  /* Single indirect (not implemented yet) */
#define EXT2_DIND_BLOCK   13  /* Double indirect (not implemented yet) */
#define EXT2_TIND_BLOCK   14  /* Triple indirect (not implemented yet) */
#define EXT2_N_BLOCKS     15

/* ── Superblock ─────────────────────────────────────────────────── */

struct ext2_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;       /* block_size = 1024 << s_log_block_size */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* Only the fields we need — more exist in rev >= 1 */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    /* ... rest ignored for basic reading */
} __attribute__((packed));

/* ── Block Group Descriptor ─────────────────────────────────────── */

struct ext2_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed));

/* ── Inode ──────────────────────────────────────────────────────── */

struct ext2_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;          /* Size in bytes (low 32 bits) */
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;        /* Number of 512-byte sectors */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT2_N_BLOCKS]; /* Pointers to data blocks */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;       /* i_size_high en rev >= 1 */
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed));

/* ── Directory Entry ────────────────────────────────────────────── */

struct ext2_dir_entry {
    uint32_t inode;
    uint16_t rec_len;         /* Total length of this entry */
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];          /* Name (NOT null-terminated on disk) */
} __attribute__((packed));

/* file_type values */
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

/* ── Mounted filesystem state ──────────────────────────────────── */

struct ext2_fs {
    uint8_t              *disk;         /* Base pointer to ramdisk */
    uint64_t              disk_size;    /* Total ramdisk size */
    struct ext2_superblock sb;          /* Copy of superblock */
    uint32_t              block_size;   /* 1024 << s_log_block_size */
    uint32_t              groups_count; /* Number of block groups */
    uint32_t              inode_size;   /* Size of each inode on disk */
    struct ext2_group_desc *gdt;        /* Pointer to group descriptor table */
};

/* ── API ────────────────────────────────────────────────────────── */

/*
 * Initialize ext2 from a ramdisk (Limine boot module).
 * Parses superblock, validates magic, sets up group descriptors.
 * Returns 0 on success, negative errno on failure.
 */
int ext2_init(void *ramdisk_base, uint64_t ramdisk_size);

/*
 * Read an inode by number. Computes block group and offset.
 * Returns 0 on success, negative errno on failure.
 */
int ext2_read_inode(uint32_t ino, struct ext2_inode *out);

/*
 * List directory entries from a directory inode.
 * Calls `callback` for each entry. User data passed through.
 * Returns 0 on success, negative errno on failure.
 */
typedef void (*ext2_dir_callback)(const char *name, uint32_t name_len,
                                  uint32_t inode, uint8_t file_type,
                                  void *ctx);
int ext2_list_dir(uint32_t dir_ino, ext2_dir_callback callback, void *ctx);

/*
 * Read file data from a regular file inode (direct blocks only).
 * Reads up to `count` bytes starting at `offset` into `buf`.
 * Returns bytes read, or negative errno on failure.
 */
int64_t ext2_read_file(uint32_t ino, void *buf, uint64_t offset, uint64_t count);

/*
 * Print filesystem info to serial (diagnostic).
 */
void ext2_dump_info(void);

/*
 * List the root directory contents to serial (diagnostic).
 */
void ext2_list_root(void);

#endif /* EXT2_H */
