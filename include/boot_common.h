#ifndef BOOT_COMMON_H
#define BOOT_COMMON_H

#define u8  unsigned char
#define u16 unsigned short
#define u32 unsigned int
#define u64 unsigned long long

/* Filled in by _start: what the loader passed in %eax and %ebx. */
extern u32 boot_magic;      /* 0x2BADB002 when the loader is Multiboot */
extern u32 boot_mbi;        /* physical address of the multiboot info  */

#endif
