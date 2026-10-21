/* The owner's side of streams: what the app and phone clients call to see who has asked, answer them, close
 * streams, block people, make invite links and set who may ask (api/openapi.yaml, "streams" and "blocks").
 * Talking to the other Puddle about any of this is the outbox's job (roadmap 3.4, 3.5): these handlers only
 * change what we hold and record what we still owe them (`owed`). */
#include "owner_streams.h"

#include "access.h"
#include "flow.h"
#include "profiles_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PAGE 50
#define MAX_PAGE 200
#define ROOM_PER_ENTRY 12500 /* the largest one entry can be: a note and the longest `about` */
#define DAY_MS (24 * 3600 * 1000LL)

/* ---- reading the request ---- */

static const char *body_text(const puddle_call *c) { return c->body_len ? (const char *)c->body : "{}"; }
static size_t body_size(const puddle_call *c) { return c->body_len ? c->body_len : 2; }

static int body_ok(const puddle_call *c) { return puddle_json_valid_object(body_text(c), body_size(c)); }

static int member_id(const puddle_call *c, const char *key, char out[PUDDLE_USER_ID_LEN + 1])
{
    puddle_span v;
    char tmp[128];
    if (puddle_json_member(body_text(c), body_size(c), key, &v) != 0 || puddle_json_string(v, tmp, sizeof tmp) != 0 ||
        !puddle_id_valid(tmp))
        return -1;
    memcpy(out, tmp, PUDDLE_USER_ID_LEN + 1);
    return 0;
}

/* an optional integer: 1 present (in *out), 0 absent, -1 present but not an integer */
static int member_int(const puddle_call *c, const char *key, int64_t *out)
{
    puddle_span v;
    int rc = puddle_json_member(body_text(c), body_size(c), key, &v);
    if (rc == 1)
        return 0;
    return rc == 0 && puddle_json_int64(v, out) == 0 ? 1 : -1;
}

static int parse_uint(const char *s, size_t *out, size_t max)
{
    if (!*s || strlen(s) > 9)
        return -1;
    size_t n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        n = n * 10 + (size_t)(*s - '0');
    }
    if (n > max)
        return -1;
    *out = n;
    return 0;
}

/* ?limit=&offset= (and `extra` named parameters); defaults filled in */
static int paging(const puddle_call *c, const char *extra, size_t *limit, size_t *offset, char *extra_value)
{
    const char *keys[3] = { "limit", "offset", extra };
    char v[3][PUDDLE_QUERY_VALUE_MAX];
    if (puddle_query_parse(c->req->target, keys, extra ? 3 : 2, v) != 0)
        return -1;
    *limit = DEFAULT_PAGE;
    *offset = 0;
    if (v[0][0] && (parse_uint(v[0], limit, MAX_PAGE) != 0 || *limit == 0))
        return -1;
    if (v[1][0] && parse_uint(v[1], offset, 1000000) != 0)
        return -1;
    if (extra)
        snprintf(extra_value, 32, "%.31s", v[2]);
    return 0;
}

/* ---- writing answers ---- */

static void stream_json(puddle_jw *w, const puddle_ctx *ctx, const puddle_stream *st)
{
    char mine[PUDDLE_PROFILE_ID_MAX + 1];
    puddle_jw_object(w);
    puddle_jw_str(w, "id", st->id);
    puddle_jw_key(w, "peer");
    puddle_jw_object(w);
    puddle_jw_str(w, "id", st->id);
    if (st->profile) {
        puddle_jw_key(w, "profile");
        puddle_jw_raw(w, st->profile, strlen(st->profile));
    }
    puddle_jw_end_object(w);
    puddle_jw_str(w, "state", puddle_stream_state_name(st->state));
    if (st->note[0])
        puddle_jw_str(w, "message", st->note);
    if (st->naddr > 0)
        puddle_jw_str(w, "address", st->addresses[0]);
    if (st->tls_fp_seen[0])
        puddle_jw_flag(w, "certificate_changed", 1);
    if (st->provisional)
        puddle_jw_flag(w, "provisional", 1);
    if (st->error[0])
        puddle_jw_str(w, "error", st->error);
    if (ctx->assignments && puddle_assignments_get(ctx->assignments, st->id, mine) == 0)
        puddle_jw_str(w, "profile", mine);
    puddle_jw_num(w, "created_at", st->created_ms);
    puddle_jw_num(w, "updated_at", st->updated_ms);
    puddle_jw_end_object(w);
}

