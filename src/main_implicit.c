/* mm_implicit.c - Dynamic memory allocator: implicit free list.
 *
 * Implements malloc, free, realloc, and calloc using a heap
 * organised as a single implicit free list.  Every block
 * allocated or free is visited linearly during mm_malloc's
 * free-block search (first-fit placement).
 *
 * ──────────────────────────────────────────────────────────────
 * HEAP ORGANISATION
 * ──────────────────────────────────────────────────────────────
 *
 *  Word offsets from mem_heap_lo():
 *
 *   +0   Alignment padding  (4 bytes, value = 0)
 *   +4   Prologue header    PACK(8, 1)  <- 8-byte block, allocated
 *   +8   Prologue footer    PACK(8, 1)  <- heap_listp points here
 *   +12  [  r e g u l a r   b l o c k s  ]
 *   +N   Epilogue header    PACK(0, 1)  <- sentinel, size = 0
 *
 * The 4-byte padding at offset 0 shifts every block payload to
 * an address that is ≡ 0 (mod 8): because the prologue footer
 * occupies byte 8 (8-aligned), the first real block starts at
 * byte 12 (header) + 4 = byte 16 (payload), which is also
 * 8-aligned on a 32-bit build where each pointer is 4 bytes.
 *
 * The prologue and epilogue are permanently allocated sentinels
 * that never change their alloc bit.  Their purpose is to make
 * all four coalesce cases uniform: prev_alloc and next_alloc are
 * always valid reads, even at the very first and very last real
 * block.
 *
 * ──────────────────────────────────────────────────────────────
 * BLOCK LAYOUT
 * ──────────────────────────────────────────────────────────────
 *
 *  Allocated block:
 *  ┌──────────┬────────────────────────────────────┬──────────┐
 *  │  HEADER  │         P  A  Y  L  O  A  D        │  FOOTER  │
 *  │  4 bytes │         (returned to caller)       │  4 bytes │
 *  └──────────┴────────────────────────────────────┴──────────┘
 *             ▲ bp — the pointer returned by mm_malloc
 *
 *  Free block (identical layout; payload is unused):
 *  ┌──────────┬────────────────────────────────────┬──────────┐
 *  │  HEADER  │         (uninitialized)            │  FOOTER  │
 *  │  4 bytes │                                    │  4 bytes │
 *  └──────────┴────────────────────────────────────┴──────────┘
 *
 *  Header / footer encoding:
 *   bits [31:3] — block size in bytes (always a multiple of 8)
 *   bit  [0]    — allocation flag: 1 = allocated, 0 = free
 *   bits [2:1]  — unused (always 0)
 *
 *  Minimum block size = 16 bytes (header + 8-byte payload + footer,
 *  rounded to the next multiple of 8).
 *
 * ──────────────────────────────────────────────────────────────
 * PLACEMENT POLICY
 * ──────────────────────────────────────────────────────────────
 *  First-fit: scan from heap_listp; return the first free block
 *  whose size >= asize.  Simple and predictable; degrades to
 *  O(n) in the worst case as the heap grows.
 *
 * ──────────────────────────────────────────────────────────────
 * COALESCING POLICY
 * ──────────────────────────────────────────────────────────────
 *  Immediate coalescing: every mm_free call immediately merges
 *  the freed block with any adjacent free blocks.  Uses the
 *  boundary-tag technique (Knuth, 1973) for O(1) neighbour access.
 *
 * ──────────────────────────────────────────────────────────────
 * COMPLEXITY SUMMARY
 * ──────────────────────────────────────────────────────────────
 *  mm_malloc  — O(n) blocks in the heap     (first-fit linear scan)
 *  mm_free    — O(1)                        (boundary tags)
 *  coalesce   — O(1)                        (boundary tags)
 *  mm_realloc — O(n) worst case             (fallback malloc+copy)
 *
 * ──────────────────────────────────────────────────────────────
 * REFERENCES
 * ──────────────────────────────────────────────────────────────
 *  Bryant & O'Hallaron, "Computer Systems: A Programmer's
 *  Perspective", 3rd ed., Chapter 9 — Virtual Memory.
 *
 *  Knuth, "The Art of Computer Programming", Vol. 1, 2.5 —
 *  boundary tag coalescing.
 *
 * Author : Piyush Khanna
 * Build  : gcc -Wall -g -m32 -Iinclude -c src/mm_implicit.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "memlib.h"

/* ══════════════════════════════════════════════════════════════
 * CONSTANTS
 * ══════════════════════════════════════════════════════════════*/

#define WSIZE 4             /* Word / header / footer size (bytes)       */
#define DSIZE 8             /* Double word = minimum alignment (bytes)   */
#define CHUNKSIZE (1 << 12) /* Heap extension granularity = 4096 bytes   */

#define MAX(x, y) ((x) > (y) ? (x) : (y))

/* ══════════════════════════════════════════════════════════════
 * HEADER / FOOTER MACROS
 *
 * All block metadata is stored in 4-byte words.  Two fields are
 * packed into each word:
 *   • size  — bits [31:3], always a multiple of 8
 *   • alloc — bit [0],     1 = allocated, 0 = free
 * ══════════════════════════════════════════════════════════════*/

/* Pack SIZE (multiple of 8) and ALLOC bit into a single word. */
#define PACK(size, alloc) ((size) | (alloc))

/* Read a word (4 bytes) at address P. */
#define GET(p) (*(unsigned int *)(p))

/* Write VAL to the word at address P. */
#define PUT(p, val) (*(unsigned int *)(p) = (val))

/* Extract the block size from a header/footer word at P.
 * Masks off the low 3 bits (used for flags). */
#define GET_SIZE(p) (GET(p) & ~0x7)

/* Extract the allocation bit from a header/footer word at P. */
#define GET_ALLOC(p) (GET(p) & 0x1)

/* ══════════════════════════════════════════════════════════════
 * BLOCK-POINTER MACROS
 *
 * All functions receive and return BP — a char* pointing to the
 * first byte of the block's payload (one word past the header).
 * ══════════════════════════════════════════════════════════════*/

/* Address of the header word for block BP.
 * The header immediately precedes the payload. */
#define HDRP(bp) ((char *)(bp) - WSIZE)

/* Address of the footer word for block BP.
 * Start at BP, advance by (block_size - DSIZE) to skip the
 * payload, landing on the footer (the last word of the block). */
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/* Payload pointer of the next block in heap order.
 * Add the current block's full size to BP. */
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))

/* Payload pointer of the previous block in heap order.
 * Read the footer of the previous block (at BP - DSIZE) to get
 * its size, then step back that many bytes. */
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE)))

/* ══════════════════════════════════════════════════════════════
 * GLOBALS
 * ══════════════════════════════════════════════════════════════*/

/* Points to the payload of the prologue block.
 * This is the fixed starting point for the heap walk in
 * find_fit and checkheap. */
static char *heap_listp;

/* ══════════════════════════════════════════════════════════════
 * INTERNAL FUNCTION PROTOTYPES
 * ══════════════════════════════════════════════════════════════*/
void checkheap(int verbose);
static void *extend_heap(size_t words);
static void place(void *bp, size_t asize);
static void *find_fit(size_t asize);
static void *coalesce(void *bp);
static void printblock(void *bp);
static void checkblock(void *bp);

/* ══════════════════════════════════════════════════════════════
 * mm_init — Initialise the heap.
 *
 * Builds the four-word preamble that anchors the heap layout:
 *
 *   [ padding (0) ][ prologue hdr ][ prologue ftr ][ epilogue hdr ]
 *     offset +0       offset +4       offset +8       offset +12
 *
 * heap_listp is set to offset +8 (prologue footer = prologue
 * payload); this is the fixed origin for the heap walk.
 *
 * After the preamble, the heap is extended by CHUNKSIZE bytes
 * (one page) to pre-populate the free list for early requests.
 *
 * Returns: 0 on success, -1 if mem_sbrk fails.
 * ══════════════════════════════════════════════════════════════*/
