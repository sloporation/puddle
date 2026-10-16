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

/* What the outbox says about our deliveries to one person. A message that is no longer there has been delivered. */
typedef struct {
    puddle_ob_item *items;
    size_t n;
    char peer[PUDDLE_USER_ID_LEN + 1];
} deliveries;

static void deliveries_load(deliveries *d, const puddle_ctx *ctx, const char *peer)
{
    memset(d, 0, sizeof *d);
    snprintf(d->peer, sizeof d->peer, "%s", peer);
    if (ctx->outbox && puddle_outbox_list(ctx->outbox, &d->items, &d->n) != 0) {
        d->items = NULL;
        d->n = 0;
    }
}

static void deliveries_free(deliveries *d)
{
    puddle_outbox_list_free(d->items, d->n);
}

/* the delivery of the message `id`: the envelope's own "id" member, not the id of something that mentions it (an edit) */
static const puddle_ob_item *delivery_of(const deliveries *d, const char *id)
{
    char needle[PUDDLE_ID_HEX + 16];
    snprintf(needle, sizeof needle, "\"id\":\"%s\"", id);
    for (size_t i = 0; i < d->n; i++) {
        const puddle_ob_item *it = &d->items[i];
        if (!strcmp(it->kind, "events") && !strcmp(it->peer, d->peer) && it->body && strstr(it->body, needle))
            return it;
    }
    return NULL;
}

static void view_json(puddle_jw *w, const puddle_view_msg *m, const char *chat, const char *me, const deliveries *d)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", m->id);
    puddle_jw_str(w, "chat", chat);
    puddle_jw_str(w, "sender", m->sender);
    puddle_jw_num(w, "ts", m->ts);
    if (!m->deleted)
        puddle_jw_str(w, "body", m->body ? m->body : "");
    if (m->ref[0])
        puddle_jw_str(w, "reply_to", m->ref);
    puddle_jw_flag(w, "outgoing", strcmp(m->sender, me) == 0);
    if (m->edited && !m->deleted) {
        puddle_jw_flag(w, "edited", 1);
        puddle_jw_num(w, "edited_at", m->edited_ts);
    }
    if (m->deleted)
        puddle_jw_flag(w, "deleted", 1);
    puddle_jw_num(w, "received_at", m->rts);
    const puddle_ob_item *it = d && !m->deleted && !strcmp(m->sender, me) ? delivery_of(d, m->id) : NULL;
    if (it) {
        puddle_jw_key(w, "delivery");
        puddle_jw_object(w);
        puddle_jw_str(w, "state", it->state == PUDDLE_OB_FAILED ? "failed" : it->state == PUDDLE_OB_HELD ? "held" : it->attempts ? "retrying" : "queued");
        if (it->reason[0])
            puddle_jw_str(w, "reason", it->reason);
        if (it->state == PUDDLE_OB_QUEUED && it->attempts)
            puddle_jw_num(w, "next_at", it->next_ms);
        puddle_jw_num(w, "attempts", it->attempts);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_object(w);
}

/* ---- owner: sending an event of ours ---- */

/* Sign `event`, keep it in our chat and queue it for `to`. 0 on success (the id is in `id`), else the status with the error
 * written. If it is kept but cannot be queued, that is said (`not_queued`) rather than hidden. */
static int commit_event(const puddle_call *c, puddle_jw *w, const char *to, const char *chat, const char *event, size_t el,
                        char id[PUDDLE_ID_HEX + 1])
{
    char sig[PUDDLE_SIG_TEXT_MAX], err[300];
    if (puddle_event_sign(c->ctx->identity, event, el, sig) != 0)
        return puddle_fail(w, 500, "internal");
    if (puddle_store_append(c->ctx->store, chat, event, el, sig, 0, id, err, sizeof err) < 0)
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
        puddle_log(PUDDLE_LOG_WARN, "message %.12s… is stored but could not be queued for delivery", id);
        puddle_jw_object(w);
        puddle_jw_str(w, "error", "not_queued");
        puddle_jw_str(w, "id", id);
        puddle_jw_str(w, "message", "it was saved here but could not be queued to send");
        puddle_jw_end_object(w);
        return 500;
    }
    return 0;
}