static int cmp_recent(const void *a, const void *b)
{
    const puddle_stream *x = a, *y = b;
    if (x->updated_ms != y->updated_ms)
        return x->updated_ms < y->updated_ms ? 1 : -1;
    return strcmp(x->id, y->id);
}

static int has_room(const puddle_jw *w) { return w->len + ROOM_PER_ENTRY <= w->cap; }

/* ---- streams ---- */

static int list_streams(const puddle_call *c, puddle_jw *w)
{
    size_t limit, offset;
    char state_name[32];
    puddle_stream_state want = PUDDLE_STREAM_ACTIVE;
    if (paging(c, "state", &limit, &offset, state_name) != 0 || (state_name[0] && puddle_stream_state_parse(state_name, &want) != 0))
        return puddle_fail(w, 400, "bad_request");

    puddle_stream *all;
    size_t n;
    if (puddle_streams_list(c->ctx->streams, &all, &n) != 0)
        return puddle_fail(w, 500, "internal");
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (!all[i].scoped_req && (!state_name[0] || all[i].state == want)) { /* a group request of ours is not someone to show */
            if (k != i) {
                puddle_stream tmp = all[k];
                all[k] = all[i];
                all[i] = tmp;
            }
            k++;
        }
    qsort(all, k, sizeof *all, cmp_recent);

    puddle_jw_object(w);
    puddle_jw_key(w, "streams");
    puddle_jw_array(w);
    size_t i = offset, shown = 0;
    for (; i < k && shown < limit && has_room(w); i++, shown++)
        stream_json(w, c->ctx, &all[i]);
    puddle_jw_end_array(w);
    puddle_jw_key(w, "next");
    if (i < k)
        puddle_jw_uint(w, i);
    else
        puddle_jw_null(w);
    puddle_jw_end_object(w);
    puddle_stream_list_free(all, n);
    return 200;
}

/* load the stream the request names; 0 and *st filled, or -1 with the error already written */
static int load(const puddle_call *c, puddle_jw *w, puddle_stream *st, int *status)
{
    char id[PUDDLE_USER_ID_LEN + 1];
    if (!body_ok(c) || member_id(c, "stream", id) != 0) {
        *status = puddle_fail(w, 400, "bad_request");
        return -1;
    }
    if (puddle_streams_get(c->ctx->streams, id, st) != 0) {
        *status = puddle_fail(w, 404, "not_found");
        return -1;
    }
    return 0;
}

typedef struct {
    unsigned allowed; /* states it may be applied to, a bit each */
    puddle_stream_state to;
    puddle_owed owed;
    int64_t now;
} transition;

static int apply_transition(void *user, puddle_stream *st, int exists)
{
    transition *t = user;
    if (!exists)
        return 2;
    if (!(t->allowed & (1u << st->state)))
        return 3;
    st->state = t->to;
    st->owed = t->owed;
    st->updated_ms = t->now;
    return 0;
}

/* Move the stream the request names to a new state, if it is in one of the `allowed` ones. The check and the
 * change are one step, so two requests can't both win. 0 and `id` filled, or the status with the error written. */
static int change(const puddle_call *c, puddle_jw *w, unsigned allowed, puddle_stream_state to, puddle_owed owed,
                  const char *conflict, char id[PUDDLE_USER_ID_LEN + 1])
{
    char err[300];
    if (!body_ok(c) || member_id(c, "stream", id) != 0)
        return puddle_fail(w, 400, "bad_request");
    transition t = { allowed, to, owed, puddle_call_now_ms(c) };
    int rc = puddle_streams_update(c->ctx->streams, id, apply_transition, &t, err, sizeof err);
    if (rc == 0)
        return 0;
    if (rc == 2)
        return puddle_fail(w, 404, "not_found");
    if (rc == 3)
        return puddle_fail(w, 409, conflict);
    puddle_jw_object(w);
    puddle_jw_str(w, "error", "internal");
    puddle_jw_str(w, "message", err);
    puddle_jw_end_object(w);
    return 500;
}

