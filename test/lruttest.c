#include "lrutrack.h"

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

    int found = 0;
    tracked_allocation_t *prev = &allocations_head;
    tracked_allocation_t *iter = allocations_head.next;
    while (iter) {
        if (iter->ptr == ptr) {
            assert(total_bytes_allocated >= iter->sz);
            total_bytes_allocated -= iter->sz;
            prev->next = iter->next;
            free(iter);
            found = 1;
            break;
        }

        prev = iter;
        iter = iter->next;
    }

    assert(found);
    free(ptr);
}

#if LRUTRACK_32BIT_KEY

#define FNV_32_PRIME ((uint32_t)0x01000193)

static uint32_t fnv32a_str(const char *str, uint32_t seed) {
    uint32_t hash = seed;
    const unsigned char *s = (const unsigned char *)str;
    while (*s) {
        hash ^= (uint32_t)*s++;
        hash *= FNV_32_PRIME;
    }
    return hash;
}

#endif

//

static void evict(void *user, lrutrack_value_t value) {
    printf("Evicting %u\n", value);
}

#define HASH_SEED 0xcafebabe
#define INVALID_VALUE 0

static void _insert(lrutrack_t *t, const char *key, lrutrack_value_t value) {
    printf("Inserting %u\n", value);
#if !LRUTRACK_32BIT_KEY
    lrutrack_insert_strkey(t, key, value);
#else
    lrutrack_insert(t, fnv32a_str(key, HASH_SEED), value);
#endif
}

static void _remove(lrutrack_t *t, const char *key) {
#if !LRUTRACK_32BIT_KEY
    lrutrack_remove_strkey(t, key);
#else
    lrutrack_remove(t, fnv32a_str(key, HASH_SEED));
#endif
}

static void _use(lrutrack_t *t, const char *key, lrutrack_value_t expected_value) {
#if !LRUTRACK_32BIT_KEY
    lrutrack_value_t v = lrutrack_use_strkey(t, key);
#else
    lrutrack_value_t v = lrutrack_use(t, fnv32a_str(key, HASH_SEED));
#endif
    if (v == INVALID_VALUE) {
        printf("Using %s - not found\n", key);
    } else {
        printf("Using %s\n", key);
        assert(v == expected_value);
    }
}

#define HASH_TABLE_SIZE 256
#define NUM_INITIAL_ITEMS 2

int main() {
    printf("lrutrack_create\n");
    lrutrack_t *t = lrutrack_create(HASH_TABLE_SIZE, NUM_INITIAL_ITEMS, HASH_SEED,
        INVALID_VALUE, NULL, evict, malloc_wrapper, free_wrapper);
    if (!t) {
        return EXIT_FAILURE;
    }

    _insert(t, "123", 123);
    _use(t, "123", 123);
    _insert(t, "234", 234);
    _use(t, "123", 123);
    _remove(t, "123");
    _use(t, "234", 234);
    _insert(t, "345", 345);
    _insert(t, "456", 456);
    _insert(t, "567", 567);
    lrutrack_remove_lru(t);
    _insert(t, "678", 678);
    //lrutrack_remove_all(t);
    _insert(t, "789", 789);
    lrutrack_remove_lru(t);
    _use(t, "123", 123);
    _use(t, "234", 234);
    _use(t, "456", 456);
    _insert(t, "890", 890);
    _remove(t, "456");
    _use(t, "345", 345);
    _use(t, "456", 456);

    printf("lrutrack_destroy\n");
    lrutrack_destroy(t);
    t = NULL;

    assert(total_bytes_allocated == 0);
    assert(allocations_head.next == NULL);

    return EXIT_SUCCESS;
}
