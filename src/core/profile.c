#include "profile.h"

#include "fsutil.h"
#include "hash.h"
#include "log.h"
#include "yamltree.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_DISPLAY_NAME 64
#define MAX_BLURB 160
#define MAX_ABOUT 2000
#define MAX_PRONOUNS 32

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ text */

/* 0 if `s` is well-formed UTF-8, with the number of characters in *chars */
static int utf8_count(const char *s, size_t *chars)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    while (*p) {
        unsigned char c = *p;
        if (c < 0x80) {
            p += 1;
        } else if (c >= 0xC2 && c <= 0xDF && (p[1] & 0xC0) == 0x80) {
            p += 2;
        } else if (c >= 0xE0 && c <= 0xEF && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   !(c == 0xE0 && p[1] < 0xA0) && !(c == 0xED && p[1] >= 0xA0)) {
            p += 3;
        } else if (c >= 0xF0 && c <= 0xF4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   (p[3] & 0xC0) == 0x80 && !(c == 0xF0 && p[1] < 0x90) && !(c == 0xF4 && p[1] >= 0x90)) {
            p += 4;
        } else {
            return -1;
        }
        n++;
    }
    *chars = n;
    return 0;
}

static int valid_profile_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n > PUDDLE_PROFILE_ID_MAX)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static int valid_peer_id(const char *id)
{
    if (strlen(id) != 52)
        return 0;
    for (int i = 0; i < 52; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '2' && id[i] <= '7')))
            return 0;
    return 1;
}

/* ----------------------------------------------------------------- images */

static const char *sniff_mime(const unsigned char *b, size_t n)
{
    if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0)
        return "image/png";
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return "image/jpeg";
    if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0))
        return "image/gif";
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0)
        return "image/webp";
    return NULL;
}

