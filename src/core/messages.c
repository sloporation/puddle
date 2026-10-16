#include "messages.h"

#include "encoding.h"
#include "flow.h"
#include "hash.h"
#include "log.h"
#include "message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PAGE 50
#define MAX_PAGE 200
#define PAGE_BUDGET 23000 /* bytes of answer a page of messages may take, out of what a handler may write */

/* ---- one message, as the owner's app sees it ---- */

static void message_json(puddle_jw *w, const puddle_event *e, const char *me)
{
    puddle_message m;
    const char *why;
    puddle_jw_object(w);
    puddle_jw_str(w, "id", e->id);
    if (puddle_message_parse(e->event, e->event_len, &m, &why) == 0) {
        puddle_jw_str(w, "chat", m.chat);
        puddle_jw_str(w, "sender", m.sender);
        puddle_jw_num(w, "ts", m.ts);
        puddle_jw_str(w, "body", m.body);
        if (m.ref[0])
            puddle_jw_str(w, "reply_to", m.ref);
        puddle_jw_flag(w, "outgoing", strcmp(m.sender, me) == 0);
        puddle_message_free(&m);
    }
    puddle_jw_num(w, "received_at", e->rts);
    puddle_jw_end_object(w);
}

/* ---- owner: send ---- */

static int send_dm(const puddle_call *c, puddle_jw *w)
{
    puddle_span v;
    const char *b = c->body_len ? (const char *)c->body : "{}";
    size_t bl = c->body_len ? c->body_len : 2;
    char to[128], ref[128] = "", me[PUDDLE_USER_ID_LEN + 1], chat[PUDDLE_USER_ID_LEN + 1];
    if (!puddle_json_valid_object(b, bl) || puddle_json_member(b, bl, "to", &v) != 0 || puddle_json_string(v, to, sizeof to) != 0 ||
        !puddle_id_valid(to))
        return puddle_fail(w, 400, "bad_request");
    if (puddle_json_member(b, bl, "body", &v) != 0 || v.len < 2 || v.p[0] != '"')
        return puddle_fail(w, 400, "bad_request");
    char *text = malloc(v.len + 1);
    if (!text || puddle_json_string(v, text, v.len + 1) != 0) {
        free(text);
        return puddle_fail(w, 400, "bad_request");
    }
    size_t tl = strlen(text);
    if (!puddle_message_body_ok(text, tl)) {
        free(text);
        puddle_jw_object(w);
        puddle_jw_str(w, "error", "bad_body");
        puddle_jw_str(w, "message", "a message is 1 to 4000 characters of text, with no control characters");
        puddle_jw_end_object(w);
        return 400;
    }
    int rc = puddle_json_member(b, bl, "reply_to", &v);
    if (rc < 0 || (rc == 0 && puddle_json_string(v, ref, sizeof ref) != 0)) {
        free(text);
        return puddle_fail(w, 400, "bad_request");
    }

    puddle_identity_id(c->ctx->identity, me);
    if (!strcmp(to, me)) {
        free(text);
        return puddle_fail(w, 400, "cannot_message_yourself");
    }
    puddle_stream st;
    int have = puddle_streams_get(c->ctx->streams, to, &st);
    int active = have == 0 && st.state == PUDDLE_STREAM_ACTIVE;
    if (have == 0)
        puddle_stream_free(&st);
    if (!active || puddle_streams_is_blocked(c->ctx->streams, to)) {
        free(text);
        return puddle_fail(w, 403, "no_stream");
    }
    puddle_dm_chat_id(me, to, chat);

    char event[PUDDLE_EVENT_MAX_BYTES];
    size_t el = puddle_message_build(chat, me, puddle_event_next_ts(puddle_call_now_ms(c)), text, tl, ref, event, sizeof event);
    free(text);
    if (!el)
        return puddle_fail(w, 400, "bad_request"); /* a reply reference that is not an id, or a message too large to send */
    char sig[PUDDLE_SIG_TEXT_MAX], id[PUDDLE_ID_HEX + 1], err[300];
    if (puddle_event_sign(c->ctx->identity, event, el, sig) != 0)
        return puddle_fail(w, 500, "internal");
    int ar = puddle_store_append(c->ctx->store, chat, event, el, sig, 0, id, err, sizeof err);
    if (ar < 0)
        return puddle_fail(w, 500, "internal");

    /* the delivery: {"chat":..,"events":[{"id","event","sig"}]} */
    size_t cap = el * 6 + 1024;
    char *body = malloc(cap);
    if (!body)
        return puddle_fail(w, 500, "internal");
    puddle_jw d;
    puddle_jw_init(&d, body, cap);
    puddle_jw_object(&d);
    puddle_jw_str(&d, "chat", chat);
    puddle_jw_key(&d, "events");
    puddle_jw_array(&d);
    puddle_jw_object(&d);
    puddle_jw_str(&d, "id", id);
    puddle_jw_key(&d, "event");
    puddle_jw_string_n(&d, event, el);
    puddle_jw_str(&d, "sig", sig);
    puddle_jw_end_object(&d);
    puddle_jw_end_array(&d);
    puddle_jw_end_object(&d);
    size_t dl = puddle_jw_finish(&d);
    int queued = dl && c->ctx->outbox && puddle_outbox_enqueue(c->ctx->outbox, to, "events", "/stream/v1/events", body, dl, err, sizeof err) == 0;
    free(body);
    if (!queued) {
        /* it is in our own chat, but will not be sent; say so rather than pretend */
        puddle_log(PUDDLE_LOG_WARN, "message %.12s… is stored but could not be queued for delivery", id);
        puddle_jw_object(w);
        puddle_jw_str(w, "error", "not_queued");
        puddle_jw_str(w, "id", id);
        puddle_jw_str(w, "message", "the message was saved here but could not be queued to send");
        puddle_jw_end_object(w);
        return 500;
    }

    puddle_event e = { .ts = 0, .rts = puddle_call_now_ms(c), .event = event, .event_len = el };
    snprintf(e.id, sizeof e.id, "%s", id);
    message_json(w, &e, me);
    return 201;
}

