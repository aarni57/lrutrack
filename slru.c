// Simple least-recently-used cache implementation in C
// Author: Aarni Gratseff (aarni.gratseff@gmail.com)
// Created (yyyy-mm-dd): 2025-03-10

#include "slru.h"

#include <string.h>
#include <assert.h>

#if !defined(NDEBUG)
#   define SLRU_ONLY_IN_DEBUG(x) x
#else
#   define SLRU_ONLY_IN_DEBUG(x)
#endif

static int slru_is_power_of_two(uint32_t x) {
    return x > 0 && (x & (x - 1)) == 0;
}

static uint32_t slru_hash(const void *key, uint32_t len, uint32_t seed,
    uint32_t hash_table_size) {
    assert(slru_is_power_of_two(hash_table_size));

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
    return h & (hash_table_size - 1);
}

int slru_cmp_keys(const void *a, uint16_t a_length,
    const void *b, uint16_t b_length) {
    if (a_length != b_length) return 0;
    return memcmp(a, b, a_length) == 0 ? 1 : 0;
}

//

typedef struct slru_item_t {
    void *key;
    uint16_t key_length;
    uint16_t consumption;
    uint32_t value;
    uint32_t next; // Next item index on a hash table row
} slru_item_t;

typedef struct slru_t {
    void *evict_user;
    slru_evict_func_t evict_func;
    slru_malloc_func_t malloc_func;
    slru_free_func_t free_func;
    uint32_t *hash_table; // Index for first item on a row
    uint32_t *hash_table_lru_links; // 2 * hash_table_size, 0 = prev, 1 = next
    slru_item_t *items;
    uint32_t num_items;
    uint32_t num_items_in_use;
    uint32_t hash_table_size;
    uint32_t lru_head; // Hash table index
    uint32_t lru_tail;
    uint32_t seed;
    uint32_t first_free;
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

    assert(slru->num_items_in_use <= slru->num_items);

    assert(slru->first_free == UINT32_MAX ||
        slru->first_free < slru->num_items);

    assert(slru->lru_head == UINT32_MAX ||
        slru->lru_head < slru->hash_table_size);
    assert(slru->lru_tail == UINT32_MAX ||
        slru->lru_tail < slru->hash_table_size);
    assert(slru->lru_head == UINT32_MAX ||
        slru->hash_table_lru_links[slru->lru_head * 2 + 0] == UINT32_MAX);
    assert(slru->lru_tail == UINT32_MAX ||
        slru->hash_table_lru_links[slru->lru_tail * 2 + 1] == UINT32_MAX);

#if SLRU_HC_TESTS
    uint32_t prev_iter = UINT32_MAX;
    uint32_t iter = slru->lru_head;
    while (iter != UINT32_MAX) {
        assert(iter < slru->hash_table_size);
        assert(slru->hash_table_lru_links[iter * 2 + 0] == prev_iter);
        prev_iter = iter;
        iter = slru->hash_table_lru_links[iter * 2 + 1];
    }

    assert(prev_iter == slru->lru_tail);

    for (uint32_t i = 0; i < slru->hash_table_size; ++i) {
        assert(slru->hash_table_lru_links[i * 2 + 0] != i);
        assert(slru->hash_table_lru_links[i * 2 + 1] != i);
        if (slru->hash_table[i] == UINT32_MAX) {
            assert(slru->hash_table_lru_links[i * 2 + 0] == UINT32_MAX);
            assert(slru->hash_table_lru_links[i * 2 + 1] == UINT32_MAX);
        }
    }

    uint32_t num_items_in_use_counter = 0;
    uint32_t consumed_total = 0;
    for (uint32_t i = 0; i < slru->num_items; ++i) {
        if (slru->items[i].consumption != 0) {
            assert(slru->items[i].next == UINT32_MAX ||
                slru->items[i].next < slru->num_items);
            consumed_total += slru->items[i].consumption;
            num_items_in_use_counter++;
        }
    }

    assert(num_items_in_use_counter == slru->num_items_in_use);
    assert(consumed_total + slru->cache_left == slru->debug_cache_size);

    for (uint32_t i = 0; i < slru->hash_table_size; ++i) {
        assert(slru->hash_table[i] == UINT32_MAX ||
            slru->hash_table[i] < slru->num_items);

        uint32_t iter = 0;
        while (iter != UINT32_MAX) {
            const slru_item_t *item = &slru->items[iter];

            iter = item->next;
        }
    }
#endif
}

