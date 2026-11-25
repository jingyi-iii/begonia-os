#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include "typedefs.h"

/*
 * Physical address of the multiboot_info that GRUB passes in EBX on entry,
 * saved by boot.S before anything clobbers it.  It is a *physical* address
 * and it only reads as a pointer while the current page table still
 * identity-maps low memory.
 */
extern u32 boot_mbi;

/*
 * mboot_init - point the parser at the multiboot info.
 *
 * @mbi_pa is the physical address from boot_mbi.  Call this while boot_mbi is
 * still readable - before switching to a page table without the low identity
 * map.  From then on the parser reaches the info through the kernel window.
 *
 * Returns 0, or E_INVAL when @mbi_pa cannot be mapped by the window.
 */
int mboot_init(u32 mbi_pa);

/* Number of modules GRUB loaded (multiboot_info.flags bit 3). */
int mboot_module_count(void);

/*
 * Start/end of the i-th module, as kernel virtual addresses: the image is in
 * physical memory, so the parser translates it with pa_to_va().  Returns 0 on
 * success or a negative errno.
 */
int mboot_module_get(int i, u8** start, u8** end);

/* Filename/cmdline string GRUB attached to the i-th module, as a kernel
 * virtual address (may be NULL or empty). */
const char* mboot_module_name(int i);

int module_name_matches(const char* module, const char* name);

#endif
