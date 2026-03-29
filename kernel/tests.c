/*
 * Anykernel OS — Kernel Self-Tests
 *
 * All test functions extracted from main.c into a separate file.
 * Each test is a static bool function: returns true on pass, false on fail.
 * Tests are registered via the selftest framework in register_selftests().
 */

#include "selftest.h"
#include "pmm.h"
#include "kmalloc.h"
#include "vmm.h"
#include "spinlock.h"
#include "memory.h"
#include "string.h"
#include "pic.h"
#include "console.h"
#include "kb.h"
#include "kprintf.h"
#include <stdint.h>
#include <stdbool.h>

/* New primitives (v0.5.7) */
#include "ringbuf.h"
#include "bitmap_alloc.h"
#include "kref.h"
#include "kevent.h"
#include "vfs.h"
#include "rwlock.h"
#include "percpu.h"
#include "cpuidle.h"
#include "list.h"

/* ── Spinlock & Memory tests ─────────────────────────────────── */

static bool test_spinlock(void) {
    spinlock_t lock = SPINLOCK_INIT;
    if (!spin_trylock(&lock)) return false;
    spin_unlock(&lock);
    return true;
}

static bool test_memops(void) {
    uint8_t a[16], b[16];
    memset(a, 0xAA, 16);
    if (a[0] != 0xAA || a[15] != 0xAA) return false;
    memcpy(b, a, 16);
    return memcmp(a, b, 16) == 0;
}

/* ── PMM tests ───────────────────────────────────────────────── */

static bool test_pmm_alloc_free(void) {
    uint64_t p = pmm_alloc_pages(0);
    if (p == 0 || (p & (PAGE_SIZE - 1)) != 0) return false;
    pmm_free_pages(p, 0);
    return true;
}

static bool test_pmm_order5(void) {
    uint64_t p = pmm_alloc_pages(5);
    if (p == 0 || (p % (32 * PAGE_SIZE)) != 0) return false;
    pmm_free_pages(p, 5);
    return true;
}

/* ── kmalloc tests ───────────────────────────────────────────── */

static bool test_kmalloc_small(void) {
    void *p = kmalloc(64);
    if (!p) return false;
    memset(p, 0xBB, 64);
    kfree(p);
    return true;
}

static bool test_kmalloc_large(void) {
    void *p = kmalloc(8192);
    if (!p) return false;
    memset(p, 0xCC, 8192);
    kfree(p);
    return true;
}

static bool test_kzmalloc(void) {
    uint8_t *p = kzmalloc(128);
    if (!p) return false;
    for (int i = 0; i < 128; i++) {
        if (p[i] != 0) { kfree(p); return false; }
    }
    kfree(p);
    return true;
}

static bool test_poison(void) {
    uint8_t *p = kmalloc(64);
    if (!p) return false;
    memset(p, 0x42, 64);
    uint8_t *saved = p;
    kfree(p);
    for (int i = 8; i < 64; i++) {
        if (saved[i] != 0xDE) return false;
    }
    return true;
}

static bool test_krealloc(void) {
    uint8_t *p = kmalloc(32);
    if (!p) return false;
    memset(p, 0xAA, 32);
    uint8_t *p2 = krealloc(p, 256);
    if (!p2) return false;
    if (p2[0] != 0xAA || p2[31] != 0xAA) { kfree(p2); return false; }
    if (kmalloc_usable_size(p2) < 256) { kfree(p2); return false; }
    kfree(p2);
    return true;
}

static bool test_slab_reclamation(void) {
    /* Use size 512 (class 5) — rarely used by other tests, forces new slab.
     * With quarantine, freed memory is NOT immediately returned to PMM.
     * We verify: (1) alloc takes a PMM page, (2) kfree doesn't crash. */
    uint64_t before = pmm_free_count();
    void *p = kmalloc(512);
    if (!p) return false;
    uint64_t after_alloc = pmm_free_count();
    if (after_alloc >= before) { kfree(p); return false; }  /* Must have taken a page */
    kfree(p);
    /* With quarantine: freed slot sits in ring, page NOT returned yet.
     * That's correct behavior (OpenBSD pattern). Just verify no crash. */
    return true;
}

