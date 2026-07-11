# my_malloc

Note: this was written purely for learning. The code is messy, variable names are rough, and none of it is production quality. It did its job.

A custom memory allocator built from scratch in C. Implicit free list with boundary-tag coalescing, 16-byte aligned payloads, mmap heap. Built as a learning project to understand how allocators actually work under the hood.

## What it does

- mmap allocates a 2MB heap with prologue/epilogue sentinels
- First-fit search through an implicit free list
- Splitting on oversized free blocks
- Immediate coalescing on free using boundary tags (all four prev/next cases)
- Heap checker that walks the entire heap asserting invariants after every operation
- Randomised interleaved harness with seeded replay for deterministic debugging

## Results

Compared against system malloc/free on 100 random interleaved operations (macOS, CLOCK_MONOTONIC).

### Malloc

| Metric | my_malloc | system malloc |
|---|---|---|
| Throughput | ~1.2M ops/s | ~3.3M ops/s |
| p99 | 4000ns | 3000-4000ns |
| p50 | <1000ns (below clock resolution) | <1000ns (below clock resolution) |

my_malloc is roughly 3x slower than system malloc. This makes sense. System malloc uses size classes and per-thread caches that give it near O(1) allocation. Mine walks a free list every time, so allocation cost scales with the number of blocks. The explicit free list (searching only free blocks instead of all blocks) would reduce this gap but didn't get to it.

### Free

| Metric | my_free | system free |
|---|---|---|
| Throughput | ~9-18M ops/s | ~1.6M ops/s |
| p99 | 1000ns | 6000-12000ns |
| p50 | <1000ns (below clock resolution) | <1000ns (below clock resolution) |

my_free is 5 to 10x faster than system free. This is real and it makes sense. My free is just pointer arithmetic and a few writes for boundary-tag coalescing. System free handles thread safety, deferred coalescing, bin management, and cross-thread returns. My custom free does none of those.

The p99 tells the same story. My free tail (1000ns) is much better than system free tail (6000-12000ns), the thread safety and bookkeeping overhead shows up specifically in the tail.

## What I learned

Pointers and pointer arithmetic in C, data types and their widths, how a free-list allocator works end to end (allocation, splitting, freeing, coalescing), boundary tags as a constant-time coalescing mechanism, heap checker as an invariant verification, and measurement (throughput, p50, p99, utilisation).