/* Is there an open, unblocked stream with `to`? */
static int open_with(const puddle_call *c, const char *to)
{
    puddle_stream st;
    int have = puddle_streams_get(c->ctx->streams, to, &st);
    int active = have == 0 && st.state == PUDDLE_STREAM_ACTIVE;
    if (have == 0)
        puddle_stream_free(&st);
    return active && !puddle_streams_is_blocked(c->ctx->streams, to);
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
    if (!open_with(c, to)) {
        free(text);
        return puddle_fail(w, 403, "no_stream");
    }
    puddle_dm_chat_id(me, to, chat);

    char event[PUDDLE_EVENT_MAX_BYTES], id[PUDDLE_ID_HEX + 1];
    int64_t ts = puddle_event_next_ts(puddle_call_now_ms(c));
    size_t el = puddle_message_build(chat, me, ts, text, tl, ref, event, sizeof event);
    if (!el) {
        free(text);
        return puddle_fail(w, 400, "bad_request"); /* a reply reference that is not an id, or a message too large to send */
    }
    int st = commit_event(c, w, to, chat, event, el, id);
    if (st) {
        free(text);
        return st;
    }
    puddle_view_msg m;
    memset(&m, 0, sizeof m);
    snprintf(m.id, sizeof m.id, "%s", id);
    snprintf(m.sender, sizeof m.sender, "%s", me);
    snprintf(m.ref, sizeof m.ref, "%s", ref);
    m.ts = ts;
    m.rts = puddle_call_now_ms(c);
    m.body = text;
    deliveries d;
    deliveries_load(&d, c->ctx, to);
    view_json(w, &m, chat, me, &d);
    deliveries_free(&d);
    free(text);
    return 201;
}

/* ---- owner: changing or taking back what we said ---- */

/* The newest events of `chat`, widened until they reach the message `target` (everything done to it is newer), as a view.
 * 1 found it (copied to *out, body malloc'd), 0 not there, -1 error. */
static int find_in_chat(const puddle_call *c, const char *chat, const char *target, puddle_view_msg *out)
{
    size_t n = 128;
    for (;;) {
        puddle_event_list tail;
        char err[200];
        if (puddle_store_tail(c->ctx->store, chat, n, &tail, err, sizeof err) != 0)
            return -1;
        puddle_view_msg *v;
        size_t m;
        int rc = puddle_chat_view(tail.items, tail.count, &v, &m);
        int full = tail.count >= n;
        puddle_event_list_free(&tail);
        if (rc != 0)
            return -1;
        for (size_t i = 0; i < m; i++)
            if (!strcmp(v[i].id, target)) {
                *out = v[i];
                v[i].body = NULL;
                puddle_view_free(v, m);
                return 1;
            }
        puddle_view_free(v, m);
        if (!full || n > (1u << 22))
            return 0;
        n *= 8;
    }
}

/* The common start of an edit and a delete: who, which message, and that it is ours to touch. 0 and the pieces, or a status. */
static int target_of(const puddle_call *c, puddle_jw *w, const char *body_key, char to[PUDDLE_USER_ID_LEN + 1], char chat[PUDDLE_USER_ID_LEN + 1],
                     char me[PUDDLE_USER_ID_LEN + 1], puddle_view_msg *t, char **new_body)
{
    puddle_span v;
    const char *b = c->body_len ? (const char *)c->body : "{}";
    size_t bl = c->body_len ? c->body_len : 2;
    char user[128], msg[128];
    *new_body = NULL;
    if (!puddle_json_valid_object(b, bl) || puddle_json_member(b, bl, "user", &v) != 0 || puddle_json_string(v, user, sizeof user) != 0 ||
        !puddle_id_valid(user) || puddle_json_member(b, bl, "message", &v) != 0 || puddle_json_string(v, msg, sizeof msg) != 0 || strlen(msg) != 64)
        return puddle_fail(w, 400, "bad_request");
    if (body_key) {
        if (puddle_json_member(b, bl, body_key, &v) != 0 || v.len < 2 || v.p[0] != '"')
            return puddle_fail(w, 400, "bad_request");
        char *text = malloc(v.len + 1);
        if (!text || puddle_json_string(v, text, v.len + 1) != 0) {
            free(text);
            return puddle_fail(w, 400, "bad_request");
        }
        if (!puddle_message_body_ok(text, strlen(text))) {
            free(text);
            puddle_jw_object(w);
            puddle_jw_str(w, "error", "bad_body");
            puddle_jw_str(w, "message", "a message is 1 to 4000 characters of text, with no control characters");
            puddle_jw_end_object(w);
            return 400;
        }
        *new_body = text;
    }
    puddle_identity_id(c->ctx->identity, me);
    memcpy(to, user, PUDDLE_USER_ID_LEN + 1);
    if (!open_with(c, to)) {
        free(*new_body);
        *new_body = NULL;
        return puddle_fail(w, 403, "no_stream");
    }
    puddle_dm_chat_id(me, to, chat);
    memset(t, 0, sizeof *t);
    int f = find_in_chat(c, chat, msg, t);
    if (f < 0 || (f == 1 && strcmp(t->sender, me) != 0)) {
        if (f == 1)
            puddle_view_clear(t);
        free(*new_body);
        *new_body = NULL;
        return f < 0 ? puddle_fail(w, 500, "internal") : puddle_fail(w, 404, "not_found"); /* someone else's is not ours to touch */
    }
    if (f == 0) {
        free(*new_body);
        *new_body = NULL;
        return puddle_fail(w, 404, "not_found");
    }
    return 0;
}