#define BIT(s) (1u << (s))

static int accept_stream(const puddle_call *c, puddle_jw *w)
{
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_stream st;
    int status = change(c, w, BIT(PUDDLE_STREAM_PENDING_IN), PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_APPROVED, "not_waiting", id);
    if (status)
        return status;
    puddle_flow_owe(c->ctx, id, PUDDLE_OWED_APPROVED); /* queue the news; the record says it is owed until it lands */
    puddle_media_want(c->ctx, id);                      /* and, after it, ask for the pictures in their profile */
    if (puddle_streams_get(c->ctx->streams, id, &st) != 0)
        return puddle_fail(w, 500, "internal");
    stream_json(w, c->ctx, &st);
    puddle_stream_free(&st);
    return 200;
}

static int decline_stream(const puddle_call *c, puddle_jw *w)
{
    char id[PUDDLE_USER_ID_LEN + 1];
    int status = change(c, w, BIT(PUDDLE_STREAM_PENDING_IN), PUDDLE_STREAM_CLOSED, PUDDLE_OWED_DECLINED, "not_waiting", id);
    if (!status)
        puddle_flow_owe(c->ctx, id, PUDDLE_OWED_DECLINED);
    return status ? status : 204;
}

/* a request waiting on you is answered with decline, so it is not one of these */
/* withdrawing a request to an address: nobody knows us there yet as far as we can tell, so there is nothing to say to them; the record
 * stays (closed) so that an answer that still comes can be recognised and closed on their side too */
static int close_quietly(void *user, puddle_stream *st, int exists)
{
    (void)user;
    if (!exists || !st->provisional || st->state != PUDDLE_STREAM_PENDING_OUT)
        return 1;
    st->state = PUDDLE_STREAM_CLOSED;
    return 0;
}

static int delete_stream(const puddle_call *c, puddle_jw *w)
{
    char id[PUDDLE_USER_ID_LEN + 1];
    if (member_id(c, "stream", id) == 0) {
        char err[200];
        if (puddle_streams_update(c->ctx->streams, id, close_quietly, NULL, err, sizeof err) == 0)
            return 204;
    }
    int status = change(c, w, BIT(PUDDLE_STREAM_ACTIVE) | BIT(PUDDLE_STREAM_PENDING_OUT), PUDDLE_STREAM_CLOSED,
                        PUDDLE_OWED_CLOSED, "not_open", id);
    if (!status)
        puddle_flow_owe(c->ctx, id, PUDDLE_OWED_CLOSED);
    return status ? status : 204;
}

/* Ask someone for a stream. */
static int request_stream(const puddle_call *c, puddle_jw *w)
{
    puddle_span v;
    char target[400], message[PUDDLE_NOTE_MAX_BYTES + 1] = "", profile[PUDDLE_PROFILE_ID_MAX + 2], lerr[100];
    puddle_link link;
    if (!body_ok(c) || puddle_json_member(body_text(c), body_size(c), "target", &v) != 0 ||
        puddle_json_string(v, target, sizeof target) != 0)
        return puddle_fail(w, 400, "bad_request");
    if (puddle_link_parse(target, &link, lerr, sizeof lerr) != 0) {
        puddle_jw_object(w);
        puddle_jw_str(w, "error", "bad_link");
        puddle_jw_str(w, "message", lerr);
        puddle_jw_end_object(w);
        return 400;
    }
    int rc = puddle_json_member(body_text(c), body_size(c), "message", &v);
    if (rc < 0 || (rc == 0 && puddle_json_string(v, message, sizeof message) != 0))
        return puddle_fail(w, 400, "bad_request");
    const char *profile_id = NULL;
    rc = puddle_json_member(body_text(c), body_size(c), "profile", &v);
    if (rc < 0 || (rc == 0 && (puddle_json_string(v, profile, sizeof profile) != 0 || !profile[0])))
        return puddle_fail(w, 400, "bad_request");
    if (rc == 0)
        profile_id = profile;

    char id[PUDDLE_USER_ID_LEN + 1];
    const char *code, *msg;
    int status = puddle_flow_request(c->ctx, &link, message, profile_id, id, &code, &msg);
    if (status != 0) {
        puddle_jw_object(w);
        puddle_jw_str(w, "error", code);
        if (msg && *msg)
            puddle_jw_str(w, "message", msg);
        puddle_jw_end_object(w);
        return status;
    }
    puddle_stream st;
    if (puddle_streams_get(c->ctx->streams, id, &st) != 0)
        return puddle_fail(w, 500, "internal");
    stream_json(w, c->ctx, &st);
    puddle_stream_free(&st);
    return 201;
}

