#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "memlib.h"
#include "mm.h"

/*
 * Test driver for the IMPLICIT free list allocator.
 * Covers: malloc, free, coalesce, realloc (shrink/grow/in-place/fallback),
 * calloc (zeroing + overflow), alignment, and edge cases.
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
    section("Test 3: Heap state after malloc");
    checkheap(1);

    /* ───────────────────────── Free + coalesce ───────────────────────── */
    section("Test 4: Free p2 (no adjacent free blocks yet)");
    mm_free(p2);
    checkheap(1);

    section("Test 5: Free p1 (p1 + p2's old space should coalesce)");
    mm_free(p1);
    checkheap(1);

    /* ───────────────────────── Reuse freed block ───────────────────────── */
    section("Test 6: Reuse freed block");
    void *p4 = mm_malloc(16);
    printf("p4=%p (should reuse coalesced p1/p2 area)\n", p4);
    checkheap(1);

    /* ───────────────────────── Null / zero-size requests ───────────────────────── */
    section("Test 7: malloc(0) returns NULL");
    void *p5 = mm_malloc(0);
    printf("malloc(0) = %p (should be NULL)\n", p5);
    assert(p5 == NULL);

    /* ───────────────────────── Large allocation (heap extension) ───────────────────────── */
    section("Test 8: Large allocation triggers extend_heap");
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
    section("Test 10: realloc(ptr, 0) behaves like free");
    void *r1_result = mm_realloc(r1, 0);
    printf("realloc(r1, 0) = %p (should be NULL)\n", r1_result);
    assert(r1_result == NULL);
    checkheap(1);

    /* ───────────────────────── realloc: shrink in place ───────────────────────── */
    section("Test 11: realloc shrink (large -> small, in place)");
    void *r2 = mm_malloc(200);
    printf("r2 (before shrink) = %p\n", r2);
    void *r2_shrunk = mm_realloc(r2, 8);
    printf("r2 (after shrink)  = %p (should be same address)\n", r2_shrunk);
    assert(r2_shrunk == r2);
    checkheap(1);

    /* ───────────────────────── realloc: grow, absorb next free block ───────────────────────── */
    section("Test 12: realloc grow - absorb adjacent free block");
    /*
     * r2_shrunk left a free remainder right after it (from the shrink).
     * Growing r2_shrunk back up should absorb that remainder in place.
     */
    void *r2_grown = mm_realloc(r2_shrunk, 64);
    printf("r2 (after grow)    = %p\n", r2_grown);
    assert(r2_grown != NULL);
    assert((size_t)r2_grown % 8 == 0);
    /* Write/readback to confirm the memory is usable */
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
    section("Test 13: realloc grow at heap end (extend in place)");
    void *r3 = mm_malloc(64);
    printf("r3 = %p\n", r3);
    /* r3 should now be the last real block before the epilogue */
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
    printf("src = %p -> \"%s\"\n", src, src);

    /* Allocate a blocker right after src so realloc cannot grow in place */
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
    /* values that overflow nmemb*size on 32-bit size_t */
    void *c3 = mm_calloc((size_t)0xFFFFFFFF, (size_t)0xFFFFFFFF);
    printf("calloc(huge,huge) = %p (should be NULL, overflow caught)\n", c3);
    assert(c3 == NULL);

    /* ───────────────────────── Free everything, full coalesce ───────────────────────── */
    section("Test 18: Free everything - verify full coalescing");
    mm_free(p3);
    mm_free(p4);
    mm_free(p6);
    mm_free(r2_grown);
    mm_free(r3_grown);
    mm_free(grown);
    mm_free(blocker);
    mm_free(arr);
    checkheap(1);

    /* ───────────────────────── Stress: many small allocations ───────────────────────── */
    section("Test 19: Stress test - many small allocations and frees");
#define N 50
    void *ptrs[N];
    for (int i = 0; i < N; i++)
    {
        ptrs[i] = mm_malloc(16 + (i % 5) * 8);
        assert(ptrs[i] != NULL);
        assert((size_t)ptrs[i] % 8 == 0);
    }
    /* Free every other one */
    for (int i = 0; i < N; i += 2)
        mm_free(ptrs[i]);
    /* Reallocate into the holes */
    for (int i = 0; i < N; i += 2)
    {
        ptrs[i] = mm_malloc(16);
        assert(ptrs[i] != NULL);
        assert((size_t)ptrs[i] % 8 == 0);
    }
    /* Free everything */
    for (int i = 0; i < N; i++)
        mm_free(ptrs[i]);
    printf("Stress test completed without crashes\n");
    checkheap(1);

    printf("\n=== ALL IMPLICIT LIST TESTS PASSED ===\n");
    return 0;
}
