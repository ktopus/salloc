/*
 * Copyright (C) 2010, 2011, 2013, 2014 Mail.RU
 * Copyright (C) 2010, 2011, 2013, 2014 Yuriy Vostrikov
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef OCTOPUS
# import <util.h>
# import <tbuf.h>
# import <say.h>
# import <salloc.h>
#else
# include "salloc.h"
# ifdef SLAB_NEED_STAT
#  define CRLF "\r\n"
# endif
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif

#if HAVE_VALGRIND_VALGRIND_H && !defined(NVALGRIND)
# include <valgrind/valgrind.h>
# include <valgrind/memcheck.h>
#else
# define VALGRIND_MAKE_MEM_DEFINED(_qzz_addr, _qzz_len) (void)0
# define VALGRIND_MAKE_MEM_NOACCESS(_qzz_addr, _qzz_len) (void)0
# define VALGRIND_MALLOCLIKE_BLOCK(addr, sizeB, rzB, is_zeroed) (void)0
# define VALGRIND_FREELIKE_BLOCK(addr, rzB) (void)0
#endif

#if defined(__SANITIZE_ADDRESS__)
void __asan_poison_memory_region(void const volatile *addr, size_t size, int magic);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#define ASAN_POISON_MEMORY_REGION(addr, size, magic) __asan_poison_memory_region((addr), (size), (magic))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) __asan_unpoison_memory_region((addr), (size))
#else
#define ASAN_POISON_MEMORY_REGION(addr, size, magic) ((void)(addr), (void)(size), (void)(magic))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) ((void)(addr), (void)(size))
#endif

// Accessible and defined
#define SLAB_MARK_MEMORY_ACCESSIBLE(addr, size)				\
	ASAN_UNPOISON_MEMORY_REGION(addr, size);			\
	VALGRIND_MAKE_MEM_DEFINED(addr, size);

#define SLAB_MARK_MEMORY_INACCESSIBLE(addr, size, magic)		\
	ASAN_POISON_MEMORY_REGION(addr, size, magic);			\
	VALGRIND_MAKE_MEM_NOACCESS(addr, size);

// Accessible and undefined
#define SLAB_MARK_MEMORY_ALLOCATED(addr, size)				\
	ASAN_UNPOISON_MEMORY_REGION(addr, size);			\
	VALGRIND_MALLOCLIKE_BLOCK(addr, size, sizeof(red_zone), 0);

#define SLAB_MARK_MEMORY_FREED(addr, size, magic)			\
	ASAN_POISON_MEMORY_REGION(addr, size, magic);			\
	VALGRIND_FREELIKE_BLOCK(addr, sizeof(red_zone));

#ifdef SLAB_DEBUG
# define SALLOC_CHECK_RED_ZONE(addr) (assert(memcmp(addr, red_zone, sizeof(red_zone)) == 0))
# define SALLOC_INIT_RED_ZONE(addr) (void)memcpy(addr, red_zone, sizeof(red_zone))
#else
# define SALLOC_CHECK_RED_ZONE(addr)
# define SALLOC_INIT_RED_ZONE(addr)
#endif

#ifndef MMAP_HINT_ADDR
# define MMAP_HINT_ADDR NULL
#endif
#ifndef MAX
# define MAX(a,b) (((a)>(b))?(a):(b))
#endif
#ifndef nelem
# define nelem(x) (sizeof((x))/sizeof((x)[0]))
#endif
#ifndef TYPEALIGN
#define TYPEALIGN(ALIGNVAL,LEN)  \
        (((uintptr_t) (LEN) + ((ALIGNVAL) - 1)) & ~((uintptr_t) ((ALIGNVAL) - 1)))
#endif

#define CACHEALIGN(LEN)	TYPEALIGN(32, (LEN))

#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif

#if defined(__SANITIZE_ADDRESS__)
#ifndef SLAB_DEBUG
# define SLAB_DEBUG
#endif
# define SALLOC_ALIGN(ptr) (void *)TYPEALIGN(8, ptr)
#else
# define SALLOC_ALIGN(ptr) ptr
#endif

#ifndef OCTOPUS
# define panic(x) abort()
# define panic_syserror(x) abort()
# define say_syserror(...) (void)0;
# define say_info(...) (void)0;
#endif

#if HAVE_MADVISE
# undef SLAB_RELEASE_EMPTY
# define SLAB_RELEASE_EMPTY 1
#endif
#ifndef SLAB_RELEASE_EMPTY
# define SLAB_RELEASE_EMPTY 0
#endif

#ifndef SLAB_SIZE
# define SLAB_SIZE (1 << 22)
#endif

#define SLAB_ALIGN_PTR(ptr) (void *)((uintptr_t)(ptr) & ~(SLAB_SIZE - 1))

#ifdef SLAB_DEBUG
#undef NDEBUG
const uint8_t red_zone[8] = { 0xfa, 0xfa, 0xfa, 0xfa, 0xfa, 0xfa, 0xfa, 0xfa };
#else
const uint8_t red_zone[0] = { };
#endif

__thread int salloc_error;

static const uint32_t SLAB_MAGIC = 0x51abface;
#define MAX_SLAB_ITEM (SLAB_SIZE / 4)
static __thread size_t page_size;

struct slab_item {
	struct slab_item *next;
};

struct slab {
	uint32_t magic;
	size_t used;
	size_t items;
	struct slab_item *free;
	struct slab_cache *cache;
	void *brk;
#if SLAB_RELEASE_EMPTY
	bool need_release;
#endif
	SLIST_ENTRY(slab) link;
	SLIST_ENTRY(slab) free_link;
	TAILQ_ENTRY(slab) cache_partial_link;
	TAILQ_ENTRY(slab) cache_link;
};

SLIST_HEAD(slab_slist_head, slab);
static __thread SLIST_HEAD(slab_cache_head, slab_cache) slab_cache = SLIST_HEAD_INITIALIZER(&slab_cache);

struct arena {
	char *brk;
	void *base;
	size_t size;
	size_t used;
	struct slab_slist_head slabs, free_slabs;
};

static __thread uint32_t slab_active_caches;
static __thread struct slab_cache slab_caches[256];
static __thread struct arena arena[2], *fixed_arena, *grow_arena;

static struct slab *
slab_of_ptr(const void *ptr)
{
	struct slab *slab = SLAB_ALIGN_PTR(ptr);
	assert(slab->magic == SLAB_MAGIC);
	return slab;
}

struct slab_cache *
slab_cache_of_ptr(const void *ptr)
{
	return slab_of_ptr(ptr)->cache;
}

size_t
salloc_usable_size(const void *ptr)
{
	return slab_cache_of_ptr(ptr)->item_size;
}

void
slab_cache_init(struct slab_cache *cache, size_t item_size, enum arena_type type, const char *name)
{
	assert(item_size <= MAX_SLAB_ITEM);
	cache->item_size = item_size > sizeof(void *) ? item_size : sizeof(void *);
	cache->name = name;
	cache->ctor = cache->dtor = NULL;

	switch (type) {
	case SLAB_FIXED:
		assert(fixed_arena != NULL);
		assert(fixed_arena->brk != NULL);
		cache->arena = fixed_arena; break;
	case SLAB_GROW:
		assert(grow_arena != NULL);
		cache->arena = grow_arena; break;
	default:
		abort();
	}

	TAILQ_INIT(&cache->slabs);
	TAILQ_INIT(&cache->partial_populated_slabs);
	if (name)
		SLIST_INSERT_HEAD(&slab_cache, cache, link);
}

static void
slab_cache_series_init(enum arena_type arena_type, size_t minimal, double factor)
{
	uint32_t i;
	size_t size;
	const size_t ptr_size = sizeof(void *);

	for (i = 0, size = minimal & ~(ptr_size - 1);
	     i < nelem(slab_caches) - 1 && size <= MAX_SLAB_ITEM;
	     i++)
	{
		slab_cache_init(&slab_caches[i], size, arena_type, NULL);

		size = MAX((size_t)(size * factor) & ~(ptr_size - 1),
			   (size + ptr_size) & ~(ptr_size - 1));
	}
	slab_cache_init(&slab_caches[i], MAX_SLAB_ITEM - sizeof(red_zone), arena_type, NULL);
	i++;

	slab_active_caches = i;
}

static void *
mmapa(size_t size, size_t align)
{
	void *ptr, *aptr;
	assert (size % align == 0);

	ptr = mmap(MMAP_HINT_ADDR, size + align, /* add padding for later rounding */
		   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ptr == MAP_FAILED) {
		say_syserror("mmap");
		return NULL;
	}

	aptr = (void *)(((uintptr_t)(ptr) & ~(align - 1)) + align);
	size_t pad_begin = aptr - ptr,
		 pad_end = align - pad_begin;

	munmap(ptr, pad_begin);
	ptr += pad_begin;
	munmap(ptr + size, pad_end);
	return ptr;
}

