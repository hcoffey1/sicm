#ifndef OP_NEW_DEL
#define OP_NEW_DEL

#include <stdio.h>
#include <stdlib.h> /* For exit() */
#include <string.h>

void* sh_alloc(int id, size_t sz);
void* sh_aligned_alloc(int id, size_t alignment, size_t sz);
void* sh_memalign(int id, size_t alignment, size_t sz);
int sh_posix_memalign(int id, void **ptr, size_t alignment, size_t sz);
void* sh_calloc(int id, size_t num, size_t sz);
void* sh_realloc(int id, void *ptr, size_t sz);
void sh_free(void* ptr);

/* Just define `malloc`, `calloc`, `realloc`, and `free`. We want
 * all allocations to come through us no matter what, else we'll have edge cases
 * where a stdlib call allocates memory without having been transformed by our compiler
 * pass. In some cases this results in a `malloc` call in a shared library (which uses `libc`'s `malloc`),
 * and an inlined `free` call which gets transformed by our compiler wrappers.
 */
void *malloc(size_t size) {
  return sh_alloc(0, size);
}

void *calloc(size_t num, size_t size) {
  return sh_calloc(0, num, size);
}

void *realloc(void *ptr, size_t new_size) {
  return sh_realloc(0, ptr, new_size);
}

void free(void *ptr) {
  sh_free(ptr);
}

int posix_memalign(void **ptr, size_t alignment, size_t size) {
  return sh_posix_memalign(0, ptr, alignment, size);
}

void * aligned_alloc(size_t alignment, size_t size) {
  return sh_aligned_alloc(0, alignment, size);
}

void * _aligned_malloc(size_t alignment, size_t size) {
  return sh_aligned_alloc(0, alignment, size);
}

void * memalign(size_t alignment, size_t size) {
  return sh_memalign(0, alignment, size);
}

/*
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  return sh_mmap(addr, length, prot, flags, fd, offset);
}

int munmap(void *addr, size_t length) {
  return sh_munmap(addr, length);
}
*/

#endif
