#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "memlib.h"
#include "mm.h"

/*
 * Test driver for the EXPLICIT free list allocator.
 * Covers everything the implicit driver does, PLUS:
 *   - free-list-specific behaviour (LIFO order, PRED/SUCC integrity)
 *   - checkheap's free-list traversal catching "allocated block on
 *     free list" errors after place()/coalesce()
 */

static void section(const char *name)
{
    printf("\n========================================\n");
    printf(" %s\n", name);
    printf("========================================\n");
}

int main(void)
{
    mem_init();
    mm_init();

    /* ───────────────────────── mm_free(NULL) safety ───────────────────────── */
    section("Test 0: mm_free(NULL) is a safe no-op");
    mm_free(NULL);
    printf("mm_free(NULL) did not crash\n");
    checkheap(1);

    /* ───────────────────────── Basic malloc ───────────────────────── */
    section("Test 1: Basic malloc");
    void *p1 = mm_malloc(8);
    void *p2 = mm_malloc(16);
    void *p3 = mm_malloc(32);
    printf("p1=%p p2=%p p3=%p\n", p1, p2, p3);
    assert(p1 && p2 && p3);

    /* ───────────────────────── Alignment ───────────────────────── */
    section("Test 2: Alignment check (8-byte)");
    printf("p1 aligned: %d\n", (size_t)p1 % 8 == 0);
    printf("p2 aligned: %d\n", (size_t)p2 % 8 == 0);
    printf("p3 aligned: %d\n", (size_t)p3 % 8 == 0);
    assert((size_t)p1 % 8 == 0);
    assert((size_t)p2 % 8 == 0);
    assert((size_t)p3 % 8 == 0);

    /* ───────────────────────── Heap state ───────────────────────── */
    section("Test 3: Heap state after malloc (free list should hold the");
    printf("remainder of the initial CHUNKSIZE extension)\n");
    checkheap(1);

    /* ───────────────────────── Free + free-list insertion ───────────────────────── */
    section("Test 4: Free p2 - check it enters the free list (LIFO head)");
    mm_free(p2);
    checkheap(1);
    printf("Expect p2 to now be printed first in 'Free list:' (LIFO head)\n");

    section("Test 5: Free p1 - p1+p2 region coalesces, merged block re-inserted");
    mm_free(p1);
    checkheap(1);

    /* ───────────────────────── Reuse freed block via find_fit ───────────────────────── */
    section("Test 6: Reuse freed block (find_fit walks free list, not heap)");
    void *p4 = mm_malloc(16);
    printf("p4=%p (should reuse coalesced p1/p2 area)\n", p4);
    checkheap(1);

    /* ───────────────────────── Null / zero-size requests ───────────────────────── */
    section("Test 7: malloc(0) returns NULL");
    void *p5 = mm_malloc(0);
    printf("malloc(0) = %p (should be NULL)\n", p5);
    assert(p5 == NULL);

    /* ───────────────────────── Large allocation (heap extension) ───────────────────────── */
    section("Test 8: Large allocation triggers extend_heap + insert_free");
    void *p6 = mm_malloc(4096);
    printf("p6=%p aligned:%d\n", p6, (size_t)p6 % 8 == 0);
    assert(p6 != NULL);
    assert((size_t)p6 % 8 == 0);
    checkheap(1);

    /* ───────────────────────── realloc: NULL pointer == malloc ───────────────────────── */
    section("Test 9: realloc(NULL, size) behaves like malloc");
    void *r1 = mm_realloc(NULL, 24);
    printf("r1=%p (fresh allocation)\n", r1);
    assert(r1 != NULL);
    assert((size_t)r1 % 8 == 0);
    checkheap(1);

    /* ───────────────────────── realloc: size 0 == free ───────────────────────── */
    section("Test 10: realloc(ptr, 0) behaves like free (re-enters free list)");
    void *r1_result = mm_realloc(r1, 0);
    printf("realloc(r1, 0) = %p (should be NULL)\n", r1_result);
    assert(r1_result == NULL);
    checkheap(1);

    /* ───────────────────────── realloc: shrink, remainder enters free list ───────────────────────── */
    section("Test 11: realloc shrink - remainder inserted into free list");
    void *r2 = mm_malloc(200);
    printf("r2 (before shrink) = %p\n", r2);
    void *r2_shrunk = mm_realloc(r2, 8);
    printf("r2 (after shrink)  = %p (should be same address)\n", r2_shrunk);
    assert(r2_shrunk == r2);
    checkheap(1);
    printf("Expect the ~184-byte remainder on the free list now\n");

    /* ───────────────────────── realloc: grow, absorb + remove_free(next) ───────────────────────── */
    section("Test 12: realloc grow - absorb next free block via remove_free");
    void *r2_grown = mm_realloc(r2_shrunk, 64);
    printf("r2 (after grow)    = %p\n", r2_grown);
    assert(r2_grown != NULL);
    assert((size_t)r2_grown % 8 == 0);
    memset(r2_grown, 0xAB, 64);
    unsigned char *check = (unsigned char *)r2_grown;
    int ok = 1;
    for (int i = 0; i < 64; i++)
        if (check[i] != 0xAB)
            ok = 0;
    printf("payload write/readback OK: %d\n", ok);
    assert(ok);
    checkheap(1);

    /* ───────────────────────── realloc: grow at end of heap (epilogue case) ───────────────────────── */
    section("Test 13: realloc grow at heap end (extend in place, no free-list change)");
    void *r3 = mm_malloc(64);
    printf("r3 = %p\n", r3);
    void *r3_grown = mm_realloc(r3, 4096);
    printf("r3 (grown to 4096) = %p (likely same address, in-place extend)\n", r3_grown);
    assert(r3_grown != NULL);
    assert((size_t)r3_grown % 8 == 0);
    memset(r3_grown, 0xCD, 4096);
    checkheap(1);

    /* ───────────────────────── realloc: fallback (malloc+copy+free) ───────────────────────── */
    section("Test 14: realloc fallback - data preservation");
    char *src = (char *)mm_malloc(32);
    strcpy(src, "Hello, allocator!");
    printf("src      = %p -> \"%s\"\n", src, src);

    void *blocker = mm_malloc(16);
    printf("blocker  = %p (prevents in-place growth of src)\n", blocker);

    char *grown = (char *)mm_realloc(src, 256);
    printf("grown    = %p -> \"%s\"\n", grown, grown);
    printf("Data preserved: %d\n", strcmp(grown, "Hello, allocator!") == 0);
    assert(strcmp(grown, "Hello, allocator!") == 0);
    checkheap(1);

    /* ───────────────────────── calloc: basic zeroing ───────────────────────── */
    section("Test 15: calloc zero-initializes memory");
    int *arr = (int *)mm_calloc(10, sizeof(int));
    printf("arr = %p\n", arr);
    assert(arr != NULL);
    int all_zero = 1;
    for (int i = 0; i < 10; i++)
        if (arr[i] != 0)
            all_zero = 0;
    printf("all zero: %d\n", all_zero);
    assert(all_zero);
    checkheap(1);

    /* ───────────────────────── calloc: zero arguments ───────────────────────── */
    section("Test 16: calloc(0, n) and calloc(n, 0) return NULL");
    void *c1 = mm_calloc(0, 16);
    void *c2 = mm_calloc(16, 0);
    printf("calloc(0,16)=%p calloc(16,0)=%p (both should be NULL)\n", c1, c2);
    assert(c1 == NULL);
    assert(c2 == NULL);

    /* ───────────────────────── calloc: overflow detection ───────────────────────── */
    section("Test 17: calloc overflow check");
    void *c3 = mm_calloc((size_t)0xFFFFFFFF, (size_t)0xFFFFFFFF);
    printf("calloc(huge,huge) = %p (should be NULL, overflow caught)\n", c3);
    assert(c3 == NULL);

    /* ───────────────────────── Free everything, full coalesce + free-list check ───────────────────────── */
    section("Test 18: Free everything - verify full coalescing + free list");
    mm_free(p3);
    mm_free(p4);
    mm_free(p6);
    mm_free(r2_grown);
    mm_free(r3_grown);
    mm_free(grown);
    mm_free(blocker);
    mm_free(arr);
    checkheap(1);
    printf("checkheap should report NO 'allocated block on free list' errors\n");

    /* ───────────────────────── LIFO insertion order check ───────────────────────── */
    section("Test 19: LIFO free-list insertion order");
    /*
     * a, b, c, d, e are allocated contiguously. We free a, c, e (the
     * "odd" ones), leaving b and d allocated as spacers in between.
     * This prevents immediate coalescing from merging any of the
     * freed blocks into one - each remains a distinct free-list node.
     *
     *   [a free][b alloc][c free][d alloc][e free]
     *
     * Free order: a, then c, then e.
     *   insert_free(a) -> free_listp = a
     *   insert_free(c) -> free_listp = c -> a   (c not adjacent to a, no merge)
     *   insert_free(e) -> free_listp = e -> c -> a
     *
     * find_fit on the next malloc(32) walks from free_listp and should
     * return e first (LIFO head), since e was freed last.
     */
    void *a = mm_malloc(32);
    void *b = mm_malloc(32);
    void *c = mm_malloc(32);
    void *d = mm_malloc(32);
    void *e = mm_malloc(32);
    printf("a=%p b=%p c=%p d=%p e=%p\n", a, b, c, d, e);

    mm_free(a); /* free_listp -> a */
    mm_free(c); /* free_listp -> c -> a   (c not adjacent to a: b sits between) */
    mm_free(e); /* free_listp -> e -> c -> a */
    checkheap(1);

    /* Next malloc of size 32 should return e's block first (LIFO head) */
    void *reuse = mm_malloc(32);
    printf("reuse=%p (expected == e=%p, since e was freed last)\n", reuse, e);
    assert(reuse == e);
    checkheap(1);

    /* Clean up */
    mm_free(reuse);
    mm_free(b);
    // mm_free(c);
    mm_free(d);
    checkheap(1);

    /* ───────────────────────── Stress: many small allocations ───────────────────────── */
    section("Test 20: Stress - many small allocations and frees");
#define N 50
    void *ptrs[N];
    for (int i = 0; i < N; i++)
    {
        ptrs[i] = mm_malloc(16 + (i % 5) * 8);
        assert(ptrs[i] != NULL);
        assert((size_t)ptrs[i] % 8 == 0);
    }
    for (int i = 0; i < N; i += 2)
        mm_free(ptrs[i]);
    for (int i = 0; i < N; i += 2)
    {
        ptrs[i] = mm_malloc(16);
        assert(ptrs[i] != NULL);
        assert((size_t)ptrs[i] % 8 == 0);
    }
    for (int i = 0; i < N; i++)
        mm_free(ptrs[i]);
    printf("Stress test completed without crashes\n");
    checkheap(1);

    /* ───────────────────────── Free-list / heap traversal consistency ───────────────────────── */
    section("Test 21: Free list should contain exactly the free blocks");
    /*
     * After Test 20, everything is freed. checkheap's heap-walk pass
     * and free-list pass should agree: every free block in the heap
     * walk should also be reachable via free_listp, and vice versa.
     * checkheap(1) prints both - inspect manually for a single large
     * coalesced region appearing in both passes.
     */
    checkheap(1);

    printf("\n=== ALL EXPLICIT LIST TESTS PASSED ===\n");
    return 0;
}