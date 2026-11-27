#include "mm/vmm.h"
#include "heap.h"
#include "page.h"
#include "errno.h"
#include "log.h"
#include "string.h"
#include "range.h"

static int vmm_region_node_cmp(const rbnode* left, const rbnode* right)
{
    if (!left || !right)
        return -1;

    vmm_region* l = rb_entry(left, vmm_region, node);
    vmm_region* r = rb_entry(right, vmm_region, node);

    if ((u32)l->va_start < (u32)r->va_start)
        return -1;
    else if ((u32)l->va_start > (u32)r->va_start)
        return 1;
    else
        return 0;
}

static int vmm_region_key_cmp(const void* key, const rbnode* node)
{
    if (!node || !key)
        return -1;

    vmm_region* r = rb_entry(node, vmm_region, node);
    if (u32_range_contains(r->va_start, r->size, (u32)key))
        return 0;
    else if ((u32)key < (u32)r->va_start)
        return -1;
    else
        return 1;
}

static inline int vmm_range_is_available(vmm_space* space, u32 va, u32 size)
{
    rbnode* node  = space->region_tree->root;
    rbnode* below = 0;      /* greatest region with va_start <= va */
    rbnode* above = 0;      /* smallest region with va_start >  va */

    while (node != space->region_tree->nil) {
        vmm_region* r = rb_entry(node, vmm_region, node);

        if (r->va_start <= va) {
            below = node;
            node  = node->right;
        } else {
            above = node;
            node  = node->left;
        }
    }

    if (below) {
        vmm_region* r = rb_entry(below, vmm_region, node);
        if (u32_ranges_overlap(va, size, r->va_start, r->size))
            return 0;
    }

    if (above) {
        vmm_region* r = rb_entry(above, vmm_region, node);
        if (u32_ranges_overlap(va, size, r->va_start, r->size))
            return 0;
    }

    return 1;
}

static int vmm_get_available_va(vmm_space* space, u32 size, u32* out_va)
{
    if (!space || !out_va)
        return E_INVAL;

    if (rbtree_is_empty(space->region_tree)) {
        if (!u32_range_within(USER_BASE_VA, size, USER_BASE_VA, KERNEL_BASE_VA))
            return E_NOMEM;

        *out_va = USER_BASE_VA;
        return 0;
    }

    rbnode* first_node = rbtree_first(space->region_tree);
    vmm_region* first = rb_entry(first_node, vmm_region, node);

    if (u32_range_within(USER_BASE_VA, size, USER_BASE_VA, first->va_start)) {
        *out_va = USER_BASE_VA;
        return 0; 
    }

    rbtree_for_each(curr_node, space->region_tree) {
        vmm_region* curr = rb_entry(curr_node, vmm_region, node);
        u32 cand = curr->va_start + curr->size;

        rbnode* next_node = rbtree_next(space->region_tree, curr_node);
        vmm_region* next = next_node ? rb_entry(next_node, vmm_region, node) : 0;

        u64 limit = next ? next->va_start : KERNEL_BASE_VA;
        if (u32_range_within(cand, size, USER_BASE_VA, limit)) {
            *out_va = cand;
            return 0;
        }
    }

    return E_NOMEM;
}

static int vmm_alloc_region(vmm_space* space, size_t size,
                            u32 flags, vmm_region** out)
{

    vmm_region* region = 0;
    u32 va = 0;

    if (!space || !space->region_tree || !out)
        return E_INVAL;

    if (!size || !IS_4KB_ALIGN(size))
        return E_INVAL;

    if (vmm_get_available_va(space, size, &va))
        return E_NOMEM;

    region = malloc(sizeof(vmm_region));
    if (!region)
        return E_NOMEM;

    memset(region, 0, sizeof(vmm_region));
    region->va_start = va;
    region->size     = size;
    region->flags    = flags;

    if (!vmm_range_is_available(space, va, size)) {
        free(region);
        return E_EXISTS;
    }

    rbtree_insert(space->region_tree, &region->node, vmm_region_node_cmp);
    *out = region;

    return 0;
}

