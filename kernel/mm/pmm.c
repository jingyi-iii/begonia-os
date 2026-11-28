#include "mm/pmm.h"
#include "string.h"
#include "log.h"
#include "spinlock.h"
#include "heap.h"
#include "bitmap.h"
#include "range.h"
#include "page.h"        /* pa_to_va: physical -> kernel window */
#include "errno.h"

/* Three bytes of marker, compared over the whole magic[] field: a space
 * pointer that was released or never reserved cannot pass this. */
#define PMM_SPACE_MAGIC "PMS"

/*
 * kmain() maps the first PMM_KERNEL_WINDOW_LIMIT bytes of physical memory
 * into the kernel high window, and pmm_alloc_blocks() zeroes every block
 * through pa_to_va(), so a managed space has to fit inside it.
 */
#define PMM_KERNEL_WINDOW_LIMIT 0x08000000u

static DECLARE_HEAD_NODE(spaces);
static DECLARE_SPINLOCK(pmm_lock);

static inline int space_is_live(const pmm_space* space)
{
    static const u8 tag[sizeof(space->magic)] = PMM_SPACE_MAGIC;

    for (u32 i = 0; i < sizeof(space->magic); i++) {
        if (space->magic[i] != tag[i])
            return 0;
    }

    return 1;
}

static inline int space_is_active(const pmm_space* space)
{
    if (!space || !space->refsmap)
        return 0;

    u32 nblocks = bitmap_block_count(space->pa_total_size, space->block_size);

    for (u32 i = 0; i < nblocks; i++) {
        if (space->refsmap[i])
            return 1;
    }

    return 0;
}

int pmm_create_space(u32 paddr, u32 total_size, u32 block_size, pmm_space** out)
{
    u32 eflags = 0;
    u32 nblocks = 0;
    u8* bitmap = 0;

    if (!out)
        return E_INVAL;
    *out = 0;

    if (!total_size || !block_size || (total_size % block_size) ||
        (paddr % block_size)) {
        log_err("%s: physical range(%p): invalid parameters (size 0x%x, block 0x%x)\n",
                __func__, (void*)paddr, total_size, block_size);
        return E_INVAL;
    }

    if (!u32_range_within(paddr, total_size, 0, PMM_KERNEL_WINDOW_LIMIT)) {
        log_err("%s: physical range(%p): ends outside the kernel window (limit 0x%x)\n",
                __func__, (void*)paddr, PMM_KERNEL_WINDOW_LIMIT);
        return E_INVAL;
    }

    /* the same physical bytes must never be managed by two spaces */
    pmm_space* other = 0;

    eflags = spinlock_lock_irqsave(&pmm_lock);
    list_for_each(node, &spaces) {
        other = list_entry(node, pmm_space, this_node);
        if (u32_ranges_overlap(paddr, total_size,
                               other->pa_start, other->pa_total_size)) {
            spinlock_unlock_irqrestore(&pmm_lock, eflags);
            log_err("%s: physical range(%p): overlaps pmm space(%p)\n",
                    __func__, (void*)paddr, (void*)other);
            return E_INVAL;
        }
    }
    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    nblocks = bitmap_block_count(total_size, block_size);
    bitmap = (u8*)malloc(bitmap_byte_count(nblocks));
    if (!bitmap) {
        log_err("%s: physical range(%p): no memory for the bitmap\n",
                __func__, (void*)paddr);
        return E_NOMEM;
    }

    u8* refsmap = (u8*)malloc(nblocks);
    if (!refsmap) {
        free(bitmap);
        log_err("%s: physical range(%p): no memory for the refsmap\n",
                __func__, (void*)paddr);
        return E_NOMEM;
    }

    pmm_space* space = (pmm_space*)malloc(sizeof(pmm_space));
    if (!space) {
        free(refsmap);
        free(bitmap);
        log_err("%s: physical range(%p): no memory for the space\n",
                __func__, (void*)paddr);
        return E_NOMEM;
    }

    memset(bitmap, 0, bitmap_byte_count(nblocks));
    memset(refsmap, 0, nblocks);
    memset(space, 0, sizeof(pmm_space));
    space->pa_start = paddr;
    space->pa_total_size = total_size;
    space->block_size = block_size;
    space->bitmap = bitmap;
    space->refsmap = refsmap;
    memcpy(space->magic, PMM_SPACE_MAGIC, sizeof(space->magic));

    eflags = spinlock_lock_irqsave(&pmm_lock);
    list_add(&space->this_node, &spaces);
    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    *out = space;
    log_success("%s: pmm space(%p): reserved pa(%p) size(0x%x) block(0x%x)\n",
                __func__, (void*)space, (void*)paddr, total_size, block_size);

    return 0;
}

