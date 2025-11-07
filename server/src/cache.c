#define _GNU_SOURCE
#include "cache.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>

/* Implementation: a hashmap (separate chaining) + doubly-linked list for LRU.
 * Thread-safe via a mutex.
 */

typedef struct node {
    char *key;
    char *value;
    struct node *prev, *next; /* for LRU list */
    struct node *hnext; /* for hash bucket chain */
} node_t;

struct lru_cache {
    size_t capacity;
    size_t size;
    node_t **buckets;
    size_t n_buckets;
    node_t *head; /* most recently used */
    node_t *tail; /* least recently used */
    pthread_mutex_t lock;
};

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = *s++)) h = ((h << 5) + h) + c;
    return h;
}

lru_cache_t *lru_cache_create(size_t capacity) {
    lru_cache_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->capacity = capacity;
    c->n_buckets = capacity * 2 + 1;
    c->buckets = calloc(c->n_buckets, sizeof(node_t*));
    if (!c->buckets) { free(c); return NULL; }
    pthread_mutex_init(&c->lock, NULL);
    c->head = c->tail = NULL;
    c->size = 0;
    return c;
}

static void detach_node(lru_cache_t *c, node_t *n) {
    if (!n) return;
    if (n->prev) n->prev->next = n->next;
    else c->head = n->next;
    if (n->next) n->next->prev = n->prev;
    else c->tail = n->prev;
    n->prev = n->next = NULL;
}

static void attach_head(lru_cache_t *c, node_t *n) {
    n->prev = NULL;
    n->next = c->head;
    if (c->head) c->head->prev = n;
    c->head = n;
    if (!c->tail) c->tail = n;
}

static void evict_if_needed(lru_cache_t *c) {
    if (c->size <= c->capacity) return;
    /* remove tail */
    node_t *to = c->tail;
    if (!to) return;
    /* remove from LRU list */
    detach_node(c, to);
    /* remove from hash */
    unsigned long h = hash_str(to->key) % c->n_buckets;
    node_t *cur = c->buckets[h], *prev = NULL;
    while (cur) {
        if (cur == to) {
            if (prev) prev->hnext = cur->hnext;
            else c->buckets[h] = cur->hnext;
            break;
        }
        prev = cur;
        cur = cur->hnext;
    }
    free(to->key);
    free(to->value);
    free(to);
    c->size--;
}

int lru_cache_put(lru_cache_t *c, const char *key, const char *value) {
    if (!c || !key || !value) return -1;
    pthread_mutex_lock(&c->lock);
    unsigned long h = hash_str(key) % c->n_buckets;
    node_t *cur = c->buckets[h];
    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            /* update value and move to head */
            free(cur->value);
            cur->value = strdup(value);
            detach_node(c, cur);
            attach_head(c, cur);
            pthread_mutex_unlock(&c->lock);
            return 0;
        }
        cur = cur->hnext;
    }
    /* new node */
    node_t *n = calloc(1, sizeof(*n));
    if (!n) { pthread_mutex_unlock(&c->lock); return -1; }
    n->key = strdup(key);
    n->value = strdup(value);
    n->hnext = c->buckets[h];
    c->buckets[h] = n;
    attach_head(c, n);
    c->size++;
    evict_if_needed(c);
    pthread_mutex_unlock(&c->lock);
    return 0;
}

int lru_cache_get(lru_cache_t *c, const char *key, char **out_value) {
    if (!c || !key || !out_value) return -1;
    pthread_mutex_lock(&c->lock);
    unsigned long h = hash_str(key) % c->n_buckets;
    node_t *cur = c->buckets[h];
    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            /* move to head */
            detach_node(c, cur);
            attach_head(c, cur);
            *out_value = strdup(cur->value);
            pthread_mutex_unlock(&c->lock);
            return 0;
        }
        cur = cur->hnext;
    }
    pthread_mutex_unlock(&c->lock);
    return -1;
}

int lru_cache_delete(lru_cache_t *c, const char *key) {
    if (!c || !key) return -1;
    pthread_mutex_lock(&c->lock);
    unsigned long h = hash_str(key) % c->n_buckets;
    node_t *cur = c->buckets[h], *prev = NULL;
    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            /* remove from hash chain */
            if (prev) prev->hnext = cur->hnext;
            else c->buckets[h] = cur->hnext;
            /* remove from LRU list */
            detach_node(c, cur);
            free(cur->key);
            free(cur->value);
            free(cur);
            c->size--;
            pthread_mutex_unlock(&c->lock);
            return 0;
        }
        prev = cur;
        cur = cur->hnext;
    }
    pthread_mutex_unlock(&c->lock);
    return -1;
}

void lru_cache_destroy(lru_cache_t *c) {
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    for (size_t i = 0; i < c->n_buckets; ++i) {
        node_t *cur = c->buckets[i];
        while (cur) {
            node_t *nx = cur->hnext;
            free(cur->key);
            free(cur->value);
            free(cur);
            cur = nx;
        }
    }
    free(c->buckets);
    pthread_mutex_unlock(&c->lock);
    pthread_mutex_destroy(&c->lock);
    free(c);
}
