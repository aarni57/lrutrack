// Least-recently-used tracking helper in C
// Author: Aarni Gratseff (aarni.gratseff@gmail.com)
// Created (yyyy-mm-dd): 2025-03-10

#include "lrutrack.h"

#include <string.h>
#include <assert.h>

#if !defined(NDEBUG)
#   define LRUTRACK_ONLY_IN_DEBUG(x) x
#else
#   define LRUTRACK_ONLY_IN_DEBUG(x)
#endif

static int lrutrack_is_power_of_two(uint32_t x) {
    return x > 0 && (x & (x - 1)) == 0;
}

static uint32_t lrutrack_hash(const void *key, uint32_t len, uint32_t seed,
    uint32_t hash_table_size) {
    assert(lrutrack_is_power_of_two(hash_table_size));

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

static int lrutrack_cmp_keys(const void *a, uint32_t a_length,
    const void *b, uint32_t b_length) {
    if (a_length != b_length) return 0;
    return memcmp(a, b, a_length) == 0 ? 1 : 0;
}

//

typedef struct lrutrack_item_t {
    void *key;
    uint32_t key_length;
    lrutrack_value_t value;
    uint32_t next; // Next item index (hash table row or free list)
} lrutrack_item_t;

typedef struct lrutrack_t {
    void *evict_user;
    lrutrack_evict_func_t evict_func;
    lrutrack_malloc_func_t malloc_func;
    lrutrack_free_func_t free_func;
    uint32_t *hash_table; // First item index on a row
    uint32_t *hash_table_lru_links; // 2 * hash_table_size, 0 = prev, 1 = next
    lrutrack_item_t *items;
    uint32_t num_items;
    uint32_t hash_table_size;
    uint32_t lru_head; // Hash table index
    uint32_t lru_tail;
    uint32_t first_free; // Item index
    uint32_t seed;
    lrutrack_value_t invalid_value;
} lrutrack_t;

//
// Private functions

static void lrutrack_check_internal_state(const lrutrack_t *t) {
    assert(t);
    assert(t->malloc_func);
    assert(t->free_func);
    assert(t->hash_table_size != 0);

    assert(t->first_free == UINT32_MAX ||
        t->first_free < t->num_items);

    assert(t->lru_head == UINT32_MAX ||
        t->lru_head < t->hash_table_size);
    assert(t->lru_tail == UINT32_MAX ||
        t->lru_tail < t->hash_table_size);
    assert(t->lru_head == UINT32_MAX ||
        t->hash_table_lru_links[t->lru_head * 2 + 0] == UINT32_MAX);
    assert(t->lru_tail == UINT32_MAX ||
        t->hash_table_lru_links[t->lru_tail * 2 + 1] == UINT32_MAX);

#if LRUTRACK_HC_TESTS
    uint32_t prev_iter = UINT32_MAX;
    uint32_t iter = t->lru_head;
    while (iter != UINT32_MAX) {
        assert(iter < t->hash_table_size);
        assert(t->hash_table_lru_links[iter * 2 + 0] == prev_iter);
        prev_iter = iter;
        iter = t->hash_table_lru_links[iter * 2 + 1];
    }

    assert(prev_iter == t->lru_tail);

    for (uint32_t i = 0; i < t->hash_table_size; ++i) {
        assert(t->hash_table_lru_links[i * 2 + 0] != i);
        assert(t->hash_table_lru_links[i * 2 + 1] != i);
        if (t->hash_table[i] == UINT32_MAX) {
            assert(t->hash_table_lru_links[i * 2 + 0] == UINT32_MAX);
            assert(t->hash_table_lru_links[i * 2 + 1] == UINT32_MAX);
        }
    }

    for (uint32_t i = 0; i < t->hash_table_size; ++i) {
        assert(t->hash_table[i] == UINT32_MAX ||
            t->hash_table[i] < t->num_items);

        uint32_t iter = 0;
        while (iter != UINT32_MAX) {
            const lrutrack_item_t *item = &t->items[iter];

            iter = item->next;
        }
    }
#endif
}

static uint32_t lrutrack_find_index(const lrutrack_t *t, const void *key,
    uint32_t key_length, uint32_t hash) {
    assert(key != NULL && key_length != 0);
    assert(hash < t->hash_table_size);
    assert(hash == lrutrack_hash(key, key_length, t->seed,
        t->hash_table_size));
    uint32_t iter = t->hash_table[hash];
    assert(iter == UINT32_MAX || iter < t->num_items);
    while (iter != UINT32_MAX &&
        !lrutrack_cmp_keys(key, key_length,
            t->items[iter].key, t->items[iter].key_length)) {
        iter = t->items[iter].next;
        assert(iter == UINT32_MAX || iter < t->num_items);
    }
    return iter;
}

static void lrutrack_insert_to_lru_head(lrutrack_t *t, uint32_t i) {
    if (t->lru_head != UINT32_MAX) {
        t->hash_table_lru_links[t->lru_head * 2 + 0] = i;
        t->hash_table_lru_links[i * 2 + 1] = t->lru_head;
        t->lru_head = i;
    } else {
        t->lru_head = i;
        t->lru_tail = i;
    }
}

static void lrutrack_remove_from_lru(lrutrack_t *t, uint32_t i) {
    if (t->lru_head == t->lru_tail) {
        t->lru_head = UINT32_MAX;
        t->lru_tail = UINT32_MAX;
    } else {
        if (i == t->lru_head) {
            t->lru_head = t->hash_table_lru_links[i * 2 + 1];
            t->hash_table_lru_links[t->lru_head * 2 + 0] = UINT32_MAX;
            t->hash_table_lru_links[i * 2 + 1] = UINT32_MAX;
        } else if (i == t->lru_tail) {
            t->lru_tail = t->hash_table_lru_links[i * 2 + 0];
            t->hash_table_lru_links[t->lru_tail * 2 + 1] = UINT32_MAX;
            t->hash_table_lru_links[i * 2 + 0] = UINT32_MAX;
        } else {
            uint32_t prev = t->hash_table_lru_links[i * 2 + 0];
            uint32_t next = t->hash_table_lru_links[i * 2 + 1];
            t->hash_table_lru_links[next * 2 + 0] = prev;
            t->hash_table_lru_links[prev * 2 + 1] = next;
            t->hash_table_lru_links[i * 2 + 0] = UINT32_MAX;
            t->hash_table_lru_links[i * 2 + 1] = UINT32_MAX;
        }
    }
}

static void lrutrack_move_to_lru_head(lrutrack_t *t, uint32_t i) {
    if (t->lru_head != t->lru_tail) {
        if (i == t->lru_tail) {
            t->lru_tail = t->hash_table_lru_links[i * 2 + 0];
            t->hash_table_lru_links[i * 2 + 0] = UINT32_MAX;
            t->hash_table_lru_links[t->lru_tail * 2 + 1] = UINT32_MAX;
            t->hash_table_lru_links[t->lru_head * 2 + 0] = i;
            t->hash_table_lru_links[i * 2 + 1] = t->lru_head;
            t->lru_head = i;
        } else if (i != t->lru_head) {
            uint32_t prev = t->hash_table_lru_links[i * 2 + 0];
            uint32_t next = t->hash_table_lru_links[i * 2 + 1];
            t->hash_table_lru_links[next * 2 + 0] = prev;
            t->hash_table_lru_links[prev * 2 + 1] = next;
            t->hash_table_lru_links[i * 2 + 0] = UINT32_MAX;
            t->hash_table_lru_links[i * 2 + 1] = t->lru_head;
            t->hash_table_lru_links[t->lru_head * 2 + 0] = i;
            t->lru_head = i;
        }
    }
}

//
// Public functions

lrutrack_t *lrutrack_create(uint32_t hash_table_size,
    uint32_t num_initial_items, uint32_t hash_seed,
    lrutrack_value_t invalid_value,
    void *evict_user, lrutrack_evict_func_t evict_func,
    lrutrack_malloc_func_t malloc_func, lrutrack_free_func_t free_func) {
    assert(hash_table_size != 0);
    assert(lrutrack_is_power_of_two(hash_table_size));
    assert(evict_func && malloc_func && free_func);

    lrutrack_t *t = malloc_func(sizeof(lrutrack_t));
    if (!t)
        return NULL;

    memset(t, 0, sizeof(*t));

    t->evict_user = evict_user;
    t->evict_func = evict_func;

    t->malloc_func = malloc_func;
    t->free_func = free_func;

    t->seed = hash_seed;
    t->invalid_value = invalid_value;

    size_t hash_table_bytesize = sizeof(*t->hash_table) * hash_table_size;
    t->hash_table = t->malloc_func(hash_table_bytesize);
    if (!t->hash_table) {
        lrutrack_destroy(t);
        return NULL;
    }

    memset(t->hash_table, 0xff, hash_table_bytesize);

    size_t hash_table_lru_links_bytesize =
        sizeof(*t->hash_table_lru_links) * hash_table_size * 2;
    t->hash_table_lru_links = t->malloc_func(hash_table_lru_links_bytesize);
    if (!t->hash_table_lru_links) {
        lrutrack_destroy(t);
        return NULL;
    }

    memset(t->hash_table_lru_links, 0xff, hash_table_lru_links_bytesize);

    t->hash_table_size = hash_table_size;
    t->lru_head = UINT32_MAX;
    t->lru_tail = UINT32_MAX;

    if (num_initial_items != 0) {
        uint32_t items_bytesize = sizeof(*t->items) * num_initial_items;
        t->items = t->malloc_func(items_bytesize);
        if (!t->items) {
            lrutrack_destroy(t);
            return NULL;
        }

        memset(t->items, 0, items_bytesize);

        for (uint32_t i = 0; i < num_initial_items - 1; ++i) {
            t->items[i].value = t->invalid_value;
            t->items[i].next = i + 1;
        }

        t->items[num_initial_items - 1].value = t->invalid_value;
        t->items[num_initial_items - 1].next = UINT32_MAX;

        t->num_items = num_initial_items;
        t->first_free = 0;
    } else {
        t->first_free = UINT32_MAX;
    }

    lrutrack_check_internal_state(t);

    return t;
}

void lrutrack_destroy(lrutrack_t *t) {
    lrutrack_check_internal_state(t);

    for (uint32_t i = 0; i < t->num_items; ++i) {
        lrutrack_item_t *item = &t->items[i];
        if (item->value != t->invalid_value) {
            t->free_func(t->items[i].key);
            t->evict_func(t->evict_user, item->value);
        } else {
            assert(item->key == NULL);
        }
    }

    t->free_func(t->items);
    t->free_func(t->hash_table_lru_links);
    t->free_func(t->hash_table);
    t->free_func(t);
}

void lrutrack_remove_all(lrutrack_t *t) {
    lrutrack_check_internal_state(t);

    for (uint32_t i = 0; i < t->hash_table_size; ++i) {
        uint32_t iter = t->hash_table[i];
        while (iter != UINT32_MAX) {
            assert(iter < t->num_items);
            lrutrack_item_t *item = &t->items[iter];
            assert(item->value != t->invalid_value);

            assert(t->evict_func);
            t->evict_func(t->evict_user, item->value);

            t->free_func(item->key);
            item->key = NULL;

            item->value = t->invalid_value;

            iter = item->next;
        }
    }

    memset(t->hash_table, 0xff, sizeof(*t->hash_table) * t->hash_table_size);
    memset(t->hash_table_lru_links, 0xff, sizeof(*t->hash_table_lru_links) * t->hash_table_size * 2);

    if (t->num_items != 0) {
        for (uint32_t i = 0; i < t->num_items - 1; ++i)
            t->items[i].next = i + 1;

        t->items[t->num_items - 1].next = UINT32_MAX;
    }

    t->lru_head = UINT32_MAX;
    t->lru_tail = UINT32_MAX;

    t->first_free = 0;

    lrutrack_check_internal_state(t);
}

int lrutrack_insert(lrutrack_t *t, const void *key, uint32_t key_length,
    lrutrack_value_t value) {
    lrutrack_check_internal_state(t);
    assert(key && key_length != 0);
    assert(value != t->invalid_value);

    uint32_t hash = lrutrack_hash(key, key_length, t->seed,
        t->hash_table_size);
    assert(hash < t->hash_table_size);
    assert(lrutrack_find_index(t, key, key_length, hash) == UINT32_MAX);

    if (t->first_free == UINT32_MAX) {
        if (t->num_items == 0) {
            uint32_t num_items = t->hash_table_size;
            assert(lrutrack_is_power_of_two(num_items));

            t->items = t->malloc_func(sizeof(*t->items) *
                num_items);
            if (!t->items)
                return LRUTRACK_OOM;

            t->num_items = num_items;

            for (uint32_t i = 0; i < t->num_items - 1; ++i)
                t->items[i].next = i + 1;

            t->items[t->num_items - 1].next = UINT32_MAX;
            t->first_free = 0;
        } else {
            lrutrack_item_t *old_items = t->items;
            uint32_t old_num_items = t->num_items;
            t->num_items *= 2;

            uint32_t items_bytesize = sizeof(*t->items) * t->num_items;
            t->items = t->malloc_func(items_bytesize);
            if (!t->items)
                return LRUTRACK_OOM;

            uint32_t old_items_bytesize = sizeof(*t->items) * old_num_items;
            memcpy(t->items, old_items, old_items_bytesize);
            t->free_func(old_items);

            memset(t->items + old_num_items, 0,
                items_bytesize - old_items_bytesize);

            for (uint32_t i = old_num_items; i < t->num_items - 1; ++i) {
                t->items[i].value = t->invalid_value;
                t->items[i].next = i + 1;
            }

            t->items[t->num_items - 1].value = t->invalid_value;
            t->items[t->num_items - 1].next = UINT32_MAX;

            t->first_free = old_num_items;
        }
    }

    uint32_t index = t->first_free; // Take first free
    assert(index < t->num_items);
    lrutrack_item_t *item = &t->items[index];

    assert(item->value == t->invalid_value);

    item->key = t->malloc_func(key_length);
    if (!item->key)
        return LRUTRACK_OOM;

    memcpy(item->key, key, key_length);
    item->key_length = key_length;

    item->value = value;

    if (t->hash_table[hash] == UINT32_MAX) {
        // Hash table row not in LRU list yet
        assert(t->hash_table_lru_links[hash * 2 + 0] == UINT32_MAX);
        assert(t->hash_table_lru_links[hash * 2 + 1] == UINT32_MAX);
        lrutrack_insert_to_lru_head(t, hash);
    } else {
        lrutrack_move_to_lru_head(t, hash);
    }

    // Update links
    t->first_free = item->next;
    item->next = t->hash_table[hash];
    t->hash_table[hash] = index;

    lrutrack_check_internal_state(t);

    return LRUTRACK_OK;
}

int lrutrack_remove(lrutrack_t *t, const void *key, uint32_t key_length) {
    assert(key != NULL && key_length != 0);
    lrutrack_check_internal_state(t);
    uint32_t hash = lrutrack_hash(key, key_length, t->seed,
        t->hash_table_size);
    uint32_t index = lrutrack_find_index(t, key, key_length, hash);
    if (index == UINT32_MAX)
        return LRUTRACK_NOT_FOUND;

    assert(index < t->num_items);
    lrutrack_item_t *item = &t->items[index];
    assert(item->value != t->invalid_value);

    assert(t->evict_func);
    t->evict_func(t->evict_user, item->value);

    uint32_t prev_index = UINT32_MAX;
    uint32_t iter = t->hash_table[hash];
    while (iter != UINT32_MAX) {
        if (iter == index)
            break;
        prev_index = iter;
        iter = t->items[iter].next;
    }

    if (prev_index == UINT32_MAX) {
        assert(t->hash_table[hash] == index);
        t->hash_table[hash] = item->next;
        if (t->hash_table[hash] == UINT32_MAX) {
            // Hash table row is empty
            lrutrack_remove_from_lru(t, hash);
        }
    } else {
        assert(t->items[prev_index].next == index);
        t->items[prev_index].next = item->next;
    }

    item->next = t->first_free;
    t->first_free = index;

    t->free_func(item->key);
    item->key = NULL;

    item->value = t->invalid_value;

    lrutrack_check_internal_state(t);

    return LRUTRACK_OK;
}

int lrutrack_remove_lru(lrutrack_t *t) {
    lrutrack_check_internal_state(t);

    if (t->lru_tail == UINT32_MAX) {
        assert(t->lru_head == UINT32_MAX);
        return LRUTRACK_NOT_FOUND;
    }

    uint32_t new_tail = t->hash_table_lru_links[t->lru_tail * 2 + 0];
    t->hash_table_lru_links[t->lru_tail * 2 + 0] = UINT32_MAX;
    assert(t->hash_table_lru_links[t->lru_tail * 2 + 1] == UINT32_MAX);

    if (new_tail != UINT32_MAX)
        t->hash_table_lru_links[new_tail * 2 + 1] = UINT32_MAX;

    uint32_t iter = t->hash_table[t->lru_tail];
    t->hash_table[t->lru_tail] = UINT32_MAX;

    if (t->lru_head == t->lru_tail)
        t->lru_head = new_tail;
    t->lru_tail = new_tail;

    while (iter != UINT32_MAX) {
        assert(iter < t->num_items);
        lrutrack_item_t *item = &t->items[iter];
        assert(item->value != t->invalid_value);

        t->free_func(item->key);
        item->key = NULL;

        assert(t->evict_func);
        t->evict_func(t->evict_user, item->value);

        item->value = t->invalid_value;

        uint32_t next = item->next;
        item->next = t->first_free;

        t->first_free = iter;
        iter = next;
    }

    lrutrack_check_internal_state(t);

    return LRUTRACK_OK;
}

lrutrack_value_t lrutrack_use(lrutrack_t *t, const void *key,
    uint32_t key_length) {
    assert(key != NULL && key_length != 0);
    lrutrack_check_internal_state(t);

    uint32_t hash = lrutrack_hash(key, key_length, t->seed,
        t->hash_table_size);
    uint32_t index = lrutrack_find_index(t, key, key_length, hash);
    if (index == UINT32_MAX)
        return t->invalid_value;

    lrutrack_move_to_lru_head(t, hash);

    assert(index < t->num_items);
    lrutrack_item_t *item = &t->items[index];
    return item->value;
}

//
// c-string key helper functions

int lrutrack_insert_strkey(lrutrack_t *t, const char *key,
    lrutrack_value_t value) {
    assert(key != NULL && strlen(key) <= UINT32_MAX);
    return lrutrack_insert(t, key, (uint32_t)strlen(key), value);
}

int lrutrack_remove_strkey(lrutrack_t *t, const char *key) {
    assert(key != NULL && strlen(key) <= UINT32_MAX);
    return lrutrack_remove(t, key, (uint32_t)strlen(key));
}

lrutrack_value_t lrutrack_use_strkey(lrutrack_t *t, const char *key) {
    assert(key != NULL && strlen(key) <= UINT32_MAX);
    return lrutrack_use(t, key, (uint32_t)strlen(key));
}
