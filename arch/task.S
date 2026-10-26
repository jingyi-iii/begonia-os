.code32
.section    .text
.globl      task_switch_asm
task_switch_asm:
    movl    4(%esp),        %eax    # from
    movl    8(%esp),        %edx    # to

    pushl   %ebp
    pushl   %ebx
    pushl   %esi
    pushl   %edi

    testl   %eax,           %eax
    jz      1f
    movl    %esp,           (%eax)  # esp must be the first member of task_ctx

1:
    movl    (%edx),         %esp
    popl    %edi
    popl    %esi
    popl    %ebx
    popl    %ebp
    ret
