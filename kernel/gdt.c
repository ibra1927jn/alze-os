/*
 * Anykernel OS v2.1 — GDT & TSS Implementation
 *
 * GDT layout (7 entries, entry 5-6 is the 16-byte TSS descriptor):
 *   [0] 0x00  Null
 *   [1] 0x08  Kernel Code 64  (DPL=0, L=1, D=0)
 *   [2] 0x10  Kernel Data 64  (DPL=0)
 *   [3] 0x18  User Code 64    (DPL=3, L=1, D=0)
 *   [4] 0x20  User Data 64    (DPL=3)
 *   [5] 0x28  TSS Low         (16-byte system descriptor, low 8 bytes)
 *   [6] 0x30  TSS High        (16-byte system descriptor, high 8 bytes)
 *
 * In x86_64 Long Mode, the GDT is simplified:
 *   - Code segments only need L=1, D=0 (base/limit ignored by CPU)
 *   - Data segments only need Present + DPL (base/limit ignored)
 *   - TSS descriptor is 16 bytes (occupies two GDT slots)
 */

#include "gdt.h"

/* ── GDT entry structure ──────────────────────────────────────── */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;   /* flags (4 bits) + limit_high (4 bits) */
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* ── TSS instance ─────────────────────────────────────────────── */

/*
 * Temporary stacks for TSS init — VMM replaces both RSP0 and IST1
 * with larger, guard-page-protected stacks during vmm_init().
 * These must be large enough for early exception handling.
 */
