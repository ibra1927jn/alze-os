# ALZE OS — Filesystem & Storage Review

Date: 2026-04-21 · Reviewer: senior FS auditor · Repo: `/root/repos/alze-os`

## Scope

| File | LOC | Role |
|------|----:|------|
| `kernel/vfs.h` | 146 | VFS public API + structs |
| `kernel/vfs.c` | 269 | VFS implementation (device table, fds) |
| `kernel/ext2.h` | 215 | ext2 on-disk layout + API |
| `kernel/ext2.c` | 485 | ext2 read-only driver |
| `kernel/devfs.c` | 192 | /dev nodes (serial, console, kb, null, zero, random) |
| `kernel/ramdisk.c` | 102 | Limine module → ramdisk → ext2 mount |
| `kernel/ramdisk.h` | 33 | Ramdisk state struct |
| `kernel/errno.h` | 30 | POSIX-ish errno constants |
| `kernel/string.c` | 229 | Freestanding mem*/str* |
| `kernel/string.h` | 49 | Declarations |

Total reviewed: ~1750 LOC. Cross-refs: `kernel/rwlock.h`, `kernel/bitmap_alloc.h`, `ERRORES.md` (32 L), `kernel/main.c`.

## VFS design

Flat, pre-1980s model. There is no mount table, no dentry cache, no inode cache, no path resolution at the VFS level. A single global array `devices[VFS_MAX_DEVICES=16]` (`vfs.c:17`) holds flat vnodes keyed by short name. `vfs_lookup(name)` (`vfs.c:65-72`) is a linear `strcmp` scan — the string is matched as-is; a caller passing `"/dev/serial"` will **miss** `"serial"`. Callers must know the bare name. ext2 is exposed as a single opaque blockdev vnode `"ext2"` whose `private_data` is just the root inode number (`ext2.c:483`); directory traversal through VFS is impossible.

**Path resolution**: does not exist in VFS. Only `ext2_path_resolve` (`ext2.c:367-398`) resolves `/a/b/c` against the mounted ext2 image. No permission checks (no uid/gid concept in VFS at all). No symlink handling (file_type `EXT2_FT_SYMLINK` is silently ignored by `ext2_lookup_cb`). No symlink loop counter — moot because symlinks aren't followed at all, but means `ln -s` targets are simply un-openable.

**File handle model**: `struct file` has `{vnode*, flags, offset, in_use}` (`vfs.h:93-98`). **fd table is global, not per-task** — a comment at `vfs.c:21` concedes "TODO: per-task fds". `VFS_MAX_FDS = 16` system-wide. This means every task shares stdin/stdout/stderr slots with every other task — a show-stopper for any multitasking beyond the kernel log console. `FD_STDIN/OUT/ERR` constants exist (`vfs.h:106-108`) but nothing installs them; slots 0/1/2 are just freely allocated.

**FD allocation**: `id_pool` bitmap (`vfs.c:24`) gives O(1) BSF-based allocation — a good choice.

**Locking**: a single global `rwlock vfs_rwlock` (`vfs.c:19`). Writers take it in `register_device`, `open`, `close`, `dup`. **`vfs_read`, `vfs_write`, `vfs_ioctl`, `vfs_seek`, `vfs_tell` take NO lock at all.** No `rwlock_read_lock` call exists anywhere in the file (confirmed by grep). On SMP this means a `close(fd)` racing with `read(fd)` can free the vnode slot or flip `in_use` between the check and the dispatch (TOCTOU at `vfs.c:143` → `vfs.c:145`); reader observes `in_use=true`, closer flips it and decrements refcount, reader then dereferences `vn->ops` on a stale vnode. There is also no per-inode lock — the offset field `fd_table[fd].offset` is read-modify-written at `vfs.c:161, 189` with no atomics, so two threads sharing an fd (after `dup`) will clobber each other's offsets.

## ext2 driver

