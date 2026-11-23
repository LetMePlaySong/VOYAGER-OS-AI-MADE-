/* Linker script for the Mac OS High Sierra clone kernel.
 * Kernel is loaded by Limine at 0xffffffff80000000 (higher half).
 * Layout follows the official limine-c-template: Limine requests get their
 * own first PT_LOAD so no headers/PHDRs sit below the kernel base.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    limine_requests PT_LOAD FLAGS(0x06); /* R+W */
    text            PT_LOAD FLAGS(0x05); /* R+X */
    rodata          PT_LOAD FLAGS(0x04); /* R   */
    data            PT_LOAD FLAGS(0x06); /* R+W */
}

SECTIONS
{
    . = 0xffffffff80000000;
    executable_start = .;

    /* Limine requests first, in their own loadable segment */
    .limine_requests : {
        KEEP(*(.limine_requests_start_marker))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end_marker))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        *(.text .text.*)
        *(.text.unlikely .text.*_unlikely .text.unlikely.*)
        *(.text.exit .text.exit.*)
        *(.text.startup .text.startup.*)
        *(.text.hot .text.hot.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
        *(.rodata1)
        *(.rodata.str1.1)
        *(.rodata.str1.8)
        *(.rodata.cst16)
        *(.rodata.cst8)
        *(.rodata.cst4)
        *(.rodata.cst2)
    } :rodata

    .note.gnu.build-id : {
        *(.note.gnu.build-id)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
        *(.data1)
        *(.data.rel.ro .data.rel.ro.*)
        *(.sdata .sdata.*)
        *(.tdata .tdata.*)
        *(.init_array .init_array.*)
        *(.fini_array .fini_array.*)
    } :data

    /* .bss must be last in :data to avoid zero-filling the binary */
    .bss : {
        *(.sbss .sbss.*)
        *(.scommon)
        *(.bss .bss.*)
        *(.tbss .tbss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.gcc_except_table*)
        *(.note .note.*)
        *(.note.GNU-stack)
        *(.interp)
        *(.comment)
        *(.debug*)
        *(.got .got.*)
        *(.got.plt .got.plt.*)
    }
}
