// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * calloc.c - Simple heap allocator (malloc/free)
 *
 * RTL8196E stage-2 bootloader
 *
 * Copyright (c) 2002-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_common.h"
#include <rtl_types.h>

/* DATA STRUCTURE DECLARATIONS
 */
typedef struct hdr {
	struct hdr *ptr;
	uint32 size;
} HEADER;

/* STATIC VARIABLE DECLARATIONS
 */
static HEADER *frhd;   /* free list head */
static uint32 memleft; /* memory left */
#define printf dprintf

/**
 * free - Return a previously allocated block to the free list
 * @ap: pointer returned by malloc()
 *
 * Coalesces contiguous free blocks where possible.
 * i_alloc() must be called before using free() or malloc().
 */
void free(void *ap)
{
	HEADER *nxt, *prev = NULL, *f;

	ASSERT_CSP(ap);
	ASSERT_CSP(frhd);

	f = (HEADER *)ap - 1;
	/* pt to header of block being returned */
	memleft += f->size;
	/*
		Note: frhd is never NULL unless i_alloc() was
		    never called to initialize package.
	*/
	if (frhd > f) {
		/*
			free queue head is higher up in memory
			than returnee
		*/
		nxt = frhd;	    /* old head */
		frhd = f;	    /* new head */
		prev = f + f->size; /* right after new head */

		if (prev == nxt) /* old and new are contiguous */
		{
			f->size += nxt->size;
			f->ptr = nxt->ptr; /* contiguate */
		} else
			f->ptr = nxt;
		return;
	}
	/*
		Otherwise current free space head is lower in
		memory. Walk down free space list looking for
		the block being returned. If the next pointer
		points past the block, make a new entry and
		link it.  If next pointer + its size points to the
		block form one contiguous block.
	*/
	nxt = frhd;
	for (nxt = frhd; nxt && nxt < f; prev = nxt, nxt = nxt->ptr) {
		if (nxt + nxt->size == f) {
			/* they are contiguous */
			nxt->size += f->size;
			/* form one block */
			f = nxt + nxt->size;
			if (f == nxt->ptr) {
				/*
					The new larger block is contiguous
					with the next free block, so form a
					larger block. There is no need to
					continue this checking since if the
					block following this free one was
					free, the two would have been
					made one already.
				*/
				nxt->size += f->size;
				nxt->ptr = f->ptr;
			}
			return;
		}
	}
	/*
		Otherwise, the addr of the block being returned
		is greater than one in the free queue ('nxt') or
		the end of the queue was reached. If at end, just
		link to the end of the queue.  Therefore, 'nxt' is
		either NULL or points to a block higher up in
		memory than the one being returned.
	*/
	prev->ptr = f;	    /* link to queue */
	prev = f + f->size; /* right after space to free */
	if (prev == nxt)    /* f and nxt are contiguous */
	{
		f->size += nxt->size;
		/* form a larger contiguous block */
		f->ptr = nxt->ptr;
	} else
		f->ptr = nxt;
	return;
}

/**
 * malloc - Allocate memory from the bootloader heap
 * @nbytes: number of bytes to allocate
 *
 * Searches the free list for a block large enough, splitting it
 * if necessary.  Halts if no memory is available.
 *
 * Return: pointer to the allocated memory
 */
void *malloc(uint32 nbytes) /* bytes to allocate */
{
	HEADER *nxt, *prev;
	uint32 nunits;

	nunits = (nbytes + sizeof(HEADER) - 1) / sizeof(HEADER) + 1;
	/*
		Change above divide to shift (for speed) only if
		the compiler doesn't do this for you and you
		don't require portability and you know that
		sizeof (HEADER) is a power of 2.

		Allocate the space requested + space for the
		header of the block.

		Search the free space queue for a block large
		enough. If block is larger than needed, break
		the block into two pieces and allocate the
		portion higher up in memory. Otherwise, just
		allocate the entire block.
	*/
	for (prev = NULL, nxt = frhd; nxt; nxt = nxt->ptr) {
		if (nxt->size >= nunits) /* big enough */
		{
			if (nxt->size > nunits)
			/* allocate head end */
			{
				HEADER *seg = nxt + nunits;
				if (prev == NULL)
					frhd = seg;
				else
					prev->ptr = seg;
				seg->ptr = nxt->ptr;
				seg->size = nxt->size - nunits;

				/* nxt now == ptr to alloc */
				nxt->size = nunits;
			} else {
				if (prev == NULL)
					frhd = nxt->ptr;
				else
					prev->ptr = nxt->ptr;
			}
			memleft -= nunits;
			return ((void *)(nxt + 1));
			/*
				Return a pointer past the header to
				the actual space requested.
			*/
		}
	}
	fatal("out of heap memory");
}

/**
 * i_alloc - Initialize the heap allocator
 * @_heapstart: first byte of the heap region
 * @_heapend: last byte of the heap region
 *
 * Must be called once before any malloc() or free() calls.
 * Sets up the initial free list spanning the entire heap.
 */
void i_alloc(void *_heapstart, void *_heapend)
{
	/* initialize the allocator */
	frhd = (HEADER *)_heapstart;
	frhd->ptr = NULL;
	frhd->size = (_heapend - _heapstart) / sizeof(HEADER);
	memleft = frhd->size; /* initial size in 4-byte units */
}
