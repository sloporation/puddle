#include "messages.h"

#include "encoding.h"
#include "hash.h"
#include "message.h"
#include "phrase.h"

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
/* a message or edit the way a peer sends it: sealed to us */
static size_t sealed_for_me(const char *type, const char *chat, const char *sender, int64_t ts, const char *ref_or_target, const char *body, char *out, size_t cap)
{
    unsigned char k[1][32];
    puddle_identity_public(me.id, NULL, k[0], NULL);
    return !strcmp(type, "edit") ? puddle_edit_build_sealed(chat, sender, ts, ref_or_target, body, strlen(body), (const unsigned char (*)[32])k, 1, out, cap)
                                 : puddle_message_build_sealed(chat, sender, ts, body, strlen(body), ref_or_target, (const unsigned char (*)[32])k, 1, out, cap);
}

static puddle_ctx ctx;
static puddle_router R;
static puddle_streams *ss;
static puddle_store *store;
static puddle_outbox *ob;
static int g_addr_mode; /* 0: we know no address for anyone; 1: a closed port on this machine (so: offline) */
static int addr_cb(void *u, const char *peer, puddle_address *out, int max)
{
    (void)u; (void)peer;
    return g_addr_mode && max > 0 && puddle_address_parse("127.0.0.1:1", &out[0]) == 0 ? 1 : 0;
}

static void make(person *p, const char *name)
{
    char dir[700], err[300];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    p->id = puddle_identity_create(dir, p->phrase, err, sizeof err);
    if (!p->id) exit(2);
    puddle_identity_confirm(dir);
    puddle_identity_id(p->id, p->uid);
}

static void roll(person *p)
{
    char fresh[PUDDLE_PHRASE_LEN + 1], err[300];
    puddle_roll *r = puddle_identity_roll_prepare(p->id, p->phrase, fresh, err, sizeof err);
    if (!r || puddle_identity_roll_commit(p->id, r, err, sizeof err) != 0) exit(2);
    memcpy(p->phrase, fresh, sizeof p->phrase);
}

static void set_peer(person *p, puddle_stream_state state)
{
    puddle_stream rec;
    char err[300];
    size_t cl;
    char *chain = puddle_identity_chain(p->id, &cl);
    CHECK(puddle_stream_init(&rec, p->uid, state, T0) == 0);
    CHECK(puddle_stream_set_chain(&rec, chain, cl) == 0);
    free(chain);
    CHECK(puddle_stream_add_address(&rec, "203.0.113.9:8443") == 0);
    CHECK(puddle_streams_put(ss, &rec, err, sizeof err) == 0);
    puddle_stream_free(&rec);
}

typedef struct { int status; char out[40000]; size_t n; } reply;

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

static reply *owner(const char *method, const char *target, const char *body) { return call(method, target, body, NULL, 1); }

static reply *peer_post(person *from, const char *body)
{
    char sig[800];
    if (puddle_reqsig_sign(from->id, "POST", "/stream/v1/events", body, strlen(body), me.uid, clock_now, sig, sizeof sig) != 0) exit(2);
    return call("POST", "/stream/v1/events", body, sig, 0);
}

/* an envelope: {"id":..,"event":"<escaped>","sig":..} */
typedef struct { char id[65]; char *event; char sig[PUDDLE_SIG_TEXT_MAX]; } env;

/* just the one conversation's object out of a list, so a check cannot match its neighbour */
static const char *conv_of(const reply *r, const char *uid)
{
    static char one[1200];
    const char *at = strstr(body_of(r), uid);
    if (!at) return NULL;
    const char *end = strstr(at, "{\"user\"");
    size_t n = end ? (size_t)(end - at) : strlen(at);
    if (n >= sizeof one) n = sizeof one - 1;
    memcpy(one, at, n);
    one[n] = '\0';
    return one;
}

static char chat_with_me(person *p, char *out) { puddle_dm_chat_id(me.uid, p->uid, out); return 0; }

static env make_env(person *signer, person *claimed_sender, const char *chat, int64_t ts, const char *body, const char *ref)
{
    env e;
    char ev[PUDDLE_EVENT_MAX_BYTES + 100];
    size_t el = sealed_for_me("message", chat, claimed_sender->uid, ts, ref, body, ev, sizeof ev);
    if (!el) { fprintf(stderr, "cannot build\n"); exit(2); }
    e.event = strdup(ev);
    puddle_sha256_hex(ev, el, e.id);
    puddle_event_sign(signer->id, ev, el, e.sig);
    return e;
}

/* {"chat":"<chat>","events":[...]} */
/* an envelope for an event whose bytes are already made */
static env env_of(person *signer, const char *event)
{
    env e;
    e.event = strdup(event);
    puddle_sha256_hex(event, strlen(event), e.id);
    puddle_event_sign(signer->id, event, strlen(event), e.sig);
    return e;
}

static char *delivery(const char *chat, env *es, size_t n)
{
    size_t cap = 1 << 20;
    char *buf = malloc(cap);
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "chat", chat);
    puddle_jw_key(&w, "events");
    puddle_jw_array(&w);
    for (size_t i = 0; i < n; i++) {
        puddle_jw_object(&w);
        puddle_jw_str(&w, "id", es[i].id);
        puddle_jw_str(&w, "event", es[i].event);
        puddle_jw_str(&w, "sig", es[i].sig);
        puddle_jw_end_object(&w);
    }
    puddle_jw_end_array(&w);
    puddle_jw_end_object(&w);
    if (!puddle_jw_finish(&w)) exit(2);
    return buf;
}

