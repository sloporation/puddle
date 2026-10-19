#include "seal.h"

#include "hash.h"

#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#define VERSION 1
#define DOMAIN "puddle-seal-v1"
#define HEADER_FIXED (1 + 32 + 12 + 1)
#define ENTRY 56 /* tag(8) + wrapped key(32) + AEAD tag(16) */

static void key_tag(const unsigned char pub[32], unsigned char tag[8])
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned mdlen = 0;
    EVP_Digest(pub, 32, md, &mdlen, EVP_sha256(), NULL);
    memcpy(tag, md, 8);
}

/* ChaCha20-Poly1305 over `in`, tag appended (encrypt) or checked (decrypt). The key is never used twice with one
 * nonce: a wrap key is unique to (ephemeral, recipient) and used once; the body key is random per blob. */
static int aead(int enc, const unsigned char key[32], const unsigned char nonce[12], const unsigned char *aad,
                size_t aad_len, const unsigned char *in, size_t in_len, unsigned char *out)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int ok = 0, n = 0;
    if (!c)
        return -1;
    if (EVP_CipherInit_ex(c, EVP_chacha20_poly1305(), NULL, NULL, NULL, enc) != 1 ||
        EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) != 1 || EVP_CipherInit_ex(c, NULL, NULL, key, nonce, enc) != 1)
        goto done;
    if (enc) {
        if (EVP_EncryptUpdate(c, NULL, &n, aad, (int)aad_len) != 1)
            goto done;
        if (in_len && EVP_EncryptUpdate(c, out, &n, in, (int)in_len) != 1)
            goto done;
        if (EVP_EncryptFinal_ex(c, out + in_len, &n) != 1 ||
            EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, out + in_len) != 1)
            goto done;
    } else {
        if (in_len < 16)
            goto done;
        size_t plain = in_len - 16;
        if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, 16, (void *)(in + plain)) != 1 ||
            EVP_DecryptUpdate(c, NULL, &n, aad, (int)aad_len) != 1)
            goto done;
        if (plain && EVP_DecryptUpdate(c, out, &n, in, (int)plain) != 1)
            goto done;
        if (EVP_DecryptFinal_ex(c, out + plain, &n) != 1)
            goto done;
    }
    ok = 1;
done:
    EVP_CIPHER_CTX_free(c);
    return ok ? 0 : -1;
}

/* The wrap key: HKDF of the DH secret, with both public keys as context. */
static int wrap_key(const unsigned char shared[32], const unsigned char epk[32], const unsigned char rpub[32],
                    unsigned char kek[32])
{
    char info[sizeof DOMAIN + 128];
    unsigned char ctx[64];
    memcpy(ctx, epk, 32);
    memcpy(ctx + 32, rpub, 32);
    /* the info string must be text for the KDF helper: hex of the two keys after the label */
    memcpy(info, DOMAIN, sizeof DOMAIN - 1);
    puddle_hex_encode(ctx, 64, info + sizeof DOMAIN - 1);
    return puddle_hkdf_sha256(shared, 32, DOMAIN, info, kek, 32);
}

/* What the AEADs authenticate besides their own text: the header up to the key entries' end, and the caller's context. */
static unsigned char *build_aad(const unsigned char *header, size_t header_len, const void *aad, size_t aad_len, size_t *out_len)
{
    unsigned char *a = malloc(sizeof DOMAIN + header_len + aad_len);
    if (!a)
        return NULL;
    memcpy(a, DOMAIN, sizeof DOMAIN - 1);
    memcpy(a + sizeof DOMAIN - 1, header, header_len);
    if (aad_len)
        memcpy(a + sizeof DOMAIN - 1 + header_len, aad, aad_len);
    *out_len = sizeof DOMAIN - 1 + header_len + aad_len;
    return a;
}