**Read-only**. `write = NULL` in `ext2_file_ops` (`ext2.c:476`). No bitmap code, no block/inode allocation, no `O_APPEND` — the flag exists in `vfs.h` but is never consulted, and is moot while the driver is RO. No journal — it is ext2, not ext3 — so crash consistency is "run fsck on the host". Since writes aren't implemented this isn't an active risk, only a future one.

**Superblock validation** (`ext2.c:42-94`):
- size ≥ 1024 + sizeof(sb) ✓ (`ext2.c:43`, fix from ERRORES 2026-03-28)
- magic == 0xEF53 ✓ (`ext2.c:55`)
- `s_log_block_size ≤ 6` ✓ (`ext2.c:62`)
- **NOT checked**: `s_state` (clean/error), `s_rev_level` (used at L69 but no upper bound check), `s_creator_os`, `s_feature_incompat` (completely ignored — a disk with `INCOMPAT_FILETYPE` or `INCOMPAT_EXTENTS` mounts happily), `s_inodes_per_group == 0` (div-by-zero at `ext2.c:114` if hostile image sets it 0), `s_blocks_per_group == 0` (div-by-zero at `ext2.c:77`), `s_inodes_count == 0`, `s_blocks_count == 0`, `s_inode_size` range (`ext2.c:70` accepts any non-zero u16 — an image with `s_inode_size=1` breaks address math, and `s_inode_size > block_size` is nonsensical but accepted).

**GDT validation**: `fs.gdt = ext2_block_ptr(gdt_block)` (`ext2.c:89`). `ext2_block_ptr` (`ext2.c:32-38`) checks `offset + block_size > disk_size` — so GDT's first block is verified, but **the GDT can span many blocks and only the first block is bounds-checked**. With `groups_count` possibly in the thousands, `fs.gdt[group]` at `ext2.c:120` can read well past the verified block, and well past disk_size with a hostile image that sets `s_blocks_count` huge. No array bounds check on `group` beyond `group < fs.groups_count` (`ext2.c:117`) — but `groups_count` itself is computed from untrusted on-disk fields.

**Inode lookup** (`ext2.c:109-130`): computes `inode_offset = inode_table_block * block_size + index * inode_size`. Both multiplications are performed in `uint64_t` (good). Bounds checked against `disk_size` (`ext2.c:124`) — good. But `bg_inode_table` is read straight from the GDT without sanity (e.g., could be 0, could be past end-of-disk) and is only caught by the `offset > disk_size` check at L124.

**Indirect resolution** (`ext2.c:200-270`): handles direct / IND / DIND / TIND. Each indirect fetch goes through `ext2_block_ptr` which bounds-checks. However, **the indirect block itself is indexed without a bounds check inside the block**: `ind[block_index]` at `ext2.c:218` assumes `block_index < ptrs_per_block` (guaranteed by the surrounding `if`), but the read `ind[ind_idx]` at `ext2.c:234` and `ind[dir_idx]` at `ext2.c:238` also assume indices are < ptrs_per_block by the math — that's correct. The real risk: **the block number stored in the indirect block is untrusted** — if it points outside the disk, `ext2_block_ptr` returns NULL, which is then checked. Good. But if the pointer points to e.g. the superblock or the inode table itself, the driver happily reads it as file data — not a crash, but a tool for crafting confused-deputy reads.

**Directory iteration** (`ext2.c:134-189`): validates `rec_len != 0` and `rec_len <= remaining - offset` (`ext2.c:173`). **BUT**: no validation that `name_len < rec_len - 8` (the dir_entry header). A malicious directory with `rec_len=8, name_len=255` will cause `callback` (`ext2.c:178`) to read 255 bytes of name starting inside the next entry, then pass that pointer+length to consumers like `print_dir_entry` (`ext2.c:443-446`) which dereferences all 255 bytes. No overrun past the block because the next iteration's `offset += rec_len` stays in-block, but the name the callback sees spills beyond its entry. For `ext2_lookup_cb` (`ext2.c:337-349`) this means the comparison may scan into adjacent entry bytes — harmless in practice (no OOB) but a correctness bug: a hostile image could make `ext2_lookup` succeed on a name the user did not type, because trailing garbage matches.

