#include "identity.h"

#include "config.h"
#include "encoding.h"
#include "fsutil.h"
#include "hash.h"
#include "json.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>

#define MAX_CHAIN_BYTES (4u << 20)
#define MAX_CHAIN_RECORDS 10000

typedef struct {
    unsigned char priv[32];
    unsigned char pub[32];
} keypair;

struct puddle_identity {
    char dir[PUDDLE_PATH_MAX];
    char keys_path[PUDDLE_PATH_MAX];
    char chain_path[PUDDLE_PATH_MAX];
    pthread_mutex_t mu;
    keypair sign, enc;           /* current */
    keypair next_sign, next_enc; /* committed to by the head record */
    char id[PUDDLE_USER_ID_LEN + 1];
    uint64_t seq;
    char head_hash[65];
};

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ crypto */

static int gen_keypair(const char *alg, keypair *kp)
{
    EVP_PKEY *pk = EVP_PKEY_Q_keygen(NULL, NULL, alg);
    if (!pk)
        return -1;
    size_t plen = 32, qlen = 32;
    int ok = EVP_PKEY_get_raw_private_key(pk, kp->priv, &plen) == 1 &&
             EVP_PKEY_get_raw_public_key(pk, kp->pub, &qlen) == 1 && plen == 32 && qlen == 32;
    EVP_PKEY_free(pk);
    return ok ? 0 : -1;
}

/* rebuild a key pair from its private half */
static int keypair_from_priv(int type, const unsigned char priv[32], keypair *kp)
{
    EVP_PKEY *pk = EVP_PKEY_new_raw_private_key(type, NULL, priv, 32);
    if (!pk)
        return -1;
    size_t qlen = 32;
    int ok = EVP_PKEY_get_raw_public_key(pk, kp->pub, &qlen) == 1 && qlen == 32;
    EVP_PKEY_free(pk);
    memcpy(kp->priv, priv, 32);
    return ok ? 0 : -1;
}

static int sign_with(const keypair *kp, const void *data, size_t len, unsigned char sig[64])
{
    EVP_PKEY *pk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, kp->priv, 32);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    size_t siglen = 64;
    int ok = pk && ctx && EVP_DigestSignInit(ctx, NULL, NULL, NULL, pk) == 1 &&
             EVP_DigestSign(ctx, sig, &siglen, data, len) == 1 && siglen == 64;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    return ok ? 0 : -1;
}

int puddle_verify_signature(const unsigned char sign_pub[32], const void *data, size_t len,
                            const unsigned char sig[64])
{
    EVP_PKEY *pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, sign_pub, 32);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int ok = pk && ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pk) == 1 &&
             EVP_DigestVerify(ctx, sig, 64, data, len) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    return ok;
}

void puddle_key_id(const unsigned char sign_pub[32], char out[65])
{
    puddle_sha256_hex(sign_pub, 32, out);
}

/* what a record commits to: SHA-256(signing public || encryption public), hex */
static void commitment(const unsigned char sign_pub[32], const unsigned char enc_pub[32], char out[65])
{
    unsigned char both[64];
    memcpy(both, sign_pub, 32);
    memcpy(both + 32, enc_pub, 32);
    puddle_sha256_hex(both, sizeof both, out);
}

static void user_id_of(const unsigned char first_sign_pub[32], char out[PUDDLE_USER_ID_LEN + 1])
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned mdlen = 0;
    EVP_Digest(first_sign_pub, 32, md, &mdlen, EVP_sha256(), NULL);
    puddle_base32_encode(md, mdlen, out, PUDDLE_USER_ID_LEN + 1);
}

/* ------------------------------------------------------------------ records */

/* One chain line: {"record":{...},"sig":"<b64>"}\n, the signature over the exact record bytes.
 * Returns the line length, or 0. */
