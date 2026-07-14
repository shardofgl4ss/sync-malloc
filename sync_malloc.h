#ifndef MALLOC_LIBRARY_H
#define MALLOC_LIBRARY_H

#include <sys/types.h>

__attribute__((visibility("default")))
size_t malloc_usable_size(void *);
__attribute__((visibility("default")))
extern void free(void *);
__attribute__((visibility("default"), malloc(free, 1)))
extern void *malloc(size_t);
__attribute__((visibility("default"), malloc(free, 1)))
extern void *realloc(void *ptr, size_t sz);
__attribute__((visibility("default"), malloc(free, 1)))
extern void *calloc(size_t n_elem, size_t elem_sz);
__attribute__((visibility("default"), malloc(free, 1)))
extern void *reallocarray(void *ptr, size_t n_elem, size_t elem_sz);
__attribute__((visibility("default"), malloc(free, 1), alloc_align(1)))
extern void *aligned_alloc(size_t alignment, size_t size);
__attribute__((visibility("default"), nonnull(1)))
extern int posix_memalign(void **memptr, size_t alignment, size_t size);


#endif // MALLOC_LIBRARY_H