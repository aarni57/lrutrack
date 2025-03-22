// Least-recently-used tracking helper in C
// Author: Aarni Gratseff (aarni.gratseff@gmail.com)
// Created (yyyy-mm-dd): 2025-03-10

#ifndef LRUTRACK_H
#define LRUTRACK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(LRUTRACK_HC_TESTS)
#   define LRUTRACK_HC_TESTS 0
#endif

#define LRUTRACK_OK 0
#define LRUTRACK_ERROR 1
#define LRUTRACK_OOM 2
#define LRUTRACK_NOT_FOUND 3

typedef uint32_t lrutrack_value_t;

typedef void (*lrutrack_evict_func_t)(void *user, uint32_t value);

typedef void *(*lrutrack_malloc_func_t)(size_t num_bytes);
typedef void (*lrutrack_free_func_t)(void *ptr);

typedef struct lrutrack_t lrutrack_t;

lrutrack_t *lrutrack_create(uint32_t hash_table_size,
    uint32_t num_initial_items, uint32_t hash_seed,
    lrutrack_value_t invalid_value,
    void *evict_user, lrutrack_evict_func_t evict_func,
    lrutrack_malloc_func_t malloc_func, lrutrack_free_func_t free_func);
void lrutrack_destroy(lrutrack_t *t);

void lrutrack_remove_all(lrutrack_t *t);

int lrutrack_insert(lrutrack_t *t, const void *key, uint32_t key_length,
    lrutrack_value_t value);

int lrutrack_remove(lrutrack_t *t, const void *key, uint32_t key_length);

int lrutrack_remove_lru(lrutrack_t *t);

lrutrack_value_t lrutrack_use(lrutrack_t *t, const void *key,
    uint32_t key_length);

//
// c-string key helper functions

int lrutrack_insert_strkey(lrutrack_t *t, const char *key,
    lrutrack_value_t value);

int lrutrack_remove_strkey(lrutrack_t *t, const char *key);

lrutrack_value_t lrutrack_use_strkey(lrutrack_t *t, const char *key);

#ifdef __cplusplus
}
#endif

#endif
