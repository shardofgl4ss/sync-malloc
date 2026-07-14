#define _GNU_SOURCE

#include "sync_malloc.h"
#include "types.h"


#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbit.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

// #define DEBUG				1

#ifdef DEBUG
#include <assert.h>
#include <fcntl.h>
#endif

#define STR2(x)				#x
#define STR(x)				STR2(x)

#define ENOMEM_STR			"-*- Out of Memory! -*-"
#define ERR_DOUBLEFREE			13
#define ERR_DOUBLEFREE_STR      	"-*- Double free or heap corruption! -*-"
#define ERR_MAP_ALIGN			99
#define ERR_MAP_ALIGN_STR		"-*- Somehow, the mmap region is still not aligned. -*-"
#define ERR_NO_LIBC_REALLOC		101
#define ERR_NO_LIBC_REALLOC_STR 	"-*- libc realloc() could not be acquired, exiting. -*-"
#define ERR_NO_LIBC_FREE		102
#define ERR_NO_LIBC_FREE_STR		"-*- libc free() could not be acquired, ignoring foreign pointers. memory will be lost. -*-"
#define ERR_NO_LIBC_USABLE_SIZE		103
#define ERR_NO_LIBC_USABLE_SIZE_STR	"-*- libc malloc_usable_size() could not be acquired, exiting. -*-"

#define PRINTERR(code, str) \
	"<ALLOCATOR ERROR> E" STR(code) " (" __FILE__ ":" STR(__LINE__) "): " str

#if defined(DEBUG)
	#define PRINTDEBUG(str) \
		"<ALLOCATOR DEBUG>" " (" __FILE__ ":" STR(__LINE__) "): " str
#else
	#define PRINTDEBUG(str)
#endif

#define ARENA_DZ		0xdeaddeaddeaddeadULL
#define ARENA_MEM_LIMIT		-1
#define ARENA_MEM_INIT		0x40000
#define ARENA_RECENT_CACHE	4

#define ALIGNMENT		0x10
#define ALLOC_MIN_SZ		0x10
#define ALLOC_MAX_SZ		PTRDIFF_MAX
#define HDR_MAGIC		0xaeae

// 48 bytes of padding basically guarantees underflow protection.
#define MAP_PADDING		0x30
#define MAP_THRESHOLD		0x20000
#define MAP_POISON		0xdeadbeefdeadbeefULL
#define MMAP_FFLAGS		(MAP_PRIVATE | MAP_ANONYMOUS)
#define MMAP_PFLAGS		(PROT_READ | PROT_WRITE)
#define MMAP_FUNCTION(sz)	mmap(NULL, sz, MMAP_PFLAGS, MMAP_FFLAGS, 0, 0)

#define SLAB_SIZE_PER		0x4000
#define SLAB_SIZE(idx)		(1ULL << slab_shift[idx])
#define SLAB_COUNT		(sizeof(slab_shift) / sizeof(slab_shift[0]))
#define SLAB_MAX_SIZE		(SLAB_COUNT * SLAB_SIZE_PER)

#if DEBUG
#define DEBUG_ASSERT(expr)	assert(expr)
#else
#define DEBUG_ASSERT(expr)
#endif

// #define PREINIT_POOL_BYTES	0x100000

#define MALLOC_ERR_WRITE_PATH	"/tmp/malloc_fail.log"
#define MALLOC_ERR_OPEN_FLAGS	O_WRONLY | O_CREAT | O_APPEND
#define MALLOC_ERR_PERMS	0644

#if defined(DEBUG)
#define MALLOC_ERR_WRITE(size) \
	do { \
		char buf[256]; \
		int fd = open(MALLOC_ERR_WRITE_PATH, MALLOC_ERR_OPEN_FLAGS, MALLOC_ERR_PERMS); \
		if (fd >= 0) {  \
			int l = 0; \
			if (core) { \
				l = snprintf(buf, sizeof(buf), \
					"NULL: sz=%zu errno=%d heap_size=%llu used=%llu map_count=%u\n", \
					((size_t)(size)), \
					errno, \
					(long long unsigned)core->heap_size, \
					(long long unsigned)core->used_size, \
					core->map_count); \
			} else { \
				l = snprintf(buf, sizeof(buf), \
					"NULL: sz=%zu errno=%d heap_size=%llu used=%llu map_count=%u\n", \
					((size_t)(size)), \
					errno, \
					(long long unsigned)0, \
					(long long unsigned)0, \
					(unsigned)0); \
			} \
			if (l > 0) write(fd, buf, (size_t)l); \
			close(fd); \
		} \
	} while (0);
#else
#define MALLOC_ERR_WRITE(size)
#endif

pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_once_t core_init_lock = PTHREAD_ONCE_INIT;

#define TRIGGER_CORE_INIT pthread_once(&core_init_lock, malloc_init)

// static void *(*libc_malloc)(size_t) = NULL;
// static void *(*libc_calloc)(size_t, size_t) = NULL;
static void *(*libc_realloc)(void *_Nullable, size_t) = NULL;
static void *(*libc_free)(void *_Nullable) = NULL;
static size_t (*libc_malloc_usable_size)(void *_Nullable) = NULL;

static_assert((SLAB_SIZE_PER % 0x4000) == 0, "invalid slab sizes!");

// id, size
#define SLAB_SET \
        X(0, 16) \
        X(1, 32) \
        X(2, 64) \
        X(3, 128) \
        X(4, 256) \
        X(5, 512) \
        X(6, 1024) \
        X(7, 2048)

#define X(id, v)	__builtin_ctz(v),
static constexpr int slab_shift[] = {SLAB_SET};
#undef X

struct heap_core_dbg_info;
struct heap_core;
struct poolhdr;
struct freehdr;
struct maphdr;
struct ghosthdr;
struct hdr_node;
struct slab_core;
struct slab_controller;
struct thread_cache;


void malloc_init(void);
__attribute__((destructor))
void malloc_fini(void);

void die(int err, const char *str);
static int pool_inc_brk(intptr_t upto_sz);
static struct hdr_node *get_node(void *uptr);
static void *allocate_brk_mem(void **npptr,
                              usize sz,
                              usize old_sz,
                              usize p_offs,
                              u16 pflags);
static void detach_free_node(struct hdr_node *node, struct hdr_node *prev);
static void attach_free_node(struct hdr_node *);
static ptrdiff_t find_block_free(usize *sz);
static ptrdiff_t find_block_offs(const usize *sz);
static void *find_block(void **npptr, usize sz, usize old_sz, u16 flags);
static bool malloc_bounds_check(void *ptr);
static int expand_in_place(struct hdr_node *node, size_t new_sz);
static int arena_allocate_map(void **npptr, usize sz, u16 flags);

static bool is_in_slabs(void *ptr);
static int slab_get_idx(usize sz);
__attribute_nonnull__((1))
static int get_slab_arr_idx(void *uptr);
__attribute_nonnull__((1))
static u32 slab_get_obj_size(void *uptr);
__attribute_nonnull__((1))
static void slab_deallocate_obj(void *uptr);
static void *slab_allocate_obj(usize sz);
__attribute_nonnull__((1))
static void adjust_ptr_to_align(void **aligned_ptr, size_t alignment);

#if defined(DEBUG)
struct heap_core_dbg_info {
	u64 allocs;
	u64 frees;
	u64 brk_increments;
	u64 brk_decrements;
	u64 slab_allocs;
	u64 slab_frees;
} __attribute__((aligned(64)));
#endif


struct heap_core {
	#if defined(DEBUG)
	struct heap_core_dbg_info dbg;
	#endif
	pthread_mutex_t heap_lock;
	void *cbrk; // this one will move as the brk is incremented
	u8 *mem; // this one doesn't move, always at the base.
	struct hdr_node *first_free;
	struct hdr_node *last_free;
	uintptr_t lowest_mmap_addr;
	uintptr_t highest_mmap_addr;
	u32 recent_free_sizes[ARENA_RECENT_CACHE];
	u64 offset;
	u64 heap_size;
	u64 used_size;
	u32 map_count;
	u32 free_count;
} __attribute__((aligned(128)));

#define IS_IN_HEAP(x) \
	((x) >= core->mem && (x) < (u8 *)core + core->heap_size)

struct hdr_node {
	union {
		struct poolhdr {
			u32 pad;
			u32 used;
		} __attribute__((aligned(8))) hdr;

		struct freehdr {
			struct hdr_node *next;
		} __attribute__((aligned(8))) free;

		struct maphdr {
			void *user_ptr;
		} map;

		struct ghosthdr {
			struct hdr_node *true_node;
		} ghost;
	};

	u32 size;
	u16 flags;
	u16 magic;
} __attribute__((aligned(16)));

struct slab_core {
	u64 bitmap[16];
	pthread_mutex_t lock;
	u8 *base;
	u32 elem_sz;
	u32 elems;
} __attribute__((aligned(128)));

struct slab_controller {
	struct slab_core cores[SLAB_COUNT];
	u8 *map_base;
	uintptr_t map_end;
} __attribute__((aligned(128)));

static thread_local struct thread_cache {
	u32 slab_index_cache; // to replace the one in slab_allocate_obj().
} tcache;


#define X(id, v) \
	{{}, PTHREAD_MUTEX_INITIALIZER, NULL, 1U << slab_shift[id], SLAB_SIZE_PER >> slab_shift[id]},
struct slab_controller slab = {
	.cores = {SLAB_SET},
	.map_base = NULL,
};
#undef X

#define SLAB_ALLOC_MAX_SIZE slab.cores[SLAB_COUNT - 1].elem_sz
#define SLAB_ALLOC_MIN_SIZE slab.cores[0].elem_sz


