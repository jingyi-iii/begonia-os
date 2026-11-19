/*
 * boot.S - BegoniaOS entry point, Multiboot v1.
 *
 * GRUB (or qemu's built-in loader) enters _start in 32-bit protected mode
 * with flat segments, paging off and interrupts disabled, at the physical
 * address of the first segment (1 MiB).
 *
 * _start lives in .boot, linked for that same low address, so it still runs
 * with the MMU off.  It installs a minimal page directory - an identity map
 * for the few pages this stub needs, plus the high window the rest of the
 * kernel is linked at - switches paging on, and far-jumps into the high half.
 * Everything from _start_high on executes at KERNEL_VMA + physical.
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

/* --------------------------------------------------------------------- */
/* .boot.text: runs at 1 MiB with paging off.  It must not call into or   */
/* reference anything linked into the high window.                        */
/* --------------------------------------------------------------------- */
.section .boot.text, "ax", @progbits
.code32
.global _start

_start:
    cli
    cld

    /* The loader hands us the magic in %eax and the multiboot info (a
     * physical address) in %ebx.  Both are kept in .boot.data, which stays
     * addressable through PDE[0] after the switch. */
    movl    %eax,               boot_magic
    movl    %ebx,               boot_mbi

    movl    $boot_stack_top,    %esp
    xorl    %ebp,               %ebp

    /* Clear the page directory. */
    movl    $boot_pd,           %edi
    xorl    %eax,               %eax
    movl    $1024,              %ecx
    rep     stosl

    /* PDE[0]: identity map 0..4 MiB, so this stub, its stack, the page
     * directory and .boot.data stay addressable across the CR0.PG switch. */
    movl    $boot_pd,           %edi
    movl    $0x00000083,        %eax           /* present | rw | 4MB page, pa = 0 */
    movl    %eax,               (%edi)

    /* PDE[960..967]: 0xF0000000..0xF2000000 -> physical 0..32 MiB, which
     * covers the whole image (the 16 MB heap in .bss puts kernel_end near
     * 18 MiB).  Adding 0x400000 to the raw entry advances the page base,
     * because the flags live in the low 12 bits. */
    /*
     * TODO(paging): the U/S bit is set only so the existing ring-3 demo, which
     * runs out of the kernel image, keeps working now that paging is on.  Drop
     * it back to 0x83 once user pages are mapped through the VMM: with it set,
     * ring 3 can read and write every byte of the kernel.
     */
    movl    $0x00000087,        %eax           /* + U/S: see the TODO below */
    movl    $(960 * 4),         %ecx
1:
    movl    %eax,               (%edi, %ecx)
    addl    $0x400000,          %eax
    addl    $4,                 %ecx
    cmpl    $(968 * 4),         %ecx
    jb      1b

    /* Our own flat GDT, so the far jump below does not depend on whatever
     * the loader left in GDTR. */
    lgdt    boot_gdt_desc

    movl    %cr4,               %eax
    orl     $0x00000010,        %eax           /* CR4.PSE: 4 MB pages */
    movl    %eax,               %cr4

    /* .boot is identity mapped, so its link address is the physical one and
     * can go straight into CR3. */
    movl    $boot_pd,           %eax
    movl    %eax,               %cr3

    movl    %cr0,               %eax
    orl     $0x80000000,        %eax           /* CR0.PG */
    movl    %eax,               %cr0

    /* From here on instruction fetches go through the page table.  The next
     * instruction is still at its low address (PDE[0]); the far jump moves
     * execution into the high window. */
    ljmp    $0x08,              $_start_high

/* --------------------------------------------------------------------- */
/* .text: the high half.  Absolute addresses are KERNEL_VMA + physical.   */
/* --------------------------------------------------------------------- */
.section .text
.code32
.global _start_high

_start_high:
    /* The low stack is only 4 KiB and is not where the kernel expects to
     * run, so switch to the real one before the first call. */
    movl    $stack_top,         %esp
    xorl    %ebp,               %ebp

    call    com_init

    pushl   $welc_msg
    call    com_print
    addl    $4,                 %esp

    /* Installs the real GDT and reloads every segment register with it. */
    call    switch_to_protect_mode

    pushl   $code32_msg
    call    com_print
    addl    $4,                 %esp

    /* From here on a fault reports through the IDT instead of resetting the
     * machine.  Interrupts stay off: nothing calls sti yet. */
    call    interrupt_init

    pushl   $exit_msg
    call    com_print
    addl    $4,                 %esp

    call    kmain

/* On qemu: write to the isa-debug-exit port and it terminates itself.  C code
 * (kmain) calls this to end the run. */
.global exit_from_qemu
exit_from_qemu:
    movw    $0x0501,            %dx
    movb    $0x10,              %al
    outb    %al,                %dx

/* On real hardware that write is ignored, so just halt forever. */
hang:
    cli
    hlt
    jmp     hang

.section .rodata
welc_msg:
    .asciz  "\r\nBegoniaOS startup running\r\n"
code32_msg:
    .asciz  "Already in 32-bit protected mode\r\n"
exit_msg:
    .asciz  "Exiting...\r\n"

/* --------------------------------------------------------------------- */
/* .boot.data / .boot.bss: state the low stub needs.  Keeping these below */
/* the high window is what makes them writable while paging is off.       */
/* --------------------------------------------------------------------- */
.section .boot.data, "aw", @progbits
.align 4
.global boot_magic
boot_magic:
    .long   0
.global boot_mbi
boot_mbi:
    .long   0                           /* physical address of the mbi      */

.align 8
boot_gdt:
    .quad   0x0000000000000000          /* null                             */
    .quad   0x00CF9A000000FFFF          /* 0x08: flat 32-bit code           */
    .quad   0x00CF92000000FFFF          /* 0x10: flat 32-bit data           */
boot_gdt_end:
boot_gdt_desc:
    .word   boot_gdt_end - boot_gdt - 1
    .long   boot_gdt

.section .boot.bss, "aw", @nobits
.align 4096
boot_pd:
    .skip   4096                        /* page directory, must be 4K aligned */
.align 16
boot_stack:
    .skip   4096
boot_stack_top:

/* --------------------------------------------------------------------- */
/* .bss: the high-half stack.                                             */
/* --------------------------------------------------------------------- */
.section .bss, "aw", @nobits
.align 16
stack_bottom:
    .skip   16384
stack_top:
