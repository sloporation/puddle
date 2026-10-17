#ifndef PUDDLE_OWNER_STREAMS_H
#define PUDDLE_OWNER_STREAMS_H

#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The owner's stream, block, invite and policy routes (owner_streams.c); routes.c adds them to the table. */
extern const puddle_route puddle_owner_stream_routes[];
extern const size_t puddle_owner_stream_routes_count;

#ifdef __cplusplus
}
#endif
#endif