static_assert(sizeof(struct hdr_node) == 0x10, "hdr_node size error");
static_assert(sizeof(struct heap_core) % 0x40 == 0, "heap_core size error");

static_assert(ARENA_RECENT_CACHE <= 4, "arenacore cache too big");


enum arenahdr_flags {
	F_ALLOCATED = (1 << 0), /**< in use allocation.   */
	F_FREE      = (1 << 1), /**< freed allocation.    */
	F_LAST      = (1 << 2), /**< last alloc in heap.  */
	F_FIRST     = (1 << 3), /**< first alloc in heap. */
	F_MAP       = (1 << 4), /**< mmap()-backed alloc. */
	F_EMPTY     = (1 << 5), /**< Zero-size alloc.     */
	F_HEAP      = (1 << 6), /**< Is in the brk heap.  */
	F_GHOST     = (1 << 7), /**< is a ghost header.   */
	F_HAS_GHOST = (1 << 8), /**< has a ghost header.  */
};

enum {
	NO_BLOCK = (ptrdiff_t)-1,
};

#define SLAB_BLOCK	((void *)-2)

enum {
	GLIBC_PTR = (ptrdiff_t)-3,
};

// Clion was complaining this may be null, hence the attribute. It should never be.
static struct heap_core core = {
	.heap_lock = PTHREAD_MUTEX_INITIALIZER,
	.cbrk = NULL,
	.mem = NULL,
	.first_free = NULL,
	.last_free = NULL,
	.lowest_mmap_addr = 0,
	.highest_mmap_addr = 0,
	.recent_free_sizes = {},
	.offset = 0,
	.heap_size = 0,
	.used_size = 0,
	.map_count = 0,
	.free_count = 0
};

#if defined(DEBUG)
	#define CORE_INC_DEBUG(var) core->dbg.var++
	#define CORE_DEC_DEBUG(var) core->dbg.var--
#else
	#define CORE_INC_DEBUG(var)
	#define CORE_DEC_DEBUG(var)
#endif


__attribute__((cold))
static void get_libc_free(void)
{
	if (libc_free) return;
	pthread_mutex_lock(&init_lock);
	if (libc_free) {
		pthread_mutex_unlock(&init_lock);
		return;
	}
	libc_free = dlsym(RTLD_NEXT, "free");
	pthread_mutex_unlock(&init_lock);
	if (!libc_free) {
		die(ERR_NO_LIBC_FREE,
		    PRINTERR(ERR_NO_LIBC_FREE, ERR_NO_LIBC_FREE_STR));
	}
}


__attribute__((cold))
static void get_libc_realloc(void)
{
	if (libc_realloc) return;
	pthread_mutex_lock(&init_lock);
	if (libc_realloc) {
		pthread_mutex_unlock(&init_lock);
		return;
	}
	libc_realloc = dlsym(RTLD_NEXT, "realloc");
	pthread_mutex_unlock(&init_lock);
	if (!libc_realloc) {
		die(ERR_NO_LIBC_REALLOC,
		    PRINTERR(ERR_NO_LIBC_REALLOC, ERR_NO_LIBC_REALLOC_STR));
	}
}


__attribute__((cold))
static void get_libc_malloc_usable_size(void)
{
	if (libc_malloc_usable_size) return;
	pthread_mutex_lock(&init_lock);
	if (libc_malloc_usable_size) {
		// not sure if a double check is needed. maybe?
		pthread_mutex_unlock(&init_lock);
		return;
	}
	libc_malloc_usable_size = dlsym(RTLD_NEXT, "malloc_usable_size");
	pthread_mutex_unlock(&init_lock);
	if (!libc_malloc_usable_size) {
		die(ERR_NO_LIBC_USABLE_SIZE,
		    PRINTERR(ERR_NO_LIBC_USABLE_SIZE,
		             ERR_NO_LIBC_USABLE_SIZE_STR));
	}
}


