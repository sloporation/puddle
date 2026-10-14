#ifndef PUDDLE_PROFILE_H
#define PUDDLE_PROFILE_H

/* Profiles: how the owner appears to others. Several can be defined (work, friends, family...) and
 * each stream is shown one of them. They live in YAML files named by the `profiles:` setting
 * (docs/profiles.md). Images are copied into <data_dir>/media/<sha256> when loaded. */

#include "config.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_PROFILE_ID_MAX 32
#define PUDDLE_MAX_PROFILES 32
#define PUDDLE_MAX_GRADIENT_STOPS 5

/* PLACEHOLDER limit so a mistaken path can't copy something huge into the data directory.
 * The real policy is an open question (docs/discuss-later.md). */
#define PUDDLE_IMAGE_MAX_BYTES (25u * 1024 * 1024)

typedef struct {
    char hash[65]; /* SHA-256 of the file, hex; empty string = no image */
    char mime[16]; /* image/png, image/jpeg, image/gif or image/webp */
    uint64_t size;
} puddle_media;

typedef enum { PUDDLE_COLOR_NONE, PUDDLE_COLOR_SOLID, PUDDLE_COLOR_GRADIENT } puddle_color_kind;

typedef struct {
    puddle_color_kind kind;
    char stops[PUDDLE_MAX_GRADIENT_STOPS][8]; /* "#rrggbb"; one stop for SOLID */
    int nstops;
    int angle; /* degrees, GRADIENT only */
} puddle_color;

typedef struct {
    char id[PUDDLE_PROFILE_ID_MAX + 1]; /* private to the owner; never sent to anyone */
    char *display_name;
    char *blurb;    /* short, may be NULL */
    char *about;    /* long, may be NULL */
    char *pronouns; /* may be NULL */
    puddle_color color;
    puddle_media avatar, banner, background;
} puddle_profile;

typedef struct {
    puddle_profile *items; /* items[0] is the default */
    size_t count;
} puddle_profiles;

/* Read every file in cfg->profile_files (or make one starter profile named after the OS user if none
 * are configured). Mistakes in the files are errors with file:line; an image that is missing,
 * oversized or not an image is dropped with a warning. 0 or -1. */
int puddle_profiles_load(puddle_profiles *out, const puddle_config *cfg, char *err, size_t errlen);
void puddle_profiles_free(puddle_profiles *p);
const puddle_profile *puddle_profiles_find(const puddle_profiles *p, const char *id);

/* Which profile each peer (stream) is shown. Stored in <data_dir>/state/stream-profiles.tsv. */
typedef struct puddle_assignments puddle_assignments;
puddle_assignments *puddle_assignments_open(const char *data_dir, char *err, size_t errlen);
void puddle_assignments_close(puddle_assignments *a);
/* 0 found (id copied to `out`), 1 nothing assigned */
int puddle_assignments_get(puddle_assignments *a, const char *peer, char out[PUDDLE_PROFILE_ID_MAX + 1]);
/* Assign `profile_id` to `peer`; NULL clears it (the peer gets the default again). 0 or -1. */
int puddle_assignments_set(puddle_assignments *a, const char *peer, const char *profile_id,
                           char *err, size_t errlen);

/* The profile to show `peer`: the assigned one if it still exists, otherwise the default. */
const puddle_profile *puddle_profile_for_peer(const puddle_profiles *p, puddle_assignments *a,
                                              const char *peer);

#ifdef __cplusplus
}
#endif
#endif
