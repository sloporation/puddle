#ifndef PUDDLE_SEAL_H
#define PUDDLE_SEAL_H

/* Encrypting a message body to the Puddles that may read it (docs/identity.md, "Encryption").
 *
 * One sealed blob holds the body encrypted once, and a small entry for each recipient, so a group message is one
 * blob with the same bytes for everyone (and so one event id and one signature). Each entry carries the body's
 * random key wrapped to that recipient's X25519 encryption key; the Puddle's one key pair is the only key involved.
 *
 *   ver(1) | ephemeral public key(32) | nonce(12) | n(1) | n x [ key tag(8) | wrapped key(48) ] | body + tag(16)
 *
 * The wrap key for a recipient is HKDF-SHA256 of X25519(ephemeral, recipient) with the ephemeral and recipient
 * public keys as context; the wrap and the body both use ChaCha20-Poly1305. This is HPKE's base mode built from
 * parts OpenSSL 3.0 has. The header (everything before the body) and the caller's `aad` are authenticated, so a
 * sealed body cannot be moved into another chat, another sender or another message, and a recipient entry cannot be
 * added, dropped or swapped without the open failing. Nothing here knows who sent it: sender authenticity is the
 * event signature's job. */

#include "identity.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_SEAL_MAX_RECIPIENTS 200
#define PUDDLE_SEAL_OVERHEAD(n) (1 + 32 + 12 + 1 + (size_t)(n) * 56 + 16)

/* Seal `plain` to the `n` public keys in `recipients` (each 32 bytes, 1 to PUDDLE_SEAL_MAX_RECIPIENTS, no
 * duplicates needed; a repeated key is simply listed twice). `aad` is context both sides must agree on (for a
 * chat event: its chat, sender and time); it is not stored. Writes PUDDLE_SEAL_OVERHEAD(n) + plain_len bytes to
 * `out` and sets *out_len. 0, or -1 if the arguments are out of range, `cap` is too small, or a key is bad. */
int puddle_seal(const unsigned char (*recipients)[32], size_t n, const void *aad, size_t aad_len, const void *plain,
                size_t plain_len, unsigned char *out, size_t cap, size_t *out_len);

/* Open a blob sealed to this identity's current encryption key. `out` must hold at least blob_len bytes; the plain
 * text is written to it and its length to *out_len (it is not NUL-terminated). 0, or -1 for anything wrong: not a
 * blob, not sealed to this key, `aad` different, or any byte changed. The reason is not told apart on purpose. */
int puddle_open(puddle_identity *me, const void *aad, size_t aad_len, const unsigned char *blob, size_t blob_len,
                unsigned char *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif
