#ifndef PAGE_H
#define PAGE_H

#include "typedefs.h"

void page_map_4mb      (u32 page_tbl, void* va, void* pa, u32 flags);
void page_unmap_4mb    (u32 page_tbl, void* va);
int  page_map_4kb      (u32 page_tbl, void* va, void* pa, u32 flags);
void page_unmap_4kb    (u32 page_tbl, void* va);
void page_map_4mb_range(u32 page_tbl, u32 start_pa, u32 end_pa, u32 flags);
void page_map_4kb_range(u32 page_tbl, u32 start_pa, u32 end_pa, u32 flags);
void page_init         (u32 total_memory, u32 reserved_end);

/*
 * TODO(paging): known gaps before this module can back real address spaces.
 *
 * - Page tables are malloc(4096)'d, but the heap only guarantees 8-byte
 *   alignment.  A page table must be 4KB-aligned: allocate it from a
 *   page-aligned source (PMM blocks with PAGE_SIZE granularity, or a
 *   dedicated page-table pool) and free it with the matching allocator.
 * - page_map_4kb(): mapping a PTE_USER page into an existing supervisor
 *   page table (page_size=0, user=0) does not promote the PDE's U/S bit,
 *   so ring 3 still faults.
 * - page_map_4kb() drops PTE_PWT/PTE_PCD/PTE_GLOBAL and does not check
 *   that va/pa are 4KB-aligned (the 4MB path does check alignment).
 * - page_map_4mb()/page_unmap_4mb() only invalidate the target VA.  If the
 *   PDE used to hold a 4KB page table, the other 1023 TLB entries keep the
 *   old translation; reload CR3 or invlpg the whole 4MB range.
 * - page_init() ignores reserved_end and maps a fixed [0, 16MB).  Map up
 *   to max(total_memory, reserved_end) and hand the free memory above the
 *   kernel to the PMM (pmm_reserve + mark the kernel image/modules used).
 *   The comment about a linker-reserved paging pool is stale: linker.ld
 *   has no .page_tables/.pmm_bitmap sections and there is no pmm_init().
 * - Page tables are allocated (and errors logged) while page_lock is held
 *   with interrupts disabled; allocate/log outside the lock and keep only
 *   the PDE/PTE update inside it.
 * - page.h still lacks the shared PAGE_SIZE / PTE_* / user-layout macros
 *   and a CR3-switch helper.  Clone/destroy of address spaces (dropped
 *   with the paging.c port) has to come back before process support.
 * - Cleanup: dead "index >= 1024" checks after PD_INDEX/PT_INDEX masking,
 *   unused mm/pmm.h include and USER_* macros, and unmap_4kb's error
 *   message still naming page_map_4kb.  The Makefile page.o rule should
 *   list heap.h and errno.h as dependencies.
 */

#endif
