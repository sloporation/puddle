#include "client.h"

#include "hash.h"
#include "reqsig.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void why(puddle_net_reply *r, puddle_net_result res, const char *fmt, ...)
{
    r->result = res;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->reason, sizeof r->reason, fmt, ap);
    va_end(ap);
}

void puddle_net_reply_free(puddle_net_reply *r)
{
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
}

static int64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- which addresses we will not dial ---- */

static int v4_dialable(const unsigned char a[4])
{
    if (a[0] == 0) return 0;                          /* 0.0.0.0/8: "this host" */
    if (a[0] >= 224) return 0;                        /* multicast, reserved, broadcast */
    if (a[0] == 169 && a[1] == 254) return 0;         /* link-local, where cloud metadata services live */
    return 1;
}

static int sockaddr_dialable(const struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        unsigned char a[4];
        memcpy(a, &((const struct sockaddr_in *)sa)->sin_addr, 4);
        return v4_dialable(a);
    }
    if (sa->sa_family == AF_INET6) {
        const unsigned char *a = ((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr;
        static const unsigned char zero[16];
        if (!memcmp(a, zero, 16)) return 0;                                  /* :: */
        if (a[0] == 0xff) return 0;                                          /* multicast */
        if (a[0] == 0xfe && (a[1] & 0xC0) == 0x80) return 0;                 /* fe80::/10 */
        static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
        if (!memcmp(a, mapped, 12)) return v4_dialable(a + 12);              /* ::ffff:a.b.c.d */
        return 1;
    }
    return 0;
}

int puddle_net_host_dialable(const char *host)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0)
        return 1; /* not resolvable is not the same as not allowed: dialling will say so */
    int ok = 1;
    for (struct addrinfo *p = res; p; p = p->ai_next)
        if (!sockaddr_dialable(p->ai_addr))
            ok = 0;
    freeaddrinfo(res);
    return ok;
}

int puddle_net_host_plausible(const char *host)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    if (getaddrinfo(host, NULL, &hints, &res) != 0)
        return 1; /* not a numeric address: a name */
    int ok = 1;
    for (struct addrinfo *p = res; p; p = p->ai_next)
        if (!sockaddr_dialable(p->ai_addr))
            ok = 0;
    freeaddrinfo(res);
    return ok;
}

/* ---- connecting ---- */

static int offline_errno(int e)
{
    return e == ECONNREFUSED || e == ETIMEDOUT || e == EHOSTUNREACH || e == ENETUNREACH || e == ENETDOWN ||
           e == EHOSTDOWN || e == EADDRNOTAVAIL || e == ECONNRESET;
}

/* a connected blocking socket, or -1 with *offline and a reason */
static int dial(const puddle_address *addr, int connect_s, int io_s, int64_t deadline, puddle_net_reply *r)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    char port[8];
    snprintf(port, sizeof port, "%u", addr->port);
    int gai = getaddrinfo(addr->host, port, &hints, &res);
    if (gai != 0) {
        why(r, PUDDLE_NET_OFFLINE, "cannot find %s: %s", addr->host, gai_strerror(gai));
        return -1;
    }
    int fd = -1, refused = 0, tried = 0;
    why(r, PUDDLE_NET_OFFLINE, "cannot reach %s", addr->address);
    for (struct addrinfo *p = res; p && fd < 0; p = p->ai_next) {
        if (!sockaddr_dialable(p->ai_addr)) {
            refused = 1;
            continue;
        }
        int s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0)
            continue;
        tried++;
        fcntl(s, F_SETFD, fcntl(s, F_GETFD) | FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
        int rc = connect(s, p->ai_addr, p->ai_addrlen);
        int err = rc == 0 ? 0 : errno;
        if (rc != 0 && err == EINPROGRESS) {
            int64_t left = deadline - mono_ms();
            int wait = (int)(left < connect_s * 1000 ? left : connect_s * 1000);
            struct pollfd pfd = { s, POLLOUT, 0 };
            int pr = wait > 0 ? poll(&pfd, 1, wait) : 0;
            if (pr > 0) {
                socklen_t el = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
            } else {
                err = ETIMEDOUT;
            }
        }
        if (err == 0) {
            fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
            struct timeval tv = { io_s, 0 };
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            fd = s;
        } else {
            close(s);
            if (offline_errno(err))
                why(r, PUDDLE_NET_OFFLINE, "cannot reach %s: %s", addr->address, strerror(err));
            else
                why(r, PUDDLE_NET_FAILED, "cannot connect to %s: %s", addr->address, strerror(err));
        }
    }
    freeaddrinfo(res);
    if (fd < 0 && refused && !tried)
        why(r, PUDDLE_NET_FAILED, "%s is not an address we will connect to", addr->address);
    return fd;
}