int mm_init(void)
{
    /* Allocate 4 words for padding + prologue + epilogue. */
    if ((heap_listp = mem_sbrk(4 * WSIZE)) == (void *)-1)
        return -1;

    PUT(heap_listp, 0);                            /* alignment padding   */
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1)); /* prologue header     */
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); /* prologue footer     */
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));     /* epilogue header     */
    heap_listp += (2 * WSIZE);                     /* bp of prologue      */

    /* Extend the heap by one CHUNKSIZE block.
     * extend_heap rounds words to an even count and calls coalesce. */
    if (extend_heap(CHUNKSIZE / WSIZE) == NULL)
        return -1;

    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * extend_heap — Grow the heap by WORDS words.
 *
 * Called in two situations:
 *   1. During mm_init to create the initial free block.
 *   2. During mm_malloc when no suitable free block exists.
 *
 * The word count is rounded up to the nearest even number so
 * that the new block maintains double-word (8-byte) alignment.
 * A free block header and footer are written at the front of the
 * new region, and a new epilogue header is written immediately
 * after.  The old epilogue is overwritten by the new free block.
 *
 * words  : Requested extension size in words (4 bytes each).
 *
 * Returns: bp of the new free block (after coalescing with any
 *          immediately preceding free block), or NULL if
 *          mem_sbrk fails.
 * ══════════════════════════════════════════════════════════════*/
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    /* Round to an even word count to maintain 8-byte alignment. */
    size = (words % 2 == 1) ? (words + 1) * WSIZE : words * WSIZE;

    if ((long)(bp = mem_sbrk(size)) == -1)
        return NULL;

    /* Initialise the new free block's header and footer. */
    PUT(HDRP(bp), PACK(size, 0));         /* free block header   */
    PUT(FTRP(bp), PACK(size, 0));         /* free block footer   */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /* new epilogue header  */

    /* Coalesce with the previous block if it is also free.
     * This handles the case where the last block was freed just
     * before extend_heap was called (fragmentation avoidance). */
    return coalesce(bp);
}

/* ══════════════════════════════════════════════════════════════
 * mm_free — Free block BP, then immediately coalesce.
 *
 * Marks both the header and footer of BP as free (alloc bit = 0),
 * then calls coalesce to merge with any adjacent free blocks.
 *
 * Calling mm_free(NULL) is a safe no-op.
 *
 * bp : Pointer previously returned by mm_malloc, mm_realloc,
 *      or mm_calloc.  Behaviour is undefined for any other value.
 * ══════════════════════════════════════════════════════════════*/
void mm_free(void *bp)
{
    if (bp == NULL)
        return;

    size_t size = GET_SIZE(HDRP(bp));

    /* Clear the allocation bit in both header and footer. */
    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));

    coalesce(bp);
}

/* ══════════════════════════════════════════════════════════════
 * coalesce — Merge BP with adjacent free blocks.
 *
 * Uses the boundary-tag technique: the footer of the previous
 * block and the header of the next block give O(1) access to
 * their allocation status.  Four cases:
 *
 *  Case 1 — both neighbours allocated:
 *    No merge.  Return BP unchanged.
 *
 *  Case 2 — next block free, previous allocated:
 *    Absorb the next block into BP.  Update BP's header and
 *    the next block's footer to reflect the combined size.
 *
 *  Case 3 — previous block free, next allocated:
 *    Absorb BP into the previous block.  Update the previous
 *    block's header and BP's footer.  Return PREV_BLKP(bp).
 *
 *  Case 4 — both neighbours free:
 *    Merge all three blocks.  Update the previous block's
 *    header and the next block's footer.
 *
 * Invariant: BP must already have its header and footer marked
 * free before this function is called.
 *
 * Returns: bp of the merged (possibly enlarged) free block.
 * ══════════════════════════════════════════════════════════════*/
static void *coalesce(void *bp)
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    /* Case 1: both neighbours are allocated — nothing to merge. */
    if (prev_alloc && next_alloc)
        return bp;

    /* Case 2: only next block is free — expand BP rightward. */
    if (prev_alloc && !next_alloc)
    {
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0)); /* extend BP's header         */
        PUT(FTRP(bp), PACK(size, 0)); /* FTRP now reaches next ftr  */
        return bp;
    }

    /* Case 3: only previous block is free — expand PREV leftward. */
    if (!prev_alloc && next_alloc)
    {
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        return PREV_BLKP(bp);
    }

    /* Case 4: both neighbours are free — three-way merge. */
    size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(FTRP(NEXT_BLKP(bp)));
    PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
    PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
    return PREV_BLKP(bp);
}

/* ══════════════════════════════════════════════════════════════
 * mm_malloc — Allocate at least SIZE bytes, 8-byte aligned.
 *
 * Step 1 — reject zero requests.
 * Step 2 — adjust SIZE to cover the header/footer overhead
 *           and round up to the nearest multiple of 8:
 *             size <= 8  →  asize = 16  (minimum block)
 *             size  > 8  →  asize = 8 * ceil((size + 8) / 8)
 * Step 3 — call find_fit; if a block is found, call place.
 * Step 4 — if no block fits, extend the heap by
 *           max(asize, CHUNKSIZE) and place in the new region.
 *
 * Returns: 8-byte-aligned pointer to payload, or NULL.
 * ══════════════════════════════════════════════════════════════*/
void *mm_malloc(size_t size)
{
    size_t asize;      /* adjusted size (aligned, with overhead)    */
    size_t extendsize; /* amount to extend heap if no block found   */
    char *bp;

    /* Step 1: ignore spurious zero-size requests. */
    if (size == 0)
        return NULL;

    /* Step 2: compute the adjusted size.
     * Adding DSIZE covers both the 4-byte header and 4-byte footer.
     * Adding (DSIZE-1) before integer division implements ceiling. */
    if (size <= DSIZE)
        asize = 2 * DSIZE; /* minimum block   */
    else
        asize = DSIZE * ((DSIZE + size + (DSIZE - 1)) / DSIZE);

    /* Step 3: search the free list with first-fit policy. */
    if ((bp = find_fit(asize)) != NULL)
    {
        place(bp, asize);
        return bp;
    }

    /* Step 4: no fit found — grow the heap.
     * Request at least CHUNKSIZE to amortise the cost of mem_sbrk. */
    extendsize = MAX(asize, CHUNKSIZE);
    if ((bp = extend_heap(extendsize / WSIZE)) == NULL)
        return NULL;

    place(bp, asize);
    return bp;
}

/* ══════════════════════════════════════════════════════════════
 * find_fit — First-fit search over all heap blocks.
 *
 * Walks from heap_listp (prologue payload) through the heap in
 * address order.  The epilogue's size field of 0 terminates the
 * loop.  Returns the first free block with size >= asize, or
 * NULL if none exists.
 *
 * Complexity: O(n) where n is the total number of heap blocks.
 * ══════════════════════════════════════════════════════════════*/
static void *find_fit(size_t asize)
{
    char *temp = heap_listp;

    /* Traverse until we hit the epilogue (size = 0). */
    while (GET_SIZE(HDRP(temp)) > 0)
    {
        if (!GET_ALLOC(HDRP(temp)) && GET_SIZE(HDRP(temp)) >= asize)
            return temp; /* first free block large enough */
        temp = NEXT_BLKP(temp);
    }

    return NULL; /* no suitable block found */
}