/* Choose which of our profiles a stream sees. `profile: null` goes back to the default. */
static int set_stream_profile(const puddle_call *c, puddle_jw *w)
{
    puddle_span v;
    char id[PUDDLE_USER_ID_LEN + 1], pid[PUDDLE_PROFILE_ID_MAX + 8], err[300];
    const char *b = body_text(c);
    size_t bl = body_size(c);
    if (!body_ok(c) || member_id(c, "stream", id) != 0 || puddle_json_member(b, bl, "profile", &v) != 0)
        return puddle_fail(w, 400, "bad_request");
    int is_null = v.len == 4 && !memcmp(v.p, "null", 4);
    if (!is_null && (puddle_json_string(v, pid, sizeof pid) != 0 || !puddle_profiles_find(c->ctx->profiles, pid)))
        return puddle_fail(w, 400, "unknown_profile");
    puddle_stream st;
    if (puddle_streams_get(c->ctx->streams, id, &st) != 0)
        return puddle_fail(w, 404, "not_found");
    int active = st.state == PUDDLE_STREAM_ACTIVE;
    puddle_stream_free(&st);
    if (puddle_assignments_set(c->ctx->assignments, id, is_null ? NULL : pid, err, sizeof err) != 0)
        return puddle_fail(w, 500, "internal");
    if (active)
        puddle_profile_push(c->ctx, id); /* they are told at once; if they are away, as soon as they are back */
    if (puddle_streams_get(c->ctx->streams, id, &st) != 0)
        return puddle_fail(w, 500, "internal");
    stream_json(w, c->ctx, &st);
    puddle_stream_free(&st);
    return 200;
}

/* The owner has looked at a peer whose certificate changed and says the new one is fine. */
static int trust_certificate(const puddle_call *c, puddle_jw *w)
{
    puddle_stream st;
    int status;
    char err[300];
    if (load(c, w, &st, &status) != 0)
        return status;
    char id[PUDDLE_USER_ID_LEN + 1];
    memcpy(id, st.id, sizeof id); /* `st` is about to be reloaded, and must not be the thing the id is read from */
    int rc = puddle_streams_trust_fingerprint(c->ctx->streams, id, err, sizeof err);
    puddle_stream_free(&st);
    if (rc == 1)
        return puddle_fail(w, 409, "nothing_to_trust");
    if (rc != 0)
        return puddle_fail(w, 500, "internal");
    if (c->ctx->outbox)
        puddle_outbox_release(c->ctx->outbox, id);
    if (puddle_streams_get(c->ctx->streams, id, &st) != 0)
        return puddle_fail(w, 500, "internal");
    stream_json(w, c->ctx, &st);
    puddle_stream_free(&st);
    return 200;
}

/* ---- blocks ---- */

