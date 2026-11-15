#ifndef PMM_H
#define PMM_H

#include "typedefs.h"
#include "list.h"

typedef struct physical_memory_block {
    u32       pa_start;
    u32       pa_total_size;
    
    u8*       bitmap;
    u32       block_size;

    u8        magic[4]; // "PMB\0"
    list_node this_node;
} pmb;

// reserve a physical memory range for the PMM to manage
pmb* pmm_reserve     (u32 paddr, u32 total_size, u32 block_size);          

// release a previously reserved physical memory range
int  pmm_release     (pmb* p);

// allocate a physical memory range of the given size from the PMM.
int  pmm_alloc_blocks(pmb* p, u32 size, u8** out_pa);

// free a previously allocated physical memory range back to the PMM
int  pmm_free_blocks (pmb* p, u8* pa, u32 size);

#endif /* PMM_H */
