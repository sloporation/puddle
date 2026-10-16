#include "outbox.h"

#include "fsutil.h"
#include "json.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RECORD_VERSION 1
#define MINUTE_MS (60 * 1000LL)
#define HOUR_MS (60 * MINUTE_MS)

struct puddle_outbox {
    puddle_outbox_hooks hooks;
    char dir[1100];
    pthread_mutex_t mu;
    pthread_cond_t wake;
    pthread_cond_t idle;       /* signalled when a peer's delivery thread ends */
    char inflight[PUDDLE_OUTBOX_PARALLEL][PUDDLE_USER_ID_LEN + 1]; /* peers with a thread running */
    size_t ninflight;
    puddle_ob_item *items;     /* oldest first */
    size_t n, cap;
    uint64_t next_seq;
    pthread_t thread;
    int running, stop;
};

typedef struct {
    puddle_outbox *ob;
    char peer[PUDDLE_USER_ID_LEN + 1];
} peer_job;

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static int64_t clock_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int64_t now_of(const puddle_outbox *ob) { return ob->hooks.now_ms ? ob->hooks.now_ms() : clock_ms(); }

int64_t puddle_outbox_retry_delay_ms(int64_t first_offline_ms, int64_t now_ms)
{
    return now_ms - first_offline_ms < 24 * HOUR_MS ? 5 * MINUTE_MS : 30 * MINUTE_MS;
}

/* ---- files ---- */

static const char *const STATE[] = { "queued", "failed", "held" };

static void item_path(const puddle_outbox *ob, uint64_t seq, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%012llu.json", ob->dir, (unsigned long long)seq);
}

static int save_item(const puddle_outbox *ob, const puddle_ob_item *it)
{
    size_t cap = it->body_len * 6 + 2048;
    char *buf = malloc(cap);
    if (!buf)
        return -1;
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_num(&w, "v", RECORD_VERSION);
    puddle_jw_num(&w, "seq", (int64_t)it->seq);
    puddle_jw_str(&w, "peer", it->peer);
    puddle_jw_str(&w, "kind", it->kind);
    puddle_jw_str(&w, "method", it->method);
    puddle_jw_str(&w, "path", it->path);
    if (it->max_reply)
        puddle_jw_num(&w, "max_reply", (int64_t)it->max_reply);
    puddle_jw_key(&w, "body");
    puddle_jw_string_n(&w, it->body, it->body_len);
    puddle_jw_num(&w, "created", it->created_ms);
    puddle_jw_num(&w, "next", it->next_ms);
    puddle_jw_num(&w, "first_offline", it->first_offline_ms);
    puddle_jw_num(&w, "attempts", it->attempts);
    puddle_jw_str(&w, "state", STATE[it->state]);
    puddle_jw_num(&w, "status", it->status);
    puddle_jw_str(&w, "reason", it->reason);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    int rc = -1;
    if (n) {
        char path[1200];
        item_path(ob, it->seq, path, sizeof path);
        rc = puddle_write_file_atomic(ob->dir, path, buf, n);
    }
    free(buf);
    return rc;
}

static void remove_file(const puddle_outbox *ob, uint64_t seq)
{
    char path[1200];
    item_path(ob, seq, path, sizeof path);
    unlink(path);
}

static void item_free(puddle_ob_item *it)
{
    free(it->body);
    it->body = NULL;
}

static int item_copy(puddle_ob_item *dst, const puddle_ob_item *src)
{
    *dst = *src;
    dst->body = malloc(src->body_len + 1);
    if (!dst->body)
        return -1;
    memcpy(dst->body, src->body, src->body_len + 1);
    return 0;
}

static int read_item(const char *path, puddle_ob_item *it)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t cap = 2 * PUDDLE_OUTBOX_BODY_MAX * 6 / 4 + 4096;
    char *text = malloc(cap + 1);
    if (!text) {
        fclose(f);
        return -1;
    }
    size_t n = fread(text, 1, cap + 1, f);
    fclose(f);
    int rc = -1;
    memset(it, 0, sizeof *it);
    if (n == 0 || n > cap || !puddle_json_valid_object(text, n))
        goto out;
    puddle_span v;
    int64_t i;
    char tmp[64];