__attribute__((cold))
void malloc_init(void)
{
	void *cur_brk = sbrk(0);
	if (brk((u8 *)cur_brk + ARENA_MEM_INIT) == -1 && errno == ENOMEM)
		goto err;

	// the brk origin for resetting is core itself.
	core.mem = cur_brk;
	core.heap_size = ARENA_MEM_INIT;
	core.cbrk = (u8 *)cur_brk + ARENA_MEM_INIT;
	// core.reserved = sizeof(struct heap_core);
	// core.mem = (u8 *)core + core->reserved;

	void *map = MMAP_FUNCTION(SLAB_MAX_SIZE + SLAB_SIZE_PER);
	if (map == MAP_FAILED) {
		errno = ENOMEM;
		brk(core.mem);
		goto err;
	}
	const uintptr_t aligned_map = ((uintptr_t)map + (SLAB_SIZE_PER - 1))
	                              & ~(uintptr_t)(SLAB_SIZE_PER - 1);
	const uintptr_t head = aligned_map - (uintptr_t)map;
	const uintptr_t tail = SLAB_SIZE_PER - head;

	if (head) munmap(map, head);
	if (tail) munmap((void *)(aligned_map + SLAB_MAX_SIZE), tail);

	if ((uintptr_t)slab.map_base % 0x4000 != 0) {
		brk(core.mem);
		die(ERR_MAP_ALIGN, PRINTERR(ERR_MAP_ALIGN, ERR_MAP_ALIGN_STR));
	}

	slab.map_base = (u8 *)aligned_map;
	slab.map_end = (uintptr_t)(slab.map_base + SLAB_MAX_SIZE);

	for (usize i = 0; i < SLAB_COUNT; i++) {
		slab.cores[i].base = slab.map_base + (i * SLAB_SIZE_PER);
	}
	core.lowest_mmap_addr = (uintptr_t)slab.cores[0].base;

	return;
err:
	die(ENOMEM, PRINTERR(ENOMEM, ENOMEM_STR));
}


__attribute__((cold, destructor))
void malloc_fini(void)
{
	// We can't clean up the heap as it turns out,
	// because glibc may be using it to the very end,
	// and will crash from an unmapped ptr if we do
	// clean it up, even if this destructor is called last.
	// We can clean up the mutexes though.
	pthread_mutex_destroy(&init_lock);
	pthread_mutex_destroy(&core.heap_lock);
	for (uint i = 0; i < SLAB_COUNT - 1; i++) {
		pthread_mutex_destroy(&slab.cores[i].lock);
	}
}


__attribute__((cold))
_Noreturn void die(int err, const char *str)
{
	write(STDERR_FILENO, str, strlen(str));
	raise(SIGABRT);
	_exit(err);
}


__attribute__((cold))
static int pool_inc_brk(const intptr_t upto_sz)
{
	uintptr_t cur_sz = (intptr_t)core.heap_size;
	do {
		cur_sz *= 2;
		#if ARENA_MEM_LIMIT > 0
		if (cur_sz > ARENA_MEM_LIMIT) {
			cur_sz = ARENA_MEM_LIMIT;
			break;
		}
		#endif
	} while ((cur_sz - core.offset)
	         < ((uintptr_t)upto_sz + sizeof(struct hdr_node)));

	const uintptr_t inc = cur_sz - core.heap_size;

	void *tmp = sbrk((intptr_t)inc);

	if (tmp == (void *)-1) {
		errno = ENOMEM;
		return 1;
	}
	if (tmp != core.cbrk) {
		// absorb the malignant growth.
		const ptrdiff_t gap = (u8 *)tmp - (u8 *)core.cbrk;
		core.offset += gap;
		core.heap_size += gap;
	}

	core.cbrk = (u8 *)tmp + inc;
	core.heap_size += inc;

	CORE_INC_DEBUG(brk_increments);

	return 0;
}


__attribute__((hot))
static struct hdr_node *get_node(void *uptr)
{
	if (is_in_slabs(uptr))
		return SLAB_BLOCK;

	struct hdr_node *ph = (void *)((u8 *)uptr - sizeof(struct hdr_node));
	struct hdr_node *maph = (void *)((u8 *)ph - MAP_PADDING);

	if (ph->magic != HDR_MAGIC && maph->magic == HDR_MAGIC)
		return maph;
	if (!(ph->flags & F_GHOST))
		return ph;
	return ph->ghost.true_node;
}


__attribute__((hot))
static void *allocate_brk_mem(void **npptr,
                              const usize sz,
                              const usize old_sz,
                              usize p_offs,
                              u16 pflags)
{
	const int flags = (!p_offs) ? F_FIRST | F_ALLOCATED : F_ALLOCATED;
	const usize true_sz = sz + sizeof(struct hdr_node);
	struct hdr_node *new_node = (void *)(core.mem + p_offs);

	const bool set_coreoffs = !(new_node->flags & F_FREE);

	new_node->flags = flags | pflags | F_HEAP;
	new_node->size = true_sz;
	new_node->magic = HDR_MAGIC;
	new_node->hdr.pad = HDR_MAGIC;
	new_node->hdr.used = (pflags & F_EMPTY) ? 0 : old_sz;

	if (set_coreoffs) core.offset += true_sz;

	core.used_size += sz;

	CORE_INC_DEBUG(allocs);
	*npptr = (u8 *)new_node + sizeof(struct hdr_node);
	return new_node;
}


static void detach_free_node(struct hdr_node *node, struct hdr_node *prev)
{
	if (prev == NULL) {
		if (node->free.next) {
			core.first_free = node->free.next;
		} else {
			core.first_free = NULL;
			core.last_free = NULL;
		}
		goto done;
	}
	if (node->free.next) {
		prev->free.next = node->free.next;
	} else {
		prev->free.next = NULL;
		core.last_free = prev;
	}
done:
	node->free.next = NULL;
	core.free_count--;
}