static int edit_dm(const puddle_call *c, puddle_jw *w)
{
    char to[PUDDLE_USER_ID_LEN + 1], chat[PUDDLE_USER_ID_LEN + 1], me[PUDDLE_USER_ID_LEN + 1], *text;
    puddle_view_msg t;
    int st = target_of(c, w, "body", to, chat, me, &t, &text);
    if (st)
        return st;
    if (t.deleted) {
        free(text);
        puddle_view_clear(&t);
        return puddle_fail(w, 409, "deleted");
    }
    char event[PUDDLE_EVENT_MAX_BYTES], id[PUDDLE_ID_HEX + 1];
    int64_t ts = puddle_event_next_ts(puddle_call_now_ms(c) > t.ts ? puddle_call_now_ms(c) : t.ts + 1); /* an edit has to be later than what it edits */
    size_t el = puddle_edit_build(chat, me, ts, t.id, text, strlen(text), event, sizeof event);
    if (!el) {
        free(text);
        puddle_view_clear(&t);
        return puddle_fail(w, 400, "bad_request");
    }
    st = commit_event(c, w, to, chat, event, el, id);
    if (!st) {
        free(t.body);
        t.body = text;
        text = NULL;
        t.edited = 1;
        t.edited_ts = ts;
        view_json(w, &t, chat, me, NULL);
        st = 200;
    }
    free(text);
    puddle_view_clear(&t);
    return st;
}

static int delete_dm(const puddle_call *c, puddle_jw *w)
{
    char to[PUDDLE_USER_ID_LEN + 1], chat[PUDDLE_USER_ID_LEN + 1], me[PUDDLE_USER_ID_LEN + 1], *text;
    puddle_view_msg t;
    int st = target_of(c, w, NULL, to, chat, me, &t, &text);
    if (st)
        return st;
    if (t.deleted) {
        puddle_view_clear(&t);
        return 204; /* already gone: nothing more to say */
    }
    char event[PUDDLE_EVENT_MAX_BYTES], id[PUDDLE_ID_HEX + 1];
    int64_t ts = puddle_event_next_ts(puddle_call_now_ms(c) > t.ts ? puddle_call_now_ms(c) : t.ts + 1);
    size_t el = puddle_redact_build(chat, me, ts, t.id, event, sizeof event);
    puddle_view_clear(&t);
    if (!el)
        return puddle_fail(w, 500, "internal");
    st = commit_event(c, w, to, chat, event, el, id);
    return st ? st : 204;
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

    /* Events, not messages, are what the store counts, and edits and redactions are events too: take a window of the newest
     * and widen it until it holds enough messages (or all there is). What was done to a message is newer than it, so
     * it is in the window whenever the message is. */
    size_t want = offset + limit + 1, n = want * 2 + 16;
    puddle_view_msg *view = NULL;
    size_t m = 0;
    for (;;) {
        puddle_event_list tail;
        if (puddle_store_tail(c->ctx->store, chat, n, &tail, err, sizeof err) != 0)
            return puddle_fail(w, 500, "internal");
        int full = tail.count >= n;
        int rc = puddle_chat_view(tail.items, tail.count, &view, &m);
        puddle_event_list_free(&tail);
        if (rc != 0)
            return puddle_fail(w, 500, "internal");
        if (m >= want || !full || n > (1u << 22))
            break;
        puddle_view_free(view, m);
        n *= 8;
    }
    /* newest first, up to the limit and what fits; then written oldest first */
    size_t end = m > offset ? m - offset : 0, start = end, used = 0;
    while (start > 0 && end - start < limit) {
        size_t cost = (view[start - 1].body ? strlen(view[start - 1].body) : 0) + 700;
        if (used + cost > PAGE_BUDGET && start < end)
            break;
        used += cost;
        start--;
    }
    puddle_jw_object(w);
    puddle_jw_key(w, "messages");
    puddle_jw_array(w);
    deliveries d;
    deliveries_load(&d, c->ctx, v[0]);
    for (size_t i = start; i < end; i++)
        view_json(w, &view[i], chat, me, &d);
    deliveries_free(&d);
    puddle_jw_end_array(w);
    puddle_jw_key(w, "next");
    if (start > 0)
        puddle_jw_uint(w, offset + (end - start));
    else
        puddle_jw_null(w);
    puddle_jw_end_object(w);
    puddle_view_free(view, m);
    return 200;
}

