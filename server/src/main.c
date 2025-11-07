#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include "cache.h"
#include "http_server.h"

static volatile int keep_running = 1;
void int_handler(int dummy) { keep_running = 0; }

int main(int argc, char **argv) {
    int port = 8080;
    int threads = 16;
    size_t cache_capacity = 1000;
    const char *db_conninfo = "host=localhost port=5432 dbname=kvdb user=kvuser password=kvpass";

    // if (argc >= 2) port = atoi(argv[1]);
    // if (argc >= 3) cache_capacity = atoi(argv[2]);
    // if (argc >= 4) db_conninfo = argv[3];

    if (argc >= 2) cache_capacity = atoi(argv[1]);
    if (argc >= 3) threads = atoi(argv[2]);

    signal(SIGINT, int_handler);
    signal(SIGTERM, int_handler);

    lru_cache_t *cache = lru_cache_create(cache_capacity);
    if (!cache) {
        fprintf(stderr, "Failed to create cache\n");
        return 1;
    }

    if (http_server_start(port, cache, db_conninfo, threads) != 0) {
        fprintf(stderr, "Failed to start http server\n");
        lru_cache_destroy(cache);
        return 1;
    }

    printf("Server running. Press Ctrl-C to stop.\n");
    while (keep_running) {
        sleep(1);
    }

    printf("Shutting down...\n");
    http_server_stop();
    lru_cache_destroy(cache);
    return 0;
}