/* ---- the call ---- */

static int ssl_send_all(SSL *ssl, const char *p, size_t n)
{
    while (n) {
        int w = SSL_write(ssl, p, (int)n);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

void puddle_net_request(const puddle_address *addr, const char *method, const char *target, const void *body,
                        size_t body_len, puddle_identity *me, const char *peer_id, const puddle_net_opts *opts,
                        puddle_net_reply *out)
{
    memset(out, 0, sizeof *out);
    puddle_net_opts o = opts ? *opts : (puddle_net_opts){ 0 };
    int connect_s = o.connect_timeout_s > 0 ? o.connect_timeout_s : 8;
    int io_s = o.io_timeout_s > 0 ? o.io_timeout_s : 10;
    int total_s = o.total_timeout_s > 0 ? o.total_timeout_s : 30;
    int64_t deadline = mono_ms() + total_s * 1000LL;

    char sig[800];
    if (puddle_reqsig_sign(me, method, target, body, body_len, peer_id, 0, sig, sizeof sig) != 0) {
        why(out, PUDDLE_NET_FAILED, "cannot sign the request");
        return;
    }

    int fd = dial(addr, connect_s, io_s, deadline, out);
    if (fd < 0)
        return;

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL *ssl = NULL;
    char *raw = NULL;
    if (!ctx) {
        why(out, PUDDLE_NET_FAILED, "cannot start TLS");
        goto done;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL); /* see the top of client.h: the signature is what authenticates */
    ssl = SSL_new(ctx);
    if (!ssl || SSL_set_fd(ssl, fd) != 1) {
        why(out, PUDDLE_NET_FAILED, "cannot start TLS");
        goto done;
    }
    if (SSL_connect(ssl) != 1) {
        unsigned long e = ERR_get_error();
        char eb[120] = "";
        if (e)
            ERR_error_string_n(e, eb, sizeof eb);
        why(out, PUDDLE_NET_FAILED, "%s did not complete a TLS handshake%s%s", addr->address, e ? ": " : "", eb);
        goto done;
    }

    X509 *cert = SSL_get1_peer_certificate(ssl);
    if (!cert) {
        why(out, PUDDLE_NET_FAILED, "%s presented no certificate", addr->address);
        goto done;
    }
    unsigned char md[32];
    unsigned mdlen = 0;
    int dok = X509_digest(cert, EVP_sha256(), md, &mdlen) == 1 && mdlen == 32;
    X509_free(cert);
    if (!dok) {
        why(out, PUDDLE_NET_FAILED, "cannot read the certificate");
        goto done;
    }
    puddle_hex_encode(md, 32, out->fingerprint);
    if (o.fingerprint_ok && !o.fingerprint_ok(o.user, out->fingerprint)) {
        why(out, PUDDLE_NET_CERT_CHANGED, "%s has a different certificate from before", addr->address);
        goto done;
    }

    {
        char head[1400];
        int hn = snprintf(head, sizeof head,
                          "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: puddle\r\nX-Puddle-Signature: %s\r\n"
                          "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                          method, target, addr->address, sig, body ? body_len : 0);
        if (hn <= 0 || (size_t)hn >= sizeof head || ssl_send_all(ssl, head, (size_t)hn) != 0 ||
            (body && body_len && ssl_send_all(ssl, body, body_len) != 0)) {
            why(out, PUDDLE_NET_FAILED, "could not send the request to %s", addr->address);
            goto done;
        }
    }

    /* the answer: headers, then a body of the length it promises, or until the other side closes */
    size_t max_body = o.max_body ? o.max_body : PUDDLE_NET_BODY_MAX;
    size_t cap = max_body + 8192, len = 0;
    raw = malloc(cap + 1);
    if (!raw) {
        why(out, PUDDLE_NET_FAILED, "out of memory");
        goto done;
    }
    size_t hend = 0;
    long want = -1;
    for (;;) {
        if (mono_ms() > deadline) {
            why(out, PUDDLE_NET_FAILED, "%s took too long to answer", addr->address);
            goto done;
        }
        if (hend && want >= 0 && len >= hend + (size_t)want)
            break;
        if (len >= cap) {
            why(out, PUDDLE_NET_FAILED, "the answer from %s was too large", addr->address);
            goto done;
        }
        int n = SSL_read(ssl, raw + len, (int)(cap - len));
        if (n <= 0) {
            int se = SSL_get_error(ssl, n);
            if (se == SSL_ERROR_ZERO_RETURN || (se == SSL_ERROR_SYSCALL && n == 0) || (hend && want < 0))
                break; /* closed: that is the end of the body */
            why(out, PUDDLE_NET_FAILED, "%s stopped answering", addr->address);
            goto done;
        }
        len += (size_t)n;
        raw[len] = '\0';
        if (!hend) {
            char *e = strstr(raw, "\r\n\r\n");
            if (e) {
                hend = (size_t)(e - raw) + 4;
                const char *cl = NULL;
                for (const char *l = strchr(raw, '\n'); l && l < raw + hend; l = strchr(l + 1, '\n'))
                    if (!strncasecmp(l + 1, "content-length:", 15))
                        cl = l + 16;
                for (const char *l = strchr(raw, '\n'); l && l < raw + hend; l = strchr(l + 1, '\n'))
                    if (!strncasecmp(l + 1, "content-type:", 13)) {
                        const char *v = l + 14;
                        while (*v == ' ' || *v == '\t')
                            v++;
                        size_t k = 0;
                        while (v < raw + hend && *v != ';' && *v != '\r' && *v != '\n' && *v != ' ' && k + 1 < sizeof out->content_type)
                            out->content_type[k++] = (char)tolower((unsigned char)*v++);
                        out->content_type[k] = '\0';
                    }
                if (cl) {
                    char *endp;
                    long v = strtol(cl, &endp, 10);
                    if (v < 0 || (unsigned long)v > max_body) {
                        why(out, PUDDLE_NET_FAILED, "the answer from %s was too large", addr->address);
                        goto done;
                    }
                    want = v;
                } else if (!strncmp(raw, "HTTP/1.1 204", 12) || !strncmp(raw, "HTTP/1.1 202", 12)) {
                    want = 0;
                }
            }
        }
    }
    raw[len] = '\0';
    int status = 0;
    if (!hend || sscanf(raw, "HTTP/1.%*d %d", &status) != 1 || status < 100 || status > 599) {
        why(out, PUDDLE_NET_FAILED, "%s did not answer with HTTP", addr->address);
        goto done;
    }
    out->result = PUDDLE_NET_OK;
    out->status = status;
    size_t bl = len - hend;
    if (want >= 0 && (size_t)want < bl)
        bl = (size_t)want;
    if (bl > max_body) {
        why(out, PUDDLE_NET_FAILED, "the answer from %s was too large", addr->address);
        goto done;
    }
    out->body = malloc(bl + 1);
    if (out->body) {
        memcpy(out->body, raw + hend, bl);
        out->body[bl] = '\0';
        out->body_len = bl;
    }
done:
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    SSL_CTX_free(ctx);
    free(raw);
    close(fd);
}
