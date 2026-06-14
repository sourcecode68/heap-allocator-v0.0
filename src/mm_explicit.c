#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "memlib.h"

/*
 * Explicit Free List Allocator
 *
 * Block layout (free block):
 *   [ HDR (4B) | PREV ptr (4B) | NEXT ptr (4B) | ... | FTR (4B) ]
 *              ^bp
 *
 * Block layout (allocated block):
 *   [ HDR (4B) | payload ...              | FTR (4B) ]
 *              ^bp
 *
 * The free list is doubly-linked and unsorted (LIFO insertion).
 * Minimum block size = 2*DSIZE = 16 bytes
 *   (HDR + PREV + NEXT + FTR, all 4 bytes each)
 */

/* ──────────────────────── Basic constants ──────────────────────── */
#define WSIZE 4                 /* Word / header / footer size (bytes) */
#define DSIZE 8                 /* Double word size (bytes) */
#define CHUNKSIZE (1 << 12)     /* Extend heap by this amount (bytes)  */
#define MIN_BLKSIZE (2 * DSIZE) /* Minimum block size = 16 bytes       */

#define MAX(x, y) ((x) > (y) ? (x) : (y))

/* Pack size + alloc bit into one word */
#define PACK(size, alloc) ((size) | (alloc))

/* Read / write a word at address p */
#define GET(p) (*(unsigned int *)(p))
#define PUT(p, val) (*(unsigned int *)(p) = (val))

/* Read size / alloc from a header or footer at p */
#define GET_SIZE(p) (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)

/* Header / footer of the block whose payload starts at bp */
#define HDRP(bp) ((char *)(bp) - WSIZE)
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/* Heap-level next / previous blocks */
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)))
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE((char *)(bp) - DSIZE))

/*
 * Free-list prev / next pointers stored inside the payload of free blocks.
 *   PRED(bp) — address of the word that holds the predecessor pointer
 *   SUCC(bp) — address of the word that holds the successor pointer
 *   GET_PRED / GET_SUCC — dereference those pointers as (char *)
 */
#define PRED(bp) ((char *)(bp))
#define SUCC(bp) ((char *)(bp) + WSIZE)
#define GET_PRED(bp) ((char *)GET(PRED(bp)))
#define GET_SUCC(bp) ((char *)GET(SUCC(bp)))

/* ──────────────────────── Globals ──────────────────────── */
static char *heap_listp; /* Always points to prologue block payload  */
static char *free_listp; /* Head of the explicit free list (or NULL) */

/* ──────────────────────── Prototypes ──────────────────────── */
void checkheap(int verbose);
static void *extend_heap(size_t words);
static void place(void *bp, size_t asize);
static void *find_fit(size_t asize);
static void *coalesce(void *bp);
static void insert_free(void *bp);
static void remove_free(void *bp);
static void printblock(void *bp);
static void checkblock(void *bp);

/* ══════════════════════════════════════════════════════════════════
 *  Free-list helpers
 * ══════════════════════════════════════════════════════════════════ */

/*
 * insert_free — prepend bp to the free list (LIFO).
 */
static void insert_free(void *bp)
{
    /* bp → old head */
    PUT(SUCC(bp), (unsigned int)free_listp);
    /* bp has no predecessor */
    PUT(PRED(bp), (unsigned int)NULL);

    if (free_listp != NULL)
        PUT(PRED(free_listp), (unsigned int)bp);

    free_listp = bp;
}

/*
 * remove_free — unlink bp from the free list.
 */
static void remove_free(void *bp)
{
    char *pred = GET_PRED(bp);
    char *succ = GET_SUCC(bp);

    if (pred != NULL)
        PUT(SUCC(pred), (unsigned int)succ); /* pred->next = succ */
    else
        free_listp = succ; /* bp was the head   */

    if (succ != NULL)
        PUT(PRED(succ), (unsigned int)pred); /* succ->prev = pred  */
}

/* ══════════════════════════════════════════════════════════════════
 *  mm_init
 * ══════════════════════════════════════════════════════════════════ */
int mm_init(void)
{
    free_listp = NULL;

    /* Allocate: padding + prologue hdr + prologue ftr + epilogue hdr */
    if ((heap_listp = mem_sbrk(4 * WSIZE)) == (void *)-1)
        return -1;

    PUT(heap_listp, 0);                            /* Alignment padding  */
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1)); /* Prologue header    */
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); /* Prologue footer    */
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));     /* Epilogue header    */
    heap_listp += (2 * WSIZE);                     /* bp of prologue     */

    if (extend_heap(CHUNKSIZE / WSIZE) == NULL)
        return -1;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 *  extend_heap
 * ══════════════════════════════════════════════════════════════════ */
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    /* Round up to maintain double-word alignment */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;

    if ((long)(bp = mem_sbrk(size)) == -1)
        return NULL;

    /* New free block */
    PUT(HDRP(bp), PACK(size, 0));         /* Free block header  */
    PUT(FTRP(bp), PACK(size, 0));         /* Free block footer  */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /* New epilogue       */

    /* Insert into free list before coalescing */
    insert_free(bp);
    return coalesce(bp);
}

