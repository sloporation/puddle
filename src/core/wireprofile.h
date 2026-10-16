#ifndef PUDDLE_WIREPROFILE_H
#define PUDDLE_WIREPROFILE_H

/* A profile as another Puddle sends it (api/federation.yaml, "Profile"), and what we make of it. Nothing a stranger sends is
 * kept as sent: it is rebuilt from the fields we understand, each checked and cleaned, so what is stored and shown is always well
 * formed. A profile with anything wrong in it is refused whole. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_WIRE_PROFILE_MAX 12288 /* the largest a rebuilt profile can be: an `about` of 2000 four-byte characters, and the rest */

/* Rebuild the profile object `json` into `out`: display_name (1 to 64 characters, required), blurb (160), about (2000, may
 * hold newlines), pronouns (32), color ({"colors":[one to five "#rrggbb"],"angle":0..360}) and avatar, banner, background as
 * {"hash","mime","size"} (a SHA-256 in hex, an image type we know, a size up to the image cap). Text is cleaned (no control
 * characters, no direction overrides, bad UTF-8 replaced). 0, or -1 if anything is wrong or it does not fit. */
int puddle_profile_wire_clean(const char *json, size_t len, char *out, size_t cap);

typedef struct {
    char hash[65];
    char mime[16];
    uint64_t size;
} puddle_media_ref;

/* The images a (cleaned) profile refers to: up to three. Returns how many, written to `refs`. */
size_t puddle_profile_wire_media(const char *json, size_t len, puddle_media_ref refs[3]);

#ifdef __cplusplus
}
#endif
#endif
