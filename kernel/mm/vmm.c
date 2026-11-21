#include "mm/vmm.h"
#include "heap.h"
#include "page.h"
#include "errno.h"
#include "log.h"
#include "string.h"

typedef struct vmm_region {
    rbnode      node;
    u32         va_start;
    u32         size;
    u32         flags;
    u32         pa_start;
    int         own_pa;
} vmm_region;

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
    if ((u32)r->va_start <= (u32)key && (u32)key < (u32)r->va_start + r->size)
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
        if (va < r->va_start + r->size)
            return 0;                       /* va starts inside the region below */
    }

    if (above) {
        vmm_region* r = rb_entry(above, vmm_region, node);
        if (r->va_start < va + size)
            return 0;                       /* the range reaches the region above */
    }

    return 1;
}

static int vmm_get_available_va(vmm_space* space, u32 size, u32* out_va)
{
    if (!space || !out_va)
        return E_INVAL;

    if (rbtree_is_empty(space->region_tree)) {
        *out_va = USER_BASE_VA;
        return 0;
    }

    rbnode* first_node = rbtree_first(space->region_tree);
    vmm_region* first = rb_entry(first_node, vmm_region, node);
    if (USER_BASE_VA + size <= first->va_start) {
        *out_va = USER_BASE_VA;
        return 0; 
    }

    rbtree_for_each(curr_node, space->region_tree) {
        vmm_region* curr = rb_entry(curr_node, vmm_region, node);
        u32 curr_va = PAGE_ALIGN(curr->va_start);

        rbnode* next_node = rbtree_next(space->region_tree, curr_node);
        if (next_node) {
            vmm_region* next = rb_entry(next_node, vmm_region, node);
            u32 next_va = PAGE_ALIGN(next->va_start);
            
            if (curr_va > KERNEL_BASE_VA || curr->size > KERNEL_BASE_VA - curr_va)
                continue;
            if (size > KERNEL_BASE_VA - curr_va - curr->size)
                continue;

            if (curr_va + curr->size + size <= next_va) {
                *out_va = curr_va + curr->size;
                return 0;
            }
        } else {
            if (curr_va > KERNEL_BASE_VA || curr->size > KERNEL_BASE_VA - curr_va)
                continue;
            if (size > KERNEL_BASE_VA - curr_va - curr->size)
                continue;

            if (curr_va + curr->size + size <= KERNEL_BASE_VA) {
                *out_va = curr_va + curr->size;
                return 0;  
            }
        }
    }

    return E_NOMEM;
}

static int vmm_alloc_region(vmm_space* space, size_t size,
                            u32 flags, vmm_region** out)
{

    vmm_region* region = 0;
    u32 va = 0;

    if (!space || !space->region_tree || !out || !IS_4KB_ALIGN(size))
        return E_INVAL;

    if (vmm_get_available_va(space, size, &va))
        return E_NOMEM;

    /* the reserved user window is [USER_BASE_VA, KERNEL_BASE_VA) */
    if (va < USER_BASE_VA || va >= KERNEL_BASE_VA || size > KERNEL_BASE_VA - va)
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

    if (!space || !space->region_tree || !out || !IS_4KB_ALIGN(size))
        return E_INVAL;

    /* a user mapping must be page aligned, non-empty, and inside the window */
    if (!size || (size & (PAGE_SIZE - 1)) || (va & (PAGE_SIZE - 1)))
        return E_INVAL;
    if (va < USER_BASE_VA || va >= KERNEL_BASE_VA)
        return E_INVAL;
    if (size > KERNEL_BASE_VA - va)
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

    if (!space)
        return 0;

    node = rbtree_search(space->region_tree, (void*)va, vmm_region_key_cmp);
    if (node)
        out = rb_entry(node, vmm_region, node);

    return out;
}

int vmm_create_space(page_table* new_table, page_table* from_table,
                     pmm_region* pmm, vmm_space* out)
{
    int ret = 0;
    vmm_space space = {0};

    if (!out || !pmm)
        return E_INVAL;

    space.region_tree = rbtree_create();
    if (!space.region_tree)
        return E_NOMEM;

    ret = page_clone_table(new_table, from_table);
    if (ret) {
        rbtree_destroy(space.region_tree);
        return ret;
    }

    space.page_tbl = new_table;
    space.pmm = pmm;
    spinlock_init(&space.sp_lock);
    *out = space;

    return 0;
}