static size_t build_line(char *out, size_t cap, uint64_t seq, const char *prev_hash, const keypair *sign,
                         const keypair *enc, const char next_commit[65], char head_hash_out[65])
{
    char sp[64], ep[64];
    puddle_base64_encode(sign->pub, 32, sp, sizeof sp);
    puddle_base64_encode(enc->pub, 32, ep, sizeof ep);

    char rec[512];
    int n;
    if (seq == 0)
        n = snprintf(rec, sizeof rec,
                     "{\"type\":\"identity.inception\",\"seq\":0,\"signing\":\"%s\",\"encryption\":\"%s\",\"next\":\"%s\",\"ts\":%lld}",
                     sp, ep, next_commit, (long long)now_ms());
    else
        n = snprintf(rec, sizeof rec,
                     "{\"type\":\"identity.rotate\",\"seq\":%llu,\"prev\":\"%s\",\"signing\":\"%s\",\"encryption\":\"%s\",\"next\":\"%s\",\"ts\":%lld}",
                     (unsigned long long)seq, prev_hash, sp, ep, next_commit, (long long)now_ms());
    if (n <= 0 || (size_t)n >= sizeof rec)
        return 0;

    unsigned char sig[64];
    char sig64[100];
    if (sign_with(sign, rec, (size_t)n, sig) != 0 || !puddle_base64_encode(sig, 64, sig64, sizeof sig64))
        return 0;
    puddle_sha256_hex(rec, (size_t)n, head_hash_out);
    int w = snprintf(out, cap, "{\"record\":%s,\"sig\":\"%s\"}\n", rec, sig64);
    return (w > 0 && (size_t)w < cap) ? (size_t)w : 0;
}

static int append_line(const char *path, const char *dir, const char *line, size_t len)
{
    struct stat st;
    int created = stat(path, &st) != 0;
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    off_t before = lseek(fd, 0, SEEK_END);
    size_t left = len;
    const char *p = line;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            if (before >= 0 && ftruncate(fd, before) != 0) {}
            close(fd);
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    int rc = puddle_fsync_fd(fd);
    close(fd);
    if (rc == 0 && created)
        puddle_fsync_dir(dir);
    return rc;
}

/* ------------------------------------------------------------- chain verify */

static int cfail(char *err, size_t errlen, size_t line, const char *what)
{
    seterr(err, errlen, "identity chain record %zu: %s", line, what);
    return -1;
}

/* a base64 field of exactly 32 bytes from a record */
static int get_key(const char *rec, size_t len, const char *name, unsigned char out[32])
{
    puddle_span v;
    char text[64];
    size_t n = 0;
    unsigned char tmp[40];
    if (puddle_json_member(rec, len, name, &v) != 0 || puddle_json_string(v, text, sizeof text) != 0 ||
        puddle_base64_decode(text, tmp, sizeof tmp, &n) != 0 || n != 32)
        return -1;
    memcpy(out, tmp, 32);
    return 0;
}

static int get_hex64(const char *rec, size_t len, const char *name, char out[65])
{
    puddle_span v;
    unsigned char raw[32];
    if (puddle_json_member(rec, len, name, &v) != 0 || puddle_json_string(v, out, 65) != 0 ||
        puddle_hex_decode(out, raw, 32) != 0)
        return -1;
    return 0;
}

