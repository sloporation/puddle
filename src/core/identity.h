#ifndef PUDDLE_IDENTITY_H
#define PUDDLE_IDENTITY_H

/* This Puddle's identity: one Ed25519 signing key pair and one X25519 encryption key pair, a stable
 * user id, and a signed chain of records that lets the keys be replaced ("rolled") without changing
 * the id. See docs/identity.md and docs/streams.md.
 *
 * Files in <data_dir>/identity/:
 *   keys         the private keys (mode 0600): current and the pre-committed next pair
 *   chain.jsonl  append-only signed records, one per line */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_USER_ID_LEN 52 /* base32 of a SHA-256 */

typedef struct puddle_identity puddle_identity;

/* Load the identity, creating it (and its first record) on first run. Interrupted rolls are finished.
 * NULL + message in `err` on failure, including a chain whose keys file is missing. */
puddle_identity *puddle_identity_open(const char *data_dir, char *err, size_t errlen);
void puddle_identity_close(puddle_identity *id);

/* The user id: base32(SHA-256(first signing public key)). It never changes, however often keys roll. */
void puddle_identity_id(puddle_identity *id, char out[PUDDLE_USER_ID_LEN + 1]);

/* Current public keys and how many times they have rolled (0 = never). Any pointer may be NULL. */
void puddle_identity_public(puddle_identity *id, unsigned char sign_pub[32], unsigned char enc_pub[32],
                            uint64_t *seq);

/* Ed25519 signature (64 bytes) by the current signing key. 0 or -1. */
int puddle_identity_sign(puddle_identity *id, const void *data, size_t len, unsigned char sig[64]);

/* Replace the keys: the pre-committed next pair become current, a fresh next pair is committed, and a
 * record signed by the new key is appended. Optional and never automatic; the id is unchanged.
 * Safe if interrupted: the next open completes it. 0 or -1. */
int puddle_identity_roll(puddle_identity *id, char *err, size_t errlen);

/* The whole chain as text (one record per line), for sending to a peer. malloc'd; NULL on error. */
char *puddle_identity_chain(puddle_identity *id, size_t *len);

/* ---- other people's identities ---- */

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    uint64_t seq;
    unsigned char sign_pub[32];  /* their current keys */
    unsigned char enc_pub[32];
    char next[65];               /* commitment to their next keys, hex */
    char head_hash[65];          /* SHA-256 of the last record, hex */
} puddle_chain_head;

/* Check a chain someone sent: the first record is signed by the key the id is the hash of, and every
 * later record is signed by the key the previous record committed to. If `expected_id` is non-NULL
 * the chain must belong to it (this is how a link's id pins a person's key). On success the current
 * keys are in `out`. 0 or -1 with the reason in `err`. */
int puddle_chain_verify(const char *text, size_t len, const char *expected_id, puddle_chain_head *out,
                        char *err, size_t errlen);

/* Verify an Ed25519 signature. 1 valid, 0 not. */
int puddle_verify_signature(const unsigned char sign_pub[32], const void *data, size_t len,
                            const unsigned char sig[64]);

/* Short stable name for a signing key: hex SHA-256 of the public key. Requests say which key signed
 * them with this. */
void puddle_key_id(const unsigned char sign_pub[32], char out[65]);

#ifdef __cplusplus
}
#endif
#endif