int pmm_destroy_space(pmm_space* target)
{
    if (!target)
        return E_INVAL;

    u8 found = 0;
    pmm_space* space = 0;

    u32 eflags = spinlock_lock_irqsave(&pmm_lock);
    list_for_each_safe(node, n, &spaces) {
        space = list_entry(node, pmm_space, this_node);
        if (!space || space != target)
            continue;

        /* check before detaching: a refused destroy must leave it usable */
        if (space_is_active(space)) {
            spinlock_unlock_irqrestore(&pmm_lock, eflags);
            log_err("%s: pmm space(%p): still holds allocated blocks, kept\n",
                    __func__, (void*)space);
            return E_BUSY;
        }

        list_del(node);
        memset(space->magic, 0, sizeof(space->magic));
        found = 1;
        break;
    }
    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    if (found) {
        log_success("%s: pmm space(%p): released pa(%p)\n",
                    __func__, (void*)space, (void*)space->pa_start);

        free(space->bitmap);
        free(space->refsmap);
        free(space);
        return 0;
    }

    log_warn("%s: pmm space(%p): not found\n", __func__, (void*)target);
    return E_INVAL;
}

int pmm_alloc_blocks(pmm_space* space, u32 size, u32* out_pa)
{
    u32 start_block = 0;
    u32 found_blocks = 0;
    u32 alloc_blocks = 0;
    u32 total_blocks = 0;
    u32 eflags = 0;

    if (!space || size == 0 || !out_pa)
        return E_INVAL;
    *out_pa = 0;

    eflags = spinlock_lock_irqsave(&pmm_lock);
    if (!space_is_live(space)) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        log_err("%s: pmm space(%p): stale or released space\n",
                __func__, (void*)space);
        return E_NODEV;
    }

    alloc_blocks = bitmap_block_count(size, space->block_size);
    total_blocks = bitmap_block_count(space->pa_total_size, space->block_size);

    for (u32 i = 0; i < total_blocks; i++) {
        if (!bitmap_test(space->bitmap, i)) {
            if (found_blocks == 0)
                start_block = i;

            found_blocks++;
            if (found_blocks >= alloc_blocks) {
                u32 offset = 0;
                u32 pa = 0;
                u32 size_bytes = 0;

                /*
                 * pmm_create_space() keeps every space inside the kernel
                 * window, so this cannot fail; still compute the range with
                 * the checked helpers instead of open-coded arithmetic.
                 */
                if (!u32_mul_checked(start_block, space->block_size, &offset) ||
                    !u32_mul_checked(alloc_blocks, space->block_size, &size_bytes) ||
                    !u32_add_checked(space->pa_start, offset, &pa)) {
                    spinlock_unlock_irqrestore(&pmm_lock, eflags);
                    log_err("%s: pmm space(%p): block %u range overflow\n",
                            __func__, (void*)space, start_block);
                    return E_OVERFLOW;
                }

                for (u32 j = start_block; j < start_block + alloc_blocks; j++) {
                    /* a block the bitmap reports free must start at zero */
                    if (space->refsmap[j] != 0) {
                        for (u32 k = start_block; k < j; k++) {
                            space->refsmap[k] = 0;
                            bitmap_clear(space->bitmap, k);
                        }
                        spinlock_unlock_irqrestore(&pmm_lock, eflags);
                        log_err("%s: pmm space(%p): block %u bitmap/refs mismatch\n",
                                __func__, (void*)space, j);
                        return E_FAULT;
                    }

                    bitmap_set(space->bitmap, j);
                    space->refsmap[j] = 1;
                }

                /* pa_to_va reaches the space through the kernel high window */
                memset(pa_to_va(pa), 0, size_bytes);
                *out_pa = pa;
                spinlock_unlock_irqrestore(&pmm_lock, eflags);
                return 0;
            }
        } else {
            found_blocks = 0;
        }
    }

    spinlock_unlock_irqrestore(&pmm_lock, eflags);
    log_err("%s: pmm space(%p): out of memory for %u blocks (size 0x%x)\n",
            __func__, (void*)space, alloc_blocks, size);
    return E_NOMEM;
}

