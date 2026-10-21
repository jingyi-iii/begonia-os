#include "boot.h"
#include "com.h"
#include "task.h"

static task_ctx ctx;

void first_task(void)
{
    com_print("first task running!\r\n");
    exit_from_qemu();
}

void kmain(void)
{
    com_print("kernel main running!\r\n");

    tss_init();
    task_init(&ctx, first_task, TASK_PRIV_USER);
    task_restore(&ctx);
}
