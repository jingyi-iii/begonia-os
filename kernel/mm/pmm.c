#include "mm/pmm.h"
#include "string.h"
#include "log.h"
#include "spinlock.h"
#include "heap.h"
#include "bitmap.h"
#include "errno.h"

#define PMB_MAGIC "PMB"

static DECLARE_HEAD_NODE(pmbs);
static DECLARE_SPINLOCK(pmm_lock);

pmb* pmm_reserve(u32 paddr, u32 total_size, u32 block_size)
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
        log_err("%s: failed to allocate memory for bitmap\n", __func__);
        return 0;
    }

    pmb* new_pmb = (pmb*)malloc(sizeof(pmb));
    if (!new_pmb) {
        free(bitmap);
        log_err("%s: failed to allocate memory for pmb\n", __func__);
        return 0;
    }

    new_pmb->pa_start = paddr;
    new_pmb->pa_total_size = total_size;
    new_pmb->bitmap = bitmap;
    new_pmb->block_size = block_size;
    memcpy(new_pmb->magic, PMB_MAGIC, 4);

    eflags = spinlock_lock_irqsave(&pmm_lock);
    list_add(&new_pmb->this_node, &pmbs);
    spinlock_unlock_irqrestore(&pmm_lock, eflags);
    log("%s: reserved physical memory block at 0x%x, size %u bytes, block size %u bytes\n",
        __func__, paddr, total_size, block_size);

    return new_pmb;
}

int pmm_release(pmb* p)
{
    if (!p)
        return E_INVAL;

    u8 found = 0;
    pmb* target = 0;

    u32 eflags = spinlock_lock_irqsave(&pmm_lock);
    list_for_each_safe(node, n, &pmbs) {
        target = list_entry(node, pmb, this_node);
        if (target == p) {
            list_del(node);
            memset(target->magic, 0, sizeof(target->magic));
            found = 1;
            break;
        }
    }
    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    if (found) {
        log("%s: released physical memory block at 0x%x\n",
            __func__, target->pa_start);

        free(target->bitmap);
        free(target);
        return 0;
    }

    return E_INVAL;
}

int pmm_alloc_blocks(pmb* p, u32 size, u8** out_pa)
{
    u32 start_block = 0;
    u32 found_blocks = 0;
    u32 alloc_blocks = 0;
    u32 total_blocks = 0;
    u32 eflags = 0;

    if (!p || size == 0 || !out_pa)
        return E_INVAL;
    *out_pa = 0;

    eflags = spinlock_lock_irqsave(&pmm_lock);
    if (strcmp((const char*)p->magic, PMB_MAGIC) != 0) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        return E_NODEV;
    }

    alloc_blocks = bitmap_block_count(size, p->block_size);
    total_blocks = bitmap_block_count(p->pa_total_size, p->block_size);

    for (u32 i = 0; i < total_blocks; i++) {
        if (!bitmap_test(p->bitmap, i)) {
            if (found_blocks == 0)
                start_block = i;

            found_blocks++;
            if (found_blocks >= alloc_blocks) {
                for (u32 j = start_block; j < start_block + alloc_blocks; j++) {
                    bitmap_set(p->bitmap, j);
                }

                memset((void*)(p->pa_start + start_block * p->block_size),
                       0, alloc_blocks * p->block_size);
                *out_pa = (u8*)(start_block * p->block_size + p->pa_start);
                spinlock_unlock_irqrestore(&pmm_lock, eflags);
                return 0;
            }
        } else {
            found_blocks = 0;
        }
    }

    spinlock_unlock_irqrestore(&pmm_lock, eflags);
    log_err("%s: out of memory for %u blocks!", __func__, alloc_blocks);
    return E_NOMEM;
}

int pmm_free_blocks(pmb* p, u8* pa, u32 size)
{
    if (!p || !pa || size == 0)
        return E_NOMEM_FREE;

    u32 eflags = spinlock_lock_irqsave(&pmm_lock);
    if (strcmp((const char*)p->magic, PMB_MAGIC) != 0) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        return E_NODEV;
    }

    u32 addr = (u32)pa;
    u32 off  = addr - p->pa_start;
    if (addr < p->pa_start || off >= p->pa_total_size || off % p->block_size) {
        spinlock_unlock_irqrestore(&pmm_lock, eflags);
        return E_NOMEM_FREE;
    }

    u32 start_block_nr = off / p->block_size;
    u32 free_blocks = bitmap_block_count(size, p->block_size);
    u32 total_blocks = bitmap_block_count(p->pa_total_size, p->block_size);

    if (free_blocks > total_blocks - start_block_nr)
        free_blocks = total_blocks - start_block_nr;

    for (u32 i = start_block_nr; i < start_block_nr + free_blocks; i++) {
        if (bitmap_test(p->bitmap, i))
            bitmap_clear(p->bitmap, i);
    }
    spinlock_unlock_irqrestore(&pmm_lock, eflags);

    return 0;
}
