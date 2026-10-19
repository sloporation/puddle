#include "groupapi.h"

#include "access.h"
#include "client.h"
#include "groupflow.h"
#include "log.h"
#include "message.h"
#include "messages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---- */

static int string_member(puddle_span obj, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_member(obj.p, obj.len, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

/* does `sigtext` sign `event` as the current key of the peer `sender`? */
static int signed_by(puddle_ctx *ctx, const char *sender, const char *event, size_t el, const char *sigtext)
{
    char kid[65];
    unsigned char sig[64], pub[32];
    if (puddle_event_sig_parse(sigtext, kid, sig) != 0 || !ctx->peer_key || !ctx->peer_key(ctx->peer_key_user, sender, kid, pub))
        return 0;
    return puddle_event_verify(pub, event, el, sig);
}

static int open_peer_state(puddle_ctx *ctx, const char *peer, int friends_only)
{
    puddle_stream st;
    if (puddle_streams_get(ctx->streams, peer, &st) != 0)
        return 0;
    int ok = friends_only ? st.state == PUDDLE_STREAM_ACTIVE : puddle_stream_carries_groups(&st);
    puddle_stream_free(&st);
    return ok;
}

/* ---- being invited ---- */

typedef struct {
    puddle_group fresh;       /* what the invitation makes of the group */
    int64_t invite_ts;
    char me[PUDDLE_USER_ID_LEN + 1];
    int accepted;
} invited;

static int invited_fn(void *user, puddle_group *g, int exists)
{
    invited *in = user;
    if (exists) {
        if (g->my == PUDDLE_GROUP_IN || g->my == PUDDLE_GROUP_INVITED)
            return 1; /* we are in it, or already asked */
        /* left or declined: a later invitation asks again, an older one does not */
        puddle_gmember *mine = puddle_group_member(g, in->me);
        if (mine && in->invite_ts <= mine->ts)
            return 1;
    }
    puddle_group_free(g);
    *g = in->fresh;
    memset(&in->fresh, 0, sizeof in->fresh); /* the members now belong to the stored record */
    in->accepted = 1;
    return 0;
}

static int group_invite_post(const puddle_call *c, puddle_jw *w)
{
    puddle_ctx *ctx = c->ctx;
    const char *b = (const char *)c->body;
    puddle_span create, inv, roster, item, v;
    char gid[PUDDLE_USER_ID_LEN + 1], me[PUDDLE_USER_ID_LEN + 1];
    if (!ctx->groups || !ctx->store)
        return puddle_fail(w, 404, "not_found");
    if (!open_peer_state(ctx, c->sender, 1)) /* an invitation travels over a friendship */
        return puddle_fail(w, 403, "no_stream");
    puddle_identity_id(ctx->identity, me);
    if (!puddle_json_valid_object(b, c->body_len) || puddle_json_member(b, c->body_len, "group", &v) != 0 ||
        puddle_json_string(v, gid, sizeof gid) != 0 || !puddle_id_valid(gid) || puddle_json_member(b, c->body_len, "create", &create) != 0 ||
        puddle_json_member(b, c->body_len, "invite", &inv) != 0 || puddle_json_member(b, c->body_len, "members", &roster) != 0)
        return puddle_fail(w, 400, "bad_request");

    invited in;
    memset(&in, 0, sizeof in);
    puddle_group *g = &in.fresh;
    char ce[PUDDLE_GROUP_CREATE_MAX + 1], cs[180], ie[600], is[180];
    const char *why;
    puddle_gevent cev, iev;
    puddle_span tv;
    if (puddle_json_member(create.p, create.len, "event", &tv) != 0 || puddle_json_string(tv, ce, sizeof ce) != 0 ||
        string_member(create, "sig", cs, sizeof cs) != 0 || puddle_group_event_parse(ce, strlen(ce), &cev, &why) != 0 || cev.type != PUDDLE_GE_CREATE ||
        strcmp(cev.chat, gid) != 0)
        return puddle_fail(w, 400, "bad_request");
    if (puddle_json_member(inv.p, inv.len, "event", &tv) != 0 || puddle_json_string(tv, ie, sizeof ie) != 0 || string_member(inv, "sig", is, sizeof is) != 0 ||
        puddle_group_event_parse(ie, strlen(ie), &iev, &why) != 0 || iev.type != PUDDLE_GE_INVITE || strcmp(iev.chat, gid) != 0 ||
        strcmp(iev.sender, c->sender) != 0 || strcmp(iev.member, me) != 0)
        return puddle_fail(w, 400, "bad_request");
    if (!signed_by(ctx, c->sender, ie, strlen(ie), is)) /* the invitation is the sender's own word */
        return puddle_fail(w, 400, "bad_request");
    if (!strcmp(cev.sender, c->sender) && !signed_by(ctx, c->sender, ce, strlen(ce), cs))
        return puddle_fail(w, 400, "bad_request");

    snprintf(g->id, sizeof g->id, "%s", gid);
    snprintf(g->name, sizeof g->name, "%s", cev.name);
    g->name_ts = cev.ts;
    snprintf(g->creator, sizeof g->creator, "%s", cev.sender);
    g->created_ms = g->updated_ms = puddle_call_now_ms(c);
    g->my = PUDDLE_GROUP_INVITED;
    snprintf(g->invited_by, sizeof g->invited_by, "%s", c->sender);
    snprintf(g->create_event, sizeof g->create_event, "%s", ce);
    snprintf(g->create_sig, sizeof g->create_sig, "%s", cs);

    int have_sender = 0, have_me = 0, have_creator = 0;
    size_t cursor = 0;
    int r;
    while ((r = puddle_json_array_next(roster, &cursor, &item)) == 1) {
        char id[PUDDLE_USER_ID_LEN + 1], st[16], addr[PUDDLE_ADDR_MAX] = "";
        int64_t ts;
        if (string_member(item, "id", id, sizeof id) != 0 || !puddle_id_valid(id) || puddle_group_member(g, id) ||
            string_member(item, "state", st, sizeof st) != 0 || (strcmp(st, "member") && strcmp(st, "invited")) || g->nmembers >= PUDDLE_GROUP_MAX_MEMBERS) {
            puddle_group_free(g);
            return puddle_fail(w, 400, "bad_request");
        }
        puddle_span ts_v, av;
        if (puddle_json_member(item.p, item.len, "ts", &ts_v) != 0 || puddle_json_int64(ts_v, &ts) != 0 || ts < 0) {
            puddle_group_free(g);
            return puddle_fail(w, 400, "bad_request");
        }
        int arc = puddle_json_member(item.p, item.len, "address", &av);
        if (arc < 0 || (arc == 0 && (puddle_json_string(av, addr, sizeof addr) != 0))) {
            puddle_group_free(g);
            return puddle_fail(w, 400, "bad_request");
        }
        puddle_address pa;
        if (addr[0] && (puddle_address_parse(addr, &pa) != 0 || !puddle_net_host_plausible(pa.host)))
            addr[0] = '\0'; /* an address we would never connect to is not kept */
        puddle_gmember *m = realloc(g->members, (g->nmembers + 1) * sizeof *m);
        if (!m) {
            puddle_group_free(g);
            return puddle_fail(w, 500, "internal");
        }
        g->members = m;
        m = &g->members[g->nmembers++];
        memset(m, 0, sizeof *m);
        snprintf(m->id, sizeof m->id, "%s", id);
        m->state = !strcmp(st, "member") ? PUDDLE_GM_MEMBER : PUDDLE_GM_INVITED;
        m->ts = ts;
        snprintf(m->address, sizeof m->address, "%s", addr);
        if (!strcmp(id, c->sender))
            have_sender = m->state == PUDDLE_GM_MEMBER;
        if (!strcmp(id, me)) {
            have_me = m->state == PUDDLE_GM_INVITED;
            m->ts = iev.ts; /* our answer has to be newer than this invitation */
            snprintf(m->by, sizeof m->by, "%s", c->sender);
        }
        if (!strcmp(id, cev.sender))
            have_creator = 1;
    }
    if (r < 0 || !have_sender || !have_me || !have_creator) {
        puddle_group_free(g);
        return puddle_fail(w, 400, "bad_request");
    }

    if (puddle_groups_pending(ctx->groups) >= PUDDLE_GROUP_MAX_PENDING_IN) {
        puddle_group_free(g);
        puddle_log(PUDDLE_LOG_INFO, "group invitation from %.12s… dropped: too many waiting", c->sender);
        return 202;
    }
    in.invite_ts = iev.ts;
    snprintf(in.me, sizeof in.me, "%s", me);
    char err[300], id[65];
    int ur = puddle_groups_update(ctx->groups, gid, invited_fn, &in, err, sizeof err);
    puddle_group_free(&in.fresh);
    if (ur == 0) {
        puddle_store_append(ctx->store, gid, ce, strlen(ce), cs, 0, id, err, sizeof err);
        puddle_store_append(ctx->store, gid, ie, strlen(ie), is, 0, id, err, sizeof err);
        puddle_log(PUDDLE_LOG_INFO, "%.12s… invited us to a group", c->sender);
    }
    return 202;
}

/* ---- events in a group ---- */

typedef struct {
    puddle_ctx *ctx;
    const char *sender;
    const char *chat;
    char me[PUDDLE_USER_ID_LEN + 1];
} gpolicy;

static const char *group_judge(void *user, const char *text, size_t tl)
{
    gpolicy *p = user;
    puddle_gevent ge;
    const char *why;
    puddle_group g;
    if (puddle_group_event_parse(text, tl, &ge, &why) == 0) {
        if (ge.type == PUDDLE_GE_CREATE)
            return "use_invite"; /* a group is first heard of through an invitation */
        if (strcmp(ge.chat, p->chat) != 0)
            return "wrong_chat";
        if (strcmp(ge.sender, p->sender) != 0)
            return "wrong_sender";
        return NULL; /* the rules are applied when it is taken */
    }
    if (strcmp(why, "unsupported_type") != 0)
        return why;
    puddle_message m;
    if (puddle_chat_event_parse(text, tl, &m, &why) != 0)
        return why;
    const char *reason = NULL;
    if (strcmp(m.chat, p->chat) != 0)
        reason = "wrong_chat";
    else if (strcmp(m.sender, p->sender) != 0)
        reason = "wrong_sender";
    puddle_message_free(&m);
    if (reason)
        return reason;
    if (puddle_groups_get(p->ctx->groups, p->chat, &g) != 0)
        return "not_in_group";
    puddle_gmember *from = puddle_group_member(&g, p->sender);
    if (g.my != PUDDLE_GROUP_IN)
        reason = "not_in_group";
    else if (!from || from->state != PUDDLE_GM_MEMBER)
        reason = "not_a_member";
    puddle_group_free(&g);
    return reason;
}

typedef struct {
    puddle_gevent ev;
    const char *why;
    int applied;
} applying;

static int apply_fn(void *user, puddle_group *g, int exists)
{
    applying *a = user;
    if (!exists || (g->my != PUDDLE_GROUP_IN && g->my != PUDDLE_GROUP_INVITED)) {
        a->why = "not_in_group";
        return 3;
    }
    a->applied = puddle_group_apply(g, &a->ev, &a->why);
    if (a->applied < 0)
        return 3;
    if (a->applied > 0)
        return 1; /* nothing changed: nothing to store */
    g->updated_ms = a->ev.ts;
    return 0;
}

static const char *group_commit(void *user, const char *text, size_t tl, const char *sigtext, char id_out[PUDDLE_ID_HEX + 1], int *dup)
{
    gpolicy *p = user;
    puddle_gevent ge;
    const char *why;
    if (puddle_group_event_parse(text, tl, &ge, &why) == 0) {
        applying a = { .ev = ge };
        char err[300];
        int ur = puddle_groups_update(p->ctx->groups, p->chat, apply_fn, &a, err, sizeof err);
        if (ur == 3)
            return a.why ? a.why : "bad_event";
        if (ur < 0)
            return "storage_error";
        const char *stored = puddle_store_delivered(p->ctx, p->chat, p->sender, text, tl, sigtext, id_out, dup);
        if (!stored && ur == 0) {
            puddle_log(PUDDLE_LOG_INFO, "group %.12s…: %s from %.12s…", p->chat, ge.type == PUDDLE_GE_JOIN ? "a join" : ge.type == PUDDLE_GE_LEAVE ? "a leave" :
                       ge.type == PUDDLE_GE_INVITE ? "an invitation" : "a new name", p->sender);
            puddle_group_reconcile(p->ctx);
        }
        return stored;
    }
    return puddle_store_delivered(p->ctx, p->chat, p->sender, text, tl, sigtext, id_out, dup);
}

int puddle_group_events_post(const puddle_call *c, puddle_jw *w, const char *chat)
{
    puddle_ctx *ctx = c->ctx;
    const char *b = (const char *)c->body;
    puddle_span evs;
    puddle_group g;
    if (!ctx->groups || puddle_groups_get(ctx->groups, chat, &g) != 0) /* no such chat here: what a direct message would say */
        return puddle_fail(w, 400, "bad_request");
    puddle_group_free(&g);

    puddle_stream st;
    if (puddle_streams_get(ctx->streams, c->sender, &st) != 0)
        return puddle_fail(w, 403, "no_stream");
    int open = puddle_stream_carries_groups(&st) && st.chain;
    char *chain = open ? strdup(st.chain) : NULL;
    size_t chain_len = open ? st.chain_len : 0;
    puddle_stream_free(&st);
    if (!chain)
        return puddle_fail(w, 403, "no_stream");
    if (puddle_json_member(b, c->body_len, "events", &evs) != 0) {
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

    gpolicy gp = { ctx, c->sender, chat, "" };
    puddle_identity_id(ctx->identity, gp.me);
    puddle_event_policy pol = { group_judge, group_commit, &gp };
    return puddle_process_delivery(c, w, evs, &pol, head.sign_pub, kid);
}

/* ================= the owner's routes ================= */

static const char *body_text(const puddle_call *c, size_t *len)
{
    *len = c->body_len ? c->body_len : 2;
    return c->body_len ? (const char *)c->body : "{}";
}

/* "key": a string member of the body, copied; 0 or -1 */
static int body_string(const puddle_call *c, const char *key, char *out, size_t cap)
{
    size_t bl;
    const char *b = body_text(c, &bl);
    puddle_span v;
    return puddle_json_valid_object(b, bl) && puddle_json_member(b, bl, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

static int body_id(const puddle_call *c, const char *key, char out[PUDDLE_USER_ID_LEN + 1])
{
    char tmp[128];
    if (body_string(c, key, tmp, sizeof tmp) != 0 || !puddle_id_valid(tmp))
        return -1;
    memcpy(out, tmp, PUDDLE_USER_ID_LEN + 1);
    return 0;
}

static int answer(puddle_jw *w, int status, const char *code)
{
    return puddle_fail(w, status, code);
}

static void write_group(puddle_jw *w, const puddle_group *g, const char *not_invited)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", g->id);
    puddle_jw_str(w, "name", g->name);
    puddle_jw_str(w, "my", puddle_group_my_name(g->my));
    puddle_jw_str(w, "creator", g->creator);
    if (g->invited_by[0] && g->my == PUDDLE_GROUP_INVITED)
        puddle_jw_str(w, "invited_by", g->invited_by);
    puddle_jw_num(w, "created_at", g->created_ms);
    puddle_jw_num(w, "updated_at", g->updated_ms);
    puddle_jw_key(w, "members");
    puddle_jw_array(w);
    for (size_t i = 0; i < g->nmembers; i++) {
        if (g->members[i].state == PUDDLE_GM_LEFT)
            continue;
        puddle_jw_object(w);
        puddle_jw_str(w, "id", g->members[i].id);
        puddle_jw_str(w, "state", puddle_gm_state_name(g->members[i].state));
        if (g->members[i].by[0])
            puddle_jw_str(w, "by", g->members[i].by);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
    if (not_invited && not_invited[0]) { /* those named at creation who could not be invited, with why: [{"user","reason"}] */
        puddle_jw_key(w, "not_invited");
        char list[2100];
        int n = snprintf(list, sizeof list, "[%s]", not_invited);
        puddle_jw_raw(w, list, (size_t)n);
    }
    puddle_jw_end_object(w);
}

static int cmp_updated(const void *a, const void *b)
{
    const puddle_group *x = a, *y = b;
    if (x->updated_ms != y->updated_ms)
        return x->updated_ms < y->updated_ms ? 1 : -1;
    return strcmp(x->id, y->id);
}

static int list_groups(const puddle_call *c, puddle_jw *w)
{
    const char *keys[2] = { "limit", "offset" };
    char v[2][PUDDLE_QUERY_VALUE_MAX];
    size_t limit = 50, offset = 0;
    if (puddle_query_parse(c->req->target, keys, 2, v) != 0 || puddle_page_params(v[0], v[1], 100, &limit, &offset) != 0)
        return puddle_fail(w, 400, "bad_request");
    puddle_group *all;
    size_t n;
    if (!c->ctx->groups || puddle_groups_list(c->ctx->groups, &all, &n) != 0)
        return puddle_fail(w, 500, "internal");
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (all[i].my == PUDDLE_GROUP_IN || all[i].my == PUDDLE_GROUP_INVITED) {
            if (k != i) {
                puddle_group tmp = all[k];
                all[k] = all[i];
                all[i] = tmp;
            }
            k++;
        }
    qsort(all, k, sizeof *all, cmp_updated);
    puddle_jw_object(w);
    puddle_jw_key(w, "groups");
    puddle_jw_array(w);
    size_t i = offset, shown = 0;
    for (; i < k && shown < limit && w->len + 8000 <= w->cap; i++, shown++)
        write_group(w, &all[i], NULL);
    puddle_jw_end_array(w);
    puddle_jw_key(w, "next");
    if (i < k)
        puddle_jw_uint(w, i);
    else
        puddle_jw_null(w);
    puddle_jw_end_object(w);
    puddle_group_list_free(all, n);
    return 200;
}

static int respond_group(const puddle_call *c, puddle_jw *w, const char *gid, int status)
{
    puddle_group g;
    if (puddle_groups_get(c->ctx->groups, gid, &g) != 0)
        return puddle_fail(w, 404, "not_found");
    write_group(w, &g, NULL);
    puddle_group_free(&g);
    return status;
}

static int create_group_route(const puddle_call *c, puddle_jw *w)
{
    char name[PUDDLE_GROUP_NAME_MAX_BYTES + 8], gid[PUDDLE_USER_ID_LEN + 1];
    const char *code;
    if (body_string(c, "name", name, sizeof name) != 0)
        return puddle_fail(w, 400, "bad_request");
    size_t bl;
    const char *b = body_text(c, &bl);
    puddle_span members;
    int has_members = puddle_json_member(b, bl, "members", &members);
    if (has_members < 0 || (has_members == 0 && members.p[0] != '['))
        return puddle_fail(w, 400, "bad_request");
    int st = puddle_group_create(c->ctx, name, gid, &code);
    if (st)
        return answer(w, st, code);
    /* invite whoever was named; the ones who cannot be are reported, not fatal */
    char failed[2048] = "";
    size_t cursor = 0;
    puddle_span item;
    int r, asked = 0;
    while (has_members == 0 && (r = puddle_json_array_next(members, &cursor, &item)) == 1 && asked < PUDDLE_GROUP_MAX_MEMBERS) {
        char id[128];
        asked++;
        if (puddle_json_string(item, id, sizeof id) != 0 || !puddle_id_valid(id)) {
            continue;
        }
        const char *why;
        int ist = puddle_group_invite(c->ctx, gid, id, &why);
        if (ist) {
            size_t used = strlen(failed);
            snprintf(failed + used, sizeof failed - used, "%s{\"user\":\"%s\",\"reason\":\"%s\"}", used ? "," : "", id, why);
        }
    }
    puddle_group g;
    if (puddle_groups_get(c->ctx->groups, gid, &g) != 0)
        return puddle_fail(w, 500, "internal");
    write_group(w, &g, failed);
    puddle_group_free(&g);
    return 201;
}

static int invite_route(const puddle_call *c, puddle_jw *w)
{
    char gid[PUDDLE_USER_ID_LEN + 1], user[PUDDLE_USER_ID_LEN + 1];
    const char *code;
    if (body_id(c, "group", gid) != 0 || body_id(c, "user", user) != 0)
        return puddle_fail(w, 400, "bad_request");
    int st = puddle_group_invite(c->ctx, gid, user, &code);
    return st ? answer(w, st, code) : 204;
}

static int accept_route(const puddle_call *c, puddle_jw *w)
{
    char gid[PUDDLE_USER_ID_LEN + 1];
    const char *code;
    if (body_id(c, "group", gid) != 0)
        return puddle_fail(w, 400, "bad_request");
    int st = puddle_group_accept(c->ctx, gid, &code);
    return st ? answer(w, st, code) : respond_group(c, w, gid, 200);
}

static int decline_route(const puddle_call *c, puddle_jw *w)
{
    char gid[PUDDLE_USER_ID_LEN + 1];
    const char *code;
    if (body_id(c, "group", gid) != 0)
        return puddle_fail(w, 400, "bad_request");
    int st = puddle_group_decline(c->ctx, gid, &code);
    return st ? answer(w, st, code) : 204;
}

static int leave_route(const puddle_call *c, puddle_jw *w)
{
    char gid[PUDDLE_USER_ID_LEN + 1];
    const char *code;
    if (body_id(c, "group", gid) != 0)
        return puddle_fail(w, 400, "bad_request");
    int st = puddle_group_leave(c->ctx, gid, &code);
    return st ? answer(w, st, code) : 204;
}

static int rename_route(const puddle_call *c, puddle_jw *w)
{
    char gid[PUDDLE_USER_ID_LEN + 1], name[PUDDLE_GROUP_NAME_MAX_BYTES + 8];
    const char *code;
    if (body_id(c, "group", gid) != 0 || body_string(c, "name", name, sizeof name) != 0)
        return puddle_fail(w, 400, "bad_request");
    int st = puddle_group_rename(c->ctx, gid, name, &code);
    return st ? answer(w, st, code) : respond_group(c, w, gid, 200);
}

/* the group's chat, with us as a member of it: 0, or the status with the error written */
static int member_group(const puddle_call *c, puddle_jw *w, const char *gid, char me[PUDDLE_USER_ID_LEN + 1])
{
    puddle_group g;
    if (!c->ctx->groups || puddle_groups_get(c->ctx->groups, gid, &g) != 0)
        return puddle_fail(w, 404, "not_found");
    int in = g.my == PUDDLE_GROUP_IN;
    puddle_group_free(&g);
    if (!in)
        return puddle_fail(w, 409, "not_in_group");
    puddle_identity_id(c->ctx->identity, me);
    return 0;
}

static int send_route(const puddle_call *c, puddle_jw *w)
{
    char gid[PUDDLE_USER_ID_LEN + 1], me[PUDDLE_USER_ID_LEN + 1], ref[128] = "";
    size_t bl;
    const char *b = body_text(c, &bl);
    puddle_span v;
    if (body_id(c, "group", gid) != 0 || puddle_json_member(b, bl, "body", &v) != 0 || v.len < 2 || v.p[0] != '"')
        return puddle_fail(w, 400, "bad_request");
    char *text = malloc(v.len + 1);
    if (!text || puddle_json_string(v, text, v.len + 1) != 0) {
        free(text);
        return puddle_fail(w, 400, "bad_request");
    }
    size_t tl = strlen(text);
    int rc = puddle_json_member(b, bl, "reply_to", &v);
    if (rc < 0 || (rc == 0 && puddle_json_string(v, ref, sizeof ref) != 0)) {
        free(text);
        return puddle_fail(w, 400, "bad_request");
    }
    if (!puddle_message_body_ok(text, tl)) {
        free(text);
        puddle_jw_object(w);
        puddle_jw_str(w, "error", "bad_body");
        puddle_jw_str(w, "message", "a message is 1 to 4000 characters of text, with no control characters");
        puddle_jw_end_object(w);
        return 400;
    }
    int st = member_group(c, w, gid, me);
    if (st) {
        free(text);
        return st;
    }
    char id[65];
    int64_t ts = puddle_event_next_ts(puddle_call_now_ms(c));
    const char *code;
    int sent;
    st = puddle_group_send_text(c->ctx, gid, 0, ts, text, tl, ref, id, &sent, &code);
    if (st) {
        free(text);
        if (!strcmp(code, "not_queued")) {
            puddle_jw_object(w);
            puddle_jw_str(w, "error", "not_queued");
            puddle_jw_str(w, "message", "it was saved here but could not be queued to send");
            puddle_jw_end_object(w);
            return 500;
        }
        return answer(w, st, code);
    }
    puddle_view_msg m;
    memset(&m, 0, sizeof m);
    snprintf(m.id, sizeof m.id, "%s", id);
    snprintf(m.sender, sizeof m.sender, "%s", me);
    snprintf(m.ref, sizeof m.ref, "%s", ref);
    m.ts = ts;
    m.rts = puddle_call_now_ms(c);
    m.body = text;
    puddle_message_json(c, w, &m, gid, me, "");
    free(text);
    return 201;
}

/* the message to edit or delete: ours, and in a group we are in */
static int own_message(const puddle_call *c, puddle_jw *w, char gid[PUDDLE_USER_ID_LEN + 1], char me[PUDDLE_USER_ID_LEN + 1], puddle_view_msg *t, char **new_body)
{
    char msg[128];
    if (new_body)
        *new_body = NULL;
    if (body_id(c, "group", gid) != 0 || body_string(c, "message", msg, sizeof msg) != 0 || strlen(msg) != 64)
        return puddle_fail(w, 400, "bad_request");
    size_t bl;
    const char *b = body_text(c, &bl);
    puddle_span v;
    if (new_body) {
        if (puddle_json_member(b, bl, "body", &v) != 0 || v.len < 2 || v.p[0] != '"')
            return puddle_fail(w, 400, "bad_request");
        char *text = malloc(v.len + 1);
        if (!text || puddle_json_string(v, text, v.len + 1) != 0 || !puddle_message_body_ok(text, strlen(text))) {
            free(text);
            return puddle_fail(w, 400, "bad_request");
        }
        *new_body = text;
    }
    int st = member_group(c, w, gid, me);
    if (st) {
        if (new_body) {
            free(*new_body);
            *new_body = NULL;
        }
        return st;
    }
    memset(t, 0, sizeof *t);
    int f = puddle_chat_find(c, gid, msg, t);
    if (f != 1 || strcmp(t->sender, me) != 0) {
        if (f == 1)
            puddle_view_clear(t);
        if (new_body) {
            free(*new_body);
            *new_body = NULL;
        }
        return f < 0 ? puddle_fail(w, 500, "internal") : puddle_fail(w, 404, "not_found");
    }
    return 0;
}

static int edit_route(const puddle_call *c, puddle_jw *w)
{
    char gid[PUDDLE_USER_ID_LEN + 1], me[PUDDLE_USER_ID_LEN + 1], *text;
    puddle_view_msg t;
    int st = own_message(c, w, gid, me, &t, &text);
    if (st)
        return st;
    if (t.deleted) {
        free(text);
        puddle_view_clear(&t);
        return puddle_fail(w, 409, "deleted");
    }
    char id[65];
    int64_t now = puddle_call_now_ms(c);
    int64_t ts = puddle_event_next_ts(now > t.ts ? now : t.ts + 1);
    const char *code;
    int sent;
    st = puddle_group_send_text(c->ctx, gid, 1, ts, text, strlen(text), t.id, id, &sent, &code);
    if (st == 0) {
        free(t.body);
        t.body = text;
        text = NULL;
        t.edited = 1;
        t.edited_ts = ts;
        puddle_message_json(c, w, &t, gid, me, "");
        st = 200;
    } else {
        st = answer(w, st, code);
    }
    free(text);
    puddle_view_clear(&t);
    return st;
}

static int delete_route(const puddle_call *c, puddle_jw *w)
{
    char gid[PUDDLE_USER_ID_LEN + 1], me[PUDDLE_USER_ID_LEN + 1];
    puddle_view_msg t;
    int st = own_message(c, w, gid, me, &t, NULL);
    if (st)
        return st;
    if (t.deleted) {
        puddle_view_clear(&t);
        return 204;
    }
    char event[PUDDLE_EVENT_MAX_BYTES], id[65];
    int64_t now = puddle_call_now_ms(c);
    int64_t ts = puddle_event_next_ts(now > t.ts ? now : t.ts + 1);
    size_t el = puddle_redact_build(gid, me, ts, t.id, event, sizeof event);
    puddle_view_clear(&t);
    if (!el)
        return puddle_fail(w, 500, "internal");
    const char *code;
    int sent;
    st = puddle_group_send_event(c->ctx, gid, event, el, id, &sent, &code);
    return st ? answer(w, st, code) : 204;
}

/* what happened to the group, for the chat to show between the messages: "X joined", "Y left", "named it ..." */
static void write_notices(void *user, puddle_jw *w, const puddle_event *events, size_t count, int64_t since_ts)
{
    (void)user;
    static const char *const names[] = { "create", "rename", "invite", "join", "leave" };
    puddle_jw_key(w, "notices");
    puddle_jw_array(w);
    size_t from = count > 200 ? count - 200 : 0;
    for (size_t i = from; i < count; i++) {
        puddle_gevent g;
        const char *why;
        if (puddle_group_event_parse(events[i].event, events[i].event_len, &g, &why) != 0 || g.ts < since_ts || w->len + 600 > w->cap)
            continue;
        puddle_jw_object(w);
        puddle_jw_str(w, "id", events[i].id);
        puddle_jw_str(w, "type", names[g.type]);
        puddle_jw_str(w, "sender", g.sender);
        puddle_jw_num(w, "ts", g.ts);
        if (g.type == PUDDLE_GE_INVITE)
            puddle_jw_str(w, "member", g.member);
        if (g.type == PUDDLE_GE_CREATE || g.type == PUDDLE_GE_RENAME)
            puddle_jw_str(w, "name", g.name);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
}

static int messages_route(const puddle_call *c, puddle_jw *w)
{
    const char *keys[3] = { "group", "limit", "offset" };
    char v[3][PUDDLE_QUERY_VALUE_MAX], me[PUDDLE_USER_ID_LEN + 1];
    if (puddle_query_parse(c->req->target, keys, 3, v) != 0 || !puddle_id_valid(v[0]))
        return puddle_fail(w, 400, "bad_request");
    size_t limit = 50, offset = 0;
    if (puddle_page_params(v[1], v[2], 200, &limit, &offset) != 0)
        return puddle_fail(w, 400, "bad_request");
    puddle_group g;
    if (!c->ctx->groups || puddle_groups_get(c->ctx->groups, v[0], &g) != 0)
        return puddle_fail(w, 404, "not_found");
    puddle_group_free(&g);
    puddle_identity_id(c->ctx->identity, me);
    return puddle_chat_page(c, w, v[0], me, "", limit, offset, write_notices, NULL);
}

/* Try again to deliver what is stuck for the group's members: what failed goes back in the queue, and what is waiting is tried now. */
static int retry_route(const puddle_call *c, puddle_jw *w)
{
    char gid[PUDDLE_USER_ID_LEN + 1];
    if (body_id(c, "group", gid) != 0)
        return puddle_fail(w, 400, "bad_request");
    puddle_group g;
    if (!c->ctx->groups || !c->ctx->outbox || puddle_groups_get(c->ctx->groups, gid, &g) != 0)
        return puddle_fail(w, 404, "not_found");
    puddle_ob_item *items;
    size_t n;
    if (puddle_outbox_list(c->ctx->outbox, &items, &n) != 0) {
        puddle_group_free(&g);
        return puddle_fail(w, 500, "internal");
    }
    int did = 0;
    for (size_t k = 0; k < g.nmembers; k++) {
        if (g.members[k].state == PUDDLE_GM_LEFT)
            continue;
        for (size_t i = 0; i < n; i++)
            if (!strcmp(items[i].peer, g.members[k].id) && !strcmp(items[i].kind, "events") && items[i].body && strstr(items[i].body, gid) &&
                puddle_outbox_requeue(c->ctx->outbox, items[i].seq) == 0)
                did++;
        did += puddle_outbox_retry_now(c->ctx->outbox, g.members[k].id);
    }
    puddle_outbox_list_free(items, n);
    puddle_group_free(&g);
    return did ? 204 : puddle_fail(w, 404, "nothing_to_retry");
}

const puddle_route puddle_group_routes[] = {
    { "POST", "/stream/v1/group/invite", PUDDLE_AUTH_PEER, 64 * 1024, group_invite_post, NULL },
    { "GET",  "/api/v1/gm",              PUDDLE_AUTH_OWNER, 0, list_groups, NULL },
    { "POST", "/api/v1/gm/create",       PUDDLE_AUTH_OWNER, 8192, create_group_route, NULL },
    { "POST", "/api/v1/gm/invite",       PUDDLE_AUTH_OWNER, 1024, invite_route, NULL },
    { "POST", "/api/v1/gm/accept",       PUDDLE_AUTH_OWNER, 1024, accept_route, NULL },
    { "POST", "/api/v1/gm/decline",      PUDDLE_AUTH_OWNER, 1024, decline_route, NULL },
    { "POST", "/api/v1/gm/leave",        PUDDLE_AUTH_OWNER, 1024, leave_route, NULL },
    { "POST", "/api/v1/gm/rename",       PUDDLE_AUTH_OWNER, 2048, rename_route, NULL },
    { "POST", "/api/v1/gm/send",         PUDDLE_AUTH_OWNER, 8192, send_route, NULL },
    { "POST", "/api/v1/gm/edit",         PUDDLE_AUTH_OWNER, 8192, edit_route, NULL },
    { "POST", "/api/v1/gm/delete",       PUDDLE_AUTH_OWNER, 1024, delete_route, NULL },
    { "POST", "/api/v1/gm/retry",        PUDDLE_AUTH_OWNER, 1024, retry_route, NULL },
    { "GET",  "/api/v1/gm/messages",     PUDDLE_AUTH_OWNER, 0, messages_route, NULL },
};
const size_t puddle_group_routes_count = sizeof puddle_group_routes / sizeof puddle_group_routes[0];
