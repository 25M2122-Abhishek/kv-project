#ifndef KEY_REGISTRY_H
#define KEY_REGISTRY_H

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char **keys;           // array of key strings
    size_t capacity;       // max keys allowed
    size_t count;          // current stored keys
    pthread_mutex_t lock;  // mutex lock
} key_registry_t;

void keys_init(key_registry_t *kr, size_t capacity);
void keys_destroy(key_registry_t *kr);

int keys_try_add(key_registry_t *kr, const char *key);
int keys_get_random(key_registry_t *kr, char **out_key);
int keys_remove_random(key_registry_t *kr, char **removed_key);

size_t keys_count(key_registry_t *kr);

#endif
