/* Heap replacement and accounting, through the linker's --wrap of the malloc family.
 *
 * libc's malloc keeps growing during a stream although the memory in use stays flat:
 * packets and frames of many sizes fragment its heap until the console runs out. Up
 * to POOL_MAX_BLOCK bytes, blocks now come from free lists of fixed sizes and go back
 * to them, so the memory held is bounded by the peak use of each size. */
#ifndef MEM_STATS_H
#define MEM_STATS_H

#include <stddef.h>
#include <stdint.h>

#define MEM_STATS_BUCKETS 6

typedef struct {
    int64_t live_bytes;
    int64_t live_blocks;
    int64_t bucket_blocks[MEM_STATS_BUCKETS]; /* live blocks up to 64, 256, 1K, 4K, 64K, above */
    int64_t pool_bytes;  /* taken from libc for the free lists, never given back */
    uint64_t failures;
    uint64_t last_failed_size;
    int64_t flexible_free_bytes; /* -1 when unknown */
} mem_stats_t;

void mem_stats_get(mem_stats_t *out);

#endif
