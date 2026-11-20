#include "mm/pmm.h"
#include "string.h"
#include "log.h"
#include "spinlock.h"
#include "heap.h"
#include "bitmap.h"
#include "page.h"        /* pa_to_va: physical -> kernel window */
#include "errno.h"

/* Three bytes of marker, compared over the whole magic[] field: a region
 * pointer that was released or never reserved cannot pass this. */
#define PMM_REGION_MAGIC "PMR"

static DECLARE_HEAD_NODE(regions);
static DECLARE_SPINLOCK(pmm_lock);

static inline int region_is_live(const pmm_region* region)
{
    static const u8 tag[sizeof(region->magic)] = PMM_REGION_MAGIC;

    for (u32 i = 0; i < sizeof(region->magic); i++) {
        if (region->magic[i] != tag[i])
            return 0;
    }

    return 1;
}

pmm_region* pmm_reserve(u32 paddr, u32 total_size, u32 block_size)
{
    u32 eflags = 0;
    u32 nblocks = 0;
    u8* bitmap = 0;

    if (!total_size || !block_size || (total_size % block_size) ||
        (total_size > 0xFFFFFFFF - paddr)) {
        log_err("%s: invalid parameters\n", __func__);
        return 0;
    }

    nblocks = bitmap_block_count(total_size, block_size);
    bitmap = (u8*)malloc(bitmap_byte_count(nblocks));
    if (!bitmap) {
        log_err("%s: failed to allocate memory for the bitmap\n", __func__);
        return 0;
    }

    pmm_region* region = (pmm_region*)malloc(sizeof(pmm_region));
    if (!region) {
        free(bitmap);
        log_err("%s: failed to allocate memory for the region\n", __func__);
        return 0;
    }

    region->pa_start = paddr;
    region->pa_total_size = total_size;
    region->bitmap = bitmap;
    region->block_size = block_size;
    memcpy(region->magic, PMM_REGION_MAGIC, sizeof(region->magic));

    eflags = spinlock_lock_irqsave(&pmm_lock);
    list_add(&region->this_node, &regions);
    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    log("%s: reserved physical memory region at 0x%x, size %u bytes, block size %u bytes\n",
        __func__, paddr, total_size, block_size);

    return region;
}

int pmm_release(pmm_region* target)
{
    if (!target)
        return E_INVAL;

    u8 found = 0;
    pmm_region* region = 0;

    u32 eflags = spinlock_lock_irqsave(&pmm_lock);
    list_for_each_safe(node, n, &regions) {
        region = list_entry(node, pmm_region, this_node);
        if (region == target) {
            list_del(node);
            memset(region->magic, 0, sizeof(region->magic));
            found = 1;
            break;
        }
    }
    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    if (found) {
        log("%s: released physical memory region at 0x%x\n",
            __func__, region->pa_start);

        free(region->bitmap);
        free(region);
        return 0;
    }

    return E_INVAL;
}

int pmm_alloc_blocks(pmm_region* region, u32 size, u8** out_pa)
{
    u32 start_block = 0;
    u32 found_blocks = 0;
    u32 alloc_blocks = 0;
    u32 total_blocks = 0;
    u32 eflags = 0;

    if (!region || size == 0 || !out_pa)
        return E_INVAL;
    *out_pa = 0;

    eflags = spinlock_lock_irqsave(&pmm_lock);
    if (!region_is_live(region)) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        return E_NODEV;
    }

    alloc_blocks = bitmap_block_count(size, region->block_size);
    total_blocks = bitmap_block_count(region->pa_total_size, region->block_size);

    for (u32 i = 0; i < total_blocks; i++) {
        if (!bitmap_test(region->bitmap, i)) {
            if (found_blocks == 0)
                start_block = i;

            found_blocks++;
            if (found_blocks >= alloc_blocks) {
                for (u32 j = start_block; j < start_block + alloc_blocks; j++) {
                    bitmap_set(region->bitmap, j);
                }

                /* pa_start is physical; pa_to_va reaches it through the
                 * kernel high window (the first 256 MiB of RAM). */
                memset(pa_to_va(region->pa_start + start_block * region->block_size),
                       0, alloc_blocks * region->block_size);
                *out_pa = (u8*)(start_block * region->block_size + region->pa_start);
                spinlock_unlock_irqrestore(&pmm_lock, eflags);
                return 0;
            }
        } else {
            found_blocks = 0;
        }
    }

    spinlock_unlock_irqrestore(&pmm_lock, eflags);
    log_err("%s: out of memory for %u blocks\n", __func__, alloc_blocks);
    return E_NOMEM;
}

int pmm_free_blocks(pmm_region* region, u8* pa, u32 size)
{
    if (!region || !pa || size == 0)
        return E_NOMEM_FREE;

    u32 eflags = spinlock_lock_irqsave(&pmm_lock);
    if (!region_is_live(region)) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        return E_NODEV;
    }

    u32 addr = (u32)pa;
    u32 off  = addr - region->pa_start;
    if (addr < region->pa_start || off >= region->pa_total_size ||
        off % region->block_size) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        return E_NOMEM_FREE;
    }

    u32 start_block_nr = off / region->block_size;
    u32 free_blocks = bitmap_block_count(size, region->block_size);
    u32 total_blocks = bitmap_block_count(region->pa_total_size, region->block_size);

    if (free_blocks > total_blocks - start_block_nr)
        free_blocks = total_blocks - start_block_nr;

    for (u32 i = start_block_nr; i < start_block_nr + free_blocks; i++) {
        if (bitmap_test(region->bitmap, i))
            bitmap_clear(region->bitmap, i);
    }
    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    return 0;
}
