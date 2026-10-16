#ifndef PUDDLE_MESSAGES_H
#define PUDDLE_MESSAGES_H

#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Direct messages: the owner's send and read routes, and the peer route that delivers events (messages.c). */
extern const puddle_route puddle_message_routes[];
extern const size_t puddle_message_routes_count;

#ifdef __cplusplus
}
#endif
#endif