static bool
arena_add_mmap(struct arena *arena, size_t size)
{
	void *ptr = mmapa(size, SLAB_SIZE);
	if (!ptr)
		return false;

	arena->size += size;
	arena->brk = arena->base = ptr;
	return true;
}

static bool
arena_init(struct arena *arena, size_t size)
{
	memset(arena, 0, sizeof(*arena));

	if (size > 0 && !arena_add_mmap(arena, size))
		return false;

	SLIST_INIT(&arena->slabs);
	SLIST_INIT(&arena->free_slabs);
	return true;
}

static void *
arena_alloc(struct arena *arena)
{
	void *ptr;
	const size_t size = SLAB_SIZE;

	if (arena->size - arena->used < size) {
		if (arena == fixed_arena)
			return NULL;

		if (!arena_add_mmap(grow_arena, SLAB_SIZE)) /* NB: see salloc_destroy comment */
			panic("arena_alloc: can't enlarge grow_arena");

		return arena_alloc(grow_arena);
	}

	ptr = arena->brk;
	arena->brk += size;
	arena->used += size;

	return ptr;
}

/* if size > 0 then fixed_arena is configured and used for slab_cache series  */
void
salloc_init(size_t size, size_t minimal, double factor)
{
	static __thread bool inited = false;

	if (inited)
		return;

	fixed_arena = &arena[0];
	grow_arena = &arena[1];

#if HAVE_PAGE_SIZE
	page_size = PAGE_SIZE;
#elif HAVE_SYSCONF
	page_size = sysconf(_SC_PAGESIZE);
#else
	page_size = 0x1000;
#endif
	assert(sizeof(struct slab) <= page_size);

	if (size > 0) {
		size -= size % SLAB_SIZE; /* round to size of max slab */
		if (size < SLAB_SIZE * 2)
			size = SLAB_SIZE * 2;

		if (!arena_init(fixed_arena, size))
			panic_syserror("salloc_init: can't initialize arena");
	}

	if (!arena_init(grow_arena, 0))
		panic_syserror("salloc_init: can't initialize arena");

	slab_cache_series_init(size > 0 ? SLAB_FIXED : SLAB_GROW,
			       MAX(sizeof(void *), minimal), factor);
	if (size > 0)
		say_info("slab allocator configured, fixed_arena:%.1fGB",
			 size / (1024. * 1024 * 1024));

	inited = true;
}