static int list_blocks(const puddle_call *c, puddle_jw *w)
{
    size_t limit, offset;
    if (paging(c, NULL, &limit, &offset, NULL) != 0)
        return puddle_fail(w, 400, "bad_request");
    size_t n = puddle_streams_blocked(c->ctx->streams, NULL, 0);
    puddle_block *all = calloc(n ? n : 1, sizeof *all);
    if (!all)
        return puddle_fail(w, 500, "internal");
    n = puddle_streams_blocked(c->ctx->streams, all, n);
    puddle_jw_object(w);
    puddle_jw_key(w, "blocks");
    puddle_jw_array(w);
    size_t i = offset;
    for (size_t shown = 0; i < n && shown < limit; i++, shown++) {
        puddle_jw_object(w);
        puddle_jw_str(w, "user", all[i].id);
        puddle_jw_num(w, "created_at", all[i].ts_ms);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
    puddle_jw_key(w, "next");
    if (i < n)
        puddle_jw_uint(w, i);
    else
        puddle_jw_null(w);
    puddle_jw_end_object(w);
    free(all);
    return 200;
}

static int block_user(const puddle_call *c, puddle_jw *w)
{
    char id[PUDDLE_USER_ID_LEN + 1], me[PUDDLE_USER_ID_LEN + 1], err[300];
    if (!body_ok(c) || member_id(c, "user", id) != 0)
        return puddle_fail(w, 400, "bad_request");
    puddle_identity_id(c->ctx->identity, me);
    if (!strcmp(id, me))
        return puddle_fail(w, 400, "cannot_block_yourself");
    if (puddle_streams_block(c->ctx->streams, id, puddle_call_now_ms(c), err, sizeof err) != 0) {
        puddle_jw_object(w);
        puddle_jw_str(w, "error", "internal");
        puddle_jw_str(w, "message", err);
        puddle_jw_end_object(w);
        return 500;
    }
    int64_t ts = 0; /* blocking someone already blocked keeps the first time */
    size_t n = puddle_streams_blocked(c->ctx->streams, NULL, 0);
    puddle_block *all = calloc(n ? n : 1, sizeof *all);
    if (all) {
        n = puddle_streams_blocked(c->ctx->streams, all, n);
        for (size_t i = 0; i < n; i++)
            if (!strcmp(all[i].id, id))
                ts = all[i].ts_ms;
        free(all);
    }
    puddle_jw_object(w);
    puddle_jw_str(w, "user", id);
    puddle_jw_num(w, "created_at", ts);
    puddle_jw_end_object(w);
    return 201;
}

static int unblock_user(const puddle_call *c, puddle_jw *w)
{
    char id[PUDDLE_USER_ID_LEN + 1], err[300];
    if (!body_ok(c) || member_id(c, "user", id) != 0)
        return puddle_fail(w, 400, "bad_request");
    int rc = puddle_streams_unblock(c->ctx->streams, id, err, sizeof err);
    if (rc == 1)
        return puddle_fail(w, 404, "not_found");
    if (rc != 0)
        return puddle_fail(w, 500, "internal");
    return 204;
}

/* ---- invites ---- */

static void invite_json(puddle_jw *w, const puddle_ctx *ctx, const puddle_invite *inv)
{
    char link[300], me[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(ctx->identity, me);
    puddle_jw_object(w);
    puddle_jw_key(w, "link");
    if (puddle_make_link(ctx->config, inv->token, link, sizeof link))
        puddle_jw_string(w, link);
    else
        puddle_jw_null(w);
    puddle_jw_str(w, "token", inv->token);
    puddle_jw_num(w, "expires_at", inv->expires_ms);
    puddle_jw_num(w, "uses", inv->uses);
    puddle_jw_end_object(w);
}

static int create_invite(const puddle_call *c, puddle_jw *w)
{
    char me[PUDDLE_USER_ID_LEN + 1], link[300], err[300];
    int64_t uses = 1, expires_in = 7 * 24 * 3600;
    if (!body_ok(c) || member_int(c, "uses", &uses) < 0 || member_int(c, "expires_in", &expires_in) < 0 || uses < 1 ||
        uses > 1000 || expires_in < 60 || expires_in > 30 * 24 * 3600)
        return puddle_fail(w, 400, "bad_request");
    puddle_identity_id(c->ctx->identity, me);
    if (!puddle_make_link(c->ctx->config, "abcdefghijklmnopqrstuvwxy2", link, sizeof link)) {
        puddle_jw_object(w);
        puddle_jw_str(w, "error", "no_address");
        puddle_jw_str(w, "message", "this Puddle doesn't know its public address; set `advertise: host:port` in puddle.yaml");
        puddle_jw_end_object(w);
        return 409;
    }
    puddle_invite inv;
    if (puddle_streams_invite_create(c->ctx->streams, puddle_call_now_ms(c), expires_in * 1000, (int)uses, &inv, err, sizeof err) != 0) {
        puddle_jw_object(w);
        puddle_jw_str(w, "error", "cannot_create");
        puddle_jw_str(w, "message", err);
        puddle_jw_end_object(w);
        return 409;
    }
    invite_json(w, c->ctx, &inv);
    return 201;
}

static int list_invites(const puddle_call *c, puddle_jw *w)
{
    puddle_invite all[PUDDLE_MAX_INVITES];
    size_t n = puddle_streams_invites(c->ctx->streams, puddle_call_now_ms(c), all, PUDDLE_MAX_INVITES);
    puddle_jw_object(w);
    puddle_jw_key(w, "invites");
    puddle_jw_array(w);
    for (size_t i = 0; i < n && i < PUDDLE_MAX_INVITES; i++)
        invite_json(w, c->ctx, &all[i]);
    puddle_jw_end_array(w);
    puddle_jw_end_object(w);
    return 200;
}

static int revoke_invite(const puddle_call *c, puddle_jw *w)
{
    puddle_span v;
    char token[64];
    if (!body_ok(c) || puddle_json_member(body_text(c), body_size(c), "token", &v) != 0 ||
        puddle_json_string(v, token, sizeof token) != 0)
        return puddle_fail(w, 400, "bad_request");
    int rc = puddle_streams_invite_revoke(c->ctx->streams, token);
    return rc == 0 ? 204 : rc == 1 ? puddle_fail(w, 404, "not_found") : puddle_fail(w, 500, "internal");
}

/* ---- who may ask ---- */

static int get_policy(const puddle_call *c, puddle_jw *w)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "access", puddle_access_name(puddle_streams_access(c->ctx->streams)));
    puddle_jw_end_object(w);
    return 200;
}