/* ---- owner: read ---- */

static int list_dm(const puddle_call *c, puddle_jw *w)
{
    const char *keys[3] = { "user", "limit", "offset" };
    char v[3][PUDDLE_QUERY_VALUE_MAX], me[PUDDLE_USER_ID_LEN + 1], chat[PUDDLE_USER_ID_LEN + 1], err[300];
    if (puddle_query_parse(c->req->target, keys, 3, v) != 0 || !puddle_id_valid(v[0]))
        return puddle_fail(w, 400, "bad_request");
    size_t limit = DEFAULT_PAGE, offset = 0;
    for (int k = 1; k <= 2; k++) {
        if (!v[k][0])
            continue;
        size_t n = 0;
        if (strlen(v[k]) > 9)
            return puddle_fail(w, 400, "bad_request");
        for (const char *p = v[k]; *p; p++) {
            if (*p < '0' || *p > '9')
                return puddle_fail(w, 400, "bad_request");
            n = n * 10 + (size_t)(*p - '0');
        }
        if (k == 1 ? (n < 1 || n > MAX_PAGE) : n > 1000000)
            return puddle_fail(w, 400, "bad_request");
        if (k == 1) limit = n; else offset = n;
    }
    puddle_identity_id(c->ctx->identity, me);
    puddle_dm_chat_id(me, v[0], chat);

    puddle_event_list tail;
    if (puddle_store_tail(c->ctx->store, chat, offset + limit + 1, &tail, err, sizeof err) != 0)
        return puddle_fail(w, 500, "internal");
    /* newest first, up to the limit and what fits; then written oldest first */
    size_t end = tail.count > offset ? tail.count - offset : 0, start = end, used = 0;
    while (start > 0 && end - start < limit) {
        size_t cost = tail.items[start - 1].event_len + 400;
        if (used + cost > PAGE_BUDGET && start < end)
            break;
        used += cost;
        start--;
    }
    puddle_jw_object(w);
    puddle_jw_key(w, "messages");
    puddle_jw_array(w);
    for (size_t i = start; i < end; i++)
        message_json(w, &tail.items[i], me);
    puddle_jw_end_array(w);
    puddle_jw_key(w, "next");
    if (start > 0)
        puddle_jw_uint(w, offset + (end - start));
    else
        puddle_jw_null(w);
    puddle_jw_end_object(w);
    puddle_event_list_free(&tail);
    return 200;
}

/* ---- a peer delivers events ---- */

typedef struct {
    puddle_jw *w;
    int started;
} result_writer;