/* Copy an image into <data_dir>/media/<sha256>. 0 imported, 1 skipped with a warning, -1 can't write. */
static int import_image(const puddle_config *cfg, const char *path, const char *who, puddle_media *out)
{
    memset(out, 0, sizeof *out);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        puddle_log(PUDDLE_LOG_WARN, "profile %s: cannot read image %s (%s); leaving it out", who, path,
                   strerror(errno));
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0 ||
        (uint64_t)st.st_size > PUDDLE_IMAGE_MAX_BYTES) {
        close(fd);
        puddle_log(PUDDLE_LOG_WARN, "profile %s: %s is not a regular file of 1 byte to %u MiB; leaving it out",
                   who, path, PUDDLE_IMAGE_MAX_BYTES >> 20);
        return 1;
    }
    size_t size = (size_t)st.st_size;
    unsigned char *buf = malloc(size);
    if (!buf) {
        close(fd);
        return -1;
    }
    size_t got = 0;
    while (got < size) {
        ssize_t r = read(fd, buf + got, size - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);

    const char *mime = got == size ? sniff_mime(buf, size) : NULL;
    if (!mime) {
        free(buf);
        puddle_log(PUDDLE_LOG_WARN, "profile %s: %s is not a png, jpg, gif or webp image; leaving it out", who, path);
        return 1;
    }

    puddle_sha256_hex(buf, size, out->hash);
    snprintf(out->mime, sizeof out->mime, "%s", mime);
    out->size = size;

    char dir[PUDDLE_PATH_MAX], dest[PUDDLE_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/media", cfg->data_dir);
    int w = snprintf(dest, sizeof dest, "%s/%s", dir, out->hash);
    int rc = 0;
    if (w < 0 || (size_t)w >= sizeof dest || puddle_mkdir_p(dir) != 0) {
        rc = -1;
    } else if (access(dest, F_OK) != 0) { /* content-addressed: already there means identical */
        rc = puddle_write_file_atomic(dir, dest, buf, size);
    }
    free(buf);
    if (rc != 0) {
        puddle_log(PUDDLE_LOG_ERROR, "profile %s: cannot store image in %s: %s", who, dir, strerror(errno));
        memset(out, 0, sizeof *out);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- loading */

static void profile_free(puddle_profile *p)
{
    free(p->display_name);
    free(p->blurb);
    free(p->about);
    free(p->pronouns);
    memset(p, 0, sizeof *p);
}

void puddle_profiles_free(puddle_profiles *p)
{
    if (!p)
        return;
    for (size_t i = 0; i < p->count; i++)
        profile_free(&p->items[i]);
    free(p->items);
    p->items = NULL;
    p->count = 0;
}

const puddle_profile *puddle_profiles_find(const puddle_profiles *p, const char *id)
{
    for (size_t i = 0; i < p->count; i++)
        if (strcmp(p->items[i].id, id) == 0)
            return &p->items[i];
    return NULL;
}

typedef struct {
    const char *file;
    char dir[PUDDLE_PATH_MAX];
    const puddle_config *cfg;
    char *err;
    size_t errlen;
} loadctx;

static int fail(const loadctx *c, const ynode *n, const char *profile, const char *fmt, ...)
{
    char what[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    if (profile && *profile)
        seterr(c->err, c->errlen, "%s:%d: profile '%s': %s", c->file, ynode_line(n), profile, what);
    else
        seterr(c->err, c->errlen, "%s:%d: %s", c->file, ynode_line(n), what);
    return -1;
}

static char *trimmed_copy(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        n--;
    char *out = malloc(n + 1);
    if (out) {
        memcpy(out, s, n);
        out[n] = '\0';
    }
    return out;
}

/* a text field into *out. A null/absent value leaves *out NULL. 0 or -1 (error already set). */
static int text_field(const loadctx *c, const ynode *v, const char *profile, const char *key,
                      size_t maxchars, int multiline, char **out)
{
    if (ynode_is_null(v))
        return 0;
    const char *s = ynode_scalar(v);
    if (!s)
        return fail(c, v, profile, "%s must be text", key);
    char *t = trimmed_copy(s);
    if (!t)
        return fail(c, v, profile, "out of memory");
    size_t chars = 0;
    if (utf8_count(t, &chars) != 0) {
        free(t);
        return fail(c, v, profile, "%s is not valid UTF-8", key);
    }
    for (const char *q = t; *q; q++) {
        unsigned char ch = (unsigned char)*q;
        if (ch < 0x20 && !(multiline && (ch == '\n' || ch == '\t'))) {
            free(t);
            return fail(c, v, profile, "%s cannot contain control characters or line breaks", key);
        }
    }
    if (chars > maxchars) {
        free(t);
        return fail(c, v, profile, "%s is %zu characters; the limit is %zu", key, chars, maxchars);
    }
    if (!*t) {
        free(t);
        return 0;
    }
    *out = t;
    return 0;
}

/* "#rgb" or "#rrggbb" into "#rrggbb" lowercase */
static int parse_hex_color(const char *s, char out[8])
{
    size_t n = strlen(s);
    if ((n != 4 && n != 7) || s[0] != '#')
        return -1;
    for (size_t i = 1; i < n; i++) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
            return -1;
    }
    out[0] = '#';
    for (int i = 0; i < 6; i++) {
        char ch = n == 4 ? s[1 + i / 2] : s[1 + i];
        out[1 + i] = (ch >= 'A' && ch <= 'F') ? (char)(ch - 'A' + 'a') : ch;
    }
    out[7] = '\0';
    return 0;
}

static int color_field(const loadctx *c, const ynode *v, const char *profile, puddle_color *out)
{
    if (ynode_is_null(v))
        return 0;
    if (ynode_kind(v) == Y_SCALAR) {
        if (parse_hex_color(ynode_scalar(v), out->stops[0]) != 0)
            return fail(c, v, profile, "color must look like \"#3366ff\", or be a gradient");
        out->kind = PUDDLE_COLOR_SOLID;
        out->nstops = 1;
        return 0;
    }
    if (ynode_kind(v) != Y_MAP)
        return fail(c, v, profile, "color must be \"#rrggbb\" or a gradient");

    const ynode *g = NULL;
    int angle = 180;
    for (size_t i = 0; i < ynode_count(v); i++) {
        const char *k = ynode_key(v, i);
        const ynode *x = ynode_item(v, i);
        if (strcmp(k, "gradient") == 0) {
            g = x;
        } else if (strcmp(k, "angle") == 0) {
            const char *a = ynode_scalar(x);
            char *end = NULL;
            long val = a ? strtol(a, &end, 10) : -1;
            if (!a || !*a || *end != '\0' || val < 0 || val > 360)
                return fail(c, x, profile, "angle must be a number of degrees from 0 to 360");
            angle = (int)val;
        } else {
            return fail(c, x, profile, "unknown color setting '%.40s' (use gradient and angle)", k);
        }
    }
    if (!g || ynode_kind(g) != Y_SEQ || ynode_count(g) < 2 || ynode_count(g) > PUDDLE_MAX_GRADIENT_STOPS)
        return fail(c, g ? g : v, profile, "gradient needs a list of 2 to %d colors", PUDDLE_MAX_GRADIENT_STOPS);
    for (size_t i = 0; i < ynode_count(g); i++) {
        const char *s = ynode_scalar(ynode_item(g, i));
        if (!s || parse_hex_color(s, out->stops[i]) != 0)
            return fail(c, ynode_item(g, i), profile, "gradient colors must look like \"#3366ff\"");
    }
    out->kind = PUDDLE_COLOR_GRADIENT;
    out->nstops = (int)ynode_count(g);
    out->angle = angle;
    return 0;
}

static int image_field(const loadctx *c, const ynode *v, const char *profile, puddle_media *out)
{
    if (ynode_is_null(v))
        return 0;
    const char *s = ynode_scalar(v);
    char path[PUDDLE_PATH_MAX];
    if (!s || !*s || puddle_resolve_path(c->dir, s, path, sizeof path) != 0)
        return fail(c, v, profile, "an image must be a file path");
    return import_image(c->cfg, path, profile, out) < 0 ? fail(c, v, profile, "cannot store the image in the data directory") : 0;
}

static int load_one(const loadctx *c, const ynode *item, puddle_profile *p)
{
    memset(p, 0, sizeof *p);
    if (ynode_kind(item) != Y_MAP)
        return fail(c, item, NULL, "each profile needs settings like \"display_name: ...\"");

    const ynode *idn = ynode_get(item, "id");
    const char *id = idn ? ynode_scalar(idn) : NULL;
    if (!id || !valid_profile_id(id))
        return fail(c, idn ? idn : item, NULL, "every profile needs an id of 1-%d letters, digits, - or _ (lowercase)",
                    PUDDLE_PROFILE_ID_MAX);
    snprintf(p->id, sizeof p->id, "%s", id);

    for (size_t i = 0; i < ynode_count(item); i++) {
        const char *k = ynode_key(item, i);
        const ynode *v = ynode_item(item, i);
        int rc = 0;
        if (strcmp(k, "id") == 0) {
            /* done */
        } else if (strcmp(k, "display_name") == 0) {
            rc = text_field(c, v, id, k, MAX_DISPLAY_NAME, 0, &p->display_name);
        } else if (strcmp(k, "blurb") == 0) {
            rc = text_field(c, v, id, k, MAX_BLURB, 0, &p->blurb);
        } else if (strcmp(k, "about") == 0) {
            rc = text_field(c, v, id, k, MAX_ABOUT, 1, &p->about);
        } else if (strcmp(k, "pronouns") == 0) {
            rc = text_field(c, v, id, k, MAX_PRONOUNS, 0, &p->pronouns);
        } else if (strcmp(k, "color") == 0) {
            rc = color_field(c, v, id, &p->color);
        } else if (strcmp(k, "avatar") == 0) {
            rc = image_field(c, v, id, &p->avatar);
        } else if (strcmp(k, "banner") == 0) {
            rc = image_field(c, v, id, &p->banner);
        } else if (strcmp(k, "background") == 0) {
            rc = image_field(c, v, id, &p->background);
        } else {
            rc = fail(c, v, id, "unknown setting '%.40s'", k);
        }
        if (rc != 0) {
            profile_free(p);
            return -1;
        }
    }
    if (!p->display_name) {
        int rc = fail(c, item, id, "display_name is required");
        profile_free(p);
        return rc;
    }
    return 0;
}

static int load_file(const puddle_config *cfg, const char *file, puddle_profiles *out, char *err, size_t errlen)
{
    ynode *root = puddle_yaml_parse_file(file, err, errlen);
    if (!root)
        return -1;

    loadctx c = { .file = file, .cfg = cfg, .err = err, .errlen = errlen };
    snprintf(c.dir, sizeof c.dir, "%s", file);
    char *slash = strrchr(c.dir, '/');
    if (slash)
        *slash = '\0';
    else
        snprintf(c.dir, sizeof c.dir, ".");

    int rc = 0;
    if (ynode_kind(root) != Y_MAP) {
        rc = fail(&c, root, NULL, "expected \"profiles:\" followed by a list");
        goto done;
    }
    const ynode *list = NULL;
    for (size_t i = 0; i < ynode_count(root); i++) {
        if (strcmp(ynode_key(root, i), "profiles") != 0) {
            rc = fail(&c, ynode_item(root, i), NULL, "unknown setting '%.40s' (a profile file holds \"profiles:\" only)",
                      ynode_key(root, i));
            goto done;
        }
        list = ynode_item(root, i);
    }
    if (!list || ynode_is_null(list))
        goto done;
    if (ynode_kind(list) != Y_SEQ) {
        rc = fail(&c, list, NULL, "profiles must be a list; start each profile with \"- id: ...\"");
        goto done;
    }
    for (size_t i = 0; i < ynode_count(list); i++) {
        const ynode *item = ynode_item(list, i);
        if (out->count >= PUDDLE_MAX_PROFILES) {
            rc = fail(&c, item, NULL, "at most %d profiles", PUDDLE_MAX_PROFILES);
            goto done;
        }
        puddle_profile p;
        if (load_one(&c, item, &p) != 0) {
            rc = -1;
            goto done;
        }
        if (puddle_profiles_find(out, p.id)) {
            rc = fail(&c, item, NULL, "there is already a profile with id '%s'", p.id);
            profile_free(&p);
            goto done;
        }
        puddle_profile *grown = realloc(out->items, (out->count + 1) * sizeof *grown);
        if (!grown) {
            profile_free(&p);
            rc = fail(&c, item, NULL, "out of memory");
            goto done;
        }
        out->items = grown;
        out->items[out->count++] = p;
    }
done:
    puddle_yaml_free(root);
    return rc;
}

int puddle_profiles_load(puddle_profiles *out, const puddle_config *cfg, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < cfg->nprofile_files; i++) {
        if (load_file(cfg, cfg->profile_files[i], out, err, errlen) != 0) {
            puddle_profiles_free(out);
            return -1;
        }
    }
    if (out->count == 0) { /* nothing configured: one profile named after the OS user */
        puddle_profile p;
        memset(&p, 0, sizeof p);
        snprintf(p.id, sizeof p.id, "default");
        char name[64];
        puddle_login_name(name, sizeof name);
        p.display_name = strdup(name);
        out->items = p.display_name ? malloc(sizeof *out->items) : NULL;
        if (!out->items) {
            free(p.display_name);
            seterr(err, errlen, "out of memory");
            return -1;
        }
        out->items[0] = p;
        out->count = 1;
    }
    return 0;
}

/* ---------------------------------------------------------- assignments */

typedef struct {
    char peer[53];
    char profile[PUDDLE_PROFILE_ID_MAX + 1];
} pair;

struct puddle_assignments {
    char path[PUDDLE_PATH_MAX];
    char dir[PUDDLE_PATH_MAX];
    pthread_mutex_t mu;
    pair *items;
    size_t n;
};

puddle_assignments *puddle_assignments_open(const char *data_dir, char *err, size_t errlen)
{
    puddle_assignments *a = calloc(1, sizeof *a);
    if (!a) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    pthread_mutex_init(&a->mu, NULL);
    snprintf(a->dir, sizeof a->dir, "%s/state", data_dir);
    snprintf(a->path, sizeof a->path, "%s/stream-profiles.tsv", a->dir);
    if (puddle_mkdir_p(a->dir) != 0) {
        seterr(err, errlen, "cannot create %s: %s", a->dir, strerror(errno));
        puddle_assignments_close(a);
        return NULL;
    }

    FILE *f = fopen(a->path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *tab = strchr(line, '\t');
            if (!tab) {
                if (*line)
                    puddle_log(PUDDLE_LOG_WARN, "state: ignoring a bad line in %s", a->path);
                continue;
            }
            *tab = '\0';
            if (!valid_peer_id(line) || !valid_profile_id(tab + 1)) {
                puddle_log(PUDDLE_LOG_WARN, "state: ignoring a bad line in %s", a->path);
                continue;
            }
            pair *g = realloc(a->items, (a->n + 1) * sizeof *g);
            if (!g)
                break;
            a->items = g;
            snprintf(g[a->n].peer, sizeof g[a->n].peer, "%s", line);
            snprintf(g[a->n].profile, sizeof g[a->n].profile, "%s", tab + 1);
            a->n++;
        }
        fclose(f);
    }
    return a;
}

void puddle_assignments_close(puddle_assignments *a)
{
    if (!a)
        return;
    pthread_mutex_destroy(&a->mu);
    free(a->items);
    free(a);
}

int puddle_assignments_get(puddle_assignments *a, const char *peer, char out[PUDDLE_PROFILE_ID_MAX + 1])
{
    int rc = 1;
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->n; i++)
        if (strcmp(a->items[i].peer, peer) == 0) {
            memcpy(out, a->items[i].profile, PUDDLE_PROFILE_ID_MAX + 1);
            rc = 0;
            break;
        }
    pthread_mutex_unlock(&a->mu);
    return rc;
}

