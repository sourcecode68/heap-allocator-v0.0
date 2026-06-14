# Heap Allocator

A dynamic memory allocator written in C, implementing the full
`malloc` / `free` / `realloc` / `calloc` API without using libc's
allocator. Built as a learning exercise in systems programming and
heap internals.

Two allocator strategies are implemented and compared:
an **implicit free list** and an **explicit doubly-linked free list**.

---

## Design

### Block layout

Every block carries a 4-byte **header** and 4-byte **footer**, each
encoding the block size and an allocated bit packed into one word:

```
[ HDR (4B) | payload ... | FTR (4B) ]
           ^bp (pointer returned to caller)
```

Size is always a multiple of 8, so the bottom 3 bits are free the
lowest bit stores the allocated flag. This is called a **boundary
tag** and enables O(1) navigation to both the next and previous
physical blocks without a separate data structure.

### Coalescing

On every `free`, the allocator checks both physical neighbours and
merges in one of four cases:

```
prev alloc, next alloc  → no merge
prev alloc, next free   → merge with next
prev free,  next alloc  → merge with prev
prev free,  next free   → merge both
```

All four cases are O(1) because the footer of the previous block
is always at a fixed offset behind the current header.

### Heap backing

`memlib.c` backs the heap with `mmap(MAP_PRIVATE | MAP_ANONYMOUS)`
instead of `malloc`, giving the allocator a contiguous virtual
address region completely isolated from libc's own heap. `mem_sbrk`
is a thin pointer-advance over this region, modelling the real Unix
`sbrk(2)` syscall.

---

## Allocator Strategies

### Implicit Free List

`find_fit` walks every block in the heap sequentially from the
prologue, checking the alloc bit of each header. First-fit policy.

| Operation | Complexity         |
|-----------|--------------------|
| malloc    | O(all blocks)      |
| free      | O(1)               |
| coalesce  | O(1)               |

### Explicit Free List

Free blocks are linked into a doubly-linked list via `PRED` and
`SUCC` pointers stored inside the payload area of each free block.
`find_fit` walks only free blocks. LIFO insertion — freed blocks are
prepended to the list head.

Minimum block size is 16 bytes (header + PRED + SUCC + footer).

| Operation | Complexity         |
|-----------|--------------------|
| malloc    | O(free blocks)     |
| free      | O(1)               |
| coalesce  | O(1)               |

The known limitation of both designs: no size classes. A single
unsorted list means `find_fit` degrades under fragmentation.
Size segregated bins (as used in glibc) would reduce `malloc` to
O(1) in practice,that is the natural next step.

---

## Project Structure

```
heap-allocator/
├── include/
│   ├── mm.h
│   └── memlib.h
├── lib/
│   └── memlib.c          ← mmap-backed heap simulation
├── src/
│   ├── mm_implicit.c     ← implicit allocator
│   ├── mm_explicit.c     ← explicit allocator
│   ├── main_implicit.c   ← correctness tests (implicit)
│   ├── main_explicit.c   ← correctness tests (explicit)
│   └── bench_mark1_throughput.c
├── build/
├── bin/
└── Makefile
```

---

## Build and Run

```bash
# Build and run implicit allocator tests
make run-implicit

# Build and run explicit allocator tests
make run-explicit

#To run both
make run-all
# Benchmark: explicit allocator vs glibc
make run-bench
```

---

## Correctness Tests

Both allocators pass a shared test suite covering:

- 8-byte payload alignment
- Block splitting and remainder insertion
- All four coalesce cases
- `realloc` — shrink in place, absorb adjacent free block,
  epilogue extension, fallback copy with data preservation
- `calloc` zero-initialisation and multiplication overflow
- LIFO free-list ordering (explicit only)
- heap checker: physical heap walk via `NEXT_BLKP`
  cross-validated against free-list traversal via `SUCC` pointers

---

## Benchmark

**Workload:** 1M allocations of random sizes (8–1024 bytes),
free every other block, reallocate into freed slots at 64 bytes,
free all.

### Per-phase results

| Phase | Operation | Explicit | glibc |
|-------|-----------|----------|-------|
| 1 | alloc 1M blocks | 16.2 M ops/s | 11.7 M ops/s |
| 2 | free 500k blocks | 47.0 M ops/s | 35.3 M ops/s |
| 3 | alloc 500k reuse | 0.01 M ops/s | 12.7 M ops/s |
| 4 | free 1M blocks | 29.5 M ops/s | 13.9 M ops/s |
| **Total** | | **0.03 M ops/s** | **14.2 M ops/s** |

### Why Phase 1 and 2 beat glibc

The explicit allocator has lower per call overhead for small
working sets. Phase 1 hits a nearly-empty free list (one large
block from `mm_init`) and finds a fit immediately. Phase 2's
`mm_free` is a pure O(1) prepend — no coalescing fires because
every freed block has an allocated neighbour on both sides.
glibc pays thread-safety and bin-management overhead even in
single-threaded use.

### Why Phase 3 collapses

After Phase 2, the free list holds 500,000 nodes. Each `malloc(64)`
in Phase 3 calls `find_fit`, which walks the list from the head.
With no size classes, it cannot jump directly to a 64-byte block —
it must scan on average ~250,000 nodes per call.
Total: 500k × 250k ≈ 125 billion pointer dereferences.

glibc avoids this entirely with size-segregated bins: a `malloc(64)`
goes directly to the 64-byte free list in O(1).

**This benchmark makes the cost of an unsorted free list under
fragmentation concretely visible, and motivates size class design.**

---

## Author

Piyush Khanna
