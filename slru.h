// Simple least-recently-used cache implementation in C
// Author: Aarni Gratseff (aarni.gratseff@gmail.com)
// Created (yyyy-mm-dd): 2025-03-10

#ifndef SLRU_H
#define SLRU_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(SLRU_HC_TESTS)
#   define SLRU_HC_TESTS 0
#endif

#define SLRU_OK 0
#define SLRU_ERROR 1
#define SLRU_OOM 2
#define SLRU_NOT_FOUND 3
#define SLRU_DOESNT_FIT 4

typedef void (*slru_evict_func_t)(void *user, uint32_t value);

typedef void *(*slru_malloc_func_t)(size_t num_bytes);
typedef void (*slru_free_func_t)(void *ptr);

typedef struct slru_t slru_t;

slru_t *slru_create(uint32_t hash_table_size, uint32_t num_initial_items,
    uint32_t cache_size, void *evict_user, slru_evict_func_t evict_func,
    slru_malloc_func_t malloc_func, slru_free_func_t free_func);
void slru_destroy(slru_t *slru);

void slru_remove_all(slru_t *slru);

int slru_insert(slru_t *slru, const void *key, uint16_t key_length,
    uint32_t value, uint16_t consumption);
int slru_remove(slru_t *slru, const void *key, uint16_t key_length);

uint32_t slru_fetch(slru_t *slru, const void *key, uint16_t key_length,
    uint32_t invalid_value);

//
// c-string key helper functions

int slru_insert_strkey(slru_t *slru, const char *key, uint32_t value,
    uint16_t consumption);
int slru_remove_strkey(slru_t *slru, const char *key);

uint32_t slru_fetch_strkey(slru_t *slru, const char *key,
    uint32_t invalid_value);

#ifdef __cplusplus
}
#endif

#endif
