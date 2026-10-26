#include "boot.h"
#include "com.h"
#include "task.h"
#include "process.h"

void first_task(void)
{
    com_print("first task running!\r\n");
    thread_yield();
}

void second_task(void)
{
    com_print("second task running!\r\n");
    exit_from_qemu();
}

void kmain(void)
{
    com_print("kernel main running!\r\n");

    tss_init();
    thread_create(TASK_PRIV_KERNEL, second_task, 0, 0);
    thread_create(TASK_PRIV_KERNEL, first_task, 0, 0);
    thread_start();

    // current flow:
    //   thread_switch() -> common thread_entry() ->
    //     1. target is ring3 -> task_switch_to_ring3() -> iret with ctx.entry
    //     2. target is ring0 -> task_switch_asm -> ret with ctx.entry
}
