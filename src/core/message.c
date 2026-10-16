#include "message.h"

#include "encoding.h"
#include "hash.h"
#include "json.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIG_DOMAIN "puddle-event-v1\n"
#define TS_MIN 946684800000LL   /* 2000-01-01 */
#define TS_MAX 4102444800000LL  /* 2100-01-01 */

int puddle_dm_chat_id(const char *a, const char *b, char out[PUDDLE_USER_ID_LEN + 1])
{
    if (strlen(a) != PUDDLE_USER_ID_LEN || strlen(b) != PUDDLE_USER_ID_LEN)
        return -1;
    const char *lo = strcmp(a, b) <= 0 ? a : b, *hi = lo == a ? b : a;
    char in[2 + 2 * PUDDLE_USER_ID_LEN];
    memcpy(in, "dm", 2);
    memcpy(in + 2, lo, PUDDLE_USER_ID_LEN);
    memcpy(in + 2 + PUDDLE_USER_ID_LEN, hi, PUDDLE_USER_ID_LEN);
    char hex[65];
    unsigned char raw[32];
    puddle_sha256_hex(in, sizeof in, hex);
    if (puddle_hex_decode(hex, raw, 32) != 0)
        return -1;
    return puddle_base32_encode(raw, 32, out, PUDDLE_USER_ID_LEN + 1) == PUDDLE_USER_ID_LEN ? 0 : -1;
}

/* ---- signatures ---- */

static int domain_wrap(const char *domain, const char *data, size_t len, char **out, size_t *outlen)
{
    size_t dl = strlen(domain);
    char *buf = malloc(dl + len);
    if (!buf)
        return -1;
    memcpy(buf, domain, dl);
    memcpy(buf + dl, data, len);
    *out = buf;
    *outlen = dl + len;
    return 0;
}

int puddle_text_sign(puddle_identity *me, const char *domain, const char *data, size_t len, char out[PUDDLE_SIG_TEXT_MAX])
{
    char *wrapped;
    size_t wl;
    if (domain_wrap(domain, data, len, &wrapped, &wl) != 0)
        return -1;
    unsigned char sig[64], pub[32];
    int rc = puddle_identity_sign(me, wrapped, wl, sig);
    free(wrapped);
    if (rc != 0)
        return -1;
    puddle_identity_public(me, pub, NULL, NULL);
    char kid[65], b64[100];
    puddle_key_id(pub, kid);
    if (!puddle_base64_encode(sig, 64, b64, sizeof b64))
        return -1;
    int n = snprintf(out, PUDDLE_SIG_TEXT_MAX, "%s:%s", kid, b64);
    return n > 0 && n < PUDDLE_SIG_TEXT_MAX ? 0 : -1;
}

int puddle_text_verify(const unsigned char sign_pub[32], const char *domain, const char *data, size_t len, const unsigned char sig[64])
{
    char *wrapped;
    size_t wl;
    if (domain_wrap(domain, data, len, &wrapped, &wl) != 0)
        return 0;
    int ok = puddle_verify_signature(sign_pub, wrapped, wl, sig);
    free(wrapped);
    return ok;
}

int puddle_event_sign(puddle_identity *me, const char *event, size_t len, char out[PUDDLE_SIG_TEXT_MAX])
{
    return puddle_text_sign(me, SIG_DOMAIN, event, len, out);
}

int puddle_event_sig_parse(const char *text, char kid[65], unsigned char sig[64])
{
    const char *colon = text ? strchr(text, ':') : NULL;
    unsigned char raw[32];
    size_t got = 0;
    if (!colon || colon - text != 64)
        return -1;
    memcpy(kid, text, 64);
    kid[64] = '\0';
    if (puddle_hex_decode(kid, raw, 32) != 0)
        return -1;
    return puddle_base64_decode(colon + 1, sig, 64, &got) == 0 && got == 64 ? 0 : -1;
}

int puddle_event_verify(const unsigned char sign_pub[32], const char *event, size_t len, const unsigned char sig[64])
{
    return puddle_text_verify(sign_pub, SIG_DOMAIN, event, len, sig);
}

/* ---- text ---- */

