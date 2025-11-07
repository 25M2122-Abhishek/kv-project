#define _GNU_SOURCE
#include "http_server.h"
#include "db.h"
#include <civetweb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jansson.h>
#include <pthread.h>

static lru_cache_t *global_cache = NULL;

/* mg = Mongoose Group (CivetWeb is a fork of Mongoose) */
/* Represents a running server instance */
static struct mg_context *global_ctx = NULL;  

/* Helper: read request body */
static char *read_body(struct mg_connection *conn, size_t len) {
    if (len == 0) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int got = mg_read(conn, buf, (int)len);
    if (got <= 0) { free(buf); return NULL; }
    buf[got] = '\0';
    return buf;
}

/* POST /kv  JSON body {"key":"k","value":"v"} */
static int post_kv_handler(struct mg_connection *conn, void *cbdata) {
    const struct mg_request_info *req_info = mg_get_request_info(conn);
    size_t len = (size_t)req_info->content_length;
    char *body = read_body(conn, len);
    if (!body) {
        mg_printf(conn,
                  "HTTP/1.1 400 Bad Request\r\n"
                  "Content-Type: text/plain\r\n\r\n"
                  "Bad body\n");
        return 1;
    }
    json_error_t jerr;
    json_t *root = json_loads(body, 0, &jerr);
    free(body);
    if (!root) {
        mg_printf(conn,
                  "HTTP/1.1 400 Bad Request\r\n"
                  "Content-Type: text/plain\r\n\r\n"
                  "Invalid JSON\n");
        return 1;
    }

    json_t *jkey = json_object_get(root, "key");
    json_t *jval = json_object_get(root, "value");
    if (!json_is_string(jkey) || !json_is_string(jval)) {
        json_decref(root);
        mg_printf(conn,
                  "HTTP/1.1 400 Bad Request\r\n"
                  "Content-Type: text/plain\r\n\r\n"
                  "Missing key/value\n");
        return 1;
    }

    const char *key = json_string_value(jkey);
    const char *val = json_string_value(jval);

    if (db_put(key, val) != 0) {
        json_decref(root);
        mg_printf(conn,
                  "HTTP/1.1 500 Internal Server Error\r\n"
                  "Content-Type: text/plain\r\n\r\n"
                  "DB error\n");
        return 1;
    }

    lru_cache_put(global_cache, key, val);
    json_decref(root);
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/plain\r\n\r\n"
              "OK\n");
    return 1;
}

/* GET /kv?key=... */
static int get_kv_handler(struct mg_connection *conn, void *cbdata) {
    const struct mg_request_info *req_info = mg_get_request_info(conn);
    const char *qs = req_info->query_string ? req_info->query_string : "";
    char key_buf[1024] = {0};

    const char *p = strstr(qs, "key=");
    if (!p) {
        mg_printf(conn,
                  "HTTP/1.1 400 Bad Request\r\n"
                  "Content-Type: text/plain\r\n\r\n"
                  "Missing key param\n");
        return 1;
    }

    strncpy(key_buf, p + 4, sizeof(key_buf) - 1);
    char *amp = strchr(key_buf, '&');
    if (amp) *amp = '\0';

    char *cached = NULL;
    if (lru_cache_get(global_cache, key_buf, &cached) == 0) {
        mg_printf(conn,
                  "HTTP/1.1 200 OK\r\n"
                   "X-Source: CACHE\r\n"
                  "Content-Type: text/plain\r\n\r\nCACHE:%s\n", cached);
        free(cached);
        return 1;
    }

    char *dbval = NULL;
    if (db_get(key_buf, &dbval) == 0) {
        lru_cache_put(global_cache, key_buf, dbval);
        mg_printf(conn,
                  "HTTP/1.1 200 OK\r\n"
                   "X-Source: DB\r\n"
                  "Content-Type: text/plain\r\n\r\nDB:%s\n", dbval);
        free(dbval);
        return 1;
    } else {
        mg_printf(conn,
                  "HTTP/1.1 404 Not Found\r\n"
                  "Content-Type: text/plain\r\n\r\n"
                  "Key not found\n");
        return 1;
    }
}

/* DELETE /kv?key=... */
static int delete_kv_handler(struct mg_connection *conn, void *cbdata) {
    const struct mg_request_info *req_info = mg_get_request_info(conn);
    const char *qs = req_info->query_string ? req_info->query_string : "";
    char key_buf[1024] = {0};

    const char *p = strstr(qs, "key=");
    if (!p) {
        mg_printf(conn,
                  "HTTP/1.1 400 Bad Request\r\n"
                  "Content-Type: text/plain\r\n\r\n"
                  "Missing key param\n");
        return 1;
    }

    strncpy(key_buf, p + 4, sizeof(key_buf) - 1);
    char *amp = strchr(key_buf, '&');
    if (amp) *amp = '\0';

    if (db_delete(key_buf) == 0) {
        lru_cache_delete(global_cache, key_buf);
        mg_printf(conn,
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/plain\r\n\r\n"
                  "Deleted\n");
        return 1;
    } else {
        mg_printf(conn,
                  "HTTP/1.1 404 Not Found\r\n"
                  "Content-Type: text/plain\r\n\r\n"
                  "Key not found\n");
        return 1;
    }
}

/* Unified request dispatcher */
static int unified_handler(struct mg_connection *conn, void *cbdata) {
    const struct mg_request_info *req = mg_get_request_info(conn);
    if (strcmp(req->request_method, "POST") == 0)
        return post_kv_handler(conn, cbdata);
    else if (strcmp(req->request_method, "GET") == 0)
        return get_kv_handler(conn, cbdata);
    else if (strcmp(req->request_method, "DELETE") == 0)
        return delete_kv_handler(conn, cbdata);

    mg_printf(conn,
              "HTTP/1.1 405 Method Not Allowed\r\n"
              "Content-Type: text/plain\r\n\r\n");
    return 1;
}

/* Server start */
int http_server_start(int port, lru_cache_t *cache, const char *db_conninfo, int threads) {
    global_cache = cache;
    if (db_init(db_conninfo) != 0) {
        fprintf(stderr, "Failed to initialize DB\n");
        return -1;
    }

    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);

    char threads_s[16];
    snprintf(threads_s, sizeof(threads_s), "%d", threads);

    const char *options[] = {
        "document_root", ".",           // Directory for static files (like HTML, JS) — not used here
        "listening_ports", port_s,      // Port number the web server listens on
        "num_threads", threads_s,            // Number of worker threads to handle requests concurrently
        NULL
    };

    /* CivetWeb uses default request handling mechanism. */
    struct mg_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));

    global_ctx = mg_start(&callbacks, NULL, options);
    if (!global_ctx) {
        fprintf(stderr, "Failed to start CivetWeb\n");
        db_close();
        return -1;
    }

    mg_set_request_handler(global_ctx, "/kv", unified_handler, NULL);

    printf("HTTP server listening on port %d\n", port);
    return 0;
}

void http_server_stop(void) {
    if (global_ctx) {
        mg_stop(global_ctx);
        global_ctx = NULL;
    }
    db_close();
}