#define TEMP_RSP0_STACK_SIZE  4096
#define TEMP_IST1_STACK_SIZE  2048
static uint8_t temp_rsp0_stack[TEMP_RSP0_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t temp_ist1_stack[TEMP_IST1_STACK_SIZE] __attribute__((aligned(16)));

static struct tss tss_instance;

/* ── GDT table ────────────────────────────────────────────────── */

/*
 * Access byte layout:
 *   Bit 7    : Present (P)
 *   Bits 5-6 : DPL (Descriptor Privilege Level)
 *   Bit 4    : S (1 = code/data, 0 = system segment like TSS)
 *   Bit 3    : E (1 = code, 0 = data)
 *   Bit 2    : DC (direction/conforming)
 *   Bit 1    : RW (readable code / writable data)
 *   Bit 0    : Accessed
 *
 * Granularity byte:
 *   Bit 7    : G (granularity: 1 = 4KB pages)
 *   Bit 6    : D/B (0 for 64-bit code)
 *   Bit 5    : L (1 = 64-bit code segment)
 *   Bit 4    : reserved
 *   Bits 0-3 : limit_high
 */

/* GDT access byte values (see bit layout in comment above) */
#define GDT_ACCESS_KCODE  0x9A  /* Present, DPL=0, Code, Readable        */
#define GDT_ACCESS_KDATA  0x92  /* Present, DPL=0, Data, Writable        */
#define GDT_ACCESS_UCODE  0xFA  /* Present, DPL=3, Code, Readable        */
#define GDT_ACCESS_UDATA  0xF2  /* Present, DPL=3, Data, Writable        */
#define GDT_ACCESS_TSS64  0x89  /* Present, DPL=0, type=0x9 (64-bit TSS) */

/* GDT flags (upper nibble of granularity byte) */
#define GDT_FLAG_LONG_MODE  0x20  /* L=1: 64-bit code segment */

static struct gdt_entry gdt[7];
static struct gdt_pointer gdtr;

/* Helper: encode a standard GDT entry */
static void gdt_set_entry(int idx, uint8_t access, uint8_t flags) {
    gdt[idx].limit_low   = 0xFFFF;
    gdt[idx].base_low    = 0;
    gdt[idx].base_mid    = 0;
    gdt[idx].access      = access;
    gdt[idx].granularity  = (flags & 0xF0) | 0x0F;  /* flags | limit_high */
    gdt[idx].base_high   = 0;
}

/* Helper: encode the 16-byte TSS descriptor across entries [idx] and [idx+1] */
static void gdt_set_tss(int idx, struct tss *tss_ptr) {
    uint64_t base = (uint64_t)tss_ptr;
    uint32_t limit = sizeof(struct tss) - 1;

    /* Low 8 bytes (entry idx) */
    gdt[idx].limit_low   = limit & 0xFFFF;
    gdt[idx].base_low    = base & 0xFFFF;
    gdt[idx].base_mid    = (base >> 16) & 0xFF;
    gdt[idx].access      = GDT_ACCESS_TSS64;
    gdt[idx].granularity  = ((limit >> 16) & 0x0F);  /* No flags for TSS */
    gdt[idx].base_high   = (base >> 24) & 0xFF;

    /* High 8 bytes (entry idx+1) — base bits 63:32 + reserved */
    uint32_t *high = (uint32_t *)&gdt[idx + 1];
    high[0] = (base >> 32) & 0xFFFFFFFF;
    high[1] = 0;
}

/* ── Initialization ───────────────────────────────────────────── */

void gdt_init(void) {
    /* Initialize TSS to zero */
    for (int i = 0; i < (int)sizeof(tss_instance); i++) {
        ((uint8_t *)&tss_instance)[i] = 0;
    }

    /* RSP0: top of kernel stack (stack grows down) */
    tss_instance.rsp0 = (uint64_t)(temp_rsp0_stack + sizeof(temp_rsp0_stack));

    /* IST1: separate stack for #DF (Double Fault) */
    tss_instance.ist[0] = (uint64_t)(temp_ist1_stack + sizeof(temp_ist1_stack));

    /* IOPB offset points past the TSS (no I/O bitmap) */
    tss_instance.iopb_offset = sizeof(struct tss);

    /* Build the GDT */
    /* [0] Null descriptor — all zeros */
    gdt[0].limit_low   = 0;
    gdt[0].base_low    = 0;
    gdt[0].base_mid    = 0;
    gdt[0].access      = 0;
    gdt[0].granularity = 0;
    gdt[0].base_high   = 0;

    /* [1] Kernel Code 64 */
    gdt_set_entry(1, GDT_ACCESS_KCODE, GDT_FLAG_LONG_MODE);

    /* [2] Kernel Data 64 */
    gdt_set_entry(2, GDT_ACCESS_KDATA, 0x00);

    /* [3] User Code 64 */
    gdt_set_entry(3, GDT_ACCESS_UCODE, GDT_FLAG_LONG_MODE);

    /* [4] User Data 64 */
    gdt_set_entry(4, GDT_ACCESS_UDATA, 0x00);

    /* [5-6] TSS descriptor (16 bytes) */
    gdt_set_tss(5, &tss_instance);

    /* Load the GDT */
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (uint64_t)&gdt;

    asm volatile("lgdt %0" : : "m"(gdtr));

    /*
     * Reload CS via far return, then reload data segments.
     * The selector must be passed as a 64-bit register operand because
     * pushq requires a 64-bit source in Long Mode. Using an immediate
     * ($0x08) or a 16-bit register causes clang codegen issues.
     * FS/GS are zeroed — they're reserved for TLS and per-CPU data.
     */
    uint64_t cs = GDT_KERNEL_CODE;
    uint16_t ds = GDT_KERNEL_DATA;
    asm volatile(
        "pushq %0\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw %w1, %%ds\n\t"
        "movw %w1, %%es\n\t"
        "movw %w1, %%ss\n\t"
        "xorw %%ax, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : "r"(cs), "r"(ds)
        : "rax", "memory"
    );

    /* Load the TSS selector */
    uint16_t tss_sel = GDT_TSS;
    asm volatile("ltr %w0" : : "r"(tss_sel));
}

void tss_set_rsp0(uint64_t rsp0) {
    tss_instance.rsp0 = rsp0;
}

void tss_set_ist1(uint64_t ist1) {
    tss_instance.ist[0] = ist1;  /* IST entries are 0-indexed: ist[0] = IST1 */
}
