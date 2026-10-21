#ifndef PUDDLE_CLIENT_H
#define PUDDLE_CLIENT_H

/* Calling another Puddle: one signed HTTPS request (docs/streams.md, "Request signatures").
 *
 * TLS here is not what authenticates anyone. Every Puddle makes its own certificate, so none can be checked
 * against a CA or a host name, and none is: the connection is encrypted and the request is signed, and the
 * signature is what the other side relies on. What the certificate still gives us is a fingerprint, which is
 * recorded on first contact and compared afterwards, so a Puddle that has been replaced or impersonated at
 * the same address is noticed (`fingerprint_ok` is asked before anything is sent). It does not depend on the
 * address, so moving house doesn't change it.
 *
 * Addresses come from strangers (a link, a request), so the ones that can only be a mistake or a probe are
 * refused before connecting: unspecified, multicast, broadcast and link-local (which includes the cloud
 * metadata address). Loopback and private ranges stay allowed, since friends on one network are normal. */

#include "identity.h"
#include "link.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PUDDLE_NET_OK,           /* an HTTP answer arrived (its status says what the other side thought) */
    PUDDLE_NET_OFFLINE,      /* nobody there: refused, timed out, unreachable, no such host. Worth retrying. */
    PUDDLE_NET_FAILED,       /* reached something, but not a Puddle conversation: TLS or HTTP went wrong. Not retried. */
    PUDDLE_NET_CERT_CHANGED  /* `fingerprint_ok` refused the certificate; nothing was sent */
} puddle_net_result;

typedef struct {
    puddle_net_result result;
    int status;               /* HTTP status, for OK */
    char reason[200];         /* in words, for anything but OK */
    char fingerprint[65];     /* hex SHA-256 of the certificate; "" if TLS was not reached */
    char content_type[48];    /* of the answer, as it said (lower case, without parameters); "" if it did not */
    char *body;               /* the response body, NUL-terminated, or NULL. Free with puddle_net_reply_free. */
    size_t body_len;
} puddle_net_reply;

typedef struct {
    int connect_timeout_s;    /* 0 = 8 */
    int io_timeout_s;         /* each read or write; 0 = 10 */
    int total_timeout_s;      /* the whole call; 0 = 30 */
    size_t max_body;          /* the largest answer to accept; 0 = PUDDLE_NET_BODY_MAX. Larger ones fail rather than fill memory. */
    /* Asked once the certificate is known and before anything is sent: 1 to go on, 0 to stop. NULL = always. */
    int (*fingerprint_ok)(void *user, const char *fingerprint_hex);
    void *user;
} puddle_net_opts;

#define PUDDLE_NET_BODY_MAX (64u * 1024)

/* POST or GET `target` at `addr`, signed by `me` for the Puddle `peer_id`. `body` is sent as JSON (NULL/0 for
 * none). Never fails to fill `out`; always puddle_net_reply_free() it. */
void puddle_net_request(const puddle_address *addr, const char *method, const char *target, const void *body,
                        size_t body_len, puddle_identity *me, const char *peer_id, const puddle_net_opts *opts,
                        puddle_net_reply *out);
void puddle_net_reply_free(puddle_net_reply *r);

/* Would we dial this address? (resolved addresses are checked the same way.) For tests and for callers who
 * want to say no early. */
int puddle_net_host_dialable(const char *host);

/* For addresses that arrive from someone else: is this host an acceptable thing to record? A numeric address we would
 * never dial (unspecified, multicast, link-local...) is not; a name is accepted, since judging it would mean a lookup, and
 * the client checks what it resolves to when it comes to connect. */
int puddle_net_host_plausible(const char *host);

#ifdef __cplusplus
}
#endif
#endif
