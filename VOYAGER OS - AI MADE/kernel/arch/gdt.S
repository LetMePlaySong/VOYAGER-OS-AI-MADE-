/* GDT for 64-bit long mode. No user mode needed - everything runs ring0. */
    .global gdt_flush
    .global gdt_ptr
    .section .text

/* Segment selectors */
.set SEL_KCODE,     0x08
.set SEL_KDATA,     0x10

gdt_ptr:
    .word gdt_end - gdt_start - 1
    .quad gdt_start

gdt_start:
    .quad 0x0000000000000000          /* 0x00 null */
    .quad 0x00af9a000000ffff          /* 0x08 kernel code64 */
    .quad 0x00cf92000000ffff          /* 0x10 kernel data */
    .quad 0x00cf98000000ffff          /* 0x18 user code32 */
    .quad 0x00cff2000000ffff          /* 0x20 user data */
    .quad 0x00affa000000ffff          /* 0x28 user code64 */
    .quad 0x0000000000000000          /* 0x30 TSS low (unused) */
    .quad 0x0000000000000000          /* 0x38 TSS high (unused) */
gdt_end:

/* Flush GDT and reload segments */
gdt_flush:
    /* Keep Limine's GDT - it already has valid ring0 code/data/TSS.
     * Loading our own GDT here breaks CS on the next iretq because the
     * running Limine selectors index into the wrong descriptors.
     * Our IDT uses Limine's code64 selector 0x18 instead. */
    ret
