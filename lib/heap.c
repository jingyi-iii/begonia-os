#include "heap.h"
#include "list.h"
#include "spinlock.h"
#include "string.h"

#define HEAP_TOTAL_SIZE (1024 * 1024 * 16)
#define HEAP_ALIGN      (8)
#define HEAP_HDR_SIZE   ((sizeof(heapchunk) + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1))

typedef struct {
    u32         size;
    u8          used;
    list_node   this_node;
} heapchunk;

typedef struct {
    u8*         pool;
    u32         avail_size;
    spinlock    splock;
    u8          inited;
    list_node   head_node;
} heappool;

static u8 krn_heap_space[HEAP_TOTAL_SIZE] = {0};
static heappool krn_pool __attribute__((aligned(HEAP_ALIGN))) = {
    .pool = krn_heap_space,
    .avail_size = HEAP_TOTAL_SIZE,
    .splock = SPINLOCK_INIT,
    .inited = 0,
    .head_node = {0},
};

static void heap_init(heappool* pool)
{
    heapchunk* chunk = (heapchunk*)pool->pool;
    u32 eflags = spinlock_lock_irqsave(&pool->splock);
    /*
     * avail_size tracks the total free bytes (header + payload of every
     * free chunk), so it starts at the whole pool size.  kmalloc() then
     * subtracts req_size (the new chunk's header+payload) and kfree()
     * adds back the freed chunk's header+payload — see the note in
     * kfree() about why that accounting must happen before merging.
     */
    pool->inited = 1;
    pool->head_node.prev = &pool->head_node;
    pool->head_node.next = &pool->head_node;

    chunk->size = HEAP_TOTAL_SIZE - HEAP_HDR_SIZE;
    chunk->used = 0;
    list_add(&chunk->this_node, &pool->head_node);
    spinlock_unlock_irqrestore(&pool->splock, eflags);
}

/*
 * One allocator, one lock.  align is the minimum alignment of the payload
 * the caller gets back: HEAP_ALIGN for malloc(), anything larger (a power of
 * two) for malloc_aligned().
 *
 * A chunk header always sits exactly HEAP_HDR_SIZE bytes in front of the
 * payload, because free() recovers the header with "pointer - HEAP_HDR_SIZE".
 * So an over-aligned request cannot simply return a rounded-up address: the
 * header has to move with it.  When the free chunk's payload is not aligned,
 * the chunk is split in two:
 *
 *      chunk            base            data           tail
 *      +--------+-------+------+--------+--------------+--------+
 *      | header |  ...  | pad  | header |    payload   | header |
 *      +--------+-------+------+--------+--------------+--------+
 *                              ^ the user's chunk, at data - HEAP_HDR_SIZE
 *
 * The pad (and the tail, when it is big enough for a header) stay free, so
 * the wasted bytes come back to the pool on the next merge.
 */
