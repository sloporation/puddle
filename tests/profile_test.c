#include "profile.h"

#include "hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];

static char *at(const char *name)
{
    static char p[4][700];
    static int i;
    i = (i + 1) % 4;
    snprintf(p[i], sizeof p[i], "%s/%s", root, name);
    return p[i];
}

static void put(const char *name, const void *data, size_t len)
{
    FILE *f = fopen(at(name), "wb");
    fwrite(data, 1, len, f);
    fclose(f);
}

static void puts_file(const char *name, const char *text) { put(name, text, strlen(text)); }

static puddle_config cfg_for(const char *file, ...)
{
    puddle_config c;
    memset(&c, 0, sizeof c);
    snprintf(c.dir, sizeof c.dir, "%s", root);
    snprintf(c.data_dir, sizeof c.data_dir, "%s/data", root);
    if (file)
        snprintf(c.profile_files[c.nprofile_files++], PUDDLE_PATH_MAX, "%s", at(file));
    return c;
}

/* load `text` as a profile file; returns the status and leaves the message in err */
static int load_text(const char *text, puddle_profiles *p, char *err, size_t n)
{
    puts_file("p.yaml", text);
    puddle_config c = cfg_for("p.yaml");
    return puddle_profiles_load(p, &c, err, n);
}

static void expect_error(const char *text, const char *needle)
{
    puddle_profiles p;
    char err[300] = "";
    int rc = load_text(text, &p, err, sizeof err);
    if (rc == 0) {
        fprintf(stderr, "FAIL expected an error containing \"%s\" for:\n%s\n", needle, text);
        failures++;
        puddle_profiles_free(&p);
    } else if (!strstr(err, needle)) {
        fprintf(stderr, "FAIL error \"%s\" should contain \"%s\"\n", err, needle);
        failures++;
    }
}

static const unsigned char PNG[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 1, 2, 3, 4 };
static const unsigned char JPG[] = { 0xFF, 0xD8, 0xFF, 0xE0, 9, 9, 9 };

static void test_loading(void)
{
    put("avatar.png", PNG, sizeof PNG);
    put("banner.jpg", JPG, sizeof JPG);
    mkdir(at("pics"), 0700);
    put("pics/bg.gif", "GIF89a....", 10);
    char webp[16];
    memcpy(webp, "RIFF\x04\0\0\0WEBPVP8 ", 16);
    put("pics/w.webp", webp, sizeof webp);

    puddle_profiles p;
    char err[300];
    CHECK(load_text(
        "profiles:\n"
        "  - id: work\n"
        "    display_name: \"  Alice Chen  \"\n"
        "    avatar: avatar.png\n"
        "    blurb: Backend engineer\n"
        "    about: |\n"
        "      Line one.\n"
        "\n"
        "      Line two, with \u00e9 and \u65e5\u672c\u8a9e.\n"
        "    pronouns: she/her\n"
        "    color: \"#ABC\"\n"
        "    banner: banner.jpg\n"
        "    background: pics/bg.gif\n"
        "  - id: friends\n"
        "    display_name: Ali\n"
        "    color:\n"
        "      gradient: [\"#FF7A18\", \"#af002d\", \"#000\"]\n"
        "      angle: 135\n"
        "    avatar: pics/w.webp\n"
        "  - id: plain\n"
        "    display_name: Alice\n", &p, err, sizeof err) == 0);
    CHECK(p.count == 3);

    const puddle_profile *w = &p.items[0];
    CHECK(strcmp(w->id, "work") == 0 && strcmp(w->display_name, "Alice Chen") == 0);
    CHECK(strcmp(w->blurb, "Backend engineer") == 0 && strcmp(w->pronouns, "she/her") == 0);
    CHECK(strcmp(w->about, "Line one.\n\nLine two, with \u00e9 and \u65e5\u672c\u8a9e.") == 0);
    CHECK(w->color.kind == PUDDLE_COLOR_SOLID && w->color.nstops == 1 && strcmp(w->color.stops[0], "#aabbcc") == 0);

    /* images are copied into media/<sha256> by content */
    char hash[65], path[800];
    puddle_sha256_hex(PNG, sizeof PNG, hash);
    CHECK(strcmp(w->avatar.hash, hash) == 0 && strcmp(w->avatar.mime, "image/png") == 0 && w->avatar.size == sizeof PNG);
    snprintf(path, sizeof path, "%s/data/media/%s", root, hash);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (size_t)st.st_size == sizeof PNG);
    CHECK(strcmp(w->banner.mime, "image/jpeg") == 0 && strcmp(w->background.mime, "image/gif") == 0);

    const puddle_profile *f = puddle_profiles_find(&p, "friends");
    CHECK(f && f->color.kind == PUDDLE_COLOR_GRADIENT && f->color.nstops == 3 && f->color.angle == 135);
    CHECK(strcmp(f->color.stops[0], "#ff7a18") == 0 && strcmp(f->color.stops[2], "#000000") == 0);
    CHECK(strcmp(f->avatar.mime, "image/webp") == 0);

    const puddle_profile *pl = puddle_profiles_find(&p, "plain");
    CHECK(pl && !pl->blurb && !pl->about && !pl->pronouns && pl->color.kind == PUDDLE_COLOR_NONE);
    CHECK(pl->avatar.hash[0] == '\0' && pl->banner.hash[0] == '\0');
    CHECK(puddle_profiles_find(&p, "nope") == NULL);
    puddle_profiles_free(&p);

    /* loading again reuses the stored images */
    CHECK(load_text("profiles:\n  - id: a\n    display_name: A\n    avatar: avatar.png\n", &p, err, sizeof err) == 0);
    CHECK(strcmp(p.items[0].avatar.hash, hash) == 0);
    puddle_profiles_free(&p);
}

static void test_mistakes(void)
{
    expect_error("profiles:\n  - id: a\n    display_name: A\n    colour: red\n", "unknown setting 'colour'");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    colour: red\n", ":4:");
    expect_error("profiles:\n  - id: a\n", "display_name is required");
    expect_error("profiles:\n  - display_name: A\n", "needs an id");
    expect_error("profiles:\n  - id: Work\n    display_name: A\n", "needs an id");
    expect_error("profiles:\n  - id: a b\n    display_name: A\n", "needs an id");
    expect_error("profiles:\n  - id: a\n    display_name: A\n  - id: a\n    display_name: B\n", "already a profile with id 'a'");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    color: red\n", "color must look like");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    color: \"#12\"\n", "color must look like");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    color:\n      gradient: [\"#fff\"]\n", "2 to 5 colors");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    color:\n      gradient: [\"#fff\", \"#000\"]\n      angle: 400\n", "0 to 360");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    color:\n      gradient: [\"#fff\", \"#00g\"]\n", "gradient colors");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    color:\n      angle: 5\n", "gradient");
    expect_error("profiles:\n  - id: a\n    display_name: A\n    pronouns: [a]\n", "pronouns must be text");
    expect_error("profiles: nope\n", "must be a list");
    expect_error("other: 1\n", "unknown setting 'other'");
    expect_error("- id: a\n", "expected");
    expect_error("profiles:\n  - just text\n", "each profile needs");
    expect_error("profiles:\n  - id: a\n    display_name: \"a\\nb\"\n", "control characters");
    expect_error("profiles:\n  - id: a\n    display_name: \xff\n", "UTF-8"); /* a raw invalid byte */
}

