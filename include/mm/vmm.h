#ifndef VMM_H
#define VMM_H

#include "typedefs.h"
#include "page.h"
#include "rbtree.h"
#include "spinlock.h"
#include "mm/pmm.h"

#define VMM_ERR_PTR(err)    ((void*)(iptr)(err))
#define VMM_PTR_ERR(ptr)    ((int)(iptr)(ptr))
#define VMM_IS_ERR(ptr)     ((uptr)(ptr) >= (uptr)-4095)

typedef struct vmm_space {
    rbtree*     tree;
    page_table* page_tbl;
    pmm_region* pmm;
    spinlock    sp_lock;
} vmm_space;

typedef struct vmm_region {
    rbnode      node;
    void*       start_va;
    u32         size;
    u32         flags;
    u32         pa;             /* physical address for pmm_free_page */
    int         own_phys;
} vmm_region;

void vmm_switch(vmm_space* space);
int vmm_create(page_table* new_table, page_table* from_table, pmm_region* pmm, int user_accessible, vmm_space* out);
void vmm_destroy(vmm_space* space);

void* vmm_alloc_pages(vmm_space* space, u32 page_cnt, u32 flags);
void vmm_free_pages(vmm_space* space, void* va);

u32 vmm_space_va_to_pa(vmm_space* space, u32 va);

#endif /* VMM_H */