int puddle_chain_verify(const char *text, size_t len, const char *expected_id, puddle_chain_head *out,
                        char *err, size_t errlen)
{
    if (len == 0 || len > MAX_CHAIN_BYTES)
        return cfail(err, errlen, 0, "empty or too large");

    puddle_chain_head h;
    memset(&h, 0, sizeof h);
    size_t count = 0, pos = 0;

    while (pos < len) {
        const char *line = text + pos;
        const char *nl = memchr(line, '\n', len - pos);
        size_t ll = nl ? (size_t)(nl - line) : len - pos;
        pos += ll + (nl ? 1 : 0);
        if (ll == 0)
            continue;
        if (++count > MAX_CHAIN_RECORDS)
            return cfail(err, errlen, count, "too many records");

        puddle_span rv, sv;
        char sig64[100], type[32];
        unsigned char sig[64], tmp[72];
        size_t siglen = 0;
        int64_t seq;
        if (!puddle_json_valid_object(line, ll) || puddle_json_member(line, ll, "record", &rv) != 0 ||
            rv.len == 0 || rv.p[0] != '{' || puddle_json_member(line, ll, "sig", &sv) != 0 ||
            puddle_json_string(sv, sig64, sizeof sig64) != 0 ||
            puddle_base64_decode(sig64, tmp, sizeof tmp, &siglen) != 0 || siglen != 64)
            return cfail(err, errlen, count, "malformed");
        memcpy(sig, tmp, 64);

        const char *rec = rv.p;
        size_t rl = rv.len;
        puddle_span t;
        if (puddle_json_member(rec, rl, "type", &t) != 0 || puddle_json_string(t, type, sizeof type) != 0 ||
            puddle_json_member(rec, rl, "seq", &t) != 0 || puddle_json_int64(t, &seq) != 0)
            return cfail(err, errlen, count, "missing type or seq");

        unsigned char sp[32], ep[32];
        char next[65];
        if (get_key(rec, rl, "signing", sp) != 0 || get_key(rec, rl, "encryption", ep) != 0 ||
            get_hex64(rec, rl, "next", next) != 0)
            return cfail(err, errlen, count, "bad keys or commitment");

        if (count == 1) {
            if (strcmp(type, "identity.inception") != 0 || seq != 0)
                return cfail(err, errlen, count, "the chain must start with an inception record");
            user_id_of(sp, h.id);
            if (expected_id && strcmp(expected_id, h.id) != 0)
                return cfail(err, errlen, count, "this chain belongs to a different id");
        } else {
            char prev[65], want[65];
            if (strcmp(type, "identity.rotate") != 0 || seq < 0 || (uint64_t)seq != h.seq + 1)
                return cfail(err, errlen, count, "out of order");
            if (get_hex64(rec, rl, "prev", prev) != 0 || strcmp(prev, h.head_hash) != 0)
                return cfail(err, errlen, count, "does not follow the previous record");
            commitment(sp, ep, want);
            if (strcmp(want, h.next) != 0)
                return cfail(err, errlen, count, "keys are not the ones the previous record committed to");
        }
        /* every record is signed by the signing key it announces */
        if (!puddle_verify_signature(sp, rec, rl, sig))
            return cfail(err, errlen, count, "bad signature");

        h.seq = (uint64_t)seq;
        memcpy(h.sign_pub, sp, 32);
        memcpy(h.enc_pub, ep, 32);
        memcpy(h.next, next, 65);
        puddle_sha256_hex(rec, rl, h.head_hash);
    }
    if (count == 0)
        return cfail(err, errlen, 0, "empty");
    *out = h;
    return 0;
}

/* ------------------------------------------------------------ keys file */

static int write_keys(const puddle_identity *id)
{
    char hs[4][65];
    puddle_hex_encode(id->sign.priv, 32, hs[0]);
    puddle_hex_encode(id->enc.priv, 32, hs[1]);
    puddle_hex_encode(id->next_sign.priv, 32, hs[2]);
    puddle_hex_encode(id->next_enc.priv, 32, hs[3]);
    char buf[512];
    int n = snprintf(buf, sizeof buf, "puddle-keys 1\nid %s\nsigning %s\nencryption %s\nnext_signing %s\nnext_encryption %s\n",
                     id->id, hs[0], hs[1], hs[2], hs[3]);
    int rc = puddle_write_file_atomic(id->dir, id->keys_path, buf, (size_t)n);
    memset(buf, 0, sizeof buf);
    memset(hs, 0, sizeof hs);
    return rc;
}

/* 0 loaded, 1 file missing, -1 bad */
static int read_keys(puddle_identity *id, char *err, size_t errlen)
{
    FILE *f = fopen(id->keys_path, "r");
    if (!f)
        return errno == ENOENT ? 1 : (seterr(err, errlen, "cannot read %s: %s", id->keys_path, strerror(errno)), -1);
    char line[256], name[32], hex[80];
    unsigned char sign[32], enc[32], nsign[32], nenc[32];
    int have = 0, header = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "puddle-keys 1", 13) == 0) {
            header = 1;
        } else if (sscanf(line, "%31s %79s", name, hex) == 2) {
            if (strcmp(name, "id") == 0 && strlen(hex) == PUDDLE_USER_ID_LEN) {
                memcpy(id->id, hex, PUDDLE_USER_ID_LEN + 1);
                continue;
            }
            unsigned char *dst = strcmp(name, "signing") == 0 ? sign : strcmp(name, "encryption") == 0 ? enc :
                                 strcmp(name, "next_signing") == 0 ? nsign : strcmp(name, "next_encryption") == 0 ? nenc : NULL;
            if (dst && puddle_hex_decode(hex, dst, 32) == 0)
                have++;
        }
    }
    fclose(f);
    memset(hex, 0, sizeof hex);
    if (!header || have != 4 ||
        keypair_from_priv(EVP_PKEY_ED25519, sign, &id->sign) != 0 || keypair_from_priv(EVP_PKEY_X25519, enc, &id->enc) != 0 ||
        keypair_from_priv(EVP_PKEY_ED25519, nsign, &id->next_sign) != 0 || keypair_from_priv(EVP_PKEY_X25519, nenc, &id->next_enc) != 0) {
        seterr(err, errlen, "%s is damaged; restore it from a backup", id->keys_path);
        return -1;
    }
    return 0;
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = malloc(MAX_CHAIN_BYTES + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, MAX_CHAIN_BYTES + 1, f);
    fclose(f);
    if (n > MAX_CHAIN_BYTES) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* ------------------------------------------------------------------- open */

