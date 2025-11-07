#ifndef DB_H
#define DB_H

#include <libpq-fe.h>

/* Initialize DB connection (conninfo is libpq connection string).
 * Returns 0 on success, non-zero on failure.
 */
int db_init(const char *conninfo);
void db_close(void);

/* create or update key */
int db_put(const char *key, const char *value);

/* read key; returns 0 and sets *out_value (caller must free), -1 if not found */
int db_get(const char *key, char **out_value);

/* delete key; returns 0 on success, -1 if not present */
int db_delete(const char *key);

#endif /* DB_H */