/* ══════════════════════════════════════════════════════════════════
 *  coalesce
 *
 *  bp must already be in the free list (inserted before this call).
 *  Adjacent free neighbours are removed from the list, the blocks
 *  are merged, and the merged block takes bp's list slot (or the
 *  neighbour's if bp was not the head).
 * ══════════════════════════════════════════════════════════════════ */
static void *coalesce(void *bp)
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    /* Case 1 — both neighbours allocated, nothing to merge */
    if (prev_alloc && next_alloc)
        return bp;

    /* Case 2 — next block is free */
    if (prev_alloc && !next_alloc)
    {
        remove_free(NEXT_BLKP(bp)); /* remove next from list       */
        remove_free(bp);            /* remove bp   from list       */
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0)); /* FTRP now reaches new footer */
        insert_free(bp);              /* re-insert merged block      */
        return bp;
    }

    /* Case 3 — previous block is free */
    if (!prev_alloc && next_alloc)
    {
        remove_free(PREV_BLKP(bp)); /* remove prev from list */
        remove_free(bp);            /* remove bp   from list */
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
        insert_free(bp);
        return bp;
    }

    /* Case 4 — both neighbours are free */
    if (!prev_alloc && !next_alloc)
    {
        remove_free(PREV_BLKP(bp));
        remove_free(NEXT_BLKP(bp));
        remove_free(bp);
        size += GET_SIZE(HDRP(PREV_BLKP(bp))) +
                GET_SIZE(FTRP(NEXT_BLKP(bp)));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
        insert_free(bp);
    }

    return bp;
}

/* ══════════════════════════════════════════════════════════════════
 *  mm_free
 * ══════════════════════════════════════════════════════════════════ */
void mm_free(void *bp)
{
    if (bp == NULL)
        return;

#ifdef DEBUG
    if (!GET_ALLOC(HDRP(bp)))
    {
        fprintf(stderr,
                "Double free detected: %p\n",
                bp);
        return;
    }
#endif
    size_t size = GET_SIZE(HDRP(bp));
    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    insert_free(bp); /* add to free list BEFORE coalescing */
    coalesce(bp);
}

/* ══════════════════════════════════════════════════════════════════
 *  find_fit  (first-fit over the explicit free list)
 * ══════════════════════════════════════════════════════════════════ */
static void *find_fit(size_t asize)
{
    for (char *bp = free_listp; bp != NULL; bp = GET_SUCC(bp))
    {
        if (GET_SIZE(HDRP(bp)) >= asize)
            return bp;
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════
 *  place
 * ══════════════════════════════════════════════════════════════════ */
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));
    int cansplit = (csize - asize) >= MIN_BLKSIZE;

    remove_free(bp); /* bp is leaving the free list */

    if (cansplit)
    {
        /* Allocate front portion */
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
        /* Remainder becomes a new free block */
        void *remainder = NEXT_BLKP(bp);
        PUT(HDRP(remainder), PACK(csize - asize, 0));
        PUT(FTRP(remainder), PACK(csize - asize, 0));
        insert_free(remainder);
    }
    else
    {
        /* Use the whole block */
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  mm_malloc
 * ══════════════════════════════════════════════════════════════════ */
void *mm_malloc(size_t size)
{
    if (size == 0)
        return NULL;

    /* Adjust to meet alignment and overhead requirements */
    size_t asize = (size <= DSIZE) ? MIN_BLKSIZE
                                   : DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);

    char *bp;
    if ((bp = find_fit(asize)) != NULL)
    {
        place(bp, asize);
        return bp;
    }

    /* No fit — extend the heap */
    size_t extendsize = MAX(asize, CHUNKSIZE);
    if ((bp = extend_heap(extendsize / WSIZE)) == NULL)
        return NULL;
    place(bp, asize);
    return bp;
}

/* ══════════════════════════════════════════════════════════════════
 *  mm_realloc
 * ══════════════════════════════════════════════════════════════════ */
