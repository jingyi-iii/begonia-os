/* Multiboot v1 kernel: loaded at 1 MiB.  The .multiboot header must stay
 * within the first 8 KiB of the file, so it is placed first. */
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot : ALIGN(4) {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(4K) {
        *(.text*)
    }

    .rodata : ALIGN(4K) {
        *(.rodata*)
    }

    .data : ALIGN(4K) {
        *(.data*)
    }

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss*)
    }

    /DISCARD/ : {
        *(.note*)
        *(.comment)
        *(.eh_frame*)
    }
}
