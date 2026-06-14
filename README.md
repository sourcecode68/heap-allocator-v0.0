# Heap Allocator

A custom dynamic memory allocator written in C that implements both implicit and explicit free-list allocation strategies.

## Features

- Custom `malloc`
- Custom `free`
- Custom `realloc`
- Custom `calloc`
- 8-byte alignment
- Boundary-tag coalescing
- Block splitting
- Heap extension using simulated memory
- Implicit free-list allocator
- Explicit doubly-linked free-list allocator
- Comprehensive correctness tests
- Throughput benchmarking

---

## Project Structure

```text
heap-allocator/
│
├── include/
│   ├── mm.h
│   └── memlib.h
│
├── lib/
│   └── memlib.c
│
├── src/
│   ├── mm_implicit.c
│   ├── mm_explicit.c
│   ├── main_implicit.c
│   ├── main_explicit.c
│   └── bench_mark1_throughput.c
│
├── build/
├── bin/
└── Makefile
```

---

## Allocator Designs

### Implicit Free List

The allocator scans the heap sequentially to find a suitable free block.

Operations:

- First-fit placement
- Block splitting
- Boundary-tag coalescing

Complexities:

| Operation | Complexity |
|------------|------------|
| malloc | O(n) |
| free | O(1) |
| coalesce | O(1) |

---

### Explicit Free List

Free blocks are maintained in a doubly-linked free list.

Operations:

- LIFO insertion
- Constant-time removal
- Block splitting
- Boundary-tag coalescing

Complexities:

| Operation | Complexity |
|------------|------------|
| malloc | O(free blocks) |
| free | O(1) |
| coalesce | O(1) |

---

## Building

### Implicit Allocator

```bash
make implicit
```

### Explicit Allocator

```bash
make explicit
```

---

## Running Tests

### Implicit

```bash
./bin/test_implicit
```

### Explicit

```bash
./bin/test_explicit
```

---

## Correctness Tests

The test suite verifies:

- malloc correctness
- free correctness
- block splitting
- coalescing
- realloc growth
- realloc shrink
- calloc initialization
- alignment guarantees
- stress testing

Both allocators pass all tests.

---

## Benchmark

Mixed-size reuse benchmark:

| Allocator | Throughput |
|------------|------------|
| Explicit Free List | Much faster |
| Implicit Free List | Slower due to linear heap scan |
| glibc malloc | Baseline |

Example result:

```text
glibc:
14.48 million ops/sec

implicit free list:
0.03 million ops/sec
```

This demonstrates the scalability limitations of heap-wide linear scans and motivates explicit free-list management.

---

## Concepts Demonstrated

- Dynamic memory allocation
- Heap organization
- Boundary tags
- Free-list management
- Memory fragmentation
- Coalescing
- Placement policies
- Systems programming in C

---

## Author

Piyush Khanna
