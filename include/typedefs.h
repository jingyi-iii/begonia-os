#ifndef TYPEDEFS_H
#define TYPEDEFS_H

#define u8  unsigned char
#define u16 unsigned short
#define u32 unsigned int
#define u64 unsigned long long

/* Wide enough to hold a pointer.  32-bit now; a 64-bit target has to
 * widen this to unsigned long. */
#define uptr unsigned int

#endif