**Path resolution** (`ext2.c:367-398`): supports `.` (`ext2.c:388`). Comment says "supports `..` for parent traversal" (`ext2.h:196`) but **no code handles `..`** — it falls through to `ext2_lookup` which will find the on-disk `..` entry by name. That works for normal dirs but there's no `chroot`-style confinement and no protection against a malicious image where `..` in `/` points somewhere surprising. No symlink support (symlink inode type silently returns the symlink inode, which then fails as not-a-dir on next component). No loop detection needed because symlinks aren't followed — until someone adds that feature.

**Endianness**: ext2 is defined as little-endian on disk. The driver just casts (`memcpy` into host struct). x86_64 is LE so it works, but there are zero `le16_to_cpu`-style wrappers — porting to BE or loading a BE-built image will silently corrupt every field.

**Fragment handling**: `s_log_frag_size` and `bg_pad`/`bg_reserved` are read and ignored. Fine for ext2 (fragments are almost never used), but not explicitly rejected.

## ramdisk

Backing store: Limine boot module, first one only (`ramdisk.c:77`). No persistence — RAM-backed, gone at reboot. Basic null/empty checks (`ramdisk.c:78`). `vfs_register_device("ramdisk", VN_BLOCKDEV, ...)` (`ramdisk.c:92`) exposes it as a block dev with a `read` that just `memcpy`s from offset 0 up to `count` (`ramdisk.c:43-50`) — this **ignores the fd offset entirely**. Calling `vfs_read` on the ramdisk vnode twice returns the same initial bytes both times (the VFS does advance `fd_table[fd].offset`, but the device `read` doesn't implement `pread` and doesn't consult the offset, so you get byte 0..N on every call). The device is essentially useless for anything beyond "peek at the header".

`ext2_init` is called unconditionally on the module (`ramdisk.c:95`) — reasonable since failure logs and continues.

## devfs

Six devices: serial, console, keyboard, null, zero, random (`devfs.c:180-192`). Each is a statically-declared `file_ops` with `{read, write}` pointers that call into existing drivers. No `mknod` syscall — registration is hard-coded at boot. No hotplug (nothing calls `vfs_register_device` after init). No unregister path at all (`vfs_unregister_device` does not exist) — if a driver were unloaded, the `devices[]` slot would dangle.

`random_ops` uses xorshift64 seeded from TSC (`devfs.c:134-176`) — a reasonable non-crypto PRNG but should not be used for secrets. `null_write` is reused as `zero_ops.write` (`devfs.c:130`) — nice touch.

