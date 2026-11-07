#include "mem_stats.h"

#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t size);
void __real_free(void *ptr);

#ifdef __ORBIS__
extern int32_t sceKernelAvailableFlexibleMemorySize(size_t *size);
#endif

#define POOL_MAX_BLOCK ((size_t)1 << 20)
/* Sizes go 32, 48, 64, 96, 128...: at most a third of a block is wasted. */
#define POOL_CLASSES 32
#define CLASS_DIRECT 0xFFFFFFFFu /* from libc, not from a free list */
#define BLOCK_MAGIC 0x4D4C4D454D504F4Full
#define LOCK_SPINS 200

/* Keeps the user pointer aligned to 16 bytes. */
typedef struct {
    uint64_t magic;
    uint32_t pool_class;
    uint32_t reserved;
    void *raw;   /* start of the libc block, for direct blocks */
    size_t size; /* bytes usable by the caller */
} block_header_t;
_Static_assert(sizeof(block_header_t) == 32, "the user pointer must stay aligned to 16");

typedef struct free_node {
    struct free_node *next;
} free_node_t;

static free_node_t *s_free_lists[POOL_CLASSES];
static atomic_flag s_pool_lock = ATOMIC_FLAG_INIT;

static _Atomic int64_t s_live_bytes;
static _Atomic int64_t s_live_blocks;
static _Atomic int64_t s_buckets[MEM_STATS_BUCKETS];
static _Atomic int64_t s_pool_bytes;
static _Atomic uint64_t s_failures;
static _Atomic uint64_t s_last_failed_size;

/* A plain spinlock: malloc runs before threads and locks are set up. */
static void pool_lock(void) {
    int spins = 0;
    while (atomic_flag_test_and_set_explicit(&s_pool_lock, memory_order_acquire)) {
        if (++spins >= LOCK_SPINS) {
            sched_yield();
            spins = 0;
        }
    }
}

static void pool_unlock(void) {
    atomic_flag_clear_explicit(&s_pool_lock, memory_order_release);
}

static size_t class_size(unsigned pool_class) {
    size_t power = (size_t)32 << (pool_class / 2);
    return (pool_class & 1) ? power + power / 2 : power;
}

static unsigned class_for(size_t size) {
    unsigned pool_class = 0;
    while (class_size(pool_class) < size)
        pool_class++;
    return pool_class;
}

static int bucket_for(size_t size) {
    static const size_t limits[MEM_STATS_BUCKETS - 1] = { 64, 256, 1024, 4096, 65536 };
    for (int i = 0; i < MEM_STATS_BUCKETS - 1; i++)
        if (size <= limits[i])
            return i;
    return MEM_STATS_BUCKETS - 1;
}

static void account(size_t size, int sign) {
    atomic_fetch_add_explicit(&s_live_bytes, sign * (int64_t)size, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_live_blocks, sign, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_buckets[bucket_for(size)], sign, memory_order_relaxed);
}

static void record_failure(size_t size) {
    atomic_fetch_add_explicit(&s_failures, 1, memory_order_relaxed);
    atomic_store_explicit(&s_last_failed_size, size, memory_order_relaxed);
}

static void *finish_block(void *user, uint32_t pool_class, void *raw, size_t size) {
    block_header_t *h = (block_header_t *)user - 1;
    h->magic = BLOCK_MAGIC;
    h->pool_class = pool_class;
    h->reserved = 0;
    h->raw = raw;
    h->size = size;
    account(size, 1);
    return user;
}

static void *direct_alloc(size_t size, size_t align) {
    size_t total;
    if (__builtin_add_overflow(size, align + sizeof(block_header_t), &total)) {
        record_failure(size);
        return NULL;
    }
    void *raw = __real_malloc(total);
    if (!raw) {
        record_failure(size);
        return NULL;
    }
    uintptr_t user = ((uintptr_t)raw + sizeof(block_header_t) + align - 1) & ~(uintptr_t)(align - 1);
    return finish_block((void *)user, CLASS_DIRECT, raw, size);
}

void *__wrap_malloc(size_t size) {
    if (size > POOL_MAX_BLOCK)
        return direct_alloc(size, 16);

    unsigned pool_class = class_for(size);
    size_t block = class_size(pool_class);

    pool_lock();
    free_node_t *node = s_free_lists[pool_class];
    if (node)
        s_free_lists[pool_class] = node->next;
    pool_unlock();
    if (node)
        return finish_block(node, pool_class, NULL, block);

    size_t total = block + sizeof(block_header_t);
    void *raw = __real_malloc(total);
    if (!raw) {
        record_failure(size);
        return NULL;
    }
    atomic_fetch_add_explicit(&s_pool_bytes, (int64_t)total, memory_order_relaxed);
    return finish_block((block_header_t *)raw + 1, pool_class, NULL, block);
}

void __wrap_free(void *ptr) {
    if (!ptr)
        return;
    block_header_t *h = (block_header_t *)ptr - 1;
    /* Only libc's own internals can hold a block without this header. */
    if (h->magic != BLOCK_MAGIC) {
        __real_free(ptr);
        return;
    }
    account(h->size, -1);
    if (h->pool_class == CLASS_DIRECT) {
        h->magic = 0;
        __real_free(h->raw);
        return;
    }
    free_node_t *node = ptr;
    pool_lock();
    node->next = s_free_lists[h->pool_class];
    s_free_lists[h->pool_class] = node;
    pool_unlock();
}

void *__wrap_calloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total))
        return NULL;
    void *p = __wrap_malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *__wrap_realloc(void *ptr, size_t size) {
    if (!ptr)
        return __wrap_malloc(size);
    block_header_t *h = (block_header_t *)ptr - 1;
    /* Only libc's own internals can hold a block without this header. */
    if (h->magic != BLOCK_MAGIC) {
        record_failure(size);
        return NULL;
    }
    size_t old = h->size;
    if (size <= old && size >= old / 2)
        return ptr;
    void *p = __wrap_malloc(size);
    if (!p)
        return NULL;
    memcpy(p, ptr, size < old ? size : old);
    __wrap_free(ptr);
    return p;
}

void *__wrap_memalign(size_t align, size_t size) {
    if (align < 16)
        align = 16;
    if (align & (align - 1))
        return NULL;
    return direct_alloc(size, align);
}

void *__wrap_aligned_alloc(size_t align, size_t size) {
    return __wrap_memalign(align, size);
}

int __wrap_posix_memalign(void **out, size_t align, size_t size) {
    if (align < sizeof(void *) || (align & (align - 1)))
        return EINVAL;
    void *p = __wrap_memalign(align, size);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

void mem_stats_get(mem_stats_t *out) {
    out->live_bytes = atomic_load_explicit(&s_live_bytes, memory_order_relaxed);
    out->live_blocks = atomic_load_explicit(&s_live_blocks, memory_order_relaxed);
    for (int i = 0; i < MEM_STATS_BUCKETS; i++)
        out->bucket_blocks[i] = atomic_load_explicit(&s_buckets[i], memory_order_relaxed);
    out->pool_bytes = atomic_load_explicit(&s_pool_bytes, memory_order_relaxed);
    out->failures = atomic_load_explicit(&s_failures, memory_order_relaxed);
    out->last_failed_size = atomic_load_explicit(&s_last_failed_size, memory_order_relaxed);
    out->flexible_free_bytes = -1;
#ifdef __ORBIS__
    size_t flexible = 0;
    if (sceKernelAvailableFlexibleMemorySize(&flexible) == 0)
        out->flexible_free_bytes = (int64_t)flexible;
#endif
}
