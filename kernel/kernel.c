#include "boot.h"
#include "com.h"
#include "task.h"
#include "process.h"
#include "pic.h"
#include "syscall.h"
#include "mailbox.h"
#include "mm/vmm.h"

static page_table krn_pt __attribute__((aligned(PAGE_SIZE))) = {0};

void first_task(void)
{
    syscall(0, 0, 0);
    while (1) {
        com_print("first task running!\r\n");
        thread_yield();
    }
}

void second_task(void)
{
    static u32 count = 0;
    while (1) {
        com_print("second task running!\r\n");

        count++;
        if (count > 1000)
            exit_from_qemu();
    }
}

void kmain(void)
{
    com_print("kernel main running!\r\n");
    pit_init(100);

    /* TODO: decouple user mode code to a single elf */
    page_map_4mb_range(&krn_pt, 0xF0000000, 0, 0x8000000, PTE_USER_PAGE);
    page_load_table(&krn_pt);

    process_env_init();
    mailbox_syscall_init();
    thread_create(TASK_PRIV_USER, second_task, 0, 0);
    thread_create(TASK_PRIV_KERNEL, first_task, 0, 0);
    thread_start();

    // current flow:
    //   thread_start() -> thread_switch() -> common thread_entry() ->
    //     1. target is ring3 -> task_switch_to_ring3() -> iret with ctx.entry
    //     2. target is ring0 -> task_switch_asm -> ret with ctx.entry
}
