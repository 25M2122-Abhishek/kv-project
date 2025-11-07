#include "key_registry.h"
#include <stdlib.h>
#include <string.h>

void keys_init(key_registry_t *kr, size_t capacity) {
    kr->keys = calloc(capacity, sizeof(char *));
    kr->capacity = capacity;
    kr->count = 0;
    pthread_mutex_init(&kr->lock, NULL);
}

void keys_destroy(key_registry_t *kr) {
    pthread_mutex_lock(&kr->lock);
    for (size_t i = 0; i < kr->count; i++) {
        free(kr->keys[i]);
    }
    free(kr->keys);
    kr->keys = NULL;
    kr->capacity = 0;
    kr->count = 0;
    pthread_mutex_unlock(&kr->lock);
    pthread_mutex_destroy(&kr->lock);
}

int keys_try_add(key_registry_t *kr, const char *key) {
    pthread_mutex_lock(&kr->lock);
    if (kr->count >= kr->capacity) {
        pthread_mutex_unlock(&kr->lock);
        return 0;
    }
    kr->keys[kr->count++] = strdup(key);
    pthread_mutex_unlock(&kr->lock);
    return 1;
}

int keys_get_random(key_registry_t *kr, char **out_key) {
    pthread_mutex_lock(&kr->lock);
    if (kr->count == 0) {
        pthread_mutex_unlock(&kr->lock);
        return 0;
    }
    size_t idx = rand() % kr->count;
    *out_key = kr->keys[idx];
    pthread_mutex_unlock(&kr->lock);
    return 1;
}

int keys_remove_random(key_registry_t *kr, char **removed_key) {
    pthread_mutex_lock(&kr->lock);
    if (kr->count == 0) {
        pthread_mutex_unlock(&kr->lock);
        return 0;
    }
    size_t idx = rand() % kr->count;
    *removed_key = kr->keys[idx];
    kr->keys[idx] = kr->keys[--kr->count];
    pthread_mutex_unlock(&kr->lock);
    return 1;
}

size_t keys_count(key_registry_t *kr) {
    pthread_mutex_lock(&kr->lock);
    size_t c = kr->count;
    pthread_mutex_unlock(&kr->lock);
    return c;
}
