/* string.h - the memory helpers the kernel needs.  There is no libc, so these
 * are ours; -fno-builtin keeps GCC from turning the bodies into calls to
 * themselves. */
#ifndef STRING_H
#define STRING_H

#include "boot_common.h"

void* memset(void* dst, int value, u32 n);
void* memcpy(void* dst, const void* src, u32 n);

#endif
