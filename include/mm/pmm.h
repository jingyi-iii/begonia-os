#ifndef PMM_H
#define PMM_H

#include "typedefs.h"
#include "list.h"

/*
 * One physical memory range under the PMM, tracked by a bitmap with
 * block_size granularity.  magic marks a live space: a stale or corrupted
 * pointer is rejected before its fields are trusted.
 */
typedef struct pmm_space {
    u32       pa_start;
    u32       pa_total_size;

    u8*       bitmap;
    u8*       refsmap;          /* one byte for a block */
    u32       block_size;

    u8        magic[4];         /* PMM_SPACE_MAGIC, not NUL terminated */
    list_node this_node;
} pmm_space;

// reserve a physical memory range for the PMM to manage
int pmm_create_space (u32 paddr, u32 total_size, u32 block_size, pmm_space** out);

// release a previously reserved physical memory range
int pmm_destroy_space(pmm_space* space);

// allocate a physical memory range of the given size from the PMM
int pmm_alloc_blocks (pmm_space* space, u32 size, u32* out_pa);

// free a previously allocated physical memory range back to the PMM
int pmm_free_blocks  (pmm_space* space, u32 pa, u32 size);

#endif /* PMM_H */