/* ══════════════════════════════════════════════════════════════
 * place — Mark block BP as allocated with adjusted size ASIZE.
 *
 * If the difference between the block's current size and ASIZE
 * is at least 2*DSIZE (= 16 bytes, the minimum block size),
 * the block is split:
 *   • The front ASIZE bytes become the new allocated block.
 *   • The remaining (csize - asize) bytes become a new free block.
 * Without splitting, the entire block is used (internal
 * fragmentation is bounded by the minimum block size - 1 = 15 B).
 *
 * bp    : Free block selected by find_fit.
 * asize : Adjusted size returned by the size-adjustment formula
 *         in mm_malloc (guaranteed to be >= 16 and a multiple of 8).
 * ══════════════════════════════════════════════════════════════*/
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));
    int cansplit = (csize >= asize + (2 * DSIZE));

    if (cansplit)
    {
        /* Allocate front ASIZE bytes. */
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));

        /* Initialise the remainder as a new free block. */
        bp = NEXT_BLKP(bp);
        PUT(HDRP(bp), PACK(csize - asize, 0));
        PUT(FTRP(bp), PACK(csize - asize, 0));
    }
    else
    {
        /* Use the entire block — no split.
         * The header and footer already hold PACK(csize, 0);
         * just flip the alloc bit. */
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

/* ══════════════════════════════════════════════════════════════
 * mm_realloc — Resize block BP to at least SIZE bytes.
 *
 * Implements POSIX realloc semantics with in-place optimisations
 * to minimise data movement.  Seven cases are handled:
 *
 *  Case 1  size == 0          →  free and return NULL.
 *  Case 2  bp == NULL         →  equivalent to mm_malloc(size).
 *  Case 3  newsize == oldsize →  return bp unchanged.
 *  Case 4  newsize < oldsize  →  shrink in place; if the tail
 *                                 is >= 16 bytes, split it off
 *                                 and free it.
 *  Case 5  grow, next free    →  absorb the adjacent free block
 *                                 without moving data.
 *  Case 6  grow, at heap end  →  extend via mem_sbrk; no copy.
 *  Case 7  fallback           →  mm_malloc + memcpy + mm_free.
 *
 * Note on the memcpy size in Case 7:
 *   We copy (oldsize - DSIZE) bytes — the old payload, excluding
 *   the 4-byte header and 4-byte footer.  This is always <= the
 *   new block's payload capacity, because Case 5 handled the
 *   in-place-grow scenario first.
 *
 * Returns: pointer to the resized block, or NULL on failure.
 *          The original block is left unchanged on NULL return.
 * ══════════════════════════════════════════════════════════════*/
void *mm_realloc(void *bp, size_t size)
{
    /* Case 1: size == 0 is a free. */
    if (size == 0)
    {
        mm_free(bp);
        return NULL;
    }

    /* Case 2: NULL pointer is a fresh allocation. */
    if (bp == NULL)
        return mm_malloc(size);

    size_t oldsize = GET_SIZE(HDRP(bp));
    /* Round the new size up the same way mm_malloc does. */
    size_t newsize = DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);

    /* Case 3: sizes match — return unchanged. */
    if (newsize == oldsize)
        return bp;

    /* Case 4: shrink in place. */
    if (newsize < oldsize)
    {
        if (oldsize - newsize >= 2 * DSIZE)
        {
            /* Enough leftover to form a valid free block — split. */
            PUT(HDRP(bp), PACK(newsize, 1));
            PUT(FTRP(bp), PACK(newsize, 1));
            void *next = NEXT_BLKP(bp);
            PUT(HDRP(next), PACK(oldsize - newsize, 0));
            PUT(FTRP(next), PACK(oldsize - newsize, 0));
            coalesce(next); /* merge remainder with any following free block */
        }
        /* If the leftover is < 16 bytes, keep the original block
         * size to avoid creating an undersized free fragment. */
        return bp;
    }

    /* newsize > oldsize: try to grow without moving. */

    void *next = NEXT_BLKP(bp);
    size_t nextsize = GET_SIZE(HDRP(next));
    size_t combined = oldsize + nextsize;

    /* Case 5: absorb next free block — no data copy needed. */
    if (!GET_ALLOC(HDRP(next)) && combined >= newsize)
    {
        if (combined - newsize >= 2 * DSIZE)
        {
            /* Absorb only as much of the next block as needed
             * and split the remainder back to free. */
            PUT(HDRP(bp), PACK(newsize, 1));
            PUT(FTRP(bp), PACK(newsize, 1));
            void *remainder = NEXT_BLKP(bp);
            PUT(HDRP(remainder), PACK(combined - newsize, 0));
            PUT(FTRP(remainder), PACK(combined - newsize, 0));
            coalesce(remainder);
        }
        else
        {
            /* Absorb the entire next block; no usable remainder. */
            PUT(HDRP(bp), PACK(combined, 1));
            PUT(FTRP(bp), PACK(combined, 1));
        }
        return bp;
    }

    /* Case 6: BP is at the end of the heap (next == epilogue).
     * Extend the heap by exactly the deficit to avoid wasted space. */
    if (nextsize == 0)
    {
        size_t extendsize = newsize - oldsize;
        if ((long)mem_sbrk(extendsize) == -1)
            return NULL;
        PUT(HDRP(bp), PACK(newsize, 1));
        PUT(FTRP(bp), PACK(newsize, 1));
        PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /* new epilogue */
        return bp;
    }

    /* Case 7: no in-place option — allocate, copy, free.
     * Copy oldsize-DSIZE bytes (the payload, not header/footer). */
    void *newbp = mm_malloc(size);
    if (newbp == NULL)
        return NULL;
    memcpy(newbp, bp, oldsize - DSIZE);
    mm_free(bp);
    return newbp;
}

