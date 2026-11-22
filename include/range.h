#ifndef RANGE_H
#define RANGE_H

#include "typedefs.h"

/*
 * Checked arithmetic on u32 ranges.
 *
 * A range is the half-open interval [start, start + size).  Everything is
 * computed in u64 first and only narrowed once the result is known to fit,
 * so callers do not have to repeat the same overflow guards.
 *
 * Conventions:
 *   - size == 0 is an empty range: it never fits, contains nothing and
 *     never overlaps another range.
 *   - a range end may be exactly 2^32 (covering the last byte of the
 *     32-bit address space); pass a window limit as u64, e.g.
 *     0x100000000ULL for a full 4 GiB window.
 *   - align must be a power of two.
 */

#define U32_RANGE_TOP   0x100000000ULL      /* exclusive end of the u32 space */

static inline int u32_pages_to_bytes(u32 page_size, u32 page_cnt, u32* out)
{
    u64 bytes = (u64)page_cnt * (u64)page_size;

    if (!page_cnt || bytes > 0xFFFFFFFFu || !out)
        return 0;

    *out = (u32)bytes;
    return 1;
}

/* a + b, if it fits in u32.  Returns 1 on success; *out is untouched on 0. */
static inline int u32_add_checked(u32 a, u32 b, u32* out)
{
    u64 sum = (u64)a + b;

    if (!out || sum > 0xFFFFFFFFu)
        return 0;

    *out = (u32)sum;
    return 1;
}

/* a * b, if it fits in u32.  Returns 1 on success; *out is untouched on 0. */
static inline int u32_mul_checked(u32 a, u32 b, u32* out)
{
    u64 product = (u64)a * b;

    if (!out || product > 0xFFFFFFFFu)
        return 0;

    *out = (u32)product;
    return 1;
}

/* Round value up to the next multiple of align (a power of two). */
static inline int u32_align_up(u32 value, u32 align, u32* out)
{
    u64 rounded;

    if (!out || align == 0 || (align & (align - 1)) != 0)
        return 0;

    rounded = ((u64)value + align - 1) & ~(u64)(align - 1);
    if (rounded > 0xFFFFFFFFu)
        return 0;

    *out = (u32)rounded;
    return 1;
}

/* Exclusive end of a range.  Returns 2^32 for a range that ends at the top. */
static inline u64 u32_range_end(u32 start, u32 size)
{
    return (u64)start + size;
}

/* size != 0 and the range does not wrap past the top of the u32 space. */
static inline int u32_range_fits(u32 start, u32 size)
{
    return size != 0 && u32_range_end(start, size) <= U32_RANGE_TOP;
}

/* size != 0 and [start, start + size) lies inside [base, limit). */
static inline int u32_range_within(u32 start, u32 size, u32 base, u64 limit)
{
    return start >= base &&
           u32_range_fits(start, size) &&
           u32_range_end(start, size) <= limit;
}

/* addr lies inside [start, start + size). */
static inline int u32_range_contains(u32 start, u32 size, u32 addr)
{
    return size != 0 &&
           (u64)addr >= start &&
           (u64)addr < u32_range_end(start, size);
}

/* The two ranges share at least one byte. */
static inline int u32_ranges_overlap(u32 a_start, u32 a_size,
                                     u32 b_start, u32 b_size)
{
    if (a_size == 0 || b_size == 0)
        return 0;

    return (u64)a_start < u32_range_end(b_start, b_size) &&
           (u64)b_start < u32_range_end(a_start, a_size);
}

#endif /* RANGE_H */