#define INT(key, dst) (puddle_json_member(text, n, key, &v) == 0 && puddle_json_int64(v, &i) == 0 ? ((dst) = i, 0) : -1)
    int64_t seq = 0, version = 0, state = 0, attempts = 0, status = 0;
    if (INT("v", version) || version != RECORD_VERSION || INT("seq", seq) || seq < 1 || INT("created", it->created_ms) ||
        INT("next", it->next_ms) || INT("first_offline", it->first_offline_ms) || INT("attempts", attempts) ||
        INT("status", status))
        goto out;
#undef INT
    it->seq = (uint64_t)seq;
    it->attempts = (int)attempts;
    it->status = (int)status;
    if (puddle_json_member(text, n, "peer", &v) != 0 || puddle_json_string(v, it->peer, sizeof it->peer) != 0 ||
        !puddle_id_valid(it->peer))
        goto out;
    if (puddle_json_member(text, n, "kind", &v) != 0 || puddle_json_string(v, it->kind, sizeof it->kind) != 0)
        goto out;
    if (puddle_json_member(text, n, "path", &v) != 0 || puddle_json_string(v, it->path, sizeof it->path) != 0 ||
        strncmp(it->path, "/stream/v1/", 11) != 0)
        goto out;
    snprintf(it->method, sizeof it->method, "POST");
    int mrc = puddle_json_member(text, n, "method", &v);
    if (mrc < 0 || (mrc == 0 && (puddle_json_string(v, it->method, sizeof it->method) != 0 || (strcmp(it->method, "POST") && strcmp(it->method, "GET")))))
        goto out;
    mrc = puddle_json_member(text, n, "max_reply", &v);
    if (mrc < 0 || (mrc == 0 && (puddle_json_int64(v, &i) != 0 || i < 1 || i > 64 * 1024 * 1024)))
        goto out;
    if (mrc == 0)
        it->max_reply = (size_t)i;
    if (puddle_json_member(text, n, "reason", &v) != 0 || puddle_json_string(v, it->reason, sizeof it->reason) != 0)
        goto out;
    if (puddle_json_member(text, n, "state", &v) != 0 || puddle_json_string(v, tmp, sizeof tmp) != 0)
        goto out;
    for (state = 0; state < 3 && strcmp(tmp, STATE[state]) != 0; state++)
        ;
    if (state == 3)
        goto out;
    it->state = (puddle_ob_state)state;
    if (puddle_json_member(text, n, "body", &v) != 0 || v.len < 2 || v.p[0] != '"')
        goto out;
    it->body = malloc(v.len + 1);
    if (!it->body || puddle_json_string(v, it->body, v.len + 1) != 0)
        goto out;
    it->body_len = strlen(it->body);
    if (it->body_len > PUDDLE_OUTBOX_BODY_MAX || (!strcmp(it->method, "POST") && !puddle_json_valid_object(it->body, it->body_len)))
        goto out;
    rc = 0;
out:
    if (rc != 0)
        item_free(it);
    free(text);
    return rc;
}

static int cmp_seq(const void *a, const void *b)
{
    const puddle_ob_item *x = a, *y = b;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static int push(puddle_outbox *ob, const puddle_ob_item *it)
{
    if (ob->n == ob->cap) {
        size_t cap = ob->cap ? ob->cap * 2 : 16;
        puddle_ob_item *n = realloc(ob->items, cap * sizeof *n);
        if (!n)
            return -1;
        ob->items = n;
        ob->cap = cap;
    }
    ob->items[ob->n++] = *it;
    return 0;
}

puddle_outbox *puddle_outbox_open(const char *data_dir, const puddle_outbox_hooks *hooks, char *err, size_t errlen)
{
    puddle_outbox *ob = calloc(1, sizeof *ob);
    if (!ob) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    ob->hooks = *hooks;
    ob->next_seq = 1;
    pthread_mutex_init(&ob->mu, NULL);
    pthread_cond_init(&ob->wake, NULL);
    pthread_cond_init(&ob->idle, NULL);
    snprintf(ob->dir, sizeof ob->dir, "%s/state/outbox", data_dir);
    if (puddle_mkdir_p(ob->dir) != 0) {
        seterr(err, errlen, "cannot create %s: %s", ob->dir, strerror(errno));
        puddle_outbox_close(ob);
        return NULL;
    }
    DIR *d = opendir(ob->dir);
    if (!d) {
        seterr(err, errlen, "cannot read %s: %s", ob->dir, strerror(errno));
        puddle_outbox_close(ob);
        return NULL;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl != 17 || strcmp(de->d_name + 12, ".json") != 0)
            continue; /* .tmp files and anything that is not <seq>.json */
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", ob->dir, de->d_name);
        puddle_ob_item it;
        if (read_item(path, &it) != 0) {
            puddle_log(PUDDLE_LOG_WARN, "skipping damaged outbox file %s", path);
            continue;
        }
        if (push(ob, &it) != 0) {
            item_free(&it);
            break;
        }
        if (it.seq >= ob->next_seq)
            ob->next_seq = it.seq + 1;
    }
    closedir(d);
    qsort(ob->items, ob->n, sizeof *ob->items, cmp_seq);
    return ob;
}

