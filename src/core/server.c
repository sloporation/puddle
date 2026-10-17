#include "server.h"

#include "auth.h"
#include "http.h"
#include "identity.h"
#include "flow.h"
#include "keysync.h"
#include "log.h"
#include "outbox.h"
#include "profile.h"
#include "reqsig.h"
#include "router.h"
#include "serve.h"
#include "store.h"
#include "groupflow.h"
#include "streams.h"
#include "tls.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNECTIONS 64
#define IO_TIMEOUT_SEC 10
#define REQUEST_DEADLINE_SEC 15

struct puddle_server {
    puddle_config cfg;
    SSL_CTX *tls;
    puddle_store *store;
    puddle_auth *auth;
    puddle_identity *identity;
    puddle_profiles profiles;
    puddle_assignments *assignments;
    puddle_streams *streams;
    puddle_groups *groups;
    puddle_outbox *outbox;
    puddle_bucket *anon_bucket;
    puddle_keyed *access_limit;
    puddle_replay *replay;
    puddle_roll_slot *roll_slot;
    puddle_ctx ctx;
    puddle_router router;
    int listen_fd;
    int stop_pipe[2];
    pthread_mutex_t mu;
    pthread_cond_t drained;
    int active;
};

typedef struct {
    puddle_server *srv;
    int fd;
    char peer[INET6_ADDRSTRLEN + 1];
} conn;

/* The outbox's view of the streams: where a peer is, whether its certificate is the one we know, and what to
 * do when something reaches an end. (Acting on the ends is roadmap 3.5; for now they are logged.) */
static int ob_addresses(void *user, const char *peer, puddle_address *out, int max)
{
    puddle_stream st;
    if (puddle_streams_get(((puddle_server *)user)->streams, peer, &st) != 0)
        return 0;
    int n = 0;
    for (int i = 0; i < st.naddr && n < max; i++)
        if (puddle_address_parse(st.addresses[i], &out[n]) == 0)
            n++;
    puddle_stream_free(&st);
    return n;
}

static int ob_fingerprint(void *user, const char *peer, const char *fp)
{
    int ok = puddle_streams_check_fingerprint(((puddle_server *)user)->streams, peer, fp);
    if (!ok)
        puddle_log(PUDDLE_LOG_WARN, "%.12s… has a different certificate from before; nothing is sent until you say it is fine", peer);
    return ok;
}

static void ob_done(void *user, const puddle_ob_item *it, puddle_ob_outcome outcome, const puddle_net_reply *reply)
{
    puddle_flow_done(&((puddle_server *)user)->ctx, it, outcome, reply);
    puddle_log(outcome == PUDDLE_OB_DELIVERED ? PUDDLE_LOG_INFO : PUDDLE_LOG_WARN, "outbox: %s to %.12s… %s%s%s", it->kind, it->peer,
               outcome == PUDDLE_OB_DELIVERED ? "delivered" : outcome == PUDDLE_OB_HOLD ? "held" : "failed",
               outcome == PUDDLE_OB_DELIVERED ? "" : ": ", outcome == PUDDLE_OB_DELIVERED ? "" : it->reason);
}

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static void set_cloexec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }

static int open_listener(puddle_server *s, char *err, size_t errlen)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;

    char port[8];
    snprintf(port, sizeof port, "%u", s->cfg.port);
    int rc = getaddrinfo(s->cfg.address, port, &hints, &res);
    if (rc != 0) {
        seterr(err, errlen, "invalid listen address '%s': %s", s->cfg.address, gai_strerror(rc));
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        seterr(err, errlen, "socket: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }
    set_cloexec(fd);

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    if (res->ai_family == AF_INET6) {
        int off = 0; /* "::" serves IPv4 too */
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    }

    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
        seterr(err, errlen, "cannot bind %s port %u: %s", s->cfg.address, s->cfg.port, strerror(errno));
        goto fail;
    }
    if (listen(fd, 128) != 0) {
        seterr(err, errlen, "listen: %s", strerror(errno));
        goto fail;
    }
    /* non-blocking so a connection reset between poll() and accept() can't hang the loop */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    freeaddrinfo(res);
    s->listen_fd = fd;
    return 0;
fail:
    close(fd);
    freeaddrinfo(res);
    return -1;
}

