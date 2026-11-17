#include "page.h"
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

static DECLARE_SPINLOCK(page_lock);

static inline pte* ptbl_of_pde(pde* p)
{
    return (pte*)(p->paddr << 12);
}

static inline void tlb_invlpg(u32 vaddr)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr) : "memory");
}

/*
 * Full TLB flush by reloading CR3.  Needed after a page table is freed:
 * invlpg only drops the entries of one address, while the paging-structure
 * caches may still hold the (now freed) table for the whole 4MB region.
 */
static inline void tlb_flush_all(void)
{
    __asm__ __volatile__(
        "mov    %%cr3, %%eax\n\t"
        "mov    %%eax, %%cr3\n\t"
        : : : "eax", "memory"
    );
}

static inline void page_enable_size_extensions(void)
{
    __asm__ __volatile__(
        "mov    %%cr4,          %%eax\n\t"
        "or     $0x00000010,    %%eax\n\t"   /* CR4.PSE = bit 4 */
        "mov    %%eax,          %%cr4\n\t"
        : : : "eax", "memory"
    );
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

void page_map_4mb(page_table* page_tbl, void* va, void* pa, u32 flags)
{
    pde* pdes = (pde*)page_tbl;
    size_t pde_index = PD_INDEX(va);

    if (!pdes) {
        log_err("%s: invalid page table directory\n", __func__);
        return;
    }

    if (!IS_4MB_ALIGN(va) || !IS_4MB_ALIGN(pa)) {
        log_err("%s: addresses must be 4MB-aligned\n", __func__);
        return;
    }

    u32 eflags = spinlock_lock_irqsave(&page_lock);

    /* free() takes the heap lock, which never takes page_lock: no inversion */
    if (pdes[pde_index].present && !pdes[pde_index].page_size)
        free(ptbl_of_pde(&pdes[pde_index]));

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

void page_unmap_4mb(page_table* page_tbl, void* va)
{
    pde* pdes = (pde*)page_tbl;
    size_t pde_index = PD_INDEX(va);

    if (!pdes) {
        log_err("%s: invalid page table directory\n", __func__);
        return;
    }

    u32 eflags = spinlock_lock_irqsave(&page_lock);

    /* unmapping a 4MB page also drops any 4KB table underneath it */
    if (pdes[pde_index].present && !pdes[pde_index].page_size) {
        free(ptbl_of_pde(&pdes[pde_index]));
        tlb_flush_all();
    }

    pdes[pde_index].raw = 0;
    tlb_invlpg((u32)va);
    spinlock_unlock_irqrestore(&page_lock, eflags);
}

/*
 * Turn one 4MB PDE into a page table that mirrors it page by page, stored in
 * the table the caller allocated (ptbl).  The caller holds page_lock.
 */
static int split_4mb_pde(pde* p, pte* ptbl, u32 user_accessible)
{
    if (!p || !ptbl) {
        log_err("%s: null argument\n", __func__);
        return E_INVAL;
    }

    if (!p->present || !p->page_size) {
        log_err("%s: PDE is not a 4MB page\n", __func__);
        return E_INVAL;
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
    u32 orig_user = p->user;
    if (user_accessible && !p->user)
        p->user = 1;

    for (size_t i = 0; i < 1024; i++) {
        ptbl[i].raw     = 0;
        ptbl[i].present = p->present;
        ptbl[i].rw      = p->rw;
        ptbl[i].user    = orig_user;
        ptbl[i].pwt     = p->pwt;
        ptbl[i].pcd     = p->pcd;
        ptbl[i].global  = p->global;
        ptbl[i].paddr   = p->paddr + i;
    }

    {
        pde new_pde;
        new_pde.raw       = p->raw;
        new_pde.present   = 1;
        new_pde.rw        = 1;      /* writable PTEs take effect */
        new_pde.page_size = 0;      /* 4KB page table */
        new_pde.dirty     = 0;      /* bit 6 is only meaningful for a 4MB page */
        /*
         * Identity-mapped kernel: the pointer is also the physical address.
         * The VMM has to translate this once it owns the address space.
         */
        new_pde.paddr     = (u32)ptbl >> 12;
        p->raw            = new_pde.raw;
    }

    return 0;
}

int page_map_4kb(page_table* page_tbl, void* va, void* pa, u32 flags)
{
    pde* pdes = (pde*)page_tbl;
    size_t pde_index = PD_INDEX(va);
    size_t pte_index = PT_INDEX(va);
    int err = 0;

    if (!pdes) {
        log_err("%s: invalid page table directory\n", __func__);
        return E_INVAL;
    }

    if (!IS_4KB_ALIGN(va) || !IS_4KB_ALIGN(pa)) {
        log_err("%s: addresses must be 4KB-aligned\n", __func__);
        return E_INVAL;
    }

    u32 eflags = spinlock_lock_irqsave(&page_lock);

    if (pdes[pde_index].present && pdes[pde_index].page_size) {
        /*
         * If a 4MB page already covers this virtual address, split it into
         * a 4KB page table first so we can create a fine-grained mapping
         * alongside the existing identity map.
         */
        pte* ptbl = (pte*)malloc_aligned(PAGE_SIZE, PAGE_SIZE);
        if (!ptbl) {
            err = E_NOMEM;
        } else if (split_4mb_pde(&pdes[pde_index], ptbl, flags & PTE_USER) != 0) {
            free(ptbl);
            err = E_INVAL;
        }
    } else if (!pdes[pde_index].present) {
        pte* ptbl = (pte*)malloc_aligned(PAGE_SIZE, PAGE_SIZE);
        if (!ptbl) {
            err = E_NOMEM;
        } else {
            memset(ptbl, 0, PAGE_SIZE);

            pdes[pde_index].raw     = 0;
            pdes[pde_index].present = 1;
            pdes[pde_index].rw      = 1;
            pdes[pde_index].user    = (flags & PTE_USER) ? 1 : 0;
            pdes[pde_index].pwt     = (flags & PTE_PWT)  ? 1 : 0;
            pdes[pde_index].pcd     = (flags & PTE_PCD)  ? 1 : 0;
            pdes[pde_index].paddr   = (u32)ptbl >> 12;
        }
    } else if ((flags & PTE_USER) && !pdes[pde_index].user) {
        /* a supervisor PDE would hide the user page from ring 3 */
        pdes[pde_index].user = 1;
    }

    if (!err) {
        pte* ptbl = ptbl_of_pde(&pdes[pde_index]);

        ptbl[pte_index].raw     = 0;
        ptbl[pte_index].present = (flags & PTE_PRESENT) ? 1 : 0;
        ptbl[pte_index].rw      = (flags & PTE_RW)      ? 1 : 0;
        ptbl[pte_index].user    = (flags & PTE_USER)    ? 1 : 0;
        ptbl[pte_index].pwt     = (flags & PTE_PWT)     ? 1 : 0;
        ptbl[pte_index].pcd     = (flags & PTE_PCD)     ? 1 : 0;
        ptbl[pte_index].global  = (flags & PTE_GLOBAL)  ? 1 : 0;
        ptbl[pte_index].paddr   = ((u32)pa) >> 12;

        tlb_invlpg((u32)va);
    }

    spinlock_unlock_irqrestore(&page_lock, eflags);

    if (err)
        log_err("%s: cannot map 0x%x (error %d)\n", __func__, (u32)va, err);

    return err;
}

void page_unmap_4kb(page_table* page_tbl, void* va)
{
    pde* pdes = (pde*)page_tbl;
    size_t pde_index = PD_INDEX(va);
    size_t pte_index = PT_INDEX(va);
    pte* ptbl = 0;

    if (!pdes) {
        log_err("%s: invalid page table directory\n", __func__);
        return;
    }

    u32 eflags = spinlock_lock_irqsave(&page_lock);

    if (!pdes[pde_index].present || pdes[pde_index].page_size) {
        spinlock_unlock_irqrestore(&page_lock, eflags);
        return;
    }

    ptbl = ptbl_of_pde(&pdes[pde_index]);
    if (!ptbl[pte_index].present) {
        spinlock_unlock_irqrestore(&page_lock, eflags);
        return;
    }

    ptbl[pte_index].raw = 0;
    tlb_invlpg((u32)va);

    /* Give the table back once its last entry is gone. */
    int empty = 1;
    for (size_t i = 0; i < 1024; i++) {
        if (ptbl[i].present) {
            empty = 0;
            break;
        }
    }

    if (empty) {
        pdes[pde_index].raw = 0;
        free(ptbl);
        tlb_flush_all();
    }

    spinlock_unlock_irqrestore(&page_lock, eflags);
}

void page_map_4mb_range(page_table* page_tbl, u32 start_pa, u32 end_pa, u32 flags)
{
    if (!page_tbl) {
        log_err("%s: invalid page table directory\n", __func__);
        return;
    }

    if (!IS_4MB_ALIGN(start_pa) || !IS_4MB_ALIGN(end_pa)) {
        log_err("%s: addresses must be 4MB-aligned\n", __func__);
        return;
    }

    for (u32 pa = start_pa; pa < end_pa; pa += 0x400000) {
        page_map_4mb(page_tbl, (void*)pa, (void*)pa, flags);
    }
}

void page_map_4kb_range(page_table* page_tbl, u32 start_pa, u32 end_pa, u32 flags)
{
    if (!page_tbl) {
        log_err("%s: invalid page table directory\n", __func__);
        return;
    }

    if (!IS_4KB_ALIGN(start_pa) || !IS_4KB_ALIGN(end_pa)) {
        log_err("%s: addresses must be 4KB-aligned\n", __func__);
        return;
    }

    for (u32 pa = start_pa; pa < end_pa; pa += PAGE_SIZE) {
        page_map_4kb(page_tbl, (void*)pa, (void*)pa, flags);
    }
}

/*
 * Deep-clone a page directory: every present second-level page table is
 * copied into a fresh 4KB-aligned table, so the caller and the source can
 * remap entries independently.  The leaf pages and any 4MB PDE are shared
 * (this is a table copy, not copy-on-write).
 *
 * user_accessible forces the U/S bit on every copied PDE; leaf permissions
 * still come from the PTEs (or from the 4MB page itself).
 *
 * On failure the directory is cleared and every table allocated so far is
 * freed, so the caller only has to free the directory itself.
 */
int page_clone_table(page_table* new, page_table* from, int user_accessible)
{
    pde* new_pdes = (pde*)new;
    pde* src_pdes = (pde*)from;
    u32 eflags = 0;
    int err = E_NOMEM;
    u32 i = 0;

    if (!new || !from || new == from) {
        log_err("%s: invalid page tables\n", __func__);
        return E_INVAL;
    }

    eflags = spinlock_lock_irqsave(&page_lock);

    for (i = 0; i < 1024; i++) {
        new_pdes[i].raw = src_pdes[i].raw;

        if (!src_pdes[i].present || src_pdes[i].page_size) {
            new_pdes[i].user = user_accessible ? 1 : 0;
            continue;                   /* nothing below this PDE to copy */
        }

        pte* src_ptbl = ptbl_of_pde(&src_pdes[i]);
        if (!src_ptbl) {
            log_err("%s: PDE %u has no page table\n", __func__, i);
            err = E_INVAL;
            goto rollback;
        }

        pte* new_ptbl = (pte*)malloc_aligned(PAGE_SIZE, PAGE_SIZE);
        if (!new_ptbl) {
            log_err("%s: no memory for page table %u\n", __func__, i);
            err = E_NOMEM;
            goto rollback;
        }

        memcpy(new_ptbl, src_ptbl, PAGE_SIZE);
        new_pdes[i].paddr = (u32)new_ptbl >> 12;
        new_pdes[i].user  = user_accessible ? 1 : 0;
    }

    spinlock_unlock_irqrestore(&page_lock, eflags);
    return 0;

rollback:
    while (i > 0) {
        i--;
        if (new_pdes[i].present && !new_pdes[i].page_size)
            free(ptbl_of_pde(&new_pdes[i]));
    }
    memset(new_pdes, 0, PAGE_SIZE);
    spinlock_unlock_irqrestore(&page_lock, eflags);
    return err;
}

/*
 * Free the second-level page tables of a cloned directory.  4MB entries are
 * skipped: they are shared with the source directory, never allocated here.
 * The directory itself belongs to the caller.
 */
int page_destroy_table(page_table* page_tbl)
{
    pde* pdes = (pde*)page_tbl;

    if (!page_tbl) {
        log_err("%s: invalid page table\n", __func__);
        return E_INVAL;
    }

    for (u32 i = 0; i < 1024; i++) {
        if (!pdes[i].present)
            continue;

        if (pdes[i].page_size == 0)
            free(ptbl_of_pde(&pdes[i]));
        pdes[i].raw = 0;
    }

    return 0;
}
