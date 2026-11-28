#include "boot.h"
#include "com.h"
#include "task.h"
#include "process.h"
#include "pic.h"
#include "syscall.h"
#include "mailbox.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "log.h"
#include "elf.h"
#include "range.h"
#include "errno.h"
#include "multiboot.h"
#include "string.h"
#include "heap.h"

static page_table krn_pt __attribute__((aligned(PAGE_SIZE))) = {0};
static vmm_space krn_vmm = {0};

extern u8 kernel_end[];             /* linker.ld: physical end of the image */

void first_task(void)
{
    syscall(0, 0, 0);
    com_print("first task running!\r\n");
    while (1) {
        thread_yield();
    }
}

void second_task(void)
{
    static u32 count = 0;
    com_print("second task running!\r\n");
    while (1) {
        count++;
        if (count > 1000)
            exit_from_qemu();
    }
}

void kmain(void)
{
    com_print("kernel main running!\r\n");
    pit_init(100);

    /*
     * boot_mbi is a physical address stored at a low virtual one, so hand it
     * to the parser now: boot_pd still identity-maps low memory, while krn_pt
     * below only maps the high window.  After this the parser goes through
     * pa_to_va(), so it can be used from the high half too.
     */
    int mbi_ret = mboot_init(boot_mbi);
    LOG("multiboot: init(boot_mbi=0x%x) -> %d", boot_mbi, mbi_ret);

    if (!page_map_4mb_range(&krn_pt, 0xF0000000, 0, 0x8000000, PTE_KERNEL_PAGE))
        page_load_table(&krn_pt);

    pmm_space* pmm = pmm_reserve(0x03000000, 0x04000000, PAGE_SIZE);
    if (!pmm) {
        log_err("kmain: failed to alloc pmm space\n");
        return;
    }

    page_table* pt = malloc_aligned(sizeof(page_table), PAGE_SIZE);
    if (!pt)
        return;

    int ret = vmm_clone_space(pt, &krn_pt, pmm, &krn_vmm);
    if (ret) {
        free(pt);
        log_err("kmain: vmm_clone_space() failed, return %d\n", ret);
        return;
    }

    process_env_init(&krn_vmm);
    mailbox_syscall_init();
    proc_create(PROC_PRIV_KERNEL, second_task, 0);
    proc_create(PROC_PRIV_KERNEL, first_task, 0);

    /* Test the module list path: hello.elf is also shipped as a GRUB module. */
    i32 hello_module = proc_load_from_elf("hello.elf");
    LOG("hello.elf: pid %d", hello_module);

    thread_start();

    // current flow:
    //   thread_start() -> thread_switch() -> common thread_entry() ->
    //     1. target is ring3 -> task_switch_to_ring3() -> iret with ctx.entry
    //     2. target is ring0 -> task_switch_asm -> ret with ctx.entry
}