puddle_server *puddle_server_create(const puddle_config *cfg, char *err, size_t errlen)
{
    signal(SIGPIPE, SIG_IGN);

    puddle_server *s = calloc(1, sizeof *s);
    if (!s) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    s->cfg = *cfg;
    s->listen_fd = -1;
    s->stop_pipe[0] = s->stop_pipe[1] = -1;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->drained, NULL);

    s->store = puddle_store_open(cfg->data_dir, err, errlen);
    if (!s->store)
        goto fail;
    s->auth = puddle_auth_open(cfg->data_dir, err, errlen);
    if (!s->auth)
        goto fail;
    s->identity = puddle_identity_open(cfg->data_dir, err, errlen);
    if (!s->identity)
        goto fail;
    {
        char uid[PUDDLE_USER_ID_LEN + 1];
        puddle_identity_id(s->identity, uid);
        puddle_log(PUDDLE_LOG_INFO, "identity %s", uid);
    }
    if (puddle_profiles_load(&s->profiles, cfg, err, errlen) != 0)
        goto fail;
    s->assignments = puddle_assignments_open(cfg->data_dir, err, errlen);
    if (!s->assignments)
        goto fail;
    s->streams = puddle_streams_open(cfg->data_dir, err, errlen);
    if (!s->streams)
        goto fail;
    s->groups = puddle_groups_open(cfg->data_dir, err, errlen);
    if (!s->groups)
        goto fail;
    s->replay = puddle_replay_new(0);
    if (!s->replay) {
        seterr(err, errlen, "out of memory");
        goto fail;
    }
    {
        puddle_outbox_hooks hooks = { .identity = s->identity, .addresses = ob_addresses, .fingerprint = ob_fingerprint,
                                      .done = ob_done, .user = s };
        s->outbox = puddle_outbox_open(cfg->data_dir, &hooks, err, errlen);
        if (!s->outbox)
            goto fail;
    }
    /* checking a stranger's key chain costs signatures: 50 a second on average, 100 in a burst. A requester
     * may ask five times an hour. */
    s->anon_bucket = puddle_bucket_new(50, 100);
    s->access_limit = puddle_keyed_new(1024, 5, 3600 * 1000LL);
    if (!s->anon_bucket || !s->access_limit) {
        seterr(err, errlen, "out of memory");
        goto fail;
    }
    s->roll_slot = puddle_roll_slot_new();
    if (!s->roll_slot) {
        seterr(err, errlen, "out of memory");
        goto fail;
    }
    s->ctx = (puddle_ctx){ .config = &s->cfg, .roll_slot = s->roll_slot, .identity = s->identity, .store = s->store, .profiles = &s->profiles,
                           .assignments = s->assignments, .streams = s->streams, .groups = s->groups, .outbox = s->outbox, .replay = s->replay,
                           .anon_bucket = s->anon_bucket, .access_limit = s->access_limit,
                           .peer_key = puddle_streams_peer_key, .peer_key_user = s->streams };
    s->router = (puddle_router){ .ctx = &s->ctx };
    s->router.routes = puddle_all_routes(&s->router.count);
    puddle_log(PUDDLE_LOG_INFO, "%zu profile%s loaded, default '%s'", s->profiles.count,
               s->profiles.count == 1 ? "" : "s", s->profiles.items[0].id);
    s->tls = puddle_tls_context(cfg->dir, err, errlen);
    if (!s->tls)
        goto fail;
    puddle_tls_fingerprint_hex(s->tls, s->ctx.own_tls_fp);
    if (open_listener(s, err, errlen) != 0)
        goto fail;
    if (pipe(s->stop_pipe) != 0) {
        seterr(err, errlen, "pipe: %s", strerror(errno));
        goto fail;
    }
    set_cloexec(s->stop_pipe[0]);
    set_cloexec(s->stop_pipe[1]);

    if (strchr(cfg->address, ':'))
        puddle_log(PUDDLE_LOG_INFO, "listening on https://[%s]:%u", cfg->address, cfg->port);
    else
        puddle_log(PUDDLE_LOG_INFO, "listening on https://%s:%u", cfg->address, cfg->port);
    return s;
fail:
    puddle_server_destroy(s);
    return NULL;
}

puddle_store *puddle_server_store(puddle_server *s)
{
    return s->store;
}

const puddle_profiles *puddle_server_profiles(puddle_server *s)
{
    return &s->profiles;
}

puddle_identity *puddle_server_identity(puddle_server *s)
{
    return s->identity;
}

puddle_assignments *puddle_server_assignments(puddle_server *s)
{
    return s->assignments;
}

puddle_streams *puddle_server_streams(puddle_server *s)
{
    return s->streams;
}

