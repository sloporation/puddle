#include "link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define ID "omhh6dd2fusc2douu4rdqigbshrjdz2zprtyxdf3j7igten6pmrq"
#define TOKEN "abcdefghijklmnopqrstuvwxy2"

static int parse(const char *t, puddle_link *l) { char e[100]; return puddle_link_parse(t, l, e, sizeof e); }
static int addr(const char *t, puddle_address *a) { return puddle_address_parse(t, a); }

int main(void)
{
    puddle_link l;
    puddle_address a;

    /* ids */
    CHECK(puddle_id_valid(ID));
    CHECK(puddle_id_valid("n2uaokv4vemetktypypeiykt2loiodxpi5wyabccwfaorgvgxgya"));
    CHECK(!puddle_id_valid(NULL) && !puddle_id_valid("") && !puddle_id_valid("abc"));
    CHECK(!puddle_id_valid("omhh6dd2fusc2douu4rdqigbshrjdz2zprtyxdf3j7igten6pmrb")); /* a second spelling of an id */
    CHECK(!puddle_id_valid("OMHH6dd2fusc2douu4rdqigbshrjdz2zprtyxdf3j7igten6pmrq")); /* upper case */
    CHECK(!puddle_id_valid("omhh6dd2fusc2douu4rdqigbshrjdz2zprtyxdf3j7igten6pmr1")); /* 1 is not base32 */
    CHECK(!puddle_id_valid(ID "q"));
    CHECK(!puddle_id_valid("../../etc/passwd/../../xxxxxxxxxxxxxxxxxxxxxxxxxxxxq"));

    /* links */
    CHECK(parse("puddle://" ID "@203.0.113.7:8443", &l) == 0);
    CHECK(!strcmp(l.id, ID) && !strcmp(l.address.host, "203.0.113.7") && l.address.port == 8443 &&
          !strcmp(l.address.address, "203.0.113.7:8443") && l.invite[0] == '\0');
    CHECK(parse("puddle://" ID "@[2001:db8::1]:9000", &l) == 0 && !strcmp(l.address.host, "2001:db8::1") &&
          l.address.port == 9000 && !strcmp(l.address.address, "[2001:db8::1]:9000"));
    CHECK(parse("puddle://" ID "@Home.Example.ORG:1", &l) == 0 && !strcmp(l.address.address, "home.example.org:1"));
    CHECK(parse("puddle://" ID "@home.example.org", &l) == 0 && l.address.port == 8443); /* port defaults */
    CHECK(parse("puddle://" ID "@[::1]", &l) == 0 && !strcmp(l.address.address, "[::1]:8443"));
    CHECK(parse("puddle://" ID "@10.0.0.1:8443?invite=" TOKEN, &l) == 0 && !strcmp(l.invite, TOKEN));
    CHECK(parse("puddle://" ID "@10.0.0.1:8443/", &l) == 0);
    CHECK(parse("puddle://" ID "@10.0.0.1:8443/?invite=" TOKEN, &l) != 0); /* a path before the query: not ours */
    CHECK(parse("puddle://" ID "@[::1]:1?invite=" TOKEN, &l) == 0);

    const char *bad[] = {
        "", "puddle://", "puddle:", "https://" ID "@1.2.3.4:1", "PUDDLE://" ID "@1.2.3.4:1", " puddle://" ID "@1.2.3.4:1",
        "puddle://" ID "@1.2.3.4:1 ", "puddle://" ID "@1.2.3.4:1\n", "puddle://" ID "@1.2.3.4:1\t",
        "puddle://" ID, "puddle://" ID "@", "puddle://@1.2.3.4:1", "puddle://abc@1.2.3.4:1",
        "puddle://" ID "x@1.2.3.4:1", "puddle://omhh6dd2fusc2douu4rdqigbshrjdz2zprtyxdf3j7igten6pmrb@1.2.3.4:1",
        "puddle://" ID "@1.2.3.4:1@5.6.7.8:1", "puddle://" ID "@evil@1.2.3.4:1",
        "puddle://" ID "@1.2.3.4:1/path", "puddle://" ID "@1.2.3.4:1#frag", "puddle://" ID "@1.2.3.4:1?x=1",
        "puddle://" ID "@1.2.3.4:1?invite=", "puddle://" ID "@1.2.3.4:1?invite=short",
        "puddle://" ID "@1.2.3.4:1?invite=" TOKEN "x", "puddle://" ID "@1.2.3.4:1?invite=ABCDEFGHIJKLMNOPQRSTUVWXY2",
        "puddle://" ID "@1.2.3.4:1?invite=" TOKEN "&invite=" TOKEN, "puddle://" ID "@1.2.3.4:1?invite=" TOKEN "&x=1",
        "puddle://" ID "@1.2.3.4:0", "puddle://" ID "@1.2.3.4:65536", "puddle://" ID "@1.2.3.4:-1",
        "puddle://" ID "@1.2.3.4:", "puddle://" ID "@1.2.3.4:80a", "puddle://" ID "@1.2.3.4:123456",
        "puddle://" ID "@1.2.3.4:+80", "puddle://" ID "@1.2.3.4:8 0",
        "puddle://" ID "@999.1.1.1:1", "puddle://" ID "@256.1.1.1:1", "puddle://" ID "@1.2.3.256:1",
        "puddle://" ID "@1.2.3.4:1?abcdef=" TOKEN, "puddle://" ID "@1.2.3.4:000080", "puddle://" ID "@1.2.3:1", "puddle://" ID "@1.2.3.4.5:1", "puddle://" ID "@1.2.3.:1",
        "puddle://" ID "@01234.1.1.1:1",
        "puddle://" ID "@::1:8443", "puddle://" ID "@[::1:8443", "puddle://" ID "@::1]:8443", "puddle://" ID "@[]:1",
        "puddle://" ID "@[::1]x", "puddle://" ID "@[::1]:", "puddle://" ID "@[g::1]:1", "puddle://" ID "@[1:2:3]:1",
        "puddle://" ID "@[1::2::3]:1", "puddle://" ID "@[:1]:1", "puddle://" ID "@[1:]:1", "puddle://" ID "@[1:::2]:1",
        "puddle://" ID "@[12345::]:1", "puddle://" ID "@[1:2:3:4:5:6:7:8:9]:1", "puddle://" ID "@[1:2:3:4:5:6:7:8::]:1",
        "puddle://" ID "@[::ffff:1.2.3.4]:1", "puddle://" ID "@[fe80::1%eth0]:1",
        "puddle://" ID "@-a.example:1", "puddle://" ID "@a-.example:1", "puddle://" ID "@a..example:1",
        "puddle://" ID "@.example:1", "puddle://" ID "@exa_mple.org:1", "puddle://" ID "@exa mple.org:1",
        "puddle://" ID "@\xC3\xA9xample.org:1", "puddle://" ID "@a.example.:1x",
        "puddle://" ID "@localhost:8443:9", "puddle://" ID "@[::1]:8443:9",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (parse(bad[i], &l) == 0) {
            fprintf(stderr, "FAIL accepted: %s\n", bad[i]);
            failures++;
        }
    }
    char e[100] = "";
    CHECK(puddle_link_parse("nope", &l, e, sizeof e) == -1 && e[0]);
    CHECK(puddle_link_parse(NULL, &l, e, sizeof e) == -1);
    CHECK(puddle_link_parse("puddle://" ID "@1.2.3.4:1", &l, NULL, 0) == 0);

    /* an over-long hostname label or name */
    char host[400], text[600];
    memset(host, 'a', 64); host[64] = 0;
    snprintf(text, sizeof text, "puddle://" ID "@%s.example:1", host);
    CHECK(parse(text, &l) != 0);
    memset(host, 'a', 63); host[63] = 0;
    snprintf(text, sizeof text, "puddle://" ID "@%s.example:1", host);
    CHECK(parse(text, &l) == 0);
    memset(host, 'a', 300); host[300] = 0;
    snprintf(text, sizeof text, "puddle://" ID "@%s:1", host);
    CHECK(parse(text, &l) != 0);

    /* addresses on their own */
    CHECK(addr("1.2.3.4:5", &a) == 0 && a.port == 5 && !strcmp(a.address, "1.2.3.4:5"));
    CHECK(addr("fe80::1", &a) != 0 && addr("[fe80::1]", &a) == 0 && !strcmp(a.address, "[fe80::1]:8443"));
    CHECK(addr("[::]:1", &a) == 0 && addr("[::ABCD]:1", &a) == 0 && !strcmp(a.host, "::abcd")); /* case folded */
    CHECK(addr("", &a) != 0 && addr(NULL, &a) != 0 && addr("a b", &a) != 0);
    CHECK(addr("a:1", &a) == 0 && addr("A:1", &a) == 0 && !strcmp(a.host, "a"));
    CHECK(addr("1-2.example:1", &a) == 0);

    /* tokens */
    CHECK(puddle_invite_token_valid(TOKEN) && !puddle_invite_token_valid("") && !puddle_invite_token_valid(NULL));
    CHECK(!puddle_invite_token_valid("abcdefghijklmnopqrstuvwxy")); /* one short */
    CHECK(!puddle_invite_token_valid("abcdefghijklmnopqrstuvwxy1")); /* 1 is not base32 */
    CHECK(!puddle_invite_token_valid("ABCDEFGHIJKLMNOPQRSTUVWXY2"));

    /* formatting, and back */
    char out[300];
    CHECK(puddle_link_format(ID, "[::1]:8443", NULL, out, sizeof out) == 0 && !strcmp(out, "puddle://" ID "@[::1]:8443"));
    CHECK(puddle_link_format(ID, "1.2.3.4:5", TOKEN, out, sizeof out) == 0 && parse(out, &l) == 0 && !strcmp(l.invite, TOKEN));
    CHECK(puddle_link_format(ID, "1.2.3.4:5", "", out, sizeof out) == 0 && !strchr(out, '?'));
    CHECK(puddle_link_format(ID, "1.2.3.4:5", NULL, out, 20) == -1);

    /* nothing random crashes, and what is accepted is always the canonical thing */
    srand(11);
    const char alphabet[] = "puddle:/@[]?=&#%.-_ 09afAF:xyz\xC3\x01";
    for (int i = 0; i < 40000; i++) {
        char t[200];
        size_t n = (size_t)(rand() % 120);
        for (size_t k = 0; k < n; k++)
            t[k] = rand() % 3 ? alphabet[rand() % (sizeof alphabet - 1)] : (char)(rand() % 256 | 1);
        t[n] = 0;
        char buf[300];
        if (rand() % 2) { /* sometimes start from something well-formed */
            snprintf(buf, sizeof buf, "puddle://" ID "@%s", t);
            if (parse(buf, &l) == 0) {
                char again[300];
                CHECK(puddle_link_format(l.id, l.address.address, l.invite, again, sizeof again) == 0 && parse(again, &l) == 0);
            }
        } else {
            (void)parse(t, &l);
            (void)addr(t, &a);
        }
    }
    return failures ? 1 : 0;
}