void puddle_outbox_close(puddle_outbox *ob)
{
    if (!ob)
        return;
    if (ob->running) {
        pthread_mutex_lock(&ob->mu);
        ob->stop = 1;
        pthread_cond_broadcast(&ob->wake);
        pthread_mutex_unlock(&ob->mu);
        pthread_join(ob->thread, NULL);
    }
    pthread_mutex_lock(&ob->mu);
    while (ob->ninflight > 0) /* a delivery in progress finishes (it is bounded by the client's timeouts) */
        pthread_cond_wait(&ob->idle, &ob->mu);
    pthread_mutex_unlock(&ob->mu);
    for (size_t i = 0; i < ob->n; i++)
        item_free(&ob->items[i]);
    free(ob->items);
    pthread_mutex_destroy(&ob->mu);
    pthread_cond_destroy(&ob->wake);
    pthread_cond_destroy(&ob->idle);
    free(ob);
}

/* ---- queueing ---- */

static int path_ok(const char *p, int allow_query)
{
    size_t n = strlen(p);
    if (n < 12 || n >= 160 || strncmp(p, "/stream/v1/", 11) != 0)
        return 0;
    int in_query = 0;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '?') {
            if (!allow_query || in_query)
                return 0;
            in_query = 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || (in_query && (c == '=' || c == '&')))) {
            return 0;
        }
    }
    return 1;
}

static int kind_ok(const char *k)
{
    size_t n = strlen(k);
    if (n == 0 || n >= 32)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= '0' && k[i] <= '9') || k[i] == '_'))
            return 0;
    return 1;
}

static int enqueue_impl(puddle_outbox *ob, const char *peer, const char *kind, const char *method, const char *path, const char *body,
                        size_t body_len, size_t max_reply, char *err, size_t errlen)
{
    int is_get = !strcmp(method, "GET");
    if (!puddle_id_valid(peer) || !kind_ok(kind) || !path_ok(path, is_get) || body_len > PUDDLE_OUTBOX_BODY_MAX ||
        (is_get ? body_len != 0 : (body_len == 0 || !puddle_json_valid_object(body, body_len)))) {
        seterr(err, errlen, "not something the outbox can send");
        return -1;
    }
    puddle_ob_item it;
    memset(&it, 0, sizeof it);
    memcpy(it.peer, peer, sizeof it.peer);
    snprintf(it.kind, sizeof it.kind, "%s", kind);
    snprintf(it.method, sizeof it.method, "%s", method);
    snprintf(it.path, sizeof it.path, "%s", path);
    it.max_reply = max_reply;
    it.body = malloc(body_len + 1);
    if (!it.body) {
        seterr(err, errlen, "out of memory");
        return -1;
    }
    if (body_len)
        memcpy(it.body, body, body_len);
    it.body[body_len] = '\0';
    it.body_len = body_len;
    it.state = PUDDLE_OB_QUEUED;
    it.created_ms = it.next_ms = now_of(ob);

    pthread_mutex_lock(&ob->mu);
    size_t queued = 0;
    for (size_t i = 0; i < ob->n; i++)
        queued += ob->items[i].state == PUDDLE_OB_QUEUED;
    int rc = -1;
    if (queued >= (size_t)(ob->hooks.max_queued ? ob->hooks.max_queued : PUDDLE_OUTBOX_MAX_QUEUED)) {
        seterr(err, errlen, "too many things are waiting to be sent");
    } else {
        it.seq = ob->next_seq;
        if (save_item(ob, &it) != 0) {
            seterr(err, errlen, "cannot save to the outbox: %s", strerror(errno));
        } else if (push(ob, &it) != 0) {
            remove_file(ob, it.seq);
            seterr(err, errlen, "out of memory");
        } else {
            ob->next_seq++;
            rc = 0;
            pthread_cond_broadcast(&ob->wake);
        }
    }
    pthread_mutex_unlock(&ob->mu);
    if (rc != 0)
        item_free(&it);
    return rc;
}

