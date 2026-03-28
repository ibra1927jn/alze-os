/* Linker script for Anykernel OS v2.1 — x86_64 Higher Half */

ENTRY(_start)

PHDRS {
    requests PT_LOAD FLAGS((1 << 0) | (1 << 1) | (1 << 2)); /* RWX */
    text     PT_LOAD FLAGS((1 << 0) | (1 << 2));             /* R-X */
    rodata   PT_LOAD FLAGS((1 << 0));                        /* R-- */
    data     PT_LOAD FLAGS((1 << 0) | (1 << 1));             /* RW- */
}

SECTIONS {
    /* Higher Half base address */
    . = 0xffffffff80000000;

    __kernel_start = .;

    /* Limine request markers — must be in their own section */
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :requests

    . = ALIGN(4096);
    __text_start = .;
    .text : {
        *(.text .text.*)
    } :text
    __text_end = .;

    . = ALIGN(4096);
    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    __rodata_end = .;

    . = ALIGN(4096);
    __data_start = .;
    .data : {
        *(.data .data.*)
    } :data
    __data_end = .;

    . = ALIGN(4096);
    __bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    __bss_end = .;

    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