static int vmm_alloc_region_at(vmm_space* space, u32 va, size_t size,
                               u32 flags, vmm_region** out)
{

    vmm_region* region = 0;

    if (!space || !space->region_tree || !out)
        return E_INVAL;

    if (!IS_4KB_ALIGN(va) || !size || !IS_4KB_ALIGN(size))
        return E_INVAL;

    if (!u32_range_within(va, size, USER_BASE_VA, KERNEL_BASE_VA))
        return E_INVAL;

    if (!vmm_range_is_available(space, va, size))
        return E_EXISTS;

    region = malloc(sizeof(vmm_region));
    if (!region)
        return E_NOMEM;

    memset(region, 0, sizeof(vmm_region));
    region->va_start = va;
    region->size     = size;
    region->flags    = flags;

    rbtree_insert(space->region_tree, &region->node, vmm_region_node_cmp);
    *out = region;

    return 0;
}

static int vmm_release_region(vmm_space* space, vmm_region* region)
{
    if (!space || !space->region_tree || !region)
        return EINVAL;

    rbtree_delete(space->region_tree, &region->node);
    free(region);

    return 0;
}

static vmm_region* vmm_lookup_region(vmm_space* space, u32 va)
{
    vmm_region* out = 0;
    rbnode* node = 0;

    if (!space || !space->region_tree)
        return 0;

    node = rbtree_search(space->region_tree, (void*)va, vmm_region_key_cmp);
    if (node)
        out = rb_entry(node, vmm_region, node);

    return out;
}

int vmm_create_space(page_table* table, pmm_region* pmm, vmm_space* out)
{
    vmm_space space = {0};

    if (!out || !pmm)
        return E_INVAL;
    if ((u32)table < KERNEL_BASE_VA || !IS_4KB_ALIGN((u32)table))
        return E_INVAL;

    space.region_tree = rbtree_create();
    if (!space.region_tree)
        return E_NOMEM;

    space.page_tbl = table;
    space.pmm = pmm;
    spinlock_init(&space.sp_lock);
    *out = space;

    return 0;
}

int vmm_clone_space(page_table* new_table, page_table* from_table,
                    pmm_region* pmm, vmm_space* out)
{
    int ret = 0;
    vmm_space space = {0};

    if (!out || !pmm)
        return E_INVAL;
    if ((u32)new_table < KERNEL_BASE_VA || (u32)from_table < KERNEL_BASE_VA)
        return E_INVAL;
    if (!IS_4KB_ALIGN((u32)new_table) || !IS_4KB_ALIGN((u32)from_table))
        return E_INVAL;

    space.region_tree = rbtree_create();
    if (!space.region_tree)
        return E_NOMEM;

    ret = page_clone_table(new_table, from_table);
    if (ret) {
        log_err("%s: page table(%p): clone from page table(%p) failed (err %d)\n",
                __func__, (void*)new_table, (void*)from_table, ret);
        rbtree_destroy(space.region_tree);
        return ret;
    }

    space.page_tbl = new_table;
    space.pmm = pmm;
    spinlock_init(&space.sp_lock);
    *out = space;

    return 0;
}

/*
 * The caller must ensure the page table is not the one loaded in CR3: reap a
 * process only after its threads are gone (children empty) and after the
 * scheduler switched away from it.
 */
void vmm_destroy_space(vmm_space* space)
{
    if (!space || !space->page_tbl)
        return;

    page_table* page_tbl = space->page_tbl;
    if (page_get_table() == (u32)page_tbl) {
        log_err("%s: page table(%p): refusing to destroy the active space\n",
                __func__, (void*)page_tbl);
        return;
    }

    u32 unmap_errors = 0;
    u32 pmm_errors = 0;
    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    if (space->region_tree) {
        rbtree_for_each_safe(pos, n, space->region_tree) {
            vmm_region* r = rb_entry(pos, vmm_region, node);
            for (u32 i = 0; i < r->size / PAGE_SIZE; i++) {
                if (page_unmap_4kb(space->page_tbl, r->va_start + i * PAGE_SIZE))
                    unmap_errors++;
            }
            rbtree_delete(space->region_tree, pos);
            if (r->own_pa && pmm_free_blocks(space->pmm, r->pa_start, r->size))
                pmm_errors++;
            free(r);
        }
        rbtree_destroy(space->region_tree);
        space->region_tree = 0;
    }

    int ret = page_destroy_table(page_tbl);
    free(space->page_tbl);
    space->page_tbl = 0;
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    if (unmap_errors)
        log_warn("%s: page table(%p): destroyed with %u unmap errors\n",
                 __func__, (void*)page_tbl, unmap_errors);
    if (pmm_errors)
        log_warn("%s: page table(%p): destroyed with %u PMM errors\n",
                 __func__, (void*)page_tbl, pmm_errors);
    if (ret)
        log_err("%s: page table(%p): destroy failed (err %d)\n",
                __func__, (void*)page_tbl, ret);
}

