/*
 * Anykernel OS v2.1 — PMM Userspace Test Harness
 *
 * Compiles with native gcc/clang on the host OS.
 * Simulates 128MB of RAM and stress-tests the buddy allocator.
 *
 * Build:  gcc -DPMM_USERSPACE_TEST -O2 -o test_pmm tests/test_pmm.c kernel/pmm.c
 * Run:    ./test_pmm
 */

#ifndef PMM_USERSPACE_TEST
#define PMM_USERSPACE_TEST
#endif

#include "../kernel/pmm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define TEST_RAM_MB     128
#define TEST_RAM_SIZE   ((uint64_t)TEST_RAM_MB * 1024 * 1024)
#define TEST_BASE_PHYS  0x100000UL   /* Simulate starting at 1MB */
#define TEST_TOTAL_PFNS (TEST_RAM_SIZE / 4096)

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("\n=== TEST: %s ===\n", name)
#define CHECK(cond, msg) do { \
    if (cond) { tests_passed++; printf("  [PASS] %s\n", msg); } \
    else { tests_failed++; printf("  [FAIL] %s\n", msg); } \
} while(0)

/* ── Test 1: Basic alloc and free ─────────────────────────────── */

static void test_basic(void) {
    TEST("Basic alloc/free");

    uint64_t initial_free = pmm_free_count();
    CHECK(initial_free > 0, "Has free pages after init");

    /* Allocate a single page */
    uint64_t p1 = pmm_alloc_pages(0);
    CHECK(p1 != 0, "Order-0 alloc succeeds");
    CHECK(pmm_free_count() == initial_free - 1, "Free count decreased by 1");

    /* Free it */
    pmm_free_pages(p1, 0);
    CHECK(pmm_free_count() == initial_free, "Free count restored after free");
}

/* ── Test 2: Alignment ────────────────────────────────────────── */

static void test_alignment(void) {
    TEST("Block alignment");

    for (uint32_t order = 0; order <= 6; order++) {
        uint64_t addr = pmm_alloc_pages(order);
        CHECK(addr != 0, "Alloc order succeeds");
        uint64_t expected_align = (1UL << order) * 4096;
        CHECK((addr % expected_align) == 0, "Block is naturally aligned");
        pmm_free_pages(addr, order);
    }
}

/* ── Test 3: Exhaustion ───────────────────────────────────────── */

static void test_exhaustion(void) {
    TEST("Memory exhaustion");

    /* Allocate all memory in order-0 pages */
    uint64_t count = 0;
    uint64_t *addrs = malloc(sizeof(uint64_t) * TEST_TOTAL_PFNS);
    assert(addrs != NULL);

    while (1) {
        uint64_t addr = pmm_alloc_pages(0);
        if (addr == 0) break;
        addrs[count++] = addr;
    }

    CHECK(count > 0, "Allocated some pages before exhaustion");
    CHECK(pmm_free_count() == 0, "Free count is 0 after exhaustion");
    printf("  Allocated %lu pages before exhaustion\n", (unsigned long)count);

    /* Verify OOM returns 0 */
    uint64_t oom = pmm_alloc_pages(0);
    CHECK(oom == 0, "OOM returns 0");

    /* Free everything */
    for (uint64_t i = 0; i < count; i++) {
        pmm_free_pages(addrs[i], 0);
    }

    CHECK(pmm_free_count() > 0, "Free pages restored after freeing all");
    free(addrs);
}

/* ── Test 4: Buddy coalescing ─────────────────────────────────── */

static void test_coalescing(void) {
    TEST("Buddy coalescing");

    uint64_t initial_free = pmm_free_count();

    /* Allocate 2 adjacent order-0 pages, free them → should coalesce to order-1 */
    uint64_t a = pmm_alloc_pages(0);
    uint64_t b = pmm_alloc_pages(0);
    CHECK(a != 0 && b != 0, "Two order-0 allocs succeed");

    pmm_free_pages(a, 0);
    pmm_free_pages(b, 0);
    CHECK(pmm_free_count() == initial_free, "Full coalescing after freeing both");

    /* Now allocate order-1 — should succeed if coalescing worked */
    uint64_t c = pmm_alloc_pages(1);
    CHECK(c != 0, "Order-1 alloc after coalesce succeeds");
    pmm_free_pages(c, 1);
}

