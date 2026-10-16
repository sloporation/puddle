#ifndef PUDDLE_MESSAGES_H
#define PUDDLE_MESSAGES_H

#include "message.h"
#include "router.h"
#include "store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Direct messages: the owner's send and read routes, and the peer route that delivers events (messages.c). */
extern const puddle_route puddle_message_routes[];
extern const size_t puddle_message_routes_count;

/* Taking a delivered event (messages.c), the same for every kind of chat: the envelope, the id and the signature are checked
 * here; the chat's own rules are the policy's. `judge` reads the event and says why it may not be taken (NULL = fine), before
 * the signature is checked; `commit` stores it (after the signature has been) and says why not (NULL = done). */
typedef struct {
    const char *(*judge)(void *user, const char *event, size_t len);
    const char *(*commit)(void *user, const char *event, size_t len, const char *sigtext, char id_inout[PUDDLE_ID_HEX + 1], int *dup);
    void *user;
} puddle_event_policy;

const char *puddle_take_signed(const puddle_event_policy *pol, const unsigned char sender_pub[32], const char *sender_kid, puddle_span ev,
                               char id_out[PUDDLE_ID_HEX + 1], int *dup);

/* Take every event of a delivery (`evs` is its "events" array) under `pol`, and answer with what became of each. Refuses a delivery
 * with too many events whole. */
int puddle_process_delivery(const puddle_call *c, puddle_jw *w, puddle_span evs, const puddle_event_policy *pol, const unsigned char sender_pub[32],
                            const char *sender_kid);

/* Keep a delivered event in `chat`'s log: NULL, or the reason it could not be stored. */
const char *puddle_store_delivered(puddle_ctx *ctx, const char *chat, const char *sender, const char *event, size_t len, const char *sigtext,
                                   char id_out[PUDDLE_ID_HEX + 1], int *dup);

/* Reading a chat (messages.c), for direct messages and groups alike. */
typedef void (*puddle_page_extra)(void *user, puddle_jw *w, const puddle_event *events, size_t count, int64_t since_ts);
int puddle_chat_page(const puddle_call *c, puddle_jw *w, const char *chat, const char *me, const char *delivery_peer, size_t limit, size_t offset,
                     puddle_page_extra extra, void *extra_user);
/* One message as the app sees it, with what became of its delivery (to `delivery_peer`, or to whoever it went to when that is ""). */
void puddle_message_json(const puddle_call *c, puddle_jw *w, const puddle_view_msg *m, const char *chat, const char *me, const char *delivery_peer);
/* The message `target` of `chat` as a view: 1 found (body malloc'd), 0 not there, -1 error. */
int puddle_chat_find(const puddle_call *c, const char *chat, const char *target, puddle_view_msg *out);
/* `limit` (1 to max_limit) and `offset` from a query, plain digits: 0, or -1 if not. Unset values are left alone. */
int puddle_page_params(const char *limit_text, const char *offset_text, size_t max_limit, size_t *limit, size_t *offset);

/* The delivery route for a group chat (groupapi.c), called when the chat is not the direct message chat of the sender. */
int puddle_group_events_post(const puddle_call *c, puddle_jw *w, const char *chat);

#ifdef __cplusplus
}
#endif
#endif
