#ifndef PAGE_H
#define PAGE_H

#include "typedefs.h"

/* Page flags */
#define PTE_PRESENT         (1 << 0)
#define PTE_RW              (1 << 1)
#define PTE_USER            (1 << 2)
#define PTE_PWT             (1 << 3)
#define PTE_PCD             (1 << 4)
#define PTE_ACCESSED        (1 << 5)
#define PTE_DIRTY           (1 << 6)
#define PTE_GLOBAL          (1 << 8)

/* Common combinations */
#define PTE_KERNEL_PAGE     (PTE_PRESENT | PTE_RW)              /* supervisor r/w */
#define PTE_KERNEL_RO       (PTE_PRESENT)                       /* supervisor r/o */
#define PTE_USER_PAGE       (PTE_PRESENT | PTE_RW | PTE_USER)   /* user r/w */
#define PTE_USER_RO         (PTE_PRESENT | PTE_USER)            /* user r/o */

#define PAGE_SIZE           (4096)
#define PAGE_MASK           (0xFFFFF000)
#define IS_4MB_ALIGN(x)     (((u32)(x) & (0x400000 - 1)) == 0)
#define IS_4KB_ALIGN(x)     (((u32)(x) & (4096 - 1)) == 0)
#define PAGE_ALIGN(x)       (((u32)(x) + PAGE_SIZE - 1) & PAGE_MASK)

#define KERNEL_VMA          (0xF0000000)
#define va_to_pa(v)         ((u32)(v) - KERNEL_VMA)
#define pa_to_va(pa)        ((void*)((u32)(pa) + KERNEL_VMA))

#define USER_BASE_VA        (0x10000000)
#define KERNEL_BASE_VA      KERNEL_VMA

typedef struct page_table {
    u32 raw[1024];      // cover 4GB
} page_table;

static inline void page_load_table(page_table* table)
{
    if (!table)
        return;

    u32 pa = va_to_pa(table);
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pa) : "memory");
}

static inline u32 page_get_table(void)
{
    u32 pa = 0;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(pa) : : "memory");

    return (u32)pa_to_va(pa);
}

int  page_map_4mb      (page_table* pt, u32 va, u32 pa, u32 flags);
int  page_unmap_4mb    (page_table* pt, u32 va);
int  page_map_4kb      (page_table* pt, u32 va, u32 pa, u32 flags);
int  page_unmap_4kb    (page_table* pt, u32 va);
int  page_map_4mb_range(page_table* pt, u32 start_va, u32 start_pa, u32 size, u32 flags);
int  page_map_4kb_range(page_table* pt, u32 start_va, u32 start_pa, u32 size, u32 flags);
int  page_clone_table  (page_table* new_pt, page_table* from_pt);
int  page_destroy_table(page_table* pt);

#endif
