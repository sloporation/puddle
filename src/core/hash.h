#ifndef PUDDLE_HASH_H
#define PUDDLE_HASH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lowercase hex of `len` bytes into `out` (needs 2*len+1 bytes). */
void puddle_hex_encode(const unsigned char *bytes, unsigned len, char *out);

/* SHA-256 of `data` as 64 lowercase hex characters (+ NUL). */
void puddle_sha256_hex(const void *data, size_t len, char out[65]);

#ifdef __cplusplus
}
#endif
#endif
