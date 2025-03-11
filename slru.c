// Simple least-recently-used cache implementation in C
// Author: Aarni Gratseff (aarni.gratseff@gmail.com)
// Created (yyyy-mm-dd): 2025-03-10

#include "slru.h"

#include <string.h>
#include <assert.h>

#if !defined(NDEBUG) || defined(DEBUG)
#   define SLRU_HC_TESTS 1
#   define SLRU_ONLY_IN_DEBUG(x) x
#else
#   define SLRU_HC_TESTS 0
#   define SLRU_ONLY_IN_DEBUG(x)
#endif

static uint32_t slru_popcount(uint32_t n) {
    // https://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel
    n = n - ((n >> 1) & 0x55555555); // reuse input as temporary
    n = (n & 0x33333333) + ((n >> 2) & 0x33333333); // temp
    return ((n + (n >> 4) & 0xF0F0F0F) * 0x1010101) >> 24; // count
}

static uint32_t slru_ctz(uint32_t x) {
    return slru_popcount(~x & (x - 1));
}

static uint32_t slru_hash(const void *key, uint32_t len, uint32_t seed,
    uint32_t hash_table_size) {
    const uint32_t m = 0x5bd1e995;
    const uint32_t r = 24;

    uint32_t h = seed ^ (uint32_t)len;

    const uint8_t *data = (const uint8_t *)key;

    while (len >= 4) {
        uint32_t k;
        k = data[0];
        k |= (uint32_t)data[1] << 8;
        k |= (uint32_t)data[2] << 16;
        k |= (uint32_t)data[3] << 24;

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    switch (len) {
        case 3:
            h ^= (uint32_t)data[2] << 16;
        case 2:
            h ^= (uint32_t)data[1] << 8;
        case 1:
            h ^= data[0];
            h *= m;
    };

    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h % hash_table_size;
}

int slru_cmp_keys(const void *a, uint32_t a_length,
    const void *b, uint32_t b_length) {
    if (a_length != b_length) return 0;
    return memcmp(a, b, a_length) == 0 ? 1 : 0;
}

//

typedef struct slru_item_t {
    void *key;
    uint32_t key_length;
    uint32_t value;
    uint32_t consumption;
    uint32_t timestamp;
    uint32_t next;
} slru_item_t;

typedef struct slru_t {
    void *evict_user;
    slru_evict_func_t evict_func;
    slru_malloc_func_t malloc_func;
    slru_free_func_t free_func;
    uint32_t *hash_table;
    slru_item_t *items;
    uint32_t num_items;
    uint32_t hash_table_size;
    uint32_t seed;
    uint32_t first_free;
    uint32_t timestamp;
    uint32_t cache_left;
    SLRU_ONLY_IN_DEBUG(uint32_t debug_cache_size;)
} slru_t;

//
// Private functions

static void slru_check_internal_state(const slru_t *slru) {
    assert(slru);
    assert(slru->malloc_func);
    assert(slru->free_func);
    assert(slru->hash_table_size != 0);

    assert(slru->first_free == UINT32_MAX ||
        slru->first_free < slru->num_items);

#if SLRU_HC_TESTS
    uint32_t consumed_total = 0;
    for (uint32_t i = 0; i < slru->num_items; ++i) {
        assert(slru->items[i].next == UINT32_MAX ||
            slru->items[i].next < slru->num_items);
        consumed_total += slru->items[i].consumption;
    }

    assert(consumed_total + slru->cache_left == slru->debug_cache_size);

    for (uint32_t i = 0; i < slru->hash_table_size; ++i)
        assert(slru->hash_table[i] == UINT32_MAX ||
            slru->hash_table[i] < slru->num_items);
#endif
}

static int slru_free_one(slru_t *slru) {
    slru_check_internal_state(slru);

    uint32_t min_timestamp = UINT32_MAX;
    uint32_t min_index = UINT32_MAX;
    uint32_t min_prev_index = UINT32_MAX;
    uint32_t min_hash = UINT32_MAX;

    for (uint32_t i = 0; i < slru->hash_table_size; ++i) {
        uint32_t prev_iter = UINT32_MAX;
        uint32_t iter = slru->hash_table[i];
        while (iter != UINT32_MAX) {
            assert(iter < slru->num_items);
            const slru_item_t *item = &slru->items[iter];

            if (item->timestamp < min_timestamp) {
                min_timestamp = item->timestamp;
                min_index = iter;
                min_prev_index = prev_iter;
                min_hash = i;
            }

            prev_iter = iter;
            iter = item->next;
        }
    }

    if (min_index == UINT32_MAX)
        return SLRU_ERROR;

    assert(min_index < slru->num_items);
    slru_item_t *item = &slru->items[min_index];

    assert(slru->evict_func);
    slru->evict_func(slru->evict_user, item->value);

    slru->free_func(item->key);
    item->key = NULL;

    if (min_prev_index != UINT32_MAX) {
        assert(slru->items[min_prev_index].next == min_index);
        slru->items[min_prev_index].next = item->next;
    } else {
        assert(min_hash < slru->hash_table_size);
        assert(slru->hash_table[min_hash] == min_index);
        slru->hash_table[min_hash] = item->next;
    }

    item->next = slru->first_free;
    slru->first_free = min_index;

    slru->cache_left += item->consumption;
    item->consumption = 0; // Used for debug-checking

    return SLRU_OK;
}

static uint32_t slru_find_index_by_hash(const slru_t *slru, const void *key,
    uint32_t key_length, uint32_t hash) {
    assert(hash < slru->hash_table_size);
    assert(hash == slru_hash(key, key_length, slru->seed,
        slru->hash_table_size));
    uint32_t iter = slru->hash_table[hash];
    assert(iter == UINT32_MAX || iter < slru->num_items);
    while (iter != UINT32_MAX &&
        !slru_cmp_keys(key, key_length,
            slru->items[iter].key, slru->items[iter].key_length)) {
        iter = slru->items[iter].next;
        assert(iter == UINT32_MAX || iter < slru->num_items);
    }
    return iter;
}

static uint32_t slru_find_index(const slru_t *slru, const void *key,
    uint32_t key_length) {
    return slru_find_index_by_hash(slru, key, key_length,
        slru_hash(key, key_length, slru->seed, slru->hash_table_size));
}

//

slru_t *slru_create(uint32_t hash_table_size, uint32_t num_initial_items,
    uint32_t cache_size, void *evict_user, slru_evict_func_t evict_func,
    slru_malloc_func_t malloc_func, slru_free_func_t free_func) {
    assert(hash_table_size != 0 && cache_size != 0);
    assert(evict_func && malloc_func && free_func);

    slru_t *slru = malloc_func(sizeof(slru_t));
    if (!slru)
        return NULL;

    memset(slru, 0, sizeof(*slru));

    slru->evict_user = evict_user;
    slru->evict_func = evict_func;

    slru->malloc_func = malloc_func;
    slru->free_func = free_func;

    slru->cache_left = cache_size;
    SLRU_ONLY_IN_DEBUG(slru->debug_cache_size = cache_size;)

    size_t hash_table_bytesize = sizeof(*slru->hash_table) * hash_table_size;
    slru->hash_table = slru->malloc_func(hash_table_bytesize);
    if (!slru->hash_table) {
        slru_destroy(slru);
        return NULL;
    }

    memset(slru->hash_table, 0xff, hash_table_bytesize);

    slru->hash_table_size = hash_table_size;

    if (num_initial_items != 0) {
        uint32_t items_bytesize = sizeof(*slru->items) * num_initial_items;
        slru->items = slru->malloc_func(items_bytesize);
        if (!slru->items) {
            slru_destroy(slru);
            return NULL;
        }

        memset(slru->items, 0, items_bytesize);

        for (uint32_t i = 0; i < num_initial_items - 1; ++i) {
            slru->items[i].next = i + 1;
        }

        slru->items[num_initial_items - 1].next = UINT32_MAX;

        slru->num_items = num_initial_items;
        slru->first_free = 0;
    } else {
        slru->first_free = UINT32_MAX;
    }

    slru_check_internal_state(slru);

    return slru;
}

void slru_destroy(slru_t *slru) {
    slru_check_internal_state(slru);

    for (uint32_t i = 0; i < slru->num_items; ++i) {
        slru_item_t *item = &slru->items[i];
        if (item->consumption != 0) {
            slru->free_func(slru->items[i].key);
            slru->evict_func(slru->evict_user, item->value);
        } else {
            assert(item->key == NULL);
        }
    }

    slru->free_func(slru->items);
    slru->free_func(slru->hash_table);
    slru->free_func(slru);
}

void slru_remove_all(slru_t *slru) {
    slru_check_internal_state(slru);

    for (uint32_t i = 0; i < slru->hash_table_size; ++i) {
        uint32_t iter = slru->hash_table[i];
        while (iter != UINT32_MAX) {
            assert(iter < slru->num_items);
            slru_item_t *item = &slru->items[iter];

            assert(slru->evict_func);
            slru->evict_func(slru->evict_user, item->value);

            slru->free_func(item->key);
            item->key = NULL;

            slru->cache_left += item->consumption;

            iter = item->next;
        }
    }

    memset(slru->hash_table, 0xff, sizeof(*slru->hash_table) * slru->hash_table_size);
    memset(slru->items, 0, sizeof(*slru->items) * slru->num_items);

    for (uint32_t i = 0; i < slru->num_items - 1; ++i) {
        slru->items[i].next = i + 1;
    }

    slru->items[slru->num_items - 1].next = UINT32_MAX;
    slru->first_free = 0;
}

int slru_insert(slru_t *slru, const void *key, uint32_t key_length,
    uint32_t value, uint32_t consumption) {
    slru_check_internal_state(slru);
    assert(key && key_length != 0);
    assert(consumption != 0);

    //

    while (slru->cache_left < consumption) {
        if (slru_free_one(slru) != SLRU_OK)
            break;
    }

    if (slru->cache_left < consumption)
        return SLRU_DOESNT_FIT;

    slru->cache_left -= consumption;

    //

    uint32_t hash = slru_hash(key, key_length, slru->seed,
        slru->hash_table_size);
    assert(hash < slru->hash_table_size);
    assert(slru_find_index_by_hash(slru, key, key_length, hash) == UINT32_MAX);

    if (slru->first_free == UINT32_MAX) {
        if (slru->num_items == 0) {
            uint32_t num_items = 1 << slru_ctz(slru->hash_table_size);

            slru->items = slru->malloc_func(sizeof(*slru->items) *
                num_items);
            if (!slru->items)
                return SLRU_OOM;

            slru->num_items = num_items;

            for (uint32_t i = 0; i < slru->num_items - 1; ++i)
                slru->items[i].next = i + 1;

            slru->items[slru->num_items - 1].next = UINT32_MAX;
            slru->first_free = 0;
        } else {
            slru_item_t *old_items = slru->items;
            uint32_t old_num_items = slru->num_items;
            slru->num_items *= 2;

            uint32_t items_bytesize = sizeof(*slru->items) * slru->num_items;
            slru->items = slru->malloc_func(items_bytesize);
            if (!slru->items)
                return SLRU_OOM;

            uint32_t old_items_bytesize = sizeof(*slru->items) * old_num_items;
            memcpy(slru->items, old_items, old_items_bytesize);
            slru->free_func(old_items);

            memset(slru->items + old_num_items, 0,
                items_bytesize - old_items_bytesize);

            for (uint32_t i = old_num_items; i < slru->num_items - 1; ++i)
                slru->items[i].next = i + 1;

            slru->items[slru->num_items - 1].next = UINT32_MAX;
            slru->first_free = old_num_items;
        }
    }

    uint32_t index = slru->first_free; // Take first free
    assert(index < slru->num_items);
    slru_item_t *item = &slru->items[index];

    // 'consumption' used to debug-check that the item is not used
    assert(item->consumption == 0);

    item->key = slru->malloc_func(key_length);
    if (!item->key)
        return SLRU_OOM;

    memcpy(item->key, key, key_length);
    item->key_length = key_length;

    // Update links
    slru->first_free = item->next;
    item->next = slru->hash_table[hash];
    slru->hash_table[hash] = index;

    item->value = value;
    item->consumption = consumption;
    item->timestamp = ++slru->timestamp;

    return SLRU_OK;
}

int slru_remove(slru_t *slru, const void *key, uint32_t key_length) {
    slru_check_internal_state(slru);
    uint32_t hash = slru_hash(key, key_length, slru->seed,
        slru->hash_table_size);
    uint32_t index = slru_find_index_by_hash(slru, key, key_length, hash);
    if (index == UINT32_MAX)
        return SLRU_NOT_FOUND;

    assert(index < slru->num_items);
    slru_item_t *item = &slru->items[index];

    uint32_t prev_index = UINT32_MAX;
    uint32_t iter = slru->hash_table[hash];
    while (iter != UINT32_MAX) {
        if (iter == index)
            break;
        prev_index = iter;
        iter = slru->items[iter].next;
    }

    if (prev_index == UINT32_MAX) {
        assert(slru->hash_table[hash] == index);
        slru->hash_table[hash] = item->next;
    } else {
        assert(slru->items[prev_index].next == index);
        slru->items[prev_index].next = item->next;
    }

    item->next = slru->first_free;
    slru->first_free = index;

    slru->free_func(item->key);
    item->key = NULL;

    slru->cache_left += item->consumption;
    item->consumption = 0; // Used for debug-checking

    return SLRU_OK;
}

uint32_t slru_fetch(slru_t *slru, const void *key, uint32_t key_length,
    uint32_t invalid_value) {
    slru_check_internal_state(slru);
    uint32_t index = slru_find_index(slru, key, key_length);
    if (index == UINT32_MAX)
        return invalid_value;
    assert(index < slru->num_items);
    slru_item_t *item = &slru->items[index];
    item->timestamp = ++slru->timestamp;
    return item->value;
}

//
// c-string key helper functions

int slru_insert_strkey(slru_t *slru, const char *key, uint32_t value,
    uint32_t consumption) {
    return slru_insert(slru, key, (uint32_t)strlen(key), value, consumption);
}

int slru_remove_strkey(slru_t *slru, const char *key) {
    return slru_remove(slru, key, (uint32_t)strlen(key));
}

uint32_t slru_fetch_strkey(slru_t *slru, const char *key,
    uint32_t invalid_value) {
    return slru_fetch(slru, key, (uint32_t)strlen(key), invalid_value);
}
