/*
 * boot.S - BegoniaOS entry point, Multiboot v1.
 *
 * GRUB (or qemu's built-in loader, `qemu -kernel`) enters _start in 32-bit
 * protected mode: flat GDT, A20 on, paging off, interrupts disabled.
 * BIOS calls are gone, so the message is printed by driving the COM1 UART
 * directly instead of using int 14h.
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
    cli
    cld
    movl    $stack_top, %esp
    xorl    %ebp,       %ebp

    call    serial_init

    movl    $welc_msg,  %esi
    call    print

    /* We are already in protected mode. switch_to_protected_mode() returns
     * here, and execution continues with the next statement. */
    call    switch_to_protect_mode

    movl    $code32_msg,  %esi
    call    print

    /* From here on a fault reports through the IDT instead of resetting
     * the machine.  Interrupts stay off: nothing calls sti yet. */
    call    interrupt_init
    int     $0x01       /* #DB: a trap, so returning really continues */
    int     $0x3F       /* 63: no CPU meaning, exercises a plain slot */

    movl    $exit_msg,  %esi
    call    print

    jmp     exit_from_qemu

/* serial_init: COM1 (0x3F8) 115200 baud, 8N1, FIFO on */
serial_init:
    movw    $0x3FB,     %dx        # line control: DLAB on
    movb    $0x80,      %al
    outb    %al,        %dx

    movw    $0x3F8,     %dx        # divisor low byte = 1 -> 115200 baud
    movb    $0x01,      %al
    outb    %al,        %dx

    movw    $0x3F9,     %dx        # divisor high byte = 0
    xorl    %eax,       %eax
    outb    %al,        %dx

    movw    $0x3FB,     %dx        # 8N1, DLAB off
    movb    $0x03,      %al
    outb    %al,        %dx

    movw    $0x3FA,     %dx        # FIFO enable and clear
    movb    $0xC7,      %al
    outb    %al,        %dx

    ret

/* print: write the NUL-terminated string at %esi to COM1 */
print:
    lodsb
    testb   %al,        %al
    jz      1f
    call    putc
    jmp     print
1:
    ret

/* putc: send the byte in %al to COM1, waiting for the transmitter */
putc:
    pushl   %eax
1:
    movw    $0x3FD,     %dx        # line status: THR empty?
    inb     %dx,        %al
    testb   $0x20,      %al
    jz      1b

    movw    $0x3F8,     %dx        # transmit holding register
    popl    %eax
    outb    %al,        %dx
    ret

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

.section .bss, "aw", @nobits
.align 16
stack_bottom:
    .skip   16384
stack_top:
