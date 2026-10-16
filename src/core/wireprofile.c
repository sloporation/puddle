#include "wireprofile.h"

#include "access.h"
#include "encoding.h"
#include "json.h"
#include "profile.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const MIMES[] = { "image/png", "image/jpeg", "image/gif", "image/webp" };

static int mime_ok(const char *m)
{
    for (size_t i = 0; i < sizeof MIMES / sizeof *MIMES; i++)
        if (!strcmp(m, MIMES[i]))
            return 1;
    return 0;
}

static int hash_ok(const char *h)
{
    unsigned char raw[32];
    if (strlen(h) != 64)
        return 0;
    for (int i = 0; i < 64; i++)
        if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f')))
            return 0; /* lower case only: one spelling of a hash */
    return puddle_hex_decode(h, raw, 32) == 0;
}

/* {"hash","mime","size"} -> a reference, 0 or -1 */
static int media_of(puddle_span obj, puddle_media_ref *r)
{
    puddle_span v;
    int64_t size;
    if (obj.len < 2 || obj.p[0] != '{')
        return -1;
    if (puddle_json_member(obj.p, obj.len, "hash", &v) != 0 || puddle_json_string(v, r->hash, sizeof r->hash) != 0 || !hash_ok(r->hash))
        return -1;
    if (puddle_json_member(obj.p, obj.len, "mime", &v) != 0 || puddle_json_string(v, r->mime, sizeof r->mime) != 0 || !mime_ok(r->mime))
        return -1;
    if (puddle_json_member(obj.p, obj.len, "size", &v) != 0 || puddle_json_int64(v, &size) != 0 || size < 1 || (uint64_t)size > PUDDLE_IMAGE_MAX_BYTES)
        return -1;
    r->size = (uint64_t)size;
    return 0;
}

static int hex_color_ok(const char *c)
{
    if (strlen(c) != 7 || c[0] != '#')
        return 0;
    for (int i = 1; i < 7; i++)
        if (!isxdigit((unsigned char)c[i]))
            return 0;
    return 1;
}

int puddle_profile_wire_clean(const char *json, size_t len, char *out, size_t cap)
{
    static const struct { const char *key; size_t max; int multiline; int required; } texts[] = {
        { "display_name", 64, 0, 1 }, { "blurb", 160, 0, 0 }, { "about", 2000, 1, 0 }, { "pronouns", 32, 0, 0 },
    };
    if (len < 2 || json[0] != '{' || !puddle_json_valid_object(json, len))
        return -1;
    puddle_jw w;
    puddle_jw_init(&w, out, cap);
    puddle_jw_object(&w);
    for (size_t i = 0; i < sizeof texts / sizeof *texts; i++) {
        puddle_span v;
        int rc = puddle_json_member(json, len, texts[i].key, &v);
        if (rc < 0 || (rc == 1 && texts[i].required))
            return -1;
        if (rc == 1)
            continue;
        char *raw = malloc(v.len + 1), *clean = malloc(v.len + 48);
        size_t chars = 0;
        int bad = !raw || !clean || puddle_json_string(v, raw, v.len + 1) != 0 ||
                  puddle_clean_text(raw, strlen(raw), texts[i].max, texts[i].multiline, clean, v.len + 48, &chars) != 0 ||
                  (texts[i].required && chars == 0);
        if (!bad && chars > 0)
            puddle_jw_str(&w, texts[i].key, clean);
        free(raw);
        free(clean);
        if (bad)
            return -1;
    }
    /* the color */
    puddle_span cv;
    int rc = puddle_json_member(json, len, "color", &cv);
    if (rc < 0)
        return -1;
    if (rc == 0) {
        puddle_span arr, it, av;
        char stops[5][8];
        int n = 0;
        int64_t angle = -1;
        if (cv.len < 2 || cv.p[0] != '{' || puddle_json_member(cv.p, cv.len, "colors", &arr) != 0)
            return -1;
        size_t cur = 0;
        int r;
        while ((r = puddle_json_array_next(arr, &cur, &it)) == 1) {
            char t[16];
            if (n >= 5 || puddle_json_string(it, t, sizeof t) != 0 || !hex_color_ok(t))
                return -1;
            for (int k = 0; k < 7; k++)
                stops[n][k] = (char)tolower((unsigned char)t[k]);
            stops[n][7] = '\0';
            n++;
        }
        if (r < 0 || n < 1)
            return -1;
        int arc = puddle_json_member(cv.p, cv.len, "angle", &av);
        if (arc < 0 || (arc == 0 && (puddle_json_int64(av, &angle) != 0 || angle < 0 || angle > 360)))
            return -1;
        puddle_jw_key(&w, "color");
        puddle_jw_object(&w);
        puddle_jw_key(&w, "colors");
        puddle_jw_array(&w);
        for (int k = 0; k < n; k++)
            puddle_jw_string(&w, stops[k]);
        puddle_jw_end_array(&w);
        if (n > 1 && angle >= 0) /* an angle means nothing for one color */
            puddle_jw_num(&w, "angle", angle);
        puddle_jw_end_object(&w);
    }
    /* the images */
    static const char *const kinds[3] = { "avatar", "banner", "background" };
    for (int k = 0; k < 3; k++) {
        puddle_span mv;
        puddle_media_ref ref;
        int mrc = puddle_json_member(json, len, kinds[k], &mv);
        if (mrc < 0 || (mrc == 0 && media_of(mv, &ref) != 0))
            return -1;
        if (mrc == 0) {
            puddle_jw_key(&w, kinds[k]);
            puddle_jw_object(&w);
            puddle_jw_str(&w, "hash", ref.hash);
            puddle_jw_str(&w, "mime", ref.mime);
            puddle_jw_num(&w, "size", (int64_t)ref.size);
            puddle_jw_end_object(&w);
        }
    }
    puddle_jw_end_object(&w);
    return puddle_jw_finish(&w) ? 0 : -1;
}

size_t puddle_profile_wire_media(const char *json, size_t len, puddle_media_ref refs[3])
{
    static const char *const kinds[3] = { "avatar", "banner", "background" };
    size_t n = 0;
    for (int k = 0; k < 3; k++) {
        puddle_span mv;
        if (puddle_json_member(json, len, kinds[k], &mv) == 0 && media_of(mv, &refs[n]) == 0)
            n++;
    }
    return n;
}
