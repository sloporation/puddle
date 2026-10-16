#include "profiles_api.h"

#include "flow.h"
#include "log.h"
#include "media.h"
#include "wireprofile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---- */

static int is_active(puddle_ctx *ctx, const char *peer)
{
    puddle_stream st;
    int have = puddle_streams_get(ctx->streams, peer, &st);
    int ok = have == 0 && puddle_stream_carries_groups(&st); /* group members see each other's pictures */
    if (have == 0)
        puddle_stream_free(&st);
    return ok;
}

/* the hash in `?hash=<64 hex>`, or -1 */
static int query_hash(const puddle_call *c, char out[PUDDLE_QUERY_VALUE_MAX])
{
    const char *keys[1] = { "hash" };
    char v[1][PUDDLE_QUERY_VALUE_MAX];
    if (puddle_query_parse(c->req->target, keys, 1, v) != 0 || !puddle_media_hash_valid(v[0]))
        return -1;
    memcpy(out, v[0], PUDDLE_QUERY_VALUE_MAX);
    return 0;
}

static int blob_of(const puddle_call *c, const char *hash)
{
    unsigned char *data;
    size_t len;
    char mime[40];
    if (!c->ctx->config || puddle_media_read(c->ctx->config->data_dir, hash, PUDDLE_IMAGE_MAX_BYTES, &data, &len, mime) != 0)
        return 0;
    c->blob->data = data;
    c->blob->len = len;
    snprintf(c->blob->mime, sizeof c->blob->mime, "%s", mime);
    return 1;
}

/* ---- the owner: their profiles, and a picture ---- */

