#include "identity.h"

#include "encoding.h"
#include "hash.h"
#include "json.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];

static char *dir_for(const char *name)
{
    static char p[600];
    snprintf(p, sizeof p, "%s/%s", root, name);
    return p;
}

static char *file_text(const char *dir, const char *name, size_t *len)
{
    char path[700];
    snprintf(path, sizeof path, "%s/identity/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(1 << 16);
    *len = fread(buf, 1, (1 << 16) - 1, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

static void write_text(const char *dir, const char *name, const char *text, size_t len)
{
    char path[700];
    snprintf(path, sizeof path, "%s/identity/%s", dir, name);
    FILE *f = fopen(path, "wb");
    fwrite(text, 1, len, f);
    fclose(f);
}

static size_t lines(const char *t)
{
    size_t n = 0;
    for (; *t; t++) if (*t == '\n') n++;
    return n;
}

/* verify a chain given as text; returns 0 or -1 and fills err */
static int verify(const char *text, const char *expect_id, puddle_chain_head *h, char *err)
{
    return puddle_chain_verify(text, strlen(text), expect_id, h, err, 200);
}

static void test_create_and_reopen(void)
{
    char err[200], id1[53], id2[53];
    unsigned char sp[32], ep[32], sp2[32], ep2[32];
    uint64_t seq;

    puddle_identity *a = puddle_identity_open(dir_for("a"), err, sizeof err);
    CHECK(a != NULL);
    puddle_identity_id(a, id1);
    CHECK(strlen(id1) == 52);
    for (int i = 0; i < 52; i++) CHECK((id1[i] >= 'a' && id1[i] <= 'z') || (id1[i] >= '2' && id1[i] <= '7'));
    puddle_identity_public(a, sp, ep, &seq);
    CHECK(seq == 0);

    /* the id is the hash of the first signing key */
    char want[53];
    unsigned char md[32]; char hex[65];
    puddle_sha256_hex(sp, 32, hex); puddle_hex_decode(hex, md, 32);
    puddle_base32_encode(md, 32, want, sizeof want);
    CHECK(strcmp(id1, want) == 0);

    struct stat st;
    char path[700];
    snprintf(path, sizeof path, "%s/identity/keys", dir_for("a"));
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

    size_t cl;
    char *chain = puddle_identity_chain(a, &cl);
    puddle_chain_head h;
    char verr[200];
    CHECK(chain && puddle_chain_verify(chain, cl, id1, &h, verr, sizeof verr) == 0);
    CHECK(h.seq == 0 && memcmp(h.sign_pub, sp, 32) == 0 && memcmp(h.enc_pub, ep, 32) == 0);
    free(chain);

    unsigned char sig[64];
    CHECK(puddle_identity_sign(a, "hello", 5, sig) == 0);
    CHECK(puddle_verify_signature(sp, "hello", 5, sig));
    CHECK(!puddle_verify_signature(sp, "hellp", 5, sig));
    sig[0] ^= 1;
    CHECK(!puddle_verify_signature(sp, "hello", 5, sig));
    puddle_identity_close(a);

    a = puddle_identity_open(dir_for("a"), err, sizeof err); /* same identity next time */
    CHECK(a != NULL);
    puddle_identity_id(a, id2);
    puddle_identity_public(a, sp2, ep2, &seq);
    CHECK(strcmp(id1, id2) == 0 && memcmp(sp, sp2, 32) == 0 && memcmp(ep, ep2, 32) == 0 && seq == 0);
    puddle_identity_close(a);

    puddle_identity *b = puddle_identity_open(dir_for("b"), err, sizeof err); /* another Puddle, another id */
    puddle_identity_id(b, id2);
    CHECK(strcmp(id1, id2) != 0);
    puddle_identity_close(b);
}

static void test_roll(void)
{
    char err[200], id0[53], id1[53];
    unsigned char sp0[32], ep0[32], sp1[32], ep1[32], sig_old[64], sig_new[64];
    uint64_t seq;
    puddle_identity *a = puddle_identity_open(dir_for("roll"), err, sizeof err);
    puddle_identity_id(a, id0);
    puddle_identity_public(a, sp0, ep0, &seq);
    CHECK(puddle_identity_sign(a, "before", 6, sig_old) == 0);

    CHECK(puddle_identity_roll(a, err, sizeof err) == 0);
    puddle_identity_id(a, id1);
    puddle_identity_public(a, sp1, ep1, &seq);
    CHECK(strcmp(id0, id1) == 0);                    /* the id survives */
    CHECK(seq == 1);
    CHECK(memcmp(sp0, sp1, 32) != 0 && memcmp(ep0, ep1, 32) != 0);
    CHECK(puddle_identity_sign(a, "after", 5, sig_new) == 0);
    CHECK(puddle_verify_signature(sp1, "after", 5, sig_new));
    CHECK(!puddle_verify_signature(sp0, "after", 5, sig_new));   /* old key no longer signs for us */
    CHECK(puddle_verify_signature(sp0, "before", 6, sig_old));   /* but what it signed still verifies */

    size_t cl;
    char *chain = puddle_identity_chain(a, &cl);
    puddle_chain_head h;
    char verr[200];
    CHECK(puddle_chain_verify(chain, cl, id0, &h, verr, sizeof verr) == 0);
    CHECK(h.seq == 1 && memcmp(h.sign_pub, sp1, 32) == 0 && memcmp(h.enc_pub, ep1, 32) == 0);
    CHECK(lines(chain) == 2);
    free(chain);

    CHECK(puddle_identity_roll(a, err, sizeof err) == 0);
    CHECK(puddle_identity_roll(a, err, sizeof err) == 0);
    puddle_identity_public(a, NULL, NULL, &seq);
    CHECK(seq == 3);
    puddle_identity_close(a);

    a = puddle_identity_open(dir_for("roll"), err, sizeof err); /* reopens cleanly after rolling */
    CHECK(a != NULL);
    puddle_identity_id(a, id1);
    puddle_identity_public(a, NULL, NULL, &seq);
    CHECK(strcmp(id0, id1) == 0 && seq == 3);
    chain = puddle_identity_chain(a, &cl);
    CHECK(puddle_chain_verify(chain, cl, id0, &h, verr, sizeof verr) == 0 && h.seq == 3);
    free(chain);
    puddle_identity_close(a);
}

/* Someone holding only the current keys (a stolen phone) tries to roll to keys of their own. */
static void test_forged_roll(void)
{
    char err[200], id[53], verr[200];
    puddle_identity *a = puddle_identity_open(dir_for("forge"), err, sizeof err);
    puddle_identity_id(a, id);
    size_t cl;
    char *chain = puddle_identity_chain(a, &cl);
    puddle_identity_close(a);

    /* the head hash they must name, and keys they generate themselves */
    puddle_span rv;
    CHECK(puddle_json_member(chain, strlen(chain), "record", &rv) == 0);
    char prev[65];
    puddle_sha256_hex(rv.p, rv.len, prev);

    EVP_PKEY *atk = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
    unsigned char priv[32], pub[32];
    size_t l1 = 32, l2 = 32;
    EVP_PKEY_get_raw_private_key(atk, priv, &l1);
    EVP_PKEY_get_raw_public_key(atk, pub, &l2);
    char pub64[64], enc64[64], rec[600], sig64[100], line[900];
    puddle_base64_encode(pub, 32, pub64, sizeof pub64);
    puddle_base64_encode(pub, 32, enc64, sizeof enc64);
    char nxt[65];
    puddle_sha256_hex(pub, 32, nxt);
    snprintf(rec, sizeof rec,
             "{\"type\":\"identity.rotate\",\"seq\":1,\"prev\":\"%s\",\"signing\":\"%s\",\"encryption\":\"%s\",\"next\":\"%s\",\"ts\":1}",
             prev, pub64, enc64, nxt);
    unsigned char sig[64];
    size_t sl = 64;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, NULL, NULL, NULL, atk);
    EVP_DigestSign(ctx, sig, &sl, (unsigned char *)rec, strlen(rec));
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(atk);
    puddle_base64_encode(sig, 64, sig64, sizeof sig64);
    snprintf(line, sizeof line, "{\"record\":%s,\"sig\":\"%s\"}\n", rec, sig64);

    char forged[4000];
    snprintf(forged, sizeof forged, "%s%s", chain, line);
    puddle_chain_head h;
    /* validly signed by the attacker's key, correctly chained, but not the keys that were committed to */
    CHECK(verify(forged, id, &h, verr) == -1 && strstr(verr, "committed to"));
    CHECK(verify(chain, id, &h, verr) == 0); /* the real chain is unaffected */
    free(chain);
}

static void test_tampering(void)
{
    char err[200], id[53], other[53], verr[200];
    puddle_identity *a = puddle_identity_open(dir_for("tamper"), err, sizeof err);
    puddle_identity_id(a, id);
    puddle_identity_roll(a, err, sizeof err);
    puddle_identity_roll(a, err, sizeof err);
    size_t cl;
    char *chain = puddle_identity_chain(a, &cl);
    puddle_identity_close(a);
    puddle_chain_head h;
    CHECK(verify(chain, id, &h, verr) == 0 && h.seq == 2);

    puddle_identity *b = puddle_identity_open(dir_for("tamper-other"), err, sizeof err);
    puddle_identity_id(b, other);
    size_t bl;
    char *bchain = puddle_identity_chain(b, &bl);
    puddle_identity_close(b);

    CHECK(verify(chain, other, &h, verr) == -1 && strstr(verr, "different id")); /* an invite pins the id */

    char t[4000];
    /* change a byte inside a record */
    snprintf(t, sizeof t, "%s", chain);
    char *p = strstr(t, "\"ts\":");
    p[5] = (p[5] == '1') ? '2' : '1';
    CHECK(verify(t, id, &h, verr) == -1 && strstr(verr, "bad signature"));

    /* damage a signature */
    snprintf(t, sizeof t, "%s", chain);
    p = strstr(t, "\"sig\":\"") + 7;
    p[3] = (p[3] == 'A') ? 'B' : 'A';
    CHECK(verify(t, id, &h, verr) == -1);

    /* drop the first record */
    const char *second = strchr(chain, '\n') + 1;
    CHECK(verify(second, NULL, &h, verr) == -1 && strstr(verr, "inception"));

    /* drop a middle record */
    const char *third = strchr(second, '\n') + 1;
    char first[1200];
    size_t fl = (size_t)(second - chain);
    memcpy(first, chain, fl); first[fl] = '\0';
    snprintf(t, sizeof t, "%s%s", first, third);
    CHECK(verify(t, id, &h, verr) == -1 && strstr(verr, "out of order"));

    /* repeat a record */
    snprintf(t, sizeof t, "%s%s", chain, second);
    CHECK(verify(t, id, &h, verr) == -1);

    /* splice another identity's first record onto this chain */
    snprintf(t, sizeof t, "%s%s", bchain, second);
    CHECK(verify(t, NULL, &h, verr) == -1 && strstr(verr, "previous"));

    /* cut off mid-line, empty, junk */
    snprintf(t, sizeof t, "%.*s", (int)(strlen(chain) - 40), chain);
    CHECK(verify(t, id, &h, verr) == -1);
    CHECK(puddle_chain_verify("", 0, NULL, &h, verr, sizeof verr) == -1);
    CHECK(verify("not json\n", NULL, &h, verr) == -1);
    CHECK(verify("{\"record\":{\"type\":\"identity.inception\"},\"sig\":\"AAAA\"}\n", NULL, &h, verr) == -1);

    free(chain);
    free(bchain);
}


/* A record signed by the correctly committed keys, in the right order, but not following the head. */
static void test_prev_link(void)
{
    char err[200], id[53], verr[200];
    puddle_identity *a = puddle_identity_open(dir_for("prev"), err, sizeof err);
    puddle_identity_id(a, id);
    size_t cl, kl;
    char *chain = puddle_identity_chain(a, &cl);
    puddle_identity_close(a);

    /* the next keys, which a real roll would use, straight from the keys file */
    char *keys = file_text(dir_for("prev"), "keys", &kl);
    char name[40], hex[80];
    unsigned char nsign[32], nenc[32];
    for (char *line = strtok(keys, "\n"); line; line = strtok(NULL, "\n"))
        if (sscanf(line, "%39s %79s", name, hex) == 2) {
            if (strcmp(name, "next_signing") == 0) puddle_hex_decode(hex, nsign, 32);
            if (strcmp(name, "next_encryption") == 0) puddle_hex_decode(hex, nenc, 32);
        }
    free(keys);
    EVP_PKEY *sk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, nsign, 32);
    EVP_PKEY *ek = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, nenc, 32);
    unsigned char spub[32], epub[32];
    size_t l = 32;
    EVP_PKEY_get_raw_public_key(sk, spub, &l);
    l = 32;
    EVP_PKEY_get_raw_public_key(ek, epub, &l);
    EVP_PKEY_free(ek);

    puddle_span rv;
    puddle_json_member(chain, strlen(chain), "record", &rv);
    char right[65], zeros[65], next[65];
    puddle_sha256_hex(rv.p, rv.len, right);
    memset(zeros, '0', 64); zeros[64] = '\0';
    puddle_sha256_hex("somewhere to roll next", 22, next);

    for (int pass = 0; pass < 2; pass++) {
        char s64[64], e64[64], rec[600], sig64[100], line[900], text[4000];
        puddle_base64_encode(spub, 32, s64, sizeof s64);
        puddle_base64_encode(epub, 32, e64, sizeof e64);
        snprintf(rec, sizeof rec,
                 "{\"type\":\"identity.rotate\",\"seq\":1,\"prev\":\"%s\",\"signing\":\"%s\",\"encryption\":\"%s\",\"next\":\"%s\",\"ts\":1}",
                 pass == 0 ? right : zeros, s64, e64, next);
        unsigned char sig[64];
        size_t sl = 64;
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, NULL, NULL, NULL, sk);
        EVP_DigestSign(ctx, sig, &sl, (unsigned char *)rec, strlen(rec));
        EVP_MD_CTX_free(ctx);
        puddle_base64_encode(sig, 64, sig64, sizeof sig64);
        snprintf(line, sizeof line, "{\"record\":%s,\"sig\":\"%s\"}\n", rec, sig64);
        snprintf(text, sizeof text, "%s%s", chain, line);
        puddle_chain_head h;
        if (pass == 0) {
            CHECK(verify(text, id, &h, verr) == 0 && h.seq == 1); /* control: the same record with the right prev is fine */
        } else {
            CHECK(verify(text, id, &h, verr) == -1 && strstr(verr, "does not follow"));
        }
    }
    EVP_PKEY_free(sk);
    free(chain);
}

