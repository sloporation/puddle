#ifndef PUDDLE_GROUPAPI_H
#define PUDDLE_GROUPAPI_H

#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Group chats: the owner's routes and the routes other Puddles use (groupapi.c). */
extern const puddle_route puddle_group_routes[];
extern const size_t puddle_group_routes_count;

#ifdef __cplusplus
}
#endif
#endif
