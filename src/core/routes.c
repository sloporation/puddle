/* The Puddle's routes. The owner API (api/openapi.yaml) and the stream API (api/federation.yaml) are built
 * up here, one handler at a time. */
#include "router.h"

#include "phrase.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROLL_TTL_MS (10 * 60 * 1000)

static int64_t now_of(const puddle_call *c)
{
    if (c->ctx->now_ms)
        return c->ctx->now_ms();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- the pending roll ---- */

struct puddle_roll_slot {
    pthread_mutex_t mu;
    puddle_roll *roll;
    int64_t expires_ms;
};

puddle_roll_slot *puddle_roll_slot_new(void)
{
    puddle_roll_slot *s = calloc(1, sizeof *s);
    if (s)
        pthread_mutex_init(&s->mu, NULL);
    return s;
}

void puddle_roll_slot_free(puddle_roll_slot *s)
{
    if (!s)
        return;
    if (s->roll)
        puddle_roll_abandon(s->roll);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

/* ---- shared pieces ---- */

/* puddle://<id>@<where to reach us>, or 0 if we cannot say where that is: listening on every interface
 * says nothing about which address the outside world should use, so the owner has to tell us. */
static int make_link(const puddle_config *cfg, const char *id, char *out, size_t cap)
{
    char where[160];
    if (cfg->advertise[0]) {
        snprintf(where, sizeof where, "%s", cfg->advertise);
    } else if (!strcmp(cfg->address, "::") || !strcmp(cfg->address, "0.0.0.0")) {
        return 0;
    } else if (strchr(cfg->address, ':')) {
        snprintf(where, sizeof where, "[%s]:%u", cfg->address, cfg->port);
    } else {
        snprintf(where, sizeof where, "%s:%u", cfg->address, cfg->port);
    }
    int n = snprintf(out, cap, "puddle://%s@%s", id, where);
    return n > 0 && (size_t)n < cap;
}

static void link_member(puddle_jw *w, const puddle_ctx *ctx, const char *id)
{
    char link[256];
    puddle_jw_key(w, "link");
    if (ctx->config && make_link(ctx->config, id, link, sizeof link))
        puddle_jw_string(w, link);
    else
        puddle_jw_null(w);
}

static void media_member(puddle_jw *w, const char *key, const puddle_media *m)
{
    if (!m->hash[0])
        return;
    puddle_jw_key(w, key);
    puddle_jw_object(w);
    puddle_jw_str(w, "hash", m->hash);
    puddle_jw_str(w, "mime", m->mime);
    puddle_jw_num(w, "size", (int64_t)m->size);
    puddle_jw_end_object(w);
}

static void profile_json(puddle_jw *w, const puddle_profile *p, int is_default)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", p->id);
    puddle_jw_flag(w, "default", is_default);
    puddle_jw_str(w, "display_name", p->display_name);
    if (p->blurb)
        puddle_jw_str(w, "blurb", p->blurb);
    if (p->about)
        puddle_jw_str(w, "about", p->about);
    if (p->pronouns)
        puddle_jw_str(w, "pronouns", p->pronouns);
    if (p->color.kind != PUDDLE_COLOR_NONE) {
        puddle_jw_key(w, "color");
        puddle_jw_object(w);
        puddle_jw_key(w, "colors");
        puddle_jw_array(w);
        for (int i = 0; i < p->color.nstops; i++)
            puddle_jw_string(w, p->color.stops[i]);
        puddle_jw_end_array(w);
        if (p->color.kind == PUDDLE_COLOR_GRADIENT)
            puddle_jw_num(w, "angle", p->color.angle);
        puddle_jw_end_object(w);
    }
    media_member(w, "avatar", &p->avatar);
    media_member(w, "banner", &p->banner);
    media_member(w, "background", &p->background);
    puddle_jw_end_object(w);
}

/* the Identity schema */
static void identity_json(puddle_jw *w, const puddle_ctx *ctx)
{
    char id[PUDDLE_USER_ID_LEN + 1], kid[65];
    unsigned char pub[32];
    uint64_t seq;
    puddle_identity_id(ctx->identity, id);
    puddle_identity_public(ctx->identity, pub, NULL, &seq);
    puddle_key_id(pub, kid);
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id);
    link_member(w, ctx, id);
    puddle_jw_num(w, "seq", (int64_t)seq);
    puddle_jw_str(w, "key_id", kid);
    puddle_jw_end_object(w);
}

/* ---- handlers ---- */

static int health(const puddle_call *c, puddle_jw *w)
{
    (void)c;
    puddle_jw_object(w);
    puddle_jw_str(w, "status", "ok");
    puddle_jw_end_object(w);
    return 200;
}