static int slru_evict_oldest(slru_t *slru) {
    assert(slru->lru_tail < slru->hash_table_size);
    uint32_t new_tail = slru->hash_table_lru_links[slru->lru_tail * 2 + 0];
    slru->hash_table_lru_links[slru->lru_tail * 2 + 0] = UINT32_MAX;
    assert(slru->hash_table_lru_links[slru->lru_tail * 2 + 1] == UINT32_MAX);
    assert(new_tail < slru->hash_table_size);
    slru->hash_table_lru_links[new_tail * 2 + 1] = UINT32_MAX;

    uint32_t iter = slru->hash_table[slru->lru_tail];
    slru->hash_table[slru->lru_tail] = UINT32_MAX;

    if (slru->lru_head == slru->lru_tail)
        slru->lru_head = new_tail;
    slru->lru_tail = new_tail;

    while (iter != UINT32_MAX) {
        assert(iter < slru->num_items);
        slru_item_t *item = &slru->items[iter];
        assert(item->consumption != 0);
        slru->free_func(item->key);
        item->key = NULL;
        assert(slru->evict_func);
        slru->evict_func(slru->evict_user, item->value);
        slru->num_items_in_use--;
        slru->cache_left += item->consumption;
        item->consumption = 0;
        uint32_t next = item->next;
        item->next = slru->first_free;
        slru->first_free = iter;
        iter = next;
    }

    return SLRU_OK;
}