void vmm_switch_to_space(vmm_space* space)
{
    if (!space || !space->page_tbl)
        return;

    page_load_table(space->page_tbl);
}

void* vmm_alloc_and_map_pages(vmm_space* space, u32 page_cnt, u32 flags)
{
    u32 pa = 0;
    int ret = 0;
    u32 size = 0;
    vmm_region* region = 0;

    if (!space || !space->region_tree)
        return 0;
    if (page_cnt == 0 || !u32_pages_to_bytes(PAGE_SIZE, page_cnt, &size))
        return 0;

    ret = pmm_alloc_blocks(space->pmm, size, &pa);
    if (ret) {
        log_err("%s: page table(%p): map %u pages: no physical memory (err %d)\n",
                __func__, (void*)space->page_tbl, page_cnt, ret);
        return 0;
    }

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    ret = vmm_alloc_region(space, size, flags, &region);
    if (ret) {
        pmm_free_blocks(space->pmm, pa, size);
        spinlock_unlock_irqrestore(&space->sp_lock, eflags);
        log_err("%s: page table(%p): map %u pages: no free VA (err %d)\n",
                __func__, (void*)space->page_tbl, page_cnt, ret);
        return 0;
    }

    for (u32 i = 0; i < region->size / PAGE_SIZE; i++) {
        ret = page_map_4kb(space->page_tbl, region->va_start + i * PAGE_SIZE,
                           pa + i * PAGE_SIZE, flags);
        if (ret) {
            u32 va_start = region->va_start;

            /* roll back */
            for (u32 j = 0; j < i; j++)
                page_unmap_4kb(space->page_tbl, region->va_start + j * PAGE_SIZE);
            vmm_release_region(space, region);
            pmm_free_blocks(space->pmm, pa, size);
            spinlock_unlock_irqrestore(&space->sp_lock, eflags);
            log_warn("%s: page table(%p): map pa(%p) at va(%p): page %u failed, rolled back (err %d)\n",
                     __func__, (void*)space->page_tbl, (void*)(u32)pa,
                     (void*)(u32)va_start, i, ret);
            return 0;
        }
    }

    region->own_pa = 1;
    region->pa_start = pa;
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    return (void*)region->va_start;
}

void* vmm_map_pages(vmm_space* space, u32 pa, u32 page_cnt, u32 flags, int own_pa)
{
    int ret = 0;
    u32 size = 0;
    vmm_region* region = 0;

    if (!space || !space->region_tree)
        return 0;
    if (!pa || !IS_4KB_ALIGN(pa))
        return 0;
    if (page_cnt == 0 || !u32_pages_to_bytes(PAGE_SIZE, page_cnt, &size))
        return 0;
    if (!u32_range_fits(pa, size))
        return 0;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    ret = vmm_alloc_region(space, size, flags, &region);
    if (ret) {
        spinlock_unlock_irqrestore(&space->sp_lock, eflags);
        log_err("%s: page table(%p): map pa(%p) size(0x%x): no free VA (err %d)\n",
                __func__, (void*)space->page_tbl, (void*)(u32)pa, size, ret);
        return 0;
    }

    for (u32 i = 0; i < region->size / PAGE_SIZE; i++) {
        ret = page_map_4kb(space->page_tbl, region->va_start + i * PAGE_SIZE,
                           pa + i * PAGE_SIZE, flags);
        if (ret) {
            u32 va_start = region->va_start;

            /* roll back */
            for (u32 j = 0; j < i; j++)
                page_unmap_4kb(space->page_tbl, region->va_start + j * PAGE_SIZE);
            vmm_release_region(space, region);
            spinlock_unlock_irqrestore(&space->sp_lock, eflags);
            log_warn("%s: page table(%p): map pa(%p) at va(%p): page %u failed, rolled back (err %d)\n",
                     __func__, (void*)space->page_tbl, (void*)(u32)pa,
                     (void*)(u32)va_start, i, ret);
            return 0;
        }
    }

    region->own_pa = own_pa;
    region->pa_start = pa;
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    return (void*)region->va_start;
}