/* ══════════════════════════════════════════════════════════════
 * mm_calloc — Allocate NMEMB * SIZE bytes, zeroed.
 *
 * Guards against the two common misuse patterns:
 *   • Either argument is zero → return NULL without allocating.
 *   • NMEMB * SIZE overflows size_t → return NULL.
 *     Detection: if totalsize / nmemb != size, overflow occurred.
 *
 * On success, calls mm_malloc then memset to zero the payload.
 * ══════════════════════════════════════════════════════════════*/
void *mm_calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0)
        return NULL;

    size_t totalsize = nmemb * size;

    /* Overflow check: valid multiplication is reversible. */
    if (size != 0 && totalsize / size != nmemb)
        return NULL;

    void *bp = mm_malloc(totalsize);
    if (bp == NULL)
        return NULL;

    memset(bp, 0, totalsize);
    return bp;
}

/* ══════════════════════════════════════════════════════════════
 * printblock — Print one block's metadata to stdout.
 *
 * Prints address, header size and alloc flag, footer size and
 * alloc flag.  Prints "EOL" for the epilogue (size == 0).
 * ══════════════════════════════════════════════════════════════*/
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

    printf("%p: header:[%zu:%c] footer:[%zu:%c]\n",
           bp,
           hsize, (halloc ? 'a' : 'f'),
           fsize, (falloc ? 'a' : 'f'));
}

/* ══════════════════════════════════════════════════════════════
 * checkblock — Assert per-block invariants.
 *
 * Emits an error message (does not abort) if:
 *   • BP is not 8-byte aligned.
 *   • BP's header and footer do not match.
 * ══════════════════════════════════════════════════════════════*/
static void checkblock(void *bp)
{
    if ((size_t)bp % 8)
        printf("Error: %p is not doubleword aligned\n", bp);
    if (GET(HDRP(bp)) != GET(FTRP(bp)))
        printf("Error: header does not match footer\n");
}

/* ══════════════════════════════════════════════════════════════
 * checkheap — Consistency checker for the implicit allocator.
 *
 * Walks the heap from heap_listp to the epilogue, calling
 * printblock and checkblock on every block.  Also verifies the
 * prologue and epilogue sentinels.
 *
 * verbose : Non-zero → print every block; 0 → silent check.
 * ══════════════════════════════════════════════════════════════*/
void checkheap(int verbose)
{
    char *bp = heap_listp;

    if (verbose)
        printf("Heap (%p):\n", heap_listp);

    /* Verify the prologue: must be DSIZE (8 bytes), allocated. */
    if ((GET_SIZE(HDRP(heap_listp)) != DSIZE) || !GET_ALLOC(HDRP(heap_listp)))
        printf("Bad prologue header\n");
    checkblock(heap_listp);

    /* Walk every block until the epilogue (size == 0). */
    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp))
    {
        if (verbose)
            printblock(bp);
        checkblock(bp);
    }

    /* Print and verify the epilogue. */
    if (verbose)
        printblock(bp);
    if ((GET_SIZE(HDRP(bp)) != 0) || !(GET_ALLOC(HDRP(bp))))
        printf("Bad epilogue header\n");
}