static int append_record(puddle_identity *id, uint64_t seq, const char *prev_hash, char *err, size_t errlen)
{
    char nc[65], line[1024], head[65];
    commitment(id->next_sign.pub, id->next_enc.pub, nc);
    size_t ll = build_line(line, sizeof line, seq, prev_hash, &id->sign, &id->enc, nc, head);
    if (!ll || append_line(id->chain_path, id->dir, line, ll) != 0) {
        seterr(err, errlen, "cannot write %s: %s", id->chain_path, strerror(errno));
        return -1;
    }
    id->seq = seq;
    memcpy(id->head_hash, head, 65);
    return 0;
}

puddle_identity *puddle_identity_open(const char *data_dir, char *err, size_t errlen)
{
    puddle_identity *id = calloc(1, sizeof *id);
    if (!id) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    pthread_mutex_init(&id->mu, NULL);
    snprintf(id->dir, sizeof id->dir, "%s/identity", data_dir);
    snprintf(id->keys_path, sizeof id->keys_path, "%s/keys", id->dir);
    snprintf(id->chain_path, sizeof id->chain_path, "%s/chain.jsonl", id->dir);
    if (puddle_mkdir_p(id->dir) != 0) {
        seterr(err, errlen, "cannot create %s: %s", id->dir, strerror(errno));
        goto fail;
    }

    int kr = read_keys(id, err, errlen);
    if (kr < 0)
        goto fail;

    size_t clen = 0;
    char *chain = read_all(id->chain_path, &clen);
    int have_chain = chain && clen > 0;

    if (kr == 1) { /* no keys */
        if (have_chain) {
            free(chain);
            seterr(err, errlen, "the identity chain exists but %s is missing; restore it from a backup "
                   "(a new identity would not be you)", id->keys_path);
            goto fail;
        }
        if (gen_keypair("ED25519", &id->sign) || gen_keypair("X25519", &id->enc) ||
            gen_keypair("ED25519", &id->next_sign) || gen_keypair("X25519", &id->next_enc) ||
            (user_id_of(id->sign.pub, id->id), write_keys(id) != 0)) {
            free(chain);
            seterr(err, errlen, "cannot create keys in %s", id->dir);
            goto fail;
        }
        puddle_log(PUDDLE_LOG_INFO, "created a new identity");
    }

    if (!have_chain) { /* first run, or the chain was lost but the keys survived: start it from the keys */
        free(chain);
        char first[PUDDLE_USER_ID_LEN + 1];
        user_id_of(id->sign.pub, first);
        if (id->id[0] && strcmp(id->id, first) != 0) { /* the keys have rolled since inception */
            seterr(err, errlen, "%s is missing or empty but the keys have rolled since they were created; "
                   "restore it from a backup", id->chain_path);
            goto fail;
        }
        memcpy(id->id, first, sizeof first);
        if (write_keys(id) != 0 || append_record(id, 0, NULL, err, errlen) != 0)
            goto fail;
        return id;
    }

    puddle_chain_head h;
    char why[200];
    if (puddle_chain_verify(chain, clen, NULL, &h, why, sizeof why) != 0) {
        free(chain);
        seterr(err, errlen, "%s: %s", id->chain_path, why);
        goto fail;
    }
    free(chain);
    if (id->id[0] && strcmp(id->id, h.id) != 0) {
        seterr(err, errlen, "%s and %s belong to different identities", id->keys_path, id->chain_path);
        goto fail;
    }
    memcpy(id->id, h.id, sizeof id->id);
    id->seq = h.seq;
    memcpy(id->head_hash, h.head_hash, 65);

    char cur[65], nxt[65];
    commitment(id->next_sign.pub, id->next_enc.pub, nxt);
    if (memcmp(h.sign_pub, id->sign.pub, 32) == 0 && memcmp(h.enc_pub, id->enc.pub, 32) == 0 && strcmp(h.next, nxt) == 0)
        return id; /* everything agrees */

    commitment(id->sign.pub, id->enc.pub, cur);
    if (strcmp(h.next, cur) == 0) { /* a roll was interrupted after the keys were swapped: finish it */
        puddle_log(PUDDLE_LOG_WARN, "finishing an interrupted key roll");
        if (append_record(id, h.seq + 1, h.head_hash, err, errlen) != 0)
            goto fail;
        return id;
    }
    seterr(err, errlen, "%s does not match %s; restore both from the same backup", id->keys_path, id->chain_path);
fail:
    puddle_identity_close(id);
    return NULL;
}