/* ---- owner: which conversations, and what has been read ---- */

static void preview_of(const char *body, char *out, size_t cap)
{
    size_t o = 0, chars = 0;
    for (const unsigned char *p = (const unsigned char *)body; *p && chars < 120;) {
        size_t k = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4; /* the body is valid UTF-8 */
        if (o + k + 1 > cap)
            break;
        if (*p == '\n' || *p == '\t')
            out[o++] = ' ';
        else
            for (size_t i = 0; i < k; i++)
                out[o++] = (char)p[i];
        p += k;
        chars++;
    }
    out[o] = '\0';
}

typedef struct {
    char user[PUDDLE_USER_ID_LEN + 1];
    puddle_stream_state state;
    int64_t last_ts;
    char preview[512];
    char last_id[65];
    char last_sender[PUDDLE_USER_ID_LEN + 1];
    int deleted;
    int unread;
} conversation;

static int cmp_conv(const void *a, const void *b)
{
    const conversation *x = a, *y = b;
    if (x->last_ts != y->last_ts)
        return x->last_ts < y->last_ts ? 1 : -1;
    return strcmp(x->user, y->user);
}

static int list_conversations(const puddle_call *c, puddle_jw *w)
{
    const char *keys[2] = { "limit", "offset" };
    char v[2][PUDDLE_QUERY_VALUE_MAX], me[PUDDLE_USER_ID_LEN + 1];
    if (puddle_query_parse(c->req->target, keys, 2, v) != 0)
        return puddle_fail(w, 400, "bad_request");
    size_t limit = DEFAULT_PAGE, offset = 0;
    for (int k = 0; k < 2; k++) {
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
        if (k == 0 ? (n < 1 || n > 100) : n > 1000000)
            return puddle_fail(w, 400, "bad_request");
        if (k == 0) limit = n; else offset = n;
    }
    puddle_identity_id(c->ctx->identity, me);

    puddle_stream *all;
    size_t n_streams;
    if (puddle_streams_list(c->ctx->streams, &all, &n_streams) != 0)
        return puddle_fail(w, 500, "internal");
    conversation *convs = calloc(n_streams ? n_streams : 1, sizeof *convs);
    if (!convs) {
        puddle_stream_list_free(all, n_streams);
        return puddle_fail(w, 500, "internal");
    }
    size_t n = 0;
    for (size_t i = 0; i < n_streams; i++) {
        char chat[PUDDLE_USER_ID_LEN + 1], err[200];
        puddle_dm_chat_id(me, all[i].id, chat);
        puddle_event_list tail;
        if (puddle_store_tail(c->ctx->store, chat, 200, &tail, err, sizeof err) != 0)
            continue;
        puddle_view_msg *view;
        size_t m;
        if (tail.count && puddle_chat_view(tail.items, tail.count, &view, &m) == 0) {
            if (m) {
                conversation *cv = &convs[n++];
                memcpy(cv->user, all[i].id, sizeof cv->user);
                cv->state = all[i].state;
                const puddle_view_msg *last = &view[m - 1];
                cv->last_ts = last->ts;
                snprintf(cv->last_id, sizeof cv->last_id, "%s", last->id);
                snprintf(cv->last_sender, sizeof cv->last_sender, "%s", last->sender);
                cv->deleted = last->deleted;
                preview_of(last->body ? last->body : "", cv->preview, sizeof cv->preview);
                int64_t marker = puddle_streams_read_marker(c->ctx->streams, all[i].id);
                for (size_t k = 0; k < m; k++)
                    if (strcmp(view[k].sender, me) != 0 && !view[k].deleted && view[k].rts > marker && cv->unread < 99)
                        cv->unread++;
            }
            puddle_view_free(view, m);
        }
        puddle_event_list_free(&tail);
    }
    puddle_stream_list_free(all, n_streams);
    qsort(convs, n, sizeof *convs, cmp_conv);

    puddle_jw_object(w);
    puddle_jw_key(w, "conversations");
    puddle_jw_array(w);
    size_t i = offset, shown = 0;
    for (; i < n && shown < limit && w->len + 1500 <= w->cap; i++, shown++) {
        const conversation *cv = &convs[i];
        puddle_jw_object(w);
        puddle_jw_str(w, "user", cv->user);
        puddle_jw_str(w, "stream", puddle_stream_state_name(cv->state));
        puddle_jw_key(w, "last");
        puddle_jw_object(w);
        puddle_jw_str(w, "id", cv->last_id);
        puddle_jw_str(w, "sender", cv->last_sender);
        puddle_jw_num(w, "ts", cv->last_ts);
        puddle_jw_flag(w, "outgoing", strcmp(cv->last_sender, me) == 0);
        if (cv->deleted)
            puddle_jw_flag(w, "deleted", 1);
        else
            puddle_jw_str(w, "preview", cv->preview);
        puddle_jw_end_object(w);
        puddle_jw_num(w, "unread", cv->unread);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
    puddle_jw_key(w, "next");
    if (i < n)
        puddle_jw_uint(w, i);
    else
        puddle_jw_null(w);
    puddle_jw_end_object(w);
    free(convs);
    return 200;
}

/* The owner has looked at the chat: everything that has arrived so far is read. */
static int mark_read(const puddle_call *c, puddle_jw *w)
{
    puddle_span v;
    const char *b = c->body_len ? (const char *)c->body : "{}";
    size_t bl = c->body_len ? c->body_len : 2;
    char user[128], err[200];
    if (!puddle_json_valid_object(b, bl) || puddle_json_member(b, bl, "user", &v) != 0 || puddle_json_string(v, user, sizeof user) != 0 ||
        !puddle_id_valid(user))
        return puddle_fail(w, 400, "bad_request");
    if (puddle_streams_set_read_marker(c->ctx->streams, user, puddle_call_now_ms(c), err, sizeof err) != 0)
        return puddle_fail(w, 500, "internal");
    return 204;
}

/* The owner asks for what is stuck to another go: what failed is queued again, and what is waiting is tried now. */
static int retry_delivery(const puddle_call *c, puddle_jw *w)
{
    puddle_span v;
    const char *b = c->body_len ? (const char *)c->body : "{}";
    size_t bl = c->body_len ? c->body_len : 2;
    char user[128];
    if (!puddle_json_valid_object(b, bl) || puddle_json_member(b, bl, "user", &v) != 0 || puddle_json_string(v, user, sizeof user) != 0 ||
        !puddle_id_valid(user))
        return puddle_fail(w, 400, "bad_request");
    if (!c->ctx->outbox)
        return puddle_fail(w, 404, "not_found");
    puddle_ob_item *items;
    size_t n;
    if (puddle_outbox_list(c->ctx->outbox, &items, &n) != 0)
        return puddle_fail(w, 500, "internal");
    int did = 0;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(items[i].peer, user) && !strcmp(items[i].kind, "events") && puddle_outbox_requeue(c->ctx->outbox, items[i].seq) == 0)
            did++;
    puddle_outbox_list_free(items, n);
    did += puddle_outbox_retry_now(c->ctx->outbox, user);
    return did ? 204 : puddle_fail(w, 404, "nothing_to_retry");
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
    if (puddle_chat_event_parse(text, tl, &m, &reason) != 0) 
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
    { "GET",  "/api/v1/conversations", PUDDLE_AUTH_OWNER, 0,  list_conversations, NULL },
    { "POST", "/api/v1/dm/retry",    PUDDLE_AUTH_OWNER, 1024, retry_delivery, NULL },
    { "POST", "/api/v1/dm/read",     PUDDLE_AUTH_OWNER, 1024, mark_read, NULL },
    { "POST", "/api/v1/dm/send",     PUDDLE_AUTH_OWNER, 8192, send_dm, NULL },
    { "POST", "/api/v1/dm/edit",     PUDDLE_AUTH_OWNER, 8192, edit_dm, NULL },
    { "POST", "/api/v1/dm/delete",   PUDDLE_AUTH_OWNER, 1024, delete_dm, NULL },
    { "GET",  "/api/v1/dm/messages", PUDDLE_AUTH_OWNER, 0,    list_dm, NULL },
    { "POST", "/stream/v1/events",   PUDDLE_AUTH_PEER,  PUDDLE_DELIVERY_MAX_BYTES, events_post, NULL },
};
const size_t puddle_message_routes_count = sizeof puddle_message_routes / sizeof puddle_message_routes[0];