void
salloc_destroy(void)
{
	struct slab *slab, *next_slab;
	struct slab_cache *cache, *tmp;

	if (fixed_arena != NULL) {
		if (fixed_arena->base != NULL)
			munmap(fixed_arena->base, fixed_arena->size);
		memset(fixed_arena, 0, sizeof(*fixed_arena));
	}

	if (grow_arena != NULL) {
		/* grow arena is increased in SLAB_SIZE chunks,
		   so there is one-to-one relation between slabs and mmaps */
		SLIST_FOREACH_SAFE(slab, &grow_arena->slabs, link, next_slab)
			munmap(slab, SLAB_SIZE);
		memset(grow_arena, 0, sizeof(*grow_arena));
	}

	/* all slab caches are no longer valid. taint them. */
	SLIST_FOREACH_SAFE(cache, &slab_cache, link, tmp)
		memset(cache, 'a', sizeof(*cache));

	SLIST_INIT(&slab_cache);
}

static void
format_slab(struct slab_cache *cache, struct slab *slab)
{
	void *initial_brk = (void *)CACHEALIGN((void *)slab + sizeof(struct slab));

	slab->magic = SLAB_MAGIC;
	slab->free = NULL;
	slab->cache = cache;
	slab->items = 0;
	slab->used = 0;

	// Red zone before first element
	slab->brk = SALLOC_ALIGN(initial_brk + sizeof(red_zone));
	SLAB_MARK_MEMORY_ACCESSIBLE(slab->brk - sizeof(red_zone), sizeof(red_zone));
	SALLOC_INIT_RED_ZONE(slab->brk - sizeof(red_zone));

	SLAB_MARK_MEMORY_INACCESSIBLE(initial_brk, SLAB_SIZE - (initial_brk - (void *)slab), 0xfa);

	TAILQ_INSERT_HEAD(&cache->slabs, slab, cache_link);
	TAILQ_INSERT_HEAD(&cache->partial_populated_slabs, slab, cache_partial_link);
}