static void* heap_malloc(heappool* pool, unsigned int alloc_size, unsigned int align)
{
    heapchunk* chunk = 0;
    heapchunk* user = 0;
    heapchunk* tail = 0;
    u8* base = 0;
    u8* data = 0;
    void* ret_addr = 0;
    unsigned int aligned_size;
    unsigned int req_size;
    unsigned int prefix = 0;
    unsigned int consumed;
    unsigned int remaining;

    if (!pool->inited)
        heap_init(pool);

    if ((align & (align - 1)) != 0)                 /* power of two only */
        return 0;
    if (align < HEAP_ALIGN)
        align = HEAP_ALIGN;                         /* 0 means "no requirement" */

    /* Reject requests that cannot fit the pool: this also prevents u32
     * wrap of req_size (HEAP_HDR_SIZE + aligned_size) for absurd sizes,
     * which would bypass the avail_size check and corrupt the heap. */
    if (alloc_size > HEAP_TOTAL_SIZE || align > HEAP_TOTAL_SIZE)
        return 0;
    if (alloc_size > (0xFFFFFFFFu - (HEAP_ALIGN - 1)))   /* align overflow */
        return 0;

    /* Round the request up so every payload is HEAP_ALIGN-aligned
     * (chunk bases are HEAP_ALIGN multiples and so is the header). */
    aligned_size = (alloc_size + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1);
    req_size = HEAP_HDR_SIZE + aligned_size;
    if (align > HEAP_ALIGN)
        req_size += align;              /* room to shift + split, see below */

    u32 eflags = spinlock_lock_irqsave(&pool->splock);
    if (req_size > pool->avail_size)
        goto ALLOC_FAIL;

    list_for_each(node, &pool->head_node) {
        heapchunk* p = list_entry(node, heapchunk, this_node);
        if (p->size >= req_size && !p->used) {
            chunk = p;
            break;
        }
    }

    if (!chunk)
        goto ALLOC_FAIL;

    base = (u8*)chunk + HEAP_HDR_SIZE;
    data = base;
    if (align > HEAP_ALIGN && ((u32)base & (align - 1)) != 0) {
        /* Shift far enough that the user chunk's header still starts at or
         * after base: the payload lands on the next align boundary with a
         * header-sized gap in front of it. */
        data = (u8*)(((u32)base + HEAP_HDR_SIZE + align - 1) & ~(align - 1));
        prefix = (unsigned int)((u32)data - HEAP_HDR_SIZE - (u32)base);
    }

    consumed = prefix + HEAP_HDR_SIZE + aligned_size;
    if (consumed > chunk->size)
        goto ALLOC_FAIL;

    remaining = chunk->size - consumed;

    if (prefix) {
        user            = (heapchunk*)(data - HEAP_HDR_SIZE);
        user->size      = aligned_size;
        user->used      = 1;
        list_init(&user->this_node);
        list_add(&user->this_node, &chunk->this_node);

        chunk->size = prefix;           /* the pad in front stays free */
        chunk->used = 0;
    } else {
        user            = chunk;
        user->size      = aligned_size;
        user->used      = 1;
    }

    if (remaining >= HEAP_HDR_SIZE) {
        tail            = (heapchunk*)((u8*)user + HEAP_HDR_SIZE + aligned_size);
        tail->size      = remaining - HEAP_HDR_SIZE;
        tail->used      = 0;
        list_init(&tail->this_node);
        list_add(&tail->this_node, &user->this_node);
    }

    ret_addr = (u8*)user + HEAP_HDR_SIZE;
    memset(ret_addr, 0, alloc_size);

    pool->avail_size -= HEAP_HDR_SIZE + aligned_size;
    spinlock_unlock_irqrestore(&pool->splock, eflags);

    return ret_addr;

ALLOC_FAIL:
    spinlock_unlock_irqrestore(&pool->splock, eflags);
    return 0;
}

static void heap_free(heappool* pool, void* pointer)
{
    heapchunk* chunk = 0;
    heapchunk* free_chunk = 0;

    if (!pointer)
        return;

    u32 eflags = spinlock_lock_irqsave(&pool->splock);
    free_chunk = (heapchunk *)((u8*)pointer - HEAP_HDR_SIZE);

    if (!free_chunk->used) {
        spinlock_unlock_irqrestore(&pool->splock, eflags);
        return;
    }

    /*
     * Account the newly freed space BEFORE merging.  avail_size tracks
     * the total free bytes (header + payload of every free chunk).
     * Free neighbours were already counted while they were free, so the
     * merge pass below must only reorganise the chunk list — adding
     * their sizes here again would double-count and let kmalloc() hand
     * out more memory than the pool actually contains.
     */
    pool->avail_size += HEAP_HDR_SIZE + free_chunk->size;

    // merge with next chunk
    if (list_next(&free_chunk->this_node) != &pool->head_node) {
        chunk = list_entry(list_next(&free_chunk->this_node), heapchunk, this_node);
        if (!chunk->used) {
            free_chunk->size += HEAP_HDR_SIZE + chunk->size;
            list_del(&chunk->this_node);
        }
    }

    // merge with prev chunk
    if (list_prev(&free_chunk->this_node) != &pool->head_node) {
        chunk = list_entry(list_prev(&free_chunk->this_node), heapchunk, this_node);
        if (!chunk->used) {
            chunk->size += HEAP_HDR_SIZE + free_chunk->size;
            list_del(&free_chunk->this_node);
            free_chunk = chunk;
        }
    }

    free_chunk->used = 0;
    spinlock_unlock_irqrestore(&pool->splock, eflags);
}

void* malloc(unsigned int alloc_size)
{
    return heap_malloc(&krn_pool, alloc_size, HEAP_ALIGN);
}

void* malloc_aligned(unsigned int alloc_size, unsigned int align)
{
    return heap_malloc(&krn_pool, alloc_size, align);
}

void free(void* p)
{
    heap_free(&krn_pool, p);
}