void puddle_identity_close(puddle_identity *id)
{
    if (!id)
        return;
    memset(&id->sign, 0, sizeof id->sign);
    memset(&id->enc, 0, sizeof id->enc);
    memset(&id->next_sign, 0, sizeof id->next_sign);
    memset(&id->next_enc, 0, sizeof id->next_enc);
    pthread_mutex_destroy(&id->mu);
    free(id);
}

void puddle_identity_id(puddle_identity *id, char out[PUDDLE_USER_ID_LEN + 1])
{
    memcpy(out, id->id, PUDDLE_USER_ID_LEN + 1);
}

void puddle_identity_public(puddle_identity *id, unsigned char sign_pub[32], unsigned char enc_pub[32],
                            uint64_t *seq)
{
    pthread_mutex_lock(&id->mu);
    if (sign_pub) memcpy(sign_pub, id->sign.pub, 32);
    if (enc_pub) memcpy(enc_pub, id->enc.pub, 32);
    if (seq) *seq = id->seq;
    pthread_mutex_unlock(&id->mu);
}

int puddle_identity_sign(puddle_identity *id, const void *data, size_t len, unsigned char sig[64])
{
    pthread_mutex_lock(&id->mu);
    int rc = sign_with(&id->sign, data, len, sig);
    pthread_mutex_unlock(&id->mu);
    return rc;
}

int puddle_identity_roll(puddle_identity *id, char *err, size_t errlen)
{
    int rc = -1;
    pthread_mutex_lock(&id->mu);

    keypair old_sign = id->sign, old_enc = id->enc, old_ns = id->next_sign, old_ne = id->next_enc;
    char prev[65];
    memcpy(prev, id->head_hash, 65);
    uint64_t seq = id->seq;

    /* 1. promote the committed keys and make a new next pair; keys file first, atomically */
    id->sign = old_ns;
    id->enc = old_ne;
    if (gen_keypair("ED25519", &id->next_sign) != 0 || gen_keypair("X25519", &id->next_enc) != 0) {
        seterr(err, errlen, "cannot generate keys");
        goto undo;
    }
    if (write_keys(id) != 0) {
        seterr(err, errlen, "cannot write %s: %s", id->keys_path, strerror(errno));
        goto undo;
    }
    /* 2. then the record. If this fails the keys file is already ahead of the chain, which the next
     *    open repairs; report it, but the roll has happened. */
    if (append_record(id, seq + 1, prev, err, errlen) != 0) {
        seterr(err, errlen, "keys rolled but the record could not be written (%s); it will be completed next start",
               strerror(errno));
        rc = -1;
        goto out;
    }
    rc = 0;
    goto out;
undo:
    id->sign = old_sign;
    id->enc = old_enc;
    id->next_sign = old_ns;
    id->next_enc = old_ne;
out:
    memset(&old_sign, 0, sizeof old_sign);
    memset(&old_enc, 0, sizeof old_enc);
    pthread_mutex_unlock(&id->mu);
    return rc;
}

char *puddle_identity_chain(puddle_identity *id, size_t *len)
{
    pthread_mutex_lock(&id->mu);
    char *text = read_all(id->chain_path, len);
    pthread_mutex_unlock(&id->mu);
    return text;
}
