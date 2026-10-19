#ifndef GDT_H
#define GDT_H

#include "boot_common.h"

#define GDT_DUMMY       (0)
#define GDT_SYS_CODE    (1)
#define GDT_SYS_DATA    (2)
#define GDT_TSS         (3)
#define GDT_LDT         (4)

#define SEL_SYS_CODE    (GDT_SYS_CODE << 3)   /* 0x08 */
#define SEL_SYS_DATA    (GDT_SYS_DATA << 3)   /* 0x10 */
#define SEL_TSS         (GDT_TSS << 3)        /* 0x18 */
#define SEL_LDT         (GDT_LDT << 3)        /* 0x20 */

typedef union {
    u64 raw;
    struct {
        u32 limit_low  : 16;
        u32 base_low   : 16;
        u32 base_mid   : 8;
        u32 type       : 4;
        u32 s          : 1;
        u32 dpl        : 2;
        u32 present    : 1;
        u32 limit_high : 4;
        u32 avl        : 1;
        u32 l          : 1;
        u32 db         : 1;
        u32 gran       : 1;
        u32 base_high  : 8;
    } __attribute__((packed));
} gdt_entry;

static inline u64 gen_gdt_entry(u32 base, u32 limit, u16 flags)
{
    u64 desc = 0;

    desc  = limit        & 0x000f0000;
    desc |= (flags << 8) & 0x00f0ff00;
    desc |= (base >> 16) & 0x000000ff;
    desc |= base         & 0xff000000;

    desc <<= 32;
    desc |= base << 16;
    desc |= limit & 0x0000ffff;

    return desc;
}

gdt_entry* gdt_get_entry(u32 index);
int gdt_set_entry(u32 index, gdt_entry entry);

#endif
