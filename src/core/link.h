#ifndef PUDDLE_LINK_H
#define PUDDLE_LINK_H

/* Links and addresses. A link is what one owner gives another so their Puddle can ask for a stream:
 *
 *     puddle://<user id>@<address>[?invite=<token>]
 *
 * where <address> is host:port, with an IPv6 literal in brackets: `203.0.113.7:8443`, `[2001:db8::1]:8443`,
 * `home.example.org:8443`. The port defaults to 8443 if left out. Nothing is looked up or resolved here;
 * this only decides whether the text is well-formed, and the strictness is the point: a link comes from
 * outside, and what it names is where we will connect and which key we will insist on. */

#include "identity.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_ADDR_MAX 128          /* "host:port", with brackets for IPv6 */
#define PUDDLE_INVITE_TOKEN_LEN 26   /* base32 of 16 random bytes */
#define PUDDLE_DEFAULT_PORT 8443

typedef struct {
    char host[PUDDLE_ADDR_MAX];      /* without brackets */
    unsigned port;
    char address[PUDDLE_ADDR_MAX];   /* canonical "host:port" / "[v6]:port", as it is stored and dialled */
} puddle_address;

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_address address;
    char invite[PUDDLE_INVITE_TOKEN_LEN + 1]; /* "" if the link has none */
} puddle_link;

/* Is `s` a user id (52 characters of lowercase base32)? Used before an id is allowed anywhere near a path. */
int puddle_id_valid(const char *s);

/* Is `s` an invite token as we make them? */
int puddle_invite_token_valid(const char *s);

/* Parse "host:port" (or just "host"). 0 or -1. Host is an IPv4 literal, a bracketed IPv6 literal, or a
 * DNS name (letters, digits, dots, hyphens; no empty labels). */
int puddle_address_parse(const char *text, puddle_address *out);

/* Parse a link. 0 or -1 with a short reason in `err`. Anything unexpected is refused: other schemes,
 * spaces, a path, a fragment, unknown query parameters, a second `@`. */
int puddle_link_parse(const char *text, puddle_link *out, char *err, size_t errlen);

/* Build a link. 0 or -1 if it does not fit. `invite` may be NULL or "". */
int puddle_link_format(const char *id, const char *address, const char *invite, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
