#include "flow.h"

#include "access.h"
#include "keysync.h"
#include "profiles_api.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int puddle_my_address(const puddle_config *cfg, char *out, size_t cap)
{
    char where[PUDDLE_ADDR_MAX];
    if (cfg->advertise[0]) {
        snprintf(where, sizeof where, "%s", cfg->advertise);
    } else if (!strcmp(cfg->address, "::") || !strcmp(cfg->address, "0.0.0.0")) {
        return 0;
    } else if (strchr(cfg->address, ':')) {
        snprintf(where, sizeof where, "[%s]:%u", cfg->address, cfg->port);
    } else {
        snprintf(where, sizeof where, "%s:%u", cfg->address, cfg->port);
    }
    puddle_address a;
    if (puddle_address_parse(where, &a) != 0 || strlen(a.address) >= cap)
        return 0;
    memcpy(out, a.address, strlen(a.address) + 1);
    return 1;
}

/* ---- what we say about ourselves ---- */

/* {"id","address","profile","chain"}: who we are, as `peer` is shown us */
int puddle_flow_write_self(puddle_ctx *ctx, const char *peer, puddle_jw *w)
{
    char me[PUDDLE_USER_ID_LEN + 1], addr[PUDDLE_ADDR_MAX];
    if (!puddle_my_address(ctx->config, addr, sizeof addr))
        return -1;
    size_t cl;
    char *chain = puddle_identity_chain(ctx->identity, &cl);
    if (!chain)
        return -1;
    puddle_identity_id(ctx->identity, me);
    const puddle_profile *pr = ctx->profiles ? puddle_profile_for_peer(ctx->profiles, ctx->assignments, peer) : NULL;
    puddle_jw_object(w);
    puddle_jw_str(w, "id", me);
    puddle_jw_str(w, "address", addr);
    puddle_jw_key(w, "profile");
    puddle_jw_object(w);
    if (pr)
        puddle_profile_write_fields(w, pr);
    puddle_jw_end_object(w);
    puddle_jw_key(w, "chain");
    puddle_jw_string_n(w, chain, cl);
    puddle_jw_end_object(w);
    free(chain);
    return 0;
}

#define BODY_ROOM(chain_estimate) ((chain_estimate) * 6 + 24 * 1024)

static char *build_body(puddle_ctx *ctx, const char *peer, const char *kind, const char *message, const char *invite, int approved,
                        size_t *len)
{
    size_t cl = 0;
    char *c = puddle_identity_chain(ctx->identity, &cl);
    free(c);
    size_t cap = BODY_ROOM(cl);
    char *buf = malloc(cap);
    if (!buf)
        return NULL;
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    if (!strcmp(kind, "access")) {
        puddle_jw_key(&w, "requester");
        if (puddle_flow_write_self(ctx, peer, &w) != 0)
            goto bad;
        if (message && *message)
            puddle_jw_str(&w, "message", message);
        if (invite && *invite)
            puddle_jw_str(&w, "invite", invite);
    } else if (!strcmp(kind, "result")) {
        puddle_jw_key(&w, "responder");
        if (puddle_flow_write_self(ctx, peer, &w) != 0)
            goto bad;
        puddle_jw_flag(&w, "approved", approved);
    }
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    if (!n)
        goto bad;
    *len = n;
    return buf;
bad:
    free(buf);
    return NULL;
}

/* ---- asking ---- */

typedef struct {
    const puddle_link *link;
    const char *message;
    int64_t now;
} asking;

static int apply_asking(void *user, puddle_stream *st, int exists)
{
    asking *a = user;
    if (exists) {
        if (st->state == PUDDLE_STREAM_ACTIVE) return 2;
        if (st->state == PUDDLE_STREAM_PENDING_OUT) return 3;
        if (st->state == PUDDLE_STREAM_PENDING_IN) return 4;
    } else {
        st->created_ms = a->now;
    }
    st->state = PUDDLE_STREAM_PENDING_OUT;
    st->owed = PUDDLE_OWED_NONE;
    st->error[0] = '\0';
    st->updated_ms = a->now;
    snprintf(st->note, sizeof st->note, "%s", a->message);
    st->naddr = 0;
    st->addr_seq = 0;
    if (puddle_stream_add_address(st, a->link->address.address) != 0)
        return -1;
    return 0;
}

static int apply_failed(void *user, puddle_stream *st, int exists)
{
    (void)user;
    if (!exists || st->state != PUDDLE_STREAM_PENDING_OUT)
        return 1;
    st->state = PUDDLE_STREAM_CLOSED;
    snprintf(st->error, sizeof st->error, "the request could not be queued");
    return 0;
}

int puddle_flow_request(puddle_ctx *ctx, const puddle_link *link, const char *message, const char *profile_id,
                        char out_id[PUDDLE_USER_ID_LEN + 1], const char **code, const char **msg_out)
{
    char me[PUDDLE_USER_ID_LEN + 1], addr[PUDDLE_ADDR_MAX], clean[PUDDLE_NOTE_MAX_BYTES + 1], err[300];
    size_t chars;
    *msg_out = "";
    puddle_identity_id(ctx->identity, me);
    if (!strcmp(link->id, me)) {
        *code = "cannot_request_yourself";
        return 400;
    }
    if (puddle_clean_text(message ? message : "", message ? strlen(message) : 0, PUDDLE_NOTE_MAX_CHARS, 1, clean, sizeof clean, &chars) != 0) {
        *code = "bad_request";
        *msg_out = "the message is too long (500 characters at most)";
        return 400;
    }
    if (profile_id && !puddle_profiles_find(ctx->profiles, profile_id)) {
        *code = "unknown_profile";
        return 400;
    }
    if (!puddle_my_address(ctx->config, addr, sizeof addr)) {
        *code = "no_address";
        *msg_out = "this Puddle doesn't know its public address, so the other side couldn't answer; set `advertise: host:port` in puddle.yaml";
        return 409;
    }
    if (!ctx->outbox) {
        *code = "internal";
        return 500;
    }
    if (puddle_streams_is_blocked(ctx->streams, link->id)) {
        *code = "blocked";
        *msg_out = "you have blocked them; unblock them first";
        return 409;
    }
    asking a = { link, clean, puddle_ctx_now_ms(ctx) };
    int rc = puddle_streams_update(ctx->streams, link->id, apply_asking, &a, err, sizeof err);
    if (rc == 2 || rc == 3 || rc == 4) {
        *code = rc == 2 ? "already_streaming" : rc == 3 ? "already_requested" : "they_asked";
        if (rc == 4)
            *msg_out = "they have already asked you; accept their request";
        return 409;
    }
    if (rc != 0) {
        *code = "internal";
        *msg_out = "";
        return 500;
    }
    if (profile_id)
        puddle_assignments_set(ctx->assignments, link->id, profile_id, err, sizeof err);

    size_t len;
    char *body = build_body(ctx, link->id, "access", clean, link->invite, 0, &len);
    if (!body || puddle_outbox_enqueue(ctx->outbox, link->id, "access", "/stream/v1/access", body, len, err, sizeof err) != 0) {
        free(body);
        puddle_streams_update(ctx->streams, link->id, apply_failed, NULL, err, sizeof err);
        *code = "internal";
        return 500;
    }
    free(body);
    memcpy(out_id, link->id, PUDDLE_USER_ID_LEN + 1);
    return 0;
}

/* ---- owing ---- */

int puddle_flow_owe(puddle_ctx *ctx, const char *peer, puddle_owed owed)
{
    if (!ctx->outbox || owed == PUDDLE_OWED_NONE)
        return -1;
    char err[300];
    size_t len = 0;
    char *body = NULL;
    const char *kind = owed == PUDDLE_OWED_CLOSED ? "close" : "result";
    const char *path = owed == PUDDLE_OWED_CLOSED ? "/stream/v1/close" : "/stream/v1/access/result";
    if (owed == PUDDLE_OWED_CLOSED) {
        body = strdup("{}");
        len = 2;
    } else {
        body = build_body(ctx, peer, "result", NULL, NULL, owed == PUDDLE_OWED_APPROVED, &len);
    }
    int rc = body && puddle_outbox_enqueue(ctx->outbox, peer, kind, path, body, len, err, sizeof err) == 0 ? 0 : -1;
    if (rc != 0)
        puddle_log(PUDDLE_LOG_WARN, "cannot queue the %s for %.12s…; it will be tried again at the next start", kind, peer);
    free(body);
    return rc;
}

int puddle_flow_resume(puddle_ctx *ctx)
{
    if (!ctx->outbox)
        return 0;
    puddle_stream *all;
    size_t n;
    puddle_ob_item *items;
    size_t ni;
    if (puddle_streams_list(ctx->streams, &all, &n) != 0)
        return 0;
    if (puddle_outbox_list(ctx->outbox, &items, &ni) != 0) {
        puddle_stream_list_free(all, n);
        return 0;
    }
    int queued = 0;
    for (size_t i = 0; i < n; i++) {
        if (all[i].owed == PUDDLE_OWED_NONE)
            continue;
        const char *kind = all[i].owed == PUDDLE_OWED_CLOSED ? "close" : "result";
        int waiting = 0;
        for (size_t k = 0; k < ni; k++)
            if (items[k].state != PUDDLE_OB_FAILED && !strcmp(items[k].peer, all[i].id) && !strcmp(items[k].kind, kind))
                waiting = 1;
        if (!waiting && puddle_flow_owe(ctx, all[i].id, all[i].owed) == 0)
            queued++;
    }
    puddle_outbox_list_free(items, ni);
    puddle_stream_list_free(all, n);
    return queued;
}

/* ---- how it turned out ---- */

typedef struct {
    puddle_owed settled;      /* the debt this item was paying */
    const char *error;        /* "" or what to show the owner */
    int closes_request;       /* an `access` that was refused */
    int only_error;           /* just note the trouble: change nothing else */
} outcome_note;

static int apply_outcome(void *user, puddle_stream *st, int exists)
{
    outcome_note *o = user;
    if (!exists)
        return 1;
    if (o->only_error) {
        snprintf(st->error, sizeof st->error, "%s", o->error);
        return 0;
    }
    if (o->closes_request) {
        if (st->state != PUDDLE_STREAM_PENDING_OUT)
            return 1;
        st->state = PUDDLE_STREAM_CLOSED;
        snprintf(st->error, sizeof st->error, "%s", o->error);
        return 0;
    }
    if (st->owed != o->settled)
        return 1; /* something newer has been decided since; that one is still owed */
    st->owed = PUDDLE_OWED_NONE;
    snprintf(st->error, sizeof st->error, "%s", o->error);
    return 0;
}

void puddle_flow_done(void *user, const puddle_ob_item *item, puddle_ob_outcome outcome, const puddle_net_reply *reply)
{
    puddle_ctx *ctx = user;
    puddle_sync_done(ctx, item, outcome);
    puddle_profiles_done(ctx, item, outcome, reply);
    if (outcome == PUDDLE_OB_HOLD)
        return; /* waiting for the owner to trust the new certificate; the stream says so */
    char err[300], why[200];
    outcome_note o = { PUDDLE_OWED_NONE, "", 0, 0 };
    if (!strcmp(item->kind, "access")) {
        if (outcome == PUDDLE_OB_DELIVERED)
            return; /* it stays pending_out until they answer */
        snprintf(why, sizeof why, "they could not be asked: %.150s", item->reason);
        o.error = why;
        o.closes_request = 1;
    } else if (!strcmp(item->kind, "result") || !strcmp(item->kind, "close")) {
        /* the body says which answer this was; "close" is its own */
        if (!strcmp(item->kind, "close"))
            o.settled = PUDDLE_OWED_CLOSED;
        else
            o.settled = strstr(item->body, "\"approved\":true") ? PUDDLE_OWED_APPROVED : PUDDLE_OWED_DECLINED;
        if (outcome != PUDDLE_OB_DELIVERED) {
            snprintf(why, sizeof why, "they could not be told: %.150s", item->reason);
            o.error = why;
        }
    } else if (!strcmp(item->kind, "events")) {
        /* messages: a delivery that got through can still have had some refused; say so, and keep going */
        const char *rej = reply && reply->body ? strstr(reply->body, "\"rejected\":[{") : NULL;
        if (outcome == PUDDLE_OB_DELIVERED && !rej)
            return;
        if (outcome == PUDDLE_OB_DELIVERED) {
            const char *r = strstr(rej, "\"reason\":\"");
            char code[40] = "unknown";
            if (r) {
                r += 10;
                snprintf(code, sizeof code, "%.*s", (int)strcspn(r, "\""), r);
            }
            snprintf(why, sizeof why, "a message was refused by them: %s", code);
        } else {
            snprintf(why, sizeof why, "a message could not be delivered: %.150s", item->reason);
        }
        o.error = why;
        o.only_error = 1;
    } else {
        return;
    }
    puddle_streams_update(ctx->streams, item->peer, apply_outcome, &o, err, sizeof err);
}