int puddle_outbox_enqueue(puddle_outbox *ob, const char *peer, const char *kind, const char *path, const char *body, size_t body_len,
                          char *err, size_t errlen)
{
    return enqueue_impl(ob, peer, kind, "POST", path, body, body_len, 0, err, errlen);
}

int puddle_outbox_enqueue_get(puddle_outbox *ob, const char *peer, const char *kind, const char *path, size_t max_reply, char *err,
                              size_t errlen)
{
    if (max_reply == 0 || max_reply > 64u * 1024 * 1024) {
        seterr(err, errlen, "not something the outbox can send");
        return -1;
    }
    return enqueue_impl(ob, peer, kind, "GET", path, "", 0, max_reply, err, errlen);
}

/* ---- delivering ---- */

typedef struct {
    puddle_outbox *ob;
    const char *peer;
} fp_ctx;

static int fp_cb(void *u, const char *fp)
{
    fp_ctx *c = u;
    return c->ob->hooks.fingerprint ? c->ob->hooks.fingerprint(c->ob->hooks.user, c->peer, fp) : 1;
}

static void attempt(puddle_outbox *ob, const puddle_ob_item *it, puddle_net_reply *reply)
{
    puddle_address addrs[4];
    int n = ob->hooks.addresses ? ob->hooks.addresses(ob->hooks.user, it->peer, addrs, 4) : 0;
    if (n <= 0) {
        memset(reply, 0, sizeof *reply);
        reply->result = PUDDLE_NET_FAILED;
        snprintf(reply->reason, sizeof reply->reason, "we have no address for them");
        return;
    }
    fp_ctx fc = { ob, it->peer };
    puddle_net_opts opts = ob->hooks.net;
    opts.fingerprint_ok = fp_cb;
    opts.user = &fc;
    if (it->max_reply)
        opts.max_body = it->max_reply;
    for (int i = 0; i < n; i++) {
        puddle_net_request(&addrs[i], it->method, it->path, it->body_len ? it->body : NULL, it->body_len, ob->hooks.identity, it->peer, &opts, reply);
        if (reply->result != PUDDLE_NET_OFFLINE)
            return; /* reached something: that is the answer */
        if (i + 1 < n)
            puddle_net_reply_free(reply);
    }
}

/* the failed items beyond the cap are forgotten, oldest first. Caller holds the lock. */
static void prune_failed(puddle_outbox *ob)
{
    size_t failed = 0;
    for (size_t i = 0; i < ob->n; i++)
        failed += ob->items[i].state == PUDDLE_OB_FAILED;
    size_t keep = (size_t)(ob->hooks.max_failed ? ob->hooks.max_failed : PUDDLE_OUTBOX_MAX_FAILED);
    for (size_t i = 0; failed > keep && i < ob->n;) {
        if (ob->items[i].state == PUDDLE_OB_FAILED) {
            remove_file(ob, ob->items[i].seq);
            item_free(&ob->items[i]);
            memmove(&ob->items[i], &ob->items[i + 1], (ob->n - i - 1) * sizeof *ob->items);
            ob->n--;
            failed--;
        } else {
            i++;
        }
    }
}

static puddle_ob_item *find_seq(puddle_outbox *ob, uint64_t seq)
{
    for (size_t i = 0; i < ob->n; i++)
        if (ob->items[i].seq == seq)
            return &ob->items[i];
    return NULL;
}

static void remove_seq(puddle_outbox *ob, uint64_t seq)
{
    for (size_t i = 0; i < ob->n; i++)
        if (ob->items[i].seq == seq) {
            remove_file(ob, seq);
            item_free(&ob->items[i]);
            memmove(&ob->items[i], &ob->items[i + 1], (ob->n - i - 1) * sizeof *ob->items);
            ob->n--;
            return;
        }
}

/* Deliver what is waiting for one peer, oldest first, until it is all gone, or the peer is away, or its
 * certificate has changed. This is the only thing that ever runs for a peer, and only one runs at a time, so
 * a peer's items go out in order; different peers run side by side. */
