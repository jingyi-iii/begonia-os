#include "mm/vmm.h"
#include "heap.h"
#include "page.h"
#include "errno.h"
#include "log.h"

#define USER_ANON_BASE  (0xD0000000)

/*
 * PA allocator:
 *   - vmm_create: page directory from the reserved paging pool
 *   - vmm_alloc_pages: user pages from the PMM
 * 
 * PA deallocator:
 *   - vmm_destroy: page directory back to the paging pool
 *   - vmm_free_pages: user pages back to the PMM
 *
 * VA allocator:
 *   - vmm_alloc_pages: for user pages
 *   - vmm_mmap_reserve: for mmap
 *
 * VA deallocator:
 *   - vmm_free_pages: for user pages
 *   - vmm_mmap_release: for mmap
 */

static int vmm_rbtree_node_cmp(const rbnode* left, const rbnode* right)
{
    if (!left || !right)
        return -1;

    vmm_region* l = rb_entry(left, vmm_region, node);
    vmm_region* r = rb_entry(right, vmm_region, node);

    if ((u32)l->start_va < (u32)r->start_va)
        return -1;
    else if ((u32)l->start_va > (u32)r->start_va)
        return 1;
    else
        return 0;
}

static int vmm_rbtree_key_cmp(const void* key, const rbnode* node)
{
    if (!node || !key)
        return -1;

    vmm_region* r = rb_entry(node, vmm_region, node);
    if (r->start_va <= key && (u32)key < (u32)r->start_va + r->size)
        return 0;
    else if ((u32)key < (u32)r->start_va)
        return -1;
    else
        return 1;
}

void vmm_switch(vmm_space* space)
{
    if (!space || !space->page_tbl)
        return;

    page_load_table(space->page_tbl);
}

int vmm_create(page_table* new_table, page_table* from_table, pmm_region* pmm, int user_accessible, vmm_space* out)
{
    int ret = 0;
    vmm_space space = {0};

    if (!out || !pmm)
        return E_INVAL;

    space.tree = rbtree_create();
    if (!space.tree)
        return E_NOMEM;

    ret = page_clone_table(new_table, from_table, user_accessible);
    if (ret) {
        rbtree_destroy(space.tree);
        return ret;
    }

    space.page_tbl = new_table;
    space.pmm = pmm;
    spinlock_init(&space.sp_lock);
    *out = space;

    return 0;
}

void vmm_destroy(vmm_space* space)
{
    if (!space || !space->page_tbl)
        return;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    if (space->tree) {
        rbtree_for_each_safe(pos, n, space->tree) {
            vmm_region* r = rb_entry(pos, vmm_region, node);
            for (u32 i = 0; i < r->size / PAGE_SIZE; i++)
                page_unmap_4kb(space->page_tbl,
                               (u8*)r->start_va + i * PAGE_SIZE);
            if (r->own_phys)
                pmm_free_blocks(space->pmm, (u8*)r->pa, r->size);
            rbtree_delete(space->tree, pos);
            free(r);
        }
        rbtree_destroy(space->tree);
        space->tree = 0;
    }

    page_destroy_table(space->page_tbl);
    space->page_tbl = 0;
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);
}