puddle_groups *puddle_server_groups(puddle_server *s)
{
    return s->groups;
}

puddle_outbox *puddle_server_outbox(puddle_server *s)
{
    return s->outbox;
}

void puddle_server_stop(puddle_server *s)
{
    int saved = errno;
    if (s && s->stop_pipe[1] >= 0) {
        ssize_t r = write(s->stop_pipe[1], "x", 1);
        (void)r;
    }
    errno = saved;
}

void puddle_server_destroy(puddle_server *s)
{
    if (!s)
        return;
    if (s->listen_fd >= 0) close(s->listen_fd);
    if (s->stop_pipe[0] >= 0) close(s->stop_pipe[0]);
    if (s->stop_pipe[1] >= 0) close(s->stop_pipe[1]);
    SSL_CTX_free(s->tls);
    puddle_replay_free(s->replay);
    puddle_roll_slot_free(s->roll_slot);
    puddle_outbox_close(s->outbox); /* stops its worker before what the worker uses goes away */
    puddle_streams_close(s->streams);
    puddle_groups_close(s->groups);
    puddle_bucket_free(s->anon_bucket);
    puddle_keyed_free(s->access_limit);
    puddle_assignments_close(s->assignments);
    puddle_identity_close(s->identity);
    puddle_auth_close(s->auth);
    puddle_profiles_free(&s->profiles);
    puddle_store_close(s->store);
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->drained);
    free(s);
}

/* TLS as a connection for puddle_http_serve(). The socket's receive timeout makes a stalled read fail with
 * EAGAIN, which is reported as a timeout (< 0) rather than as the other side closing (0). */
static int ssl_read_cb(void *io, void *buf, size_t cap)
{
    SSL *ssl = io;
    errno = 0;
    int n = SSL_read(ssl, buf, (int)cap);
    if (n > 0)
        return n;
    int e = errno;
    if (SSL_get_error(ssl, n) == SSL_ERROR_ZERO_RETURN)
        return 0;
    return (e == EAGAIN || e == EWOULDBLOCK) ? -1 : 0;
}

static int ssl_write_cb(void *io, const void *buf, size_t len)
{
    SSL *ssl = io;
    const char *p = buf;
    while (len) {
        int w = SSL_write(ssl, p, (int)len);
        if (w <= 0)
            return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static int owner_check_cb(void *user, const char *authorization)
{
    return puddle_auth_check_bearer(((puddle_server *)user)->auth, authorization);
}

static size_t body_limit_cb(void *user, const puddle_request *req, int owner_ok)
{
    return puddle_router_body_limit(&((puddle_server *)user)->router, req, owner_ok);
}

static size_t route_big_cb(void *user, const puddle_request *req, int owner_ok, const unsigned char *body, size_t body_len, char *out,
                           size_t cap, int *status, puddle_big *big)
{
    return puddle_router_handle_big(&((puddle_server *)user)->router, req, owner_ok, body, body_len, out, cap, status, big);
}

static size_t route_cb(void *user, const puddle_request *req, int owner_ok, const unsigned char *body,
                       size_t body_len, char *out, size_t cap, int *status)
{
    return puddle_router_handle(&((puddle_server *)user)->router, req, owner_ok, body, body_len, out, cap, status);
}

static void handle_request(puddle_server *srv, SSL *ssl, const char *peer)
{
    puddle_http_handler h = { .owner_check = owner_check_cb, .body_limit = body_limit_cb, .route = route_cb, .route_big = route_big_cb,
                              .user = srv, .deadline_seconds = REQUEST_DEADLINE_SEC };
    puddle_io io = { .read = ssl_read_cb, .write = ssl_write_cb, .io = ssl };
    puddle_serve_info info;
    puddle_http_serve(&h, &io, &info);
    if (info.status) /* the owner reads this log, so say why a 404 happened; the client is told nothing */
        puddle_log(PUDDLE_LOG_INFO, "%s %s %s [%s] -> %d", peer, info.method, info.path, info.auth, info.status);
}

/* Closing a socket that still holds unread bytes (a body we refused to read) makes the kernel send a reset, which
 * can destroy the answer before the client reads it. Say we are done, then swallow what is left, briefly. */
static void linger_close(int fd)
{
    shutdown(fd, SHUT_WR);
    char junk[1024];
    size_t total = 0;
    for (int i = 0; i < 20 && total < 65536; i++) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 50) <= 0)
            break;
        ssize_t n = read(fd, junk, sizeof junk);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
}