static void deliver_peer(puddle_outbox *ob, const char *peer)
{
    int online = 0; /* once they have answered, the rest of their items follow whether or not they were due */
    for (;;) {
        int64_t now = now_of(ob);
        pthread_mutex_lock(&ob->mu);
        puddle_ob_item *head = NULL;
        for (size_t i = 0; i < ob->n && !head; i++) /* a key change first, then the oldest */
            if (ob->items[i].state == PUDDLE_OB_QUEUED && !strcmp(ob->items[i].peer, peer) && !strcmp(ob->items[i].kind, "identity"))
                head = &ob->items[i];
        for (size_t i = 0; i < ob->n && !head; i++)
            if (ob->items[i].state == PUDDLE_OB_QUEUED && !strcmp(ob->items[i].peer, peer))
                head = &ob->items[i];
        puddle_ob_item it;
        int have = head && (online || head->next_ms <= now);
        int copied = have && item_copy(&it, head) == 0;
        pthread_mutex_unlock(&ob->mu);
        if (!copied)
            return; /* nothing due, nothing left, or out of memory (it will be found again) */

        puddle_net_reply reply;
        attempt(ob, &it, &reply);
        now = now_of(ob);

        puddle_ob_outcome outcome = PUDDLE_OB_REJECTED;
        int offline = 0, reached = 1;
        char reason[200] = "";
        if (reply.result == PUDDLE_NET_OK && reply.status >= 200 && reply.status < 300) {
            outcome = PUDDLE_OB_DELIVERED;
        } else if (reply.result == PUDDLE_NET_OK) {
            snprintf(reason, sizeof reason, "they answered HTTP %d", reply.status);
        } else if (reply.result == PUDDLE_NET_CERT_CHANGED) {
            outcome = PUDDLE_OB_HOLD;
            reached = 0;
            snprintf(reason, sizeof reason, "%s", reply.reason);
        } else if (reply.result == PUDDLE_NET_OFFLINE) {
            offline = 1;
            reached = 0;
            snprintf(reason, sizeof reason, "%s", reply.reason);
        } else {
            snprintf(reason, sizeof reason, "%s", reply.reason);
        }

        pthread_mutex_lock(&ob->mu);
        puddle_ob_item *live = find_seq(ob, it.seq);
        if (live) {
            live->attempts++;
            live->status = reply.status;
            if (offline) {
                int64_t fo = live->first_offline_ms ? live->first_offline_ms : now;
                int64_t next = now + puddle_outbox_retry_delay_ms(fo, now);
                snprintf(live->reason, sizeof live->reason, "%s", reason);
                for (size_t i = 0; i < ob->n; i++) {
                    puddle_ob_item *o = &ob->items[i];
                    if (o->state == PUDDLE_OB_QUEUED && !strcmp(o->peer, it.peer)) {
                        if (!o->first_offline_ms)
                            o->first_offline_ms = fo;
                        o->next_ms = next;
                        save_item(ob, o);
                    }
                }
            } else if (outcome == PUDDLE_OB_DELIVERED) {
                remove_seq(ob, it.seq);
            } else if (outcome == PUDDLE_OB_HOLD) {
                for (size_t i = 0; i < ob->n; i++) {
                    puddle_ob_item *o = &ob->items[i];
                    if (o->state == PUDDLE_OB_QUEUED && !strcmp(o->peer, it.peer)) {
                        o->state = PUDDLE_OB_HELD;
                        snprintf(o->reason, sizeof o->reason, "%s", reason);
                        save_item(ob, o);
                    }
                }
            } else {
                live->state = PUDDLE_OB_FAILED;
                snprintf(live->reason, sizeof live->reason, "%s", reason);
                save_item(ob, live);
                prune_failed(ob);
            }
            if (reached) { /* they answered, so whatever was counting their absence starts over */
                for (size_t i = 0; i < ob->n; i++) {
                    puddle_ob_item *o = &ob->items[i];
                    if (o->state == PUDDLE_OB_QUEUED && !strcmp(o->peer, it.peer) && o->first_offline_ms) {
                        o->first_offline_ms = 0;
                        o->next_ms = now;
                        save_item(ob, o);
                    }
                }
            }
        }
        pthread_mutex_unlock(&ob->mu);

        if (ob->hooks.done && live && !offline) {
            snprintf(it.reason, sizeof it.reason, "%s", reason);
            it.status = reply.status;
            ob->hooks.done(ob->hooks.user, &it, outcome, &reply);
        }
        puddle_net_reply_free(&reply);
        item_free(&it);
        if (offline || outcome == PUDDLE_OB_HOLD)
            return;
        online = 1;
    }
}

