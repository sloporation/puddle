#include "router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BODY_BUF 28000 /* what a handler may write; the rest of the 32 KB response is headers */

int64_t puddle_ctx_now_ms(const puddle_ctx *ctx)
{
    if (ctx->now_ms)
        return ctx->now_ms();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t puddle_call_now_ms(const puddle_call *call) { return puddle_ctx_now_ms(call->ctx); }

static const puddle_route *find(const puddle_router *r, const char *method, const char *path)
{
    for (size_t i = 0; i < r->count; i++)
        if (!strcmp(r->routes[i].path, path) && !strcmp(r->routes[i].method, method))
            return &r->routes[i];
    return NULL;
}

size_t puddle_router_body_limit(const puddle_router *r, const puddle_request *req, int owner_ok)
{
    const puddle_route *rt = find(r, req->method, req->path);
    if (!rt || (rt->auth == PUDDLE_AUTH_OWNER && !owner_ok))
        return 0;
    return rt->body_max;
}

int puddle_query_parse(const char *target, const char *const *keys, size_t nkeys, char (*values)[PUDDLE_QUERY_VALUE_MAX])
{
    for (size_t i = 0; i < nkeys; i++)
        values[i][0] = '\0';
    const char *q = strchr(target, '?');
    if (!q)
        return 0;
    q++;
    while (*q) {
        const char *eq = strchr(q, '=');
        const char *end = strchr(q, '&');
        if (!end)
            end = q + strlen(q);
        if (!eq || eq > end || end == q)
            return -1;
        size_t kl = (size_t)(eq - q), vl = (size_t)(end - eq - 1);
        if (vl == 0 || vl >= PUDDLE_QUERY_VALUE_MAX)
            return -1;
        size_t k = 0;
        while (k < nkeys && !(strlen(keys[k]) == kl && !strncmp(keys[k], q, kl)))
            k++;
        if (k == nkeys || values[k][0])
            return -1; /* unknown or repeated */
        for (size_t i = 0; i < vl; i++) {
            char c = eq[1 + i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return -1;
        }
        memcpy(values[k], eq + 1, vl);
        values[k][vl] = '\0';
        q = *end ? end + 1 : end;
        if (*end && !*q)
            return -1; /* a trailing & */
    }
    return 0;
}

int puddle_fail(puddle_jw *w, int status, const char *code)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "error", code);
    puddle_jw_end_object(w);
    return status;
}

static int no_peer(void *u, const char *a, const char *b, unsigned char c[32])
{
    (void)u; (void)a; (void)b; (void)c;
    return 0;
}

typedef struct {
    puddle_ctx *ctx;
    const puddle_route *route;
    const unsigned char *body;
    size_t len;
} chain_look;

static int chain_lookup(void *u, const char *sender, const char *kid, unsigned char pub[32])
{
    chain_look *c = u;
    return c->route->chain_key && c->route->chain_key(c->ctx, c->body, c->len, sender, kid, pub);
}

static size_t not_found(char *out, size_t cap, int *status)
{
    *status = 404;
    return puddle_http_error(404, out, cap);
}

size_t puddle_router_handle(const puddle_router *r, const puddle_request *req, int owner_ok,
                            const unsigned char *body, size_t body_len, char *out, size_t cap, int *status)
{
    /* the plain form: a file answer has to fit in `out` with its headers, as it can for a small image in a test */
    puddle_big big = { NULL, 0 };
    size_t n = puddle_router_handle_big(r, req, owner_ok, body, body_len, out, cap, status, &big);
    if (big.data) {
        if (n && n + big.len <= cap) {
            memcpy(out + n, big.data, big.len);
            n += big.len;
        } else {
            *status = 500;
            n = puddle_http_error(500, out, cap);
        }
        free(big.data);
    }
    return n;
}