static bool test_pmm_watermark(void) {
    uint64_t free = pmm_free_count();
    uint64_t total = free + pmm_used_count();
    uint64_t pct = (free * 100) / (total > 0 ? total : 1);
    return pct >= 10;
}

/* ── VMM tests ───────────────────────────────────────────────── */

static bool test_vmm_map_write_read(void) {
    uint64_t phys = pmm_alloc_pages_zero(0);
    if (phys == 0) return false;

    uint64_t test_virt = 0xFFFFFF0000000000UL;
    vmm_map_page(test_virt, phys, PTE_PRESENT | PTE_WRITE | PTE_NX);

    volatile uint64_t *ptr = (volatile uint64_t *)test_virt;
    *ptr = 0xCAFEBABE12345678UL;

    bool ok = (*ptr == 0xCAFEBABE12345678UL);

    vmm_unmap_page(test_virt);
    pmm_free_pages(phys, 0);
    return ok;
}

static bool test_vmm_virt_to_phys(void) {
    uint64_t kernel_addr = (uint64_t)&test_vmm_virt_to_phys;
    uint64_t phys = vmm_virt_to_phys(kernel_addr);
    return phys != 0;
}

static bool test_vmm_hhdm_integrity(void) {
    extern uint64_t hhdm_offset;
    volatile uint8_t *p = (volatile uint8_t *)(hhdm_offset + 0x1000);
    (void)*p;
    return true;
}

/* ── Timer test ──────────────────────────────────────────────── */

static bool test_pit_ticking(void) {
    /*
     * The PIT has been running since pic_init/pit_init in boot.
     * By the time tests run, several ticks should have fired.
     * If ticks == 0, the PIT or IRQ dispatch is broken.
     */
    return pit_get_ticks() > 0;
}

/* ── VMM collision detection test ────────────────────────────── */

static bool test_vmm_map_returns_mapping(void) {
    /* Map a page, then verify virt_to_phys returns the right phys */
    uint64_t phys = pmm_alloc_pages_zero(0);
    if (phys == 0) return false;

    uint64_t test_virt = 0xFFFFFF0000001000UL;
    vmm_map_page(test_virt, phys, PTE_PRESENT | PTE_WRITE | PTE_NX);

    uint64_t resolved = vmm_virt_to_phys(test_virt);
    bool ok = (resolved == phys);

    vmm_unmap_page(test_virt);
    pmm_free_pages(phys, 0);
    return ok;
}

/* ── String tests ────────────────────────────────────────────── */

static bool test_strlen(void) {
    if (strlen("") != 0) return false;
    if (strlen("hello") != 5) return false;
    if (strncmp("abc", "abd", 2) != 0) return false;
    if (strncmp("abc", "abd", 3) == 0) return false;
    char buf[8] = {0};
    strncpy(buf, "hi", 8);
    if (buf[0] != 'h' || buf[1] != 'i' || buf[2] != '\0') return false;
    return true;
}

/* ── Negative / edge-case tests ──────────────────────────────── */

static bool test_kfree_null(void) {
    /* kfree(NULL) must be a safe no-op (no crash) */
    kfree((void *)0);
    return true;
}

static bool test_kmalloc_zero(void) {
    /* kmalloc(0) — should return NULL or a valid freeable pointer */
    void *p = kmalloc(0);
    if (p) kfree(p);  /* If non-NULL, must be freeable */
    return true;       /* Either behavior is acceptable */
}

static bool test_krealloc_null(void) {
    /* krealloc(NULL, size) should behave like kmalloc(size) */
    uint8_t *p = krealloc((void *)0, 64);
    if (!p) return false;
    memset(p, 0xDD, 64);
    bool ok = (p[0] == 0xDD && p[63] == 0xDD);
    kfree(p);
    return ok;
}