static int list_profiles(const puddle_call *c, puddle_jw *w)
{
    puddle_jw_array(w);
    for (size_t i = 0; c->ctx->profiles && i < c->ctx->profiles->count; i++) {
        const puddle_profile *p = &c->ctx->profiles->items[i];
        puddle_jw_object(w);
        puddle_jw_str(w, "id", p->id);
        puddle_jw_flag(w, "default", i == 0);
        puddle_profile_write_fields(w, p);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
    return 200;
}

static int owner_media(const puddle_call *c, puddle_jw *w)
{
    char hash[PUDDLE_QUERY_VALUE_MAX];
    if (query_hash(c, hash) != 0)
        return puddle_fail(w, 400, "bad_request");
    if (!blob_of(c, hash))
        return puddle_fail(w, 404, "not_found");
    return 200;
}

/* ---- a peer asks for one of our pictures ---- */

static int peer_media(const puddle_call *c, puddle_jw *w)
{
    char hash[PUDDLE_QUERY_VALUE_MAX];
    if (query_hash(c, hash) != 0 || !is_active(c->ctx, c->sender) || !c->ctx->profiles)
        return puddle_fail(w, 404, "not_found");
    const puddle_profile *p = puddle_profile_for_peer(c->ctx->profiles, c->ctx->assignments, c->sender);
    /* only what is in the profile they are shown: anything else is as if it were not here */
    if (!p || (strcmp(p->avatar.hash, hash) && strcmp(p->banner.hash, hash) && strcmp(p->background.hash, hash)))
        return puddle_fail(w, 404, "not_found");
    if (!blob_of(c, hash))
        return puddle_fail(w, 404, "not_found");
    return 200;
}

/* ---- we tell a peer which profile it sees ---- */

int puddle_profile_push(puddle_ctx *ctx, const char *peer)
{
    if (!ctx->outbox || !ctx->profiles)
        return -1;
    const puddle_profile *p = puddle_profile_for_peer(ctx->profiles, ctx->assignments, peer);
    if (!p)
        return -1;
    char body[PUDDLE_WIRE_PROFILE_MAX + 64], err[200];
    puddle_jw w;
    puddle_jw_init(&w, body, sizeof body);
    puddle_jw_object(&w);
    puddle_jw_key(&w, "profile");
    puddle_jw_object(&w);
    puddle_profile_write_fields(&w, p);
    puddle_jw_end_object(&w);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    return n && puddle_outbox_enqueue(ctx->outbox, peer, "profile", "/stream/v1/profile", body, n, err, sizeof err) == 0 ? 0 : -1;
}

/* ---- a peer tells us which profile we see ---- */

typedef struct {
    const char *profile;
} profile_set;

static int apply_profile(void *user, puddle_stream *st, int exists)
{
    profile_set *p = user;
    if (!exists || !puddle_stream_carries_groups(st))
        return 1;
    return puddle_stream_set_profile(st, p->profile, strlen(p->profile)) == 0 ? 0 : -1;
}

static int profile_post(const puddle_call *c, puddle_jw *w)
{
    puddle_span pv;
    const char *b = (const char *)c->body;
    char out[PUDDLE_WIRE_PROFILE_MAX + 8];
    if (!is_active(c->ctx, c->sender))
        return puddle_fail(w, 403, "no_stream");
    if (!puddle_json_valid_object(b, c->body_len) || puddle_json_member(b, c->body_len, "profile", &pv) != 0 ||
        puddle_profile_wire_clean(pv.p, pv.len, out, sizeof out) != 0)
        return puddle_fail(w, 400, "bad_request");
    profile_set ps = { out };
    char err[300];
    int rc = puddle_streams_update(c->ctx->streams, c->sender, apply_profile, &ps, err, sizeof err);
    if (rc < 0)
        return puddle_fail(w, 500, "internal");
    if (rc == 0)
        puddle_media_want(c->ctx, c->sender);
    return 204;
}

/* ---- fetching what their profile points at ---- */

int puddle_media_want(puddle_ctx *ctx, const char *peer)
{
    if (!ctx->outbox || !ctx->config)
        return 0;
    puddle_stream st;
    if (puddle_streams_get(ctx->streams, peer, &st) != 0)
        return 0;
    puddle_media_ref refs[3];
    size_t n = puddle_stream_carries_groups(&st) && st.profile ? puddle_profile_wire_media(st.profile, strlen(st.profile), refs) : 0;
    puddle_stream_free(&st);
    if (!n)
        return 0;
    puddle_ob_item *items;
    size_t ni;
    if (puddle_outbox_list(ctx->outbox, &items, &ni) != 0)
        return 0;
    int queued = 0;
    for (size_t i = 0; i < n; i++) {
        if (puddle_media_have(ctx->config->data_dir, refs[i].hash))
            continue;
        char path[160], err[200];
        snprintf(path, sizeof path, "/stream/v1/media?hash=%s", refs[i].hash);
        int waiting = 0;
        for (size_t k = 0; k < ni; k++)
            if (items[k].state != PUDDLE_OB_FAILED && !strcmp(items[k].peer, peer) && !strcmp(items[k].path, path))
                waiting = 1;
        if (!waiting && puddle_outbox_enqueue_get(ctx->outbox, peer, "media", path, (size_t)refs[i].size + 4096, err, sizeof err) == 0)
            queued++;
    }
    puddle_outbox_list_free(items, ni);
    return queued;
}

typedef struct {
    const char *hash;
    puddle_media_ref found;
    int have;
    const char *error;
    char message[200];
} fetched;

static int apply_note(void *user, puddle_stream *st, int exists)
{
    fetched *f = user;
    if (!exists)
        return 1;
    snprintf(st->error, sizeof st->error, "%s", f->message);
    return 0;
}

void puddle_profiles_done(puddle_ctx *ctx, const puddle_ob_item *item, puddle_ob_outcome outcome, const puddle_net_reply *reply)
{
    int is_media = !strcmp(item->kind, "media"), is_profile = !strcmp(item->kind, "profile");
    if ((!is_media && !is_profile) || outcome == PUDDLE_OB_HOLD)
        return;
    fetched f;
    memset(&f, 0, sizeof f);
    char err[300], why[160] = "";
    if (outcome == PUDDLE_OB_REJECTED) {
        snprintf(f.message, sizeof f.message, is_media ? "a picture of theirs could not be fetched: %.140s" : "they could not be told about your profile: %.140s", item->reason);
        puddle_streams_update(ctx->streams, item->peer, apply_note, &f, err, sizeof err);
        return;
    }
    if (!is_media)
        return;
    /* delivered: what came is the picture only if it is the one we asked for, the size and kind they said, whole */
    const char *q = strstr(item->path, "hash=");
    if (!q || !puddle_media_hash_valid(q + 5)) {
        return;
    }
    puddle_stream st;
    if (puddle_streams_get(ctx->streams, item->peer, &st) != 0)
        return;
    puddle_media_ref refs[3];
    size_t n = st.profile ? puddle_profile_wire_media(st.profile, strlen(st.profile), refs) : 0;
    puddle_stream_free(&st);
    const puddle_media_ref *want = NULL;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(refs[i].hash, q + 5))
            want = &refs[i];
    if (!want)
        return; /* their profile no longer points at it: nothing to keep */
    if (!ctx->config || !reply || !reply->body ||
        puddle_media_store(ctx->config->data_dir, want->hash, (const unsigned char *)reply->body, reply->body_len, want->size, want->mime, why,
                           sizeof why) != 0) {
        snprintf(f.message, sizeof f.message, "a picture they sent did not check out: %.140s", why[0] ? why : "nothing came");
        puddle_streams_update(ctx->streams, item->peer, apply_note, &f, err, sizeof err);
    }
}

const puddle_route puddle_profile_routes[] = {
    { "GET",  "/api/v1/profile",    PUDDLE_AUTH_OWNER, 0,    list_profiles, NULL },
    { "GET",  "/api/v1/media",      PUDDLE_AUTH_OWNER, 0,    owner_media,   NULL },
    { "GET",  "/stream/v1/media",   PUDDLE_AUTH_PEER,  0,    peer_media,    NULL },
    { "POST", "/stream/v1/profile", PUDDLE_AUTH_PEER,  16384, profile_post, NULL },
};
const size_t puddle_profile_routes_count = sizeof puddle_profile_routes / sizeof puddle_profile_routes[0];