/* ── Test 5: Large block splitting ────────────────────────────── */

static void test_splitting(void) {
    TEST("Block splitting");

    /* Allocate a large block, then many small ones from the split halves */
    uint64_t big = pmm_alloc_pages(5);  /* 128KB */
    CHECK(big != 0, "Order-5 alloc succeeds");

    /* Verify alignment (2^5 * 4KB = 128KB aligned) */
    CHECK((big % (32 * 4096)) == 0, "Order-5 block is 128KB aligned");

    pmm_free_pages(big, 5);

    /* Now allocate 32 order-0 pages (should come from the split) */
    uint64_t addrs[32];
    int all_ok = 1;
    for (int i = 0; i < 32; i++) {
        addrs[i] = pmm_alloc_pages(0);
        if (addrs[i] == 0) all_ok = 0;
    }
    CHECK(all_ok, "32 order-0 allocs after order-5 free");

    for (int i = 0; i < 32; i++) {
        pmm_free_pages(addrs[i], 0);
    }
}

/* ── Test 6: Stress test ──────────────────────────────────────── */

static void test_stress(void) {
    TEST("Stress test (100K alloc/free cycles)");

    uint64_t initial_free = pmm_free_count();
    srand((unsigned)time(NULL));

    #define STRESS_POOL 1000
    uint64_t pool_addrs[STRESS_POOL];
    uint32_t pool_orders[STRESS_POOL];
    int pool_used = 0;

    int allocs = 0, frees = 0;

    for (int i = 0; i < 100000; i++) {
        if (pool_used > 0 && (rand() % 3 == 0 || pool_used >= STRESS_POOL)) {
            /* Free a random page from the pool */
            int idx = rand() % pool_used;
            pmm_free_pages(pool_addrs[idx], pool_orders[idx]);
            pool_addrs[idx] = pool_addrs[pool_used - 1];
            pool_orders[idx] = pool_orders[pool_used - 1];
            pool_used--;
            frees++;
        } else {
            /* Allocate a random-sized block */
            uint32_t order = rand() % 5;  /* order 0-4 */
            uint64_t addr = pmm_alloc_pages(order);
            if (addr != 0) {
                pool_addrs[pool_used] = addr;
                pool_orders[pool_used] = order;
                pool_used++;
                allocs++;
            }
        }
    }

    /* Free remaining */
    for (int i = 0; i < pool_used; i++) {
        pmm_free_pages(pool_addrs[i], pool_orders[i]);
    }

    CHECK(pmm_free_count() == initial_free,
          "Free count matches after stress test");
    printf("  %d allocs, %d frees completed\n", allocs, frees);
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(void) {
    printf("=== PMM Buddy Allocator — Userspace Test Suite ===\n");
    printf("Simulating %dMB RAM at phys 0x%lx\n\n",
           TEST_RAM_MB, (unsigned long)TEST_BASE_PHYS);

    /* Allocate the page array on the host */
    struct page *pages = calloc(TEST_TOTAL_PFNS, sizeof(struct page));
    assert(pages != NULL);

    printf("Page array: %lu entries × %lu bytes = %lu KB\n",
           (unsigned long)TEST_TOTAL_PFNS,
           (unsigned long)sizeof(struct page),
           (unsigned long)(TEST_TOTAL_PFNS * sizeof(struct page) / 1024));

    /* Initialize PMM in test mode */
    pmm_init_test(TEST_BASE_PHYS, TEST_RAM_SIZE, pages, TEST_TOTAL_PFNS);
    pmm_dump_stats();

    /* Run tests */
    test_basic();
    test_alignment();
    test_coalescing();
    test_splitting();
    test_exhaustion();
    test_stress();

    /* Final stats */
    printf("\n=== Final Stats ===\n");
    pmm_dump_stats();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    free(pages);
    return tests_failed > 0 ? 1 : 0;
}
