#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "memlib.h"
/* Basic constants and macros */
#define WSIZE 4             /* Word and header/footer size (bytes) */
#define DSIZE 8             /* Double word size (bytes) */
#define CHUNKSIZE (1 << 12) /* Extend heap by this amount (bytes) 4096 bytes==pagesize */
#define MAX(x, y) ((x) > (y) ? (x) : (y))
/* Pack a size and allocated bit into a word */
#define PACK(size, alloc) ((size) | (alloc))

/* Read and write a word at address p */
#define GET(p) (*(unsigned int *)(p))
#define PUT(p, val) (*(unsigned int *)(p) = (val))
/* Read the size and allocated fields from address p */
#define GET_SIZE(p) (GET(p) & ~0x7) // removes last 3 bits cause always 0
#define GET_ALLOC(p) (GET(p) & 0x1) // returns allocated bit
/* Given block ptr bp, compute address of its header and footer */
#define HDRP(bp) ((char *)(bp) - WSIZE)
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE) // fetches size fo the entire block so from the start of the payload we add header + payload +footer then subtract size of header and footer to reach the start of the footer.
/* Given block ptr bp, compute address of next and previous blocks */
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE))) // reach footer of prev block get size of that block bp-header of bp -footer of prev -payload of prev

// (bp - WSIZE)	header of current block
// (bp - DSIZE)	footer of previous block
// GET_SIZE(...)	size of that block

/*Points to the Prologue Block*/
static char *heap_listp;
// public functions
void checkheap(int verbose);
/* Function prototypes for internal helper routines */
static void *extend_heap(size_t words);
static void place(void *bp, size_t asize);
static void *find_fit(size_t asize);
static void *coalesce(void *bp);
static void printblock(void *bp);
static void checkblock(void *bp);
// initializes the allocator 0 on success -1 on failure
int mm_init(void)
{
    /*Creates the initial empty heap*/
    if ((heap_listp = mem_sbrk(4 * WSIZE)) == (void *)-1)
        return -1;
    PUT(heap_listp, 0); /*We add a 4-byte padding so that headers land at 4 mod 8, making payloads land at 0 mod 8.*/
    /*
    padding  0x1000
    header   0x1004
    payload  0x1008
*/
    /*Also memlib.c gives 8 byte aligned memory -32bit So the code works */
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1)); /*Prologue header*/
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); /*Prologue Footer*/
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));     /*Epilogue Header*/
    heap_listp += (2 * WSIZE);

    // extend heap -will come back to this
    // invoked 1.when heap is initialized
    // 2. when mm_malloc is unable to find a suitable fit
    if (extend_heap(CHUNKSIZE / WSIZE) == NULL)
        return -1;
    return 0;
}
static void *extend_heap(size_t words)
{
    char *bp; // bp is the current block
    size_t size;
    // make it even
    size = (words % 2 == 1) ? (words + 1) * WSIZE : words * WSIZE;
    if ((long)(bp = mem_sbrk(size)) == -1)
        return NULL;

    /* Initialize free block header/footer and the epilogue header*/
    PUT(HDRP(bp), PACK(size, 0));         /*Free Block Header*/
    PUT(FTRP(bp), PACK(size, 0));         /*Free Block Footer*/
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /*New Epilogoue header*/

    return coalesce(bp);
}

void mm_free(void *bp)
{
    if (bp == NULL)
        return;
    size_t size = GET_SIZE(HDRP(bp));

    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));

    coalesce(bp);
}
// simple coalescing 4 case implementation
static void *coalesce(void *bp)
{
    // get previous allocated block
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    // Case 1 no coalescing possible both prev and next are allocated
    if (prev_alloc && next_alloc)
    {
        return bp;
    }

    // case 2: Next block free merging next with the current block
    else if (prev_alloc && !next_alloc)
    {
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0));   // update current header
        PUT((FTRP(bp)), PACK(size, 0)); // update next footer
        return bp;
    }

    // case 3: Previous block free merging previous with the current block
    else if (!prev_alloc && next_alloc)
    {
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp); // using footer of previous here which is not modified and still holds the address of the previous block
    }
    // case 4: Both previous and next block are free
    else
    {
        size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(FTRP(NEXT_BLKP(bp)));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
    }
    return bp;
}

