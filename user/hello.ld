/* A user program: linked into the user window, not the kernel's high half.
 *
 * USER_VMA matches USER_BASE_VA in arch/i686/page.h; the loader maps every
 * PT_LOAD segment at the address written here. */
ENTRY(_start)

USER_VMA = 0x10000000;

PHDRS
{
    text PT_LOAD FLAGS(5);      /* R E */
    data PT_LOAD FLAGS(6);      /* RW  */
}

SECTIONS
{
    . = USER_VMA;

    .text ALIGN(4K) : {
        *(.text*)
    } :text

    .rodata ALIGN(4K) : {
        *(.rodata*)
    } :text

    .data ALIGN(4K) : {
        *(.data*)
    } :data

    .bss ALIGN(4K) : {
        *(.bss*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
