#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>

#include "loadgen.h"
#include "key_registry.h"   /* use the global g_keys instance from main.c */

extern key_registry_t g_keys; /* ensure matches to expose it, or remove extern if static in main.c */

 /* choose operation based on weights */
static op_type_t choose_op(unsigned int rnd) {
    int g = g_cfg.mix_get;
    int p = g_cfg.mix_post;
    int cum1 = g;
    int cum2 = g + p;
    unsigned int val = rnd % 100;
    if ((int)val < cum1) return OP_GET;
    if ((int)val < cum2) return OP_POST;
    return OP_DELETE;
}

/* no-op write callback to discard response body */
static size_t discard_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr; (void)userdata;
    return size * nmemb;
}

/* helpers as before */
static void do_get(CURL *curl, const char *key, int *success, uint64_t *lat_ns) {
    char url[1024];
    snprintf(url, sizeof(url), "%s?key=%s", g_cfg.server_url, key);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CURLcode res = curl_easy_perform(curl);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    *lat_ns = timespec_diff_ns(&t1, &t2);
    long respcode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &respcode);
    /* treat 200 and 404 as OK for GET (404 means key absent) */
    *success = (res == CURLE_OK && (respcode == 200 || respcode == 404)) ? 1 : 0;
}

static void do_post(CURL *curl, const char *key, const char *value, int *success, uint64_t *lat_ns) {
    char json[1024];
    snprintf(json, sizeof(json), "{\"key\":\"%s\",\"value\":\"%s\"}", key, value);
    curl_easy_setopt(curl, CURLOPT_URL, g_cfg.server_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CURLcode res = curl_easy_perform(curl);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    *lat_ns = timespec_diff_ns(&t1, &t2);
    long respcode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &respcode);
    *success = (res == CURLE_OK && respcode == 200) ? 1 : 0;

    curl_slist_free_all(hdrs);
    curl_easy_setopt(curl, CURLOPT_POST, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
}

static void do_delete(CURL *curl, const char *key, int *success, uint64_t *lat_ns) {
    char url[1024];
    snprintf(url, sizeof(url), "%s?key=%s", g_cfg.server_url, key);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CURLcode res = curl_easy_perform(curl);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    *lat_ns = timespec_diff_ns(&t1, &t2);
    long respcode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &respcode);
    *success = (res == CURLE_OK && respcode == 200) ? 1 : 0;
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, NULL);
}

