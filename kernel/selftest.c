/*
 * Anykernel OS — Kernel Self-Test Framework Implementation
 */

#include "selftest.h"
#include "log.h"
#include "kprintf.h"

static selftest_entry tests[SELFTEST_MAX];
static int test_count = 0;

void selftest_register(const char *name, selftest_fn func) {
    if (test_count >= SELFTEST_MAX) return;
    tests[test_count].name = name;
    tests[test_count].func = func;
    test_count++;
}

int run_all_selftests(void) {
    kprintf("\n=== Running %d kernel self-tests ===\n", test_count);

    int passed = 0, failed = 0;

    for (int i = 0; i < test_count; i++) {
        bool result = tests[i].func();
        if (result) {
            LOG_OK("[%d/%d] %s", i + 1, test_count, tests[i].name);
            passed++;
        } else {
            LOG_FAIL("[%d/%d] %s", i + 1, test_count, tests[i].name);
            failed++;
        }
    }

    kprintf("=== Results: %d passed, %d failed ===\n\n", passed, failed);
    return failed;
}

int selftest_count(void) {
    return test_count;
}