void vmm_destroy_space(vmm_space* space)
{
    if (!space || !space->page_tbl)
        return;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    if (space->region_tree) {
        rbtree_for_each_safe(pos, n, space->region_tree) {
            vmm_region* r = rb_entry(pos, vmm_region, node);
            for (u32 i = 0; i < r->size / PAGE_SIZE; i++)
                page_unmap_4kb(space->page_tbl, r->va_start + i * PAGE_SIZE);
            rbtree_delete(space->region_tree, pos);
            if (r->own_pa)
                pmm_free_blocks(space->pmm, r->pa_start, r->size);
            free(r);
        }
        rbtree_destroy(space->region_tree);
        space->region_tree = 0;
    }

    page_destroy_table(space->page_tbl);
    space->page_tbl = 0;
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);
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
    vmm_region* region = 0;

    if (!space || !space->region_tree || page_cnt == 0)
        return 0;

    ret = pmm_alloc_blocks(space->pmm, PAGE_SIZE * page_cnt, &pa);
    if (ret) {
        log_err("%s: failed to allocate pmm blocks, ret = 0x%x\n",
                __func__, ret);
        return 0;
    }

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    ret = vmm_alloc_region(space, PAGE_SIZE * page_cnt, flags, &region);
    if (ret) {
        pmm_free_blocks(space->pmm, pa, PAGE_SIZE * page_cnt);
        spinlock_unlock_irqrestore(&space->sp_lock, eflags);
        log_err("%s: failed to allocate vmm region, ret = 0x%x\n",
                __func__, ret);
        return 0;
    }

    for (u32 i = 0; i < region->size / PAGE_SIZE; i++) {
        ret = page_map_4kb(space->page_tbl, region->va_start + i * PAGE_SIZE,
                           pa + i * PAGE_SIZE, flags);
        if (ret) {
            /* roll back */
            for (u32 j = 0; j < i; j++)
                page_unmap_4kb(space->page_tbl, region->va_start + j * PAGE_SIZE);
            vmm_release_region(space, region);
            pmm_free_blocks(space->pmm, pa, PAGE_SIZE * page_cnt);
            spinlock_unlock_irqrestore(&space->sp_lock, eflags);
            return 0;
        }
    }

    region->own_pa = 1;
    region->pa_start = pa;
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    return (void*)region->va_start;
}

void* vmm_map_pages(vmm_space* space, u32 pa, u32 page_cnt, u32 flags)
{
    int ret = 0;
    vmm_region* region = 0;

    if (!space || !space->region_tree || page_cnt == 0)
        return 0;
    if (!pa || !IS_4KB_ALIGN(pa))
        return 0;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    ret = vmm_alloc_region(space, PAGE_SIZE * page_cnt, flags, &region);
    if (ret) {
        log_err("%s: failed to allocate vmm region, ret = 0x%x\n",
                __func__, ret);
        spinlock_unlock_irqrestore(&space->sp_lock, eflags);
        return 0;
    }

    for (u32 i = 0; i < region->size / PAGE_SIZE; i++) {
        ret = page_map_4kb(space->page_tbl, region->va_start + i * PAGE_SIZE,
                           pa + i * PAGE_SIZE, flags);
        if (ret) {
            /* roll back */
            for (u32 j = 0; j < i; j++)
                page_unmap_4kb(space->page_tbl, region->va_start + j * PAGE_SIZE);
            vmm_release_region(space, region);
            spinlock_unlock_irqrestore(&space->sp_lock, eflags);
            return 0;
        }
    }

    region->own_pa = 0;
    region->pa_start = pa;
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    return (void*)region->va_start;
}

void* vmm_map_pages_at(vmm_space* space, u32 fixed_va,
                       u32 pa, u32 page_cnt, u32 flags)
{
    int ret = 0;
    vmm_region* region = 0;

    if (!space || !space->region_tree || page_cnt == 0)
        return 0;
    if (!pa || !IS_4KB_ALIGN(pa))
        return 0;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    ret = vmm_alloc_region_at(space, fixed_va,
                              PAGE_SIZE * page_cnt, flags, &region);
    if (ret) {
        spinlock_unlock_irqrestore(&space->sp_lock, eflags);
        log_err("%s: failed to allocate vmm region, ret = 0x%x\n",
                __func__, ret);
        return 0;
    }

    for (u32 i = 0; i < region->size / PAGE_SIZE; i++) {
        ret = page_map_4kb(space->page_tbl, region->va_start + i * PAGE_SIZE,
                           pa + i * PAGE_SIZE, flags);
        if (ret) {
            /* roll back */
            for (u32 j = 0; j < i; j++)
                page_unmap_4kb(space->page_tbl, region->va_start + j * PAGE_SIZE);
            vmm_release_region(space, region);
            spinlock_unlock_irqrestore(&space->sp_lock, eflags);
            return 0;
        }
    }

    region->own_pa = 0;
    region->pa_start = pa;
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    return (void*)region->va_start;
}

void vmm_unmap_pages(vmm_space* space, u32 any_va_in_pages)
{
    vmm_region* region = 0;

    if (!space || !space->region_tree)
        return;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    region = vmm_lookup_region(space, any_va_in_pages);
    if (!region) {
        spinlock_unlock_irqrestore(&space->sp_lock, eflags);
        return;
    }

    for (u32 i = 0; i < region->size / PAGE_SIZE; i++) {
        page_unmap_4kb(space->page_tbl,
                       region->va_start + i * PAGE_SIZE);
    }
    if (region->own_pa)
        pmm_free_blocks(space->pmm, region->pa_start, region->size);

    vmm_release_region(space, region);
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);
}