static void attach_free_node(struct hdr_node *node)
{
	if (!node) return;

	struct hdr_node *curr = core.first_free;
	struct hdr_node *prev = NULL;

	while (curr && curr < node) {
		prev = curr;
		curr = curr->free.next;
	}

	node->free.next = curr;

	if (prev) {
		prev->free.next = node;
		if ((void *)((u8 *)prev + prev->size) == node) {
			prev->size += node->size;
			prev->free.next = curr;
			node = prev;
			core.free_count--;
		}
	} else {
		core.first_free = node;
	}

	if (!curr) {
		core.last_free = node;
		return;
	}

	if ((void *)((u8 *)node + node->size) != curr)
		return;

	node->size += curr->size;
	node->free.next = curr->free.next;
	core.free_count--;
	if (core.last_free == curr)
		core.last_free = node;
}


/* Returns offset of a free block. Returns -1 if none found. */
__attribute__((hot))
static ptrdiff_t find_block_free(usize *sz)
{
	if (core.free_count == 0)
		return NO_BLOCK;

	struct hdr_node *curr = core.first_free;
	struct hdr_node *prev = NULL;

	while (curr->size - sizeof(struct hdr_node) < *sz) {
		prev = curr;
		if (!curr->free.next) return NO_BLOCK;
		curr = curr->free.next;
	};

	if (core.first_free == curr)
		prev = NULL;
	detach_free_node(curr, prev);

	const usize block_size = curr->size;
	const usize req_size = *sz + sizeof(struct hdr_node);
	constexpr usize min_split_size = sizeof(struct hdr_node) + ALLOC_MIN_SZ;

	if (block_size >= req_size + min_split_size) {
		struct hdr_node *split = (void *)((u8 *)curr + req_size);
		split->size = block_size - req_size;
		split->flags = F_FREE | F_HEAP;
		split->magic = HDR_MAGIC;
		split->free.next = NULL;

		curr->size = req_size;
		*sz = req_size - sizeof(struct hdr_node);
		core.free_count++;
		attach_free_node(split);
	} else {
		*sz = block_size - sizeof(struct hdr_node);
	}

	return (ptrdiff_t)curr - (ptrdiff_t)core.mem;
}


/* Returns offset of a new block. Returns -1 if out of pool space. */
__attribute__((hot))
static ptrdiff_t find_block_offs(const usize *sz)
{
	const usize true_sz = *sz + sizeof(struct hdr_node);
	const usize core_true_sz = core.heap_size;

	if (true_sz > core_true_sz - core.offset)
		return NO_BLOCK;

	return (ptrdiff_t)core.offset;
}


__attribute__((hot))
static void *find_block(void **npptr,
                        const usize sz,
                        const usize old_sz,
                        u16 flags)
{
	ptrdiff_t p_offs;
	usize alloc_given_sz = sz;

	p_offs = find_block_free(&alloc_given_sz);
	if (p_offs != NO_BLOCK)
		goto done;

	p_offs = find_block_offs(&alloc_given_sz);
	if (p_offs != NO_BLOCK)
		goto done;

	if (pool_inc_brk((intptr_t)sz))
		return NULL;
	p_offs = find_block_offs(&alloc_given_sz);
	if (p_offs == NO_BLOCK)
		return NULL;
done:
	return allocate_brk_mem(npptr, alloc_given_sz, old_sz, p_offs, flags);
}


/* Returns false if not in bounds. True if in bounds. */
__attribute__((hot))
static bool malloc_bounds_check(void *ptr)
{
	if (!core.mem) return false;

	const uintptr_t start = (uintptr_t)core.mem;
	const uintptr_t end = (uintptr_t)(core.mem + core.heap_size);
	const uintptr_t addr = (uintptr_t)ptr;
	if (addr >= start && addr < end)
		return true;
	if (is_in_slabs(ptr))
		return true;
	if (addr >= core.lowest_mmap_addr && addr < core.highest_mmap_addr)
		return true;
	return false;
}


/* Expands the allocation in place if there are free nodes of matching size. */
static int expand_in_place(struct hdr_node *node, size_t new_sz)
{
	return 1;
}


