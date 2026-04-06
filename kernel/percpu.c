/*
 * Anykernel OS — Per-CPU Bootstrap
 *
 * Sets up the GS segment register for BSP (Bootstrap Processor).
 * Future APs will call a similar function during AP startup.
 */

#include "percpu.h"
#include "kmalloc.h"
#include "log.h"
#include "string.h"

/* BSP's per-CPU data (statically allocated for early boot) */
static struct cpu_local bsp_cpu_local __attribute__((aligned(64)));

void percpu_init_bsp(void) {
    memset(&bsp_cpu_local, 0, sizeof(bsp_cpu_local));

    bsp_cpu_local.self = &bsp_cpu_local;
    bsp_cpu_local.cpu_id = 0;       /* BSP is always CPU 0 */
    bsp_cpu_local.current = 0;      /* Set later by sched_init */
    bsp_cpu_local.idle = 0;         /* Set later by sched_init */
    bsp_cpu_local.need_resched = 0;
    bsp_cpu_local.ticks = 0;

    /* Point GS segment to our per-CPU data */
    wrmsr(MSR_GS_BASE, (uint64_t)&bsp_cpu_local);

    LOG_OK("Per-CPU data initialized for BSP (GS:0x%lx)",
           (uint64_t)&bsp_cpu_local);
}
