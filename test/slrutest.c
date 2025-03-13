#include "slru.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#define HASH_SEED 0xcafebabe
#define HASH_TABLE_SIZE 256
#define NUM_INITIAL_ITEMS 2
#define CACHE_SIZE 8

void evict(void *user, uint32_t value) {
    printf("Evicting %u\n", value);
}

typedef struct tracked_allocation_t tracked_allocation_t;
typedef struct tracked_allocation_t {
    void *ptr;
    size_t sz;
    tracked_allocation_t *next;
} tracked_allocation_t;

static tracked_allocation_t allocations_head = { NULL, 0, NULL };
static size_t total_bytes_allocated = 0;

void *malloc_wrapper(size_t sz) {
    void *ptr = malloc(sz);
    if (!ptr)
        return NULL;

    tracked_allocation_t *t = malloc(sizeof(tracked_allocation_t));
    t->ptr = ptr;
    t->sz = sz;
    t->next = allocations_head.next;
    allocations_head.next = t;

    total_bytes_allocated += sz;

    return ptr;
}

void free_wrapper(void *ptr) {
    if (!ptr)
        return;

    tracked_allocation_t *prev = &allocations_head;
    tracked_allocation_t *iter = allocations_head.next;
    while (iter) {
        if (iter->ptr == ptr) {
            assert(total_bytes_allocated >= iter->sz);
            total_bytes_allocated -= iter->sz;
            prev->next = iter->next;
            free(iter);
            break;
        }

        prev = iter;
        iter = iter->next;
    }

    free(ptr);
}

int main() {
    printf("slru_create\n");
    slru_t *slru = slru_create(HASH_TABLE_SIZE, NUM_INITIAL_ITEMS, CACHE_SIZE,
        HASH_SEED, NULL, evict, malloc_wrapper, free_wrapper);
    if (!slru) {
        return EXIT_FAILURE;
    }

    slru_insert_strkey(slru, "123", 123, 5);
    uint32_t value1 = slru_fetch_strkey(slru, "123", 0);
    slru_insert_strkey(slru, "234", 234, 3);
    uint32_t value2 = slru_fetch_strkey(slru, "123", 0);
    slru_remove_strkey(slru, "123");
    uint32_t value3 = slru_fetch_strkey(slru, "234", 0);
    slru_insert_strkey(slru, "345", 345, 1);
    slru_insert_strkey(slru, "456", 456, 3);
    slru_insert_strkey(slru, "567", 567, 2);
    slru_insert_strkey(slru, "678", 678, 1);
    slru_insert_strkey(slru, "789", 789, 1);
    uint32_t value4 = slru_fetch_strkey(slru, "123", 0);
    uint32_t value5 = slru_fetch_strkey(slru, "234", 0);
    slru_insert_strkey(slru, "890", 890, 1);
    slru_remove_strkey(slru, "456");
    uint32_t value6 = slru_fetch_strkey(slru, "345", 0);
    uint32_t value7 = slru_fetch_strkey(slru, "456", 0);

    printf("slru_destroy\n");
    slru_destroy(slru);
    slru = NULL;

    assert(total_bytes_allocated == 0);
    assert(allocations_head.next == NULL);

    return EXIT_SUCCESS;
}
