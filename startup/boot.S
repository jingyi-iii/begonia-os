/*
 * boot.S - BegoniaOS entry point, Multiboot v1.
 *
 * GRUB (or qemu's built-in loader, `qemu -kernel`) enters _start in 32-bit
 * protected mode: flat GDT, A20 on, paging off, interrupts disabled.
 * BIOS calls are gone, so the messages go out through the COM1 UART driven
 * by com.c instead of int 14h.
 */

/* --------------------------------------------------------------------- */
/* Multiboot v1 header: 4-byte aligned, within the first 8 KiB of the    */
/* file.  linker.ld keeps .multiboot first in the image for that reason. */
/* --------------------------------------------------------------------- */
.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    0x00000000
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a", @progbits
.align 4
    .long   MB_MAGIC
    .long   MB_FLAGS
    .long   MB_CHECKSUM

.section .text
.code32
.global _start

/* Loader state on entry: %eax = 0x2BADB002, %ebx = multiboot info address. */
_start:
    /* The loader hands us the magic in %eax and the multiboot info in
     * %ebx.  Save them before anything else: the first call into C
     * clobbers %eax, and no caller is obliged to keep %ebx either. */
    movl    %eax,       boot_magic
    movl    %ebx,       boot_mbi
    cli
    cld
    movl    $stack_top, %esp
    xorl    %ebp,       %ebp

    call    com_init

    pushl   $welc_msg
    call    com_print
    addl    $4,         %esp

    /* We are already in protected mode. switch_to_protected_mode() returns
     * here, and execution continues with the next statement. */
    call    switch_to_protect_mode

    pushl   $code32_msg
    call    com_print
    addl    $4,         %esp

    /* From here on a fault reports through the IDT instead of resetting
     * the machine.  Interrupts stay off: nothing calls sti yet. */
    call    interrupt_init
    int     $0x01       /* #DB: a trap, so returning really continues */
    int     $0x3F       /* 63: no CPU meaning, exercises a plain slot */

    pushl   $exit_msg
    call    com_print
    addl    $4,         %esp

    jmp     exit_from_qemu

/* On qemu: write to the isa-debug-exit port and it terminates itself. */
exit_from_qemu:
    movw    $0x0501,    %dx
    movb    $0x10,      %al
    outb    %al,        %dx

/* On real hardware that write is ignored, so just halt forever. */
hang:
    cli
    hlt
    jmp     hang

.section .rodata
welc_msg:
    .asciz  "\r\nBegoniaOS bootloader running\r\n"
code32_msg:
    .asciz  "Already in 32-bit protected mode\r\n"
exit_msg:
    .asciz  "Exiting...\r\n"

.section .data
.align 4
.global boot_magic
boot_magic:
    .long   0
.global boot_mbi
boot_mbi:
    .long   0

.section .bss, "aw", @nobits
.align 16
stack_bottom:
    .skip   16384
stack_top:
