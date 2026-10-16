#include "access.h"

#include "client.h"
#include "wireprofile.h"
#include "flow.h"
#include "profiles_api.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- text ---- */

/* length of the valid UTF-8 sequence at s, or 0 */
static size_t seq_len(const unsigned char *s, size_t n)
{
    if (s[0] < 0x80) return 1;
    if (s[0] >= 0xC2 && s[0] <= 0xDF) return n >= 2 && (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (s[0] == 0xE0 && s[1] < 0xA0) return 0;
        if (s[0] == 0xED && s[1] >= 0xA0) return 0;
        return 3;
    }
    if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (s[0] == 0xF0 && s[1] < 0x90) return 0;
        if (s[0] == 0xF4 && s[1] >= 0x90) return 0;
        return 4;
    }
    return 0;
}

static unsigned decode_cp(const unsigned char *s, size_t k)
{
    if (k == 1) return s[0];
    if (k == 2) return ((unsigned)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    if (k == 3) return ((unsigned)(s[0] & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    return ((unsigned)(s[0] & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12) | ((unsigned)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
}

int puddle_clean_text(const char *in, size_t len, size_t max_chars, int multiline, char *out, size_t cap, size_t *chars)
{
    const unsigned char *u = (const unsigned char *)in;
    size_t o = 0, n = 0, i = 0;
    int last_kept = 0; /* output length that ends in something other than trailing blanks */
    while (i < len) {
        size_t k = seq_len(u + i, len - i);
        const char *emit = (const char *)u + i;
        size_t el = k;
        unsigned cp;
        if (k == 0) {
            emit = "\xEF\xBF\xBD";
            el = 3;
            k = 1;
            cp = 0xFFFD;
        } else {
            cp = decode_cp(u + i, k);
        }
        i += k;
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0) || cp == 0x2028 || cp == 0x2029) {
            if (cp == '\n' && multiline) {
                /* kept */
            } else if (cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x2028 || cp == 0x2029) {
                emit = " ";
                el = 1;
            } else {
                continue; /* other control characters just go */
            }
        }
        if ((cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0x200E || cp == 0x200F || cp == 0x061C)
            continue; /* direction overrides and marks */
        if (o == 0 && (emit[0] == ' ' || emit[0] == '\n') && el == 1)
            continue; /* leading blanks */
        if (o + el >= cap || n + 1 > max_chars + 64) /* hard stop well past the limit; the exact check is below */
            return -1;
        memcpy(out + o, emit, el);
        o += el;
        n++;
        if (!(el == 1 && (emit[0] == ' ' || emit[0] == '\n')))
            last_kept = (int)o;
    }
    /* trailing blanks: drop them and the characters they were */
    for (size_t j = (size_t)last_kept; j < o; j++)
        n--;
    o = (size_t)last_kept;
    out[o] = '\0';
    if (n > max_chars)
        return -1;
    *chars = n;
    return 0;
}

/* ---- reading a request ---- */

typedef struct {
    puddle_peer_info peer;
    char message[PUDDLE_NOTE_MAX_BYTES + 1];
    char invite[PUDDLE_INVITE_TOKEN_LEN + 1];
    int has_scope;
} request;

static void request_free(request *r) { puddle_peer_info_free(&r->peer); }

static int string_member(puddle_span obj, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_member(obj.p, obj.len, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

/* ---- a person as one side tells it to the other: id, address, profile, chain ---- */

void puddle_peer_info_free(puddle_peer_info *p)
{
    free(p->chain);
    p->chain = NULL;
}

int puddle_peer_parse(const char *obj, size_t len, puddle_peer_info *p)
{
    memset(p, 0, sizeof *p);
    puddle_span o = { obj, len }, prof, v;
    if (len < 2 || obj[0] != '{')
        return -1;
    if (string_member(o, "id", p->id, sizeof p->id) != 0 || !puddle_id_valid(p->id))
        return -1;
    char addr[PUDDLE_ADDR_MAX];
    puddle_address pa;
    if (string_member(o, "address", addr, sizeof addr) != 0 || puddle_address_parse(addr, &pa) != 0 || !puddle_net_host_plausible(pa.host))
        return -1;
    snprintf(p->address, sizeof p->address, "%s", pa.address);
    if (puddle_json_member(obj, len, "profile", &prof) != 0 || prof.len < 2 || prof.p[0] != '{' ||
        puddle_profile_wire_clean(prof.p, prof.len, p->profile, sizeof p->profile) != 0)
        return -1;
    if (puddle_json_member(obj, len, "chain", &v) != 0 || v.len < 2 || v.p[0] != '"' || v.len > PUDDLE_ACCESS_CHAIN_MAX * 6u)
        return -1;
    p->chain = malloc(v.len + 1);
    if (!p->chain || puddle_json_string(v, p->chain, v.len + 1) != 0)
        goto bad;
    p->chain_len = strlen(p->chain);
    if (p->chain_len == 0 || p->chain_len > PUDDLE_ACCESS_CHAIN_MAX)
        goto bad;
    return 0;
bad:
    puddle_peer_info_free(p);
    return -1;
}

int puddle_peer_key(const puddle_peer_info *p, const char *sender_id, const char *kid, unsigned char sign_pub[32])
{
    if (!p->chain || strcmp(p->id, sender_id) != 0) /* the signature must be this person's own */
        return 0;
    puddle_chain_head head;
    char err[200], want[65];
    if (puddle_chain_verify(p->chain, p->chain_len, p->id, &head, err, sizeof err) != 0)
        return 0;
    puddle_key_id(head.sign_pub, want);
    if (strcmp(want, kid) != 0)
        return 0;
    memcpy(sign_pub, head.sign_pub, 32);
    return 1;
}

static int parse_request(const unsigned char *body, size_t len, request *r)
{
    memset(r, 0, sizeof *r);
    const char *b = (const char *)body;
    puddle_span req, v;
    if (puddle_json_member(b, len, "requester", &req) != 0 || puddle_peer_parse(req.p, req.len, &r->peer) != 0)
        return -1;
    int rc = puddle_json_member(b, len, "message", &v);
    if (rc < 0)
        goto bad;
    if (rc == 0) {
        char *raw = malloc(v.len + 1);
        size_t chars;
        int ok = raw && puddle_json_string(v, raw, v.len + 1) == 0 &&
                 puddle_clean_text(raw, strlen(raw), PUDDLE_NOTE_MAX_CHARS, 1, r->message, sizeof r->message, &chars) == 0;
        free(raw);
        if (!ok)
            goto bad;
    }
    rc = puddle_json_member(b, len, "invite", &v);
    if (rc < 0)
        goto bad;
    if (rc == 0 && (puddle_json_string(v, r->invite, sizeof r->invite) != 0 || !puddle_invite_token_valid(r->invite)))
        r->invite[0] = '\0'; /* a malformed token is just no token: the request still stands */
    rc = puddle_json_member(b, len, "scope", &v);
    if (rc < 0)
        goto bad;
    r->has_scope = rc == 0;
    return 0;
bad:
    request_free(r);
    return -1;
}

/* ---- the chain lookup for the signature check ---- */

int puddle_access_chain_key(puddle_ctx *ctx, const unsigned char *body, size_t body_len, const char *sender_id,
                            const char *kid, unsigned char sign_pub[32])
{
    /* Everything below costs signature checks on input from a stranger: cheap refusals first. */
    if (!puddle_id_valid(sender_id) || !kid)
        return 0;
    if (ctx->anon_bucket && !puddle_bucket_take(ctx->anon_bucket, puddle_ctx_now_ms(ctx)))
        return 0;
    request r;
    if (parse_request(body, body_len, &r) != 0)
        return 0;
    int ok = puddle_peer_key(&r.peer, sender_id, kid, sign_pub);
    request_free(&r);
    return ok;
}

/* ---- what is done with a request that is really from who it says ---- */

static void refuse(const char *why, const char *id)
{
    puddle_log(PUDDLE_LOG_INFO, "stream request from %.12s… dropped: %s", id, why);
}

typedef struct {
    const request *r;
    int approve;
    int64_t now;
} apply;

static int apply_request(void *user, puddle_stream *st, int exists)
{
    apply *a = user;
    if (exists && st->state == PUDDLE_STREAM_ACTIVE)
        return 1;
    st->state = a->approve ? PUDDLE_STREAM_ACTIVE : PUDDLE_STREAM_PENDING_IN;
    st->owed = a->approve ? PUDDLE_OWED_APPROVED : PUDDLE_OWED_NONE; /* an approval has to be told to them */
    st->updated_ms = a->now;
    if (!exists)
        st->created_ms = a->now;
    snprintf(st->note, sizeof st->note, "%s", a->r->message);
    st->addr_seq = 0;
    st->naddr = 0;
    puddle_stream_add_address(st, a->r->peer.address);
    if (puddle_stream_set_chain(st, a->r->peer.chain, a->r->peer.chain_len) != 0 ||
        puddle_stream_set_profile(st, a->r->peer.profile, strlen(a->r->peer.profile)) != 0)
        return -1;
    return 0;
}

int puddle_access_post(const puddle_call *call, puddle_jw *w)
{
    (void)w; /* the answer is always empty */
    puddle_ctx *ctx = call->ctx;
    int64_t now = puddle_call_now_ms(call);
    request r;
    if (parse_request(call->body, call->body_len, &r) != 0 || strcmp(r.peer.id, call->sender) != 0)
        return 202; /* cannot happen after authentication, but never trust that */
    const char *id = r.peer.id;
    puddle_stream rec;
    int have = -1;

    if (r.has_scope) {
        refuse("group-only connections are not supported yet", id);
        goto done;
    }
    if (puddle_streams_is_blocked(ctx->streams, id)) {
        refuse("blocked", id);
        goto done;
    }
    if (ctx->access_limit && !puddle_keyed_hit(ctx->access_limit, id, now)) {
        refuse("too many requests from them", id);
        goto done;
    }

    have = puddle_streams_get(ctx->streams, id, &rec);
    int mutual = have == 0 && rec.state == PUDDLE_STREAM_PENDING_OUT; /* we asked them, and now they ask us */
    puddle_access policy = puddle_streams_access(ctx->streams);
    if (have == 0 && rec.state == PUDDLE_STREAM_ACTIVE) {
        refuse("already a stream", id);
        goto done;
    }
    if (policy == PUDDLE_ACCESS_CLOSED && !mutual) { /* "closed" means ignore strangers, not people we asked */
        refuse("the Puddle is not taking requests", id);
        goto done;
    }
    if (have != 0 && puddle_streams_size(ctx->streams) >= (ctx->max_peers ? ctx->max_peers : PUDDLE_MAX_PEERS)) {
        refuse("too many people on record", id);
        goto done;
    }

    /* Who gets in at once: someone we asked ourselves, anyone if the policy is open, whoever brings a good
     * invite (spent only when it is what lets them in); everyone else waits for the owner. */
    int approve = mutual || policy == PUDDLE_ACCESS_OPEN ||
                  (r.invite[0] && puddle_streams_invite_consume(ctx->streams, r.invite, now));
    if (!approve && !(have == 0 && rec.state == PUDDLE_STREAM_PENDING_IN) &&
        puddle_streams_count(ctx->streams, PUDDLE_STREAM_PENDING_IN) >= PUDDLE_MAX_PENDING_IN) {
        refuse("too many requests waiting", id);
        goto done;
    }

    {
        /* fold the request into whatever is on file at the moment of writing, so what other threads have set
         * (a certificate noted, an answer owed) is kept and a stream that became active meanwhile is not touched */
        apply a = { &r, approve, now };
        char err[300];
        int urc = puddle_streams_update(ctx->streams, id, apply_request, &a, err, sizeof err);
        if (urc == 0) {
            puddle_log(PUDDLE_LOG_INFO, "stream request from %.12s… %s", id, approve ? "approved" : "waiting for the owner");
            if (approve) {
                puddle_flow_owe(ctx, id, PUDDLE_OWED_APPROVED);
                puddle_media_want(ctx, id);
            }
        }
        else if (urc < 0)
            puddle_log(PUDDLE_LOG_WARN, "stream request from %.12s… could not be recorded", id);
    }
done:
    if (have == 0)
        puddle_stream_free(&rec);
    request_free(&r);
    return 202;
}
