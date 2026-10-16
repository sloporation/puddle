#ifndef PUDDLE_ACCESS_H
#define PUDDLE_ACCESS_H

/* Taking a request for a stream from someone we may not know: POST /stream/v1/access (docs/streams.md,
 * "Asking for a stream"). Two pieces, both plugged into the route table:
 *
 *   puddle_access_chain_key  finds the requester's key in the chain the request carries, so the router can
 *                            check the signature. This is work done for a stranger, so it is rate-limited.
 *   puddle_access_post       what happens to a request whose signature is good.
 *
 * A request that does not authenticate is a 404 like any unknown path. One that does is answered 202 with
 * no body whatever is decided (blocked, closed, rate-limited, over capacity, approved or left waiting), and
 * the decision is only written to our own log. */

#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_ACCESS_BODY_MAX (96u * 1024)     /* the whole request */
#define PUDDLE_ACCESS_CHAIN_MAX (64u * 1024)    /* the chain inside it (about 80 key changes) */
#define PUDDLE_NOTE_MAX_CHARS 500
#define PUDDLE_MAX_PENDING_IN 200               /* requests waiting on the owner */
#define PUDDLE_MAX_PEERS 5000                   /* records of any kind */

/* One side's account of itself to the other: {"id","address","profile","chain"}. The profile is rebuilt from the
 * few text fields we use, the address must parse, and the chain is kept as text (verified by puddle_peer_key). */
typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    char address[PUDDLE_ADDR_MAX];
    char profile[12288]; /* PUDDLE_WIRE_PROFILE_MAX */
    char *chain;
    size_t chain_len;
} puddle_peer_info;
int puddle_peer_parse(const char *obj, size_t len, puddle_peer_info *p);
void puddle_peer_info_free(puddle_peer_info *p);
/* Is `p` really `sender_id`, and does its chain (which must verify, and belong to that id) name `kid` as the
 * current signing key? Fills `sign_pub`. For the chain-proven routes' key lookup. */
int puddle_peer_key(const puddle_peer_info *p, const char *sender_id, const char *kid, unsigned char sign_pub[32]);

int puddle_access_chain_key(puddle_ctx *ctx, const unsigned char *body, size_t body_len, const char *sender_id,
                            const char *kid, unsigned char sign_pub[32]);
int puddle_access_post(const puddle_call *call, puddle_jw *w);

/* Clean up text a stranger sent: valid UTF-8 (bad bytes become U+FFFD), no control characters (a newline is kept
 * if `multiline`), no bidirectional overrides (they let a name read backwards to hide what it says), trimmed.
 * 0 if it fits in `max_chars` characters and `cap` bytes, else -1; `*chars` gets the length. */
int puddle_clean_text(const char *in, size_t len, size_t max_chars, int multiline, char *out, size_t cap, size_t *chars);

#ifdef __cplusplus
}
#endif
#endif
