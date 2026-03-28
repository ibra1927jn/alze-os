/*
 * Anykernel OS — Software Watchdog
 *
 * Detects runaway or hung tasks by monitoring how many consecutive
 * PIT ticks a task runs without yielding or being rescheduled.
 *
 * If a task exceeds WATCHDOG_THRESHOLD ticks (default: 500 = 5 seconds),
 * a warning is logged. In production, this could kill the task.
 *
 * Inspired by: Linux softlockup detector, macOS watchdog timer.
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>

/* Threshold: ticks without yielding before warning (5 seconds at 100 Hz) */
#define WATCHDOG_THRESHOLD 500

/* Check the current task's watchdog counter */
void watchdog_check(void);

/* Initialize the watchdog subsystem */
void watchdog_init(void);

#endif /* WATCHDOG_H */
