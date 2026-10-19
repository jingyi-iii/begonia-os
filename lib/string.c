#include "string.h"

void* memset(void* dst, int value, u32 n)
{
    u8* p = dst;

    while (n--)
        *p++ = (u8)value;

    return dst;
}

void* memcpy(void* dst, const void* src, u32 n)
{
    u8* d = dst;
    const u8* s = src;

    while (n--)
        *d++ = *s++;

    return dst;
}
