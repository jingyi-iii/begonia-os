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
    u32 eflags = spinlock_lock(&pool->splock);
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
    spinlock_unlock(&pool->splock, eflags);
}

static void* heap_malloc(heappool* pool, unsigned int alloc_size)
{
    heapchunk* chunk = 0;
    heapchunk* new_chunk = 0;
    void* ret_addr = 0;
    unsigned int aligned_size;
    unsigned int req_size;

    if (!pool->inited)
        heap_init(pool);

    /* Round the request up so every payload is HEAP_ALIGN-aligned
     * (chunk bases are HEAP_ALIGN multiples and so is the header). */
    /* Reject requests that cannot fit the pool: this also prevents u32
     * wrap of req_size (HEAP_HDR_SIZE + aligned_size) for absurd sizes,
     * which would bypass the avail_size check and corrupt the heap. */
    if (alloc_size > HEAP_TOTAL_SIZE)
        return 0;
    if (alloc_size > (0xFFFFFFFFu - (HEAP_ALIGN - 1)))   /* align overflow */
        return 0;
    aligned_size = (alloc_size + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1);
    req_size = HEAP_HDR_SIZE + aligned_size;

    u32 eflags = spinlock_lock(&pool->splock);
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

    new_chunk = (heapchunk*)((u8*)chunk + req_size);
    if ((u8*)new_chunk + HEAP_HDR_SIZE <= (u8*)pool->pool + HEAP_TOTAL_SIZE) {
        new_chunk->size = chunk->size - req_size;
        new_chunk->used = 0;
        list_add(&new_chunk->this_node, &chunk->this_node);
    }

    ret_addr = (u8*)chunk + HEAP_HDR_SIZE;
    memset(ret_addr, 0, alloc_size);
    chunk->used = 1;
    chunk->size = aligned_size;

    pool->avail_size -= req_size;
    spinlock_unlock(&pool->splock, eflags);

    return (void*)ret_addr;

ALLOC_FAIL:
    spinlock_unlock(&pool->splock, eflags);
    return 0;
}

static void heap_free(heappool* pool, void* pointer)
{
    heapchunk* chunk = 0;
    heapchunk* free_chunk = 0;

    if (!pointer)
        return;

    u32 eflags = spinlock_lock(&pool->splock);
    free_chunk = (heapchunk *)((u8*)pointer - HEAP_HDR_SIZE);

    if (!free_chunk->used) {
        spinlock_unlock(&pool->splock, eflags);
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
    spinlock_unlock(&pool->splock, eflags);
}

void* malloc(unsigned int alloc_size)
{
    return heap_malloc(&krn_pool, alloc_size);
}

void free(void* p)
{
    heap_free(&krn_pool, p);
}
