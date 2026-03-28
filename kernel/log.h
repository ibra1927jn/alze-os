/*
 * Anykernel OS v2.1 — Kernel Logging with ANSI Colors
 *
 * Colored log levels for serial output. QEMU's -serial stdio
 * renders ANSI escape codes natively, giving instant visual triage:
 *
 *   LOG_INFO  → cyan      — normal boot/init messages
 *   LOG_DEBUG → magenta   — verbose debugging (disable later)
 *   LOG_WARN  → yellow    — non-fatal warnings
 *   LOG_ERROR → red       — errors that may cause problems
 *
 * Usage:
 *   LOG_INFO("PMM initialized: %lu pages", total_pages);
 *   LOG_DEBUG("Buddy merge: order %d at PFN %lu", order, pfn);
 *   LOG_ERROR("Out of memory!");
 */

#ifndef LOG_H
#define LOG_H

#include "kprintf.h"

/* ANSI color codes */
#define ANSI_RESET   "\033[0m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"

/* Log level macros — each appends \n automatically */
#define LOG_INFO(fmt, ...)  \
    kprintf(ANSI_CYAN    "[INFO]"  ANSI_RESET "  " fmt "\n", ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    kprintf(ANSI_MAGENTA "[DEBUG]" ANSI_RESET " " fmt "\n", ##__VA_ARGS__)

#define LOG_WARN(fmt, ...)  \
    kprintf(ANSI_YELLOW  "[WARN]"  ANSI_RESET "  " fmt "\n", ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    kprintf(ANSI_RED     "[ERROR]" ANSI_RESET " " fmt "\n", ##__VA_ARGS__)

#define LOG_OK(fmt, ...)    \
    kprintf(ANSI_GREEN   "[OK]"    ANSI_RESET "    " fmt "\n", ##__VA_ARGS__)

#define LOG_FAIL(fmt, ...)  \
    kprintf(ANSI_RED     "[FAIL]"  ANSI_RESET "  " fmt "\n", ##__VA_ARGS__)

#endif /* LOG_H */