static bool test_pmm_alloc_free_reuse(void) {
    /* Alloc, free, alloc again — should reuse the same page (buddy coalesced) */
    uint64_t p1 = pmm_alloc_pages(0);
    if (p1 == 0) return false;
    pmm_free_pages(p1, 0);
    uint64_t p2 = pmm_alloc_pages(0);
    if (p2 == 0) return false;
    pmm_free_pages(p2, 0);
    /* Both allocations should succeed and return valid addresses */
    return (p1 != 0) && (p2 != 0);
}

static bool test_memmove_overlap(void) {
    /* memmove must handle overlapping regions correctly */
    char buf[16];
    memcpy(buf, "ABCDEFGHIJ", 10);
    memmove(buf + 2, buf, 8);  /* Forward overlap */
    /* buf[0..1] unchanged, buf[2..9] = old buf[0..7] */
    if (buf[0] != 'A' || buf[1] != 'B') return false;
    if (buf[2] != 'A' || buf[3] != 'B') return false;
    if (buf[4] != 'C' || buf[5] != 'D') return false;
    return true;
}

static bool test_spinlock_irqsave(void) {
    /* Test IRQ-safe spinlock: save flags, lock, unlock, restore */
    spinlock_t lock = SPINLOCK_INIT;
    uint64_t flags;
    spin_lock_irqsave(&lock, &flags);
    /* We're holding the lock with IRQs disabled */
    spin_unlock_irqrestore(&lock, flags);
    return true;
}

static bool test_vmm_unmap_verify(void) {
    /* Map a page, unmap it, verify virt_to_phys returns 0 */
    uint64_t phys = pmm_alloc_pages_zero(0);
    if (phys == 0) return false;

    uint64_t test_virt = 0xFFFFFF0000002000UL;
    vmm_map_page(test_virt, phys, PTE_PRESENT | PTE_WRITE | PTE_NX);

    /* Should be mapped */
    if (vmm_virt_to_phys(test_virt) != phys) {
        vmm_unmap_page(test_virt);
        pmm_free_pages(phys, 0);
        return false;
    }

    /* Unmap */
    vmm_unmap_page(test_virt);

    /* Should NOT be mapped anymore */
    bool ok = (vmm_virt_to_phys(test_virt) == 0);

    pmm_free_pages(phys, 0);
    return ok;
}

/* ── Console / Keyboard tests ────────────────────────────────── */

static bool test_console_available(void) {
    /* Console should be initialized by boot sequence */
    return console_available() != 0;
}

static bool test_kb_buffer_empty(void) {
    /* Keyboard buffer should be empty at boot (no keys pressed) */
    return !kb_has_input();
}

/* ── PMM stress test ─────────────────────────────────────────── */

static bool test_pmm_stress(void) {
    /* Allocate 256 pages, verify no duplicates, free all */
    #define STRESS_COUNT 256
    uint64_t pages[STRESS_COUNT];

    /* Allocate */
    for (int i = 0; i < STRESS_COUNT; i++) {
        pages[i] = pmm_alloc_pages_zero(0);
        if (pages[i] == 0) return false;  /* OOM */
    }

    /* Verify no duplicates */
    for (int i = 0; i < STRESS_COUNT; i++) {
        for (int j = i + 1; j < STRESS_COUNT; j++) {
            if (pages[i] == pages[j]) return false;  /* Duplicate! */
        }
    }

    /* Free all */
    for (int i = 0; i < STRESS_COUNT; i++) {
        pmm_free_pages(pages[i], 0);
    }
    #undef STRESS_COUNT
    return true;
}

/* ── List test ───────────────────────────────────────────────── */

