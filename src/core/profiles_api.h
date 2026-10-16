#ifndef PUDDLE_PROFILES_API_H
#define PUDDLE_PROFILES_API_H

/* How the profile we show a friend, and theirs, travel (docs/profiles.md; roadmap 4.3): the owner lists their profiles and
 * chooses which one each stream sees; the choice is pushed to the peer (`POST /stream/v1/profile`); a friend's images are
 * fetched from them by hash (`GET /stream/v1/media`) and kept; and what we have of ours is served to the owner's app
 * (`GET /api/v1/media`). Only an open stream sees any of it. */

#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const puddle_route puddle_profile_routes[];
extern const size_t puddle_profile_routes_count;

/* Tell `peer` which profile it now sees (queued, retried while it is away). 0 or -1. */
int puddle_profile_push(puddle_ctx *ctx, const char *peer);

/* Queue fetches for the images in `peer`'s profile that we do not hold. Only for an open stream. Returns how many were queued. */
int puddle_media_want(puddle_ctx *ctx, const char *peer);

/* The outbox's `done` hook for what this module sends: a fetched image is checked and kept; trouble is noted on the stream. */
void puddle_profiles_done(puddle_ctx *ctx, const puddle_ob_item *item, puddle_ob_outcome outcome, const puddle_net_reply *reply);

#ifdef __cplusplus
}
#endif
#endif
