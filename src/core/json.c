#include "json.h"

#include <string.h>

#define MAX_DEPTH 64

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* p at the opening quote; returns just past the closing quote, or NULL */
static const char *scan_string(const char *p, const char *end)
{
    if (p >= end || *p != '"')
        return NULL;
    p++;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c == '"')
            return p + 1;
        if (c < 0x20)
            return NULL;
        if (c == '\\') {
            p++;
            if (p >= end)
                return NULL;
            switch (*p) {
            case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
                p++;
                break;
            case 'u':
                if (end - p < 5)
                    return NULL;
                for (int i = 1; i <= 4; i++)
                    if (hexval(p[i]) < 0)
                        return NULL;
                p += 5;
                break;
            default:
                return NULL;
            }
        } else {
            p++;
        }
    }
    return NULL;
}

static const char *scan_number(const char *p, const char *end)
{
    if (p < end && *p == '-')
        p++;
    if (p >= end)
        return NULL;
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (p < end && is_digit(*p)) p++;
    } else {
        return NULL;
    }
    if (p < end && *p == '.') {
        p++;
        if (p >= end || !is_digit(*p))
            return NULL;
        while (p < end && is_digit(*p)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-'))
            p++;
        if (p >= end || !is_digit(*p))
            return NULL;
        while (p < end && is_digit(*p)) p++;
    }
    return p;
}

static const char *scan_literal(const char *p, const char *end, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(end - p) < n || memcmp(p, lit, n) != 0)
        return NULL;
    return p + n;
}

static const char *scan_value(const char *p, const char *end, int depth)
{
    if (depth > MAX_DEPTH)
        return NULL;
    p = skip_ws(p, end);
    if (p >= end)
        return NULL;

    switch (*p) {
    case '{':
        p = skip_ws(p + 1, end);
        if (p < end && *p == '}')
            return p + 1;
        for (;;) {
            p = scan_string(skip_ws(p, end), end);
            if (!p) return NULL;
            p = skip_ws(p, end);
            if (p >= end || *p != ':') return NULL;
            p = scan_value(p + 1, end, depth + 1);
            if (!p) return NULL;
            p = skip_ws(p, end);
            if (p >= end) return NULL;
            if (*p == ',') { p++; continue; }
            if (*p == '}') return p + 1;
            return NULL;
        }
    case '[':
        p = skip_ws(p + 1, end);
        if (p < end && *p == ']')
            return p + 1;
        for (;;) {
            p = scan_value(p, end, depth + 1);
            if (!p) return NULL;
            p = skip_ws(p, end);
            if (p >= end) return NULL;
            if (*p == ',') { p++; continue; }
            if (*p == ']') return p + 1;
            return NULL;
        }
    case '"': return scan_string(p, end);
    case 't': return scan_literal(p, end, "true");
    case 'f': return scan_literal(p, end, "false");
    case 'n': return scan_literal(p, end, "null");
    default:  return scan_number(p, end);
    }
}

int puddle_json_valid_object(const char *json, size_t len)
{
    const char *end = json + len;
    const char *p = skip_ws(json, end);
    if (p >= end || *p != '{')
        return 0;
    p = scan_value(p, end, 0);
    return p && skip_ws(p, end) == end;
}

int puddle_json_string(puddle_span v, char *out, size_t cap)
{
    if (v.len < 2 || v.p[0] != '"' || v.p[v.len - 1] != '"' || cap == 0)
        return -1;

    const char *p = v.p + 1, *end = v.p + v.len - 1;
    size_t o = 0;
#define PUT(ch) do { if (o + 1 >= cap) return -1; out[o++] = (char)(ch); } while (0)

    while (p < end) {
        if (*p != '\\') {
            PUT(*p++);
            continue;
        }
        p++;
        switch (*p) {
        case '"': PUT('"'); p++; break;
        case '\\': PUT('\\'); p++; break;
        case '/': PUT('/'); p++; break;
        case 'b': PUT('\b'); p++; break;
        case 'f': PUT('\f'); p++; break;
        case 'n': PUT('\n'); p++; break;
        case 'r': PUT('\r'); p++; break;
        case 't': PUT('\t'); p++; break;
        case 'u': {
            if (end - p < 5) return -1;
            unsigned cp = 0;
            for (int i = 1; i <= 4; i++) {
                int h = hexval(p[i]);
                if (h < 0) return -1;
                cp = cp << 4 | (unsigned)h;
            }
            p += 5;
            if (cp >= 0xD800 && cp <= 0xDBFF) { /* needs a following low surrogate */
                if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return -1;
                unsigned lo = 0;
                for (int i = 2; i <= 5; i++) {
                    int h = hexval(p[i]);
                    if (h < 0) return -1;
                    lo = lo << 4 | (unsigned)h;
                }
                if (lo < 0xDC00 || lo > 0xDFFF) return -1;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return -1;
            }
            if (cp == 0)
                return -1; /* would truncate the C string */
            if (cp < 0x80) {
                PUT(cp);
            } else if (cp < 0x800) {
                PUT(0xC0 | (cp >> 6)); PUT(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                PUT(0xE0 | (cp >> 12)); PUT(0x80 | ((cp >> 6) & 0x3F)); PUT(0x80 | (cp & 0x3F));
            } else {
                PUT(0xF0 | (cp >> 18)); PUT(0x80 | ((cp >> 12) & 0x3F));
                PUT(0x80 | ((cp >> 6) & 0x3F)); PUT(0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            return -1;
        }
    }
#undef PUT
    out[o] = '\0';
    return 0;
}

int puddle_json_int64(puddle_span v, int64_t *out)
{
    size_t i = 0;
    int neg = 0;
    if (v.len == 0)
        return -1;
    if (v.p[0] == '-') {
        neg = 1;
        i++;
    }
    if (i >= v.len)
        return -1;
    if (v.p[i] == '0' && v.len - i > 1)
        return -1;
    int64_t acc = 0;
    for (; i < v.len; i++) {
        if (!is_digit(v.p[i]))
            return -1;
        int d = v.p[i] - '0';
        if (acc > (INT64_MAX - d) / 10)
            return -1;
        acc = acc * 10 + d;
    }
    *out = neg ? -acc : acc;
    return 0;
}

static int key_equals(const char *ks, const char *ke, const char *key)
{
    char buf[128];
    puddle_span s = { ks, (size_t)(ke - ks) };
    return puddle_json_string(s, buf, sizeof buf) == 0 && strcmp(buf, key) == 0;
}

int puddle_json_member(const char *json, size_t len, const char *key, puddle_span *val)
{
    const char *end = json + len;
    const char *p = skip_ws(json, end);
    int found = 0;
    puddle_span result = { NULL, 0 };

    if (p >= end || *p != '{')
        return -1;
    p = skip_ws(p + 1, end);
    if (p < end && *p == '}') {
        p++;
    } else {
        for (;;) {
            const char *ks = skip_ws(p, end);
            const char *ke = scan_string(ks, end);
            if (!ke) return -1;
            p = skip_ws(ke, end);
            if (p >= end || *p != ':') return -1;
            const char *vs = skip_ws(p + 1, end);
            const char *ve = scan_value(vs, end, 1);
            if (!ve) return -1;
            if (key_equals(ks, ke, key)) {
                if (found) return -1; /* duplicate key */
                found = 1;
                result.p = vs;
                result.len = (size_t)(ve - vs);
            }
            p = skip_ws(ve, end);
            if (p >= end) return -1;
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; break; }
            return -1;
        }
    }
    if (skip_ws(p, end) != end)
        return -1;
    if (!found)
        return 1;
    *val = result;
    return 0;
}
