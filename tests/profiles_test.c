#include "profiles_api.h"

#include "hash.h"
#include "media.h"
#include "phrase.h"
#include "wireprofile.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define T0 1760000000000LL
static char root[512], data[700];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }
static int64_t clock_now = T0;
static int64_t now_cb(void) { return clock_now; }

typedef struct { puddle_identity *id; char uid[PUDDLE_USER_ID_LEN + 1]; char phrase[PUDDLE_PHRASE_LEN + 1]; } person;
static person me, alice, carol;
static puddle_ctx ctx;
static puddle_router R;
static puddle_streams *ss;
static puddle_outbox *ob;
static puddle_profiles profiles;
static puddle_assignments *as;
static puddle_config cfg;
static char avatar_main[65], avatar_work[65], banner_work[65];
static size_t size_main, size_work;

static void make(person *p, const char *name)
{
    char dir[700], err[300];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    p->id = puddle_identity_create(dir, p->phrase, err, sizeof err);
    if (!p->id) exit(2);
    puddle_identity_confirm(dir);
    puddle_identity_id(p->id, p->uid);
}

/* a file that starts like a PNG, with `fill` after it */
static size_t write_png(const char *path, char fill, size_t total)
{
    FILE *f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    for (size_t i = 8; i < total; i++) fputc(fill, f);
    fclose(f);
    return total;
}

typedef struct { int status; char out[70000]; size_t n; } reply;

static reply *call(const char *method, const char *target, const char *body, const char *sig, int owner)
{
    static reply rp;
    puddle_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "%s", method);
    snprintf(req.target, sizeof req.target, "%s", target);
    snprintf(req.path, sizeof req.path, "%s", target);
    char *q = strchr(req.path, '?');
    if (q) *q = '\0';
    if (sig) snprintf(req.signature, sizeof req.signature, "%s", sig);
    size_t bl = body ? strlen(body) : 0;
    req.content_length = bl;
    unsigned char *copy = malloc(bl + 1);
    if (bl) memcpy(copy, body, bl);
    copy[bl] = 0;
    memset(&rp, 0, sizeof rp);
    rp.n = puddle_router_handle(&R, &req, owner, copy, bl, rp.out, sizeof rp.out - 1, &rp.status);
    free(copy);
    return &rp;
}

static const char *body_of(const reply *r) { const char *b = strstr(r->out, "\r\n\r\n"); return b ? b + 4 : ""; }
static reply *owner_call(const char *m, const char *t, const char *b) { return call(m, t, b, NULL, 1); }

static reply *peer_call(person *from, const char *method, const char *target, const char *body)
{
    char sig[800];
    const char *b = body ? body : "";
    if (puddle_reqsig_sign(from->id, method, target, b, strlen(b), me.uid, clock_now, sig, sizeof sig) != 0) exit(2);
    clock_now += 5;
    return call(method, target, body, sig, 0);
}

static void set_peer(person *p, puddle_stream_state state, const char *profile)
{
    puddle_stream rec;
    char err[300];
    size_t cl;
    char *chain = puddle_identity_chain(p->id, &cl);
    CHECK(puddle_stream_init(&rec, p->uid, state, T0) == 0);
    CHECK(puddle_stream_set_chain(&rec, chain, cl) == 0);
    free(chain);
    CHECK(puddle_stream_add_address(&rec, "203.0.113.9:8443") == 0);
    if (profile) CHECK(puddle_stream_set_profile(&rec, profile, strlen(profile)) == 0);
    CHECK(puddle_streams_put(ss, &rec, err, sizeof err) == 0);
    puddle_stream_free(&rec);
}

static int queued(const char *peer, const char *kind)
{
    puddle_ob_item *l;
    size_t n;
    int c = 0;
    puddle_outbox_list(ob, &l, &n);
    for (size_t i = 0; i < n; i++)
        c += !strcmp(l[i].peer, peer) && (!kind || !strcmp(l[i].kind, kind)) && l[i].state == PUDDLE_OB_QUEUED;
    puddle_outbox_list_free(l, n);
    return c;
}

static void drop_outbox(void)
{
    puddle_outbox_close(ob);
    char path[900], err[200];
    snprintf(path, sizeof path, "%s/state/outbox", data);
    nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    puddle_outbox_hooks h = { .identity = me.id, .now_ms = now_cb };
    ob = puddle_outbox_open(data, &h, err, sizeof err);
    ctx.outbox = ob;
}

