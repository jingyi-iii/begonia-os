#ifndef PMM_H
#define PMM_H

#include "typedefs.h"
#include "list.h"

/*
 * One physical memory range under the PMM, tracked by a bitmap with
 * block_size granularity.  magic marks a live region: a stale or corrupted
 * pointer is rejected before its fields are trusted.
 */
typedef struct pmm_region {
    u32       pa_start;
    u32       pa_total_size;

    u8*       bitmap;
    u32       block_size;

    u8        magic[4];         /* PMM_REGION_MAGIC, not NUL terminated */
    list_node this_node;
} pmm_region;

// reserve a physical memory range for the PMM to manage
pmm_region* pmm_reserve     (u32 paddr, u32 total_size, u32 block_size);

// release a previously reserved physical memory range
int         pmm_release     (pmm_region* region);

// allocate a physical memory range of the given size from the PMM
int         pmm_alloc_blocks(pmm_region* region, u32 size, u8** out_pa);

// free a previously allocated physical memory range back to the PMM
int         pmm_free_blocks (pmm_region* region, u8* pa, u32 size);

#endif /* PMM_H */