int puddle_seal(const unsigned char (*recipients)[32], size_t n, const void *aad, size_t aad_len, const void *plain,
                size_t plain_len, unsigned char *out, size_t cap, size_t *out_len)
{
    if (!recipients || n < 1 || n > PUDDLE_SEAL_MAX_RECIPIENTS || (plain_len && !plain) || (aad_len && !aad) ||
        plain_len > (1u << 20) || aad_len > (1u << 16))
        return -1;
    size_t header_len = HEADER_FIXED + n * ENTRY, total = header_len + plain_len + 16;
    if (cap < total || !out)
        return -1;

    unsigned char cek[32], shared[32], kek[32], zero[12] = { 0 }, esk[32], epk[32];
    unsigned char *wrapaad = NULL, *bodyaad = NULL;
    size_t wrapaad_len = 0, bodyaad_len = 0;
    int rc = -1;
    size_t kl = 32;
    EVP_PKEY *eph = NULL, *peer = NULL;
    EVP_PKEY_CTX *kg = NULL, *dh = NULL;

    kg = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    if (!kg || EVP_PKEY_keygen_init(kg) != 1 || EVP_PKEY_keygen(kg, &eph) != 1)
        goto done;
    size_t sl = 32;
    if (EVP_PKEY_get_raw_private_key(eph, esk, &sl) != 1 || EVP_PKEY_get_raw_public_key(eph, epk, &kl) != 1)
        goto done;
    if (RAND_bytes(cek, 32) != 1 || RAND_bytes(out + 33, 12) != 1)
        goto done;

    out[0] = VERSION;
    memcpy(out + 1, epk, 32);
    /* nonce already at out+33 */
    out[45] = (unsigned char)n;

    /* the wrap's context is fixed before the entries exist: ephemeral key, nonce and the caller's context */
    wrapaad = build_aad(out, 45, aad, aad_len, &wrapaad_len);
    if (!wrapaad)
        goto done;
    for (size_t i = 0; i < n; i++) {
        unsigned char *e = out + HEADER_FIXED + i * ENTRY;
        peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, recipients[i], 32);
        dh = peer ? EVP_PKEY_CTX_new(eph, NULL) : NULL;
        size_t dl = 32;
        if (!dh || EVP_PKEY_derive_init(dh) != 1 || EVP_PKEY_derive_set_peer(dh, peer) != 1 ||
            EVP_PKEY_derive(dh, shared, &dl) != 1 || dl != 32)
            goto done;
        EVP_PKEY_CTX_free(dh);
        EVP_PKEY_free(peer);
        dh = NULL;
        peer = NULL;
        if (wrap_key(shared, epk, recipients[i], kek) != 0)
            goto done;
        key_tag(recipients[i], e);
        if (aead(1, kek, zero, wrapaad, wrapaad_len, cek, 32, e + 8) != 0)
            goto done;
    }
    bodyaad = build_aad(out, header_len, aad, aad_len, &bodyaad_len);
    if (!bodyaad || aead(1, cek, out + 33, bodyaad, bodyaad_len, plain, plain_len, out + header_len) != 0)
        goto done;
    *out_len = total;
    rc = 0;
done:
    EVP_PKEY_CTX_free(dh);
    EVP_PKEY_free(peer);
    EVP_PKEY_CTX_free(kg);
    EVP_PKEY_free(eph);
    OPENSSL_cleanse(cek, sizeof cek);
    OPENSSL_cleanse(shared, sizeof shared);
    OPENSSL_cleanse(kek, sizeof kek);
    OPENSSL_cleanse(esk, sizeof esk);
    free(wrapaad);
    free(bodyaad);
    return rc;
}

int puddle_open(puddle_identity *me, const void *aad, size_t aad_len, const unsigned char *blob, size_t blob_len,
                unsigned char *out, size_t cap, size_t *out_len)
{
    if (!me || !blob || (aad_len && !aad) || aad_len > (1u << 16) || blob_len < HEADER_FIXED + ENTRY + 16 || blob[0] != VERSION)
        return -1;
    size_t n = blob[45];
    if (n < 1 || n > PUDDLE_SEAL_MAX_RECIPIENTS)
        return -1;
    size_t header_len = HEADER_FIXED + n * ENTRY;
    if (blob_len < header_len + 16)
        return -1;
    size_t plain_len = blob_len - header_len - 16;
    if (cap < plain_len)
        return -1;

    unsigned char mypub[32], tag[8], shared[32], kek[32], cek[32], zero[12] = { 0 };
    puddle_identity_public(me, NULL, mypub, NULL);
    key_tag(mypub, tag);
    const unsigned char *epk = blob + 1;

    unsigned char *wrapaad = NULL, *bodyaad = NULL;
    size_t wrapaad_len = 0, bodyaad_len = 0;
    int rc = -1, have_cek = 0;
    wrapaad = build_aad(blob, 45, aad, aad_len, &wrapaad_len);
    if (!wrapaad)
        goto done;
    if (puddle_identity_dh(me, epk, shared) != 0 || wrap_key(shared, epk, mypub, kek) != 0)
        goto done;
    for (size_t i = 0; i < n && !have_cek; i++) {
        const unsigned char *e = blob + HEADER_FIXED + i * ENTRY;
        if (memcmp(e, tag, 8) == 0 && aead(0, kek, zero, wrapaad, wrapaad_len, e + 8, 48, cek) == 0)
            have_cek = 1;
    }
    if (!have_cek)
        goto done;
    bodyaad = build_aad(blob, header_len, aad, aad_len, &bodyaad_len);
    if (!bodyaad || aead(0, cek, blob + 33, bodyaad, bodyaad_len, blob + header_len, plain_len + 16, out) != 0)
        goto done;
    *out_len = plain_len;
    rc = 0;
done:
    OPENSSL_cleanse(cek, sizeof cek);
    OPENSSL_cleanse(shared, sizeof shared);
    OPENSSL_cleanse(kek, sizeof kek);
    free(wrapaad);
    free(bodyaad);
    return rc;
}
