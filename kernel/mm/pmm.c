#include "mm/pmm.h"
#include "string.h"
#include "log.h"
#include "spinlock.h"
#include "heap.h"
#include "bitmap.h"
#include "page.h"        /* pa_to_va: physical -> kernel window */
#include "errno.h"

/* Three bytes of marker, compared over the whole magic[] field: a space
 * pointer that was released or never reserved cannot pass this. */
#define PMM_SPACE_MAGIC "PMS"

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

pmm_space* pmm_reserve(u32 paddr, u32 total_size, u32 block_size)
{
    u32 eflags = 0;
    u32 nblocks = 0;
    u8* bitmap = 0;

    if (!total_size || !block_size || (total_size % block_size) ||
        (total_size > 0xFFFFFFFF - paddr)) {
        log_err("%s: physical range(%p): invalid parameters (size 0x%x, block 0x%x)\n",
                __func__, (void*)paddr, total_size, block_size);
        return 0;
    }

    nblocks = bitmap_block_count(total_size, block_size);
    bitmap = (u8*)malloc(bitmap_byte_count(nblocks));
    if (!bitmap) {
        log_err("%s: physical range(%p): no memory for the bitmap\n",
                __func__, (void*)paddr);
        return 0;
    }

    pmm_space* space = (pmm_space*)malloc(sizeof(pmm_space));
    if (!space) {
        free(bitmap);
        log_err("%s: physical range(%p): no memory for the space\n",
                __func__, (void*)paddr);
        return 0;
    }

    space->pa_start = paddr;
    space->pa_total_size = total_size;
    space->bitmap = bitmap;
    space->block_size = block_size;
    memcpy(space->magic, PMM_SPACE_MAGIC, sizeof(space->magic));

    eflags = spinlock_lock_irqsave(&pmm_lock);
    list_add(&space->this_node, &spaces);
    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    log_success("%s: pmm space(%p): reserved pa(%p) size(0x%x) block(0x%x)\n",
                __func__, (void*)space, (void*)paddr, total_size, block_size);

    return space;
}

int pmm_release(pmm_space* target)
{
    if (!target)
        return E_INVAL;

    u8 found = 0;
    pmm_space* space = 0;

    u32 eflags = spinlock_lock_irqsave(&pmm_lock);
    list_for_each_safe(node, n, &spaces) {
        space = list_entry(node, pmm_space, this_node);
        if (space == target) {
            list_del(node);
            memset(space->magic, 0, sizeof(space->magic));
            found = 1;
            break;
        }
    }
    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    if (found) {
        log_success("%s: pmm space(%p): released pa(%p)\n",
                    __func__, (void*)space, (void*)space->pa_start);

        free(space->bitmap);
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
                for (u32 j = start_block; j < start_block + alloc_blocks; j++) {
                    bitmap_set(space->bitmap, j);
                }

                /* pa_start is physical; pa_to_va reaches it through the
                 * kernel high window (the first 256 MiB of RAM). */
                memset(pa_to_va(space->pa_start + start_block * space->block_size),
                       0, alloc_blocks * space->block_size);
                *out_pa = start_block * space->block_size + space->pa_start;
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

int pmm_free_blocks(pmm_space* space, u32 pa, u32 size)
{
    if (!space || !pa || size == 0)
        return E_NOMEM_FREE;

    u32 eflags = spinlock_lock_irqsave(&pmm_lock);
    if (!space_is_live(space)) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        log_err("%s: pmm space(%p): stale or released space\n",
                __func__, (void*)space);
        return E_NODEV;
    }

    u32 off  = pa - space->pa_start;
    if (pa < space->pa_start || off >= space->pa_total_size ||
        off % space->block_size) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        log_err("%s: pmm space(%p): free pa(%p) size(0x%x) outside space\n",
                __func__, (void*)space, (void*)pa, size);
        return E_NOMEM_FREE;
    }

    u32 start_block_nr = off / space->block_size;
    u32 requested_blocks = bitmap_block_count(size, space->block_size);
    u32 free_blocks = requested_blocks;
    u32 total_blocks = bitmap_block_count(space->pa_total_size, space->block_size);

    if (free_blocks > total_blocks - start_block_nr)
        free_blocks = total_blocks - start_block_nr;

    for (u32 i = start_block_nr; i < start_block_nr + free_blocks; i++) {
        if (bitmap_test(space->bitmap, i))
            bitmap_clear(space->bitmap, i);
    }
    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    if (free_blocks < requested_blocks)
        log_warn("%s: pmm space(%p): free pa(%p) size(0x%x): clamped to %u of %u blocks\n",
                 __func__, (void*)space, (void*)pa, size,
                 free_blocks, requested_blocks);

    return 0;
}