/* ---- running peers side by side ---- */

static void *peer_thread(void *arg)
{
    peer_job *job = arg;
    puddle_outbox *ob = job->ob;
    deliver_peer(ob, job->peer);
    pthread_mutex_lock(&ob->mu);
    for (size_t i = 0; i < ob->ninflight; i++)
        if (!strcmp(ob->inflight[i], job->peer)) {
            memcpy(ob->inflight[i], ob->inflight[--ob->ninflight], PUDDLE_USER_ID_LEN + 1);
            break;
        }
    pthread_cond_broadcast(&ob->idle); /* a poll may be waiting for this, and the worker may have more to start */
    pthread_cond_broadcast(&ob->wake);
    pthread_mutex_unlock(&ob->mu);
    free(job);
    return NULL;
}

/* Start a thread for every peer whose oldest item is due and who has none running, up to the limit.
 * Returns how many were started. */
static int dispatch(puddle_outbox *ob)
{
    int64_t now = now_of(ob);
    int started = 0;
    pthread_mutex_lock(&ob->mu);
    for (size_t i = 0; i < ob->n && ob->ninflight < PUDDLE_OUTBOX_PARALLEL; i++) {
        const puddle_ob_item *it = &ob->items[i];
        if (it->state != PUDDLE_OB_QUEUED)
            continue;
        int running = 0, older = 0;
        for (size_t k = 0; k < ob->ninflight; k++)
            running |= !strcmp(ob->inflight[k], it->peer);
        for (size_t k = 0; k < i; k++)
            older |= ob->items[k].state == PUDDLE_OB_QUEUED && !strcmp(ob->items[k].peer, it->peer);
        if (running || older || it->next_ms > now) /* only a peer's oldest item decides whether it is due */
            continue;
        peer_job *job = malloc(sizeof *job);
        if (!job)
            break;
        job->ob = ob;
        memcpy(job->peer, it->peer, sizeof job->peer);
        memcpy(ob->inflight[ob->ninflight], it->peer, sizeof it->peer);
        ob->ninflight++;
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&t, &attr, peer_thread, job);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            ob->ninflight--;
            free(job);
            break;
        }
        started++;
    }
    pthread_mutex_unlock(&ob->mu);
    return started;
}

void puddle_outbox_poll(puddle_outbox *ob)
{
    /* wait for each round to finish; peers beyond the limit get a later round */
    for (int round = 0; round < 1000; round++) {
        int started = dispatch(ob);
        pthread_mutex_lock(&ob->mu);
        while (ob->ninflight > 0)
            pthread_cond_wait(&ob->idle, &ob->mu);
        pthread_mutex_unlock(&ob->mu);
        if (!started)
            return;
    }
}

/* ---- the worker ---- */

