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
#include "outbox.h"
#include "ratelimit.h"
#include "reqsig.h"
#include "store.h"
#include "streams.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A roll of the keys that has been started and is waiting for the owner to say the new phrase is written down. */
typedef struct puddle_roll_slot puddle_roll_slot;
puddle_roll_slot *puddle_roll_slot_new(void);
void puddle_roll_slot_free(puddle_roll_slot *s); /* abandons anything pending */

typedef enum { PUDDLE_AUTH_OWNER, PUDDLE_AUTH_PEER, PUDDLE_AUTH_CHAIN, PUDDLE_AUTH_CHAIN_OPEN } puddle_auth_kind;

/* What handlers can reach. Streams and the outbox join it in phase 3. */
typedef struct {
    const puddle_config *config;
    puddle_identity *identity;
    puddle_store *store;
    const puddle_profiles *profiles;
    puddle_assignments *assignments;
    puddle_streams *streams;
    puddle_outbox *outbox;                 /* what we owe other Puddles; NULL in tests that do not send */
    puddle_replay *replay;                 /* one nonce memory for the whole Puddle */
    puddle_bucket *anon_bucket;            /* work done for strangers before they are authenticated; NULL = unlimited */
    puddle_keyed *access_limit;            /* requests for a stream, per requester; NULL = unlimited */
    char own_tls_fp[65];                   /* the SHA-256 of our certificate, hex; "" if not known */
    size_t max_peers;                      /* records on file before new strangers are ignored; 0 = PUDDLE_MAX_PEERS */
    puddle_roll_slot *roll_slot;
    puddle_peer_key_fn peer_key;           /* PEER routes: current signing key of a known peer (puddle_streams_peer_key); NULL = none known */
    void *peer_key_user;
    int64_t (*now_ms)(void);               /* NULL = the clock; a seam for tests */
} puddle_ctx;

/* An answer that is a file, not JSON: a handler fills this (data is malloc'd; the router sends and frees it). */
typedef struct {
    unsigned char *data;
    size_t len;
    char mime[40];
} puddle_blob;

typedef struct {
    puddle_ctx *ctx;
    puddle_blob *blob;                     /* set to answer with a file instead of JSON */
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

/* As puddle_router_handle, for the server: a file answer comes back in `big` (headers in `out`) instead of having to fit in it. */
size_t puddle_router_handle_big(const puddle_router *r, const puddle_request *req, int owner_ok, const unsigned char *body,
                                size_t body_len, char *out, size_t cap, int *status, puddle_big *big);

/* How many body bytes a request may carry, from its headers alone (asked before the body is read). A route the
 * caller could not use gets 0, so a body to it is refused the way an unknown path is. */
size_t puddle_router_body_limit(const puddle_router *r, const puddle_request *req, int owner_ok);

/* Authenticate, run the handler, build the full HTTP response. `owner_ok`: the request carried the owner token. */
size_t puddle_router_handle(const puddle_router *r, const puddle_request *req, int owner_ok,
                            const unsigned char *body, size_t body_len, char *out, size_t cap, int *status);

/* The time in milliseconds, from the context's clock when there is one (tests set it). */
int64_t puddle_ctx_now_ms(const puddle_ctx *ctx);
int64_t puddle_call_now_ms(const puddle_call *call);

/* The address to give someone for this Puddle (host[:port], the default port left off), with `?invite=<token>` when there is one,
 * or 0 if we cannot say where that is:
 * listening on every interface says nothing about which address the outside world should use, so the owner
 * has to set `advertise:` in the config. */
int puddle_make_link(const puddle_config *cfg, const char *invite, char *out, size_t cap);

#define PUDDLE_QUERY_VALUE_MAX 80 /* room for a user id (52 characters) */

/* Read the query string of a request target. `keys` lists the parameters allowed; each found one is copied
 * (letters, digits, `_` and `-` only, up to 79 characters) to `values[i]`, and left "" if absent. A parameter not
 * in the list, repeated, empty, or with any other character is an error. 0 or -1. */
int puddle_query_parse(const char *target, const char *const *keys, size_t nkeys, char (*values)[PUDDLE_QUERY_VALUE_MAX]);

/* Helper for handlers: write {"error":"<code>"} and return `status`. */
int puddle_fail(puddle_jw *w, int status, const char *code);

/* The Puddle's own routes (routes.c). */
extern const puddle_route puddle_routes[];
extern const size_t puddle_routes_count;

/* Every route the Puddle serves: the table above plus the owner's stream routes. Built once, on first use;
 * call it before starting threads. */
const puddle_route *puddle_all_routes(size_t *count);

#ifdef __cplusplus
}
#endif
#endif