/* Creates a new mmap region for a large allocation >= MAP_THRESHOLD */
static int arena_allocate_map(void **npptr, const usize sz, u16 flags)
{
	static constexpr u32 extra_sz = sizeof(struct hdr_node) + MAP_PADDING;
	const usize new_sz = sz + extra_sz;
	struct hdr_node *map_node =
			mmap(NULL, new_sz, MMAP_PFLAGS, MMAP_FFLAGS, 0, 0);
	if (map_node == MAP_FAILED)
		goto err;

	map_node->size = new_sz;
	map_node->map.user_ptr = ((u8 *)map_node + extra_sz);
	map_node->flags |= (F_MAP | F_ALLOCATED | flags);
	map_node->magic = HDR_MAGIC;
	*npptr = map_node->map.user_ptr;
	core.map_count++;
	CORE_INC_DEBUG(allocs);

	const uintptr_t map_addr = (uintptr_t)map_node;
	if (map_addr < core.lowest_mmap_addr)
		core.lowest_mmap_addr = map_addr;
	if (map_addr + new_sz > core.highest_mmap_addr)
		core.highest_mmap_addr = map_addr + new_sz;
	return 0;
err:
	errno = ENOMEM;
	*npptr = NULL;
	return 1;
}


static bool is_in_slabs(void *ptr)
{
	return ((uintptr_t)ptr >= (uintptr_t)slab.map_base
	        && (uintptr_t)ptr < slab.map_end);
}


static int slab_get_idx(usize sz)
{
	return ((64 - __builtin_clzll(sz - 1)) - 4);
}


__attribute_nonnull__((1))
static int get_slab_arr_idx(void *uptr)
{
	if (!is_in_slabs(uptr)) return -1;
	const uintptr_t offs = (((uintptr_t)uptr & ~0x3FFFULL)
	                        - (uintptr_t)slab.map_base);
	return (int)(offs >> 14);
}


/* Returns slab elem size, or if ptr is aligned beyond the standard obj alignment,
   returns slab size - (uptr - obj). Meant for realloc purposes. */
static u32 slab_get_obj_size(void *uptr)
{
	const struct slab_core *s = &slab.cores[get_slab_arr_idx(uptr)];
	const uintptr_t obj_base_addr = (uintptr_t)uptr
	                                & ~((uintptr_t)s->elem_sz - 1);
	if ((uintptr_t)uptr > obj_base_addr)
		return s->elem_sz - ((uintptr_t)uptr - obj_base_addr);
	return s->elem_sz;
}


static void slab_deallocate_obj(void *uptr)
{
	struct slab_core *s = &slab.cores[get_slab_arr_idx(uptr)];
	const uintptr_t obj_base_addr = (uintptr_t)uptr
	                                & ~((uintptr_t)s->elem_sz - 1);
	const uintptr_t slab_base = obj_base_addr & ~0x3FFFULL;
	const ptrdiff_t bit_index = (obj_base_addr - slab_base) / s->elem_sz;
	const int bitmap_index = bit_index / 64;
	const int bit_offset = bit_index % 64;

	pthread_mutex_lock(&s->lock);
	s->bitmap[bitmap_index] &= ~(1ULL << bit_offset);
	pthread_mutex_unlock(&s->lock);
}


static void *slab_allocate_obj(usize sz)
{
	struct slab_core *slab_core;
	static thread_local int index_cache = -1;
	int index = index_cache;

	if (index < 0) {
		index = slab_get_idx(sz);
		index_cache = index;
		goto get_idx;
	}

	const bool cache_over_min_sz = (index > 0)
	                               ? (sz > slab.cores[index - 1].elem_sz)
	                               : true;
	const bool cache_under_max_sz = sz <= slab.cores[index].elem_sz;

	if (cache_over_min_sz && cache_under_max_sz)
		goto get_idx;

	index = slab_get_idx(sz);
	index_cache = index;
get_idx:
	slab_core = &slab.cores[index];
	pthread_mutex_lock(&slab_core->lock);
	i32 index_limit = (slab_core->elems + LLONG_WIDTH - 1) / (LLONG_WIDTH);
	if (index_limit > 16) index_limit = 16;

	i32 bitmap_index = 0;
	i32 bit = 0;
	u32 checked_elements = 0;

	for (; bitmap_index < index_limit; bitmap_index++) {
		u64 bitmap = slab_core->bitmap[bitmap_index];
		const u32 remaining = slab_core->elems - checked_elements;
		const u32 valid_bits = (remaining < LLONG_WIDTH)
		                       ? remaining
		                       : LLONG_WIDTH;
		if (valid_bits < LLONG_WIDTH)
			bitmap |= ~((1ULL << valid_bits) - 1);
		bit = stdc_first_trailing_zero_ull(bitmap);
		if (bit != 0) goto allocate;
		checked_elements += LLONG_WIDTH;
	}
	pthread_mutex_unlock(&slab_core->lock);
	return NULL;
allocate:
	bit--;
	slab_core->bitmap[bitmap_index] |= (1ULL << bit);
	const usize obj_idx =
			((bitmap_index * LLONG_WIDTH) + bit) * slab_core->
			elem_sz;
	pthread_mutex_unlock(&slab_core->lock);

	return (slab_core->base + obj_idx);
}