void *mm_realloc(void *bp, size_t size)
{
    if (size == 0)
    {
        mm_free(bp);
        return NULL;
    }
    if (bp == NULL)
    {
        return mm_malloc(size);
    }

    size_t oldsize = GET_SIZE(HDRP(bp));
    size_t newsize = DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);
    // Case 3:
    if (newsize == oldsize)
        return bp;

    /* Case 1 — shrink (or same size) */
    if (newsize < oldsize)
    {
        if (oldsize - newsize >= MIN_BLKSIZE)
        {
            PUT(HDRP(bp), PACK(newsize, 1));
            PUT(FTRP(bp), PACK(newsize, 1));
            void *next = NEXT_BLKP(bp);
            PUT(HDRP(next), PACK(oldsize - newsize, 0));
            PUT(FTRP(next), PACK(oldsize - newsize, 0));
            insert_free(next);
            coalesce(next);
        }
        return bp;
    }

    void *next = NEXT_BLKP(bp);
    size_t nextsize = GET_SIZE(HDRP(next));
    size_t combined = oldsize + nextsize;

    /* Case 2 — absorb free next block */
    if (!GET_ALLOC(HDRP(next)) && combined >= newsize)
    {
        remove_free(next);
        if (combined - newsize >= MIN_BLKSIZE)
        {
            PUT(HDRP(bp), PACK(newsize, 1));
            PUT(FTRP(bp), PACK(newsize, 1));
            void *remainder = NEXT_BLKP(bp);
            PUT(HDRP(remainder), PACK(combined - newsize, 0));
            PUT(FTRP(remainder), PACK(combined - newsize, 0));
            insert_free(remainder);
            coalesce(remainder); // no need just for checking
        }
        else
        {
            PUT(HDRP(bp), PACK(combined, 1));
            PUT(FTRP(bp), PACK(combined, 1));
        }
        return bp;
    }

    /* Case 3 — next is epilogue, extend heap in-place */
    if (nextsize == 0)
    {
        size_t extendsize = newsize - oldsize;
        if ((long)mem_sbrk(extendsize) == -1)
            return NULL;
        PUT(HDRP(bp), PACK(newsize, 1));
        PUT(FTRP(bp), PACK(newsize, 1));
        PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));
        return bp;
    }

    /* Case 4 — fall back: allocate new, copy, free old */
    void *newbp = mm_malloc(size);
    if (newbp == NULL)
        return NULL;
    memcpy(newbp, bp, oldsize - DSIZE);
    mm_free(bp);
    return newbp;
}

/* ══════════════════════════════════════════════════════════════════
 *  mm_calloc
 * ══════════════════════════════════════════════════════════════════ */
void *mm_calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0)
        return NULL;

    size_t totalsize = nmemb * size;
    if (size != 0 && totalsize / size != nmemb) /* overflow check */
        return NULL;

    void *bp = mm_malloc(totalsize);
    if (bp == NULL)
        return NULL;

    memset(bp, 0, totalsize);
    return bp;
}

/* ══════════════════════════════════════════════════════════════════
 *  Heap checker
 * ══════════════════════════════════════════════════════════════════ */
static void printblock(void *bp)
{
    size_t hsize = GET_SIZE(HDRP(bp));
    size_t halloc = GET_ALLOC(HDRP(bp));
    size_t fsize = GET_SIZE(FTRP(bp));
    size_t falloc = GET_ALLOC(FTRP(bp));

    if (hsize == 0)
    {
        printf("%p: EOL\n", bp);
        return;
    }

    if (!halloc)
        printf("%p: header:[%zu:%c] pred:%p succ:%p footer:[%zu:%c]\n",
               bp, hsize, (halloc ? 'a' : 'f'),
               (void *)GET_PRED(bp), (void *)GET_SUCC(bp),
               fsize, (falloc ? 'a' : 'f'));
    else
        printf("%p: header:[%zu:%c] footer:[%zu:%c]\n",
               bp, hsize, (halloc ? 'a' : 'f'),
               fsize, (falloc ? 'a' : 'f'));
}

static void checkblock(void *bp)
{
    if ((size_t)bp % 8)
        printf("Error: %p is not doubleword aligned\n", bp);
    if (GET(HDRP(bp)) != GET(FTRP(bp)))
        printf("Error: header does not match footer at %p\n", bp);
}

void checkheap(int verbose)
{
    char *bp = heap_listp;

    if (verbose)
        printf("Heap (%p):\n", heap_listp);

    if ((GET_SIZE(HDRP(heap_listp)) != DSIZE) || !GET_ALLOC(HDRP(heap_listp)))
        printf("Bad prologue header\n");
    checkblock(heap_listp);

    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp))
    {
        if (verbose)
            printblock(bp);
        checkblock(bp);
    }

    if (verbose)
        printblock(bp);
    if ((GET_SIZE(HDRP(bp)) != 0) || !GET_ALLOC(HDRP(bp)))
        printf("Bad epilogue header\n");

    /* Verify every free-list block is actually free */
    if (verbose)
        printf("\nFree list:\n");
    for (bp = free_listp; bp != NULL; bp = GET_SUCC(bp))
    {
        if (verbose)
            printblock(bp);
        if (GET_ALLOC(HDRP(bp)))
            printf("Error: allocated block %p on free list\n", bp);
    }
}