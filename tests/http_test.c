#include "http.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int status_of(const char *raw)
{
    puddle_request req;
    char out[2048];
    int status = 0;
    int ok = puddle_http_parse(raw, strlen(raw), &req) == 0;
    puddle_http_route(ok ? &req : NULL, out, sizeof out, &status);
    return status;
}

int main(void)
{
    CHECK(status_of("GET /health HTTP/1.1\r\nHost: x\r\n\r\n") == 200);
    CHECK(status_of("GET /health?verbose=1 HTTP/1.1\r\n\r\n") == 200);
    CHECK(status_of("HEAD /health HTTP/1.1\r\n\r\n") == 200);
    CHECK(status_of("POST /health HTTP/1.1\r\n\r\n") == 405);
    CHECK(status_of("GET /nope HTTP/1.1\r\n\r\n") == 404);
    CHECK(status_of("garbage\r\n\r\n") == 400);
    CHECK(status_of("GET /health SPDY/3\r\n\r\n") == 400);

    puddle_request req;
    char out[2048];
    int status;
    const char *raw = "GET /health HTTP/1.1\r\n\r\n";
    CHECK(puddle_http_parse(raw, strlen(raw), &req) == 0);
    puddle_http_route(&req, out, sizeof out, &status);
    CHECK(strstr(out, "200 OK") != NULL);
    CHECK(strstr(out, "{\"status\":\"ok\"}") != NULL);

    puddle_http_route(&req, out, sizeof out, &status); /* HEAD: no body */
    strcpy(req.method, "HEAD");
    puddle_http_route(&req, out, sizeof out, &status);
    CHECK(strstr(out, "{\"status\"") == NULL);
    CHECK(strstr(out, "Content-Length: 16") != NULL);

    return failures ? 1 : 0;
}
