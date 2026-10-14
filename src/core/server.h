#ifndef PUDDLE_SERVER_H
#define PUDDLE_SERVER_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct puddle_server puddle_server;

/* Open the data directory (locked: a second Puddle on the same directory fails), load/generate
 * TLS material and bind the listening socket. NULL + message in `err` on failure. */
puddle_server *puddle_server_create(const puddle_config *cfg, char *err, size_t errlen);

struct puddle_store;
/* The chat store this server opened (valid until puddle_server_destroy). */
struct puddle_store *puddle_server_store(puddle_server *s);

/* Accept loop; blocks until puddle_server_stop(), then drains in-flight
 * connections. Returns 0 on clean stop. */
int puddle_server_run(puddle_server *s);

/* Ask run() to return. Thread-safe and async-signal-safe. */
void puddle_server_stop(puddle_server *s);

/* Only after run() has returned (or was never called). */
void puddle_server_destroy(puddle_server *s);

#ifdef __cplusplus
}
#endif
#endif
