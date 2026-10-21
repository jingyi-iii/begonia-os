#ifndef BOOT_H
#define BOOT_H

#include "typedefs.h"

/* Filled in by _start: what the loader passed in %eax and %ebx. */
extern u32 boot_magic;      /* 0x2BADB002 when the loader is Multiboot */
extern u32 boot_mbi;        /* physical address of the multiboot info  */

void exit_from_qemu(void);

#endif
