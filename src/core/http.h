#ifndef PUDDLE_HTTP_H
#define PUDDLE_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char method[8];
    char path[1024];    /* query string and fragment stripped */
    char target[1100];  /* path and query exactly as sent (what request signatures cover) */
    char authorization[512];   /* "" if absent */
    char signature[1024];      /* X-Puddle-Signature, "" if absent */
} puddle_request;

/* Parse the request line and the two headers we use of a (complete) header block. A repeated
 * Authorization or X-Puddle-Signature, or one that is too long, is malformed. 0 = ok, -1 = malformed. */
int puddle_http_parse(const char *buf, size_t len, puddle_request *req);

/* Build a full HTTP/1.1 response for `req` (NULL = malformed) into `out`.
 * `owner_ok` says the request carried the owner token. Anything that needs it and doesn't have it is
 * answered exactly like a path that doesn't exist. Stores the status code in *status and returns the
 * byte length. */
size_t puddle_http_route(const puddle_request *req, int owner_ok, char *out, size_t cap, int *status);

/* Build a bodyless-routing error response (e.g. 408, 431). */
size_t puddle_http_error(int status, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
