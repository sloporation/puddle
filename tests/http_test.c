#include "http.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int status_of(const char *raw, int owner_ok)
{
    puddle_request req;
    char out[2048];
    int status = 0;
    int ok = puddle_http_parse(raw, strlen(raw), &req) == 0;
    puddle_http_route(ok ? &req : NULL, owner_ok, out, sizeof out, &status);
    return status;
}

static int parse(const char *raw, puddle_request *req) { return puddle_http_parse(raw, strlen(raw), req); }

int main(void)
{
    /* /health needs the owner token; without it, it is exactly a path that doesn't exist */
    CHECK(status_of("GET /health HTTP/1.1\r\nHost: x\r\n\r\n", 1) == 200);
    CHECK(status_of("GET /health?verbose=1 HTTP/1.1\r\n\r\n", 1) == 200);
    CHECK(status_of("HEAD /health HTTP/1.1\r\n\r\n", 1) == 200);
    CHECK(status_of("POST /health HTTP/1.1\r\n\r\n", 1) == 405);
    CHECK(status_of("GET /health HTTP/1.1\r\n\r\n", 0) == 404);
    CHECK(status_of("HEAD /health HTTP/1.1\r\n\r\n", 0) == 404);
    CHECK(status_of("POST /health HTTP/1.1\r\n\r\n", 0) == 404); /* not even a 405 to a stranger */
    CHECK(status_of("GET /nope HTTP/1.1\r\n\r\n", 1) == 404);
    CHECK(status_of("GET /nope HTTP/1.1\r\n\r\n", 0) == 404);
    CHECK(status_of("garbage\r\n\r\n", 1) == 400);
    CHECK(status_of("GET /health SPDY/3\r\n\r\n", 1) == 400);

    /* an unauthorized answer is byte-for-byte the same as for a path that isn't there */
    puddle_request a, b;
    char out1[2048], out2[2048];
    int s1, s2;
    parse("GET /health HTTP/1.1\r\n\r\n", &a);
    parse("GET /nothing-here HTTP/1.1\r\n\r\n", &b);
    size_t n1 = puddle_http_route(&a, 0, out1, sizeof out1, &s1);
    size_t n2 = puddle_http_route(&b, 0, out2, sizeof out2, &s2);
    CHECK(n1 == n2 && memcmp(out1, out2, n1) == 0 && s1 == 404 && s2 == 404);

    puddle_request req;
    int status;
    char out[2048];
    CHECK(parse("GET /health HTTP/1.1\r\n\r\n", &req) == 0);
    puddle_http_route(&req, 1, out, sizeof out, &status);
    CHECK(strstr(out, "200 OK") != NULL && strstr(out, "{\"status\":\"ok\"}") != NULL);
    strcpy(req.method, "HEAD");
    puddle_http_route(&req, 1, out, sizeof out, &status);
    CHECK(strstr(out, "{\"status\"") == NULL && strstr(out, "Content-Length: 16") != NULL);

    /* the two headers we read */
    CHECK(parse("GET /a/b?x=1&y=2 HTTP/1.1\r\nHost: h\r\nAuthorization: Bearer abc\r\nx-puddle-signature:  v1 id=z  \r\n\r\n", &req) == 0);
    CHECK(strcmp(req.path, "/a/b") == 0 && strcmp(req.target, "/a/b?x=1&y=2") == 0);
    CHECK(strcmp(req.authorization, "Bearer abc") == 0); /* name matching ignores case; the value is trimmed */
    CHECK(strcmp(req.signature, "v1 id=z") == 0);
    CHECK(parse("GET /a#frag HTTP/1.1\r\n\r\n", &req) == 0 && strcmp(req.target, "/a") == 0);
    CHECK(parse("GET / HTTP/1.1\r\n\r\n", &req) == 0 && req.authorization[0] == '\0' && req.signature[0] == '\0');

    /* things that could make two parsers disagree are refused */
    CHECK(parse("GET / HTTP/1.1\r\nAuthorization: a\r\nAuthorization: b\r\n\r\n", &req) == -1);
    CHECK(parse("GET / HTTP/1.1\r\nX-Puddle-Signature: a\r\nx-puddle-signature: b\r\n\r\n", &req) == -1);
    CHECK(parse("GET / HTTP/1.1\r\nAuthorization: a\r\n  folded\r\n\r\n", &req) == -1);
    CHECK(parse("GET / HTTP/1.1\r\nno colon here\r\n\r\n", &req) == -1);
    char big[1500];
    memset(big, 'x', 700);
    big[700] = '\0';
    char raw[1800];
    snprintf(raw, sizeof raw, "GET / HTTP/1.1\r\nAuthorization: %s\r\n\r\n", big);
    CHECK(parse(raw, &req) == -1); /* longer than we keep */
    snprintf(raw, sizeof raw, "GET / HTTP/1.1\r\nX-Other: %s\r\n\r\n", big); /* other headers may be long */
    CHECK(parse(raw, &req) == 0);

    return failures ? 1 : 0;
}