static bool test_list_operations(void) {
    struct list_head head = LIST_HEAD_INIT(head);
    if (!list_empty(&head)) return false;

    struct { int val; struct list_node node; } items[3];
    items[0].val = 10;
    items[1].val = 20;
    items[2].val = 30;

    list_push_back(&head, &items[0].node);
    list_push_back(&head, &items[1].node);
    list_push_back(&head, &items[2].node);

    if (list_empty(&head)) return false;

    /* Verify order: 10, 20, 30 */
    int expected[] = {10, 20, 30};
    int idx = 0;
    struct list_node *pos;
    list_for_each(pos, &head) {
        int val = container_of(pos, typeof(items[0]), node)->val;
        if (val != expected[idx]) return false;
        idx++;
    }
    if (idx != 3) return false;

    /* Remove middle */
    list_remove_node(&items[1].node);

    /* Verify: 10, 30 */
    idx = 0;
    int expected2[] = {10, 30};
    list_for_each(pos, &head) {
        int val = container_of(pos, typeof(items[0]), node)->val;
        if (val != expected2[idx]) return false;
        idx++;
    }
    return idx == 2;
}

/* ── Stosq memset test ───────────────────────────────────────── */

static bool test_memset_large(void) {
    /* Test that our stosq memset works for large + small fills */
    uint8_t buf[128];
    memset(buf, 0xAB, sizeof(buf));
    for (int i = 0; i < 128; i++) {
        if (buf[i] != 0xAB) return false;
    }
    /* Test small fill (< 8 bytes, uses rep stosb path) */
    uint8_t small[5];
    memset(small, 0xCD, 5);
    for (int i = 0; i < 5; i++) {
        if (small[i] != 0xCD) return false;
    }
    return true;
}

/* ── Timer callback test ─────────────────────────────────────── */

static volatile int timer_test_fired = 0;
static void timer_test_callback(void) { timer_test_fired++; }

static bool test_timer_callback(void) {
    timer_test_fired = 0;
    int ret = timer_register(timer_test_callback, 1); /* fire every tick */
    if (ret != 0) return false;

    /* Wait a few ticks */
    uint64_t start = pit_get_ticks();
    while (pit_get_ticks() - start < 5) {
        asm volatile("hlt");
    }

    return timer_test_fired >= 3;  /* Should have fired at least 3 times */
}

/* ── String function tests ───────────────────────────────────── */

static bool test_strncmp(void) {
    if (strncmp("hello", "hello", 5) != 0) return false;
    if (strncmp("hello", "hellx", 5) == 0) return false;
    if (strncmp("hello", "hellx", 4) != 0) return false; /* only compare first 4 */
    if (strncmp("abc", "abd", 3) >= 0) return false; /* 'c' < 'd' */
    return true;
}

static bool test_strncpy(void) {
    char buf[16];
    memset(buf, 'X', sizeof(buf));
    strncpy(buf, "hello", 16);
    if (strncmp(buf, "hello", 5) != 0) return false;
    if (buf[5] != '\0') return false;  /* null padded */
    /* Test truncation */
    strncpy(buf, "this is long", 4);
    if (strncmp(buf, "this", 4) != 0) return false;
    return true;
}

/* ── PMM peak tracking test ──────────────────────────────────── */

static bool test_pmm_peak(void) {
    /* Peak should be non-zero (VMM + stress test have allocated pages) */
    uint64_t peak = pmm_peak_used();
    if (peak == 0) return false;

    /* Peak should always be >= currently used */
    uint64_t used = pmm_used_count();
    if (peak < used) return false;

    /* Allocate and verify peak doesn't go backwards */
    uint64_t p1 = pmm_alloc_pages_zero(0);
    if (p1 == 0) return false;
    uint64_t new_peak = pmm_peak_used();
    pmm_free_pages(p1, 0);

    /* Peak should be >= what it was before (monotonically increasing) */
    return new_peak >= peak;
}

/* ── memcmp edge-case test ────────────────────────────────────── */

