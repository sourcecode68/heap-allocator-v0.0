#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef USE_MM
#include "mm.h"
#include "memlib.h"
#define MALLOC mm_malloc
#define FREE mm_free
#define REALLOC mm_realloc
#else
#define MALLOC malloc
#define FREE free
#define REALLOC realloc
#endif

#define N 1000000

static double elapsed(struct timespec a, struct timespec b)
{
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

static size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024};

int main(void)
{
#ifdef USE_MM
    mem_init();
    mm_init();
#endif

    void **ptrs = calloc(N, sizeof(void *)); /* calloc: NULL-init */
    size_t *reqs = malloc(N * sizeof(size_t));

    if (!ptrs || !reqs)
    {
        fprintf(stderr, "Failed to allocate benchmark arrays\n");
        return 1;
    }

    srand(42);
    for (size_t i = 0; i < N; i++)
        reqs[i] = sizes[rand() % 8];

    struct timespec t0, t1, t2, t3, t4;
    int errors = 0;

    /* ── Phase 1: allocate N blocks ── */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t i = 0; i < N; i++)
    {
        ptrs[i] = MALLOC(reqs[i]);
        if (!ptrs[i])
        {
            fprintf(stderr, "Alloc failed at %zu\n", i);
            break;
        }
        /* write a sentinel byte for correctness check */
        *(unsigned char *)ptrs[i] = (unsigned char)(i & 0xFF);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    /* ── Phase 2: free every other block ── */
    for (size_t i = 0; i < N; i += 2)
        FREE(ptrs[i]);
    clock_gettime(CLOCK_MONOTONIC, &t2);

    /* ── Phase 3: reuse freed slots with 64-byte allocs ── */
    for (size_t i = 0; i < N; i += 2)
    {
        ptrs[i] = MALLOC(64);
        if (!ptrs[i])
        {
            fprintf(stderr, "Reuse alloc failed at %zu\n", i);
            break;
        }
        *(unsigned char *)ptrs[i] = (unsigned char)(i & 0xFF);
    }
    clock_gettime(CLOCK_MONOTONIC, &t3);

    /* ── Correctness: verify sentinels in odd slots (never freed) ── */
    for (size_t i = 1; i < N; i += 2)
    {
        if (*(unsigned char *)ptrs[i] != (unsigned char)(i & 0xFF))
        {
            fprintf(stderr,
                    "CORRUPTION at odd slot %zu: expected %u got %u\n",
                    i,
                    (unsigned)(i & 0xFF),
                    (unsigned)*(unsigned char *)ptrs[i]);
            errors++;
            if (errors > 10)
            {
                fprintf(stderr, "... (stopping)\n");
                break;
            }
        }
    }

    /* ── Phase 4: free everything ── */
    for (size_t i = 0; i < N; i++)
        FREE(ptrs[i]);
    clock_gettime(CLOCK_MONOTONIC, &t4);

    /* ── Report ── */
    double p1 = elapsed(t0, t1);
    double p2 = elapsed(t1, t2);
    double p3 = elapsed(t2, t3);
    double p4 = elapsed(t3, t4);
    double total = elapsed(t0, t4);
    long ops = (long)N + N / 2 + N / 2 + N; /* 3N */

    printf("\n=== MIXED SIZE REUSE BENCHMARK ===\n");
    printf("Blocks        : %d\n", N);
    printf("Total ops     : %ld\n", ops);
    printf("\nPer-phase breakdown:\n");
    printf("  Phase 1 (alloc N)          : %8.4f s  —  %6.2f M ops/s\n",
           p1, (double)N / p1 / 1e6);
    printf("  Phase 2 (free  N/2)        : %8.4f s  —  %6.2f M ops/s\n",
           p2, (double)(N / 2) / p2 / 1e6);
    printf("  Phase 3 (alloc N/2 reuse)  : %8.4f s  —  %6.2f M ops/s\n",
           p3, (double)(N / 2) / p3 / 1e6);
    printf("  Phase 4 (free  N)          : %8.4f s  —  %6.2f M ops/s\n",
           p4, (double)N / p4 / 1e6);
    printf("\nTotal time    : %.6f s\n", total);
    printf("Overall ops/s : %.2f million\n", (double)ops / total / 1e6);
    printf("Correctness   : %s\n", errors == 0 ? "PASS" : "FAIL");

#ifdef USE_MM
    printf("Heap size     : %zu bytes  (%.1f MB)\n",
           mem_heapsize(), (double)mem_heapsize() / (1024 * 1024));
#endif

    free(ptrs);
    free(reqs);
    return errors ? 1 : 0;
}