static int persist_locked(puddle_assignments *a)
{
    size_t cap = a->n * 100 + 16;
    char *buf = malloc(cap);
    if (!buf)
        return -1;
    size_t len = 0;
    for (size_t i = 0; i < a->n; i++)
        len += (size_t)snprintf(buf + len, cap - len, "%s\t%s\n", a->items[i].peer, a->items[i].profile);
    int rc = puddle_write_file_atomic(a->dir, a->path, buf, len);
    free(buf);
    return rc;
}

int puddle_assignments_set(puddle_assignments *a, const char *peer, const char *profile_id, char *err,
                           size_t errlen)
{
    if (!valid_peer_id(peer) || (profile_id && !valid_profile_id(profile_id))) {
        seterr(err, errlen, "invalid user or profile id");
        return -1;
    }
    int rc = 0;
    pthread_mutex_lock(&a->mu);
    size_t idx = a->n;
    for (size_t i = 0; i < a->n; i++)
        if (strcmp(a->items[i].peer, peer) == 0) {
            idx = i;
            break;
        }
    pair saved = idx < a->n ? a->items[idx] : (pair){ { 0 }, { 0 } };
    size_t saved_n = a->n;
    if (!profile_id) {
        if (idx < a->n)
            a->items[idx] = a->items[--a->n];
    } else if (idx < a->n) {
        snprintf(a->items[idx].profile, sizeof a->items[idx].profile, "%s", profile_id);
    } else {
        pair *g = realloc(a->items, (a->n + 1) * sizeof *g);
        if (!g) {
            rc = -1;
        } else {
            a->items = g;
            snprintf(g[a->n].peer, sizeof g[a->n].peer, "%s", peer);
            snprintf(g[a->n].profile, sizeof g[a->n].profile, "%s", profile_id);
            a->n++;
        }
    }
    if (rc == 0 && persist_locked(a) != 0) {
        rc = -1;
        /* keep memory and disk in step: undo */
        if (!profile_id && idx < saved_n) {
            a->items[a->n++] = a->items[idx];
            a->items[idx] = saved;
        } else if (idx < saved_n) {
            a->items[idx] = saved;
        } else if (a->n > saved_n) {
            a->n = saved_n;
        }
    }
    if (rc != 0)
        seterr(err, errlen, "cannot save %s: %s", a->path, strerror(errno));
    pthread_mutex_unlock(&a->mu);
    return rc;
}

const puddle_profile *puddle_profile_for_peer(const puddle_profiles *p, puddle_assignments *a, const char *peer)
{
    char id[PUDDLE_PROFILE_ID_MAX + 1];
    if (a && puddle_assignments_get(a, peer, id) == 0) {
        const puddle_profile *found = puddle_profiles_find(p, id);
        if (found)
            return found;
    }
    return p->count ? &p->items[0] : NULL;
}