static uint32_t slru_find_index(const slru_t *slru, const void *key,
    uint16_t key_length, uint32_t hash) {
    assert(key != NULL && key_length != 0);
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

static void slru_insert_to_lru_head(slru_t *slru, uint32_t i) {
    if (slru->lru_head != UINT32_MAX) {
        slru->hash_table_lru_links[slru->lru_head * 2 + 0] = i;
        slru->hash_table_lru_links[i * 2 + 1] = slru->lru_head;
        slru->lru_head = i;
    } else {
        slru->lru_head = i;
        slru->lru_tail = i;
    }
}

static void slru_remove_from_lru(slru_t *slru, uint32_t i) {
    if (slru->lru_head == slru->lru_tail) {
        slru->lru_head = UINT32_MAX;
        slru->lru_tail = UINT32_MAX;
    } else {
        if (i == slru->lru_head) {
            slru->lru_head = slru->hash_table_lru_links[i * 2 + 1];
            slru->hash_table_lru_links[slru->lru_head * 2 + 0] = UINT32_MAX;
            slru->hash_table_lru_links[i * 2 + 1] = UINT32_MAX;
        } else if (i == slru->lru_tail) {
            slru->lru_tail = slru->hash_table_lru_links[i * 2 + 0];
            slru->hash_table_lru_links[slru->lru_tail * 2 + 1] = UINT32_MAX;
            slru->hash_table_lru_links[i * 2 + 0] = UINT32_MAX;
        } else {
            uint32_t prev = slru->hash_table_lru_links[i * 2 + 0];
            uint32_t next = slru->hash_table_lru_links[i * 2 + 1];
            slru->hash_table_lru_links[prev * 2 + 0] = next;
            slru->hash_table_lru_links[next * 2 + 1] = prev;
            slru->hash_table_lru_links[i * 2 + 0] = UINT32_MAX;
            slru->hash_table_lru_links[i * 2 + 1] = UINT32_MAX;
        }
    }
}

static void slru_move_to_lru_head(slru_t *slru, uint32_t i) {
    if (slru->lru_head != slru->lru_tail) {
        if (i == slru->lru_tail) {
            slru->lru_tail = slru->hash_table_lru_links[i * 2 + 0];
            slru->hash_table_lru_links[i * 2 + 0] = UINT32_MAX;
            slru->hash_table_lru_links[slru->lru_tail * 2 + 1] = UINT32_MAX;
            slru->hash_table_lru_links[slru->lru_head * 2 + 0] = i;
            slru->hash_table_lru_links[i * 2 + 1] = slru->lru_head;
            slru->lru_head = i;
        } else if (i != slru->lru_head) {
            uint32_t prev = slru->hash_table_lru_links[i * 2 + 0];
            uint32_t next = slru->hash_table_lru_links[i * 2 + 1];
            slru->hash_table_lru_links[next * 2 + 0] = prev;
            slru->hash_table_lru_links[prev * 2 + 1] = next;
            slru->hash_table_lru_links[i * 2 + 0] = UINT32_MAX;
            slru->hash_table_lru_links[i * 2 + 1] = slru->lru_head;
            slru->hash_table_lru_links[slru->lru_head * 2 + 0] = i;
            slru->lru_head = i;
        }
    }
}

//

slru_t *slru_create(uint32_t hash_table_size, uint32_t num_initial_items,
    uint32_t cache_size, void *evict_user, slru_evict_func_t evict_func,
    slru_malloc_func_t malloc_func, slru_free_func_t free_func) {
    assert(hash_table_size != 0 && cache_size != 0);
    assert(slru_is_power_of_two(hash_table_size));
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

    size_t hash_table_lru_links_bytesize =
        sizeof(*slru->hash_table_lru_links) * hash_table_size * 2;
    slru->hash_table_lru_links = slru->malloc_func(hash_table_lru_links_bytesize);
    if (!slru->hash_table_lru_links) {
        slru_destroy(slru);
        return NULL;
    }

    memset(slru->hash_table_lru_links, 0xff, hash_table_lru_links_bytesize);

    slru->hash_table_size = hash_table_size;
    slru->lru_head = UINT32_MAX;
    slru->lru_tail = UINT32_MAX;

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
    slru->free_func(slru->hash_table_lru_links);
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
    memset(slru->hash_table_lru_links, 0xff, sizeof(*slru->hash_table_lru_links) * slru->hash_table_size * 2);
    memset(slru->items, 0, sizeof(*slru->items) * slru->num_items);

    for (uint32_t i = 0; i < slru->num_items - 1; ++i)
        slru->items[i].next = i + 1;

    slru->items[slru->num_items - 1].next = UINT32_MAX;
    slru->first_free = 0;

    slru->num_items_in_use = 0;
}

int slru_insert(slru_t *slru, const void *key, uint16_t key_length,
    uint32_t value, uint16_t consumption) {
    slru_check_internal_state(slru);
    assert(key && key_length != 0);
    assert(consumption != 0);

    //

    while (slru->cache_left < consumption) {
        if (slru_evict_oldest(slru) != SLRU_OK)
            break;
    }

    if (slru->cache_left < consumption)
        return SLRU_DOESNT_FIT;

    slru->cache_left -= consumption;

    //

    uint32_t hash = slru_hash(key, key_length, slru->seed,
        slru->hash_table_size);
    assert(hash < slru->hash_table_size);
    assert(slru_find_index(slru, key, key_length, hash) == UINT32_MAX);

    if (slru->first_free == UINT32_MAX) {
        if (slru->num_items == 0) {
            uint32_t num_items = slru->hash_table_size;
            assert(slru_is_power_of_two(num_items));

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

    assert(item->consumption == 0);

    item->key = slru->malloc_func(key_length);
    if (!item->key)
        return SLRU_OOM;

    memcpy(item->key, key, key_length);
    item->key_length = key_length;

    item->value = value;
    item->consumption = consumption;

    if (slru->hash_table[hash] == UINT32_MAX) {
        // Hash table row not in LRU list yet
        assert(slru->hash_table_lru_links[hash * 2 + 0] == UINT32_MAX);
        assert(slru->hash_table_lru_links[hash * 2 + 1] == UINT32_MAX);
        slru_insert_to_lru_head(slru, hash);
    } else {
        slru_move_to_lru_head(slru, hash);
    }

    // Update links
    slru->first_free = item->next;
    item->next = slru->hash_table[hash];
    slru->hash_table[hash] = index;

    slru->num_items_in_use++;

    return SLRU_OK;
}

int slru_remove(slru_t *slru, const void *key, uint16_t key_length) {
    assert(key != NULL && key_length != 0 && key_length <= UINT16_MAX);
    slru_check_internal_state(slru);
    uint32_t hash = slru_hash(key, key_length, slru->seed,
        slru->hash_table_size);
    uint32_t index = slru_find_index(slru, key, key_length, hash);
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
        if (slru->hash_table[hash] == UINT32_MAX) {
            // Hash table row is empty
            slru_remove_from_lru(slru, hash);
        }
    } else {
        assert(slru->items[prev_index].next == index);
        slru->items[prev_index].next = item->next;
    }

    item->next = slru->first_free;
    slru->first_free = index;

    slru->free_func(item->key);
    item->key = NULL;

    slru->cache_left += item->consumption;
    item->consumption = 0; // Important to zero

    slru->num_items_in_use--;

    return SLRU_OK;
}

uint32_t slru_fetch(slru_t *slru, const void *key, uint16_t key_length,
    uint32_t invalid_value) {
    assert(key != NULL && key_length != 0);
    slru_check_internal_state(slru);

    uint32_t hash = slru_hash(key, key_length, slru->seed,
        slru->hash_table_size);
    uint32_t index = slru_find_index(slru, key, key_length, hash);
    if (index == UINT32_MAX)
        return invalid_value;

    slru_move_to_lru_head(slru, hash);

    assert(index < slru->num_items);
    slru_item_t *item = &slru->items[index];
    return item->value;
}

//
// c-string key helper functions

int slru_insert_strkey(slru_t *slru, const char *key, uint32_t value,
    uint16_t consumption) {
    assert(key != NULL && strlen(key) <= UINT16_MAX);
    return slru_insert(slru, key, (uint16_t)strlen(key), value, consumption);
}

int slru_remove_strkey(slru_t *slru, const char *key) {
    assert(key != NULL && strlen(key) <= UINT16_MAX);
    return slru_remove(slru, key, (uint16_t)strlen(key));
}

uint32_t slru_fetch_strkey(slru_t *slru, const char *key,
    uint32_t invalid_value) {
    assert(key != NULL && strlen(key) <= UINT16_MAX);
    return slru_fetch(slru, key, (uint16_t)strlen(key), invalid_value);
}
