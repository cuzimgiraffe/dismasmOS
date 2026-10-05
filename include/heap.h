#ifndef HEAP_H
#define HEAP_H

#include "types.h"

#define HEAP_MAGIC 0x48454150ULL
#define HEAP_ALIGNMENT 16
#define HEAP_ALIGN(sz) (((sz) + (HEAP_ALIGNMENT - 1)) & ~(HEAP_ALIGNMENT - 1))

struct heap_block {
    uint64_t magic;
    size_t size;
    uint8_t is_free;
    uint8_t padding[15];
    struct heap_block *next;
    struct heap_block *prev;
};

void heap_init(void);
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

size_t heap_get_total(void);
size_t heap_get_used(void);
size_t heap_get_free(void);
size_t heap_get_alloc_count(void);
uintptr_t heap_get_start_addr(void);
uintptr_t heap_get_end_addr(void);
int heap_self_test(void);

#endif