static int set_policy(const puddle_call *c, puddle_jw *w)
{
    puddle_span v;
    char name[32], err[300];
    puddle_access a;
    if (!body_ok(c) || puddle_json_member(body_text(c), body_size(c), "access", &v) != 0 ||
        puddle_json_string(v, name, sizeof name) != 0 || puddle_access_parse(name, &a) != 0)
        return puddle_fail(w, 400, "bad_request");
    if (puddle_streams_set_access(c->ctx->streams, a, err, sizeof err) != 0)
        return puddle_fail(w, 500, "internal");
    return get_policy(c, w);
}

const puddle_route puddle_owner_stream_routes[] = {
    { "GET",  "/api/v1/stream",                PUDDLE_AUTH_OWNER, 0,   list_streams,   NULL },
    { "POST", "/api/v1/stream/request",        PUDDLE_AUTH_OWNER, 2048, request_stream, NULL },
    { "POST", "/api/v1/stream/accept",         PUDDLE_AUTH_OWNER, 256, accept_stream,  NULL },
    { "POST", "/api/v1/stream/decline",        PUDDLE_AUTH_OWNER, 256, decline_stream, NULL },
    { "POST", "/api/v1/stream/delete",         PUDDLE_AUTH_OWNER, 256, delete_stream,  NULL },
    { "POST", "/api/v1/stream/profile",        PUDDLE_AUTH_OWNER, 256, set_stream_profile, NULL },
    { "POST", "/api/v1/stream/trust",          PUDDLE_AUTH_OWNER, 256, trust_certificate, NULL },
    { "GET",  "/api/v1/stream/invite",         PUDDLE_AUTH_OWNER, 0,   list_invites,   NULL },
    { "POST", "/api/v1/stream/invite",         PUDDLE_AUTH_OWNER, 256, create_invite,  NULL },
    { "POST", "/api/v1/stream/invite/revoke",  PUDDLE_AUTH_OWNER, 256, revoke_invite,  NULL },
    { "GET",  "/api/v1/stream/policy",         PUDDLE_AUTH_OWNER, 0,   get_policy,     NULL },
    { "POST", "/api/v1/stream/policy",         PUDDLE_AUTH_OWNER, 256, set_policy,     NULL },
    { "GET",  "/api/v1/user/block",            PUDDLE_AUTH_OWNER, 0,   list_blocks,    NULL },
    { "POST", "/api/v1/user/block",            PUDDLE_AUTH_OWNER, 512, block_user,     NULL },
    { "POST", "/api/v1/user/unblock",          PUDDLE_AUTH_OWNER, 256, unblock_user,   NULL },
};
const size_t puddle_owner_stream_routes_count = sizeof puddle_owner_stream_routes / sizeof puddle_owner_stream_routes[0];