static char *error_of(person *p)
{
    static char e[300];
    puddle_stream st;
    e[0] = 0;
    if (puddle_streams_get(ss, p->uid, &st) == 0) {
        snprintf(e, sizeof e, "%s", st.error);
        puddle_stream_free(&st);
    }
    return e;
}

static char profile_of_peer[2000];
static const char *peer_profile(person *p)
{
    puddle_stream st;
    profile_of_peer[0] = 0;
    if (puddle_streams_get(ss, p->uid, &st) == 0) {
        if (st.profile) snprintf(profile_of_peer, sizeof profile_of_peer, "%s", st.profile);
        puddle_stream_free(&st);
    }
    return profile_of_peer;
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-profiles-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    make(&me, "me"); make(&alice, "alice"); make(&carol, "carol");
    char err[300], path[900];
    snprintf(data, sizeof data, "%s/data", root);

    /* our profiles: a default with an avatar, and "work" with another avatar and a banner */
    char a1[700], a2[700], b2[700];
    snprintf(a1, sizeof a1, "%s/avatar-main.png", root);
    snprintf(a2, sizeof a2, "%s/avatar-work.png", root);
    snprintf(b2, sizeof b2, "%s/banner-work.png", root);
    size_main = write_png(a1, 'a', 300);
    size_work = write_png(a2, 'b', 500);
    write_png(b2, 'c', 400);
    snprintf(path, sizeof path, "%s/profiles.yaml", root);
    FILE *f = fopen(path, "w");
    fprintf(f, "profiles:\n  - id: main\n    display_name: Me\n    blurb: the default\n    avatar: avatar-main.png\n"
               "  - id: work\n    display_name: Me at work\n    avatar: avatar-work.png\n    banner: banner-work.png\n"
               "    color:\n      gradient: [\"#112233\", \"#445566\"]\n      angle: 135\n");
    fclose(f);
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.dir, sizeof cfg.dir, "%s", root);
    snprintf(cfg.data_dir, sizeof cfg.data_dir, "%s", data);
    snprintf(cfg.address, sizeof cfg.address, "127.0.0.1");
    cfg.port = 8443;
    snprintf(cfg.profile_files[0], PUDDLE_PATH_MAX, "%s", path);
    cfg.nprofile_files = 1;
    if (puddle_profiles_load(&profiles, &cfg, err, sizeof err) != 0) { fprintf(stderr, "profiles: %s\n", err); return 2; }
    snprintf(avatar_main, sizeof avatar_main, "%s", profiles.items[0].avatar.hash);
    snprintf(avatar_work, sizeof avatar_work, "%s", profiles.items[1].avatar.hash);
    snprintf(banner_work, sizeof banner_work, "%s", profiles.items[1].banner.hash);
    CHECK(avatar_main[0] && avatar_work[0] && banner_work[0] && strcmp(avatar_main, avatar_work) != 0);

    ss = puddle_streams_open(data, err, sizeof err);
    as = puddle_assignments_open(data, err, sizeof err);
    puddle_outbox_hooks hooks = { .identity = me.id, .now_ms = now_cb };
    ob = puddle_outbox_open(data, &hooks, err, sizeof err);
    ctx = (puddle_ctx){ .config = &cfg, .identity = me.id, .profiles = &profiles, .assignments = as, .streams = ss, .outbox = ob,
                        .replay = puddle_replay_new(4096), .now_ms = now_cb, .peer_key = puddle_streams_peer_key, .peer_key_user = ss };
    R = (puddle_router){ .ctx = &ctx };
    R.routes = puddle_all_routes(&R.count);
    reply *r;
    char t[400], bodyj[1200];

    /* ---- the owner's profiles ---- */
    r = owner_call("GET", "/api/v1/profile", NULL);
    CHECK(r->status == 200 && puddle_json_valid_object("{}", 2) && body_of(r)[0] == '[');
    CHECK(strstr(body_of(r), "\"id\":\"main\",\"default\":true,\"display_name\":\"Me\",\"blurb\":\"the default\",\"avatar\":{\"hash\":\"") &&
          strstr(body_of(r), "\"id\":\"work\",\"default\":false,\"display_name\":\"Me at work\"") && strstr(body_of(r), "\"color\":{\"colors\":[\"#112233\",\"#445566\"],\"angle\":135}") &&
          strstr(body_of(r), avatar_work) && strstr(body_of(r), banner_work));
    CHECK(call("GET", "/api/v1/profile", NULL, NULL, 0)->status == 404);

    /* ---- the owner fetches a picture ---- */
    snprintf(t, sizeof t, "/api/v1/media?hash=%s", avatar_main);
    r = owner_call("GET", t, NULL);
    CHECK(r->status == 200 && strstr(r->out, "Content-Type: image/png\r\n") && strstr(r->out, "Content-Length: 300\r\n") &&
          strstr(r->out, "immutable") && strstr(r->out, "nosniff") && r->n > 300 && !memcmp(r->out + r->n - 292, "aaaa", 4));
    CHECK(!memcmp(strstr(r->out, "\r\n\r\n") + 4, "\x89PNG\r\n\x1a\n", 8));
    CHECK(call("GET", t, NULL, NULL, 0)->status == 404);                                          /* not the owner */
    CHECK(owner_call("GET", "/api/v1/media?hash=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", NULL)->status == 404);
    CHECK(owner_call("GET", "/api/v1/media?hash=nope", NULL)->status == 400 && owner_call("GET", "/api/v1/media", NULL)->status == 400);
    CHECK(owner_call("GET", "/api/v1/media?hash=0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef", NULL)->status == 400);
    CHECK(owner_call("GET", "/api/v1/media?hash=../../etc/passwd", NULL)->status == 400);
    {
        /* a file in the media directory that is not an image is not served as one */
        char junk[900], hash[65];
        puddle_sha256_hex("not an image", 12, hash);
        snprintf(junk, sizeof junk, "%s/media/%s", data, hash);
        f = fopen(junk, "wb");
        fputs("not an image", f);
        fclose(f);
        snprintf(t, sizeof t, "/api/v1/media?hash=%s", hash);
        CHECK(owner_call("GET", t, NULL)->status == 404);
    }

    /* a file too large for what we serve, and one too large to fit in a plain answer */
    {
        char big_hash[65], path2[900];
        puddle_sha256_hex("big one", 7, big_hash);
        snprintf(path2, sizeof path2, "%s/media/%s", data, big_hash);
        f = fopen(path2, "wb");
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        fseek(f, (long)PUDDLE_IMAGE_MAX_BYTES, SEEK_SET);                                          /* a sparse file: one byte over the cap */
        fputc(0, f);
        fclose(f);
        snprintf(t, sizeof t, "/api/v1/media?hash=%s", big_hash);
        CHECK(owner_call("GET", t, NULL)->status == 404);
        char mid_hash[65];
        puddle_sha256_hex("mid one", 7, mid_hash);
        snprintf(path2, sizeof path2, "%s/media/%s", data, mid_hash);
        write_png(path2, 'm', 80000);
        snprintf(t, sizeof t, "/api/v1/media?hash=%s", mid_hash);
        CHECK(owner_call("GET", t, NULL)->status == 500);                                         /* the plain form cannot carry it; the server's can */
        {
            /* the form the server uses hands it back whole */
            puddle_request rq;
            memset(&rq, 0, sizeof rq);
            snprintf(rq.method, sizeof rq.method, "GET");
            snprintf(rq.target, sizeof rq.target, "%s", t);
            snprintf(rq.path, sizeof rq.path, "/api/v1/media");
            char out[2000];
            int st;
            puddle_big bg;
            size_t hl = puddle_router_handle_big(&R, &rq, 1, (const unsigned char *)"", 0, out, sizeof out, &st, &bg);
            CHECK(st == 200 && hl > 0 && hl < sizeof out && bg.data && bg.len == 80000 && strstr(out, "Content-Length: 80000\r\n"));
            free(bg.data);
        }
    }

    /* ---- a friend asks for our pictures ---- */
    set_peer(&alice, PUDDLE_STREAM_ACTIVE, NULL);
    snprintf(t, sizeof t, "/stream/v1/media?hash=%s", avatar_main);
    r = peer_call(&alice, "GET", t, NULL);
    CHECK(r->status == 200 && strstr(r->out, "image/png") && !memcmp(strstr(r->out, "\r\n\r\n") + 4, "\x89PNG", 4));
    snprintf(t, sizeof t, "/stream/v1/media?hash=%s", avatar_work);
    CHECK(peer_call(&alice, "GET", t, NULL)->status == 404);                                     /* in a profile she is not shown */
    snprintf(t, sizeof t, "/stream/v1/media?hash=%s", banner_work);
    CHECK(peer_call(&alice, "GET", t, NULL)->status == 404);
    CHECK(peer_call(&alice, "GET", "/stream/v1/media?hash=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", NULL)->status == 404);
    CHECK(peer_call(&alice, "GET", "/stream/v1/media?hash=zz", NULL)->status == 404 && peer_call(&alice, "GET", "/stream/v1/media", NULL)->status == 404);
    snprintf(t, sizeof t, "/stream/v1/media?hash=%s", avatar_main);
    CHECK(call("GET", t, NULL, NULL, 0)->status == 404);                                         /* unsigned */
    set_peer(&carol, PUDDLE_STREAM_PENDING_IN, NULL);
    CHECK(peer_call(&carol, "GET", t, NULL)->status == 404);                                     /* asked, not yet let in */
    set_peer(&carol, PUDDLE_STREAM_CLOSED, NULL);
    CHECK(peer_call(&carol, "GET", t, NULL)->status == 404);

    /* ---- the owner chooses what a stream sees ---- */
    snprintf(bodyj, sizeof bodyj, "{\"stream\":\"%s\",\"profile\":\"work\"}", alice.uid);
    CHECK(call("POST", "/api/v1/stream/profile", bodyj, NULL, 0)->status == 404);
    r = owner_call("POST", "/api/v1/stream/profile", bodyj);
    CHECK(r->status == 200 && strstr(body_of(r), "\"profile\":\"work\""));
    {
        char pr[PUDDLE_PROFILE_ID_MAX + 1];
        CHECK(puddle_assignments_get(as, alice.uid, pr) == 0 && !strcmp(pr, "work"));
    }
    CHECK(queued(alice.uid, "profile") == 1);                                                    /* and she is told */
    {
        puddle_ob_item *l;
        size_t n;
        puddle_outbox_list(ob, &l, &n);
        CHECK(n == 1 && !strcmp(l[0].path, "/stream/v1/profile") && !strcmp(l[0].method, "POST"));
        char *clean = malloc(PUDDLE_WIRE_PROFILE_MAX + 8);
        puddle_span pv;
        CHECK(puddle_json_member(l[0].body, l[0].body_len, "profile", &pv) == 0 && puddle_profile_wire_clean(pv.p, pv.len, clean, PUDDLE_WIRE_PROFILE_MAX + 8) == 0);
        CHECK(strstr(clean, "\"display_name\":\"Me at work\"") && strstr(clean, avatar_work) && strstr(clean, banner_work) && !strstr(clean, avatar_main) &&
              strstr(clean, "\"color\":{\"colors\":[\"#112233\",\"#445566\"],\"angle\":135}"));  /* what she receives passes her own checks, unchanged */
        free(clean);
        puddle_outbox_list_free(l, n);
    }
    snprintf(t, sizeof t, "/stream/v1/media?hash=%s", avatar_work);
    CHECK(peer_call(&alice, "GET", t, NULL)->status == 200);                                     /* now this one, and no longer the other */
    snprintf(t, sizeof t, "/stream/v1/media?hash=%s", avatar_main);
    CHECK(peer_call(&alice, "GET", t, NULL)->status == 404);
    snprintf(t, sizeof t, "/stream/v1/media?hash=%s", banner_work);
    CHECK(peer_call(&alice, "GET", t, NULL)->status == 200);
    snprintf(bodyj, sizeof bodyj, "{\"stream\":\"%s\",\"profile\":null}", alice.uid);
    r = owner_call("POST", "/api/v1/stream/profile", bodyj);
    CHECK(r->status == 200 && !strstr(body_of(r), "\"profile\":\"work\"") && queued(alice.uid, "profile") == 2);
    {
        char pr[PUDDLE_PROFILE_ID_MAX + 1];
        CHECK(puddle_assignments_get(as, alice.uid, pr) == 1);                                   /* back to the default */
    }
    /* what it refuses */
    snprintf(bodyj, sizeof bodyj, "{\"stream\":\"%s\",\"profile\":\"nope\"}", alice.uid);
    CHECK(owner_call("POST", "/api/v1/stream/profile", bodyj)->status == 400);
    snprintf(bodyj, sizeof bodyj, "{\"stream\":\"%s\",\"profile\":5}", alice.uid);
    CHECK(owner_call("POST", "/api/v1/stream/profile", bodyj)->status == 400);
    snprintf(bodyj, sizeof bodyj, "{\"stream\":\"%s\"}", alice.uid);
    CHECK(owner_call("POST", "/api/v1/stream/profile", bodyj)->status == 400);
    CHECK(owner_call("POST", "/api/v1/stream/profile", "{\"stream\":\"zaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"profile\":\"work\"}")->status == 404);
    CHECK(owner_call("POST", "/api/v1/stream/profile", "{\"profile\":\"work\"}")->status == 400);
    /* a stream that is not open is not pushed to, but the choice is kept for when it is */
    snprintf(bodyj, sizeof bodyj, "{\"stream\":\"%s\",\"profile\":\"work\"}", carol.uid);
    int before = queued(carol.uid, NULL);
    CHECK(owner_call("POST", "/api/v1/stream/profile", bodyj)->status == 200 && queued(carol.uid, NULL) == before);
    {
        char pr[PUDDLE_PROFILE_ID_MAX + 1];
        CHECK(puddle_assignments_get(as, carol.uid, pr) == 0 && !strcmp(pr, "work"));
    }

    /* ---- a friend tells us which profile we see ---- */
    {
        drop_outbox();
        const char *h1 = "1111111111111111111111111111111111111111111111111111111111111111";
        const char *h2 = "2222222222222222222222222222222222222222222222222222222222222222";
        set_peer(&alice, PUDDLE_STREAM_ACTIVE, "{\"display_name\":\"Old\"}");
        snprintf(bodyj, sizeof bodyj,
                 "{\"profile\":{\"display_name\":\"Alice (new)\",\"blurb\":\"hi\",\"junk\":1,\"color\":{\"colors\":[\"#AA0000\"]},"
                 "\"avatar\":{\"hash\":\"%s\",\"mime\":\"image/png\",\"size\":100},\"banner\":{\"hash\":\"%s\",\"mime\":\"image/jpeg\",\"size\":200}}}", h1, h2);
        CHECK(call("POST", "/stream/v1/profile", bodyj, NULL, 0)->status == 404);                /* unsigned */
        r = peer_call(&alice, "POST", "/stream/v1/profile", bodyj);
        CHECK(r->status == 204 && !strstr(r->out, "Content-Length"));
        CHECK(strstr(peer_profile(&alice), "\"display_name\":\"Alice (new)\"") && !strstr(peer_profile(&alice), "junk") &&
              strstr(peer_profile(&alice), "\"colors\":[\"#aa0000\"]"));                          /* kept as rebuilt, not as sent */
        /* and the pictures it points at are asked for, from her, by hash, and no larger than they were said to be */
        puddle_ob_item *l;
        size_t n;
        puddle_outbox_list(ob, &l, &n);
        CHECK(n == 2 && !strcmp(l[0].kind, "media") && !strcmp(l[0].method, "GET") && l[0].body_len == 0 && l[0].max_reply == 100 + 4096);
        CHECK(!strcmp(l[0].path + strlen("/stream/v1/media?hash="), h1) || !strcmp(l[1].path + strlen("/stream/v1/media?hash="), h1));
        CHECK(l[1].max_reply == 200 + 4096 || l[0].max_reply == 200 + 4096);
        puddle_outbox_list_free(l, n);
        CHECK(puddle_media_want(&ctx, alice.uid) == 0);                                          /* already asked: not again */
        /* the same push again changes nothing and asks for nothing more */
        CHECK(peer_call(&alice, "POST", "/stream/v1/profile", bodyj)->status == 204 && queued(alice.uid, "media") == 2);
        /* what it refuses */
        CHECK(peer_call(&alice, "POST", "/stream/v1/profile", "{}")->status == 400);
        CHECK(peer_call(&alice, "POST", "/stream/v1/profile", "nonsense")->status == 400);
        CHECK(peer_call(&alice, "POST", "/stream/v1/profile", "{\"profile\":{}}")->status == 400);
        CHECK(peer_call(&alice, "POST", "/stream/v1/profile", "{\"profile\":{\"display_name\":\"x\",\"avatar\":{\"hash\":\"zz\"}}}")->status == 400);
        CHECK(peer_call(&alice, "POST", "/stream/v1/profile", "{\"profile\":[1]}")->status == 400);
        CHECK(strstr(peer_profile(&alice), "Alice (new)"));                                      /* none of those changed it */
        set_peer(&alice, PUDDLE_STREAM_CLOSED, NULL);
        CHECK(peer_call(&alice, "POST", "/stream/v1/profile", bodyj)->status == 403);            /* no open stream */
        set_peer(&alice, PUDDLE_STREAM_PENDING_IN, NULL);
        CHECK(peer_call(&alice, "POST", "/stream/v1/profile", bodyj)->status == 403);
    }

    /* ---- fetching what their profile points at ---- */
    {
        drop_outbox();
        unsigned char png[200];
        memcpy(png, "\x89PNG\r\n\x1a\n", 8);
        memset(png + 8, 'z', sizeof png - 8);
        char hash[65];
        puddle_sha256_hex(png, sizeof png, hash);
        char prof[900];
        snprintf(prof, sizeof prof, "{\"display_name\":\"Alice\",\"avatar\":{\"hash\":\"%s\",\"mime\":\"image/png\",\"size\":200}}", hash);
        set_peer(&alice, PUDDLE_STREAM_ACTIVE, prof);
        CHECK(puddle_media_want(&ctx, alice.uid) == 1 && queued(alice.uid, "media") == 1);
        CHECK(puddle_media_want(&ctx, alice.uid) == 0);
        CHECK(puddle_media_want(&ctx, carol.uid) == 0);                                          /* not open */
        set_peer(&carol, PUDDLE_STREAM_CLOSED, prof);
        CHECK(puddle_media_want(&ctx, carol.uid) == 0 && queued(carol.uid, "media") == 0);        /* closed, with pictures it points at: still not */
        set_peer(&carol, PUDDLE_STREAM_PENDING_IN, prof);
        CHECK(puddle_media_want(&ctx, carol.uid) == 0 && queued(carol.uid, "media") == 0);        /* asked, not yet let in */
        puddle_ob_item it;
        memset(&it, 0, sizeof it);
        snprintf(it.peer, sizeof it.peer, "%s", alice.uid);
        snprintf(it.kind, sizeof it.kind, "media");
        snprintf(it.method, sizeof it.method, "GET");
        snprintf(it.path, sizeof it.path, "/stream/v1/media?hash=%s", hash);
        it.body = (char *)"";
        puddle_net_reply nr;
        memset(&nr, 0, sizeof nr);
        nr.result = PUDDLE_NET_OK;
        nr.status = 200;

        /* the wrong picture, the wrong size, the wrong kind, something that is not a picture: none is kept */
        unsigned char other[200];
        memcpy(other, png, sizeof other);
        other[100] = 'y';
        nr.body = (char *)other;
        nr.body_len = sizeof other;
        puddle_profiles_done(&ctx, &it, PUDDLE_OB_DELIVERED, &nr);
        CHECK(!puddle_media_have(data, hash) && strstr(error_of(&alice), "did not check out") && strstr(error_of(&alice), "hash"));
        nr.body = (char *)png;
        nr.body_len = sizeof png - 1;
        puddle_profiles_done(&ctx, &it, PUDDLE_OB_DELIVERED, &nr);
        CHECK(!puddle_media_have(data, hash) && strstr(error_of(&alice), "size"));
        nr.body = NULL;
        nr.body_len = 0;
        puddle_profiles_done(&ctx, &it, PUDDLE_OB_DELIVERED, &nr);
        CHECK(!puddle_media_have(data, hash));
        puddle_profiles_done(&ctx, &it, PUDDLE_OB_DELIVERED, NULL);
        CHECK(!puddle_media_have(data, hash));
        /* the right bytes under the wrong name: it says JPEG, the bytes are a PNG; or the bytes are not a picture at all */
        {
            char pj[900];
            snprintf(pj, sizeof pj, "{\"display_name\":\"Alice\",\"avatar\":{\"hash\":\"%s\",\"mime\":\"image/jpeg\",\"size\":200}}", hash);
            set_peer(&alice, PUDDLE_STREAM_ACTIVE, pj);
            nr.body = (char *)png;
            nr.body_len = sizeof png;
            puddle_profiles_done(&ctx, &it, PUDDLE_OB_DELIVERED, &nr);
            CHECK(!puddle_media_have(data, hash) && strstr(error_of(&alice), "kind"));
            char text[200], th[65];
            memset(text, 't', sizeof text);
            puddle_sha256_hex(text, sizeof text, th);
            snprintf(pj, sizeof pj, "{\"display_name\":\"Alice\",\"avatar\":{\"hash\":\"%s\",\"mime\":\"image/png\",\"size\":200}}", th);
            set_peer(&alice, PUDDLE_STREAM_ACTIVE, pj);
            char saved_path[160];
            snprintf(saved_path, sizeof saved_path, "%s", it.path);
            snprintf(it.path, sizeof it.path, "/stream/v1/media?hash=%s", th);
            nr.body = text;
            nr.body_len = sizeof text;
            puddle_profiles_done(&ctx, &it, PUDDLE_OB_DELIVERED, &nr);
            CHECK(!puddle_media_have(data, th) && strstr(error_of(&alice), "kind"));
            snprintf(it.path, sizeof it.path, "%s", saved_path);
            set_peer(&alice, PUDDLE_STREAM_ACTIVE, prof);
        }
        /* the picture, as it should be */
        nr.body = (char *)png;
        nr.body_len = sizeof png;
        puddle_profiles_done(&ctx, &it, PUDDLE_OB_DELIVERED, &nr);
        CHECK(puddle_media_have(data, hash));
        drop_outbox();
        CHECK(puddle_media_want(&ctx, alice.uid) == 0);                                           /* kept, and so no longer wanted */
        snprintf(t, sizeof t, "/api/v1/media?hash=%s", hash);
        r = owner_call("GET", t, NULL);
        CHECK(r->status == 200 && r->n > 200 && !memcmp(r->out + r->n - 192, "zzzz", 4));        /* and the owner's app can show it */
        /* the picture of a profile they have since changed: nothing is kept for it */
        char h2[65];
        unsigned char png2[120];
        memcpy(png2, "\x89PNG\r\n\x1a\n", 8);
        memset(png2 + 8, 'q', sizeof png2 - 8);
        puddle_sha256_hex(png2, sizeof png2, h2);
        snprintf(it.path, sizeof it.path, "/stream/v1/media?hash=%s", h2);
        nr.body = (char *)png2;
        nr.body_len = sizeof png2;
        puddle_profiles_done(&ctx, &it, PUDDLE_OB_DELIVERED, &nr);
        CHECK(!puddle_media_have(data, h2));
        /* it could not be fetched: said so, once */
        snprintf(it.path, sizeof it.path, "/stream/v1/media?hash=%s", hash);
        set_peer(&alice, PUDDLE_STREAM_ACTIVE, prof);
        snprintf(it.reason, sizeof it.reason, "they answered HTTP 404");
        puddle_profiles_done(&ctx, &it, PUDDLE_OB_REJECTED, &nr);
        CHECK(strstr(error_of(&alice), "could not be fetched") && strstr(error_of(&alice), "404"));
        set_peer(&alice, PUDDLE_STREAM_ACTIVE, prof);
        puddle_profiles_done(&ctx, &it, PUDDLE_OB_HOLD, &nr);
        CHECK(!error_of(&alice)[0]);
        it.kind[0] = 'x';
        puddle_profiles_done(&ctx, &it, PUDDLE_OB_REJECTED, &nr);
        CHECK(!error_of(&alice)[0]);
        snprintf(it.kind, sizeof it.kind, "profile");
        snprintf(it.reason, sizeof it.reason, "they answered HTTP 400");
        puddle_profiles_done(&ctx, &it, PUDDLE_OB_REJECTED, &nr);
        CHECK(strstr(error_of(&alice), "could not be told about your profile"));
    }

    puddle_outbox_close(ob);
    puddle_assignments_close(as);
    puddle_streams_close(ss);
    puddle_profiles_free(&profiles);
    puddle_replay_free(ctx.replay);
    puddle_identity_close(me.id); puddle_identity_close(alice.id); puddle_identity_close(carol.id);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