static bool
fully_populated(const struct slab *slab)
{
	return slab->brk + slab->cache->item_size + sizeof(red_zone) >= (void *)slab + SLAB_SIZE &&
	       slab->free == NULL;
}

void
slab_validate(void)
{
#ifdef SLAB_DEBUG
	struct slab *slab;

	for (uint32_t i = 0; i < nelem(arena); i++) {
		SLIST_FOREACH(slab, &arena[i].slabs, link) {
			void *initial_brk = (void *)CACHEALIGN((void *)slab + sizeof(struct slab));
			void *item = SALLOC_ALIGN(initial_brk + sizeof(red_zone));
			void *slab_red_zone = item - sizeof(red_zone);

			// Check red zone before the first item
			SLAB_MARK_MEMORY_ACCESSIBLE(slab_red_zone, sizeof(red_zone));
			SALLOC_CHECK_RED_ZONE(slab_red_zone);
			SLAB_MARK_MEMORY_INACCESSIBLE(slab_red_zone, sizeof(red_zone), 0xfa);

			// Check red zones after items
			while (item < slab->brk) {
				slab_red_zone = item + slab->cache->item_size;

				SLAB_MARK_MEMORY_ACCESSIBLE(slab_red_zone, sizeof(red_zone));
				SALLOC_CHECK_RED_ZONE(slab_red_zone);
				SLAB_MARK_MEMORY_INACCESSIBLE(slab_red_zone, sizeof(red_zone), 0xfa);

				item = SALLOC_ALIGN(item + slab->cache->item_size + sizeof(red_zone));
			}
		}
	}
#endif
}

static struct slab_cache *
cache_for(size_t size)
{
	for (uint32_t i = 0; i < slab_active_caches; i++)
		if (slab_caches[i].item_size >= size)
			return &slab_caches[i];

	salloc_error = ESALLOC_NOCACHE;
	return NULL;
}

static struct slab *
slab_of(struct slab_cache *cache)
{
	struct slab *slab;

	slab = TAILQ_LAST(&cache->partial_populated_slabs, slab_tailq_head);
	if (slab != NULL) {
		assert(slab->magic == SLAB_MAGIC);
		return slab;
	}

	if (!SLIST_EMPTY(&cache->arena->free_slabs)) {
		slab = SLIST_FIRST(&cache->arena->free_slabs);
		assert(slab->magic == SLAB_MAGIC);
		SLIST_REMOVE_HEAD(&cache->arena->free_slabs, free_link);
		format_slab(cache, slab);
		return slab;
	}

	if ((slab = arena_alloc(cache->arena)) != NULL) {
		SLIST_INSERT_HEAD(&cache->arena->slabs, slab, link);
		format_slab(cache, slab);
		return slab;
	}

	salloc_error = ESALLOC_NOMEM;
	return NULL;
}