static void test_limits(void)
{
    char text[4000], big[2100];

    memset(big, 'x', 65); big[65] = '\0';
    snprintf(text, sizeof text, "profiles:\n  - id: a\n    display_name: %s\n", big);
    expect_error(text, "65 characters; the limit is 64");

    memset(big, 'x', 64); big[64] = '\0'; /* exactly at the limit is fine */
    snprintf(text, sizeof text, "profiles:\n  - id: a\n    display_name: %s\n", big);
    puddle_profiles p; char err[300];
    CHECK(load_text(text, &p, err, sizeof err) == 0);
    puddle_profiles_free(&p);

    /* limits count characters, not bytes: 64 two-byte characters pass */
    char two[200] = "";
    for (int i = 0; i < 64; i++) strcat(two, "\xc3\xa9");
    snprintf(text, sizeof text, "profiles:\n  - id: a\n    display_name: \"%s\"\n", two);
    CHECK(load_text(text, &p, err, sizeof err) == 0);
    puddle_profiles_free(&p);

    memset(big, 'x', 161); big[161] = '\0';
    snprintf(text, sizeof text, "profiles:\n  - id: a\n    display_name: A\n    blurb: %s\n", big);
    expect_error(text, "blurb is 161 characters");

    memset(big, 'y', 2001); big[2001] = '\0';
    snprintf(text, sizeof text, "profiles:\n  - id: a\n    display_name: A\n    about: %s\n", big);
    expect_error(text, "about is 2001 characters");
}

