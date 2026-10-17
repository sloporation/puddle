#include "wireprofile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define H1 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define H2 "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"

static int clean(const char *in, char *out, size_t cap) { return puddle_profile_wire_clean(in, strlen(in), out, cap); }

int main(void)
{
    char out[PUDDLE_WIRE_PROFILE_MAX + 8];

    /* the whole thing, in one spelling */
    CHECK(clean("{\"pronouns\":\"they/them\",\"display_name\":\"  Ann\\u202e  \",\"unknown\":{\"x\":[1,2]},\"about\":\"line1\\nline2\","
                "\"blurb\":\"hi\",\"color\":{\"angle\":45,\"colors\":[\"#AABBCC\",\"#001122\"]},"
                "\"background\":{\"size\":3,\"mime\":\"image/webp\",\"hash\":\"" H2 "\"},"
                "\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":1234},\"banner\":{\"hash\":\"" H1 "\",\"mime\":\"image/gif\",\"size\":9}}",
                out, sizeof out) == 0);
    CHECK(!strcmp(out, "{\"display_name\":\"Ann\",\"blurb\":\"hi\",\"about\":\"line1\\nline2\",\"pronouns\":\"they/them\","
                       "\"color\":{\"colors\":[\"#aabbcc\",\"#001122\"],\"angle\":45},"
                       "\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":1234},"
                       "\"banner\":{\"hash\":\"" H1 "\",\"mime\":\"image/gif\",\"size\":9},"
                       "\"background\":{\"hash\":\"" H2 "\",\"mime\":\"image/webp\",\"size\":3}}"));
    /* what it comes to is valid, and cleaning it again changes nothing */
    {
        char again[PUDDLE_WIRE_PROFILE_MAX + 8];
        CHECK(clean(out, again, sizeof again) == 0 && !strcmp(again, out));
    }
    CHECK(clean("{\"display_name\":\"Just a name\"}", out, sizeof out) == 0 && !strcmp(out, "{\"display_name\":\"Just a name\"}"));
    CHECK(clean("{\"display_name\":\"x\",\"color\":{\"colors\":[\"#ABCDEF\"],\"angle\":90}}", out, sizeof out) == 0 &&
          !strcmp(out, "{\"display_name\":\"x\",\"color\":{\"colors\":[\"#abcdef\"]}}"));           /* one color has no direction */
    CHECK(clean("{\"display_name\":\"x\",\"color\":{\"colors\":[\"#000000\",\"#111111\",\"#222222\",\"#333333\",\"#444444\"],\"angle\":0}}", out, sizeof out) == 0 &&
          strstr(out, "\"angle\":0"));
    CHECK(clean("{\"display_name\":\"x\",\"color\":{\"colors\":[\"#000000\",\"#111111\"],\"angle\":360}}", out, sizeof out) == 0 && strstr(out, "\"angle\":360"));
    CHECK(clean("{\"display_name\":\"x\",\"color\":{\"colors\":[\"#000000\",\"#111111\"]}}", out, sizeof out) == 0 && !strstr(out, "angle"));
    CHECK(clean("{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":26214400}}", out, sizeof out) == 0);   /* exactly the cap */

    /* what makes a profile unacceptable, whole */
    const char *bad[] = {
        "", "[]", "{", "{}", "{\"display_name\":\"\"}", "{\"display_name\":\"   \"}", "{\"display_name\":\"\\u202e\"}", "{\"display_name\":7}",
        "{\"display_name\":\"x\",\"blurb\":7}", "{\"display_name\":\"x\",\"about\":[]}", "{\"display_name\":\"x\",\"pronouns\":{}}",
        "{\"display_name\":\"x\",\"display_name\":\"y\"}",                                                                       /* a repeated key is ambiguous */
        "{\"display_name\":\"x\",\"color\":7}", "{\"display_name\":\"x\",\"color\":{}}", "{\"display_name\":\"x\",\"color\":{\"colors\":[]}}",
        "{\"display_name\":\"x\",\"color\":{\"colors\":\"#000000\"}}", "{\"display_name\":\"x\",\"color\":{\"colors\":[7]}}",
        "{\"display_name\":\"x\",\"color\":{\"colors\":[\"red\"]}}", "{\"display_name\":\"x\",\"color\":{\"colors\":[\"#00000\"]}}",
        "{\"display_name\":\"x\",\"color\":{\"colors\":[\"#0000000\"]}}", "{\"display_name\":\"x\",\"color\":{\"colors\":[\"000000\"]}}", "{\"display_name\":\"x\",\"color\":{\"colors\":[\"x000000\"]}}",
        "{\"display_name\":\"x\",\"color\":{\"colors\":[\"#00000g\"]}}",
        "{\"display_name\":\"x\",\"color\":{\"colors\":[\"#000000\",\"#111111\",\"#222222\",\"#333333\",\"#444444\",\"#555555\"]}}",
        "{\"display_name\":\"x\",\"color\":{\"colors\":[\"#000000\"],\"angle\":-1}}", "{\"display_name\":\"x\",\"color\":{\"colors\":[\"#000000\"],\"angle\":361}}",
        "{\"display_name\":\"x\",\"color\":{\"colors\":[\"#000000\"],\"angle\":\"90\"}}", "{\"display_name\":\"x\",\"color\":{\"colors\":[\"#000000\"],\"angle\":1.5}}",
        "{\"display_name\":\"x\",\"avatar\":7}", "{\"display_name\":\"x\",\"avatar\":{}}",
        "{\"display_name\":\"x\",\"avatar\":{\"mime\":\"image/png\",\"size\":5}}",
        "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"size\":5}}", "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\"}}",
        "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"zz\",\"mime\":\"image/png\",\"size\":5}}",
        "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\",\"mime\":\"image/png\",\"size\":5}}",   /* one spelling */
        "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde\",\"mime\":\"image/png\",\"size\":5}}",
        "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/svg+xml\",\"size\":5}}",
        "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"text/html\",\"size\":5}}",
        "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":0}}",
        "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":-1}}",
        "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":26214401}}",
        "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":\"5\"}}",
        "{\"display_name\":\"x\",\"banner\":\"" H1 "\"}", "{\"display_name\":\"x\",\"background\":null}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        if (clean(bad[i], out, sizeof out) == 0) { fprintf(stderr, "FAIL accepted: %s\n", bad[i]); failures++; }

    /* limits of the text */
    {
        char big[40000], j[41000];
        memset(big, 'n', 64);
        big[64] = 0;
        snprintf(j, sizeof j, "{\"display_name\":\"%s\"}", big);
        CHECK(clean(j, out, sizeof out) == 0);
        memset(big, 'n', 65);
        big[65] = 0;
        snprintf(j, sizeof j, "{\"display_name\":\"%s\"}", big);
        CHECK(clean(j, out, sizeof out) != 0);
        for (int i = 0; i < 2000; i++) memcpy(big + i * 4, "\xF0\x9F\x99\x82", 4);                                         /* the longest an `about` can be */
        big[8000] = 0;
        snprintf(j, sizeof j, "{\"display_name\":\"x\",\"about\":\"%s\"}", big);
        CHECK(clean(j, out, sizeof out) == 0 && strlen(out) < PUDDLE_WIRE_PROFILE_MAX);
        memcpy(big + 8000, "\xF0\x9F\x99\x82", 4);
        big[8004] = 0;
        snprintf(j, sizeof j, "{\"display_name\":\"x\",\"about\":\"%s\"}", big);
        CHECK(clean(j, out, sizeof out) != 0);                                                                          /* 2001 characters */
        CHECK(clean("{\"display_name\":\"x\"}", out, 10) != 0);                                                          /* does not fit */
    }

    /* the pictures a profile points at */
    {
        puddle_media_ref refs[3];
        CHECK(puddle_profile_wire_media("{\"display_name\":\"x\"}", 20, refs) == 0);
        const char *p = "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":7},"
                        "\"background\":{\"hash\":\"" H2 "\",\"mime\":\"image/jpeg\",\"size\":8}}";
        CHECK(puddle_profile_wire_media(p, strlen(p), refs) == 2);
        CHECK(!strcmp(refs[0].hash, H1) && !strcmp(refs[0].mime, "image/png") && refs[0].size == 7 && !strcmp(refs[1].hash, H2) && refs[1].size == 8);
        p = "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"bad\",\"mime\":\"image/png\",\"size\":7}}";
        CHECK(puddle_profile_wire_media(p, strlen(p), refs) == 0);                                                      /* a bad one is no reference */
    }
    return failures ? 1 : 0;
}
