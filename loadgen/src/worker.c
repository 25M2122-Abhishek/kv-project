#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

#include "loadgen.h"
#include "key_registry.h"   /* use the global g_keys instance from main.c */

extern key_registry_t g_keys; /* global from main.c */

/* --- Tunables --- */
#ifndef INTERNAL_CONCURRENCY
#define INTERNAL_CONCURRENCY 16    /* Option B: 16 easy handles per worker */
#endif

#ifndef QUEUE_CAP
#define QUEUE_CAP 1024             /* bounded job queue per top-level worker */
#endif

/* choose operation based on weights (same as before) */
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

/* convenience: current monotonic time in ns */
static inline uint64_t now_ns() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

/* Request job for internal pool threads.
   Ownership: scheduler allocates and enqueues; worker thread frees. */
typedef struct {
    op_type_t op;
    char *key;       /* null-terminated owned string (strdup or removed from registry) */
    char *postdata;  /* for POST: JSON body (owned) */
} request_t;

/* Simple bounded queue for request_t* */
typedef struct {
    request_t *buf[QUEUE_CAP];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} job_queue_t;

static void queue_init(job_queue_t *q) {
    q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void queue_destroy(job_queue_t *q) {
    pthread_mutex_lock(&q->lock);
    /* Free any remaining jobs if present */
    while (q->count > 0) {
        request_t *r = q->buf[q->head];
        q->head = (q->head + 1) % QUEUE_CAP;
        q->count--;
        if (r) {
            if (r->key) free(r->key);
            if (r->postdata) free(r->postdata);
            free(r);
        }
    }
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

/* push job, block if full. Return 0 on success, -1 if stop_flag set while waiting (caller must retry/stop) */
static int queue_push(job_queue_t *q, request_t *job) {
    pthread_mutex_lock(&q->lock);
    while (q->count == QUEUE_CAP && !stop_flag) {
        pthread_cond_wait(&q->not_full, &q->lock);
    }
    if (stop_flag && q->count == QUEUE_CAP) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    q->buf[q->tail] = job;
    q->tail = (q->tail + 1) % QUEUE_CAP;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/* pop job, block if empty. Returns job or NULL if stop_flag and empty. */
static request_t *queue_pop(job_queue_t *q) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !stop_flag) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    if (q->count == 0 && stop_flag) {
        pthread_mutex_unlock(&q->lock);
        return NULL;
    }
    request_t *r = q->buf[q->head];
    q->head = (q->head + 1) % QUEUE_CAP;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return r;
}

/* Internal pool thread args */
typedef struct {
    job_queue_t *queue;
    int pool_idx;
    int tid; /* outer top-level worker id */
    unsigned int seed;
} pool_thread_arg_t;

/* Helper: build POST JSON into a malloc'd string (owned by caller) */
static char *make_post_json(const char *key, const char *value) {
    size_t len = strlen(key) + strlen(value) + 64;
    char *buf = malloc(len);
    if (!buf) return NULL;
    snprintf(buf, len, "{\"key\":\"%s\",\"value\":\"%s\"}", key, value);
    return buf;
}

/* Pool thread function: owns its own CURL *easy and services jobs */
static void *pool_thread_func(void *arg) {
    pool_thread_arg_t *parg = (pool_thread_arg_t *)arg;
    job_queue_t *q = parg->queue;
    int tid = parg->tid;
    unsigned int seed = parg->seed ^ (unsigned int)(parg->pool_idx * 7919);

    CURL *easy = curl_easy_init();
    if (!easy) {
        fprintf(stderr, "Worker %d pool %d: curl_easy_init failed\n", tid, parg->pool_idx);
        return NULL;
    }

    /* set common options once */
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, discard_write);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 5L);

    while (1) {
        request_t *req = queue_pop(q);
        if (!req) {
            /* queue empty and stop_flag set -> exit */
            break;
        }

        uint64_t start_ns = now_ns();

        /* configure easy for this request */
        if (req->op == OP_POST) {
            curl_easy_setopt(easy, CURLOPT_URL, g_cfg.server_url);
            curl_easy_setopt(easy, CURLOPT_POST, 1L);
            struct curl_slist *hdrs = NULL;
            hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, req->postdata);
            /* perform */
            CURLcode res = curl_easy_perform(easy);
            long rc = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &rc);
            uint64_t end_ns = now_ns();
            uint64_t lat_ns = end_ns - start_ns;
            int success = (res == CURLE_OK && rc == 200) ? 1 : 0;
            if (success && req->key) {
                /* add to global key registry */
                keys_try_add(&g_keys, req->key);
            }
            metrics_record(OP_POST, success, lat_ns);
            /* cleanup header */
            curl_slist_free_all(hdrs);
            /* reset POST flag and header for safety */
            curl_easy_setopt(easy, CURLOPT_POST, 0L);
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, NULL);

        } else if (req->op == OP_GET) {
            char url[1024];
            snprintf(url, sizeof(url), "%s?key=%s", g_cfg.server_url, req->key ? req->key : "");
            curl_easy_setopt(easy, CURLOPT_URL, url);
            curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
            curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, NULL);
            CURLcode res = curl_easy_perform(easy);
            long rc = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &rc);
            uint64_t end_ns = now_ns();
            uint64_t lat_ns = end_ns - start_ns;
            int success = (res == CURLE_OK && (rc == 200 || rc == 404)) ? 1 : 0;
            metrics_record(OP_GET, success, lat_ns);
            /* ensure no leftover POST flags */
            curl_easy_setopt(easy, CURLOPT_POST, 0L);
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, NULL);

        } else { /* OP_DELETE */
            char url[1024];
            snprintf(url, sizeof(url), "%s?key=%s", g_cfg.server_url, req->key ? req->key : "");
            curl_easy_setopt(easy, CURLOPT_URL, url);
            curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "DELETE");
            curl_easy_setopt(easy, CURLOPT_POST, 0L);
            CURLcode res = curl_easy_perform(easy);
            long rc = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &rc);
            uint64_t end_ns = now_ns();
            uint64_t lat_ns = end_ns - start_ns;
            int success = (res == CURLE_OK && (rc == 200 || rc == 404)) ? 1 : 0;
            metrics_record(OP_DELETE, success, lat_ns);
            /* reset customrequest */
            curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, NULL);
        }

        /* free request ownership (key and postdata were allocated by scheduler or by keys_remove_random) */
        if (req->key) free(req->key);
        if (req->postdata) free(req->postdata);
        free(req);
    }

    curl_easy_cleanup(easy);
    return NULL;
}

