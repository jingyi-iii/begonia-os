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

static page_table krn_pt __attribute__((aligned(PAGE_SIZE))) = {0};

/*
 * Demo user address space.  The page directory and the vmm_space are plain
 * statics (not pointers): vmm_create_space() fills them in place.
 */
static page_table demo_pt __attribute__((aligned(PAGE_SIZE))) = {0};
static vmm_space   demo_vmm;
static pmm_region* demo_pmm = 0;

extern u8 kernel_end[];             /* linker.ld: physical end of the image */

/* hello.elf, embedded by ld -r -b binary (see the user/ rules in the Makefile). */
extern const u8 _binary_build_hello_elf_start[];
extern const u8 _binary_build_hello_elf_end[];

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

/*
 * elf_load_space - load an ELF32 executable into @space.
 *
 * Every address decision lives here: each segment takes pages from
 * @space->pmm, is mapped at its linked virtual address, and is filled through
 * the kernel's high-window alias of those pages.  lib/elf.c only parses the
 * image and copies the bytes.
 *
 * On failure everything this call created is released again.
 */
int elf_load_space(vmm_space* space, const void* image, u32 image_size,
                   u32* out_entry)
{
    elf_image img;
    void* dst[ELF_MAX_LOAD_SEGMENTS] = {0};
    u32   va[ELF_MAX_LOAD_SEGMENTS] = {0};
    u32   pa[ELF_MAX_LOAD_SEGMENTS] = {0};
    u32   sz[ELF_MAX_LOAD_SEGMENTS] = {0};
    u32   n = 0;
    int   ret;

    if (!space || !space->page_tbl || !space->pmm || !image || !out_entry)
        return E_INVAL;

    ret = elf_parse(image, image_size, &img);
    if (ret)
        return ret;

    /* the entry point has to land in the user window as well */
    if (!u32_range_within(img.entry, 1, USER_BASE_VA, KERNEL_BASE_VA))
        return E_INVAL;

    for (u32 i = 0; i < img.seg_count; i++) {
        u32 page_base = img.seg[i].vaddr & ~(PAGE_SIZE - 1);
        u32 offset    = img.seg[i].vaddr - page_base;
        u32 size;

        /* user mappings only, and the whole segment has to fit the window */
        if (!u32_range_within(img.seg[i].vaddr, img.seg[i].memsz,
                              USER_BASE_VA, KERNEL_BASE_VA)) {
            ret = E_INVAL;
            goto fail;
        }

        /* a segment may start mid-page and always needs room for its bss tail */
        if (!u32_align_up(offset + img.seg[i].memsz, PAGE_SIZE, &size)) {
            ret = E_INVAL;
            goto fail;
        }

        if (pmm_alloc_blocks(space->pmm, size, &pa[n])) {
            ret = E_NOMEM;
            goto fail;
        }

        if (!vmm_map_pages_at(space, page_base, pa[n], size / PAGE_SIZE,
                              PTE_USER_PAGE)) {
            pmm_free_blocks(space->pmm, pa[n], size);
            ret = E_NOMEM;
            goto fail;
        }

        va[n]  = page_base;
        sz[n]  = size;
        dst[n] = (u8*)pa_to_va(pa[n]) + offset;     /* kernel-side alias */
        n++;
    }

    ret = elf_load_into(image, image_size, &img, dst);
    if (ret)
        goto fail;

    *out_entry = img.entry;
    return 0;

fail:
    while (n > 0) {
        n--;
        vmm_unmap_pages(space, va[n]);
        pmm_free_blocks(space->pmm, pa[n], sz[n]);
    }

    return ret;
}

/*
 * Reserve the physical window the demo's user pages come from: right behind
 * the kernel image, and low enough that pa_to_va() can reach every byte of it
 * (the kernel window maps the first 256 MiB).
 */
static int demo_mem_init(void)
{
    u32 base;
    u32 size = 0x04000000;                      /* 64 MiB */

    if (demo_pmm)
        return 0;

    base = PAGE_ALIGN((u32)kernel_end);

    if (!u32_range_within(base, size, (u32)kernel_end, 0x10000000ULL))
        return E_INVAL;

    demo_pmm = pmm_reserve(base, size, PAGE_SIZE);

    return demo_pmm ? 0 : E_NOMEM;
}

/*
 * load_demo_elf - build a fresh address space and load @image into it.
 *
 * On success @out_entry receives the entry point (a user VA).  The caller
 * still has to switch to the demo space and start a ring-3 thread at it.
 */
int load_demo_elf(const void* image, u32 image_size, u32* out_entry)
{
    int ret = demo_mem_init();
    if (ret)
        return ret;

    ret = vmm_create_space(&demo_pt, (page_table*)page_get_table(),
                           demo_pmm, &demo_vmm);
    if (ret)
        return ret;

    /*
     * The ring-3 demo still executes kernel code and keeps its stack inside
     * the tcb (kernel heap), so the high window has to stay user-accessible
     * in this space.  page_clone_table() moved it to supervisor by design.
     * TODO: drop this once user code owns its stack and page tables.
     */
    for (u32 i = 0; i < 0x8000000u / 0x400000u; i++)
        page_unmap_4mb(&demo_pt, 0xF0000000 + i * 0x400000);
    if (page_map_4mb_range(&demo_pt, 0xF0000000, 0, 0x8000000, PTE_USER_PAGE)) {
        vmm_destroy_space(&demo_vmm);
        return E_NOMEM;
    }

    ret = elf_load_space(&demo_vmm, image, image_size, out_entry);
    if (ret)
        vmm_destroy_space(&demo_vmm);

    return ret;
}

void kmain(void)
{
    com_print("kernel main running!\r\n");
    pit_init(100);

    /* TODO: decouple user mode code to a single elf */
    if (!page_map_4mb_range(&krn_pt, 0xF0000000, 0, 0x8000000, PTE_USER_PAGE))
        page_load_table(&krn_pt);

    /* Load the embedded user program and run it in its own address space. */
    u32 hello_entry = 0;
    u32 hello_size  = (u32)(_binary_build_hello_elf_end -
                            _binary_build_hello_elf_start);
    int hello_ok    = 0;

    if (!load_demo_elf(_binary_build_hello_elf_start, hello_size, &hello_entry)) {
        LOG("hello.elf: loaded %u bytes, entry 0x%x", hello_size, hello_entry);
        vmm_switch_to_space(&demo_vmm);
        hello_ok = 1;
    } else {
        log_err("hello.elf: load failed");
    }

    process_env_init();
    mailbox_syscall_init();
    thread_create(TASK_PRIV_USER, second_task, 0, 0);
    thread_create(TASK_PRIV_KERNEL, first_task, 0, 0);
    if (hello_ok)
        thread_create(TASK_PRIV_USER, (task_entry)(uptr)hello_entry, 0, 0);
    thread_start();

    // current flow:
    //   thread_start() -> thread_switch() -> common thread_entry() ->
    //     1. target is ring3 -> task_switch_to_ring3() -> iret with ctx.entry
    //     2. target is ring0 -> task_switch_asm -> ret with ctx.entry
}
