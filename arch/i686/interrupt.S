/*
 * interrupt.S - the interrupt stub table.
 *
 * isr_tbl holds one slot per IDT vector, ISR_SLOT bytes apart, so a vector's
 * entry point is arithmetic:
 *
 *      isr_tbl + vector * ISR_SLOT
 *
 * and the C side never needs 256 separate symbols.  0x90 is a multiple of 16,
 * so every slot stays 16-byte aligned wherever the table lands.
 *
 * Every slot is a working stub.  It pushes its own vector number in front of
 * the frame the CPU already built and jumps to the shared isr_common, so the
 * log printed from C can name the vector that fired.  The CPU pushes an error
 * code by itself for a few exceptions only, so for every other vector the slot
 * pushes a dummy 0 first and the frame keeps one shape:
 *
 *      edi esi ebp esp ebx edx ecx eax      pusha, in reverse
 *      gs fs es ds                          saved by isr_common
 *      vector                               pushed by the slot
 *      error                                pushed by the CPU, or the dummy 0
 *      eip cs eflags                        pushed by the CPU
 *      esp ss                               pushed by the CPU on a ring change
 *
 * The rest of the 0x90 bytes is NOP padding: room to grow the stub, or to
 * patch it at run time.
 */

.set ISR_SLOT,  0x90        /* bytes reserved per vector */
.set ISR_COUNT, 256         /* one slot per IDT entry    */

.code32
.section .text
.align 16

isr_common:
    pushl   %ds
    pushl   %es
    pushl   %fs
    pushl   %gs
    pusha

    movw    $0x10,      %ax
    movw    %ax,        %ds
    movw    %ax,        %es
    movw    %ax,        %fs
    movw    %ax,        %gs

    pushl   %esp                /* isr_frame * */
    call    isr_dispatch
    addl    $4,         %esp

    popa
    popl    %gs
    popl    %fs
    popl    %es
    popl    %ds
    addl    $8,         %esp    /* drop the vector and the error code */
    iret

.align 16
.global isr_tbl
isr_tbl:
isr_begin:
    .set    ISR_INDEX, 0
    .rept   ISR_COUNT
    /* 8 #DF, 10..14 #TS/#NP/#SS/#GP/#PF and 17 #AC bring their own error
     * code; every other vector needs the dummy so the frame stays uniform. */
    .if !(ISR_INDEX == 8 || (ISR_INDEX >= 10 && ISR_INDEX <= 14) || ISR_INDEX == 17)
    pushl   $0
    .endif
    pushl   $ISR_INDEX          /* the vector this slot stands for */
    jmp     isr_common
    .set    ISR_INDEX, ISR_INDEX + 1

    /* Pad to the next slot boundary by index, not by "how long was the stub":
     * gas relaxes `jmp` to its short form wherever it fits (3 bytes less), so
     * a fixed-size stub would silently break the ISR_SLOT stride.  .org also
     * fails the build if a stub ever grows past its slot. */
    .org    isr_begin + ISR_INDEX * ISR_SLOT, 0x90
    .endr
isr_end:

.global syscall_tbl
syscall_tbl:
    pushl   %ds
    pushl   %es
    pushl   %fs
    pushl   %gs
    pusha

    movw    $0x10,      %ax
    movw    %ax,        %ds
    movw    %ax,        %es
    movw    %ax,        %fs
    movw    %ax,        %gs

    /* ebx = id, ecx = data, edx = data_size */
    pushl   %edx
    pushl   %ecx
    pushl   %ebx
    call    syscall_dispatch
    addl    $12, %esp

    movl    %eax,       28(%esp)    /* return value survives popa */

    popa
    popl    %gs
    popl    %fs
    popl    %es
    popl    %ds
    iret                            /* no vector/error to drop: int pushes 3 words */