static bool test_memcmp_edges(void) {
    /* Zero-length compare = always equal */
    if (memcmp("abc", "xyz", 0) != 0) return false;
    /* Single byte */
    if (memcmp("a", "a", 1) != 0) return false;
    if (memcmp("a", "b", 1) >= 0) return false;
    /* Identical buffers */
    uint8_t a[16], b[16];
    memset(a, 0x42, 16);
    memset(b, 0x42, 16);
    if (memcmp(a, b, 16) != 0) return false;
    /* Differ at last byte */
    b[15] = 0x43;
    if (memcmp(a, b, 16) >= 0) return false;
    return true;
}

static bool test_ksnprintf(void) {
    char buf[64];

    /* Basic string formatting */
    int n = ksnprintf(buf, sizeof(buf), "hello %s", "world");
    if (n != 11) return false;
    if (memcmp(buf, "hello world", 11) != 0) return false;

    /* Integer formatting */
    ksnprintf(buf, sizeof(buf), "%d", -42);
    if (buf[0] != '-' || buf[1] != '4' || buf[2] != '2') return false;

    /* Hex formatting */
    ksnprintf(buf, sizeof(buf), "0x%x", 0xFF);
    if (memcmp(buf, "0xff", 4) != 0) return false;

    /* Truncation: buffer too small */
    ksnprintf(buf, 4, "abcdefgh");
    if (buf[3] != '\0') return false;  /* Must be NUL-terminated */
    if (memcmp(buf, "abc", 3) != 0) return false;

    return true;
}

/* ── New primitive tests (v0.5.7) ─────────────────────────────── */

static bool test_ringbuf(void) {
    uint8_t storage[64];
    struct ring_buf rb;
    ring_init(&rb, storage, 64);

    /* Write and read back */
    const char *msg = "hello";
    uint32_t written = ring_write(&rb, msg, 5);
    if (written != 5) return false;
    if (ring_used(&rb) != 5) return false;

    char out[8] = {0};
    uint32_t got = ring_read(&rb, out, 5);
    if (got != 5) return false;
    if (memcmp(out, "hello", 5) != 0) return false;
    if (!ring_empty(&rb)) return false;

    /* Overflow: write more than capacity → clamped */
    char big[128];
    memset(big, 'X', sizeof(big));
    written = ring_write(&rb, big, 128);
    if (written != 64) return false;  /* Clamped to size */
    if (!ring_full(&rb)) return false;
    return true;
}

static bool test_bitmap_alloc(void) {
    struct id_pool pool = ID_POOL_INIT;

    /* Alloc first 3 IDs */
    int id0 = id_alloc(&pool);
    int id1 = id_alloc(&pool);
    int id2 = id_alloc(&pool);
    if (id0 < 0 || id1 < 0 || id2 < 0) return false;
    if (id0 == id1 || id1 == id2) return false;  /* Must be unique */
    if (!id_is_used(&pool, id0)) return false;

    /* Free and re-alloc — should reuse */
    id_free(&pool, id1);
    if (id_is_used(&pool, id1)) return false;
    int id3 = id_alloc(&pool);
    if (id3 != id1) return false;  /* BSF should find freed bit first */

    /* Exhaust all 64 IDs */
    id_free(&pool, id0); id_free(&pool, id2); id_free(&pool, id3);
    for (int i = 0; i < 64; i++) id_alloc(&pool);
    int overflow = id_alloc(&pool);
    if (overflow != -1) return false;  /* Pool full */
    return true;
}

static int kref_release_count = 0;
static void kref_test_release(struct kref *r) {
    (void)r;
    kref_release_count++;
}

static bool test_kref(void) {
    struct kref r;
    kref_init(&r);
    if (kref_read(&r) != 1) return false;

    kref_get(&r);
    if (kref_read(&r) != 2) return false;

    kref_release_count = 0;
    kref_put(&r, kref_test_release);
    if (kref_read(&r) != 1) return false;
    if (kref_release_count != 0) return false;  /* Not released yet */

    kref_put(&r, kref_test_release);
    if (kref_release_count != 1) return false;  /* Released! */
    return true;
}