static void test_recovery(void)
{
    char err[600], id0[53], id1[53], verr[200];
    uint64_t seq;
    size_t cl, kl;

    /* a roll interrupted after the keys were swapped but before the chain record: finished next time */
    puddle_identity *a = puddle_identity_open(dir_for("crash"), err, sizeof err);
    puddle_identity_id(a, id0);
    char *before = puddle_identity_chain(a, &cl);
    puddle_identity_roll(a, err, sizeof err);
    unsigned char sp_after[32], sp2[32];
    puddle_identity_public(a, sp_after, NULL, &seq);
    puddle_identity_close(a);
    write_text(dir_for("crash"), "chain.jsonl", before, cl); /* as if the append never happened */
    free(before);

    a = puddle_identity_open(dir_for("crash"), err, sizeof err);
    CHECK(a != NULL);
    puddle_identity_public(a, sp2, NULL, &seq);
    CHECK(seq == 1 && memcmp(sp_after, sp2, 32) == 0);
    puddle_identity_id(a, id1);
    CHECK(strcmp(id0, id1) == 0);
    char *chain = puddle_identity_chain(a, &cl);
    puddle_chain_head h;
    CHECK(puddle_chain_verify(chain, cl, id0, &h, verr, sizeof verr) == 0 && h.seq == 1 && lines(chain) == 2);
    free(chain);
    puddle_identity_close(a);

    /* the chain file lost, keys never rolled: it is started again from the keys, same id */
    a = puddle_identity_open(dir_for("lostchain"), err, sizeof err);
    puddle_identity_id(a, id0);
    puddle_identity_close(a);
    write_text(dir_for("lostchain"), "chain.jsonl", "", 0);
    a = puddle_identity_open(dir_for("lostchain"), err, sizeof err);
    CHECK(a != NULL);
    puddle_identity_id(a, id1);
    CHECK(strcmp(id0, id1) == 0);
    puddle_identity_close(a);

    /* the chain file lost after a roll: refuse rather than quietly become someone else */
    a = puddle_identity_open(dir_for("lostchain2"), err, sizeof err);
    puddle_identity_roll(a, err, sizeof err);
    puddle_identity_close(a);
    write_text(dir_for("lostchain2"), "chain.jsonl", "", 0);
    CHECK(puddle_identity_open(dir_for("lostchain2"), err, sizeof err) == NULL && strstr(err, "rolled"));

    /* the keys file lost: refuse, never make a new identity over an existing chain */
    a = puddle_identity_open(dir_for("lostkeys"), err, sizeof err);
    puddle_identity_close(a);
    char path[700];
    snprintf(path, sizeof path, "%s/identity/keys", dir_for("lostkeys"));
    unlink(path);
    CHECK(puddle_identity_open(dir_for("lostkeys"), err, sizeof err) == NULL && strstr(err, "missing"));

    /* damaged keys, and keys that belong to a different chain */
    a = puddle_identity_open(dir_for("damaged"), err, sizeof err);
    puddle_identity_close(a);
    write_text(dir_for("damaged"), "keys", "puddle-keys 1\nsigning 00\n", 25);
    CHECK(puddle_identity_open(dir_for("damaged"), err, sizeof err) == NULL && strstr(err, "damaged"));

    a = puddle_identity_open(dir_for("m1"), err, sizeof err); puddle_identity_close(a);
    a = puddle_identity_open(dir_for("m2"), err, sizeof err); puddle_identity_close(a);
    char *k2 = file_text(dir_for("m2"), "keys", &kl);
    write_text(dir_for("m1"), "keys", k2, kl);
    CHECK(puddle_identity_open(dir_for("m1"), err, sizeof err) == NULL && strstr(err, "different identities"));
    free(k2);
}

static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-identity-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root))
        return 2;
    test_create_and_reopen();
    test_roll();
    test_forged_roll();
    test_tampering();
    test_prev_link();
    test_recovery();
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
