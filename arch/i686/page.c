#include "page.h"
#include "string.h"
#include "spinlock.h"
#include "log.h"
#include "heap.h"
#include "errno.h"
#include "range.h"

/* Address decomposition */
#define PD_INDEX(vaddr)     (((u32)(vaddr) >> 22) & 0x3FF)
#define PT_INDEX(vaddr)     (((u32)(vaddr) >> 12) & 0x3FF)

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
    /* paddr is physical; page tables live in the kernel high window */
    return (pte*)pa_to_va(p->paddr << 12);
}

static inline void tlb_flush_page(u32 vaddr)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr) : "memory");
}

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
    __asm__ __volatile__(
        "mov    %%cr0,          %%eax\n\t"
        "or     $0x80000000,    %%eax\n\t"
        "mov    %%eax,          %%cr0\n\t"
        "jmp    1f\n\t"
        "1:     \n\t"
        : : : "eax", "memory"
    );
}

int page_map_4mb(page_table* pt, u32 va, u32 pa, u32 flags)
{
    pde* pdes = (pde*)pt;
    size_t pde_index = PD_INDEX(va);

    if (!pdes)
        return E_INVAL;

    if (!IS_4MB_ALIGN(va) || !IS_4MB_ALIGN(pa)) {
        log_err("%s: page table(%p): address must be 4MB-aligned\n",
                __func__, pt);
        return E_INVAL;
    }

    u32 eflags = spinlock_lock_irqsave(&page_lock);
    if (pdes[pde_index].present) {
        log_err("%s: page table(%p): page already mapped, unmap it first\n",
                __func__, pt);
        spinlock_unlock_irqrestore(&page_lock, eflags);
        return E_EXISTS;
    }

    pdes[pde_index].raw       = 0;
    pdes[pde_index].page_size = 1;    /* 4MB page */
    pdes[pde_index].present   = (flags & PTE_PRESENT)  ? 1 : 0;
    pdes[pde_index].rw        = (flags & PTE_RW)       ? 1 : 0;
    pdes[pde_index].user      = (flags & PTE_USER)     ? 1 : 0;
    pdes[pde_index].pwt       = (flags & PTE_PWT)      ? 1 : 0;
    pdes[pde_index].pcd       = (flags & PTE_PCD)      ? 1 : 0;
    pdes[pde_index].global    = (flags & PTE_GLOBAL)   ? 1 : 0;
    pdes[pde_index].paddr     = ((u32)pa) >> 12;
    tlb_flush_page((u32)va);
    spinlock_unlock_irqrestore(&page_lock, eflags);

    log_success("%s: page table(%p): map pa(%p) to va(%p)\n",
                __func__, pt, pa, va);
    return 0;
}

int page_unmap_4mb(page_table* pt, u32 va)
{
    pde* pdes = (pde*)pt;
    size_t pde_index = PD_INDEX(va);

    if (!pdes)
        return E_INVAL;

    u32 eflags = spinlock_lock_irqsave(&page_lock);

    /* unmapping a 4MB page also drops any 4KB table underneath it */
    if (pdes[pde_index].present && !pdes[pde_index].page_size) {
        free(ptbl_of_pde(&pdes[pde_index]));
        tlb_flush_all();
    }

    pdes[pde_index].raw = 0;
    tlb_flush_page((u32)va);
    spinlock_unlock_irqrestore(&page_lock, eflags);

    log_success("%s: page table(%p): unmap va(%p)\n",
                __func__, pt, va);
    return 0;
}

/*
 * Turn one 4MB PDE into a page table that mirrors it page by page, stored in
 * the table the caller allocated (ptbl).  The caller holds page_lock.
 */
static int split_4mb_pde(pde* p, pte* ptbl, u32 user_accessible)
{
    if (!p || !ptbl)
        return E_INVAL;

    if (!p->present || !p->page_size) {
        log_err("%s: page table(%p): PDE is not a 4MB page\n", __func__, ptbl);
        return E_INVAL;
    }

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
        new_pde.paddr     = va_to_pa(ptbl) >> 12;
        p->raw            = new_pde.raw;
    }

    return 0;
}

int page_map_4kb(page_table* pt, u32 va, u32 pa, u32 flags)
{
    pde* pdes = (pde*)pt;
    size_t pde_index = PD_INDEX(va);
    size_t pte_index = PT_INDEX(va);
    int need_flush_pde = 0;
    int err = 0;

    if (!pdes)
        return E_INVAL;

    if (!IS_4KB_ALIGN(va) || !IS_4KB_ALIGN(pa)) {
        log_err("%s: page table(%p): addresses must be 4KB-aligned\n",
                __func__, pt);
        return E_INVAL;
    }

    u32 eflags = spinlock_lock_irqsave(&page_lock);

    if (!pdes[pde_index].present) {
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
            pdes[pde_index].paddr   = va_to_pa(ptbl) >> 12;
        }

        need_flush_pde = 1;
    } else if (pdes[pde_index].page_size) {
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

        need_flush_pde = 1;
    } else {
        pte* ptbl = ptbl_of_pde(&pdes[pde_index]);
        if (ptbl[pte_index].present) {
            spinlock_unlock_irqrestore(&page_lock, eflags);
            log_err("%s: page table(%p): va(%p) already mapped\n",
                    __func__, pt, (void*)(u32)va);
            return E_EXISTS;
        }
        if ((flags & PTE_USER) && !pdes[pde_index].user)
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

        if (need_flush_pde)
            tlb_flush_all();
        else
            tlb_flush_page(va);
    }

    spinlock_unlock_irqrestore(&page_lock, eflags);

    if (err)
        log_err("%s: page table(%p): cannot map 0x%x (error %d)\n",
                __func__, pt, (u32)va, err);

    return err;
}

