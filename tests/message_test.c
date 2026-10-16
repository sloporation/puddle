#include "message.h"

#include "encoding.h"
#include "hash.h"
#include "link.h"
#include "phrase.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

static puddle_identity *make(const char *name, char *phrase, char *uid)
{
    char dir[700], err[300];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    puddle_identity *id = puddle_identity_create(dir, phrase, err, sizeof err);
    if (!id) exit(2);
    puddle_identity_confirm(dir);
    puddle_identity_id(id, uid);
    return id;
}

#define T 1760000000000LL

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-message-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    char pa[PUDDLE_PHRASE_LEN + 1], pb[PUDDLE_PHRASE_LEN + 1], ua[64], ub[64];
    puddle_identity *alice = make("alice", pa, ua), *bob = make("bob", pb, ub);

    /* ---- the chat id ---- */
    char c1[64], c2[64], c3[64];
    CHECK(puddle_dm_chat_id(ua, ub, c1) == 0 && puddle_dm_chat_id(ub, ua, c2) == 0 && !strcmp(c1, c2));   /* both sides agree */
    CHECK(strlen(c1) == 52 && puddle_id_valid(c1));
    CHECK(puddle_dm_chat_id(ua, ua, c3) == 0 && strcmp(c3, c1) != 0);
    CHECK(puddle_dm_chat_id(ua, "short", c3) == -1 && puddle_dm_chat_id("", ub, c3) == -1);
    {
        /* fixed: the formula in docs/storage.md, worked out independently */
        char in[2 + 104], hex[65];
        const char *lo = strcmp(ua, ub) < 0 ? ua : ub, *hi = lo == ua ? ub : ua;
        memcpy(in, "dm", 2); memcpy(in + 2, lo, 52); memcpy(in + 54, hi, 52);
        puddle_sha256_hex(in, 106, hex);
        unsigned char raw[32];
        char want[64];
        CHECK(puddle_hex_decode(hex, raw, 32) == 0 && puddle_base32_encode(raw, 32, want, sizeof want) == 52 && !strcmp(want, c1));
    }

    /* ---- bodies ---- */
    CHECK(puddle_message_body_ok("hello", 5) && puddle_message_body_ok("a", 1));
    CHECK(puddle_message_body_ok("line\none\ttab", 12));
    CHECK(puddle_message_body_ok("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x99\x82 \xE6\x97\xA5\xE6\x9C\xAC", 16));  /* any language */
    CHECK(!puddle_message_body_ok("", 0));
    CHECK(!puddle_message_body_ok("a\0b", 3) && !puddle_message_body_ok("bell\a", 5) && !puddle_message_body_ok("x\x7f", 2) && !puddle_message_body_ok("x\r", 2));
    CHECK(!puddle_message_body_ok("bad\xFF", 4) && !puddle_message_body_ok("\xC0\xAF", 2) && !puddle_message_body_ok("\xED\xA0\x80", 3) && !puddle_message_body_ok("\xE2\x82", 2));
    CHECK(!puddle_message_body_ok("a\xC2\x85z", 4));                                                   /* a C1 control */
    CHECK(!puddle_message_body_ok("a\xE2\x80\xA8z", 5) && !puddle_message_body_ok("a\xE2\x80\xA9z", 5)); /* line/paragraph separators */
    {
        char big[16100];
        memset(big, 'x', sizeof big);
        CHECK(puddle_message_body_ok(big, 4000) && !puddle_message_body_ok(big, 4001));
        for (int i = 0; i < 4000; i++) memcpy(big + i * 4, "\xF0\x9F\x99\x82", 4);
        CHECK(puddle_message_body_ok(big, 16000));                                                     /* 4000 characters of 4 bytes */
        CHECK(!puddle_message_body_ok(big, 16004));
    }

    /* ---- time never repeats ---- */
    {
        int64_t a = puddle_event_next_ts(T), b = puddle_event_next_ts(T), c = puddle_event_next_ts(T - 5000), d = puddle_event_next_ts(T + 100000);
        CHECK(a == T && b == T + 1 && c == T + 2 && d == T + 100000);
    }

    /* ---- build, parse ---- */
    char ev[PUDDLE_EVENT_MAX_BYTES];
    size_t el = puddle_message_build(c1, ua, T, "hi \"there\"\nsecond", 17, NULL, ev, sizeof ev);
    CHECK(el > 0 && el == strlen(ev) && !strchr(ev, '\n') && !strstr(ev, ", "));                     /* compact, one line */
    CHECK(!strncmp(ev, "{\"type\":\"message\",\"chat\":\"", 26));                                  /* the fields come in a fixed order */
    puddle_message m;
    const char *why;
    CHECK(puddle_message_parse(ev, el, &m, &why) == 0 && !strcmp(m.chat, c1) && !strcmp(m.sender, ua) && m.ts == T &&
          !strcmp(m.body, "hi \"there\"\nsecond") && !m.ref[0]);
    puddle_message_free(&m);
    const char *ref = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    el = puddle_message_build(c1, ua, T, "re", 2, ref, ev, sizeof ev);
    CHECK(el > 0 && puddle_message_parse(ev, el, &m, &why) == 0 && !strcmp(m.ref, ref));
    puddle_message_free(&m);
    CHECK(puddle_message_build(c1, ua, T, "x", 1, "not an id", ev, sizeof ev) == 0);
    CHECK(puddle_message_build(c1, ua, T, "", 0, NULL, ev, sizeof ev) == 0);
    CHECK(puddle_message_build(c1, ua, T, "bell\a", 5, NULL, ev, sizeof ev) == 0);
    CHECK(puddle_message_build(c1, ua, T, "hello", 5, NULL, ev, 40) == 0);                              /* does not fit */
    CHECK(puddle_message_build(c1, ua, T, "x", 1, "", ev, sizeof ev) > 0);                              /* an empty reply is none */

    /* what a receiver refuses, and says why */
    {
        struct { const char *event; const char *why; } bad[] = {
            { "", "malformed" }, { "[]", "malformed" }, { "{", "malformed" }, { "{\"type\":\"message\"}", "malformed" },
            { "{\"type\":\"edit\",\"chat\":\"x\",\"sender\":\"y\",\"ts\":1,\"body\":\"b\"}", "unsupported_type" },
            { "{\"type\":7}", "malformed" },
        };
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            if (puddle_message_parse(bad[i].event, strlen(bad[i].event), &m, &why) == 0 || strcmp(why, bad[i].why)) {
                fprintf(stderr, "FAIL parse %zu: %s\n", i, why);
                failures++;
            }
        }
        char t[1200];
        const char *fmt = "{\"type\":\"message\",\"chat\":\"%s\",\"sender\":\"%s\",\"ts\":%lld,\"body\":%s%s}";
        struct { long long ts; const char *body; const char *extra; const char *why; } v[] = {
            { 0, "\"x\"", "", "bad_time" }, { 946684799999LL, "\"x\"", "", "bad_time" }, { 4102444800000LL, "\"x\"", "", "bad_time" },
            { -5, "\"x\"", "", "bad_time" }, { T, "\"\"", "", "bad_body" }, { T, "7", "", "bad_body" }, { T, "\"a\\u0000b\"", "", "bad_body" },
            { T, "\"bell\\u0007\"", "", "bad_body" }, { T, "\"x\"", ",\"ref\":\"short\"", "malformed" }, { T, "\"x\"", ",\"ref\":5", "malformed" },
            { T, "\"x\"", ",\"ref\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg\"", "malformed" },
        };
        for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
            snprintf(t, sizeof t, fmt, c1, ua, v[i].ts, v[i].body, v[i].extra);
            if (puddle_message_parse(t, strlen(t), &m, &why) == 0 || strcmp(why, v[i].why)) {
                fprintf(stderr, "FAIL parse variant %zu: %s (wanted %s)\n", i, why, v[i].why);
                failures++;
            }
        }
        snprintf(t, sizeof t, fmt, c1, ua, (long long)T, "\"fine\"", "");
        CHECK(puddle_message_parse(t, strlen(t), &m, &why) == 0);
        puddle_message_free(&m);
        snprintf(t, sizeof t, "{\"type\":\"message\",\"type\":\"message\",\"chat\":\"%s\",\"sender\":\"%s\",\"ts\":%lld,\"body\":\"x\"}", c1, ua, (long long)T);
        CHECK(puddle_message_parse(t, strlen(t), &m, &why) == -1);                                     /* a repeated key is ambiguous */
        snprintf(t, sizeof t, fmt, "short", ua, (long long)T, "\"x\"", "");
        CHECK(puddle_message_parse(t, strlen(t), &m, &why) == -1);
        static char huge[PUDDLE_EVENT_MAX_BYTES + 100];
        memset(huge, 'x', sizeof huge);
        huge[0] = '{';
        huge[sizeof huge - 1] = '}';
        CHECK(puddle_message_parse(huge, sizeof huge, &m, &why) == -1 && !strcmp(why, "malformed"));
    }

    /* ---- signatures ---- */
    {
        el = puddle_message_build(c1, ua, T, "signed", 6, NULL, ev, sizeof ev);
        char sig[PUDDLE_SIG_TEXT_MAX], kid[65], mykid[65];
        unsigned char s[64], pub[32], bpub[32];
        CHECK(puddle_event_sign(alice, ev, el, sig) == 0);
        puddle_identity_public(alice, pub, NULL, NULL);
        puddle_identity_public(bob, bpub, NULL, NULL);
        puddle_key_id(pub, mykid);
        CHECK(puddle_event_sig_parse(sig, kid, s) == 0 && !strcmp(kid, mykid));
        CHECK(puddle_event_verify(pub, ev, el, s));
        CHECK(!puddle_event_verify(bpub, ev, el, s));                                                  /* someone else's key */
        ev[el - 5] ^= 1;
        CHECK(!puddle_event_verify(pub, ev, el, s));                                                   /* one bit changed */
        ev[el - 5] ^= 1;
        CHECK(!puddle_event_verify(pub, ev, el - 1, s));                                               /* cut short */
        /* the signature is over a domain and the bytes: the bytes alone do not verify */
        unsigned char plain[64];
        CHECK(puddle_identity_sign(alice, ev, el, plain) == 0 && !puddle_event_verify(pub, ev, el, plain));
        CHECK(puddle_verify_signature(pub, ev, el, plain));
        /* a signature made here is not a signature over the same bytes without the domain */
        CHECK(!puddle_verify_signature(pub, ev, el, s));

        CHECK(puddle_event_sig_parse(NULL, kid, s) == -1 && puddle_event_sig_parse("", kid, s) == -1 && puddle_event_sig_parse("nocolon", kid, s) == -1);
        char t[300];
        snprintf(t, sizeof t, "%.63s:%s", sig, strchr(sig, ':') + 1);
        CHECK(puddle_event_sig_parse(t, kid, s) == -1);                                                /* a short key id */
        snprintf(t, sizeof t, "%s", sig);
        t[10] = 'g';
        CHECK(puddle_event_sig_parse(t, kid, s) == -1);                                                /* not hex */
        snprintf(t, sizeof t, "%.64s:AAAA", sig);
        CHECK(puddle_event_sig_parse(t, kid, s) == -1);                                                /* too short */
        snprintf(t, sizeof t, "%.64s:%s=", sig, strchr(sig, ':') + 1);
        CHECK(puddle_event_sig_parse(t, kid, s) == -1);                                                /* too long */
        snprintf(t, sizeof t, "%.64s", sig);
        CHECK(puddle_event_sig_parse(t, kid, s) == -1);                                                /* no signature */
    }

    puddle_identity_close(alice);
    puddle_identity_close(bob);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