void* vmm_map_pages_at(vmm_space* space, u32 fixed_va,
                       u32 pa, u32 page_cnt, u32 flags, int own_pa)
{
    int ret = 0;
    u32 size = 0;
    vmm_region* region = 0;

    if (!space || !space->region_tree)
        return 0;
    if (!pa || !IS_4KB_ALIGN(pa))
        return 0;
    if (page_cnt == 0 || !u32_pages_to_bytes(PAGE_SIZE, page_cnt, &size))
        return 0;
    if (!u32_range_fits(pa, size))
        return 0;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    ret = vmm_alloc_region_at(space, fixed_va, size, flags, &region);
    if (ret) {
        spinlock_unlock_irqrestore(&space->sp_lock, eflags);
        log_err("%s: page table(%p): map pa(%p) at va(%p): no free VA (err %d)\n",
                __func__, (void*)space->page_tbl, (void*)(u32)pa,
                (void*)(u32)fixed_va, ret);
        return 0;
    }

    for (u32 i = 0; i < region->size / PAGE_SIZE; i++) {
        ret = page_map_4kb(space->page_tbl, region->va_start + i * PAGE_SIZE,
                           pa + i * PAGE_SIZE, flags);
        if (ret) {
            u32 va_start = region->va_start;

            /* roll back */
            for (u32 j = 0; j < i; j++)
                page_unmap_4kb(space->page_tbl, region->va_start + j * PAGE_SIZE);
            vmm_release_region(space, region);
            spinlock_unlock_irqrestore(&space->sp_lock, eflags);
            log_warn("%s: page table(%p): map pa(%p) at va(%p): page %u failed, rolled back (err %d)\n",
                     __func__, (void*)space->page_tbl, (void*)(u32)pa,
                     (void*)(u32)va_start, i, ret);
            return 0;
        }
    }

    region->own_pa = own_pa;
    region->pa_start = pa;
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    return (void*)region->va_start;
}

int vmm_unmap_pages(vmm_space* space, u32 any_va_in_pages)
{
    vmm_region* region = 0;
    int ret = 0;

    if (!space || !space->region_tree)
        return E_INVAL;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    region = vmm_lookup_region(space, any_va_in_pages);
    if (!region) {
        spinlock_unlock_irqrestore(&space->sp_lock, eflags);
        log_warn("%s: page table(%p): unmap va(%p): not found\n",
                 __func__, (void*)space->page_tbl, (void*)(u32)any_va_in_pages);
        return E_NOTFOUND;
    }

    /*
     * An address may already be absent (for example a retried unmap after a
     * PMM failure), so E_NOTFOUND is tolerated.  Any other error means some
     * of the region may still be mapped: keep the region and its physical
     * pages so the caller can retry, instead of handing pages back to the
     * PMM while page tables still point at them.
     */
    int unmap_error = 0;
    u32 failed_va = 0;

    for (u32 i = 0; i < region->size / PAGE_SIZE; i++) {
        u32 va = region->va_start + i * PAGE_SIZE;

        ret = page_unmap_4kb(space->page_tbl, va);
        if (ret && ret != E_NOTFOUND) {
            unmap_error = ret;
            failed_va = va;
            break;
        }
    }

    if (unmap_error) {
        spinlock_unlock_irqrestore(&space->sp_lock, eflags);
        log_err("%s: page table(%p): unmap va(%p) failed (err %d), region kept\n",
                __func__, (void*)space->page_tbl, (void*)(u32)failed_va,
                unmap_error);
        return unmap_error;
    }

    if (region->own_pa) {
        ret = pmm_free_blocks(space->pmm, region->pa_start, region->size);
        if (ret) {
            u32 pa = region->pa_start;
            u32 size = region->size;

            spinlock_unlock_irqrestore(&space->sp_lock, eflags);
            log_err("%s: page table(%p): free pa(%p) size(0x%x) failed (err %d), region kept\n",
                    __func__, (void*)space->page_tbl, (void*)(u32)pa, size, ret);
            return ret;
        }
    }

    ret = vmm_release_region(space, region);
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    if (ret)
        log_err("%s: page table(%p): release region failed (err %d)\n",
                __func__, (void*)space->page_tbl, ret);

    return ret;
}

int vmm_va_to_pa(vmm_space* space, u32 va, u32* out_pa, u32* out_size)
{
    vmm_region* region = 0;

    if (!space || !space->region_tree)
        return E_INVAL;
    if (!out_pa || !out_size)
        return E_INVAL;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);

    region = vmm_lookup_region(space, va);
    if (!region) {
        spinlock_unlock_irqrestore(&space->sp_lock, eflags);
        return E_INVAL;
    }

    *out_pa = region->pa_start + (va - region->va_start);
    *out_size = region->size + (va - region->va_start);
    
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    return 0;
}
