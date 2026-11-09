// SPDX-License-Identifier: BSD-3-Clause

#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "osmem.h"
#include "block_meta.h"

#define ALIGNMENT		8
#define ALIGN(size)		(((size) + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1))
#define META_SIZE		ALIGN(sizeof(struct block_meta))
#define MMAP_THRESHOLD		(128 * 1024)
#define PREALLOC_SIZE		(128 * 1024)

/* Doubly linked list of blocks placed on the heap (in address order). */
static struct block_meta *heap_head;
/* Doubly linked list of blocks allocated with mmap(). */
static struct block_meta *mmap_head;
static int heap_initialized;

static void *payload(struct block_meta *block)
{
	return (char *)block + META_SIZE;
}

static struct block_meta *get_block(void *ptr)
{
	return (struct block_meta *)((char *)ptr - META_SIZE);
}

static struct block_meta *heap_last(void)
{
	struct block_meta *b = heap_head;

	while (b && b->next)
		b = b->next;
	return b;
}

static void heap_append(struct block_meta *block)
{
	struct block_meta *last = heap_last();

	block->next = NULL;
	block->prev = last;
	if (last)
		last->next = block;
	else
		heap_head = block;
}

static void *alloc_mapped(size_t size)
{
	size_t total = META_SIZE + ALIGN(size);
	struct block_meta *block;

	block = mmap(NULL, total, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	DIE(block == MAP_FAILED, "mmap");

	block->size = ALIGN(size);
	block->status = STATUS_MAPPED;
	block->prev = NULL;
	block->next = mmap_head;
	if (mmap_head)
		mmap_head->prev = block;
	mmap_head = block;

	return payload(block);
}

static void free_mapped(struct block_meta *block)
{
	if (block->prev)
		block->prev->next = block->next;
	else
		mmap_head = block->next;
	if (block->next)
		block->next->prev = block->prev;

	DIE(munmap(block, block->size + META_SIZE) == -1, "munmap");
}

/* Merge @block with the following block (both must be free). */
static void merge_next(struct block_meta *block)
{
	struct block_meta *next = block->next;

	block->size += META_SIZE + next->size;
	block->next = next->next;
	if (next->next)
		next->next->prev = block;
}

static void coalesce_all(void)
{
	struct block_meta *b = heap_head;

	while (b && b->next) {
		if (b->status == STATUS_FREE && b->next->status == STATUS_FREE)
			merge_next(b);
		else
			b = b->next;
	}
}

/* Keep @size bytes in @block and turn the rest into a new free block. */
static void split_block(struct block_meta *block, size_t size)
{
	struct block_meta *rest;

	if (block->size < size + META_SIZE + ALIGN(1))
		return;

	rest = (struct block_meta *)((char *)payload(block) + size);
	rest->size = block->size - size - META_SIZE;
	rest->status = STATUS_FREE;
	rest->prev = block;
	rest->next = block->next;
	if (block->next)
		block->next->prev = rest;
	block->next = rest;
	block->size = size;
}

static struct block_meta *find_best(size_t size)
{
	struct block_meta *best = NULL;

	for (struct block_meta *b = heap_head; b; b = b->next)
		if (b->status == STATUS_FREE && b->size >= size &&
		    (!best || b->size < best->size))
			best = b;
	return best;
}

static void preallocate_heap(void)
{
	struct block_meta *block = sbrk(PREALLOC_SIZE);

	DIE(block == (void *)-1, "sbrk");
	block->size = PREALLOC_SIZE - META_SIZE;
	block->status = STATUS_FREE;
	heap_append(block);
	heap_initialized = 1;
}

static void *alloc_heap(size_t size)
{
	struct block_meta *block, *last;

	size = ALIGN(size);
	if (!heap_initialized)
		preallocate_heap();

	coalesce_all();
	block = find_best(size);
	if (block) {
		split_block(block, size);
		block->status = STATUS_ALLOC;
		return payload(block);
	}

	last = heap_last();
	if (last && last->status == STATUS_FREE) {
		/* Expand the last free block instead of creating a new one. */
		DIE(sbrk(size - last->size) == (void *)-1, "sbrk");
		last->size = size;
		last->status = STATUS_ALLOC;
		return payload(last);
	}

	block = sbrk(META_SIZE + size);
	DIE(block == (void *)-1, "sbrk");
	block->size = size;
	block->status = STATUS_ALLOC;
	heap_append(block);
	return payload(block);
}

static void *alloc_with_threshold(size_t size, size_t threshold)
{
	if (size == 0)
		return NULL;
	if (META_SIZE + ALIGN(size) >= threshold)
		return alloc_mapped(size);
	return alloc_heap(size);
}

void *os_malloc(size_t size)
{
	return alloc_with_threshold(size, MMAP_THRESHOLD);
}

void os_free(void *ptr)
{
	struct block_meta *block;

	if (!ptr)
		return;

	block = get_block(ptr);
	if (block->status == STATUS_MAPPED)
		free_mapped(block);
	else if (block->status == STATUS_ALLOC)
		block->status = STATUS_FREE;
}

void *os_calloc(size_t nmemb, size_t size)
{
	void *ptr;

	if (nmemb == 0 || size == 0)
		return NULL;
	if (nmemb > (size_t)-1 / size)
		return NULL;

	ptr = alloc_with_threshold(nmemb * size, (size_t)getpagesize());
	if (ptr)
		memset(ptr, 0, nmemb * size);
	return ptr;
}

static void *move_block(struct block_meta *block, size_t size)
{
	void *new_ptr = os_malloc(size);
	size_t copy = block->size < size ? block->size : size;

	if (!new_ptr)
		return NULL;
	memcpy(new_ptr, payload(block), copy);
	os_free(payload(block));
	return new_ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	struct block_meta *block;

	if (!ptr)
		return os_malloc(size);
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	block = get_block(ptr);
	if (block->status == STATUS_FREE)
		return NULL;

	size = ALIGN(size);
	if (block->status == STATUS_MAPPED || META_SIZE + size >= MMAP_THRESHOLD)
		return move_block(block, size);

	/* Shrinking (or same size): truncate in place. */
	if (size <= block->size) {
		split_block(block, size);
		return ptr;
	}

	/* Last block on the heap: just move the program break. */
	if (!block->next) {
		DIE(sbrk(size - block->size) == (void *)-1, "sbrk");
		block->size = size;
		return ptr;
	}

	/* Absorb following free blocks one at a time. */
	while (block->next && block->next->status == STATUS_FREE) {
		merge_next(block);
		if (block->size >= size) {
			split_block(block, size);
			return ptr;
		}
	}

	return move_block(block, size);
}