static int get_identity(const puddle_call *c, puddle_jw *w)
{
    identity_json(w, c->ctx);
    return 200;
}

static int get_me(const puddle_call *c, puddle_jw *w)
{
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(c->ctx->identity, id);
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id);
    puddle_jw_key(w, "profile");
    profile_json(w, &c->ctx->profiles->items[0], 1);
    link_member(w, c->ctx, id);
    puddle_jw_end_object(w);
    return 200;
}

/* Step one of changing keys: check the CURRENT phrase, hand back a NEW one. Nothing changes until confirm. */
static int roll_start(const puddle_call *c, puddle_jw *w)
{
    puddle_span v;
    char phrase[128], fresh[PUDDLE_PHRASE_LEN + 1], err[256];
    if (puddle_json_member((const char *)c->body, c->body_len, "phrase", &v) != 0 ||
        puddle_json_string(v, phrase, sizeof phrase) != 0)
        return puddle_fail(w, 400, "bad_request");

    puddle_roll_slot *slot = c->ctx->roll_slot;
    pthread_mutex_lock(&slot->mu);
    if (slot->roll) {
        puddle_roll_abandon(slot->roll); /* starting again replaces an unfinished roll */
        slot->roll = NULL;
    }
    slot->roll = puddle_identity_roll_prepare(c->ctx->identity, phrase, fresh, err, sizeof err);
    memset(phrase, 0, sizeof phrase);
    if (!slot->roll) {
        pthread_mutex_unlock(&slot->mu);
        puddle_jw_object(w);
        puddle_jw_str(w, "error", "bad_phrase");
        puddle_jw_str(w, "message", err);
        puddle_jw_end_object(w);
        return 400;
    }
    slot->expires_ms = now_of(c) + ROLL_TTL_MS;
    pthread_mutex_unlock(&slot->mu);

    puddle_jw_object(w);
    puddle_jw_str(w, "new_phrase", fresh);
    puddle_jw_num(w, "expires_in", ROLL_TTL_MS / 1000);
    puddle_jw_end_object(w);
    memset(fresh, 0, sizeof fresh);
    return 200;
}

/* Step two: the owner has written the new phrase down. */
static int roll_confirm(const puddle_call *c, puddle_jw *w)
{
    char err[256];
    puddle_roll_slot *slot = c->ctx->roll_slot;
    pthread_mutex_lock(&slot->mu);
    puddle_roll *roll = slot->roll;
    slot->roll = NULL;
    if (!roll || now_of(c) > slot->expires_ms) {
        if (roll)
            puddle_roll_abandon(roll); /* too old: the phrase was shown long ago, so ask again */
        pthread_mutex_unlock(&slot->mu);
        return puddle_fail(w, 409, "no_roll_pending");
    }
    int rc = puddle_identity_roll_commit(c->ctx->identity, roll, err, sizeof err);
    if (rc != 0)
        puddle_roll_abandon(roll);
    pthread_mutex_unlock(&slot->mu);
    if (rc != 0) {
        puddle_jw_object(w);
        puddle_jw_str(w, "error", "roll_failed");
        puddle_jw_str(w, "message", err);
        puddle_jw_end_object(w);
        return 500;
    }
    identity_json(w, c->ctx);
    return 200;
}

static int roll_cancel(const puddle_call *c, puddle_jw *w)
{
    puddle_roll_slot *slot = c->ctx->roll_slot;
    pthread_mutex_lock(&slot->mu);
    int had = slot->roll != NULL;
    if (had)
        puddle_roll_abandon(slot->roll);
    slot->roll = NULL;
    pthread_mutex_unlock(&slot->mu);
    puddle_jw_object(w);
    puddle_jw_flag(w, "cancelled", had);
    puddle_jw_end_object(w);
    return 200;
}

const puddle_route puddle_routes[] = {
    { "GET",  "/health",                   PUDDLE_AUTH_OWNER, 0,   health,       NULL },
    { "GET",  "/api/v1/identity",          PUDDLE_AUTH_OWNER, 0,   get_identity, NULL },
    { "GET",  "/api/v1/me",                PUDDLE_AUTH_OWNER, 0,   get_me,       NULL },
    { "POST", "/api/v1/identity/roll",     PUDDLE_AUTH_OWNER, 512, roll_start,   NULL },
    { "POST", "/api/v1/identity/roll/confirm", PUDDLE_AUTH_OWNER, 64, roll_confirm, NULL },
    { "POST", "/api/v1/identity/roll/cancel",  PUDDLE_AUTH_OWNER, 64, roll_cancel,  NULL },
};
const size_t puddle_routes_count = sizeof puddle_routes / sizeof puddle_routes[0];