static void test_images_are_forgiving(void)
{
    puts_file("notimage.txt", "hello");
    char fake[8] = { (char)0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    put("huge.png", fake, sizeof fake);
    truncate(at("huge.png"), (off_t)PUDDLE_IMAGE_MAX_BYTES + 1); /* sparse, 25 MiB + 1 */
    put("empty.png", "", 0);

    puddle_profiles p; char err[300];
    CHECK(load_text(
        "profiles:\n"
        "  - id: a\n"
        "    display_name: A\n"
        "    avatar: missing.png\n"
        "    banner: notimage.txt\n"
        "    background: huge.png\n"
        "  - id: b\n"
        "    display_name: B\n"
        "    avatar: empty.png\n"
        "    banner: /\n", &p, err, sizeof err) == 0); /* warned about, not fatal */
    CHECK(p.count == 2);
    CHECK(p.items[0].avatar.hash[0] == '\0' && p.items[0].banner.hash[0] == '\0' && p.items[0].background.hash[0] == '\0');
    CHECK(p.items[1].avatar.hash[0] == '\0' && p.items[1].banner.hash[0] == '\0');
    puddle_profiles_free(&p);

    /* but an image setting that isn't a path at all is a mistake */
    expect_error("profiles:\n  - id: a\n    display_name: A\n    avatar: [x]\n", "image must be a file path");
}

static void test_files_and_default(void)
{
    puddle_profiles p; char err[300];

    puddle_config none = cfg_for(NULL);
    CHECK(puddle_profiles_load(&p, &none, err, sizeof err) == 0);
    CHECK(p.count == 1 && strcmp(p.items[0].id, "default") == 0 && p.items[0].display_name[0] != '\0');
    puddle_profiles_free(&p);

    puts_file("one.yaml", "profiles:\n  - id: first\n    display_name: First\n  - id: second\n    display_name: Second\n");
    puts_file("two.yaml", "profiles:\n  - id: third\n    display_name: Third\n");
    puddle_config c = cfg_for("one.yaml");
    snprintf(c.profile_files[c.nprofile_files++], PUDDLE_PATH_MAX, "%s", at("two.yaml"));
    CHECK(puddle_profiles_load(&p, &c, err, sizeof err) == 0);
    CHECK(p.count == 3 && strcmp(p.items[0].id, "first") == 0 && strcmp(p.items[2].id, "third") == 0);
    puddle_profiles_free(&p);

    puts_file("dup.yaml", "profiles:\n  - id: first\n    display_name: Again\n");
    snprintf(c.profile_files[1], PUDDLE_PATH_MAX, "%s", at("dup.yaml"));
    CHECK(puddle_profiles_load(&p, &c, err, sizeof err) == -1 && strstr(err, "already a profile with id 'first'"));

    snprintf(c.profile_files[1], PUDDLE_PATH_MAX, "%s", at("absent.yaml"));
    CHECK(puddle_profiles_load(&p, &c, err, sizeof err) == -1 && strstr(err, "absent.yaml"));
}

#define PEER_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define PEER_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define PEER_C "cccccccccccccccccccccccccccccccccccccccccccccccccccc"

static void test_assignments(void)
{
    puts_file("a.yaml", "profiles:\n  - id: friends\n    display_name: F\n  - id: work\n    display_name: W\n  - id: family\n    display_name: Fam\n");
    puddle_config c = cfg_for("a.yaml");
    puddle_profiles p; char err[300], id[PUDDLE_PROFILE_ID_MAX + 1];
    CHECK(puddle_profiles_load(&p, &c, err, sizeof err) == 0);

    puddle_assignments *a = puddle_assignments_open(c.data_dir, err, sizeof err);
    CHECK(a != NULL);
    CHECK(puddle_assignments_get(a, PEER_A, id) == 1);
    CHECK(strcmp(puddle_profile_for_peer(&p, a, PEER_A)->id, "friends") == 0); /* default: the first */

    CHECK(puddle_assignments_set(a, PEER_A, "work", err, sizeof err) == 0);
    CHECK(puddle_assignments_set(a, PEER_B, "family", err, sizeof err) == 0);
    CHECK(puddle_assignments_get(a, PEER_A, id) == 0 && strcmp(id, "work") == 0);
    CHECK(strcmp(puddle_profile_for_peer(&p, a, PEER_A)->id, "work") == 0);
    CHECK(strcmp(puddle_profile_for_peer(&p, a, PEER_B)->id, "family") == 0);
    CHECK(strcmp(puddle_profile_for_peer(&p, a, PEER_C)->id, "friends") == 0);

    CHECK(puddle_assignments_set(a, PEER_A, "family", err, sizeof err) == 0); /* change */
    CHECK(strcmp(puddle_profile_for_peer(&p, a, PEER_A)->id, "family") == 0);
    puddle_assignments_close(a);

    a = puddle_assignments_open(c.data_dir, err, sizeof err); /* survives a restart */
    CHECK(puddle_assignments_get(a, PEER_A, id) == 0 && strcmp(id, "family") == 0);
    CHECK(puddle_assignments_get(a, PEER_B, id) == 0 && strcmp(id, "family") == 0);

    CHECK(puddle_assignments_set(a, PEER_A, NULL, err, sizeof err) == 0); /* clear */
    CHECK(puddle_assignments_get(a, PEER_A, id) == 1);
    CHECK(puddle_assignments_get(a, PEER_B, id) == 0);
    puddle_assignments_close(a);
    a = puddle_assignments_open(c.data_dir, err, sizeof err);
    CHECK(puddle_assignments_get(a, PEER_A, id) == 1 && puddle_assignments_get(a, PEER_B, id) == 0);

    /* a profile removed from the files falls back to the default */
    CHECK(puddle_assignments_set(a, PEER_C, "gone", err, sizeof err) == 0);
    CHECK(strcmp(puddle_profile_for_peer(&p, a, PEER_C)->id, "friends") == 0);

    /* only well-formed ids are accepted */
    CHECK(puddle_assignments_set(a, "short", "work", err, sizeof err) == -1);
    CHECK(puddle_assignments_set(a, PEER_A, "Bad Id", err, sizeof err) == -1);
    CHECK(puddle_assignments_set(a, PEER_A "\t", "work", err, sizeof err) == -1);

    puddle_assignments_close(a);
    puddle_profiles_free(&p);
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-profile-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root))
        return 2;

    test_loading();
    test_mistakes();
    test_limits();
    test_images_are_forgiving();
    test_files_and_default();
    test_assignments();

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) {}
    return failures ? 1 : 0;
}
