#ifndef PUDDLE_API_H
#define PUDDLE_API_H

/* The owner API (docs/objects.md): one route table per kind of object. Each handler gets an object from the path,
 * asks it to do something, and answers with the object. routes.cpp adds these tables to the Puddle's routes. */

#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const puddle_route puddle_user_routes[];          /* api/users.cpp: users, blocks */
extern const size_t puddle_user_routes_count;
extern const puddle_route puddle_stream_api_routes[];    /* api/streams.cpp: streams, policy, invites */
extern const size_t puddle_stream_api_routes_count;
extern const puddle_route puddle_conversation_routes[];  /* api/conversations.cpp: conversations, messages */
extern const size_t puddle_conversation_routes_count;
extern const puddle_route puddle_group_api_routes[];     /* api/groups.cpp: groups */
extern const size_t puddle_group_api_routes_count;

#ifdef __cplusplus
}
#endif
#endif