void *worker_func(void *arg) {
    int tid = (int)(intptr_t)arg;
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Thread %d: curl init failed\n", tid);
        return NULL;
    }
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)(tid * 7919);

    uint64_t seq = 0;
    while (!stop_flag) {
        seq++;
        op_type_t op = OP_GET;

        /* select op based on workload mode */
        if (g_cfg.workload == WL_PUT_ALL) {
            /* alternate or random between POST and DELETE */
            unsigned int v = rand_r(&seed) % 2;
            op = (v == 0) ? OP_POST : OP_DELETE;
        } else if (g_cfg.workload == WL_GET_ALL) {
            op = OP_GET;
        } else if (g_cfg.workload == WL_GET_POPULAR) {
            op = OP_GET;
        } else { /* WL_MIX */
            unsigned int rnd = rand_r(&seed) % 100;
            op = choose_op(rnd);
        }

        char keybuf[256];
        char value[128];
        int success = 0;
        uint64_t lat_ns = 0;

        if (op == OP_POST) {
            /* create a new key (unique) */
            snprintf(keybuf, sizeof(keybuf), "%s_thr%d_seq%lu", g_cfg.key_prefix, tid, (unsigned long)seq);
            snprintf(value, sizeof(value), "v_%d_%lu", tid, (unsigned long)seq);
            do_post(curl, keybuf, value, &success, &lat_ns);
            if (success) {
                /* add to key pool so other threads can GET/DELETE it */
                keys_try_add(&g_keys, keybuf);
            }
        } else if (op == OP_GET) {
            /* GET behavior depends */
            if (g_cfg.workload == WL_GET_ALL) {
                /* unique never-before-seen key to force DB read */
                snprintf(keybuf, sizeof(keybuf), "%s_unique_thr%d_%lu", g_cfg.key_prefix, tid, (unsigned long)seq);
                do_get(curl, keybuf, &success, &lat_ns);
            } else if (g_cfg.workload == WL_GET_POPULAR) {
                /* always pick from popular pool */
                char *kptr = NULL;
                if (!keys_get_random(&g_keys, &kptr)) {
                    /* If pool empty, fallback to unique GET */
                    snprintf(keybuf, sizeof(keybuf), "%s_unique_thr%d_%lu", g_cfg.key_prefix, tid, (unsigned long)seq);
                    do_get(curl, keybuf, &success, &lat_ns);
                } else {
                    /* keys_get_random returns pointer to internal storage: copy before using */
                    snprintf(keybuf, sizeof(keybuf), "%s", kptr);
                    do_get(curl, keybuf, &success, &lat_ns);
                }
            } else { /* WL_MIX or default */
                /* 50% chance to pick from pool if available */
                if (keys_count(&g_keys) > 0 && (rand_r(&seed) % 100) < 50) {
                    char *kptr = NULL;
                    if (!keys_get_random(&g_keys, &kptr)) {
                        snprintf(keybuf, sizeof(keybuf), "%s_unique_thr%d_%lu", g_cfg.key_prefix, tid, (unsigned long)seq);
                    } else {
                        snprintf(keybuf, sizeof(keybuf), "%s", kptr);
                    }
                } else {
                    snprintf(keybuf, sizeof(keybuf), "%s_unique_thr%d_%lu", g_cfg.key_prefix, tid, (unsigned long)seq);
                }
                do_get(curl, keybuf, &success, &lat_ns);
            }
        } else { /* DELETE */
            /* for delete we should remove existing keys when available */
            if (g_cfg.workload == WL_PUT_ALL) {
                /* In put-all we expect many creates; try to remove random key if exists */
                char *removed = NULL;
                if (!keys_remove_random(&g_keys, &removed)) {
                    /* no key to delete -> create one instead */
                    snprintf(keybuf, sizeof(keybuf), "%s_thr%d_seq%lu", g_cfg.key_prefix, tid, (unsigned long)seq);
                    snprintf(value, sizeof(value), "v_%d_%lu", tid, (unsigned long)seq);
                    do_post(curl, keybuf, value, &success, &lat_ns);
                    if (success) keys_try_add(&g_keys, keybuf);
                } else {
                    /* removed owns the string, use it and free after */
                    do_delete(curl, removed, &success, &lat_ns);
                    free(removed); /* we removed from registry; free memory */
                }
            } else if (g_cfg.workload == WL_MIX) {
                /* try remove existing key */
                char *removed = NULL;
                if (!keys_remove_random(&g_keys, &removed)) {
                    /* no key to delete -> do a POST */
                    snprintf(keybuf, sizeof(keybuf), "%s_thr%d_seq%lu", g_cfg.key_prefix, tid, (unsigned long)seq);
                    snprintf(value, sizeof(value), "v_%d_%lu", tid, (unsigned long)seq);
                    do_post(curl, keybuf, value, &success, &lat_ns);
                    if (success) keys_try_add(&g_keys, keybuf);
                } else {
                    do_delete(curl, removed, &success, &lat_ns);
                    free(removed);
                }
            } else {
                /* other modes: try to remove existing key, otherwise treat as no-op */
                char *removed = NULL;
                if (keys_remove_random(&g_keys, &removed)) {
                    do_delete(curl, removed, &success, &lat_ns);
                    free(removed);
                } else {
                    success = 0;
                    lat_ns = 0;
                }
            }
        }

        metrics_record(op, success, lat_ns);
    }

    curl_easy_cleanup(curl);
    return NULL;
}