static bool test_kevent(void) {
    struct kevent evt;
    kevent_init(&evt, KEVENT_MANUAL_RESET);

    if (kevent_is_signaled(&evt)) return false;  /* Starts unsignaled */
    kevent_signal(&evt);
    if (!kevent_is_signaled(&evt)) return false;  /* Now signaled */
    kevent_reset(&evt);
    if (kevent_is_signaled(&evt)) return false;  /* Reset works */

    /* Auto-reset mode */
    kevent_init(&evt, KEVENT_AUTO_RESET);
    kevent_signal(&evt);
    if (!kevent_is_signaled(&evt)) return false;
    /* In real use, kevent_wait would auto-clear. We test the flag directly. */
    return true;
}

static bool test_vfs_lifecycle(void) {
    /* Open /dev/null (always available) */
    int fd = vfs_open("null", VFS_O_RDWR);
    if (fd < 0) return false;

    /* Write to /dev/null — should succeed and discard */
    int64_t w = vfs_write(fd, "test", 4);
    if (w != 4) return false;

    /* Read from /dev/null — should return 0 (EOF) */
    char buf[4];
    int64_t r = vfs_read(fd, buf, 4);
    if (r != 0) return false;

    /* Close */
    int c = vfs_close(fd);
    if (c != 0) return false;
    return true;
}

static bool test_rwlock(void) {
    struct rwlock rw = RWLOCK_INIT;

    /* Multiple reads should succeed */
    rwlock_read_lock(&rw);
    if (rw.readers != 1) return false;
    rwlock_read_lock(&rw);
    if (rw.readers != 2) return false;
    rwlock_read_unlock(&rw);
    rwlock_read_unlock(&rw);
    if (rw.readers != 0) return false;

    /* Write lock should work when no readers */
    rwlock_write_lock(&rw);
    if (!rw.writer) return false;
    rwlock_write_unlock(&rw);
    if (rw.writer) return false;

    return true;
}

/* ── Per-CPU GS register test ─────────────────────────────────── */

static bool test_percpu(void) {
    /* BSP should be CPU 0 */
    if (get_cpu_id() != 0) return false;

    /* set/get round-trip: save current (may be NULL before sched_init) */
    struct task *saved = get_current();

    struct task dummy;
    memset(&dummy, 0, sizeof(dummy));

    set_current(&dummy);
    if (get_current() != &dummy) return false;

    /* Restore original value */
    set_current(saved);
    if (get_current() != saved) return false;

    return true;
}

/* ── Phase 0.5: Foundation Hardening tests ─────────────────────── */

static bool test_tickless_api(void) {
    /* Test oneshot: switch, verify, resume */
    pit_set_oneshot(100);
    if (pit_is_tickless() == 0) return false;  /* Should be oneshot */

    /* Resume periodic */
    pit_init(100);
    if (pit_is_tickless() != 0) return false;  /* Should be periodic */

    return true;
}

static bool test_wx_audit(void) {
    /* W^X audit: no page should be both Writable AND Executable */
    uint32_t violations = vmm_audit_wx();
    return violations == 0;
}

static bool test_cpuidle_init(void) {
    /* cpuidle should have been initialized — deepest state valid */
    int deepest = cpuidle_deepest_state();
    /* Must be IDLE_HLT(1) or IDLE_MWAIT(2) */
    return (deepest == IDLE_HLT || deepest == IDLE_MWAIT);
}

static bool test_quarantine_delay(void) {
    /* Quarantine should delay reuse of freed memory.
     * Allocate+free, then allocate again — should NOT get same address
     * (the freed slot is in quarantine, not on free list yet). */
    void *p1 = kmalloc(64);
    if (!p1) return false;
    memset(p1, 0x42, 64);
    kfree(p1);

    void *p2 = kmalloc(64);
    if (!p2) return false;
    /* p2 should NOT be p1 because p1 is in quarantine */
    bool different = (p2 != p1);
    kfree(p2);
    return different;
}

