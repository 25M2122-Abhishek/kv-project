#define _GNU_SOURCE
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "loadgen.h"

void metrics_init(metrics_t *m) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    pthread_mutex_init(&m->lock, NULL);
}

void metrics_destroy(metrics_t *m) {
    if (!m) return;
    pthread_mutex_destroy(&m->lock);
    memset(m, 0, sizeof(*m));
}

void metrics_record(op_type_t op, int success, uint64_t latency_ns) {
    if (op < 0 || op > 2) return;
    metrics_t *m = &g_metrics;
    pthread_mutex_lock(&m->lock);
    m->stats[op].count++;
    if (success) {
        m->stats[op].success++;
        m->stats[op].total_ns += latency_ns;
    } else {
        m->stats[op].failure++;
    }
    pthread_mutex_unlock(&m->lock);
}

uint64_t timespec_diff_ns(const struct timespec *start, const struct timespec *end) {
    uint64_t s = (uint64_t)start->tv_sec;
    uint64_t ns = (uint64_t)start->tv_nsec;
    uint64_t es = (uint64_t)end->tv_sec;
    uint64_t ens = (uint64_t)end->tv_nsec;
    return (es - s) * (uint64_t)1000000000ULL + (ens - ns);
}
