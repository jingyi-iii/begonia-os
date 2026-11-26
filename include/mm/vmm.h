#ifndef VMM_H
#define VMM_H

#include "typedefs.h"
#include "page.h"
#include "rbtree.h"
#include "spinlock.h"
#include "mm/pmm.h"

#define VMM_ERR_PTR(err)    ((void*)(iptr)(err))
#define VMM_PTR_ERR(ptr)    ((int)(iptr)(ptr))
#define VMM_IS_ERR(ptr)     ((uptr)(ptr) >= (uptr) - 4095)

typedef struct vmm_space {
    rbtree*     region_tree;
    page_table* page_tbl;
    pmm_region* pmm;
    spinlock    sp_lock;
} vmm_space;

int   vmm_create_space       (page_table* table, pmm_region* pmm, vmm_space* out);
int   vmm_clone_space        (page_table* new_table, page_table* from_table,
                              pmm_region* pmm, vmm_space* out);
void  vmm_destroy_space      (vmm_space* space);
void  vmm_switch_to_space    (vmm_space* space);

void* vmm_alloc_and_map_pages(vmm_space* space, u32 page_cnt, u32 flags);
void* vmm_map_pages          (vmm_space* space, u32 pa, u32 page_cnt,
                              u32 flags, int own_pa);
void* vmm_map_pages_at       (vmm_space* space, u32 va, u32 pa, u32 page_cnt,
                              u32 flags, int own_pa);
int   vmm_unmap_pages        (vmm_space* space, u32 any_va_in_pages);

#endif /* VMM_H */