void* vmm_alloc_pages(vmm_space* space, u32 page_cnt, u32 flags)
{
    u32 pa = 0;
    u32 va = 0;
    u8* out_pa = 0;
    int ret = 0;
    vmm_region* region = 0;
    vmm_region* cur = 0;

    if (!space || !space->tree || page_cnt == 0)
        return 0;

    ret = pmm_alloc_blocks(space->pmm, PAGE_SIZE * page_cnt, &out_pa);
    if (ret) {
        log_err("%s: failed to allocate pmm blocks, ret = 0x%x\n", ret);
        return 0;
    }

    pa = (u32)out_pa;
    region = malloc(sizeof(vmm_region));
    if (!region) {
        pmm_free_blocks(space->pmm, out_pa, PAGE_SIZE * page_cnt);
        return 0;
    }

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);

    /* pick a free VA and reserve it atomically with the insert below */
    if (space->tree->root != space->tree->nil) {
        rbtree_for_each(node, space->tree) {
            cur = rb_entry(node, vmm_region, node);

            rbnode* next_node = rbtree_next(space->tree, &cur->node);
            if (next_node) {
                vmm_region* next_region = rb_entry(next_node, vmm_region, node);
                va = (u32)cur->start_va + cur->size;
                va = (va + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                if (page_cnt > (0xffffffff - va) / PAGE_SIZE) {
                    /* overflow */
                    spinlock_unlock_irqrestore(&space->sp_lock, eflags);
                    pmm_free_blocks(space->pmm, out_pa, PAGE_SIZE * page_cnt);
                    free(region);
                    return 0;
                }
                if (va + page_cnt * PAGE_SIZE <= (u32)next_region->start_va)
                    break;
            } else {
                if ((u32)cur->start_va > (0xffffffff - cur->size)) {
                    /* overflow */
                    spinlock_unlock_irqrestore(&space->sp_lock, eflags);
                    pmm_free_blocks(space->pmm, out_pa, PAGE_SIZE * page_cnt);
                    free(region);
                    return 0;
                }

                va = (u32)cur->start_va + cur->size;
                va = (va + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                if (va == 0 || page_cnt > (0xffffffff - va) / PAGE_SIZE) {
                    /* overflow after alignment */
                    spinlock_unlock_irqrestore(&space->sp_lock, eflags);
                    pmm_free_blocks(space->pmm, out_pa, PAGE_SIZE * page_cnt);
                    free(region);
                    return 0;
                }
                break;
            }
        }
    }

    /* tree is empty — anchor the first region at USER_ANON_BASE.
     * NEVER fall back to va = pa: low physical pages live in the kernel's
     * identity band, where a user mapping aliases the identity map — and
     * freeing the region (thread stack exit) would delete the identity
     * PTE of that physical page, so a later pmm_alloc_pages() zeroing of
     * the page faults under this page_tbl (ukernel.md #12). */
    if (space->tree->root == space->tree->nil)
        va = USER_ANON_BASE;

    region->start_va = (void*)va;
    region->size     = PAGE_SIZE * page_cnt;
    region->flags    = flags;
    region->pa       = pa;
    region->own_phys = 1;
    rbtree_insert(space->tree, &region->node, vmm_rbtree_node_cmp);
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    for (u32 i = 0; i < region->size / PAGE_SIZE; i++) {
        if (page_map_4kb(space->page_tbl, (void*)(va + i * PAGE_SIZE),
                         (void*)(pa + i * PAGE_SIZE), flags) != 0) {
            /* roll back: unmap pages mapped so far, free phys, drop region */
            for (u32 j = 0; j < i; j++)
                page_unmap_4kb(space->page_tbl, (void*)(va + j * PAGE_SIZE));
            eflags = spinlock_lock_irqsave(&space->sp_lock);
            rbtree_delete(space->tree, &region->node);
            spinlock_unlock_irqrestore(&space->sp_lock, eflags);
            pmm_free_blocks(space->pmm, out_pa, PAGE_SIZE * page_cnt);
            free(region);
            return 0;
        }
    }
    return (void*)va;
}

void vmm_free_pages(vmm_space* space, void* va)
{
    rbnode* del_node = 0;   /* node captured inside the loop; the loop-scoped
                             * `node` goes out of scope after rbtree_for_each */
    vmm_region* region = 0;
    int found = 0;

    if (!space || !space->tree)
        return;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    rbtree_for_each(node, space->tree) {
        region = rb_entry(node, vmm_region, node);

        if (((u32)region->start_va <= (u32)va) && 
            ((u32)region->start_va + region->size > (u32)va) &&
            region->own_phys) {
            found = 1;
            del_node = node;
            break;
        }
    }

    if (found) {
        for (u32 i = 0; i < region->size / PAGE_SIZE; i++)
            page_unmap_4kb(space->page_tbl,
                           (u8*)region->start_va + i * PAGE_SIZE);
        pmm_free_blocks(space->pmm, (u8*)region->pa, region->size);
        rbtree_delete(space->tree, del_node);
        free(region);
    }
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);
}

