#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

// typedef struct {
//     char *key;
//     char *value;
// } kv_item_t;

typedef struct lru_cache lru_cache_t;

/* Create/destroy cache */
lru_cache_t *lru_cache_create(size_t capacity);
void lru_cache_destroy(lru_cache_t *cache);

/* Thread-safe operations:
 * Returns 0 on success and fills *out_value (caller frees)
 * Returns -1 if not found
 */
int lru_cache_get(lru_cache_t *cache, const char *key, char **out_value);
int lru_cache_put(lru_cache_t *cache, const char *key, const char *value);
int lru_cache_delete(lru_cache_t *cache, const char *key);

#endif /* CACHE_H */
