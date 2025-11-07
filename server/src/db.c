#define _GNU_SOURCE
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpq-fe.h>
#include <pthread.h>

static PGconn *conn = NULL;
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;

int db_init(const char *conninfo) {
    pthread_mutex_lock(&db_lock);
    conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "db_init: connection failed: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
        conn = NULL;
        pthread_mutex_unlock(&db_lock);
        return -1;
    }
    /* ensure table exists */
    const char *sql = "CREATE TABLE IF NOT EXISTS kv_store ("
                      "key TEXT PRIMARY KEY,"
                      "value TEXT NOT NULL);";
    PGresult *res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "db_init: failed to create table: %s\n", PQerrorMessage(conn));
        PQclear(res);
        PQfinish(conn);
        conn = NULL;
        pthread_mutex_unlock(&db_lock);
        return -1;
    }
    PQclear(res);
    pthread_mutex_unlock(&db_lock);
    return 0;
}

void db_close(void) {
    pthread_mutex_lock(&db_lock);
    if (conn) { PQfinish(conn); conn = NULL; }
    pthread_mutex_unlock(&db_lock);
}

int db_put(const char *key, const char *value) {
    if (!conn) return -1;
    pthread_mutex_lock(&db_lock);
    /* upsert using ON CONFLICT */
    const char *params[2] = { key, value };
    PGresult *res = PQexecParams(conn,
                                 "INSERT INTO kv_store (key, value) VALUES ($1, $2) "
                                 "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value;",
                                 2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "db_put error: %s\n", PQerrorMessage(conn));
        PQclear(res);
        pthread_mutex_unlock(&db_lock);
        return -1;
    }
    PQclear(res);
    pthread_mutex_unlock(&db_lock);
    return 0;
}

int db_get(const char *key, char **out_value) {
    if (!conn) return -1;
    pthread_mutex_lock(&db_lock);
    const char *paramValues[1] = { key };
    PGresult *res = PQexecParams(conn,
                                 "SELECT value FROM kv_store WHERE key = $1;",
                                 1, NULL, paramValues, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        pthread_mutex_unlock(&db_lock);
        return -1;
    }
    if (PQntuples(res) == 0) {
        PQclear(res);
        pthread_mutex_unlock(&db_lock);
        return -1;
    }
    char *val = strdup(PQgetvalue(res, 0, 0));
    PQclear(res);
    *out_value = val;
    pthread_mutex_unlock(&db_lock);
    return 0;
}

int db_delete(const char *key) {
    if (!conn) return -1;
    pthread_mutex_lock(&db_lock);
    const char *params[1] = { key };
    PGresult *res = PQexecParams(conn, "DELETE FROM kv_store WHERE key = $1;", 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        pthread_mutex_unlock(&db_lock);
        return -1;
    }
    int affected = atoi(PQcmdTuples(res));
    PQclear(res);
    pthread_mutex_unlock(&db_lock);
    return (affected > 0) ? 0 : -1;
}
