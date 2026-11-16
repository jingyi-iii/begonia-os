#include "page.h"
#include "mm/pmm.h"
#include "string.h"
#include "spinlock.h"
#include "log.h"
#include "heap.h"
#include "errno.h"

#define PAGE_SIZE           (4096)
#define PAGE_MASK           (0xFFFFF000)

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
#define PTE_KERNEL          (PTE_PRESENT | PTE_RW)              /* supervisor r/w */
#define PTE_KERNEL_RO       (PTE_PRESENT)                       /* supervisor r/o */
#define PTE_USER_PAGE       (PTE_PRESENT | PTE_RW | PTE_USER)   /* user r/w */
#define PTE_USER_RO         (PTE_PRESENT | PTE_USER)            /* user r/o */

/* Address decomposition */
#define PD_INDEX(vaddr)     (((u32)(vaddr) >> 22) & 0x3FF)
#define PT_INDEX(vaddr)     (((u32)(vaddr) >> 12) & 0x3FF)
#define PAGE_OFFSET(vaddr)  ((u32)(vaddr) & 0xFFF)
#define PAGE_ALIGN(x)       (((u32)(x) + PAGE_SIZE - 1) & PAGE_MASK)

#define IS_4MB_ALIGN(x)     (((u32)(x) & (0x400000 - 1)) == 0)
#define IS_4KB_ALIGN(x)     (((u32)(x) & (4096 - 1)) == 0)
#define IS_PAGE_ALIGNED(x)  (((u32)(x) & (PAGE_SIZE - 1)) == 0)

#define USER_HEAP_BASE      (0xC0000000)
#define USER_HEAP_SIZE      (0x01000000)                        /* 16 MB */
#define USER_HEAP_END       (USER_HEAP_BASE + USER_HEAP_SIZE)   /* 0xC1000000 */
#define USER_ELF_BASE       (0xC1000000)
#define USER_ANON_BASE      (0xD0000000)

#define KERNEL_BASE_VADDR   (0xF0000000)                        /* reserved kernel high-half */
#define USER_SPACE_TOP      (KERNEL_BASE_VADDR)

/* Page Directory Entry (PDE) */
typedef union pde {
    u32 raw;
    struct {
        u32 present    : 1;   /* 0: Page is present in memory */
        u32 rw         : 1;   /* 1: 0=Read-only, 1=Read/Write */
        u32 user       : 1;   /* 2: 0=Supervisor, 1=User */
        u32 pwt        : 1;   /* 3: Page-level Write-Through */
        u32 pcd        : 1;   /* 4: Page-level Cache Disable */
        u32 accessed   : 1;   /* 5: Accessed */
        u32 dirty      : 1;   /* 6: Dirty (4MB pages only, zero otherwise) */
        u32 page_size  : 1;   /* 7: 0=4KB page, 1=4MB page */
        u32 global     : 1;   /* 8: Global (ignored for 4KB PTs) */
        u32 avail      : 3;   /* 9-11: Available for OS use */
        u32 paddr      : 20;  /* 12-31: Physical address of page table (4KB aligned) */
    } __attribute__((packed));
} pde;

/* Page Table Entry (PTE) */
typedef union pte {
    u32 raw;
    struct {
        u32 present    : 1;   /* 0: Page is present */
        u32 rw         : 1;   /* 1: Read/Write */
        u32 user       : 1;   /* 2: User/Supervisor */
        u32 pwt        : 1;   /* 3: Page-level Write-Through */
        u32 pcd        : 1;   /* 4: Page-level Cache Disable */
        u32 accessed   : 1;   /* 5: Accessed */
        u32 dirty      : 1;   /* 6: Dirty */
        u32 pat        : 1;   /* 7: PAT (0 for 4KB pages) */
        u32 global     : 1;   /* 8: Global */
        u32 avail      : 3;   /* 9-11: Available for OS use */
        u32 paddr      : 20;  /* 12-31: Physical address of 4KB page */
    } __attribute__((packed));
} pte;