static void reject(puddle_jw *w, const char *id, const char *reason)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id);
    puddle_jw_str(w, "reason", reason);
    puddle_jw_end_object(w);
}

/* One event from the peer `sender`: the reason it is refused, or NULL if it was stored (and `dup` says whether it
 * was already there). */
static const char *take_event(puddle_ctx *ctx, const char *sender, const unsigned char sender_pub[32],
                              const char *sender_kid, const char *chat, puddle_span ev, char id_out[PUDDLE_ID_HEX + 1], int *dup)
{
    puddle_span v;
    char claimed[PUDDLE_ID_HEX + 1], sigtext[PUDDLE_SIG_TEXT_MAX + 4];
    *dup = 0;
    id_out[0] = '\0';
    if (ev.len < 2 || ev.p[0] != '{' || !puddle_json_valid_object(ev.p, ev.len)) 
        return "malformed";
    if (puddle_json_member(ev.p, ev.len, "id", &v) != 0 || puddle_json_string(v, claimed, sizeof claimed) != 0 || strlen(claimed) != 64)
        return "malformed";
    snprintf(id_out, PUDDLE_ID_HEX + 1, "%s", claimed);
    if (puddle_json_member(ev.p, ev.len, "event", &v) != 0 || v.len < 2 || v.p[0] != '"' || v.len > PUDDLE_EVENT_MAX_BYTES * 6u)
        return "malformed";
    char *text = malloc(v.len + 1);
    if (!text || puddle_json_string(v, text, v.len + 1) != 0) {
        free(text);
        return "malformed";
    }
    size_t tl = strlen(text);
    const char *reason = NULL;
    puddle_message m;
    memset(&m, 0, sizeof m);
    char hash[65];
    unsigned char sig[64], pub[32];
    char kid[65];
    if (tl == 0 || tl > PUDDLE_EVENT_MAX_BYTES) {
        reason = "too_large";
        goto out;
    }
    puddle_sha256_hex(text, tl, hash);
    if (strcmp(hash, claimed) != 0) {
        reason = "bad_id";
        goto out;
    }
    if (puddle_json_member(ev.p, ev.len, "sig", &v) != 0 || puddle_json_string(v, sigtext, sizeof sigtext) != 0 ||
        puddle_event_sig_parse(sigtext, kid, sig) != 0) {
        reason = "bad_signature";
        goto out;
    }
    if (puddle_message_parse(text, tl, &m, &reason) != 0) 
        goto out;
    if (strcmp(m.chat, chat) != 0) {
        reason = "wrong_chat";
        goto out;
    }
    if (strcmp(m.sender, sender) != 0) {
        reason = "wrong_sender";
        goto out;
    }
    /* Only the key they use now. A retired key is what a stolen phone leaves behind; a message it signs after the
     * owner has changed keys is exactly what changing keys is meant to stop. */
    if (strcmp(kid, sender_kid) != 0) {
        reason = "unknown_key";
        goto out;
    }
    memcpy(pub, sender_pub, 32);
    if (!puddle_event_verify(pub, text, tl, sig)) {
        reason = "bad_signature";
        goto out;
    }
    {
        char err[300];
        int ar = puddle_store_append(ctx->store, chat, text, tl, sigtext, puddle_ctx_now_ms(ctx), id_out, err, sizeof err);
        if (ar < 0) {
            reason = "storage_error";
            puddle_log(PUDDLE_LOG_WARN, "cannot store a message from %.12s…: %s", sender, err);
        }
        else
            *dup = ar == PUDDLE_APPEND_DUPLICATE;
    }
out:
    puddle_message_free(&m);
    free(text);
    return reason;
}

