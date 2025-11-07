#ifndef LOADGEN_H
#define LOADGEN_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include "key_registry.h"

/* ---------- Operation Types ---------- */
typedef enum { OP_GET = 0, OP_POST = 1, OP_DELETE = 2 } op_type_t;

/* ---------- Metrics ---------- */
typedef struct {
    uint64_t count;
    uint64_t success;
    uint64_t failure;
    uint64_t total_ns;
} op_stats_t;

typedef struct {
    op_stats_t stats[3];
    pthread_mutex_t lock;
} metrics_t;

/* ---------- Workload Modes ---------- */
typedef enum {
    WL_MIX = 0,
    WL_PUT_ALL,
    WL_GET_ALL,
    WL_GET_POPULAR
} workload_t;

/* ---------- Configuration ---------- */
typedef struct {
    char server_url[512];
    int threads;
    int duration;
    int mix_get;
    int mix_post;
    int mix_delete;
    char key_prefix[64];

    /* extended fields */
    workload_t workload;
    size_t key_pool_size;   /* default 100000 */
    size_t popular_size;    /* default 100 */
} config_t;

/* ---------- Extern Globals (defined in main.c) ---------- */
extern config_t g_cfg;
extern metrics_t g_metrics;
extern volatile int stop_flag;

/* Key registry global (defined in main.c) */
extern key_registry_t g_keys;

/* ---------- Metrics API ---------- */
void metrics_init(metrics_t *m);
void metrics_destroy(metrics_t *m);
void metrics_record(op_type_t op, int success, uint64_t latency_ns);

/* ---------- Time Utility ---------- */
uint64_t timespec_diff_ns(const struct timespec *start, const struct timespec *end);

#endif /* LOADGEN_H */
