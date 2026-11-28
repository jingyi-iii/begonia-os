#ifndef VMM_H
#define VMM_H

#include "typedefs.h"
#include "page.h"
#include "rbtree.h"
#include "spinlock.h"
#include "mm/pmm.h"

#define VMM_REGION_FLAGS_SHARED (1 << 1)

typedef struct vmm_region {
    rbnode      node;
    u32         va_start;
    u32         pa_start;
    u32         size;
    u32         page_flags;
    u32         region_flags;
} vmm_region;

typedef struct vmm_space {
    rbtree*     region_tree;
    page_table* page_tbl;
    pmm_space*  pmm;
    spinlock    sp_lock;
} vmm_space;

int   vmm_create_space        (page_table* table, pmm_space* pmm, vmm_space* out);
int   vmm_clone_space         (page_table* new_table, page_table* from_table,
                               pmm_space* pmm, vmm_space* out);
void  vmm_destroy_space       (vmm_space* space);
void  vmm_switch_to_space     (vmm_space* space);

void* vmm_alloc_and_map_pages (vmm_space* space, u32 page_cnt, u32 page_flags);
int   vmm_unmap_and_free_pages(vmm_space* space, u32 any_va_in_pages);

void* vmm_map_pages           (vmm_space* space, u32 pa, u32 page_cnt, u32 page_flags);
void* vmm_map_pages_at        (vmm_space* space, u32 va, u32 pa, u32 page_cnt, u32 page_flags);
int   vmm_unmap_pages         (vmm_space* space, u32 any_va_in_pages);

void* vmm_map_pages_shared    (vmm_space* space, u32 pa, u32 page_cnt, u32 page_flags);
int   vmm_unmap_pages_shared  (vmm_space* space, u32 any_va_in_pages);

int   vmm_va_to_pa            (vmm_space* space, u32 va, u32* out_pa, u32* out_size);

#endif
