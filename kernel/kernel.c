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

static page_table krn_pt __attribute__((aligned(PAGE_SIZE))) = {0};

/*
 * Demo user address space.  The page directory and the vmm_space are plain
 * statics (not pointers): vmm_create_space() fills them in place.
 */
static page_table demo_pt __attribute__((aligned(PAGE_SIZE))) = {0};
static vmm_space   demo_vmm;
static pmm_region* demo_pmm = 0;

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

static int demo_mem_init(void)
{
    u32 base;
    u32 size = 0x04000000;                      /* 64 MiB */

    if (demo_pmm)
        return 0;

    base = PAGE_ALIGN((u32)0x03000000);

    if (!u32_range_within(base, size, (u32)kernel_end, 0x10000000ULL))
        return E_INVAL;

    demo_pmm = pmm_reserve(base, size, PAGE_SIZE);

    return demo_pmm ? 0 : E_NOMEM;
}

static int module_name_matches(const char* module, const char* name)
{
    u32 mlen = 0;
    u32 nlen = 0;

    if (!module || !name || !name[0])
        return 0;

    while (module[mlen])
        mlen++;
    while (name[nlen])
        nlen++;

    if (nlen > mlen)
        return 0;

    return strcmp(module + (mlen - nlen), name) == 0;
}

static i32 load_user_elf_by_name(const char* name)
{
    int n = mboot_module_count();
    int ret = demo_mem_init();
    if (ret)
        return ret;

    ret = vmm_create_space(&demo_pt, (page_table*)page_get_table(),
                           demo_pmm, &demo_vmm);
    if (ret)
        return ret;
    vmm_switch_to_space(&demo_vmm);

    LOG("%s: %d module(s), looking for '%s'", __func__, n, name);

    for (int i = 0; i < n; i++) {
        const char* mod_name = mboot_module_name(i);
        u8* start = 0;
        u8* end = 0;
        u32 filesz = 0;
        u32 size = 0;
        u32 pa = 0;
        u32 va = 0;
        u32 copy = 0;

        log("%s: index %d: %s\n", __func__, i,
            mod_name ? mod_name : "(unnamed)");

        if (mboot_module_get(i, &start, &end) != 0)
            break;

        if (!module_name_matches(mod_name, name))
            continue;

        filesz = (u32)(end - start);
        LOG("%s: found '%s' at va 0x%x..0x%x (%u bytes)", __func__, name,
            (u32)(uptr)start, (u32)(uptr)end, filesz);
        
        if (!u32_align_up((u32)(end - start), PAGE_SIZE, &size))
            continue;

        if (pmm_alloc_blocks(demo_pmm, size, &pa))
            continue;

        copy = (u32)pa_to_va(pa);
        memcpy((void*)copy, start, filesz);
        memset((void*)copy + filesz, 0, size - filesz);

        u32 entry = 0;
        int ret = elf_load_space(&demo_vmm, (void*)copy, size, &entry);
        if (ret) {
            vmm_destroy_space(&demo_vmm);
            log_err("%s: elf_load_space('%s') failed: %d", __func__, name, ret);
            return ret;
        }

        va = (u32)vmm_map_pages(&demo_vmm, pa, size / PAGE_SIZE, PTE_USER_PAGE);
        if (!va) {
            pmm_free_blocks(demo_pmm, pa, size);
            continue;
        }

        LOG("%s: '%s' loaded, entry 0x%x", __func__, name, entry);

        thread_create(TASK_PRIV_USER, (task_entry)(uptr)entry, 0, 0);
        return i;

    }

    LOG("%s: '%s' not found among %d modules", __func__, name, n);
    return E_NOTFOUND;
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

    process_env_init(&demo_vmm);
    mailbox_syscall_init();
    thread_create(TASK_PRIV_KERNEL, second_task, 0, 0);
    thread_create(TASK_PRIV_KERNEL, first_task, 0, 0);

    /* Test the module list path: hello.elf is also shipped as a GRUB module. */
    i32 hello_module = load_user_elf_by_name("hello.elf");
    LOG("hello.elf: module index %d", hello_module);

    thread_start();

    // current flow:
    //   thread_start() -> thread_switch() -> common thread_entry() ->
    //     1. target is ring3 -> task_switch_to_ring3() -> iret with ctx.entry
    //     2. target is ring0 -> task_switch_asm -> ret with ctx.entry
}
