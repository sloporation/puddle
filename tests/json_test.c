#include "json.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int member(const char *json, const char *key, puddle_span *v)
{
    return puddle_json_member(json, strlen(json), key, v);
}

static int valid(const char *json) { return puddle_json_valid_object(json, strlen(json)); }

int main(void)
{
    puddle_span v;
    char buf[64];
    int64_t n;

    /* validity */
    CHECK(valid("{}"));
    CHECK(valid(" {\"a\":[1,2.5e3,-0,true,false,null,{\"b\":\"c\"}]} "));
    CHECK(!valid("[]"));
    CHECK(!valid("{\"a\":1,}"));
    CHECK(!valid("{\"a\":01}"));
    CHECK(!valid("{\"a\":\"x}"));
    CHECK(!valid("{\"a\":\"\\q\"}"));
    CHECK(!valid("{\"a\":1} x"));
    CHECK(!valid("{\"a\":\"line\nbreak\"}")); /* raw control char in string */

    /* depth limit */
    char deep[400];
    int d = 0;
    for (int i = 0; i < 100; i++) deep[d++] = '[';
    for (int i = 0; i < 100; i++) deep[d++] = ']';
    char wrapped[500];
    snprintf(wrapped, sizeof wrapped, "{\"a\":%.*s}", d, deep);
    CHECK(!puddle_json_valid_object(wrapped, strlen(wrapped)));

    /* member lookup returns raw bytes and only matches top-level keys */
    const char *ev = "{\"body\":\"\\\"ts\\\":1 and \\\"chat\\\":\\\"x\\\"\",\"inner\":{\"ts\":5},\"ts\":1760000000000,\"chat\":\"c1\"}";
    CHECK(member(ev, "ts", &v) == 0);
    CHECK(puddle_json_int64(v, &n) == 0 && n == 1760000000000LL);
    CHECK(member(ev, "chat", &v) == 0);
    CHECK(puddle_json_string(v, buf, sizeof buf) == 0 && strcmp(buf, "c1") == 0);
    CHECK(member(ev, "inner", &v) == 0 && v.len == 8 && memcmp(v.p, "{\"ts\":5}", 8) == 0);
    CHECK(member(ev, "missing", &v) == 1);

    /* duplicate keys are rejected, not resolved */
    CHECK(member("{\"ts\":1,\"ts\":2}", "ts", &v) == -1);
    CHECK(member("{\"ts\":1", "ts", &v) == -1);

    /* escaped keys */
    CHECK(member("{\"t\\u0073\":7}", "ts", &v) == 0);

    /* strings */
    CHECK(member("{\"s\":\"a\\n\\u00e9\\ud83d\\ude00\"}", "s", &v) == 0);
    CHECK(puddle_json_string(v, buf, sizeof buf) == 0);
    CHECK(strcmp(buf, "a\n\xc3\xa9\xf0\x9f\x98\x80") == 0);
    CHECK(member("{\"s\":\"\\ud83d\"}", "s", &v) == 0 && puddle_json_string(v, buf, sizeof buf) == -1); /* lone surrogate */
    CHECK(member("{\"s\":\"\\u0000\"}", "s", &v) == 0 && puddle_json_string(v, buf, sizeof buf) == -1);
    CHECK(member("{\"s\":\"toolongvalue\"}", "s", &v) == 0 && puddle_json_string(v, buf, 5) == -1);
    CHECK(member("{\"s\":5}", "s", &v) == 0 && puddle_json_string(v, buf, sizeof buf) == -1);

    /* integers */
    CHECK(member("{\"n\":-42}", "n", &v) == 0 && puddle_json_int64(v, &n) == 0 && n == -42);
    CHECK(member("{\"n\":9223372036854775807}", "n", &v) == 0 && puddle_json_int64(v, &n) == 0 && n == INT64_MAX);
    CHECK(member("{\"n\":9223372036854775808}", "n", &v) == 0 && puddle_json_int64(v, &n) == -1);
    CHECK(member("{\"n\":1.5}", "n", &v) == 0 && puddle_json_int64(v, &n) == -1);
    CHECK(member("{\"n\":1e3}", "n", &v) == 0 && puddle_json_int64(v, &n) == -1);
    CHECK(member("{\"n\":\"7\"}", "n", &v) == 0 && puddle_json_int64(v, &n) == -1);

    return failures ? 1 : 0;
}
