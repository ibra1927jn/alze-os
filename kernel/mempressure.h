/*
 * Anykernel OS — Memory Pressure Notification System
 *
 * Monitors physical memory usage and notifies registered callbacks
 * when memory drops below thresholds. Like macOS Jetsam / Linux
 * memory cgroups pressure notifications.
 *
 * Three pressure levels:
 *   NORMAL   — Plenty of memory (> 75% free)
 *   WARNING  — Getting low (25-75% free)
 *   CRITICAL — Almost out (< 25% free)
 *
 * Callbacks can free caches, flush buffers, or kill background tasks.
 */

#ifndef MEMPRESSURE_H
#define MEMPRESSURE_H

#include <stdint.h>

/* ── Pressure levels ─────────────────────────────────────────── */

enum mem_pressure_level {
    MEM_PRESSURE_NORMAL   = 0,
    MEM_PRESSURE_WARNING  = 1,
    MEM_PRESSURE_CRITICAL = 2,
};

/* Threshold percentages (of total pages) */
#define MEM_THRESHOLD_WARNING  75  /* Below 75% free = warning */
#define MEM_THRESHOLD_CRITICAL 25  /* Below 25% free = critical */

/* ── Callback type ───────────────────────────────────────────── */

typedef void (*mem_pressure_fn)(enum mem_pressure_level level);

/* ── API ─────────────────────────────────────────────────────── */

/* Initialize memory pressure monitoring */
void mempressure_init(void);

/* Register a callback (returns slot index or -1) */
int mempressure_register(mem_pressure_fn fn);

/* Check current pressure level (call periodically from idle task) */
enum mem_pressure_level mempressure_check(void);

/* Get human-readable name */
const char *mempressure_name(enum mem_pressure_level level);

#endif /* MEMPRESSURE_H */
