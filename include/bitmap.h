#ifndef BITMAP_H
#define BITMAP_H

#include "typedefs.h"

static inline u32 bitmap_block_count(u32 total_size, u32 block_size)
{
    return (total_size / block_size + (total_size % block_size != 0));
}

static inline u32 bitmap_byte_count(u32 nblocks)
{
    return (nblocks / 8 + (nblocks % 8 != 0));
}

static inline void bitmap_set(u8* map, u32 block)
{
    map[block / 8] |= (u8)(1U << (block % 8));
}

static inline void bitmap_clear(u8* map, u32 block)
{
    map[block / 8] &= (u8)(~(1U << (block % 8)));
}

static inline int bitmap_test(u8* map, u32 block)
{
    return (map[block / 8] >> (block % 8)) & 1;
}

#endif