static pde page_table_4gb[1024] __attribute__((aligned(PAGE_SIZE)));
static DECLARE_SPINLOCK(page_lock);

static inline pte* ptbl_of_pde(pde* p)
{
    return (pte*)(p->paddr << 12);
}

static inline void tlb_invlpg(u32 vaddr)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static inline void load_cr3(u32 pde_addr)
{
    /* Writing CR3 flushes the TLB for all non-global entries */
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pde_addr) : "memory");
}

static inline u32 get_cr3(void)
{
    u32 cr3 = 0;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void page_enable_size_extensions(void)
{
    __asm__ __volatile__(
        "mov    %%cr4,          %%eax\n\t"
        "or     $0x00000010,    %%eax\n\t"   /* CR4.PSE = bit 4 */
        "mov    %%eax,          %%cr4\n\t"
        : : : "eax", "memory"
    );
    log("%s: PSE enabled (4MB pages)\n", __func__);
}

static inline void page_enable(void)
{
    /*
     * Set PG bit (bit 31) in CR0.
     * The near jump flushes the prefetch queue so that all subsequent
     * instruction fetches go through the page-translation hardware.
     */
    __asm__ __volatile__(
        "mov    %%cr0,          %%eax\n\t"
        "or     $0x80000000,    %%eax\n\t"
        "mov    %%eax,          %%cr0\n\t"
        "jmp    1f\n\t"
        "1:     \n\t"
        : : : "eax", "memory"
    );
}

void page_map_4mb(u32 page_tbl, void* va, void* pa, u32 flags)
{
    pde* pdes = (pde*)page_tbl;
    size_t pde_index = PD_INDEX(va);

    if (pde_index >= 1024) {
        log_err("page_map_4mb: virtual address out of range(4GB)");
        return;
    }

    if (!pdes) {
        log_err("page_map_4mb: invalid page_tbl\n");
        return;
    }

    if (!IS_4MB_ALIGN(va) || !IS_4MB_ALIGN(pa)) {
        log_err("page_map_4mb: addresses must be 4MB-aligned");
        return;
    }

    u32 eflags = spinlock_lock_irqsave(&page_lock);
    pdes[pde_index].raw         = 0;
    pdes[pde_index].page_size   = 1;    /* 4MB page */
    pdes[pde_index].present     = (flags & PTE_PRESENT)  ? 1 : 0;
    pdes[pde_index].rw          = (flags & PTE_RW)       ? 1 : 0;
    pdes[pde_index].user        = (flags & PTE_USER)     ? 1 : 0;
    pdes[pde_index].pwt         = (flags & PTE_PWT)      ? 1 : 0;
    pdes[pde_index].pcd         = (flags & PTE_PCD)      ? 1 : 0;
    pdes[pde_index].global      = (flags & PTE_GLOBAL)   ? 1 : 0;
    pdes[pde_index].paddr       = ((u32)pa) >> 12;
    tlb_invlpg((u32)va);
    spinlock_unlock_irqrestore(&page_lock, eflags);
}

void page_unmap_4mb(u32 page_tbl, void* va)
{
    pde* pdes = (pde*)page_tbl;
    size_t pde_index = PD_INDEX(va);

    if (pde_index >= 1024) {
        log_err("%s: virtual address out of range(4GB)\n", __func__);
        return;
    }

    if (!pdes) {
        log_err("%s: invalid page_tbl\n", __func__);
        return;
    }

    u32 eflags = spinlock_lock_irqsave(&page_lock);
    pdes[pde_index].raw = 0;
    tlb_invlpg((u32)va);
    spinlock_unlock_irqrestore(&page_lock, eflags);
}

static int split_4mb_pde(u32 page_tbl, u32 user_accessible)
{
    pde* p = (pde*)page_tbl;
    pte* ptl = 0;

    if (!p) {
        log_err("split_4mb_pde: invalid page_tbl");
        return -1;
    }

    if (!p->present || !p->page_size) {
        log_err("split_4mb_pde: PDE is not a 4MB page");
        return -1;
    }

    ptl = (pte*)malloc(4096);
    if (!ptl) {
        log_err("split_4mb_pde: failed to allocate page table");
        return E_NOMEM;
    }

    /*
     * x86: a PDE with U/S=0 makes every page in its table supervisor-only,
     * so to host a USER page in a region previously covered by a kernel
     * 4MB page, the PDE itself must become user (U/S=1).  That only opens
     * the TABLE to ring-3 — each page's permission still comes from its
     * own PTE's U/S bit.  Preserve the ORIGINAL page permission in the
     * sibling PTEs (kernel pages stay supervisor); page_map_4kb() then
     * sets just the one target PTE to the requested flags.
     */
    int orig_user = p->user;
    if (user_accessible && p->user == 0)
        p->user = 1;

    for (size_t i = 0; i < 1024; i++) {
        ptl[i].raw          = 0;
        ptl[i].present      = p->present;
        ptl[i].rw           = p->rw;
        ptl[i].user         = orig_user;
        ptl[i].pwt          = p->pwt;
        ptl[i].pcd          = p->pcd;
        ptl[i].global       = p->global;
        ptl[i].paddr        = p->paddr + i;
    }

    {
        pde new_pde;
        new_pde.raw         = p->raw;
        new_pde.present     = 1;
        new_pde.rw          = 1;         /* writable PTEs take effect */
        new_pde.page_size   = 0;  /* 4KB page table */
        new_pde.paddr       = (u32)ptl >> 12;
        p->raw              = new_pde.raw;
    }

    return 0;
}

int page_map_4kb(u32 page_tbl, void* va, void* pa, u32 flags)
{
    pde* pdes = (pde*)page_tbl;
    size_t pde_index = PD_INDEX(va);
    size_t pte_index = PT_INDEX(va);

    if (pde_index >= 1024 || pte_index >= 1024) {
        log_err("page_map_4kb: virtual address out of range");
        return E_INVAL;
    }

    if (!pdes) {
        log_err("page_map_4kb: invalid page_tbl");
        return E_INVAL;
    }

    u32 eflags = spinlock_lock_irqsave(&page_lock);

    /*
     * If a 4MB page already covers this virtual address, split it
     * into 4KB page tables first so we can create a fine-grained
     * 4KB mapping alongside the existing identity map.
     */
    if (pdes[pde_index].present && pdes[pde_index].page_size) {
        if (split_4mb_pde((u32)&pdes[pde_index], flags & PTE_USER) != 0) {
            log_err("page_map_4kb: failed to split 4MB PDE at index %u", pde_index);
            spinlock_unlock_irqrestore(&page_lock, eflags);
            return E_NOMEM;
        }
    }

    if (!pdes[pde_index].present) {
        u32 pt_pa = (u32)malloc(4096);
        if (!pt_pa) {
            log_err("page_map_4kb: failed to allocate page table for vaddr 0x%x", va);
            spinlock_unlock_irqrestore(&page_lock, eflags);
            return E_NOMEM;
        }

        pdes[pde_index].raw     = 0;
        pdes[pde_index].present = 1;
        pdes[pde_index].rw      = 1;
        pdes[pde_index].user    = (flags & PTE_USER) ? 1 : 0;
        pdes[pde_index].paddr   = pt_pa >> 12;

        memset((void*)pt_pa, 0, PAGE_SIZE);
    }

    pte* ptbl = ptbl_of_pde(&pdes[pde_index]);
    ptbl[pte_index].raw         = 0;
    ptbl[pte_index].present     = (flags & PTE_PRESENT) ? 1 : 0;
    ptbl[pte_index].rw          = (flags & PTE_RW)      ? 1 : 0;
    ptbl[pte_index].user        = (flags & PTE_USER)    ? 1 : 0;
    ptbl[pte_index].paddr       = ((u32)pa) >> 12;

    tlb_invlpg((u32)va);
    spinlock_unlock_irqrestore(&page_lock, eflags);

    return 0;
}

void page_unmap_4kb(u32 page_tbl, void* va)
{
    pde* pdes = (pde*)page_tbl;
    size_t pde_index = PD_INDEX(va);
    size_t pte_index = PT_INDEX(va);

    if (pde_index >= 1024 || pte_index >= 1024) {
        log_err("page_unmap_4kb: virtual address out of range");
        return;
    }

    if (!pdes) {
        log_err("page_map_4kb: invalid page_tbl");
        return;
    }

    u32 eflags = spinlock_lock_irqsave(&page_lock);
    if (!pdes[pde_index].present || pdes[pde_index].page_size) {
        spinlock_unlock_irqrestore(&page_lock, eflags);
        return;
    }

    pte* ptbl = ptbl_of_pde(&pdes[pde_index]);
    if (!ptbl[pte_index].present) {
        spinlock_unlock_irqrestore(&page_lock, eflags);
        return;
    }

    if ((u32)va < USER_HEAP_BASE) {
        log_err("page_unmap_4kb: kernel-band VA 0x%x kept as supervisor identity", (u32)va);
        ptbl[pte_index].raw = ((u32)va & PAGE_MASK) | PTE_PRESENT | PTE_RW;
        tlb_invlpg((u32)va);
        spinlock_unlock_irqrestore(&page_lock, eflags);
        return;
    }

    ptbl[pte_index].raw = 0;
    tlb_invlpg((u32)va);
    spinlock_unlock_irqrestore(&page_lock, eflags);
}

void page_map_4mb_range(u32 page_tbl, u32 start_pa, u32 end_pa, u32 flags)
{
    if (!page_tbl) {
        log_err("page_map_4mb_range: invalid page_tbl");
        return;
    }

    if (!IS_4MB_ALIGN(start_pa) || !IS_4MB_ALIGN(end_pa)) {
        log_err("page_map_4mb_range: addresses must be 4MB-aligned");
        return;
    }

    for (u32 pa = start_pa; pa < end_pa; pa += 0x400000) {
        page_map_4mb(page_tbl, (void*)pa, (void*)pa, flags);
    }
}

void page_map_4kb_range(u32 page_tbl, u32 start_pa, u32 end_pa, u32 flags)
{
    if (!page_tbl) {
        log_err("page_map_4kb_range: invalid page_tbl");
        return;
    }

    if (!IS_4KB_ALIGN(start_pa) || !IS_4KB_ALIGN(end_pa)) {
        log_err("page_map_4kb_range: addresses must be 4KB-aligned");
        return;
    }

    for (u32 pa = start_pa; pa < end_pa; pa += PAGE_SIZE) {
        page_map_4kb(page_tbl, (void*)pa, (void*)pa, flags);
    }
}

void page_init(u32 total_memory, u32 reserved_end)
{
    (void)reserved_end;  /* currently unused — the paging pool sits below
                          * the PMM bitmap, so pmm_init() keeps it reserved */

    memset(page_table_4gb, 0, sizeof(page_table_4gb));
    
    page_map_4mb_range((u32)page_table_4gb, 0x0, 0x1000000, PTE_KERNEL);  /* identity map first 16MB */
    for (u32 addr = 0x1000000; addr < total_memory; addr += 0x400000)
        page_map_4mb((u32)page_table_4gb, (void*)addr, (void*)addr, PTE_KERNEL);

    page_enable_size_extensions();
    load_cr3((u32)page_table_4gb);
    page_enable();

    log("Paging: enabled (CR0.PG=1), CR3=0x%x", (u32)page_table_4gb);
}
