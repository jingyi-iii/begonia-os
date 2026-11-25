/*
 * lib/multiboot.c - minimal multiboot v1 info/module parser.
 *
 * GRUB (and qemu -kernel) pass the physical address of the multiboot_info in
 * EBX on entry; boot.S saves it as boot_mbi.  This tree runs the kernel from
 * a high window and drops the low identity map, so the info and everything it
 * points at must be reached through pa_to_va():
 *
 *   mboot_init() must be called while boot_mbi itself is still readable -
 *   that is, before the switch to a page table that no longer identity-maps
 *   low memory.  After that every access below goes through the window, so
 *   the rest of the parser works from the high half.
 *
 * Only the module fields are parsed here; everything else in the info is left
 * untouched.
 */

#include "multiboot.h"
#include "page.h"
#include "errno.h"
#include "string.h"

/* Multiboot v1 info field offsets (u32 words), per the spec. */
#define MBI_FLAGS       0   /* bit 3 = modules present */
#define MBI_MODS_COUNT  5
#define MBI_MODS_ADDR   6

/* One multiboot v1 module entry - u32 words. */
#define MOD_START       0
#define MOD_END         1
#define MOD_STRING      2

/*
 * multiboot_info.flags - NOTE: these bit numbers are the INFO struct's own
 * numbering (bit 0 = memory info), NOT the kernel multiboot header's flags.
 * Module list present = bit 3 (0x08); bit 4 (0x10) is the a.out symbol table,
 * a classic off-by-one trap.
 */
#define MULTIBOOT_FLAG_MODS (1u << 3)

/*
 * The kernel window maps the first 128 MiB of physical memory (that is what
 * kmain's page_map_4mb_range(&krn_pt, ..., 0x8000000, ...) covers), so a
 * physical address at or above it cannot be translated with pa_to_va().
 */
#define MBI_PA_LIMIT    (0x8000000u)

static volatile u32* mbi = 0;

int mboot_init(u32 mbi_pa)
{
    if (!mbi_pa || mbi_pa >= MBI_PA_LIMIT)
        return E_INVAL;

    mbi = (volatile u32*)pa_to_va(mbi_pa);

    return 0;
}

int mboot_module_count(void)
{
    if (!mbi || !(mbi[MBI_FLAGS] & MULTIBOOT_FLAG_MODS))
        return 0;

    return (int)mbi[MBI_MODS_COUNT];
}

int mboot_module_get(int i, u8** start, u8** end)
{
    volatile u32* mods;
    u32 mods_pa;
    u32 start_pa;
    u32 end_pa;

    if (!start || !end)
        return E_INVAL;
    if (!mbi || !(mbi[MBI_FLAGS] & MULTIBOOT_FLAG_MODS))
        return E_NOTFOUND;
    if (i < 0 || (u32)i >= mbi[MBI_MODS_COUNT])
        return E_NOTFOUND;

    mods_pa = mbi[MBI_MODS_ADDR];
    if (!mods_pa || mods_pa >= MBI_PA_LIMIT)
        return E_INVAL;

    mods = (volatile u32*)pa_to_va(mods_pa);
    start_pa = mods[i * 4 + MOD_START];
    end_pa   = mods[i * 4 + MOD_END];

    if (!start_pa || end_pa < start_pa || end_pa > MBI_PA_LIMIT)
        return E_INVAL;

    *start = (u8*)pa_to_va(start_pa);
    *end   = (u8*)pa_to_va(end_pa);

    return 0;
}

const char* mboot_module_name(int i)
{
    volatile u32* mods;
    u32 mods_pa;
    u32 str_pa;

    if (!mbi || !(mbi[MBI_FLAGS] & MULTIBOOT_FLAG_MODS))
        return 0;
    if (i < 0 || (u32)i >= mbi[MBI_MODS_COUNT])
        return 0;

    mods_pa = mbi[MBI_MODS_ADDR];
    if (!mods_pa || mods_pa >= MBI_PA_LIMIT)
        return 0;

    mods = (volatile u32*)pa_to_va(mods_pa);
    str_pa = mods[i * 4 + MOD_STRING];
    if (!str_pa || str_pa >= MBI_PA_LIMIT)
        return 0;

    return (const char*)pa_to_va(str_pa);
}

int module_name_matches(const char* module, const char* name)
{
    u32 mlen = 0;
    u32 nlen = 0;

    if (!module || !name || !name[0])
        return 0;

    while (module[mlen])
        mlen++;
    while (name[nlen])
        nlen++;

    if (nlen > mlen)
        return 0;

    return strcmp(module + (mlen - nlen), name) == 0;
}