static void *conn_main(void *arg)
{
    conn *c = arg;
    puddle_server *s = c->srv;

    SSL *ssl = SSL_new(s->tls);
    if (ssl && SSL_set_fd(ssl, c->fd) == 1) {
        if (SSL_accept(ssl) == 1) {
            handle_request(s, ssl, c->peer);
            SSL_shutdown(ssl);
            linger_close(c->fd);
        } else {
            /* expected whenever a client rejects the self-signed cert */
            puddle_log(PUDDLE_LOG_INFO, "%s TLS handshake failed", c->peer);
        }
    }
    SSL_free(ssl);
    close(c->fd);
    free(c);

    pthread_mutex_lock(&s->mu);
    if (--s->active == 0)
        pthread_cond_broadcast(&s->drained);
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

static void prepare_client_fd(int fd)
{
    /* BSD/macOS accepted sockets inherit O_NONBLOCK from the listener */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    set_cloexec(fd);
    struct timeval tv = { IO_TIMEOUT_SEC, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

int puddle_server_run(puddle_server *s)
{
    int rc = 0;
    int synced = puddle_sync_resume(&s->ctx);
    if (synced)
        puddle_log(PUDDLE_LOG_INFO, "queued %d update%s for the people we are connected to", synced, synced == 1 ? "" : "s");
    int resumed = puddle_flow_resume(&s->ctx);
    if (resumed)
        puddle_log(PUDDLE_LOG_INFO, "queued %d answer%s that were still owed", resumed, resumed == 1 ? "" : "s");
    puddle_group_reconcile(&s->ctx); /* connect to group members we are not yet connected to */
    if (puddle_outbox_start(s->outbox) != 0)
        puddle_log(PUDDLE_LOG_WARN, "cannot start the outbox; nothing will be sent to other Puddles");

    for (;;) {
        struct pollfd pfd[2] = {
            { s->listen_fd, POLLIN, 0 },
            { s->stop_pipe[0], POLLIN, 0 },
        };
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            puddle_log(PUDDLE_LOG_ERROR, "poll: %s", strerror(errno));
            rc = -1;
            break;
        }
        if (pfd[1].revents)
            break;
        if (pfd[0].revents & (POLLERR | POLLNVAL)) {
            puddle_log(PUDDLE_LOG_ERROR, "listening socket failed");
            rc = -1;
            break;
        }
        if (!(pfd[0].revents & POLLIN))
            continue;

        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int fd = accept(s->listen_fd, (struct sockaddr *)&ss, &sl);
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE) {
                puddle_log(PUDDLE_LOG_WARN, "accept: out of file descriptors");
                usleep(100000);
            }
            continue; /* EINTR, EAGAIN, ECONNABORTED, ... */
        }
        prepare_client_fd(fd);

        conn *c = calloc(1, sizeof *c);
        if (!c) {
            close(fd);
            continue;
        }
        c->srv = s;
        c->fd = fd;
        if (getnameinfo((struct sockaddr *)&ss, sl, c->peer, sizeof c->peer, NULL, 0,
                        NI_NUMERICHOST) != 0)
            snprintf(c->peer, sizeof c->peer, "?");

        pthread_mutex_lock(&s->mu);
        int busy = s->active >= MAX_CONNECTIONS;
        if (!busy)
            s->active++;
        pthread_mutex_unlock(&s->mu);
        if (busy) {
            puddle_log(PUDDLE_LOG_WARN, "%s rejected: too many connections", c->peer);
            close(fd);
            free(c);
            continue;
        }

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t tid;
        int pc = pthread_create(&tid, &attr, conn_main, c);
        pthread_attr_destroy(&attr);
        if (pc != 0) {
            puddle_log(PUDDLE_LOG_ERROR, "pthread_create: %s", strerror(pc));
            pthread_mutex_lock(&s->mu);
            s->active--;
            pthread_mutex_unlock(&s->mu);
            close(fd);
            free(c);
        }
    }

    close(s->listen_fd);
    s->listen_fd = -1;

    /* in-flight connections are bounded by IO_TIMEOUT_SEC / REQUEST_DEADLINE_SEC */
    pthread_mutex_lock(&s->mu);
    while (s->active > 0)
        pthread_cond_wait(&s->drained, &s->mu);
    pthread_mutex_unlock(&s->mu);

    puddle_log(PUDDLE_LOG_INFO, "stopped");
    return rc;
}