Dispatch is via `file_ops` vtable; no type-checking (writing to `/dev/zero` silently discards, not EPERM — that's Linux-compatible).

## String utilities

`memset`, `memcpy`, `memmove`, `memcmp`, `strlen`, `strcmp`, `strncmp`, `strncpy`, `strcat`, `strchr`, `strstr`, `memchr`. **No `strlcpy`, no `strnlen`, no `snprintf`-family** (outside of `kprintf`). `memcpy` and `memset` use `rep movsq`/`rep stosq` with byte residual (`string.c:33-48, 74-88`) — fast. `memmove` uses `std; rep movsq` for backward overlap (`string.c:130-137`), confirmed correct and noted as the fix from `ERRORES.md:31`.

**`strncpy` is C-standard unsafe**: does NOT null-terminate if `strlen(src) >= n` (`string.c:186-195`). Every caller must manually `dest[n-1] = '\0'`. `vfs_register_device` (`vfs.c:50-51`) does this correctly. `sched.c:285`, `sched.c:589`, `sched.c:657`, `vma.c:48` use `strncpy` — several of those do NOT manually null-terminate (e.g. `sched.c:657` passes the full buffer size, so if `"init"` is shorter it's fine, but if a longer name is passed to any of these, they silently produce un-terminated strings that will overflow subsequent `strlen`/`kprintf("%s")`).

`strcat` is fully unbounded (`string.c:197-202`) — no `strncat`. Present but unused in the FS code.

## Highlights

- ERRORES.md:18 shows a prior disk-size bounds-check bug already caught and fixed — the team learns from incidents.
- `ext2_block_ptr` centralizes bounds checks (`ext2.c:32-38`) — good pattern.
- `u64` promotion in offset math at `ext2.c:121-122` avoids the classic 32-bit overflow on `block * block_size`.
- File offsets in indirect resolution (`ext2.c:200-270`) use 64-bit capacity math via `uint32_t ptrs_per_block` products that are safe only because block_size ≤ 64 KB caps `ptrs_per_block` at 16384 and `dind_capacity` at ~2.6e8 — fits in u32. If block_size cap were raised, this silently overflows.
- RWlock design exists and `register_device` / `open` / `close` / `dup` use it consistently.
- ext2 driver refuses to parse `s_log_block_size > 6` (`ext2.c:62`) — prevents shift UB on 64-bit.
- `memmove` backward path uses `std; rep movsq` correctly (`string.c:130-137`).

## Issues found

Priority tags: **[CRIT]** data-corruption or crash · **[HIGH]** exploitable by hostile image · **[MED]** correctness · **[LOW]** hygiene.

1. **[CRIT] No read-lock on hot path.** `vfs_read`/`vfs_write`/`vfs_ioctl`/`vfs_seek`/`vfs_tell` never acquire `vfs_rwlock` (`vfs.c:141-205, 209-241`). A concurrent `vfs_close` on the same fd (`vfs.c:114`) will set `in_use=false`, null `vnode`, and decrement refcount between the reader's `in_use` check (`vfs.c:143`) and its vnode dereference (`vfs.c:145`). Race → NULL-deref or use-after-... well, use-after-clear. On a single-CPU config the scheduler could still preempt mid-read. Fix: take `rwlock_read_lock` around the fd lookup and copy `vnode*` onto the stack before dropping, OR make `fd_table[].in_use` + `vnode` pointer updates atomic with a refcount protocol.

2. **[CRIT] Global fd table, not per-task.** `static struct file fd_table[16]` (`vfs.c:23`). Comment acknowledges it (`vfs.c:21`). Any task opens/reads/writes on the same 16 slots as all other tasks. Breaks process isolation completely. Fix: move fd_table into the `task_struct`.

3. **[HIGH] Div-by-zero on hostile superblock.** `ext2.c:76-77` divides by `s_blocks_per_group`, `ext2.c:114-115` divides by `s_inodes_per_group`. Neither is validated as non-zero before use. An image with `s_inodes_per_group=0` crashes the kernel on first `ext2_read_inode`. Fix: reject sb if either is 0.

4. **[HIGH] `s_inode_size` not range-checked.** `ext2.c:69-73` accepts any nonzero u16. Values like 1, 2, 17, or `> block_size` produce garbage inode reads or per-inode bounds math that silently wraps. Fix: require `inode_size >= 128`, `inode_size <= block_size`, and `inode_size` be a power of 2.

5. **[HIGH] GDT bounds check covers only first block.** `ext2.c:89` bounds-checks the GDT's first block via `ext2_block_ptr`, but `fs.gdt[group]` (`ext2.c:120`) can walk off the end of the disk for large `groups_count`. Fix: bounds-check `gdt_block_count * block_size` against `disk_size`, where `gdt_block_count = ceil(groups_count * sizeof(group_desc) / block_size)`.

6. **[HIGH] Feature flags ignored.** `s_feature_incompat` / `s_feature_ro_compat` / `s_feature_compat` are never read (`ext2.c:42-105`). A disk with `INCOMPAT_FILETYPE` or `INCOMPAT_META_BG` (ext2 extension) mounts happily and will misparse. Fix: reject any unknown `feature_incompat` bit. A feature-incompat check is effectively the canonical hostile-image defense.

7. **[HIGH] Directory entry `name_len` not validated against `rec_len`.** `ext2.c:173` checks `rec_len` but not `name_len`. An entry with `rec_len=8, name_len=200` makes `callback` read 200 bytes past the entry header (`ext2.c:178`). No buffer overflow in the driver itself, but `print_dir_entry` (`ext2.c:443-446`) will dump 200 bytes of adjacent-entry memory to the console, and `ext2_lookup_cb` (`ext2.c:337-349`) may silently match extra bytes. Fix: `if (name_len + 8 > rec_len) break;`.

8. **[HIGH] `bg_inode_table` not validated.** `ext2.c:120` trusts the on-disk block number. A value of 0 would cause inode reads to return superblock bytes as inodes; a huge value is caught only by the final offset-vs-disk-size check at L124. Fix: validate `inode_table_block >= first_data_block && inode_table_block < s_blocks_count`.

9. **[MED] Offset ignored on ramdisk read.** `ramdisk_vfs_read` (`ramdisk.c:43-50`) ignores offset — always returns bytes 0..count. VFS advances `fd_table[fd].offset` correctly, but the device never reads from that offset. Fix: implement `pread` not `read`, using `offset` + bounds-check.

10. **[MED] ext2 VFS adapter also ignores offset.** `ext2_vfs_read` (`ext2.c:464-470`) hard-codes offset=0. Every VFS read of the ext2 vnode returns the first `count` bytes of the root inode's data. Fix: same — register `pread` and pass `offset`.

11. **[MED] `..` path component not specially handled.** `ext2.h:196` claims support, but `ext2_path_resolve` (`ext2.c:367-398`) does not skip/validate `..`; it relies on the on-disk `..` entry. Usually benign, but diverges from the docstring.

12. **[MED] No symlink support and no loop detection.** A symlink inode returned from `ext2_lookup` is not resolved; opening fails. Not a bug per se, but if someone adds symlink resolution later they must add a counter — there is no infrastructure for it today.

13. **[MED] `vfs_close` vnode deref after refcount drop.** `vfs.c:124-134` decrements `vn->refcount`, releases the rwlock, THEN calls `vn->ops->close(vn)` (`vfs.c:134`). If another thread dropped the last ref and e.g. unregistered (though no unregister exists today), the close would touch freed memory. Today safe because nothing unregisters; fragile by design.

14. **[MED] Refcount is 32-bit and uses non-return atomic ops.** `__sync_fetch_and_sub(&vn->refcount, 1)` (`vfs.c:127`) — the caller never checks whether it hit 0, so there's no "last-close" hook. Fine for current RO VFS; breaks as soon as you add filesystem unmount.

15. **[MED] `vfs_open` calls `vn->ops->open` AFTER marking fd in-use.** `vfs.c:89-108`. If `open` fails, the rollback path correctly frees the fd (`vfs.c:101-105`). But between marking `in_use=true` at L92 and the open callback at L99, another thread can already observe and use the fd. For read-only stateless devices this is harmless; for a device whose `open` performs init (none today), reads see uninitialized state.

16. **[LOW] `strncpy` everywhere without sentinel.** `vfs.c:50-51` does the manual terminator correctly. Other call sites (`sched.c`, `vma.c`) do not always. The whole codebase would benefit from a real `strlcpy`. Add one to `string.c`.

17. **[LOW] Endianness assumptions unhidden.** ext2 on-disk fields are read via `memcpy` into host structs (`ext2.c:52, 128`). No `le32_to_cpu`. Works on x86_64 but is undocumented and a portability landmine. Add a `#if __BYTE_ORDER != __LITTLE_ENDIAN #error` or helper macros.

18. **[LOW] `vfs_seek` on devices with no size concept.** `SEEK_END` returns `-ENOTSUP` correctly (`vfs.c:223`), but `SEEK_SET` on a chardev (e.g. `/dev/serial`) silently succeeds and sets an offset that the device never honors. Consider blocking seek on `VN_CHARDEV`.

19. **[LOW] `vfs_tell` returns 0 instead of `-EBADF` on bad fd.** `vfs.c:237-241`. Caller cannot distinguish "bad fd" from "offset is 0". Non-standard.

20. **[LOW] `vfs_lookup` not guarded by the rwlock.** `vfs.c:65-72` reads `device_count` and `devices[i].name` without locking. Since `register_device` only appends and `device_count` is written last, this is mostly safe today on x86 (atomic u32 load), but not guaranteed by the memory model. Either add a read-lock or mark `device_count` `volatile _Atomic`.

21. **[LOW] No validation that `s_first_data_block` matches block size.** Should be 1 for `block_size=1024`, 0 otherwise. Mismatch with `EXT2_GDT_BLOCK_1K`/`EXT2_GDT_BLOCK_LARGE` would silently read wrong GDT.

## Recommendations

- **Per-task fd table** is the single highest-value change; until then multitasking + VFS is unsound.
- Wrap all fd lookups under `rwlock_read_lock(&vfs_rwlock)` at minimum; copy the `vnode*` on the stack, then drop and dispatch.
- Add an `ext2_sb_validate()` static helper that enforces every field (`inodes_per_group > 0`, `blocks_per_group > 0`, `inode_size` in `[128, block_size]`, power-of-2, `first_data_block` consistent, `feature_incompat == 0` or in an allowlist). Call it once in `ext2_init`.
- Validate every directory entry: `name_len + 8 <= rec_len`, `rec_len % 4 == 0`, `rec_len >= 8`.
- Use `ext2_block_ptr` for the **whole** GDT span and every indirect block, but also validate that block numbers retrieved from on-disk metadata are within `[s_first_data_block, s_blocks_count)`.
- Wire ext2's VFS adapter to `pread`/`pwrite` (with offset) and register as `VN_FILE` rather than `VN_BLOCKDEV`. Same for ramdisk.
- Add `strlcpy` and switch all callers.
- Add a `fsck`-style self-test harness that mounts a pre-canned corrupted ext2 image and verifies the driver rejects it cleanly without crashing — belongs alongside `tests/` and `selftest_register` already present at `tests.c:1035`.
- Document endianness assumption; add static-assert.
- Add `vfs_unregister_device` if only to make refcount drains meaningful.

## Risk zones

- **Untrusted disk image**: the driver is a net of the validations above. Today, a carefully crafted 1 MB ext2 image can (a) DoS via div-by-zero (`s_{inodes,blocks}_per_group = 0`), (b) leak adjacent kernel memory to the console via `name_len > rec_len`, (c) redirect inode reads to the superblock via malicious `bg_inode_table`. It cannot, as far as I can tell, achieve arbitrary write — writes are not implemented.
- **Power loss mid-write**: N/A today (RO). When writes land, the absence of a journal means every unclean shutdown requires host fsck; plan for `e2fsck` in the boot/test loop, or skip straight to ext4 with journaling.
- **Large files** (>12 blocks, >1028 blocks, >1 M blocks): indirect/doubly/triply covered and tested by the math in `ext2_resolve_block`. Capacity math is safe for `block_size ≤ 64 KB` (the cap enforced at `ext2.c:62`). Lifting that cap silently overflows `dind_capacity` — add a static assertion.
- **Directories with many entries**: iteration is O(N) per lookup (`ext2_list_dir` → `ext2_lookup_cb`). No hashed-dir support (`INCOMPAT_HASH_INDEX`). Pathological directories with thousands of entries will be slow but not buggy.
- **Concurrent multi-task FS access**: currently unsafe (issues #1 and #2). Do not expose VFS to userspace until both are fixed.
- **Ramdisk replacement**: any future real block device must implement `pread` (not `read`), or the VFS's offset-bump protocol creates silent corruption.