void *mm_malloc(size_t size)
{
    // adjusted size
    size_t asize;
    size_t extendsize; // Amount to extend heap if no fit
    char *bp;
    // Ignore Spurious requests
    if (size == 0)
        return NULL;

    // Adjust Block size for overhead and alignment reqs
    if (size <= DSIZE)
        asize = 2 * DSIZE;
    else
        asize = DSIZE * ((DSIZE + size + (DSIZE - 1)) / DSIZE);

    // search the free list implicit
    if ((bp = find_fit(asize)) != NULL)
    {
        place(bp, asize);
        return bp;
    }
    // NO fit found extend heap(asize is already correct 8 byte aligned)
    extendsize = MAX(asize, CHUNKSIZE);
    if ((bp = extend_heap(extendsize / WSIZE)) == NULL)
        return NULL;
    place(bp, asize);
    return bp;
}
static void *find_fit(size_t asize)
{
    // pointer for traversal
    char *temp = heap_listp;
    // first block
    // first fit policy
    while (GET_SIZE(HDRP(temp)) > 0)
    {
        if (!GET_ALLOC(HDRP(temp)) && GET_SIZE(HDRP(temp)) >= asize)
            return temp;
        temp = NEXT_BLKP(temp);
    }
    return NULL;
}

// places the block makes it allocated set bit and split if space available
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));
    size_t cansplit = (csize) >= asize + (2 * DSIZE);
    if (cansplit)
    {
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
        bp = NEXT_BLKP(bp);
        PUT(HDRP(bp), PACK(csize - asize, 0));
        PUT(FTRP(bp), PACK(csize - asize, 0));
        // SPlits the block into two maintaining min size of DSIZE(4+4+1->16)
    }
    else
    {
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

// h means details about header
// f means details about footer
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
           bp, hsize, (halloc ? 'a' : 'f'),
           fsize, (falloc ? 'a' : 'f'));
}

/*
 * checkheap - Minimal check of the heap for consistency
 */
void checkheap(int verbose)
{
    char *bp = heap_listp; // points to the header of the prologue block

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
    if ((GET_SIZE(HDRP(bp)) != 0) || !(GET_ALLOC(HDRP(bp))))
        printf("Bad epilogue header\n");
}
static void checkblock(void *bp)
{
    if ((size_t)bp % 8) // check 8 byte alignment
        printf("Error: %p is not doubleword aligned\n", bp);
    if (GET(HDRP(bp)) != GET(FTRP(bp)))
        printf("Error: header does not match footer\n");
}

void *mm_realloc(void *bp, size_t size)
{
    // Case 1
    if (size == 0)
    {
        mm_free(bp);
        return NULL;
    }
    // Case 2
    if (bp == NULL)
        return mm_malloc(size);

    size_t oldsize = GET_SIZE(HDRP(bp));
    size_t newsize = DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE); // ensurres newsize is always 16B
    // Case 3:
    if (newsize == oldsize)
        return bp;

    // Case 4 shrink
    if (newsize < oldsize)
    {
        // Case 4 If newsize>=2*DSIZE and oldsize-newsize>=2*DSIZE
        if (oldsize - newsize >= 2 * DSIZE)
        {
            PUT(HDRP(bp), PACK(newsize, 1));
            PUT(FTRP(bp), PACK(newsize, 1));
            void *next = NEXT_BLKP(bp);
            PUT(HDRP(next), PACK(oldsize - newsize, 0));
            PUT(FTRP(next), PACK(oldsize - newsize, 0));
            coalesce(next);
        }
        return bp;
    }

    // newsize is greater than oldsize

    void *next = NEXT_BLKP(bp);
    size_t nextsize = GET_SIZE(HDRP(next));
    size_t combined = oldsize + nextsize; // combined space that is available if next block is available

    // Case 5 absorb next free block
    if (!GET_ALLOC(HDRP(next)) && combined >= newsize)
    {
        if (combined - newsize >= 2 * DSIZE)
        {
            PUT(HDRP(bp), PACK(newsize, 1));
            PUT(FTRP(bp), PACK(newsize, 1));
            void *remainder = NEXT_BLKP(bp);
            PUT(HDRP(remainder), PACK(combined - newsize, 0));
            PUT(FTRP(remainder), PACK(combined - newsize, 0));
            coalesce(remainder);
        }
        else
        {
            PUT(HDRP(bp), PACK(combined, 1));
            PUT(FTRP(bp), PACK(combined, 1));
        }
        return bp;
    }

    // Case 6  next is epilogue extend heap
    if (nextsize == 0)
    {
        size_t extendsize = newsize - oldsize; // extend with what you exactly need
        if ((long)mem_sbrk(extendsize) == -1)
            return NULL;
        PUT(HDRP(bp), PACK(newsize, 1));
        PUT(FTRP(bp), PACK(newsize, 1));
        PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); // created new epilogue
        return bp;
    }

    // Case 7 — malloc new, copy, free old
    void *newbp = mm_malloc(size);
    if (newbp == NULL)
        return NULL;
    memcpy(newbp, bp, oldsize - DSIZE);
    mm_free(bp);
    return newbp;
}
void *mm_calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0)
        return NULL;

    size_t totalsize = nmemb * size;

    // overflow check
    if (size != 0 && totalsize / size != nmemb)
        return NULL;

    void *bp = mm_malloc(totalsize);
    if (bp == NULL)
        return NULL;

    memset(bp, 0, totalsize);
    return bp;
}