#ifndef NDEBUG
static bool
valid_item(struct slab *slab, void *item)
{
	return (void *)item >= (void *)(slab) + sizeof(struct slab) &&
	    (void *)item < (void *)(slab) + sizeof(struct slab) + SLAB_SIZE;
}
#endif

void *
slab_cache_alloc(struct slab_cache *cache)
{
	struct slab *slab;
	struct slab_item *item;

	if ((slab = slab_of(cache)) == NULL)
		return NULL;

	if (slab->free == NULL) {
		assert(valid_item(slab, slab->brk));
		item = slab->brk;

		SLAB_MARK_MEMORY_ACCESSIBLE((void *)item, cache->item_size + sizeof(red_zone));
		SALLOC_INIT_RED_ZONE((void *)item + cache->item_size);
		SLAB_MARK_MEMORY_INACCESSIBLE((void *)item + cache->item_size, sizeof(red_zone), 0xfa);

		SLAB_MARK_MEMORY_ALLOCATED(item, cache->item_size);

		/* we can leave slab here in case of last item has been allocated
		    and align has been occured */
		slab->brk = SALLOC_ALIGN(slab->brk + cache->item_size + sizeof(red_zone));

		/* call ctor _after_ SLAB_MARK_MEMORY_ALLOCATED */
		if (cache->ctor)
			cache->ctor(item);
	} else {
		assert(valid_item(slab, slab->free));
		item = slab->free;

		SLAB_MARK_MEMORY_ACCESSIBLE(item, sizeof(void *));
		slab->free = item->next;

		SLAB_MARK_MEMORY_ALLOCATED(item, cache->item_size);
	}

	if (fully_populated(slab)) {
		TAILQ_REMOVE(&cache->partial_populated_slabs, slab, cache_partial_link);
#if SLAB_RELEASE_EMPTY
		slab->need_release = true;
#endif
	}

	slab->used += cache->item_size + sizeof(red_zone);
	slab->items += 1;

	return (void *)item;
}

void *
salloc(size_t size)
{
	struct slab_cache *cache;

	if ((cache = cache_for(size)) == NULL)
		return NULL;

	return slab_cache_alloc(cache);
}

void
sfree(void *ptr)
{
	assert(ptr != NULL);
	struct slab *slab = slab_of_ptr(ptr);
	struct slab_cache *cache = slab->cache;
	struct slab_item *item = ptr;

	if (fully_populated(slab))
		TAILQ_INSERT_TAIL(&cache->partial_populated_slabs, slab, cache_partial_link);

	assert(valid_item(slab, item));
	assert(slab->free == NULL || valid_item(slab, slab->free));

	item->next = slab->free;
	slab->free = item;
	slab->used -= cache->item_size + sizeof(red_zone);
	slab->items -= 1;

	if (slab->items == 0) {
		bool slab_still_alive = true;

		TAILQ_REMOVE(&cache->partial_populated_slabs, slab, cache_partial_link);
		TAILQ_REMOVE(&cache->slabs, slab, cache_link);

#if SLAB_RELEASE_EMPTY
		if (slab->need_release) {
			slab->need_release = false;
			int r = 0;
# if HAVE_MADVISE
			r = madvise((void *)slab + page_size, SLAB_SIZE - page_size, MADV_DONTNEED);
# else
			if (cache->arena == grow_arena) {
				SLIST_REMOVE(&cache->arena->slabs, slab, slab, link);
				r = munmap(slab, SLAB_SIZE);
				slab_still_alive = false;
			}
# endif // ! HAVE_MADVISE
			(void)r;
			assert(r == 0);
		}
#endif // SLAB_RELEASE_EMPTY

		if (slab_still_alive)
			SLIST_INSERT_HEAD(&cache->arena->free_slabs, slab, free_link);
	}

	SLAB_MARK_MEMORY_FREED(item, cache->item_size, 0xfd);
}