/* Worker thread using internal thread-pool and request queue (Option B) */
void *worker_func(void *arg) {
    int tid = (int)(intptr_t)arg;

    /* seed per-thread rand_r for scheduler */
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)(tid * 7919);

    /* Initialize job queue */
    job_queue_t queue;
    queue_init(&queue);

    /* Create pool threads */
    pthread_t pool_threads[INTERNAL_CONCURRENCY];
    pool_thread_arg_t pargs[INTERNAL_CONCURRENCY];

    for (int i = 0; i < INTERNAL_CONCURRENCY; ++i) {
        pargs[i].queue = &queue;
        pargs[i].pool_idx = i;
        pargs[i].tid = tid;
        pargs[i].seed = seed;
        int rc = pthread_create(&pool_threads[i], NULL, pool_thread_func, &pargs[i]);
        if (rc != 0) {
            fprintf(stderr, "Thread %d: failed to create pool thread %d\n", tid, i);
            /* continue creating remaining threads */
            pool_threads[i] = 0;
        }
    }

    /* Scheduler: produce jobs until stop_flag set */
    unsigned long seq = 0;
    while (!stop_flag) {
        /* choose op exactly like old worker.c */
        op_type_t op;
        if (g_cfg.workload == WL_PUT_ALL) {
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

        /* allocate job and populate key/postdata ownership */
        request_t *job = malloc(sizeof(request_t));
        if (!job) {
            usleep(1000);
            continue;
        }
        job->op = op;
        job->key = NULL;
        job->postdata = NULL;

        if (op == OP_POST) {
            ++seq;
            char tmpkey[128];
            snprintf(tmpkey, sizeof(tmpkey), "%s_thr%d_seq%lu", g_cfg.key_prefix, tid, seq);
            char tmpval[128];
            snprintf(tmpval, sizeof(tmpval), "v_%d_%lu", tid, seq);

            job->key = strdup(tmpkey);
            job->postdata = make_post_json(tmpkey, tmpval);

            /* push job (block if queue full) */
            if (queue_push(&queue, job) != 0) {
                /* shutdown in progress; free job and break */
                if (job->key) free(job->key);
                if (job->postdata) free(job->postdata);
                free(job);
                break;
            }

        } else if (op == OP_GET) {
            ++seq;
            int got = 0;
            char key_local[256];

            if (g_cfg.workload == WL_GET_POPULAR ||
               (g_cfg.workload == WL_MIX && keys_count(&g_keys) > 0 && (rand_r(&seed) % 100) < 50)) {
                char *kptr = NULL;
                if (keys_get_random(&g_keys, &kptr)) {
                    /* keys_get_random returns pointer into registry; copy it */
                    snprintf(key_local, sizeof(key_local), "%s", kptr);
                    got = 1;
                }
            }
            if (!got) {
                snprintf(key_local, sizeof(key_local), "%s_unique_thr%d_%lu", g_cfg.key_prefix, tid, seq);
            }
            job->key = strdup(key_local);

            if (queue_push(&queue, job) != 0) {
                if (job->key) free(job->key);
                free(job);
                break;
            }

        } else { /* DELETE */
            ++seq;
            char *removed = NULL;
            int removed_ok = 0;
            if (g_cfg.workload == WL_PUT_ALL || g_cfg.workload == WL_MIX) {
                if (keys_remove_random(&g_keys, &removed)) removed_ok = 1;
            } else {
                if (keys_remove_random(&g_keys, &removed)) removed_ok = 1;
            }

            if (removed_ok) {
                /* keys_remove_random returned an allocated string (ownership transferred), use directly */
                job->key = removed;
                /* removed already allocated by registry; do not free here */
            } else {
                char tmpkey[128];
                snprintf(tmpkey, sizeof(tmpkey), "%s_thr%d_seq%lu", g_cfg.key_prefix, tid, seq);
                job->key = strdup(tmpkey);
            }

            if (queue_push(&queue, job) != 0) {
                if (job->key) free(job->key);
                free(job);
                break;
            }
        }

        /* scheduler pacing: slight sleep to avoid burning 100% CPU if queue is full or service slow */
        /* this is deliberate small backoff, tune if needed */
        usleep(0); /* no sleep by default; yields CPU */
    }

    /* Stop: wake up all pool threads (they exit when queue empty and stop_flag set) */
    pthread_mutex_lock(&queue.lock);
    pthread_cond_broadcast(&queue.not_empty);
    pthread_cond_broadcast(&queue.not_full);
    pthread_mutex_unlock(&queue.lock);

    /* join pool threads */
    for (int i = 0; i < INTERNAL_CONCURRENCY; ++i) {
        if (pool_threads[i]) pthread_join(pool_threads[i], NULL);
    }

    /* destroy queue (frees any leftover jobs) */
    queue_destroy(&queue);

    return NULL;
}