/* Frees an allocation. Does nothing and returns 1 if ptr is null. */
__attribute__((hot, visibility("default")))
void free(void *ptr)
{
	if (!ptr) return;

	if (!malloc_bounds_check(ptr)) {
		get_libc_free();
		if (libc_free) libc_free(ptr);
		return;
	}
	CORE_INC_DEBUG(frees);
	struct hdr_node *ph = get_node(ptr);

	if (ph == SLAB_BLOCK) {
		// this isnt mutex locked because its already locked inside.
		slab_deallocate_obj(ptr);
		return;
	}
	pthread_mutex_lock(&core.heap_lock);
	if (ph->flags & F_MAP) {
		munmap(ph, ph->size);
		core.map_count--;
		goto done;
	}

	const u32 free_hdr_used = ph->size - sizeof(struct hdr_node);
	ph->flags &= ~F_ALLOCATED;
	ph->flags |= F_FREE;
	ph->free.next = NULL;

	if (!core.first_free) {
		core.first_free = ph;
		core.last_free = ph;
	} else {
		attach_free_node(ph);
	}

	core.used_size -= free_hdr_used;
	core.free_count++;
done:
	pthread_mutex_unlock(&core.heap_lock);

	// TODO implement using cache first instead of freelist
}


/* Allocates <sz> ptr onto the heap. */
__attribute__((hot, visibility("default"), malloc(free, 1)))
void *malloc(size_t sz)
{
	TRIGGER_CORE_INIT;
	if (sz > ALLOC_MAX_SZ) {
		errno = ENOMEM;
		return NULL;
	}

	usize new_sz = sz;

	if (new_sz < ALLOC_MIN_SZ) new_sz = ALLOC_MIN_SZ;
	new_sz = (new_sz + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);

	const u16 pflags = (!sz) ? F_EMPTY : 0;
	void *newptr = NULL;

	if (sz >= SLAB_ALLOC_MIN_SIZE && sz < SLAB_ALLOC_MAX_SIZE) {
		newptr = slab_allocate_obj(sz);
		if (newptr) {
			CORE_INC_DEBUG(slab_allocs);
			return newptr;
		}
		newptr = NULL;
	}
	pthread_mutex_lock(&core.heap_lock);
	if (new_sz >= MAP_THRESHOLD) {
		arena_allocate_map(&newptr, new_sz, pflags);
		goto done;
	}

	if (new_sz > core.heap_size - core.used_size) {
		if (pool_inc_brk((intptr_t)new_sz)) {
			errno = ENOMEM;
			pthread_mutex_unlock(&core.heap_lock);
			return NULL;
		}
	}
	find_block(&newptr, new_sz, sz, pflags);
done:
	if (newptr == NULL) {
		errno = ENOMEM;
		MALLOC_ERR_WRITE(sz);
	}
	pthread_mutex_unlock(&core.heap_lock);
	return newptr;
}


/* Reallocates <ptr> to new size <sz>, returns new ptr. */
__attribute__((hot, visibility("default"), malloc(free, 1)))
void *realloc(void *ptr, size_t sz)
{
	TRIGGER_CORE_INIT;
	if (!ptr) return malloc(sz);

	void *newptr = NULL;

	if (!malloc_bounds_check(ptr)) {
		get_libc_realloc();
		if (libc_realloc) newptr = libc_realloc(ptr, sz);
		return newptr;
	}

	if (sz == 0) {
		free(ptr);
		return malloc(0);
	}

	newptr = malloc(sz);
	if (!newptr) {
		errno = ENOMEM;
		MALLOC_ERR_WRITE(sz);
		return NULL;
	}

	const usize copy_sz = malloc_usable_size(ptr);
	memcpy(newptr, ptr, copy_sz > sz ? sz : copy_sz);
	if (ptr != newptr)
		free(ptr);
	return newptr;
}


/* Like malloc(), but initializes all space to zero. Has safe multiplication. */
__attribute__((hot, visibility("default"), malloc(free, 1)))
void *calloc(size_t n_elem, size_t elem_sz)
{
	if (!n_elem || !elem_sz)
		return malloc(0);

	usize total_sz;
	if (ckd_mul(&total_sz, n_elem, elem_sz)) {
		errno = EOVERFLOW;
		MALLOC_ERR_WRITE(total_sz);
		return NULL;
	}
	void *uptr = malloc(total_sz);
	if (!uptr) return NULL;

	memset(uptr, 0, malloc_usable_size(uptr));
	return uptr;
}


/* Like realloc(), but has safe multiplication. */
__attribute__((visibility("default"), malloc(free, 1)))
void *reallocarray(void *ptr, size_t n_elem, size_t elem_sz)
{
	if (!n_elem || !elem_sz) {
		return malloc(0);
	}
	usize total_sz;
	if (ckd_mul(&total_sz, n_elem, elem_sz)) {
		errno = EOVERFLOW;
		MALLOC_ERR_WRITE(total_sz);
		return NULL;
	}
	return realloc(ptr, total_sz);
}


