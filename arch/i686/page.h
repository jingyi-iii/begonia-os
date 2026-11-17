#ifndef PAGE_H
#define PAGE_H

#include "typedefs.h"

void page_map_4mb      (u32 page_tbl, void* va, void* pa, u32 flags);
void page_unmap_4mb    (u32 page_tbl, void* va);
int  page_map_4kb      (u32 page_tbl, void* va, void* pa, u32 flags);
void page_unmap_4kb    (u32 page_tbl, void* va);
void page_map_4mb_range(u32 page_tbl, u32 start_pa, u32 end_pa, u32 flags);
void page_map_4kb_range(u32 page_tbl, u32 start_pa, u32 end_pa, u32 flags);

#endif