static void* vmm_mmap_reserve(vmm_space* space, u32 pa, size_t size,
                                u32 flags, int* found)
{
    u32 va = 0;
    vmm_region* region = 0;

    if (!space || !space->tree || !found)
        return 0;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    *found = 0;

    if (space->tree->root != space->tree->nil) {
        rbtree_for_each(node, space->tree) {
            region = rb_entry(node, vmm_region, node);

            rbnode* next_node = rbtree_next(space->tree, &region->node);
            if (next_node) {
                vmm_region* next_region = rb_entry(next_node, vmm_region, node);
                va = (u32)region->start_va + region->size;
                va = (va + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                if (size > 0xffffffff - (u32)va) {
                    /* overflow */
                    spinlock_unlock_irqrestore(&space->sp_lock, eflags);
                    return 0;
                }
                if (va + size <= (u32)next_region->start_va)
                    break;
            } else {
                if ((u32)region->start_va > (0xffffffff - region->size)) {
                    /* overflow */
                    spinlock_unlock_irqrestore(&space->sp_lock, eflags);
                    return 0;
                }

                va = (u32)region->start_va + region->size;
                va = (va + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                if (va == 0 || size > 0xffffffff - (u32)va) {
                    /* overflow after alignment */
                    spinlock_unlock_irqrestore(&space->sp_lock, eflags);
                    return 0;
                }
                break;
            }
        }
    }

    if (space->tree->root == space->tree->nil)
        va = pa;

    region = malloc(sizeof(vmm_region));
    if (!region) {
        spinlock_unlock_irqrestore(&space->sp_lock, eflags);
        return 0;
    }

    region->start_va = (void*)va;
    region->size     = size;
    region->flags    = flags;
    region->pa       = pa;
    region->own_phys = 0;           /* mmap regions do not own the pages */
    rbtree_insert(space->tree, &region->node, vmm_rbtree_node_cmp);
    *found = 1;
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    return (void*)va;
}

static int vmm_mmap_release(vmm_space* space, void* va, u32 size,
                              void** out_start, u32* out_size)
{
    rbnode* del_node = 0;   /* node captured inside the loop; the loop-scoped
                             * `node` goes out of scope after rbtree_for_each */
    vmm_region* region = 0;
    int found = 0;

    if (!space || !space->tree)
        return EINVAL;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    rbtree_for_each(node, space->tree) {
        region = rb_entry(node, vmm_region, node);
        if (region->own_phys)
            continue;

        if (((u32)region->start_va <= (u32)va) &&
            ((u32)region->start_va + region->size > (u32)va)) {
            found = 1;
            del_node = node;
            break;
        }
    }

    if (found) {
        /*
         * A mapping is released as a whole.  Reject requests that extend
         * beyond the mapped region (or wrap around 32-bit), so a caller
         * can never silently unmap more than it asked for.
         */
        if (size > region->size ||
            (u32)va + size < (u32)va ||
            (u32)va + size > (u32)region->start_va + region->size) {
            spinlock_unlock_irqrestore(&space->sp_lock, eflags);
            return EINVAL;
        }

        if (out_start)
            *out_start = region->start_va;
        if (out_size)
            *out_size = region->size;
        rbtree_delete(space->tree, del_node);
        free(region);
    }
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    return found ? 0 : EINVAL;
}

int vmm_lookup_region(vmm_space* space, u32 va, u32* out_pa, u32* out_pa_size,
                      void** out_start_va)
{
    vmm_region* region = 0;
    int found = 0;

    if (!space || !out_pa || !out_pa_size)
        return EINVAL;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    rbtree_for_each(node, space->tree) {
        region = rb_entry(node, vmm_region, node);

        if (((u32)region->start_va <= (u32)va) &&
            ((u32)region->start_va + region->size > (u32)va)) {
            found = 1;
            *out_pa = region->pa;
            *out_pa_size = region->size;
            if (out_start_va)
                *out_start_va = region->start_va;
            break;
        }
    }
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    return found ? 0 : EINVAL;
}

void* vmm_map_memory(vmm_space* space, u32 phys_addr, size_t size, u32 flags)
{
    u32 aligned_pa = phys_addr & ~(PAGE_SIZE - 1);
    u32 offset = phys_addr - aligned_pa;
    size_t aligned_sz = 0;
    void* va = 0;
    int found = 0;

    if (!space)
        return VMM_ERR_PTR(EINVAL);


    /* reject empty mappings and 32-bit overflow of the aligned size */
    if (size == 0 || size > (size_t)-1 - offset - (PAGE_SIZE - 1))
        return VMM_ERR_PTR(EINVAL);

    aligned_sz = (size + offset + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);

    va = vmm_mmap_reserve(space, aligned_pa, aligned_sz, flags, &found);
    if (!found)
        return VMM_ERR_PTR(E_NOMEM);

    for (u32 i = 0; i < aligned_sz / PAGE_SIZE; i++) {
        if (page_map_4kb((void*)space->page_tbl, (void*)(va + i * PAGE_SIZE),
                         (void*)(aligned_pa + i * PAGE_SIZE), flags) != 0) {
            /* roll back: unmap pages mapped so far, drop the reserved region */
            for (u32 j = 0; j < i; j++)
                page_unmap_4kb(space->page_tbl, (void*)(va + j * PAGE_SIZE));
            vmm_mmap_release(space, va, 0, 0, 0);
            return VMM_ERR_PTR(E_NOMEM);
        }
    }

    return (void*)((u8*)va + offset);
}

int vmm_map_fixed(vmm_space* space, u32 phys_addr, void* vaddr, size_t size,
                  u32 flags, int own_phys)
{
    vmm_region* region = 0;
    u32 vs = (u32)vaddr;
    u32 npages = size / PAGE_SIZE;

    if (!space)
        return E_INVAL;
    if ((phys_addr & (PAGE_SIZE - 1)) || (vs & (PAGE_SIZE - 1)))
        return E_INVAL;
    if (size == 0 || (size & (PAGE_SIZE - 1)))
        return E_INVAL;

    region = malloc(sizeof(vmm_region));
    if (!region)
        return E_NOMEM;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    /* Refuse to overlap an existing region. */
    rbtree_for_each(node, space->tree) {
        vmm_region* r = rb_entry(node, vmm_region, node);
        u32 rs = (u32)r->start_va;
        if (vs < rs + r->size && rs < vs + size) {
            spinlock_unlock_irqrestore(&space->sp_lock, eflags);
            free(region);
            return E_EXISTS;
        }
    }

    region->start_va = vaddr;
    region->size     = size;
    region->flags    = flags;
    region->pa       = phys_addr;
    region->own_phys = own_phys;   /* 1: ELF loader pages / 0: MMIO alias */
    rbtree_insert(space->tree, &region->node, vmm_rbtree_node_cmp);
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    for (u32 i = 0; i < npages; i++) {
        if (page_map_4kb(space->page_tbl,
                         (u8*)vaddr + i * PAGE_SIZE,
                         (void*)(phys_addr + i * PAGE_SIZE), flags) != 0) {
            /* roll back: unmap what was mapped, drop the region */
            for (u32 j = 0; j < i; j++)
                page_unmap_4kb(space->page_tbl,
                               (u8*)vaddr + j * PAGE_SIZE);
            eflags = spinlock_lock_irqsave(&space->sp_lock);
            rbtree_delete(space->tree, &region->node);
            spinlock_unlock_irqrestore(&space->sp_lock, eflags);
            free(region);
            return E_LIMIT;
        }
    }

    return 0;
}

int vmm_unmap_memory(vmm_space* space, void* virt_addr, size_t size)
{
    u32 pa = 0;
    u32 pa_size = 0;
    u32 region_size = 0;
    void* va = 0;
    int ret = 0;

    if (!space)
        return EINVAL;

    ret = vmm_lookup_region(space, (u32)virt_addr, &pa, &pa_size, 0);
    if (ret)
        return E_NOTFOUND;

    /*
     * Only unmap when a matching mmap region actually exists.  The
     * requested range is validated against the region inside
     * vmm_mmap_release so a bad size cannot silently unmap more.
     */
    if (vmm_mmap_release(space, virt_addr, (u32)size,
                         &va, &region_size) != 0)
        return EINVAL;

    for (u32 i = 0; i < region_size / PAGE_SIZE; i++)
        page_unmap_4kb(space->page_tbl,
                       (u8*)va + i * PAGE_SIZE);

    return 0;
}

int vmm_unmap_fixed(vmm_space* space, void* vaddr, size_t size)
{
    vmm_region* region = 0;
    u32 pa = 0;
    u32 pa_size = 0;
    u32 own = 0;
    u32 region_size = 0;
    void* start_va = 0;
    int ret;

    if (!space || !space->tree)
        return EINVAL;

    /* Find the region + its physical range first (any own_phys), then
     * cap-check exactly like vmm_unmap_memory(). */
    ret = vmm_lookup_region(space, (u32)vaddr, &pa, &pa_size, &start_va);
    if (ret)
        return E_NOTFOUND;


    /* Re-find + validate + delete the region under the lock. */
    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    rbtree_for_each(node, space->tree) {
        vmm_region* r = rb_entry(node, vmm_region, node);
        if ((u32)r->start_va <= (u32)vaddr &&
            (u32)r->start_va + r->size > (u32)vaddr) {
            if (size > r->size ||
                (u32)vaddr + size < (u32)vaddr ||
                (u32)vaddr + size > (u32)r->start_va + r->size) {
                spinlock_unlock_irqrestore(&space->sp_lock, eflags);
                return EINVAL;
            }
            region = r;
            break;
        }
    }
    if (!region) {
        spinlock_unlock_irqrestore(&space->sp_lock, eflags);
        return E_NOTFOUND;
    }

    start_va    = region->start_va;
    region_size = region->size;
    pa          = region->pa;
    own         = region->own_phys;
    rbtree_delete(space->tree, &region->node);
    free(region);
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    /* Drop the PTEs.  Only own_phys = 1 mappings return their pages to
     * the PMM; MMIO windows (own_phys = 0) are pure aliases. */
    for (u32 i = 0; i < region_size / PAGE_SIZE; i++)
        page_unmap_4kb(space->page_tbl,
                       (u8*)start_va + i * PAGE_SIZE);
    if (own)
        pmm_free_blocks(space->pmm, (u8*)pa, region_size);

    return 0;
}

u32 vmm_space_va_to_pa(vmm_space* space, u32 va)
{
    u32 pa = 0;
    if (!space || !space->tree)
        return 0;

    u32 eflags = spinlock_lock_irqsave(&space->sp_lock);
    rbnode* node = rbtree_search(space->tree, (void*)va, vmm_rbtree_key_cmp);
    if (node) {
        vmm_region* region = rb_entry(node, vmm_region, node);
        pa = region->pa + ((u32)va - (u32)region->start_va);
    }
    spinlock_unlock_irqrestore(&space->sp_lock, eflags);

    return pa;
}

u32 vmm_va_to_pa(vmm_space* space, u32 va)
{
    if (!space)
        return 0;

    return vmm_space_va_to_pa(space, va);
}

