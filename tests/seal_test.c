#include "seal.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

static puddle_identity *make(const char *name)
{
    char dir[700], err[300], phrase[PUDDLE_PHRASE_LEN + 1];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    puddle_identity *id = puddle_identity_create(dir, phrase, err, sizeof err);
    if (!id) exit(2);
    puddle_identity_confirm(dir);
    return id;
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-seal-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    puddle_identity *a = make("a"), *b = make("b"), *c = make("c");
    unsigned char pa[32], pb[32], pc[32];
    puddle_identity_public(a, NULL, pa, NULL);
    puddle_identity_public(b, NULL, pb, NULL);
    puddle_identity_public(c, NULL, pc, NULL);

    const char *msg = "hello, \xE2\x98\x95 this is a body";
    const char *aad = "chat|sender|1760000000000";
    unsigned char blob[4096], plain[4096];
    size_t bl = 0, pl = 0;

    /* ---- one recipient, and a group ---- */
    unsigned char one[1][32];
    memcpy(one[0], pb, 32);
    CHECK(puddle_seal(one, 1, aad, strlen(aad), msg, strlen(msg), blob, sizeof blob, &bl) == 0);
    CHECK(bl == PUDDLE_SEAL_OVERHEAD(1) + strlen(msg));
    CHECK(memmem(blob, bl, "hello", 5) == NULL);                                  /* no plain text in it */
    CHECK(puddle_open(b, aad, strlen(aad), blob, bl, plain, sizeof plain, &pl) == 0 && pl == strlen(msg) && !memcmp(plain, msg, pl));
    CHECK(puddle_open(a, aad, strlen(aad), blob, bl, plain, sizeof plain, &pl) == -1);   /* not for a */
    CHECK(puddle_open(c, aad, strlen(aad), blob, bl, plain, sizeof plain, &pl) == -1);

    unsigned char three[3][32];
    memcpy(three[0], pa, 32); memcpy(three[1], pb, 32); memcpy(three[2], pc, 32);
    unsigned char g[4096];
    size_t gl = 0;
    CHECK(puddle_seal(three, 3, aad, strlen(aad), msg, strlen(msg), g, sizeof g, &gl) == 0 && gl == PUDDLE_SEAL_OVERHEAD(3) + strlen(msg));
    puddle_identity *all[3] = { a, b, c };
    for (int i = 0; i < 3; i++)
        CHECK(puddle_open(all[i], aad, strlen(aad), g, gl, plain, sizeof plain, &pl) == 0 && pl == strlen(msg) && !memcmp(plain, msg, pl));
    {
        puddle_identity *d = make("d");                                           /* a stranger on the same blob */
        CHECK(puddle_open(d, aad, strlen(aad), g, gl, plain, sizeof plain, &pl) == -1);
        puddle_identity_close(d);
    }

    /* ---- never the same bytes twice ---- */
    unsigned char g2[4096];
    size_t g2l = 0;
    CHECK(puddle_seal(three, 3, aad, strlen(aad), msg, strlen(msg), g2, sizeof g2, &g2l) == 0 && g2l == gl && memcmp(g, g2, gl) != 0);

    /* ---- the context is bound ---- */
    CHECK(puddle_open(b, "other", 5, g, gl, plain, sizeof plain, &pl) == -1);
    CHECK(puddle_open(b, NULL, 0, g, gl, plain, sizeof plain, &pl) == -1);
    CHECK(puddle_open(b, aad, strlen(aad) - 1, g, gl, plain, sizeof plain, &pl) == -1);

    /* ---- any changed byte fails, whichever part it is in ---- */
    int bad = 0;
    for (size_t i = 0; i < gl; i++) {
        g[i] ^= 1;
        for (int r = 0; r < 3; r++)
            if (puddle_open(all[r], aad, strlen(aad), g, gl, plain, sizeof plain, &pl) == 0)
                bad++;
        g[i] ^= 1;
    }
    CHECK(bad == 0);
    CHECK(puddle_open(b, aad, strlen(aad), g, gl, plain, sizeof plain, &pl) == 0);   /* and the original still opens */

    /* ---- truncated, extended, empty, garbage ---- */
    for (size_t i = 0; i < gl; i++)
        if (puddle_open(b, aad, strlen(aad), g, i, plain, sizeof plain, &pl) == 0) bad++;
    CHECK(bad == 0);
    g[gl] = 0;
    CHECK(puddle_open(b, aad, strlen(aad), g, gl + 1, plain, sizeof plain, &pl) == -1);
    CHECK(puddle_open(b, aad, strlen(aad), NULL, 0, plain, sizeof plain, &pl) == -1);
    {
        unsigned char junk[200];
        for (size_t i = 0; i < sizeof junk; i++) junk[i] = (unsigned char)(i * 7 + 1);
        junk[0] = 1; junk[45] = 1;
        CHECK(puddle_open(b, aad, strlen(aad), junk, sizeof junk, plain, sizeof plain, &pl) == -1);
    }

    /* ---- an entry cannot be dropped or reordered from under the body ---- */
    {
        unsigned char x[4096];
        memcpy(x, g, gl);
        /* swap entries 0 and 1 */
        unsigned char tmp[56];
        memcpy(tmp, x + 46, 56); memcpy(x + 46, x + 46 + 56, 56); memcpy(x + 46 + 56, tmp, 56);
        CHECK(puddle_open(b, aad, strlen(aad), x, gl, plain, sizeof plain, &pl) == -1);
    }

    /* ---- sizes: empty body, the largest body, and the limits ---- */
    CHECK(puddle_seal(one, 1, "", 0, "", 0, blob, sizeof blob, &bl) == 0 && bl == PUDDLE_SEAL_OVERHEAD(1));
    CHECK(puddle_open(b, "", 0, blob, bl, plain, sizeof plain, &pl) == 0 && pl == 0);
    {
        size_t big = 16000;
        unsigned char *p = malloc(big), *s = malloc(big + 1024), *o = malloc(big);
        for (size_t i = 0; i < big; i++) p[i] = (unsigned char)i;
        size_t sl = 0, ol = 0;
        CHECK(puddle_seal(one, 1, aad, strlen(aad), p, big, s, big + 1024, &sl) == 0);
        CHECK(puddle_open(b, aad, strlen(aad), s, sl, o, big, &ol) == 0 && ol == big && !memcmp(o, p, big));
        CHECK(puddle_open(b, aad, strlen(aad), s, sl, o, big - 1, &ol) == -1);        /* output too small */
        CHECK(puddle_seal(one, 1, aad, strlen(aad), p, big, s, sl - 1, &sl) == -1);   /* buffer too small */
        free(p); free(s); free(o);
    }
    CHECK(puddle_seal(one, 0, aad, 3, "x", 1, blob, sizeof blob, &bl) == -1);
    {
        static unsigned char many[PUDDLE_SEAL_MAX_RECIPIENTS + 1][32];
        static unsigned char mb[20000];
        for (int i = 0; i <= PUDDLE_SEAL_MAX_RECIPIENTS; i++) memcpy(many[i], pb, 32);
        CHECK(puddle_seal(many, PUDDLE_SEAL_MAX_RECIPIENTS, aad, 3, "x", 1, mb, sizeof mb, &bl) == 0);
        CHECK(puddle_open(b, aad, 3, mb, bl, plain, sizeof plain, &pl) == 0);
        CHECK(puddle_seal(many, PUDDLE_SEAL_MAX_RECIPIENTS + 1, aad, 3, "x", 1, mb, sizeof mb, &bl) == -1);
    }

    /* ---- bad recipient keys ---- */
    {
        unsigned char zero[1][32] = { { 0 } };                                    /* a low-order point: no shared secret */
        CHECK(puddle_seal(zero, 1, aad, 3, "x", 1, blob, sizeof blob, &bl) == -1);
    }

    /* ---- the DH both sides compute is the same ---- */
    CHECK(puddle_identity_dh(a, pb, plain) == 0 && puddle_identity_dh(b, pa, plain + 32) == 0 && !memcmp(plain, plain + 32, 32));

    puddle_identity_close(a); puddle_identity_close(b); puddle_identity_close(c);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("seal_test ok\n");
    return 0;
}
