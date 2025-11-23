#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <curl/curl.h>
#include <unistd.h>
#include <time.h>

#include "loadgen.h"
#include "key_registry.h"

void *worker_func(void *arg);
static void usage(const char *prog);

config_t g_cfg = {
    .server_url = "http://kv_server:8080/kv",
    .threads = 4,
    .duration = 20,
    .mix_get = 60,
    .mix_post = 30,
    .mix_delete = 10,
    .key_prefix = "key",
    .workload = WL_MIX,
    .key_pool_size = 100000,
    .popular_size = 100
};
metrics_t g_metrics;
volatile int stop_flag = 0;

/* Global key registry instance (used by worker.c) */
key_registry_t g_keys;

static size_t discard_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    return size * nmemb;
}

static int do_post_once(CURL *curl, const char *key, const char *value) {
    char json[512];
    snprintf(json, sizeof(json), "{\"key\":\"%s\",\"value\":\"%s\"}", key, value);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, g_cfg.server_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    CURLcode res = curl_easy_perform(curl);
    long rc = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &rc);
    curl_slist_free_all(hdrs);
    curl_easy_setopt(curl, CURLOPT_POST, 0L);
    return (res == CURLE_OK && rc == 200) ? 1 : 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--server") == 0 && i+1 < argc) {
            strncpy(g_cfg.server_url, argv[++i], sizeof(g_cfg.server_url)-1);
        } else if (strcmp(argv[i], "--threads") == 0 && i+1 < argc) {
            g_cfg.threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i+1 < argc) {
            g_cfg.duration = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mix") == 0 && i+1 < argc) {
            int g,p,d;
            if (sscanf(argv[++i], "%d,%d,%d", &g, &p, &d) == 3) {
                g_cfg.mix_get = g; g_cfg.mix_post = p; g_cfg.mix_delete = d;
            } else { usage(argv[0]); return 1; }
        } else if (strcmp(argv[i], "--key-prefix") == 0 && i+1 < argc) {
            strncpy(g_cfg.key_prefix, argv[++i], sizeof(g_cfg.key_prefix)-1);
        } else if (strcmp(argv[i], "--workload") == 0 && i+1 < argc) {
            const char *w = argv[++i];
            if (strcmp(w, "put-all") == 0) g_cfg.workload = WL_PUT_ALL;
            else if (strcmp(w, "get-all") == 0) g_cfg.workload = WL_GET_ALL;
            else if (strcmp(w, "get-popular") == 0) g_cfg.workload = WL_GET_POPULAR;
            else if (strcmp(w, "mix") == 0) g_cfg.workload = WL_MIX;
            else { fprintf(stderr, "Unknown workload '%s'\n", w); return 1; }
        } else if (strcmp(argv[i], "--key-pool-size") == 0 && i+1 < argc) {
            g_cfg.key_pool_size = (size_t)atol(argv[++i]);
        } else if (strcmp(argv[i], "--popular-size") == 0 && i+1 < argc) {
            g_cfg.popular_size = (size_t)atol(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (g_cfg.mix_get + g_cfg.mix_post + g_cfg.mix_delete != 100 && g_cfg.workload == WL_MIX) {
        fprintf(stderr, "Mix must sum to 100 (got %d+%d+%d)\n", g_cfg.mix_get, g_cfg.mix_post, g_cfg.mix_delete);
        return 1;
    }

    printf("LoadGen in progress...\n");
    metrics_init(&g_metrics);

    /* initialize key registry with configured capacity */
    keys_init(&g_keys, g_cfg.key_pool_size);

    curl_global_init(CURL_GLOBAL_ALL);

    /* Seed popular keys if requested */
    if (g_cfg.workload == WL_GET_POPULAR) {
        CURL *curl = curl_easy_init();
        if (!curl) { fprintf(stderr, "curl init failed for seeding\n"); return 1; }
        for (size_t i = 0; i < g_cfg.popular_size; ++i) {
            char key[128], val[128];
            snprintf(key, sizeof(key), "%s_pop_%zu", g_cfg.key_prefix, i);
            snprintf(val, sizeof(val), "v_pop_%zu", i);
            if (do_post_once(curl, key, val)) {
                /* add to registry; ignore failure if pool full */
                keys_try_add(&g_keys, key);
            } else {
                fprintf(stderr, "Warning: seeding key %s failed\n", key);
            }
        }
        curl_easy_cleanup(curl);
        printf("Seeded %zu popular keys (pool size now %zu)\n", g_cfg.popular_size, (size_t)keys_count(&g_keys));
    }

    pthread_t *tids = calloc(g_cfg.threads, sizeof(pthread_t));
    if (!tids) { perror("calloc"); return 1; }

    for (int i = 0; i < g_cfg.threads; ++i) {
        int rc = pthread_create(&tids[i], NULL, worker_func, (void *)(intptr_t)i);
        if (rc != 0) { perror("pthread_create"); return 1; }
    }

    sleep(g_cfg.duration);
    stop_flag = 1;

    for (int i = 0; i < g_cfg.threads; ++i) pthread_join(tids[i], NULL);

    uint64_t total_req = 0, total_success = 0, total_failure = 0;
    printf("\n=== LoadGen Summary ===\n");
    printf("Threads: %d\n", g_cfg.threads);
    printf("Duration: %d s\n", g_cfg.duration);
    for (int op=0; op<3; ++op) {
        op_stats_t *s = &g_metrics.stats[op];
        total_req += s->count;
        total_success += s->success;
        total_failure += s->failure;
    }
    double throughput = (double)total_success / (double)g_cfg.duration;
    printf("Total Requests: %lu\n", (unsigned long)total_req);
    printf("Success: %lu, Failure: %lu\n", (unsigned long)total_success, (unsigned long)total_failure);
    printf("Throughput (req/s): %.2f\n", throughput);

    const char *names[3] = {"GET", "POST", "DELETE"};
    for (int op=0; op<3; ++op) {
        op_stats_t *s = &g_metrics.stats[op];
        double avg_ms = s->success > 0 ? (double)s->total_ns / s->success / 1e6 : 0.0;
        printf("%s: attempts=%lu success=%lu fail=%lu avg_latency_ms=%.3f\n",
               names[op], (unsigned long)s->count, (unsigned long)s->success, (unsigned long)s->failure, avg_ms);
    }

    free(tids);
    keys_destroy(&g_keys);
    metrics_destroy(&g_metrics);
    curl_global_cleanup();
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [--server url] [--threads N] [--duration S] [--mix GET,POST,DELETE]\n"
        "       [--key-prefix prefix] [--workload put-all|get-all|get-popular|mix]\n"
        "       [--key-pool-size N] [--popular-size N]\n"
        "Defaults: server=http://kv_server:8080/kv threads=4 duration=20 mix=60,30,10 key-prefix=key\n",
        prog);
}
