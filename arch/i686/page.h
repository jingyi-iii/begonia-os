#ifndef PAGE_H
#define PAGE_H

#include "typedefs.h"

#define PAGE_SIZE       (4096)
#define PAGE_MASK       (0xFFFFF000)

typedef struct page_table {
    u32 raw[1024];      // cover 4GB
} page_table;

/*
 * The CR3/paging switch helpers below have no caller yet: the VMM owns the
 * address space and will call them once it lands.
 */
static inline void page_load_table(page_table* table)
{
    if (!table)
        return;

    /* Writing CR3 flushes the TLB for all non-global entries */
    __asm__ __volatile__("mov %0, %%cr3" : : "r"((u32)table) : "memory");
}


void page_map_4mb      (page_table* page_tbl, void* va, void* pa, u32 flags);
void page_unmap_4mb    (page_table* page_tbl, void* va);
int  page_map_4kb      (page_table* page_tbl, void* va, void* pa, u32 flags);
void page_unmap_4kb    (page_table* page_tbl, void* va);
void page_map_4mb_range(page_table* page_tbl, u32 start_pa, u32 end_pa, u32 flags);
void page_map_4kb_range(page_table* page_tbl, u32 start_pa, u32 end_pa, u32 flags);
int  page_clone_table  (page_table* new, page_table* from, int user_accessible);
int  page_destroy_table(page_table* page_tbl);

#endif