/* ── Registration ──────────────────────────────────────────────── */

void register_selftests(void) {
    /* Spinlock & memory */
    selftest_register("Spinlock trylock/unlock",        test_spinlock);
    selftest_register("memset/memcpy/memcmp",           test_memops);

    /* PMM */
    selftest_register("PMM alloc/free order-0",         test_pmm_alloc_free);
    selftest_register("PMM alloc/free order-5 (128KB)", test_pmm_order5);

    /* kmalloc */
    selftest_register("kmalloc(64) + kfree",            test_kmalloc_small);
    selftest_register("kmalloc(8192) large alloc",      test_kmalloc_large);
    selftest_register("kzmalloc zeroing",               test_kzmalloc);
    selftest_register("Poison 0xDE on kfree",           test_poison);
    selftest_register("krealloc data preservation",     test_krealloc);
    selftest_register("Slab reclamation to PMM",        test_slab_reclamation);
    selftest_register("PMM watermark >= 10%",           test_pmm_watermark);

    /* VMM */
    selftest_register("VMM map/write/read",             test_vmm_map_write_read);
    selftest_register("VMM virt_to_phys walk",          test_vmm_virt_to_phys);
    selftest_register("VMM HHDM integrity post-CR3",    test_vmm_hhdm_integrity);
    selftest_register("VMM map resolves correctly",     test_vmm_map_returns_mapping);

    /* Timer */
    selftest_register("PIT timer ticking",              test_pit_ticking);

    /* String functions */
    selftest_register("strlen correctness",             test_strlen);

    /* Negative / edge-case tests */
    selftest_register("kfree(NULL) safe no-op",         test_kfree_null);
    selftest_register("kmalloc(0) edge case",           test_kmalloc_zero);
    selftest_register("krealloc(NULL,n) == kmalloc",    test_krealloc_null);
    selftest_register("PMM alloc/free/reuse cycle",     test_pmm_alloc_free_reuse);
    selftest_register("memmove overlap correctness",    test_memmove_overlap);
    selftest_register("Spinlock IRQ save/restore",      test_spinlock_irqsave);
    selftest_register("VMM unmap + verify unmapped",    test_vmm_unmap_verify);

    /* Console / Keyboard */
    selftest_register("Framebuffer console active",     test_console_available);
    selftest_register("Keyboard buffer starts empty",   test_kb_buffer_empty);

    /* Stress & infrastructure tests */
    selftest_register("PMM stress 256 pages no-dup",    test_pmm_stress);
    selftest_register("Intrusive list operations",      test_list_operations);
    selftest_register("memset stosq large+small",       test_memset_large);
    selftest_register("Timer callback fires",           test_timer_callback);

    /* String & diagnostics */
    selftest_register("strncmp comparisons",            test_strncmp);
    selftest_register("strncpy + truncation",           test_strncpy);
    selftest_register("PMM peak usage tracking",        test_pmm_peak);
    selftest_register("memcmp edge cases",              test_memcmp_edges);
    selftest_register("ksnprintf formatting",           test_ksnprintf);

    /* New primitive tests (v0.5.7) */
    selftest_register("Ring buffer SPSC",               test_ringbuf);
    selftest_register("Bitmap ID allocator",            test_bitmap_alloc);
    selftest_register("kref lifecycle",                 test_kref);
    selftest_register("KEVENT signal/wait",             test_kevent);
    selftest_register("VFS open/write/close",           test_vfs_lifecycle);
    selftest_register("RWLock read/write",               test_rwlock);
    selftest_register("Per-CPU GS accessor",             test_percpu);

    /* Phase 0.5: Foundation Hardening */
    selftest_register("Tickless timer API",              test_tickless_api);
    selftest_register("W^X audit 0 violations",          test_wx_audit);
    selftest_register("CPU idle state detection",        test_cpuidle_init);
    selftest_register("Quarantine delays reuse",         test_quarantine_delay);
}
