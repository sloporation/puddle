#ifndef PUDDLE_ROUTER_H
#define PUDDLE_ROUTER_H

/* The route table: method + path -> handler, and who may call it. Every route declares one of three ways a
 * caller proves itself, and the router checks it before the handler runs, so a handler never sees a request
 * that was not authenticated for it. A failed check is answered exactly like a path that does not exist.
 *
 *   OWNER  the request carries the owner token (the app, curl, a paired phone client)
 *   PEER   a signed request from a Puddle we already have a stream with (its current key is looked up)
 *   CHAIN  a signed request proven by a key chain carried in the request itself: a stranger asking for a
 *          stream, or a peer pushing its new keys after a roll. The route says how to find the key. */

#include "config.h"
#include "http.h"
#include "identity.h"
#include "json.h"
#include "profile.h"
#include "reqsig.h"
#include "store.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A roll of the keys that has been started and is waiting for the owner to say the new phrase is written down. */
typedef struct puddle_roll_slot puddle_roll_slot;
puddle_roll_slot *puddle_roll_slot_new(void);
void puddle_roll_slot_free(puddle_roll_slot *s); /* abandons anything pending */

typedef enum { PUDDLE_AUTH_OWNER, PUDDLE_AUTH_PEER, PUDDLE_AUTH_CHAIN } puddle_auth_kind;

/* What handlers can reach. Streams and the outbox join it in phase 3. */
typedef struct {
    const puddle_config *config;
    puddle_identity *identity;
    puddle_store *store;
    const puddle_profiles *profiles;
    puddle_assignments *assignments;
    puddle_replay *replay;                 /* one nonce memory for the whole Puddle */
    puddle_roll_slot *roll_slot;
    puddle_peer_key_fn peer_key;           /* PEER routes: current signing key of a known peer; NULL = none known */
    void *peer_key_user;
    int64_t (*now_ms)(void);               /* NULL = the clock; a seam for tests */
} puddle_ctx;

typedef struct {
    puddle_ctx *ctx;
    const puddle_request *req;
    const unsigned char *body;             /* NUL-terminated past body_len, binary-safe by length */
    size_t body_len;
    puddle_auth_kind auth;
    const char *sender;                    /* PEER/CHAIN: the authenticated sender's user id; NULL for OWNER */
} puddle_call;

/* Write the JSON answer into `w` and return the HTTP status; write nothing for an empty body. */
typedef int (*puddle_handler)(const puddle_call *call, puddle_jw *w);

/* CHAIN routes: find the key a request names in the chain the body carries, verifying that chain. Return 1 and
 * fill `sign_pub` only if it checks out. */
typedef int (*puddle_chain_key_fn)(puddle_ctx *ctx, const unsigned char *body, size_t body_len,
                                   const char *sender_id, const char *kid, unsigned char sign_pub[32]);

typedef struct {
    const char *method;                    /* "GET" or "POST" only */
    const char *path;
    puddle_auth_kind auth;
    size_t body_max;                       /* 0 = takes no body */
    puddle_handler fn;
    puddle_chain_key_fn chain_key;         /* CHAIN only */
} puddle_route;

typedef struct {
    const puddle_route *routes;
    size_t count;
    puddle_ctx *ctx;
} puddle_router;

/* How many body bytes a request may carry, from its headers alone (asked before the body is read). A route the
 * caller could not use gets 0, so a body to it is refused the way an unknown path is. */
size_t puddle_router_body_limit(const puddle_router *r, const puddle_request *req, int owner_ok);

/* Authenticate, run the handler, build the full HTTP response. `owner_ok`: the request carried the owner token. */
size_t puddle_router_handle(const puddle_router *r, const puddle_request *req, int owner_ok,
                            const unsigned char *body, size_t body_len, char *out, size_t cap, int *status);

/* Helper for handlers: write {"error":"<code>"} and return `status`. */
int puddle_fail(puddle_jw *w, int status, const char *code);

/* The Puddle's own routes (routes.c). */
extern const puddle_route puddle_routes[];
extern const size_t puddle_routes_count;

#ifdef __cplusplus
}
#endif
#endif
