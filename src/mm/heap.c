#include "heap.h"
#include "string.h"
#include "types.h"

extern uint8_t _kernel_end;

static void *heap_base = NULL;
static size_t heap_total_size = 0;
static size_t heap_used_bytes = 0;
static size_t heap_alloc_count = 0;
static struct heap_block *heap_head = NULL;

void heap_init(void) {
    uintptr_t start = ((uintptr_t)&_kernel_end + 0x10000 + 0xFFF) & ~(uintptr_t)0xFFF;
    heap_base = (void *)start;
    heap_total_size = 16 * 1024 * 1024;
    heap_used_bytes = 0;
    heap_alloc_count = 0;

    heap_head = (struct heap_block *)heap_base;
    heap_head->magic = HEAP_MAGIC;
    heap_head->size = heap_total_size - sizeof(struct heap_block);
    heap_head->is_free = 1;
    heap_head->next = NULL;
    heap_head->prev = NULL;
    memset(heap_head->padding, 0, sizeof(heap_head->padding));
}

void *malloc(size_t size) {
    if (heap_head == NULL || size == 0) {
        return NULL;
    }

    size = HEAP_ALIGN(size);

    struct heap_block *curr = heap_head;
    while (curr != NULL) {
        if (curr->is_free && curr->size >= size) {
            if (curr->size >= size + sizeof(struct heap_block) + HEAP_ALIGNMENT) {
                struct heap_block *rem = (struct heap_block *)((uint8_t *)(curr + 1) + size);
                rem->magic = HEAP_MAGIC;
                rem->size = curr->size - size - sizeof(struct heap_block);
                rem->is_free = 1;
                rem->next = curr->next;
                rem->prev = curr;
                memset(rem->padding, 0, sizeof(rem->padding));

                if (rem->next != NULL) {
                    rem->next->prev = rem;
                }
                curr->next = rem;
                curr->size = size;
            }

            curr->is_free = 0;
            heap_used_bytes += curr->size;
            heap_alloc_count++;
            return (void *)(curr + 1);
        }
        curr = curr->next;
    }

    return NULL;
}

void free(void *ptr) {
    if (ptr == NULL || heap_base == NULL) {
        return;
    }

    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)heap_base;
    if (addr < base + sizeof(struct heap_block) || addr >= base + heap_total_size) {
        return;
    }

    struct heap_block *block = (struct heap_block *)ptr - 1;
    if (block->magic != HEAP_MAGIC || block->is_free) {
        return;
    }

    block->is_free = 1;
    if (heap_used_bytes >= block->size) {
        heap_used_bytes -= block->size;
    } else {
        heap_used_bytes = 0;
    }

    if (heap_alloc_count > 0) {
        heap_alloc_count--;
    }

    if (block->next != NULL && block->next->is_free) {
        block->size += sizeof(struct heap_block) + block->next->size;
        block->next = block->next->next;
        if (block->next != NULL) {
            block->next->prev = block;
        }
    }

    if (block->prev != NULL && block->prev->is_free) {
        block->prev->size += sizeof(struct heap_block) + block->size;
        block->prev->next = block->next;
        if (block->next != NULL) {
            block->next->prev = block->prev;
        }
    }
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) {
        return NULL;
    }
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    struct heap_block *block = (struct heap_block *)ptr - 1;
    if (block->magic != HEAP_MAGIC) {
        return NULL;
    }

    if (block->size >= size) {
        return ptr;
    }

    void *new_ptr = malloc(size);
    if (new_ptr == NULL) {
        return NULL;
    }

    size_t copy_bytes = (block->size < size) ? block->size : size;
    memcpy(new_ptr, ptr, copy_bytes);
    free(ptr);
    return new_ptr;
}

size_t heap_get_total(void) {
    return heap_total_size;
}

size_t heap_get_used(void) {
    return heap_used_bytes;
}

size_t heap_get_free(void) {
    return (heap_total_size > heap_used_bytes) ? (heap_total_size - heap_used_bytes) : 0;
}

size_t heap_get_alloc_count(void) {
    return heap_alloc_count;
}

uintptr_t heap_get_start_addr(void) {
    return (uintptr_t)heap_base;
}

uintptr_t heap_get_end_addr(void) {
    return (uintptr_t)heap_base + heap_total_size;
}

int heap_self_test(void) {
    uint8_t *p1 = (uint8_t *)malloc(64);
    if (p1 == NULL) return 1;

    uint8_t *p2 = (uint8_t *)malloc(256);
    if (p2 == NULL) {
        free(p1);
        return 2;
    }

    uint8_t *p3 = (uint8_t *)malloc(1024);
    if (p3 == NULL) {
        free(p1);
        free(p2);
        return 3;
    }

    memset(p1, 0xAA, 64);
    memset(p2, 0xBB, 256);
    memset(p3, 0xCC, 1024);

    for (size_t i = 0; i < 64; i++) {
        if (p1[i] != 0xAA) {
            free(p1); free(p2); free(p3);
            return 4;
        }
    }
    for (size_t i = 0; i < 256; i++) {
        if (p2[i] != 0xBB) {
            free(p1); free(p2); free(p3);
            return 5;
        }
    }
    for (size_t i = 0; i < 1024; i++) {
        if (p3[i] != 0xCC) {
            free(p1); free(p2); free(p3);
            return 6;
        }
    }

    uint32_t *p4 = (uint32_t *)calloc(32, sizeof(uint32_t));
    if (p4 == NULL) {
        free(p1); free(p2); free(p3);
        return 7;
    }
    for (size_t i = 0; i < 32; i++) {
        if (p4[i] != 0) {
            free(p1); free(p2); free(p3); free(p4);
            return 8;
        }
    }

    uint8_t *p1_expanded = (uint8_t *)realloc(p1, 128);
    if (p1_expanded == NULL) {
        free(p1); free(p2); free(p3); free(p4);
        return 9;
    }
    for (size_t i = 0; i < 64; i++) {
        if (p1_expanded[i] != 0xAA) {
            free(p1_expanded); free(p2); free(p3); free(p4);
            return 10;
        }
    }

    free(p1_expanded);
    free(p2);
    free(p3);
    free(p4);

    return 0;
}
