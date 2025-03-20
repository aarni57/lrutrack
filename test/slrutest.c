#include "slru.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

typedef struct tracked_allocation_t tracked_allocation_t;
typedef struct tracked_allocation_t {
    void *ptr;
    size_t sz;
    tracked_allocation_t *next;
} tracked_allocation_t;

static tracked_allocation_t allocations_head = { NULL, 0, NULL };
static size_t total_bytes_allocated = 0;

static void *malloc_wrapper(size_t sz) {
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

static void free_wrapper(void *ptr) {
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

//

static void evict(void *user, uint32_t value) {
    printf("Evicting %u\n", value);
}

#define INVALID_VALUE 0

static void _insert(slru_t *slru, const char *key, slru_value_t value) {
    printf("Inserting %u\n", value);
    slru_insert_strkey(slru, key, value);
}

static void _remove(slru_t *slru, const char *key) {
    slru_remove_strkey(slru, key);
}

static void _fetch(slru_t *slru, const char *key, slru_value_t expected_value) {
    uint32_t v = slru_fetch_strkey(slru, key);
    if (v == INVALID_VALUE) {
        printf("Fetching %s - not found\n", key);
    } else {
        printf("Fetching %s\n", key);
        assert(v == expected_value);
    }
}

#define HASH_SEED 0xcafebabe
#define HASH_TABLE_SIZE 256
#define NUM_INITIAL_ITEMS 2

int main() {
    printf("slru_create\n");
    slru_t *slru = slru_create(HASH_TABLE_SIZE, NUM_INITIAL_ITEMS, HASH_SEED,
        INVALID_VALUE, NULL, evict, malloc_wrapper, free_wrapper);
    if (!slru) {
        return EXIT_FAILURE;
    }

    _insert(slru, "123", 123);
    _fetch(slru, "123", 123);
    _insert(slru, "234", 234);
    _fetch(slru, "123", 123);
    _remove(slru, "123");
    _fetch(slru, "234", 234);
    _insert(slru, "345", 345);
    _insert(slru, "456", 456);
    _insert(slru, "567", 567);
    slru_remove_lru(slru);
    _insert(slru, "678", 678);
    //slru_remove_all(slru);
    _insert(slru, "789", 789);
    slru_remove_lru(slru);
    _fetch(slru, "123", 123);
    _fetch(slru, "234", 234);
    _fetch(slru, "456", 456);
    _insert(slru, "890", 890);
    _remove(slru, "456");
    _fetch(slru, "345", 345);
    _fetch(slru, "456", 456);

    printf("slru_destroy\n");
    slru_destroy(slru);
    slru = NULL;

    assert(total_bytes_allocated == 0);
    assert(allocations_head.next == NULL);

    return EXIT_SUCCESS;
}
