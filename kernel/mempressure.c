/*
 * Anykernel OS — Memory Pressure Implementation
 */

#include "mempressure.h"
#include "pmm.h"
#include "log.h"
#include "errno.h"

/* ── State ───────────────────────────────────────────────────── */

#define MAX_PRESSURE_CALLBACKS 8

static mem_pressure_fn callbacks[MAX_PRESSURE_CALLBACKS];
static uint32_t callback_count = 0;
static enum mem_pressure_level last_level = MEM_PRESSURE_NORMAL;

/* ── Init ────────────────────────────────────────────────────── */

void mempressure_init(void) {
    callback_count = 0;
    last_level = MEM_PRESSURE_NORMAL;
    LOG_OK("Memory pressure monitor initialized");
}

/* ── Register ────────────────────────────────────────────────── */

int mempressure_register(mem_pressure_fn fn) {
    if (callback_count >= MAX_PRESSURE_CALLBACKS) return -ENOSPC;
    callbacks[callback_count++] = fn;
    return (int)(callback_count - 1);
}

/* ── Name ────────────────────────────────────────────────────── */

const char *mempressure_name(enum mem_pressure_level level) {
    switch (level) {
        case MEM_PRESSURE_NORMAL:   return "NORMAL";
        case MEM_PRESSURE_WARNING:  return "WARNING";
        case MEM_PRESSURE_CRITICAL: return "CRITICAL";
        default: return "???";
    }
}

/* ── Check ───────────────────────────────────────────────────── */

enum mem_pressure_level mempressure_check(void) {
    uint64_t free_pg = pmm_free_count();
    uint64_t used_pg = pmm_used_count();
    uint64_t total = free_pg + used_pg;

    if (total == 0) return MEM_PRESSURE_NORMAL;

    uint64_t free_pct = (free_pg * 100) / total;

    enum mem_pressure_level level;
    if (free_pct < MEM_THRESHOLD_CRITICAL) {
        level = MEM_PRESSURE_CRITICAL;
    } else if (free_pct < MEM_THRESHOLD_WARNING) {
        level = MEM_PRESSURE_WARNING;
    } else {
        level = MEM_PRESSURE_NORMAL;
    }

    /* Only notify on level transitions */
    if (level != last_level) {
        if (level > MEM_PRESSURE_NORMAL) {
            LOG_WARN("Memory pressure: %s (free: %lu%%)",
                     mempressure_name(level), free_pct);
        } else {
            LOG_INFO("Memory pressure: %s (free: %lu%%)",
                     mempressure_name(level), free_pct);
        }

        for (uint32_t i = 0; i < callback_count; i++) {
            callbacks[i](level);
        }
        last_level = level;
    }

    return level;
}
