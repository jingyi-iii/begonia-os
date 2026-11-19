/* Multiboot v1 kernel.
 *
 * .boot runs at 1 MiB with paging off and is linked for that same address, so
 * it is executable before the MMU is on.  Everything after it is linked for
 * the high window (KERNEL_VMA + physical) and only reachable once _start has
 * installed its page directory.  The loader still places every section at its
 * physical address (LMA), which is what AT() preserves. */
ENTRY(_start)

KERNEL_VMA = 0xF0000000;
KERNEL_LMA = 0x00100000;

PHDRS
{
    boot_text PT_LOAD FLAGS(5);     /* R E: multiboot header + _start       */
    boot_data PT_LOAD FLAGS(6);     /* RW : boot GDT, page directory, stack */
    text      PT_LOAD FLAGS(5);     /* R E: the high-half image             */
    data      PT_LOAD FLAGS(6);     /* RW : high-half data and bss          */
}

SECTIONS
{
    . = KERNEL_LMA;

    .multiboot : ALIGN(4) {
        KEEP(*(.multiboot))
    } :boot_text

    .boot.text : ALIGN(4K) {
        *(.boot.text)
    } :boot_text

    .boot.data : ALIGN(4K) {
        *(.boot.data)
    } :boot_data

    .boot.bss : ALIGN(4K) {
        *(.boot.bss)
    } :boot_data

    /* Switch the location counter to the high half: from here on the link
     * addresses are KERNEL_VMA + lma, while AT() keeps the load addresses
     * packed right after .boot. */
    . = KERNEL_VMA + .;

    .text : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text*)
    } :text

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata*)
    } :text

    .data : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data*)
    } :data

    .bss : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss*)
    } :data

    . = ALIGN(4K);
    kernel_end = . - KERNEL_VMA;

    /DISCARD/ : {
        *(.note*)
        *(.comment)
        *(.eh_frame*)
    }
}
