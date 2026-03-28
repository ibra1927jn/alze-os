/*
 * Anykernel OS v2.1 — Kernel Panic & Assertions
 *
 * "Fail Fast, Fail Loud" — any unrecoverable error freezes the CPU,
 * preserving the crime scene for serial log analysis.
 */

#ifndef PANIC_H
#define PANIC_H

/* Halt the system with a diagnostic message, file, and line number.
 * Disables interrupts and enters an infinite halt loop. */
__attribute__((noreturn))
void kernel_panic(const char *msg, const char *file, int line);

/* Convenience macros */
#define PANIC(msg)     kernel_panic((msg), __FILE__, __LINE__)

#define KASSERT(cond)                                               \
    do {                                                            \
        if (__builtin_expect(!(cond), 0)) {                         \
            PANIC("Assertion failed: " #cond);                      \
        }                                                           \
    } while (0)

#endif /* PANIC_H */