int pmm_ref_blocks(pmm_space* space, u32 pa, u32 size)
{
    if (!space || size == 0)
        return E_INVAL;

    u32 eflags = spinlock_lock_irqsave(&pmm_lock);
    if (!space_is_live(space)) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        log_err("%s: pmm space(%p): stale or released space\n",
                __func__, (void*)space);
        return E_NODEV;
    }

    if (!u32_range_contains(space->pa_start, space->pa_total_size, pa) ||
        ((pa - space->pa_start) % space->block_size)) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        log_err("%s: pmm space(%p): ref pa(%p) size(0x%x) outside space\n",
                __func__, (void*)space, (void*)pa, size);
        return E_INVAL;
    }

    u32 start_block_nr = (pa - space->pa_start) / space->block_size;
    u32 ref_blocks = bitmap_block_count(size, space->block_size);
    u32 total_blocks = bitmap_block_count(space->pa_total_size, space->block_size);

    if (!u32_range_within(start_block_nr, ref_blocks, 0, total_blocks)) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        log_err("%s: pmm space(%p): ref pa(%p) size(0x%x) out of range\n",
                __func__, (void*)space, (void*)pa, size);
        return E_INVAL;
    }

    /* pass 1: validate the whole range before touching any counter */
    for (u32 i = start_block_nr; i < start_block_nr + ref_blocks; i++) {
        if (space->refsmap[i] == 0) {
            spinlock_unlock_irqrestore(&pmm_lock, eflags);
            log_err("%s: pmm space(%p): block %u ref but not allocated\n",
                    __func__, (void*)space, i);
            return E_NOMEM_FREE;
        }

        if (space->refsmap[i] == 0xFF) {
            spinlock_unlock_irqrestore(&pmm_lock, eflags);
            log_err("%s: pmm space(%p): block %u refcount overflow\n",
                    __func__, (void*)space, i);
            return E_LIMIT;
        }
    }

    /* pass 2: take one more reference per block; the bitmap stays untouched */
    for (u32 i = start_block_nr; i < start_block_nr + ref_blocks; i++)
        space->refsmap[i]++;

    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    return 0;
}

int pmm_free_blocks(pmm_space* space, u32 pa, u32 size)
{
    if (!space || size == 0)
        return E_INVAL;

    u32 eflags = spinlock_lock_irqsave(&pmm_lock);
    if (!space_is_live(space)) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        log_err("%s: pmm space(%p): stale or released space\n",
                __func__, (void*)space);
        return E_NODEV;
    }

    if (!u32_range_contains(space->pa_start, space->pa_total_size, pa) ||
        ((pa - space->pa_start) % space->block_size)) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        log_err("%s: pmm space(%p): free pa(%p) size(0x%x) outside space\n",
                __func__, (void*)space, (void*)pa, size);
        return E_INVAL;
    }

    u32 start_block_nr = (pa - space->pa_start) / space->block_size;
    u32 requested_blocks = bitmap_block_count(size, space->block_size);
    u32 free_blocks = requested_blocks;
    u32 total_blocks = bitmap_block_count(space->pa_total_size, space->block_size);

    if (!u32_range_within(start_block_nr, free_blocks, 0, total_blocks)) {
        log_err("%s: pmm space(%p): free pa(%p) size(0x%x) out of range\n",
                __func__, (void*)space, (void*)pa, size);
        return E_INVAL;
    }

    /* pass 1: validate the whole range, so a bad block leaves no partial state */
    for (u32 i = start_block_nr; i < start_block_nr + free_blocks; i++) {
        if (space->refsmap[i] == 0) {
            spinlock_unlock_irqrestore(&pmm_lock, eflags);
            log_err("%s: pmm space(%p): block %u freed but not allocated\n",
                    __func__, (void*)space, i);
            return E_NOMEM_FREE;
        }
    }

    /* pass 2: drop one reference per block */
    for (u32 i = start_block_nr; i < start_block_nr + free_blocks; i++) {
        space->refsmap[i]--;
        if (space->refsmap[i] > 0)
            continue;

        if (bitmap_test(space->bitmap, i))
            bitmap_clear(space->bitmap, i);
    }
    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    return 0;
}
