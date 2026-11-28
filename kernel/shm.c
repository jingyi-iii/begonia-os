#include "shm.h"
#include "range.h"
#include "log.h"

int shm_share(i32 target_pid, u32 src_va, size_t src_size, u32* out_va)
{
    tcb* thread = thread_get_current();
    pcb* curr = thread ? thread->parent : 0;
    pcb* target = proc_get_by_pid(target_pid);
    u32 pa = 0;
    u32 pa_size = 0;
    u32 mapping_size = 0;
    int ret = 0;

    if (!out_va)
        return E_INVAL;

    if (!curr || !target)
        return E_NOTFOUND;

    if (!IS_4KB_ALIGN(src_va))
        return E_INVAL;

    if (vmm_va_to_pa(&curr->vmm, src_va, &pa, &pa_size))
        return E_NOTFOUND;

    if (!u32_align_up(src_size, PAGE_SIZE, &mapping_size))
        return E_INVAL;

    if (mapping_size == 0 || mapping_size > pa_size)
        return E_INVAL;

    ret = pmm_ref_blocks(curr->vmm.pmm, pa, mapping_size);
    if (ret)
        return ret;

    *out_va = (u32)vmm_map_pages_shared(&target->vmm, pa,
                mapping_size / PAGE_SIZE, PTE_USER_PAGE);
    if (!(*out_va)) {
        log_err("%s: failed to map the pages in target process\n", __func__);
        pmm_free_blocks(curr->vmm.pmm, pa, mapping_size);
        *out_va = 0;
        return E_NOMEM;
    }

    return 0;
}

int shm_unshare(i32 target_pid, u32 target_va)
{
    tcb* thread = thread_get_current();
    pcb* curr = thread ? thread->parent : 0;
    pcb* target = proc_get_by_pid(target_pid);

    if (!curr || !target)
        return E_NOTFOUND;

    return vmm_unmap_pages_shared(&target->vmm, target_va);
}
