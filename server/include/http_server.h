#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "cache.h"

/* initialize http server, returns 0 on success */
int http_server_start(int port, lru_cache_t *cache, const char *db_conninfo, int threads);

/* stop server (not implemented fully) */
void http_server_stop(void);

#endif /* HTTP_SERVER_H */
