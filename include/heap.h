#ifndef HEAP_H
#define HEAP_H

#include "typedefs.h"

void* malloc(unsigned int alloc_size);

/* Same as malloc(), but the returned pointer is a multiple of align, which
 * must be a power of two (0 and small values mean the pool's own
 * alignment).  free() works on both. */
void* malloc_aligned(unsigned int alloc_size, unsigned int align);

void free(void* p);

#endif
