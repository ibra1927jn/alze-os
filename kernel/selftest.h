/*
 * Anykernel OS — Kernel Self-Test Framework
 *
 * Clean separation of init from tests. Each subsystem registers
 * tests as function pointers. run_all_selftests() executes them
 * and reports pass/fail count.
 */

#ifndef SELFTEST_H
#define SELFTEST_H

#include <stdbool.h>

/* Max number of self-tests that can be registered */
#define SELFTEST_MAX 64

/* Test function signature: returns true on pass */
typedef bool (*selftest_fn)(void);

typedef struct {
    const char  *name;
    selftest_fn  func;
} selftest_entry;

/*
 * Register a test. Call during init, before run_all_selftests().
 */
void selftest_register(const char *name, selftest_fn func);

/*
 * Run all registered tests. Returns number of failures (0 = all pass).
 */
int run_all_selftests(void);

/*
 * Return the number of registered tests.
 */
int selftest_count(void);

#endif /* SELFTEST_H */