static int events_post(const puddle_call *c, puddle_jw *w)
{
    const char *b = (const char *)c->body;
    puddle_span chat_v, evs, ev;
    char chat[PUDDLE_USER_ID_LEN + 1], me[PUDDLE_USER_ID_LEN + 1], want[PUDDLE_USER_ID_LEN + 1];

    /* the sender is a known peer (their key is how the request was checked), but may not have an open stream */
    puddle_stream st;
    if (puddle_streams_get(c->ctx->streams, c->sender, &st) != 0)
        return puddle_fail(w, 403, "no_stream");
    int active = st.state == PUDDLE_STREAM_ACTIVE && st.chain;
    char *chain = active ? strdup(st.chain) : NULL;
    size_t chain_len = active ? st.chain_len : 0;
    puddle_stream_free(&st);
    if (!chain)
        return puddle_fail(w, 403, "no_stream");

    puddle_identity_id(c->ctx->identity, me);
    puddle_dm_chat_id(me, c->sender, want);
    if (!puddle_json_valid_object(b, c->body_len) || puddle_json_member(b, c->body_len, "chat", &chat_v) != 0 ||
        puddle_json_string(chat_v, chat, sizeof chat) != 0 || strcmp(chat, want) != 0 ||
        puddle_json_member(b, c->body_len, "events", &evs) != 0) {
        free(chain);
        return puddle_fail(w, 400, "bad_request");
    }

    puddle_chain_head head;
    char err[200], kid[65];
    int cv = puddle_chain_verify(chain, chain_len, c->sender, &head, err, sizeof err);
    free(chain);
    if (cv != 0)
        return puddle_fail(w, 500, "internal");
    puddle_key_id(head.sign_pub, kid);

    /* first pass: count, so a delivery that is too long is refused whole rather than half taken */
    size_t cursor = 0, count = 0;
    int r;
    while ((r = puddle_json_array_next(evs, &cursor, &ev)) == 1)
        if (++count > PUDDLE_DELIVERY_MAX_EVENTS)
            return puddle_fail(w, 400, "too_many_events");
    if (r < 0)
        return puddle_fail(w, 400, "bad_request");

    char (*acc)[PUDDLE_ID_HEX + 1] = calloc(count ? count : 1, sizeof *acc);
    char (*dup)[PUDDLE_ID_HEX + 1] = calloc(count ? count : 1, sizeof *dup);
    char (*rej_id)[PUDDLE_ID_HEX + 1] = calloc(count ? count : 1, sizeof *rej_id);
    const char **rej_why = calloc(count ? count : 1, sizeof *rej_why);
    if (!acc || !dup || !rej_id || !rej_why) {
        free(acc); free(dup); free(rej_id); free(rej_why);
        return puddle_fail(w, 500, "internal");
    }
    size_t na = 0, nd = 0, nr = 0;
    cursor = 0;
    while (puddle_json_array_next(evs, &cursor, &ev) == 1) {
        char id[PUDDLE_ID_HEX + 1];
        int is_dup;
        const char *why = take_event(c->ctx, c->sender, head.sign_pub, kid, chat, ev, id, &is_dup);
        if (why) {
            snprintf(rej_id[nr], sizeof rej_id[nr], "%s", id);
            rej_why[nr++] = why;
        } else if (is_dup) {
            snprintf(dup[nd++], sizeof dup[0], "%s", id);
        } else {
            snprintf(acc[na++], sizeof acc[0], "%s", id);
        }
    }
    puddle_jw_object(w);
    puddle_jw_key(w, "accepted");
    puddle_jw_array(w);
    for (size_t i = 0; i < na; i++) puddle_jw_string(w, acc[i]);
    puddle_jw_end_array(w);
    puddle_jw_key(w, "duplicate");
    puddle_jw_array(w);
    for (size_t i = 0; i < nd; i++) puddle_jw_string(w, dup[i]);
    puddle_jw_end_array(w);
    puddle_jw_key(w, "rejected");
    puddle_jw_array(w);
    for (size_t i = 0; i < nr; i++) reject(w, rej_id[i], rej_why[i]);
    puddle_jw_end_array(w);
    puddle_jw_end_object(w);
    free(acc); free(dup); free(rej_id); free(rej_why);
    if (na)
        puddle_log(PUDDLE_LOG_INFO, "%zu message%s from %.12s…", na, na == 1 ? "" : "s", c->sender);
    return 200;
}

const puddle_route puddle_message_routes[] = {
    { "POST", "/api/v1/dm/send",     PUDDLE_AUTH_OWNER, 8192, send_dm, NULL },
    { "GET",  "/api/v1/dm/messages", PUDDLE_AUTH_OWNER, 0,    list_dm, NULL },
    { "POST", "/stream/v1/events",   PUDDLE_AUTH_PEER,  PUDDLE_DELIVERY_MAX_BYTES, events_post, NULL },
};
const size_t puddle_message_routes_count = sizeof puddle_message_routes / sizeof puddle_message_routes[0];
