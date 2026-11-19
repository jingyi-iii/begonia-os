#ifndef PAGE_H
#define PAGE_H

#include "typedefs.h"

#define PAGE_SIZE       (4096)
#define PAGE_MASK       (0xFFFFF000)
#define IS_4MB_ALIGN(x) (((u32)(x) & (0x400000 - 1)) == 0)
#define IS_4KB_ALIGN(x) (((u32)(x) & (4096 - 1)) == 0)

/*
 * Address-space layout.  User mappings live in [USER_BASE_VA, KERNEL_BASE_VA);
 * everything at or above KERNEL_BASE_VA belongs to the kernel.  page_clone_table()
 * uses the same boundary to decide which PDEs ring 3 may reach.
 */
#define USER_BASE_VA    (0x10000000)    /* 256 MiB */
#define KERNEL_BASE_VA  (0xC0000000)    /* 3 GiB   */

/*
 * The kernel image is linked for a high window, so a kernel address is
 * KERNEL_VMA + its physical address.  Page table entries keep physical
 * addresses: va_to_pa()/pa_to_va() are the only bridge between the two.
 * KERNEL_BASE_VA above stays the user/kernel boundary (and the future
 * physical-direct-map base).
 */
#define KERNEL_VMA      (0xF0000000)
#define va_to_pa(v)     ((u32)(v) - KERNEL_VMA)
#define pa_to_va(pa)    ((void*)((u32)(pa) + KERNEL_VMA))
#define PAGE_ALIGN(x)   (((u32)(x) + PAGE_SIZE - 1) & PAGE_MASK)


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