size_t puddle_router_handle_big(const puddle_router *r, const puddle_request *req, int owner_ok,
                                const unsigned char *body, size_t body_len, char *out, size_t cap, int *status, puddle_big *big)
{
    big->data = NULL;
    big->len = 0;
    char json[BODY_BUF];
    puddle_jw w;
    puddle_ctx *ctx = r->ctx;
    const puddle_route *rt = find(r, req->method, req->path);

    if (!rt) {
        /* The owner is told when a path exists but not for that method; anyone else learns nothing. */
        if (owner_ok) {
            char allow[64] = "";
            for (size_t i = 0; i < r->count; i++)
                if (!strcmp(r->routes[i].path, req->path) && r->routes[i].auth == PUDDLE_AUTH_OWNER &&
                    strlen(allow) + strlen(r->routes[i].method) + 4 < sizeof allow)
                    snprintf(allow + strlen(allow), sizeof allow - strlen(allow), "%s%s", *allow ? ", " : "",
                             r->routes[i].method);
            if (*allow) {
                char hdr[96];
                snprintf(hdr, sizeof hdr, "Allow: %s\r\n", allow);
                *status = 405;
                return puddle_http_response(405, hdr, "{\"error\":\"Method Not Allowed\"}", 29, out, cap);
            }
        }
        return not_found(out, cap, status);
    }
    if (body_len > rt->body_max)
        return not_found(out, cap, status); /* the server never reads this much; belt and braces */

    puddle_blob blob;
    memset(&blob, 0, sizeof blob);
    puddle_call call = { .ctx = ctx, .blob = &blob, .req = req, .body = body, .body_len = body_len, .auth = rt->auth };
    char sender[PUDDLE_USER_ID_LEN + 1];

    if (rt->auth == PUDDLE_AUTH_OWNER) {
        if (!owner_ok)
            return not_found(out, cap, status);
    } else {
        char me[PUDDLE_USER_ID_LEN + 1];
        puddle_identity_id(ctx->identity, me);
        chain_look cl = { ctx, rt, body, body_len };
        puddle_peer_key_fn lookup = rt->auth == PUDDLE_AUTH_CHAIN ? chain_lookup : (ctx->peer_key ? ctx->peer_key : no_peer);
        void *user = rt->auth == PUDDLE_AUTH_CHAIN ? (void *)&cl : ctx->peer_key_user;
        int64_t now = puddle_ctx_now_ms(ctx);
        puddle_reqsig sig;
        puddle_authn a = puddle_reqsig_authenticate(req->signature, req->method, req->target, body, body_len, me,
                                                    lookup, user, ctx->replay, now, &sig);
        if (a == PUDDLE_AUTHN_REJECT)
            return not_found(out, cap, status);
        /* from here the sender is who it says, so it may be told what went wrong */
        puddle_jw_init(&w, json, sizeof json);
        if (a == PUDDLE_AUTHN_SKEW) {
            puddle_jw_object(&w);
            puddle_jw_str(&w, "error", "clock_skew");
            puddle_jw_num(&w, "now", now);
            puddle_jw_end_object(&w);
            *status = 401;
        } else if (a == PUDDLE_AUTHN_REPLAYED) {
            *status = puddle_fail(&w, 401, "replayed");
        } else if (a != PUDDLE_AUTHN_OK) {
            *status = puddle_fail(&w, 503, "busy");
        } else {
            memcpy(sender, sig.id, sizeof sender);
            call.sender = sender;
            goto run;
        }
        size_t n = puddle_jw_finish(&w);
        return puddle_http_response(*status, "", json, n, out, cap);
    }

run:
    puddle_jw_init(&w, json, sizeof json);
    int st = rt->fn(&call, &w);
    if (blob.data) { /* a file: headers here, the bytes sent after them */
        *status = st;
        size_t hl = puddle_http_head(st, blob.mime[0] ? blob.mime : "application/octet-stream",
                                     "Cache-Control: private, max-age=31536000, immutable\r\nX-Content-Type-Options: nosniff\r\n", blob.len, out, cap);
        if (!hl) {
            free(blob.data);
            *status = 500;
            return puddle_http_error(500, out, cap);
        }
        big->data = blob.data;
        big->len = blob.len;
        return hl;
    }
    size_t n = 0;
    if (w.len == 0 && !w.err) {
        json[0] = '\0'; /* empty answer */
    } else {
        n = puddle_jw_finish(&w);
        if (n == 0) {
            *status = 500;
            return puddle_http_error(500, out, cap);
        }
    }
    *status = st;
    size_t total = puddle_http_response(st, "", json, n, out, cap);
    memset(json, 0, sizeof json); /* may have held a recovery phrase */
    return total;
}