/* characters in valid UTF-8 text with no control characters but \n and \t, or -1 */
static long chars_of(const char *s, size_t len)
{
    const unsigned char *u = (const unsigned char *)s;
    long n = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = u[i];
        size_t k;
        if (c < 0x80) {
            if (c < 0x20 && c != '\n' && c != '\t')
                return -1;
            if (c == 0x7f)
                return -1;
            k = 1;
        } else if (c >= 0xC2 && c <= 0xDF && i + 1 < len && (u[i + 1] & 0xC0) == 0x80) {
            k = 2;
        } else if (c >= 0xE0 && c <= 0xEF && i + 2 < len && (u[i + 1] & 0xC0) == 0x80 && (u[i + 2] & 0xC0) == 0x80 &&
                   !(c == 0xE0 && u[i + 1] < 0xA0) && !(c == 0xED && u[i + 1] >= 0xA0)) {
            k = 3;
            if (c == 0xE2 && u[i + 1] == 0x80 && (u[i + 2] == 0xA8 || u[i + 2] == 0xA9))
                return -1; /* line and paragraph separators: they break lines in places a log should not */
        } else if (c >= 0xF0 && c <= 0xF4 && i + 3 < len && (u[i + 1] & 0xC0) == 0x80 && (u[i + 2] & 0xC0) == 0x80 &&
                   (u[i + 3] & 0xC0) == 0x80 && !(c == 0xF0 && u[i + 1] < 0x90) && !(c == 0xF4 && u[i + 1] >= 0x90)) {
            k = 4;
        } else {
            return -1;
        }
        if (c == 0xC2 && u[i + 1] < 0xA0)
            return -1; /* C1 controls */
        i += k;
        n++;
    }
    return n;
}

int puddle_message_body_ok(const char *body, size_t len)
{
    if (len == 0 || len > PUDDLE_MESSAGE_MAX_CHARS * 4u)
        return 0;
    long n = chars_of(body, len);
    return n >= 1 && n <= PUDDLE_MESSAGE_MAX_CHARS;
}

/* ---- time ---- */

int64_t puddle_event_next_ts(int64_t now_ms)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static int64_t last;
    pthread_mutex_lock(&mu);
    int64_t t = now_ms > last ? now_ms : last + 1;
    last = t;
    pthread_mutex_unlock(&mu);
    return t;
}

/* ---- the message event ---- */

static int is_event_id(const char *s)
{
    unsigned char raw[32];
    return s && strlen(s) == 64 && puddle_hex_decode(s, raw, 32) == 0;
}

size_t puddle_message_build(const char *chat, const char *sender, int64_t ts, const char *body, size_t body_len,
                            const char *ref, char *out, size_t cap)
{
    if (!puddle_message_body_ok(body, body_len) || (ref && *ref && !is_event_id(ref)))
        return 0;
    puddle_jw w;
    puddle_jw_init(&w, out, cap);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "type", "message");
    puddle_jw_str(&w, "chat", chat);
    puddle_jw_str(&w, "sender", sender);
    puddle_jw_num(&w, "ts", ts);
    puddle_jw_key(&w, "body");
    puddle_jw_string_n(&w, body, body_len);
    if (ref && *ref)
        puddle_jw_str(&w, "ref", ref);
    puddle_jw_end_object(&w);
    return puddle_jw_finish(&w);
}

void puddle_message_free(puddle_message *m)
{
    free(m->body);
    m->body = NULL;
}

static int member_str(const char *e, size_t n, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_member(e, n, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

int puddle_message_parse(const char *event, size_t len, puddle_message *m, const char **why)
{
    memset(m, 0, sizeof *m);
    *why = "malformed";
    if (len == 0 || len > PUDDLE_EVENT_MAX_BYTES || !puddle_json_valid_object(event, len))
        return -1;
    char type[16];
    if (member_str(event, len, "type", type, sizeof type) != 0)
        return -1;
    if (strcmp(type, "message") != 0) {
        *why = "unsupported_type";
        return -1;
    }
    if (member_str(event, len, "chat", m->chat, sizeof m->chat) != 0 || member_str(event, len, "sender", m->sender, sizeof m->sender) != 0 ||
        strlen(m->chat) != PUDDLE_USER_ID_LEN || strlen(m->sender) != PUDDLE_USER_ID_LEN)
        return -1;
    puddle_span v;
    if (puddle_json_member(event, len, "ts", &v) != 0 || puddle_json_int64(v, &m->ts) != 0)
        return -1;
    if (m->ts < TS_MIN || m->ts >= TS_MAX) {
        *why = "bad_time";
        return -1;
    }
    if (puddle_json_member(event, len, "body", &v) != 0 || v.len < 2 || v.p[0] != '"')
        goto bad_body;
    m->body = malloc(v.len + 1);
    if (!m->body || puddle_json_string(v, m->body, v.len + 1) != 0 || !puddle_message_body_ok(m->body, strlen(m->body)))
        goto bad_body;
    int rc = puddle_json_member(event, len, "ref", &v);
    if (rc < 0) {
        puddle_message_free(m);
        return -1;
    }
    if (rc == 0 && (puddle_json_string(v, m->ref, sizeof m->ref) != 0 || !is_event_id(m->ref))) {
        puddle_message_free(m);
        return -1;
    }
    *why = NULL;
    return 0;
bad_body:
    puddle_message_free(m);
    *why = "bad_body";
    return -1;
}
