#include "gdt.h"

/* CR0 control bits. */
#define CR0_PE          (1u << 0)   /* bit  0: Protection Enable -> enter protected mode */
#define CR0_PG          (1u << 31)  /* bit 31: Paging -> NOT here, see the note below   */

#define GDT_TYPE_CODE   (0xA)       /* code, readable -> access byte 0x9A */
#define GDT_TYPE_DATA   (0x2)       /* data, writable -> access byte 0x92 */

typedef struct {
    u16 limit;
    u32 base;
} __attribute__((packed)) gdt_meta;

static gdt_entry gdt[256] = {0};
static gdt_meta gdtmeta = {
    .limit = sizeof(gdt) - 1,
    .base = (u32)gdt
};

static inline gdt_entry gdt_flat(u32 type)
{
    return (gdt_entry){
        .limit_low = 0xFFFF, .limit_high = 0xF,   /* limit 0xFFFFF, 4 KiB units */
        .type = type, .s = 1, .present = 1,       /* ring 0 code or data        */
        .db = 1, .gran = 1,                       /* 32-bit, 4 KiB granularity  */
    };
}

void switch_to_protect_mode(void)
{
    gdt[GDT_DUMMY]    = (gdt_entry){0};            // null descriptor
    gdt[GDT_SYS_CODE] = gdt_flat(GDT_TYPE_CODE);   // 0x08: flat 32-bit code
    gdt[GDT_SYS_DATA] = gdt_flat(GDT_TYPE_DATA);   // 0x10: flat 32-bit data

    __asm__ __volatile__ (
        "lgdt   %0\n"
        "movw   %1,     %%ax\n"          /* reload DS/ES/FS/GS/SS with 0x10 */
        "movw   %%ax,   %%ds\n"
        "movw   %%ax,   %%es\n"
        "movw   %%ax,   %%fs\n"
        "movw   %%ax,   %%gs\n"
        "movw   %%ax,   %%ss\n"
        "ljmp   %2,     $next\n"         /* reload CS with 0x08 (far jump) */
        "next:\n"
        :
        : "m"(gdtmeta), "i"(SEL_SYS_DATA), "i"(SEL_SYS_CODE)
        : "eax", "memory"
    );

    __asm__ __volatile__ (
        "movl   %%cr0,  %%eax\n"
        "orl    %0,     %%eax\n"         /* CR0_PE == 1, NOT CR0_PG == 1 << 31 */
        "movl   %%eax,  %%cr0\n"
        :
        : "i"(CR0_PE)
        : "eax", "memory"
    );
}

gdt_entry* gdt_get_entry(u32 index)
{
    if (index >= sizeof(gdt) / sizeof(gdt[0]))
        return 0;

    return &gdt[index];
}

int gdt_set_entry(u32 index, gdt_entry entry)
{
    if (index >= sizeof(gdt) / sizeof(gdt[0]))
        return -1;

    gdt[index] = entry;
    return 0;
}