/*
 * Anykernel OS — CPUID Detection
 *
 * Queries x86 CPUID to identify CPU vendor, brand, and features.
 * Reports NX, SSE, AVX, RDRAND support.
 */

#ifndef CPUID_H
#define CPUID_H

/* Detect and log CPU information */
void cpuid_detect(void);

#endif /* CPUID_H */