static int count_in(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

static size_t stored(const char *chat)
{
    puddle_event_list l;
    char err[200];
    if (puddle_store_tail(store, chat, 10000, &l, err, sizeof err) != 0) return (size_t)-1;
    size_t n = l.count;
    puddle_event_list_free(&l);
    return n;
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-messages-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    make(&me, "me"); make(&alice, "alice"); make(&carol, "carol");
    char err[300];
    snprintf(data, sizeof data, "%s/data", root);
    store = puddle_store_open(data, err, sizeof err);
    ss = puddle_streams_open(data, err, sizeof err);
    puddle_outbox_hooks hooks = { .identity = me.id, .now_ms = now_cb };
    ob = puddle_outbox_open(data, &hooks, err, sizeof err);
    ctx = (puddle_ctx){ .identity = me.id, .store = store, .streams = ss, .outbox = ob, .replay = puddle_replay_new(1024), .now_ms = now_cb,
                        .peer_key = puddle_streams_peer_key, .peer_key_user = ss };
    R = (puddle_router){ .ctx = &ctx };
    R.routes = puddle_all_routes(&R.count);

    char chat_a[64], chat_c[64];
    chat_with_me(&alice, chat_a);
    chat_with_me(&carol, chat_c);
    char chat_ac[64];
    puddle_dm_chat_id(alice.uid, carol.uid, chat_ac);
    reply *r;
    char *b;
    env e1;

    /* ---- a peer delivers ---- */
    set_peer(&alice, PUDDLE_STREAM_ACTIVE);
    e1 = make_env(&alice, &alice, chat_a, T0 + 10, "hello Bob", NULL);
    b = delivery(chat_a, &e1, 1);
    r = peer_post(&alice, b);
    CHECK(r->status == 200 && strstr(body_of(r), e1.id) && strstr(body_of(r), "\"accepted\":[\"") && !strstr(body_of(r), "\"duplicate\":[\""));
    CHECK(stored(chat_a) == 1);
    {
        puddle_event ev;
        CHECK(puddle_store_get(store, chat_a, e1.id, &ev, err, sizeof err) == 0);
        CHECK(!strcmp(ev.event, e1.event) && !strcmp(ev.sig, e1.sig));                          /* kept exactly as signed */
        puddle_event_free(&ev);
    }
    {
        /* the very same request sent twice is a replay, whatever it carries */
        char sig[800];
        puddle_reqsig_sign(alice.id, "POST", "/stream/v1/events", b, strlen(b), me.uid, clock_now, sig, sizeof sig);
        CHECK(call("POST", "/stream/v1/events", b, sig, 0)->status == 200);
        CHECK(call("POST", "/stream/v1/events", b, sig, 0)->status == 401);
    }
    clock_now += 1000;
    r = peer_post(&alice, b);                                                                    /* the same message in a new request */
    CHECK(r->status == 200 && strstr(body_of(r), "\"accepted\":[]") && strstr(body_of(r), "\"duplicate\":[\"") && stored(chat_a) == 1);
    free(b);

    /* a delivery of several, some bad: each is judged alone */
    {
        clock_now += 1000;
        env es[14];
        int n = 0;
        es[n++] = make_env(&alice, &alice, chat_a, T0 + 20, "second", NULL);                    /* 0 good */
        es[n++] = make_env(&alice, &carol, chat_a, T0 + 21, "as Carol", NULL);                  /* 1 wrong sender */
        es[n++] = make_env(&alice, &alice, chat_ac, T0 + 22, "wrong chat", NULL);               /* 2 wrong chat */
        es[n] = make_env(&alice, &alice, chat_a, T0 + 23, "tampered id", NULL);                 /* 3 id is not the hash */
        es[n].id[0] = es[n].id[0] == 'a' ? 'b' : 'a';
        n++;
        es[n] = make_env(&alice, &alice, chat_a, T0 + 24, "tampered body", NULL);               /* 4 bytes changed after signing */
        { char *p = strstr(es[n].event, "\"body\":\"") + 8 + 100; p[0] = p[0] == 'A' ? 'B' : 'A'; puddle_sha256_hex(es[n].event, strlen(es[n].event), es[n].id); }
        n++;
        es[n++] = make_env(&carol, &alice, chat_a, T0 + 25, "forged", NULL);                    /* 5 signed by someone else */
        es[n++] = make_env(&alice, &alice, chat_a, T0 + 26, "x", NULL);                         /* 6 good (placeholder replaced below) */
        { free(es[n - 1].event); es[n - 1].event = strdup("{\"type\":\"chat.create\",\"chat\":\"x\",\"sender\":\"y\",\"ts\":1,\"body\":\"b\"}");
          puddle_sha256_hex(es[n - 1].event, strlen(es[n - 1].event), es[n - 1].id);
          puddle_event_sign(alice.id, es[n - 1].event, strlen(es[n - 1].event), es[n - 1].sig); }  /* 6 a type we do not take */
        es[n++] = make_env(&alice, &alice, chat_a, 0, "no time", NULL);                         /* 7 bad time */
        es[n] = make_env(&alice, &alice, chat_a, T0 + 28, "bad sig", NULL);                     /* 8 garbage signature */
        snprintf(es[n].sig, sizeof es[n].sig, "%.64s:AAAA", es[n].sig);
        n++;
        es[n] = make_env(&alice, &alice, chat_a, T0 + 29, "no sig", NULL);                      /* 9 no signature at all */
        es[n].sig[0] = 0;
        n++;
        es[n] = make_env(&alice, &alice, chat_a, T0 + 30, "other key", NULL);                   /* 10 a key that is not theirs */
        {
            char kid[65];
            unsigned char cp[32];
            puddle_identity_public(carol.id, cp, NULL, NULL);
            puddle_key_id(cp, kid);
            snprintf(es[n].sig, sizeof es[n].sig, "%s%s", kid, strchr(es[n].sig, ':'));
        }
        n++;
        es[n++] = make_env(&alice, &alice, chat_a, T0 + 31, "third", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"); /* 11 good, a reply */
        es[n] = make_env(&alice, &alice, chat_a, T0 + 32, "dup in same delivery", NULL);        /* 12 and 13: the same twice */
        n++;
        es[n] = es[n - 1];
        es[n].event = strdup(es[n - 1].event);
        n++;
        b = delivery(chat_a, es, (size_t)n);
        r = peer_post(&alice, b);
        CHECK(r->status == 200);
        const char *o = body_of(r);
        CHECK(count_in(o, "\"reason\"") == 10);
        const char *whys[] = { "wrong_sender", "wrong_chat", "bad_id", "bad_id", "unknown_key", "unsupported_type", "bad_time", "bad_signature", "bad_signature" };
        (void)whys;
        CHECK(strstr(o, "wrong_sender") && strstr(o, "wrong_chat") && strstr(o, "bad_id") && strstr(o, "unknown_key") && strstr(o, "unsupported_type") &&
              strstr(o, "bad_time") && strstr(o, "bad_signature"));
        CHECK(count_in(o, "\"duplicate\":[\"") == 1 && strstr(o, es[13].id));                   /* the repeat inside a delivery is a duplicate */
        CHECK(strstr(o, es[0].id) && strstr(o, es[11].id) && strstr(o, es[12].id));
        CHECK(stored(chat_a) == 4);                                                             /* hello, second, third, the one repeated */
        for (int i = 0; i < n; i++) free(es[i].event);
        free(b);
    }

    /* the delivery as a whole */
    {
        clock_now += 1000;
        e1 = make_env(&alice, &alice, chat_a, T0 + 40, "x", NULL);
        b = delivery(chat_c, &e1, 1);
        CHECK(peer_post(&alice, b)->status == 400);                                              /* a chat that is not yours with us */
        free(b);
        clock_now += 1000;
        CHECK(peer_post(&alice, "{\"chat\":\"nonsense\",\"events\":[]}")->status == 400);
        clock_now += 1000;
        CHECK(peer_post(&alice, "not json")->status == 400);
        clock_now += 1000;
        char t[400];
        snprintf(t, sizeof t, "{\"chat\":\"%s\"}", chat_a);
        CHECK(peer_post(&alice, t)->status == 400);                                              /* no events */
        clock_now += 1000;
        snprintf(t, sizeof t, "{\"chat\":\"%s\",\"events\":{}}", chat_a);
        CHECK(peer_post(&alice, t)->status == 400);                                              /* not a list */
        clock_now += 1000;
        snprintf(t, sizeof t, "{\"chat\":\"%s\",\"events\":[1,2,", chat_a);
        CHECK(peer_post(&alice, t)->status == 400);
        clock_now += 1000;
        snprintf(t, sizeof t, "{\"chat\":\"%s\",\"events\":[]}", chat_a);
        r = peer_post(&alice, t);
        CHECK(r->status == 200 && strstr(body_of(r), "\"accepted\":[]"));                       /* nothing is fine */
        clock_now += 1000;
        snprintf(t, sizeof t, "{\"chat\":\"%s\",\"events\":[1,\"x\",[],null,{}]}", chat_a);
        r = peer_post(&alice, t);
        CHECK(r->status == 200 && count_in(body_of(r), "\"reason\":\"malformed\"") == 5);        /* junk entries are refused one by one */
        clock_now += 1000;
        env many[PUDDLE_DELIVERY_MAX_EVENTS + 1];
        for (int i = 0; i <= PUDDLE_DELIVERY_MAX_EVENTS; i++) many[i] = make_env(&alice, &alice, chat_a, T0 + 1000 + i, "bulk", NULL);
        b = delivery(chat_a, many, PUDDLE_DELIVERY_MAX_EVENTS + 1);
        CHECK(peer_post(&alice, b)->status == 400);                                              /* over the limit: refused whole */
        CHECK(stored(chat_a) == 4);
        free(b);
        clock_now += 1000;
        b = delivery(chat_a, many, PUDDLE_DELIVERY_MAX_EVENTS);
        r = peer_post(&alice, b);
        CHECK(r->status == 200 && count_in(body_of(r), "\"reason\"") == 0 && stored(chat_a) == 4 + 100);   /* the same words, a hundred times: each is its own message */
        for (int i = 0; i <= PUDDLE_DELIVERY_MAX_EVENTS; i++) free(many[i].event);
        free(b);
    }

    /* an event that is too large for one message */
    {
        clock_now += 1000;
        env e;
        e.event = malloc(PUDDLE_EVENT_MAX_BYTES + 10);
        memset(e.event, 'x', PUDDLE_EVENT_MAX_BYTES + 5);
        e.event[0] = '{';
        e.event[PUDDLE_EVENT_MAX_BYTES + 4] = '}';
        e.event[PUDDLE_EVENT_MAX_BYTES + 5] = 0;
        puddle_sha256_hex(e.event, strlen(e.event), e.id);
        snprintf(e.sig, sizeof e.sig, "x");
        b = delivery(chat_a, &e, 1);
        r = peer_post(&alice, b);
        CHECK(r->status == 200 && strstr(body_of(r), "too_large"));
        free(b); free(e.event);
    }

    /* ---- who may deliver ---- */
    {
        clock_now += 1000;
        e1 = make_env(&carol, &carol, chat_c, T0 + 50, "hi", NULL);
        b = delivery(chat_c, &e1, 1);
        char sig[800];
        puddle_reqsig_sign(carol.id, "POST", "/stream/v1/events", b, strlen(b), me.uid, clock_now, sig, sizeof sig);
        CHECK(call("POST", "/stream/v1/events", b, sig, 0)->status == 404);                      /* a stranger: no such route */
        set_peer(&carol, PUDDLE_STREAM_PENDING_IN);
        clock_now += 1000;
        CHECK(peer_post(&carol, b)->status == 403 && strstr(body_of(call("POST", "/nothing", "{}", NULL, 0)), "Not Found"));  /* known, but no open stream */
        reply *rr = peer_post(&carol, b);
        clock_now += 1000;
        rr = peer_post(&carol, b);
        CHECK(rr->status == 403 && strstr(body_of(rr), "no_stream"));
        set_peer(&carol, PUDDLE_STREAM_CLOSED);
        clock_now += 1000;
        CHECK(peer_post(&carol, b)->status == 403);
        CHECK(stored(chat_c) == 0);
        set_peer(&carol, PUDDLE_STREAM_ACTIVE);
        CHECK(puddle_streams_block(ss, carol.uid, T0, err, sizeof err) == 0);
        clock_now += 1000;
        CHECK(peer_post(&carol, b)->status == 404);                                              /* blocked: as if not there */
        CHECK(puddle_streams_unblock(ss, carol.uid, err, sizeof err) == 0);
        set_peer(&carol, PUDDLE_STREAM_ACTIVE);
        clock_now += 1000;
        CHECK(peer_post(&carol, b)->status == 200 && stored(chat_c) == 1);
        free(b);
        /* Alice cannot deliver into Carol's chat with us, nor Carol into Alice's */
        e1 = make_env(&alice, &alice, chat_c, T0 + 51, "into carol's chat", NULL);
        b = delivery(chat_c, &e1, 1);
        clock_now += 1000;
        CHECK(peer_post(&alice, b)->status == 400 && stored(chat_c) == 1);
        free(b);
    }

    /* ---- after a key change ---- */
    {
        roll(&alice);                                                                             /* we have not been told yet */
        clock_now += 1000;
        e1 = make_env(&alice, &alice, chat_a, T0 + 60, "after the roll", NULL);
        b = delivery(chat_a, &e1, 1);
        r = peer_post(&alice, b);
        CHECK(r->status == 404);                                                                  /* her new key is not one we hold: not her */
        free(b);
        set_peer(&alice, PUDDLE_STREAM_ACTIVE);                                                   /* the news arrives (4.2 will do this) */
        clock_now += 1000;
        b = delivery(chat_a, &e1, 1);
        r = peer_post(&alice, b);
        CHECK(r->status == 200 && strstr(body_of(r), "\"accepted\":[\""));
        free(b);
    }

    /* ---- the owner sends ---- */
    {
        size_t before_chat = stored(chat_a);
        char body[1200], cs[64];
        snprintf(body, sizeof body, "{\"to\":\"%s\",\"body\":\"hi Alice\"}", alice.uid);
        CHECK(call("POST", "/api/v1/dm/send", body, NULL, 0)->status == 404);                    /* not the owner */
        clock_now += 1000;
        r = owner("POST", "/api/v1/dm/send", body);
        CHECK(r->status == 201 && strstr(body_of(r), "\"body\":\"hi Alice\"") && strstr(body_of(r), "\"outgoing\":true") && strstr(body_of(r), chat_a));
        CHECK(stored(chat_a) == before_chat + 1);
        puddle_ob_item *l;
        size_t n;
        puddle_outbox_list(ob, &l, &n);
        CHECK(n == 1 && !strcmp(l[0].kind, "events") && !strcmp(l[0].path, "/stream/v1/events") && !strcmp(l[0].peer, alice.uid));
        {
            /* what is queued is what the other side will accept: the id is the hash, and the signature is ours */
            puddle_span chat_v, evs, ev, id_v, event_v, sig_v;
            size_t cur = 0;
            CHECK(puddle_json_member(l[0].body, l[0].body_len, "events", &evs) == 0 && puddle_json_array_next(evs, &cur, &ev) == 1);
            CHECK(puddle_json_member(ev.p, ev.len, "id", &id_v) == 0 && puddle_json_member(ev.p, ev.len, "event", &event_v) == 0 &&
                  puddle_json_member(ev.p, ev.len, "sig", &sig_v) == 0);
            char idt[80], sigt[PUDDLE_SIG_TEXT_MAX], *evt = malloc(event_v.len + 1), hash[65], kid[65];
            unsigned char sg[64], mp[32];
            CHECK(puddle_json_string(id_v, idt, sizeof idt) == 0 && puddle_json_string(event_v, evt, event_v.len + 1) == 0 &&
                  puddle_json_string(sig_v, sigt, sizeof sigt) == 0);
            puddle_sha256_hex(evt, strlen(evt), hash);
            CHECK(!strcmp(hash, idt) && puddle_event_sig_parse(sigt, kid, sg) == 0);
            puddle_identity_public(me.id, mp, NULL, NULL);
            CHECK(puddle_event_verify(mp, evt, strlen(evt), sg));
            CHECK(puddle_json_member(l[0].body, l[0].body_len, "chat", &chat_v) == 0);
            free(evt);
        }
        puddle_outbox_list_free(l, n);

        /* the same words twice are two messages */
        r = owner("POST", "/api/v1/dm/send", body);
        CHECK(r->status == 201 && stored(chat_a) == before_chat + 2);
        /* a reply */
        char first_id[80] = "";
        {
            puddle_event_list tail;
            puddle_store_tail(store, chat_a, 1, &tail, err, sizeof err);
            snprintf(first_id, sizeof first_id, "%s", tail.items[0].id);
            puddle_event_list_free(&tail);
        }
        snprintf(body, sizeof body, "{\"to\":\"%s\",\"body\":\"re\",\"reply_to\":\"%s\"}", alice.uid, first_id);
        r = owner("POST", "/api/v1/dm/send", body);
        CHECK(r->status == 201 && strstr(body_of(r), first_id) && strstr(body_of(r), "\"reply_to\""));

        /* what it refuses */
        snprintf(cs, sizeof cs, "%s", carol.uid);
        struct { const char *body; int status; const char *code; } bad[8];
        char b1[400], b2[400], b3[400], b4[400], b5[400], b6[400];
        snprintf(b1, sizeof b1, "{\"to\":\"%s\",\"body\":\"\"}", alice.uid);
        snprintf(b2, sizeof b2, "{\"to\":\"%s\",\"body\":\"bell\\u0007\"}", alice.uid);
        snprintf(b3, sizeof b3, "{\"to\":\"%s\"}", alice.uid);
        snprintf(b4, sizeof b4, "{\"to\":\"%s\",\"body\":\"x\"}", me.uid);
        snprintf(b5, sizeof b5, "{\"to\":\"%s\",\"body\":\"x\",\"reply_to\":\"nope\"}", alice.uid);
        snprintf(b6, sizeof b6, "{\"to\":\"%s\",\"body\":5}", alice.uid);
        bad[0] = (typeof(bad[0])){ b1, 400, "bad_body" };
        bad[1] = (typeof(bad[0])){ b2, 400, "bad_body" };
        bad[2] = (typeof(bad[0])){ b3, 400, "bad_request" };
        bad[3] = (typeof(bad[0])){ b4, 400, "cannot_message_yourself" };
        bad[4] = (typeof(bad[0])){ b5, 400, "bad_request" };
        bad[5] = (typeof(bad[0])){ b6, 400, "bad_request" };
        bad[6] = (typeof(bad[0])){ "{\"to\":\"junk\",\"body\":\"x\"}", 400, "bad_request" };
        bad[7] = (typeof(bad[0])){ "nonsense", 400, "bad_request" };
        size_t after = stored(chat_a);
        for (int i = 0; i < 8; i++) {
            r = owner("POST", "/api/v1/dm/send", bad[i].body);
            if (r->status != bad[i].status || !strstr(body_of(r), bad[i].code)) { fprintf(stderr, "FAIL send refusal %d: %d %s\n", i, r->status, body_of(r)); failures++; }
        }
        {
            char big[4200 + 100];
            snprintf(big, sizeof big, "{\"to\":\"%s\",\"body\":\"", alice.uid);
            size_t k = strlen(big);
            memset(big + k, 'x', 4001);
            strcpy(big + k + 4001, "\"}");
            CHECK(owner("POST", "/api/v1/dm/send", big)->status == 400);
            memset(big + k, 'x', 4000);
            strcpy(big + k + 4000, "\"}");
            CHECK(owner("POST", "/api/v1/dm/send", big)->status == 201);                          /* exactly 4000 */
            after++;
        }
        CHECK(stored(chat_a) == after);                                                           /* nothing refused was kept */
        /* only to someone we have an open stream with */
        snprintf(body, sizeof body, "{\"to\":\"%s\",\"body\":\"x\"}", cs);
        set_peer(&carol, PUDDLE_STREAM_CLOSED);
        CHECK(owner("POST", "/api/v1/dm/send", body)->status == 403 && strstr(body_of(r = owner("POST", "/api/v1/dm/send", body)), "no_stream"));
        set_peer(&carol, PUDDLE_STREAM_PENDING_OUT);
        CHECK(owner("POST", "/api/v1/dm/send", body)->status == 403);
        set_peer(&carol, PUDDLE_STREAM_ACTIVE);
        CHECK(puddle_streams_block(ss, carol.uid, T0, err, sizeof err) == 0);
        CHECK(owner("POST", "/api/v1/dm/send", body)->status == 403);
        CHECK(puddle_streams_unblock(ss, carol.uid, err, sizeof err) == 0);
        char stranger[64];
        snprintf(stranger, sizeof stranger, "%s", "zaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        snprintf(body, sizeof body, "{\"to\":\"%s\",\"body\":\"x\"}", stranger);
        CHECK(owner("POST", "/api/v1/dm/send", body)->status == 403);
        CHECK(stored(chat_c) == 1);
    }

    /* ---- the owner reads ---- */
    {
        reply *rr;
        char t[300], first[64], last[64];
        snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s", carol.uid);
        rr = owner("GET", t, NULL);
        CHECK(rr->status == 200 && strstr(body_of(rr), "\"body\":\"hi\"") && strstr(body_of(rr), "\"outgoing\":false") && strstr(body_of(rr), "\"next\":null"));
        snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s", "zaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        rr = owner("GET", t, NULL);
        CHECK(rr->status == 200 && !strcmp(body_of(rr), "{\"messages\":[],\"next\":null}"));      /* no one: nothing */

        /* the whole chat with Alice, in pages of twenty, oldest of each page first */
        size_t total = stored(chat_a);
        int seen = 0;
        size_t offset = 0;
        int64_t prev_ts_of_older_page = 0;
        (void)prev_ts_of_older_page;
        int pages = 0;
        for (;;) {
            snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s&limit=20&offset=%zu", alice.uid, offset);
            rr = owner("GET", t, NULL);
            CHECK(rr->status == 200);
            int in_page = count_in(body_of(rr), "\"outgoing\"");
            seen += in_page;
            pages++;
            puddle_span arr, it;
            size_t cur = 0;
            int64_t last_ts = 0;
            const char *bd = body_of(rr);
            CHECK(puddle_json_member(bd, strlen(bd), "messages", &arr) == 0);
            while (puddle_json_array_next(arr, &cur, &it) == 1) {
                puddle_span tv;
                int64_t ts = 0;
                CHECK(puddle_json_member(it.p, it.len, "ts", &tv) == 0 && puddle_json_int64(tv, &ts) == 0 && ts > last_ts);   /* chat order */
                last_ts = ts;
            }
            puddle_span nx;
            CHECK(puddle_json_member(bd, strlen(bd), "next", &nx) == 0);
            if (nx.len == 4 && !memcmp(nx.p, "null", 4)) break;
            offset = (size_t)atoi(nx.p);
            if (pages > 30) { failures++; break; }
        }
        CHECK((size_t)seen == total && pages == (int)((total + 19) / 20));
        (void)first; (void)last;
        snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s&limit=1", alice.uid);
        rr = owner("GET", t, NULL);
        CHECK(count_in(body_of(rr), "\"outgoing\"") == 1 && strstr(body_of(rr), "\"outgoing\":true"));  /* the newest is the 4000-character one we sent */
        const char *badq[] = { "", "?user=junk", "?user=%s&limit=0", "?user=%s&limit=201", "?user=%s&limit=x", "?user=%s&offset=-1", "?user=%s&x=1",
                               "?user=%s&user=%s" };
        for (size_t i = 0; i < sizeof badq / sizeof *badq; i++) {
            char q[300];
            snprintf(q, sizeof q, badq[i], alice.uid, alice.uid);
            snprintf(t, sizeof t, "/api/v1/dm/messages%s", q);
            if (owner("GET", t, NULL)->status != 400) { fprintf(stderr, "FAIL query %s accepted\n", t); failures++; }
        }
        snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s", alice.uid);
        CHECK(call("GET", t, NULL, NULL, 0)->status == 404);

        /* long messages: a page holds what fits, and no message is lost between pages */
        set_peer(&carol, PUDDLE_STREAM_ACTIVE);                                                   /* (blocking her closed the stream) */
        char longbody[4001 * 4];
        for (int i = 0; i < 4000; i++) memcpy(longbody + i * 4, "\xF0\x9F\x99\x82", 4);
        longbody[16000] = 0;
        for (int i = 0; i < 8; i++) {
            clock_now += 1000;
            env e = make_env(&carol, &carol, chat_c, T0 + 500000 + i, longbody, NULL);
            char *dl = delivery(chat_c, &e, 1);
            CHECK(strlen(e.event) <= PUDDLE_EVENT_MAX_BYTES);
            r = peer_post(&carol, dl);
            CHECK(r->status == 200 && strstr(body_of(r), "\"accepted\":[\""));
            free(dl); free(e.event);
        }
        int got = 0, rounds = 0;
        offset = 0;
        for (;;) {
            snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s&limit=200&offset=%zu", carol.uid, offset);
            rr = owner("GET", t, NULL);
            const char *bd = body_of(rr);
            CHECK(rr->status == 200 && puddle_json_valid_object(bd, strlen(bd)));
            got += count_in(bd, "\"outgoing\"");
            rounds++;
            puddle_span nx;
            CHECK(puddle_json_member(bd, strlen(bd), "next", &nx) == 0);
            if (nx.len == 4 && !memcmp(nx.p, "null", 4)) break;
            offset = (size_t)atoi(nx.p);
            if (rounds > 20) { failures++; break; }
        }
        CHECK(got == 9 && rounds >= 2);
    }

    /* ---- editing and taking back ---- */
    {
        /* a fresh pair of people so the earlier chats stay as they were */
        person dina;
        make(&dina, "dina");
        set_peer(&dina, PUDDLE_STREAM_ACTIVE);
        char chat_d[64], t[600], bodyj[900];
        chat_with_me(&dina, chat_d);
        puddle_ob_item *l;
        size_t n;

        /* messages in both directions */
        reply *rr;
        char mine[3][65];
        for (int i = 0; i < 3; i++) {
            clock_now += 1000;
            snprintf(bodyj, sizeof bodyj, "{\"to\":\"%s\",\"body\":\"mine %d\"}", dina.uid, i);
            rr = owner("POST", "/api/v1/dm/send", bodyj);
            CHECK(rr->status == 201);
            const char *p = strstr(body_of(rr), "\"id\":\"");
            snprintf(mine[i], 65, "%.64s", p + 6);
        }
        env theirs = make_env(&dina, &dina, chat_d, T0 + 900000, "theirs 0", NULL);
        char *dl = delivery(chat_d, &theirs, 1);
        clock_now += 1000;
        CHECK(peer_post(&dina, dl)->status == 200);
        free(dl);

        /* edit */
        clock_now += 1000;
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"mine 1, corrected\"}", dina.uid, mine[1]);
        CHECK(call("POST", "/api/v1/dm/edit", bodyj, NULL, 0)->status == 404);                        /* not the owner */
        rr = owner("POST", "/api/v1/dm/edit", bodyj);
        CHECK(rr->status == 200 && strstr(body_of(rr), "\"body\":\"mine 1, corrected\"") && strstr(body_of(rr), "\"edited\":true") &&
              strstr(body_of(rr), "\"edited_at\":") && strstr(body_of(rr), mine[1]) && strstr(body_of(rr), "\"outgoing\":true"));
        puddle_outbox_list(ob, &l, &n);
        {
            int found = 0;
            for (size_t i = 0; i < n; i++)
                if (!strcmp(l[i].peer, dina.uid) && strstr(l[i].body, mine[1]) && strstr(l[i].body, "\\\"type\\\":\\\"edit\\\"")) found++;
            CHECK(found == 1);                                                                        /* the edit is queued as an event of its own */
        }
        puddle_outbox_list_free(l, n);
        snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s", dina.uid);
        rr = owner("GET", t, NULL);
        CHECK(rr->status == 200 && count_in(body_of(rr), "\"outgoing\"") == 4 && strstr(body_of(rr), "mine 1, corrected") && !strstr(body_of(rr), "\"mine 1\""));
        CHECK(count_in(body_of(rr), "\"edited\":true") == 1);                                         /* edits are not messages: four, not five */
        clock_now += 1000;
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"mine 1, corrected twice\"}", dina.uid, mine[1]);
        CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 200);
        rr = owner("GET", t, NULL);
        CHECK(strstr(body_of(rr), "corrected twice") && !strstr(body_of(rr), "mine 1, corrected\"") && count_in(body_of(rr), "\"outgoing\"") == 4);   /* the last one stands */

        /* what cannot be edited, and why */
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"x\"}", dina.uid, theirs.id);
        CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 404);                                /* hers, not ours */
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"x\"}", dina.uid, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 404);
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"\"}", dina.uid, mine[0]);
        CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 400);
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"bell\\u0007\"}", dina.uid, mine[0]);
        CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 400);
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\"}", dina.uid, mine[0]);
        CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 400);                                /* no text */
        snprintf(bodyj, sizeof bodyj, "{\"message\":\"%s\",\"body\":\"x\"}", mine[0]);
        CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 400);                                /* no user */
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"short\",\"body\":\"x\"}", dina.uid);
        CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 400);
        CHECK(owner("POST", "/api/v1/dm/edit", "nonsense")->status == 400);

        /* taking back */
        clock_now += 1000;
        size_t before = stored(chat_d);
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\"}", dina.uid, mine[0]);
        rr = owner("POST", "/api/v1/dm/delete", bodyj);
        CHECK(rr->status == 204 && !strstr(rr->out, "Content-Length") && stored(chat_d) == before + 1);
        rr = owner("GET", t, NULL);
        CHECK(count_in(body_of(rr), "\"outgoing\"") == 4 && count_in(body_of(rr), "\"deleted\":true") == 1 && !strstr(body_of(rr), "\"mine 0\""));
        CHECK(count_in(body_of(rr), "\"body\"") == 3);                                                 /* the gap has no body at all, not an empty one */
        CHECK(strstr(body_of(rr), mine[0]));                                                          /* still there, as a gap, in its place */
        CHECK(owner("POST", "/api/v1/dm/delete", bodyj)->status == 204 && stored(chat_d) == before + 1);   /* again: nothing more is sent */
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"too late\"}", dina.uid, mine[0]);
        CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 409 && strstr(body_of(owner("POST", "/api/v1/dm/edit", bodyj)), "deleted"));
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\"}", dina.uid, theirs.id);
        CHECK(owner("POST", "/api/v1/dm/delete", bodyj)->status == 404);                              /* hers */
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\"}", dina.uid, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        CHECK(owner("POST", "/api/v1/dm/delete", bodyj)->status == 404);
        CHECK(owner("POST", "/api/v1/dm/delete", "{}")->status == 400);

        /* paging counts messages, however many edits sit between them */
        for (int i = 0; i < 6; i++) {
            clock_now += 1000;
            snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"mine 2, edit %d\"}", dina.uid, mine[2], i);
            CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 200);
        }
        snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s&limit=1&offset=3", dina.uid);
        rr = owner("GET", t, NULL);
        CHECK(count_in(body_of(rr), "\"outgoing\"") == 1 && strstr(body_of(rr), mine[0]) && strstr(body_of(rr), "\"next\":null"));   /* the oldest of four */
        snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s&limit=2&offset=1", dina.uid);
        rr = owner("GET", t, NULL);
        CHECK(count_in(body_of(rr), "\"outgoing\"") == 2 && strstr(body_of(rr), "mine 1, corrected twice") && strstr(body_of(rr), "mine 2, edit 5") &&
              strstr(body_of(rr), "\"next\":3"));                                                       /* (hers is the newest; then mine 2 and mine 1) */

        /* what has to be said to Dina: no stream, no editing */
        set_peer(&dina, PUDDLE_STREAM_CLOSED);
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"x\"}", dina.uid, mine[2]);
        CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 403);
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\"}", dina.uid, mine[2]);
        CHECK(owner("POST", "/api/v1/dm/delete", bodyj)->status == 403);
        set_peer(&dina, PUDDLE_STREAM_ACTIVE);

        /* far back: the edits of a message that is many events behind */
        for (int i = 0; i < 70; i++) {
            clock_now += 10;
            snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"mine 2, many %d\"}", dina.uid, mine[2], i);
            CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 200);
        }
        env late = make_env(&dina, &dina, chat_d, T0 + 900001, "theirs 1", NULL);
        dl = delivery(chat_d, &late, 1);
        clock_now += 1000;
        CHECK(peer_post(&dina, dl)->status == 200);
        free(dl);
        snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s&limit=1&offset=3", dina.uid);
        rr = owner("GET", t, NULL);
        CHECK(strstr(body_of(rr), "mine 1, corrected twice") && count_in(body_of(rr), "\"outgoing\"") == 1);   /* behind seventy events */
        snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s&limit=1&offset=2", dina.uid);
        rr = owner("GET", t, NULL);
        CHECK(strstr(body_of(rr), "mine 2, many 69"));                                                /* 70 edits later, the last of them */
        /* an edit of something even further back, and a clock that has gone backwards */
        for (int i = 0; i < 60; i++) {
            clock_now += 10;
            snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"mine 2, more %d\"}", dina.uid, mine[2], i);
            CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 200);
        }
        clock_now += 1000;
        snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"mine 1, found far back\"}", dina.uid, mine[1]);
        CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 200);                                /* more than 128 events behind */
        {
            clock_now += 5000000;                                                                      /* a message stamped well ahead... */
            snprintf(bodyj, sizeof bodyj, "{\"to\":\"%s\",\"body\":\"stamped ahead\"}", dina.uid);
            rr = owner("POST", "/api/v1/dm/send", bodyj);
            char ahead[65];
            const char *ip = strstr(body_of(rr), "\"id\":\"");
            snprintf(ahead, sizeof ahead, "%.64s", ip + 6);
            clock_now -= 4000000;                                                                      /* ...and then the clock is set back */
            snprintf(bodyj, sizeof bodyj, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"fixed anyway\"}", dina.uid, ahead);
            CHECK(owner("POST", "/api/v1/dm/edit", bodyj)->status == 200);
            snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s&limit=200", dina.uid);
            rr = owner("GET", t, NULL);
            CHECK(strstr(body_of(rr), "fixed anyway") && !strstr(body_of(rr), "stamped ahead"));        /* the edit counted: it was stamped after what it edits */
            clock_now += 1000000;
        }
        free(theirs.event); free(late.event);

        /* ---- and from the other side ---- */
        {
            char ev[PUDDLE_EVENT_MAX_BYTES];
            env es[4];
            /* Dina edits and takes back her own, and also tries it on one of ours */
            size_t el = sealed_for_me("edit", chat_d, dina.uid, T0 + 900100, theirs.id, "theirs 0, edited", ev, sizeof ev);
            es[0] = env_of(&dina, ev);
            el = sealed_for_me("edit", chat_d, dina.uid, T0 + 900101, mine[2], "she rewrites ours", ev, sizeof ev);
            es[1] = env_of(&dina, ev);
            el = puddle_redact_build(chat_d, dina.uid, T0 + 900102, mine[2], ev, sizeof ev);
            es[2] = env_of(&dina, ev);
            el = puddle_redact_build(chat_d, dina.uid, T0 + 900103, late.id, ev, sizeof ev);
            es[3] = env_of(&dina, ev);
            (void)el;
            dl = delivery(chat_d, es, 4);
            clock_now += 1000;
            rr = peer_post(&dina, dl);
            CHECK(rr->status == 200 && count_in(body_of(rr), "\"reason\"") == 0);
            free(dl);
            for (int i = 0; i < 4; i++) free(es[i].event);
            snprintf(t, sizeof t, "/api/v1/dm/messages?user=%s&limit=200", dina.uid);
            rr = owner("GET", t, NULL);
            CHECK(strstr(body_of(rr), "theirs 0, edited") && count_in(body_of(rr), "\"outgoing\":false") == 2);           /* her edit of her own message shows */
            CHECK(!strstr(body_of(rr), "she rewrites ours") && strstr(body_of(rr), "mine 2, more 59"));                      /* her edit of ours does not */
            CHECK(count_in(body_of(rr), "\"deleted\":true") == 2);                                                        /* ours (mine 0) and her own (theirs 1) */
            /* she cannot, though, say it as someone else */
            el = sealed_for_me("edit", chat_d, me.uid, T0 + 900200, mine[2], "as the owner", ev, sizeof ev);
            env bad = env_of(&dina, ev);
            dl = delivery(chat_d, &bad, 1);
            clock_now += 1000;
            rr = peer_post(&dina, dl);
            CHECK(strstr(body_of(rr), "wrong_sender"));
            free(dl);
            free(bad.event);
        }
        puddle_identity_close(dina.id);
    }

    /* ---- a message that could not be queued says so ---- */
    {
        puddle_outbox_close(ob);
        hooks.max_queued = 1;
        ob = puddle_outbox_open(data, &hooks, err, sizeof err);
        ctx.outbox = ob;
        char body[300];
        snprintf(body, sizeof body, "{\"to\":\"%s\",\"body\":\"queue is full\"}", alice.uid);
        size_t before = stored(chat_a);
        r = owner("POST", "/api/v1/dm/send", body);
        CHECK(r->status == 500 && strstr(body_of(r), "not_queued") && stored(chat_a) == before + 1);   /* kept, and honest about it */
        ctx.outbox = NULL;
        r = owner("POST", "/api/v1/dm/send", body);
        CHECK(r->status == 500 && strstr(body_of(r), "not_queued"));
    }

    ctx.outbox = ob;

    /* ---- conversations and what has been read ---- */
    {
        person dave;
        make(&dave, "dave");
        clock_now += 100000000; /* well after everything above, so this is the newest conversation */
        char data2[720];
        snprintf(data2, sizeof data2, "%s/data2", root);
        puddle_outbox_hooks hooks2 = { .identity = me.id, .now_ms = now_cb, .addresses = addr_cb };
        puddle_outbox *ob2 = puddle_outbox_open(data2, &hooks2, err, sizeof err); /* the first one was filled by the tests above */
        CHECK(ob2 != NULL);
        ctx.outbox = ob2;
        set_peer(&dave, PUDDLE_STREAM_ACTIVE);
        char chat_d[64];
        chat_with_me(&dave, chat_d);
        CHECK(puddle_streams_read_marker(ss, dave.uid) == 0);
        clock_now += 100000;
        env d1 = make_env(&dave, &dave, chat_d, clock_now, "first\nline", NULL);
        char *db = delivery(chat_d, &d1, 1);
        CHECK(peer_post(&dave, db)->status == 200);
        free(db);
        clock_now += 1000;
        env d2 = make_env(&dave, &dave, chat_d, clock_now, "second", NULL);
        db = delivery(chat_d, &d2, 1);
        CHECK(peer_post(&dave, db)->status == 200);
        free(db);

        r = owner("GET", "/api/v1/conversations", NULL);
        b = (char *)body_of(r);
        CHECK(r->status == 200 && strstr(b, "\"conversations\":[{\"kind\":\"dm\",\"user\":\"") );
        const char *mine = conv_of(r, dave.uid);
        CHECK(mine && strstr(mine, "\"preview\":\"second\"") && strstr(mine, "\"unread\":2") && strstr(mine, "\"outgoing\":false"));
        CHECK(strstr(b, dave.uid) == strstr(b, "\"user\":\"") + 8); /* the newest conversation comes first */
        CHECK(owner("GET", "/api/v1/conversations?limit=0", NULL)->status == 400);
        CHECK(owner("GET", "/api/v1/conversations?offset=x", NULL)->status == 400);
        CHECK(call("GET", "/api/v1/conversations", NULL, NULL, 0)->status == 404);

        char rb[200];
        snprintf(rb, sizeof rb, "{\"user\":\"%s\"}", dave.uid);
        CHECK(owner("POST", "/api/v1/dm/read", rb)->status == 204);
        CHECK(puddle_streams_read_marker(ss, dave.uid) == clock_now);
        r = owner("GET", "/api/v1/conversations", NULL);
        mine = conv_of(r, dave.uid);
        CHECK(mine && strstr(mine, "\"unread\":0"));
        CHECK(owner("POST", "/api/v1/dm/read", "{\"user\":\"nope\"}")->status == 400);
        CHECK(owner("POST", "/api/v1/dm/read", "{}")->status == 400);
        CHECK(call("POST", "/api/v1/dm/read", rb, NULL, 0)->status == 404);

        /* a later message is unread again; the marker never goes backwards */
        clock_now += 1000;
        env d3 = make_env(&dave, &dave, chat_d, clock_now, "third", NULL);
        db = delivery(chat_d, &d3, 1);
        CHECK(peer_post(&dave, db)->status == 200);
        free(db);
        r = owner("GET", "/api/v1/conversations", NULL);
        mine = conv_of(r, dave.uid);
        CHECK(mine && strstr(mine, "\"unread\":1"));
        int64_t m = puddle_streams_read_marker(ss, dave.uid);
        CHECK(puddle_streams_set_read_marker(ss, dave.uid, 5, err, sizeof err) == 0 && puddle_streams_read_marker(ss, dave.uid) == m);

        /* it survives a restart */
        puddle_streams_close(ss);
        ss = puddle_streams_open(data, err, sizeof err);
        ctx.streams = ss;
        ctx.peer_key_user = ss;
        CHECK(puddle_streams_read_marker(ss, dave.uid) == m);

        /* the owner's own message shows as outgoing, and a deleted one has no preview */
        {
            char sb[300];
            snprintf(sb, sizeof sb, "{\"to\":\"%s\",\"body\":\"mine\"}", dave.uid);
            clock_now += 1000;
            CHECK(owner("POST", "/api/v1/dm/send", sb)->status == 201);
            r = owner("GET", "/api/v1/conversations", NULL);
            mine = conv_of(r, dave.uid);
            CHECK(mine && strstr(mine, "\"preview\":\"mine\"") && strstr(mine, "\"outgoing\":true") && strstr(mine, "\"unread\":1"));
        }
        /* what became of what we sent: waiting, retrying, failed, and a way to try again */
        {
            const char *mk = "/api/v1/dm/messages?user=%s";
            char url[300], sb2[300];
            snprintf(url, sizeof url, mk, dave.uid);
            r = owner("GET", url, NULL);
            b = (char *)body_of(r);
            const char *last = strrchr(b, '{');                                     /* the newest message, "mine" */
            CHECK(strstr(b, "\"body\":\"mine\"") && strstr(strstr(b, "\"body\":\"mine\""), "\"delivery\":{\"state\":\"queued\""));
            CHECK(!strstr(b, "\"body\":\"third\",\"outgoing\":false,\"delivery\"") && last);
            CHECK(count_in(b, "\"delivery\"") == 1);                                /* theirs have none */

            g_addr_mode = 0;
            puddle_outbox_poll(ob2);                                                /* no way to reach them: fails once, with the reason */
            r = owner("GET", url, NULL);
            b = (char *)body_of(r);
            CHECK(strstr(b, "\"delivery\":{\"state\":\"failed\",\"reason\":\"we have no address for them\""));
            CHECK(owner("POST", "/api/v1/dm/retry", rb)->status == 204);            /* again */
            r = owner("GET", url, NULL);
            CHECK(strstr(body_of(r), "\"state\":\"queued\"") && !strstr(body_of(r), "\"state\":\"failed\""));

            g_addr_mode = 1;
            puddle_outbox_poll(ob2);                                                /* they are offline: it will be tried again */
            r = owner("GET", url, NULL);
            b = (char *)body_of(r);
            CHECK(strstr(b, "\"delivery\":{\"state\":\"retrying\"") && strstr(b, "\"next_at\":") && strstr(b, "\"attempts\":1"));
            CHECK(owner("POST", "/api/v1/dm/retry", rb)->status == 204);            /* try now */

            /* an edit that is waiting is not mistaken for the delivery of the message it edits */
            char sbm[300];
            snprintf(sbm, sizeof sbm, "{\"to\":\"%s\",\"body\":\"mine2\"}", dave.uid);
            clock_now += 1000;
            CHECK(owner("POST", "/api/v1/dm/send", sbm)->status == 201);
            g_addr_mode = 0;
            puddle_outbox_poll(ob2);                                                /* "mine2" fails */
            r = owner("GET", url, NULL);
            b = (char *)body_of(r);
            {
                const char *at = strstr(b, "\"body\":\"mine2\"");
                CHECK(at != NULL);
                char mid[PUDDLE_ID_HEX + 1] = "";
                for (const char *q = at; q > b; q--)
                    if (!strncmp(q, "{\"id\":\"", 7)) { memcpy(mid, q + 7, PUDDLE_ID_HEX); mid[PUDDLE_ID_HEX] = 0; break; }
                CHECK(strlen(mid) == PUDDLE_ID_HEX);
                puddle_ob_item *l;
                size_t cnt;
                CHECK(puddle_outbox_list(ob2, &l, &cnt) == 0);
                for (size_t i = 0; i < cnt; i++)
                    if (!strcmp(l[i].peer, dave.uid) && strstr(l[i].body, mid) && l[i].state == PUDDLE_OB_FAILED)
                        CHECK(puddle_outbox_dismiss(ob2, l[i].seq) == 0);          /* delivered, as far as the outbox can tell */
                puddle_outbox_list_free(l, cnt);
                char eb[400];
                snprintf(eb, sizeof eb, "{\"user\":\"%s\",\"message\":\"%s\",\"body\":\"mine2 edited\"}", dave.uid, mid);
                clock_now += 1000;
                CHECK(owner("POST", "/api/v1/dm/edit", eb)->status == 200);
                r = owner("GET", url, NULL);
                b = (char *)body_of(r);
                CHECK(strstr(b, "\"body\":\"mine2 edited\"") && count_in(b, "\"delivery\"") == 1); /* only "mine" still has one */
            }
            snprintf(sb2, sizeof sb2, "{\"user\":\"%s\"}", alice.uid);
            CHECK(owner("POST", "/api/v1/dm/retry", sb2)->status == 404);           /* nothing to retry for someone else */
            CHECK(owner("POST", "/api/v1/dm/retry", "{\"user\":\"nope\"}")->status == 400);
            CHECK(call("POST", "/api/v1/dm/retry", rb, NULL, 0)->status == 404);
        }
        free(d1.event); free(d2.event); free(d3.event);
        ctx.outbox = ob;
        puddle_outbox_close(ob2);
    }

    /* ---- sealed bodies ---- */
    {
        char url[300], plainev[PUDDLE_EVENT_MAX_BYTES], tmp[PUDDLE_EVENT_MAX_BYTES];
        snprintf(url, sizeof url, "/api/v1/dm/messages?user=%s", alice.uid);
        set_peer(&alice, PUDDLE_STREAM_ACTIVE);
        size_t before = stored(chat_a);

        /* a peer's message with its text in the clear is refused: nothing from a peer travels unsealed */
        size_t pl = puddle_message_build(chat_a, alice.uid, T0 + 700000, "in the clear", 12, NULL, plainev, sizeof plainev);
        CHECK(pl > 0);
        env pe = env_of(&alice, plainev);
        b = delivery(chat_a, &pe, 1);
        clock_now += 1000;
        r = peer_post(&alice, b);
        CHECK(r->status == 200 && strstr(body_of(r), "not_encrypted") && stored(chat_a) == before);
        free(b); free(pe.event);

        /* sealed to someone else: it is signed and addressed to us, but we cannot open it */
        unsigned char ck[1][32], mk[1][32];
        puddle_identity_public(carol.id, NULL, ck[0], NULL);
        puddle_identity_public(me.id, NULL, mk[0], NULL);
        size_t el = puddle_message_build_sealed(chat_a, alice.uid, T0 + 700001, "not for you", 11, NULL, (const unsigned char (*)[32])ck, 1, plainev, sizeof plainev);
        CHECK(el > 0);
        env se = env_of(&alice, plainev);
        b = delivery(chat_a, &se, 1);
        clock_now += 1000;
        r = peer_post(&alice, b);
        CHECK(r->status == 200 && strstr(body_of(r), "undecryptable") && stored(chat_a) == before);
        free(b); free(se.event);

        /* sealed to us, but moved to another time: the blob is bound to the event it was made for */
        el = puddle_message_build_sealed(chat_a, alice.uid, T0 + 700002, "bound", 5, NULL, (const unsigned char (*)[32])mk, 1, plainev, sizeof plainev);
        CHECK(el > 0);
        {
            char *at = strstr(plainev, "\"ts\":");
            CHECK(at != NULL);
            snprintf(tmp, sizeof tmp, "%.*s\"ts\":%lld%s", (int)(at - plainev), plainev, (long long)(T0 + 700003), strchr(at, ',') );
            env me2 = env_of(&alice, tmp);
            b = delivery(chat_a, &me2, 1);
            clock_now += 1000;
            r = peer_post(&alice, b);
            CHECK(r->status == 200 && strstr(body_of(r), "undecryptable") && stored(chat_a) == before);
            free(b); free(me2.event);
        }

        /* sealed to us: taken, and the log holds the sealed event with the text beside it */
        env good = make_env(&alice, &alice, chat_a, T0 + 700004, "sealed hello", NULL);
        b = delivery(chat_a, &good, 1);
        clock_now += 1000;
        r = peer_post(&alice, b);
        CHECK(r->status == 200 && strstr(body_of(r), "\"accepted\":[\"") && stored(chat_a) == before + 1);
        free(b);
        {
            puddle_event ev;
            CHECK(puddle_store_get(store, chat_a, good.id, &ev, err, sizeof err) == 0);
            CHECK(!strstr(ev.event, "sealed hello") && strstr(ev.event, "\"encrypted\":true"));
            CHECK(ev.plain && !strcmp(ev.plain, "sealed hello") && !strcmp(ev.event, good.event));
            puddle_event_free(&ev);
        }
        r = owner("GET", url, NULL);
        CHECK(r->status == 200 && strstr(body_of(r), "sealed hello"));

        /* it survives a restart of the store: the text is in the record, not only in memory */
        puddle_store_close(store);
        store = puddle_store_open(data, err, sizeof err);
        ctx.store = store;
        r = owner("GET", url, NULL);
        CHECK(r->status == 200 && strstr(body_of(r), "sealed hello"));

        /* our keys roll: what was sealed to the old key can no longer be opened, but what we already hold still reads */
        env old = make_env(&alice, &alice, chat_a, T0 + 700005, "sealed to the old key", NULL);   /* made while she still had our old key */
        roll(&me);
        clock_now += 1000;
        b = delivery(chat_a, &old, 1);
        r = peer_post(&alice, b);
        CHECK(r->status == 200 && strstr(body_of(r), "undecryptable"));
        free(b);
        r = owner("GET", url, NULL);
        CHECK(r->status == 200 && strstr(body_of(r), "sealed hello"));                              /* history is not lost */
        env fresh = make_env(&alice, &alice, chat_a, T0 + 700006, "sealed to the new key", NULL);
        b = delivery(chat_a, &fresh, 1);
        clock_now += 1000;
        r = peer_post(&alice, b);
        CHECK(r->status == 200 && strstr(body_of(r), "\"accepted\":[\""));
        free(b);
        r = owner("GET", url, NULL);
        CHECK(strstr(body_of(r), "sealed to the new key") && strstr(body_of(r), "sealed hello"));
        free(good.event); free(old.event); free(fresh.event);
    }

    puddle_outbox_close(ob);
    puddle_streams_close(ss);
    puddle_store_close(store);
    puddle_replay_free(ctx.replay);
    puddle_identity_close(me.id); puddle_identity_close(alice.id); puddle_identity_close(carol.id);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);

    return failures ? 1 : 0;
}