static void *worker(void *arg)
{
    puddle_outbox *ob = arg;
    for (;;) {
        pthread_mutex_lock(&ob->mu);
        int stop = ob->stop;
        pthread_mutex_unlock(&ob->mu);
        if (stop)
            return NULL;
        dispatch(ob); /* does not wait: a slow peer delays no one but itself */

        pthread_mutex_lock(&ob->mu);
        int64_t now = now_of(ob), wait = 60000;
        for (size_t i = 0; i < ob->n; i++)
            if (ob->items[i].state == PUDDLE_OB_QUEUED && ob->items[i].next_ms - now < wait)
                wait = ob->items[i].next_ms - now;
        if (wait < 250)
            wait = 250;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += wait / 1000;
        until.tv_nsec += (wait % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        if (!ob->stop)
            pthread_cond_timedwait(&ob->wake, &ob->mu, &until);
        pthread_mutex_unlock(&ob->mu);
    }
}

int puddle_outbox_start(puddle_outbox *ob)
{
    if (ob->running)
        return 0;
    if (pthread_create(&ob->thread, NULL, worker, ob) != 0)
        return -1;
    ob->running = 1;
    return 0;
}

/* ---- the owner's view ---- */

int puddle_outbox_release(puddle_outbox *ob, const char *peer)
{
    int n = 0;
    int64_t now = now_of(ob);
    pthread_mutex_lock(&ob->mu);
    for (size_t i = 0; i < ob->n; i++) {
        puddle_ob_item *o = &ob->items[i];
        if (o->state == PUDDLE_OB_HELD && !strcmp(o->peer, peer)) {
            o->state = PUDDLE_OB_QUEUED;
            o->next_ms = now;
            o->first_offline_ms = 0;
            o->reason[0] = '\0';
            save_item(ob, o);
            n++;
        }
    }
    if (n)
        pthread_cond_broadcast(&ob->wake);
    pthread_mutex_unlock(&ob->mu);
    return n;
}

int puddle_outbox_rewrite(puddle_outbox *ob, uint64_t seq, const char *body, size_t body_len)
{
    if (body_len == 0 || body_len > PUDDLE_OUTBOX_BODY_MAX || !puddle_json_valid_object(body, body_len))
        return -1;
    char *copy = malloc(body_len + 1);
    if (!copy)
        return -1;
    memcpy(copy, body, body_len);
    copy[body_len] = '\0';
    int rc = 1;
    pthread_mutex_lock(&ob->mu);
    puddle_ob_item *it = find_seq(ob, seq);
    if (it && it->state != PUDDLE_OB_FAILED) {
        char *old = it->body;
        size_t old_len = it->body_len;
        it->body = copy;
        it->body_len = body_len;
        if (save_item(ob, it) != 0) {
            it->body = old;
            it->body_len = old_len;
            rc = -1;
        } else {
            free(old);
            copy = NULL;
            rc = 0;
        }
    }
    pthread_mutex_unlock(&ob->mu);
    free(copy);
    return rc;
}

int puddle_outbox_retry_now(puddle_outbox *ob, const char *peer)
{
    int n = 0;
    int64_t now = now_of(ob);
    pthread_mutex_lock(&ob->mu);
    for (size_t i = 0; i < ob->n; i++) {
        puddle_ob_item *o = &ob->items[i];
        if (o->state == PUDDLE_OB_QUEUED && !strcmp(o->peer, peer)) {
            o->next_ms = now;
            o->first_offline_ms = 0;
            save_item(ob, o);
            n++;
        }
    }
    if (n)
        pthread_cond_broadcast(&ob->wake);
    pthread_mutex_unlock(&ob->mu);
    return n;
}

int puddle_outbox_requeue(puddle_outbox *ob, uint64_t seq)
{
    int rc = 1;
    int64_t now = now_of(ob);
    pthread_mutex_lock(&ob->mu);
    puddle_ob_item *it = find_seq(ob, seq);
    if (it && it->state == PUDDLE_OB_FAILED) {
        it->state = PUDDLE_OB_QUEUED;
        it->attempts = 0;
        it->status = 0;
        it->reason[0] = '\0';
        it->next_ms = now;
        it->first_offline_ms = 0;
        save_item(ob, it);
        pthread_cond_broadcast(&ob->wake);
        rc = 0;
    }
    pthread_mutex_unlock(&ob->mu);
    return rc;
}

int puddle_outbox_list(puddle_outbox *ob, puddle_ob_item **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    pthread_mutex_lock(&ob->mu);
    puddle_ob_item *l = calloc(ob->n ? ob->n : 1, sizeof *l);
    int rc = l ? 0 : -1;
    for (size_t i = 0; l && i < ob->n; i++)
        if (item_copy(&l[i], &ob->items[i]) != 0) {
            for (size_t j = 0; j < i; j++)
                item_free(&l[j]);
            free(l);
            l = NULL;
            rc = -1;
        }
    if (l) {
        *out = l;
        *count = ob->n;
    }
    pthread_mutex_unlock(&ob->mu);
    return rc;
}

void puddle_outbox_list_free(puddle_ob_item *items, size_t count)
{
    for (size_t i = 0; i < count; i++)
        item_free(&items[i]);
    free(items);
}

int puddle_outbox_dismiss(puddle_outbox *ob, uint64_t seq)
{
    int rc = 1;
    pthread_mutex_lock(&ob->mu);
    puddle_ob_item *it = find_seq(ob, seq);
    if (it && it->state != PUDDLE_OB_QUEUED) {
        remove_seq(ob, seq);
        rc = 0;
    }
    pthread_mutex_unlock(&ob->mu);
    return rc;
}
