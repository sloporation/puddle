#ifndef PUDDLE_HTTP_H
#define PUDDLE_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char method[8];
    char path[1024]; /* query string and fragment stripped */
} puddle_request;

/* Parse the request line of a (complete) header block. 0 = ok, -1 = malformed. */
int puddle_http_parse(const char *buf, size_t len, puddle_request *req);

/* Build a full HTTP/1.1 response for `req` (NULL = malformed) into `out`.
 * Stores the status code in *status and returns the byte length. */
size_t puddle_http_route(const puddle_request *req, char *out, size_t cap, int *status);

/* Build a bodyless-routing error response (e.g. 408, 431). */
size_t puddle_http_error(int status, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
