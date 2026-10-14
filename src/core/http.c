#include "http.h"

#include <stdio.h>
#include <string.h>

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 431: return "Request Header Fields Too Large";
    default:  return "Internal Server Error";
    }
}

static size_t respond(char *out, size_t cap, int status, const char *extra_headers,
                      const char *body, int head_only)
{
    size_t blen = strlen(body);
    int n = snprintf(out, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n"
                     "%s",
                     status, reason(status), blen, extra_headers, head_only ? "" : body);
    if (n < 0 || (size_t)n >= cap)
        return 0;
    return (size_t)n;
}

size_t puddle_http_error(int status, char *out, size_t cap)
{
    char body[96];
    snprintf(body, sizeof body, "{\"error\":\"%s\"}\n", reason(status));
    return respond(out, cap, status, "", body, 0);
}

int puddle_http_parse(const char *buf, size_t len, puddle_request *req)
{
    const char *eol = memchr(buf, '\n', len);
    if (!eol)
        return -1;
    size_t n = (size_t)(eol - buf);
    if (n && buf[n - 1] == '\r')
        n--;

    char line[1100];
    if (n >= sizeof line)
        return -1;
    memcpy(line, buf, n);
    line[n] = '\0';

    char method[8], target[1024], version[16];
    if (sscanf(line, "%7s %1023s %15s", method, target, version) != 3)
        return -1;
    if (strncmp(version, "HTTP/1.", 7) != 0)
        return -1;

    char *q = strpbrk(target, "?#");
    if (q)
        *q = '\0';

    memcpy(req->method, method, sizeof req->method);
    memcpy(req->path, target, sizeof req->path);
    return 0;
}

size_t puddle_http_route(const puddle_request *req, char *out, size_t cap, int *status)
{
    if (!req) {
        *status = 400;
        return puddle_http_error(400, out, cap);
    }

    if (strcmp(req->path, "/health") == 0) {
        int is_get = strcmp(req->method, "GET") == 0;
        int is_head = strcmp(req->method, "HEAD") == 0;
        if (is_get || is_head) {
            *status = 200;
            return respond(out, cap, 200, "", "{\"status\":\"ok\"}\n", is_head);
        }
        *status = 405;
        return respond(out, cap, 405, "Allow: GET, HEAD\r\n", "{\"error\":\"Method Not Allowed\"}\n", 0);
    }

    *status = 404;
    return puddle_http_error(404, out, cap);
}