int page_unmap_4kb(page_table* pt, u32 va)
{
    pde* pdes = (pde*)pt;
    size_t pde_index = PD_INDEX(va);
    size_t pte_index = PT_INDEX(va);
    pte* ptbl = 0;

    if (!pdes)
        return E_NOTFOUND;

    u32 eflags = spinlock_lock_irqsave(&page_lock);

    if (!pdes[pde_index].present || pdes[pde_index].page_size) {
        spinlock_unlock_irqrestore(&page_lock, eflags);
        return E_NOTFOUND;
    }

    ptbl = ptbl_of_pde(&pdes[pde_index]);
    if (!ptbl[pte_index].present) {
        spinlock_unlock_irqrestore(&page_lock, eflags);
        return E_NOTFOUND;
    }

    ptbl[pte_index].raw = 0;
    tlb_flush_page((u32)va);

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

    return 0;
}

int page_map_4mb_range(page_table* pt, u32 start_va, u32 start_pa, u32 size, u32 flags)
{
    int ret = 0;

    if (!pt)
        return E_INVAL;

    if (!IS_4MB_ALIGN(start_va) || !IS_4MB_ALIGN(start_pa) || !IS_4MB_ALIGN(size)) {
        log_err("%s: page table(%p): address/size must be 4MB-aligned\n",
                __func__, pt);
        return E_INVAL;
    }

    if (!u32_range_fits(start_va, size) ||
        !u32_range_fits(start_pa, size)) {
        return E_INVAL;
    }

    for (u32 i = 0; i < size; i += 0x400000) {
        ret = page_map_4mb(pt, start_va + i, start_pa + i, flags);
        if (ret) {
            for (u32 j = 0; j < i; j += 0x400000) {
                page_unmap_4mb(pt, start_va + j);
            }
            return ret;
        }
    }

    return ret;
}

int page_map_4kb_range(page_table* pt, u32 start_va, u32 start_pa, u32 size, u32 flags)
{
    int ret = 0;

    if (!pt)
        return E_INVAL;

    if (!IS_4KB_ALIGN(start_va) || !IS_4KB_ALIGN(start_pa) || !IS_4KB_ALIGN(size)) {
        log_err("%s: page table(%p): addresses must be 4KB-aligned\n",
                __func__, pt);
        return E_INVAL;
    }

    if (!u32_range_fits(start_va, size) ||
        !u32_range_fits(start_pa, size)) {
        return E_INVAL;
    }

    for (u32 i = 0; i < size; i += PAGE_SIZE) {
        ret = page_map_4kb(pt, start_va + i, start_pa + i, flags);
        if (ret) {
            for (u32 j = 0; j < i; j += PAGE_SIZE) {
                page_unmap_4kb(pt, start_va + j);
            }
            return ret;
        }
    }

    return ret;
}

int page_clone_table(page_table* new, page_table* from)
{
    pde* new_pdes = (pde*)new;
    pde* src_pdes = (pde*)from;
    u32 eflags = 0;
    int err = E_NOMEM;
    u32 i = 0;
    u32 first_krn_index = PD_INDEX(KERNEL_VMA);

    if (!new || !from)
        return E_INVAL;

    if (new == from) {
        log_err("%s: page table(%p): cannot clone a table onto itself\n",
                __func__, new);
        return E_INVAL;
    }

    eflags = spinlock_lock_irqsave(&page_lock);

    for (i = 0; i < 1024; i++) {
        new_pdes[i].raw = src_pdes[i].raw;
        new_pdes[i].user = (i < first_krn_index) ? 1 : 0;

        if (!src_pdes[i].present || src_pdes[i].page_size)
            continue;

        pte* src_ptbl = ptbl_of_pde(&src_pdes[i]);
        if (!src_ptbl) {
            log_err("%s: page table(%p): PDE %u has no page table\n",
                    __func__, new, i);
            err = E_INVAL;
            goto rollback;
        }

        pte* new_ptbl = (pte*)malloc_aligned(PAGE_SIZE, PAGE_SIZE);
        if (!new_ptbl) {
            log_err("%s: page table(%p): no memory for page table %u\n",
                    __func__, new, i);
            err = E_NOMEM;
            goto rollback;
        }

        memcpy(new_ptbl, src_ptbl, PAGE_SIZE);
        new_pdes[i].paddr = va_to_pa(new_ptbl) >> 12;
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

int page_destroy_table(page_table* pt)
{
    pde* pdes = (pde*)pt;

    if (!pt)
        return E_INVAL;

    u32 eflags = spinlock_lock_irqsave(&page_lock);
    for (u32 i = 0; i < 1024; i++) {
        if (!pdes[i].present)
            continue;

        if (pdes[i].page_size == 0) {
            pte* ptes = ptbl_of_pde(&pdes[i]);
            free(ptes);
        }

        pdes[i].raw = 0;
    }
    tlb_flush_all();
    spinlock_unlock_irqrestore(&page_lock, eflags);

    return 0;
}