void
slab_cache_free(struct slab_cache *cache, void *ptr)
{
	if (cache->dtor)
		cache->dtor(ptr);
	sfree(ptr);
}

#ifdef SLAB_NEED_STAT
static int64_t
cache_stat(struct slab_cache *cache, struct tbuf *out)
{
	struct slab *slab;
	int slabs = 0;
	size_t items = 0, used = 0, free = 0;

	TAILQ_FOREACH(slab, &cache->slabs, cache_link) {
		free += SLAB_SIZE - slab->used - sizeof(struct slab);
		items += slab->items;
		used += sizeof(struct slab) + slab->used;
		slabs++;
	}

	if (slabs == 0 && cache->name == NULL)
		return 0;

	tbuf_printf(out,
		    "     - { name: %-16s, item_size: %- 5i, slabs: %- 3i, items: %-11zu"
		    ", bytes_used: %-12zu, bytes_free: %-12zu }" CRLF,
		    cache->name, (int)cache->item_size, slabs, items, used, free);

	return used;
}

void
slab_stat(struct tbuf *t)
{
	struct slab *slab;
	struct slab_cache *cache;

	int64_t total_used = 0;
	tbuf_printf(t, "slab statistics:" CRLF);

	tbuf_printf(t, "  arenas:" CRLF);
	for (int i = 0; i < nelem(arena); i++) {
		if (arena[i].size == 0)
			break;

		int free_slabs = 0;
		SLIST_FOREACH(slab, &arena[i].free_slabs, free_link)
			free_slabs++;


		tbuf_printf(t, "    - { type: %s, used: %.2f, size: %zu, free_slabs: %i }" CRLF,
			    &arena[i] == fixed_arena ? "fixed" :
			    &arena[i] == grow_arena ? "grow" : "unknown",
			    (double)arena[i].used / arena[i].size * 100,
			    arena[i].size, free_slabs);
	}

	tbuf_printf(t, "  caches:" CRLF);
	for (uint32_t i = 0; i < slab_active_caches; i++)
		total_used += cache_stat(&slab_caches[i], t);

	SLIST_FOREACH(cache, &slab_cache, link)
		cache_stat(cache, t);


	int fixed_free_slabs = 0;
	SLIST_FOREACH(slab, &fixed_arena->free_slabs, free_link)
			fixed_free_slabs++;

	int64_t fixed_used_adj = fixed_arena->used - fixed_free_slabs * SLAB_SIZE;
	if (fixed_arena->size != 0) {
		tbuf_printf(t, "  items_used: %.2f" CRLF, (double)total_used / fixed_arena->size * 100);
		tbuf_printf(t, "  arena_used: %.2f" CRLF, (double)fixed_used_adj / fixed_arena->size * 100);
	} else {
		tbuf_printf(t, "  items_used: 0" CRLF);
		tbuf_printf(t, "  arena_used: 0" CRLF);
	}
}
#endif

#ifdef OCTOPUS
register_source();
#endif

void slab_cache_stat(struct slab_cache *cache, uint64_t *bytes_used, uint64_t *items)
{
	struct slab *slab;

	*bytes_used = *items = 0;
	TAILQ_FOREACH(slab, &cache->slabs, cache_link) {
		*bytes_used += slab->used;
		*items += slab->items;
	}
}

void
slab_total_stat(uint64_t *bytes_used, uint64_t *items)
{
	struct slab *slab;

	*bytes_used = *items = 0;
	for (uint32_t i = 0; i < nelem(arena); i++) {
		SLIST_FOREACH(slab, &arena[i].slabs, link) {
			*bytes_used += slab->used;
			*items += slab->items;
		}
	}
}