#define IS_POW_TWO(n)		(((n) & ((n) - 1)) == 0)
#define IS_X_MULT_OF_Y(x, y)	((x) % (y) == 0)
#define ALIGN_UP(align, n)	(((n) + ((align) - 1)) & ~((align) - 1))

__attribute_nonnull__((1))
/* METAL GEAR SOLID V: THE PHANTOM HEADER */
static void adjust_ptr_to_align(void **aligned_ptr, size_t alignment)
{
	uintptr_t new_ptr_addr;
	if (is_in_slabs(*aligned_ptr)) {
		new_ptr_addr = ALIGN_UP(alignment, (uintptr_t)(*aligned_ptr));
		goto done;
	}
	new_ptr_addr = (uintptr_t)(
		(*(u8 **)aligned_ptr) + sizeof(struct hdr_node));
	new_ptr_addr = ALIGN_UP(alignment, new_ptr_addr);


	struct hdr_node *ghost = (void *)
			(new_ptr_addr - sizeof(struct hdr_node));
	ghost->ghost.true_node = (void *)
			((u8 *)(*aligned_ptr) - sizeof(struct hdr_node));
	struct hdr_node *true_node = ghost->ghost.true_node;
	ghost->flags = (F_ALLOCATED | F_GHOST);
	ghost->magic = HDR_MAGIC;
	ghost->size = true_node->size
	              - ((uintptr_t)ghost - (uintptr_t)true_node);
	true_node->flags |= F_HAS_GHOST;
done:
	*aligned_ptr = (void *)new_ptr_addr;
}


__attribute__((visibility("default")))
size_t malloc_usable_size(void *ptr)
{
	if (!ptr) return 0;
	if (!malloc_bounds_check(ptr)) {
		get_libc_malloc_usable_size();
		if (libc_malloc_usable_size)
			return libc_malloc_usable_size(ptr);
		return 0;
	}
	const struct hdr_node *hdr = get_node(ptr);
	if (hdr == SLAB_BLOCK) {
		return slab_get_obj_size(ptr);
	}
	if (hdr->flags & F_MAP) {
		return hdr->size - MAP_PADDING - sizeof(struct hdr_node);
	}
	if (!(hdr->flags & F_HAS_GHOST))
		return hdr->size - sizeof(struct hdr_node);
	hdr = (void *)((u8 *)ptr - sizeof(struct hdr_node));
	return hdr->size - sizeof(struct hdr_node);
}


/* Like malloc(), but guarantees alignment of <alignment> */
__attribute__((visibility("default"), malloc(free, 1), alloc_align(1)))
void *aligned_alloc(size_t alignment, size_t size)
{
	if (!IS_POW_TWO(alignment)) {
		errno = EINVAL;
		goto err;
	}
	TRIGGER_CORE_INIT;

	const size_t new_sz = alignment + size + sizeof(struct hdr_node);
	void *newptr = malloc(new_sz);
	if (!newptr) {
		errno = ENOMEM;
		goto err;
	}
	adjust_ptr_to_align(&newptr, alignment);

	if (!is_in_slabs(newptr)) {
		struct hdr_node *node = get_node(newptr);
		if (node->flags & F_HEAP) { // for realloc.
			node->hdr.used = size;
		}
	}

	DEBUG_ASSERT((uintptr_t)newptr % alignment == 0);
	return newptr;
err:
	MALLOC_ERR_WRITE(size);
	return NULL;
}


/* Like aligned_alloc(), but allocates to *memptr. */
__attribute__((visibility("default"), nonnull(1)))
int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	if (!IS_POW_TWO(alignment)
	    || !IS_X_MULT_OF_Y(alignment, sizeof(void *))) {
		MALLOC_ERR_WRITE(size);
		return EINVAL;
	};
	const int saved_errno = errno;

	// so, adding sizeof(struct hdr_node) is a soft requirememnt
	// even if we know alignment + sz <= SLAB_ALLOC_MIN_SIZE
	// as malloc will use normal heap brk memory instead of
	// the slabs if they're all full, and adding a check
	// for slab size would probably cause too many
	// cache misses. Maybe I will implement a check, just not now.
	// Until then, we just soak the extra 16 bytes of space waste
	// if the allocation ends up going into a slab.
	const size_t new_sz = alignment + size + sizeof(struct hdr_node);
	void *newptr = malloc(new_sz);
	if (!newptr) {
		errno = saved_errno;
		MALLOC_ERR_WRITE(size);
		return ENOMEM;
	}

	adjust_ptr_to_align(&newptr, alignment);
	*memptr = newptr;

	if (!is_in_slabs(newptr)) {
		struct hdr_node *node = get_node(*memptr);
		if (node->flags & F_HEAP) { // for realloc.
			node->hdr.used = size;
		}
	}

	errno = saved_errno;

	DEBUG_ASSERT((uintptr_t)newptr % alignment == 0);
	return 0;
}
