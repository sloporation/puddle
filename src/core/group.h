#ifndef PUDDLE_GROUP_H
#define PUDDLE_GROUP_H

/* Group chats (docs/groups.md): the events that make one, and what a Puddle remembers about the groups it is in.
 *
 * A group is a chat whose id is made from its creator, a time and a random nonce, and written into its first event,
 * `chat.create`, which every member can check. Messages, edits and redactions are the same events as in a direct message,
 * with the group's id as the chat. Five more events change the group:
 *
 *   chat.create   the creator; `name`, `nonce`
 *   chat.rename   any member; `body` is the new name
 *   member.invite any member, naming the invitee in `member`
 *   member.join   the invitee, saying yes
 *   member.leave  any member or invitee, for themselves
 *
 * There is no one in charge. What each Puddle believes about the members is worked out from these by the same rules, so
 * the answer is the same on every Puddle that has seen the same events, in whatever order they came. */

#include "identity.h"
#include "link.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_GROUP_NAME_MAX_CHARS 100
#define PUDDLE_GROUP_NAME_MAX_BYTES (PUDDLE_GROUP_NAME_MAX_CHARS * 4)
#define PUDDLE_GROUP_MAX_MEMBERS 50     /* members and the invited together */
#define PUDDLE_GROUP_MAX_PENDING_IN 50  /* invitations waiting for the owner */
#define PUDDLE_GROUP_CREATE_MAX 1024    /* bytes of a create event (a name of 100 characters is at most a few hundred) */

/* The id of a group: base32(SHA-256("group\n" creator "\n" ts "\n" nonce)). 0, or -1 for bad input. */
int puddle_group_chat_id(const char *creator, int64_t ts, const char *nonce, char out[PUDDLE_USER_ID_LEN + 1]);
/* 32 random hex characters for a nonce. */
int puddle_group_nonce(char out[33]);

typedef enum { PUDDLE_GE_CREATE, PUDDLE_GE_RENAME, PUDDLE_GE_INVITE, PUDDLE_GE_JOIN, PUDDLE_GE_LEAVE } puddle_ge_type;

typedef struct {
    puddle_ge_type type;
    char chat[PUDDLE_USER_ID_LEN + 1];
    char sender[PUDDLE_USER_ID_LEN + 1];
    int64_t ts;
    char name[PUDDLE_GROUP_NAME_MAX_BYTES + 1]; /* create, rename */
    char nonce[33];                             /* create */
    char member[PUDDLE_USER_ID_LEN + 1];        /* invite: the invitee */
} puddle_gevent;

/* Is `name` a group name: 1 to 100 characters, one line of valid text? */
int puddle_group_name_ok(const char *name);

/* Build events (compact JSON, NUL-terminated) into `out`; the length, or 0 if it does not fit or the input is not acceptable. */
size_t puddle_group_create_build(const char *creator, int64_t ts, const char *nonce, const char *name, char chat_out[PUDDLE_USER_ID_LEN + 1],
                                 char *out, size_t cap);
size_t puddle_group_rename_build(const char *chat, const char *sender, int64_t ts, const char *name, char *out, size_t cap);
size_t puddle_group_invite_build(const char *chat, const char *sender, int64_t ts, const char *invitee, char *out, size_t cap);
size_t puddle_group_join_build(const char *chat, const char *sender, int64_t ts, char *out, size_t cap);
size_t puddle_group_leave_build(const char *chat, const char *sender, int64_t ts, char *out, size_t cap);

/* Read a group event, strictly. 0 (and *why NULL), or -1 with a reason code in *why (`unsupported_type` for anything that is not
 * one of the five, such as a message, so a caller can go on to read it as a chat event). A create is also checked: its chat is
 * the id its creator, time and nonce make. */
int puddle_group_event_parse(const char *event, size_t len, puddle_gevent *g, const char **why);

/* ---- what we remember about a group ---- */

typedef enum { PUDDLE_GM_INVITED, PUDDLE_GM_MEMBER, PUDDLE_GM_LEFT } puddle_gm_state;
typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_gm_state state;
    char by[PUDDLE_USER_ID_LEN + 1]; /* who invited them; "" for the creator */
    int64_t ts;                      /* of the event that put them in this state */
    char address[PUDDLE_ADDR_MAX];   /* where the one who invited us said to reach them, "" if not told; only kept to connect once */
    int told;                        /* we have sent them our own member.join (they cannot learn it from anyone else) */
} puddle_gmember;

/* Where we stand in a group. */
typedef enum {
    PUDDLE_GROUP_INVITED,  /* someone invited us; the owner has not answered */
    PUDDLE_GROUP_IN,       /* we are a member */
    PUDDLE_GROUP_LEFT,     /* we left */
    PUDDLE_GROUP_DECLINED  /* we said no to the invitation */
} puddle_group_my;

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    char name[PUDDLE_GROUP_NAME_MAX_BYTES + 1];
    int64_t name_ts;
    char creator[PUDDLE_USER_ID_LEN + 1];
    int64_t created_ms, updated_ms;
    puddle_group_my my;
    char invited_by[PUDDLE_USER_ID_LEN + 1]; /* when my == INVITED */
    char create_event[PUDDLE_GROUP_CREATE_MAX + 1]; /* the chat.create, to hand to those we invite */
    char create_sig[180];
    char join_event[PUDDLE_GROUP_CREATE_MAX + 1];  /* our own member.join, kept to show the members we connect to (we were invited, not asked) */
    char join_sig[180];
    puddle_gmember *members;                 /* everyone we know of, us included */
    size_t nmembers;
} puddle_group;

const char *puddle_group_my_name(puddle_group_my m);
const char *puddle_gm_state_name(puddle_gm_state s);

void puddle_group_free(puddle_group *g);
puddle_gmember *puddle_group_member(const puddle_group *g, const char *id); /* NULL if not listed */
size_t puddle_group_count(const puddle_group *g, puddle_gm_state state);

/* A group as it begins for its creator: just the creator, as a member. */
int puddle_group_init_created(puddle_group *g, const puddle_gevent *create, int64_t now_ms);

/* Fold one membership event into `g` (never a create). The rules, so every Puddle agrees:
 *  - rename: from a member; the newer one wins
 *  - invite: from a member, naming someone not already in; a leaver may be invited again by a later event
 *  - join: only from someone invited, and only the answer to an invitation older than it
 *  - leave: from a member or an invited person, for themselves; an older one than their last change is stale
 *  - never more than 50 members and invited together
 * Returns 0 if it changed `g`, 1 if it was fine but changed nothing (already so, or stale), -1 if it is not allowed, with a reason
 * code in *why (`not_a_member`, `not_invited`, `group_full`, `bad_event`). */
int puddle_group_apply(puddle_group *g, const puddle_gevent *e, const char **why);

/* ---- the store ---- */

typedef struct puddle_groups puddle_groups;

puddle_groups *puddle_groups_open(const char *data_dir, char *err, size_t errlen); /* <data_dir>/state/groups/ */
void puddle_groups_close(puddle_groups *s);

/* 0 found (free with puddle_group_free), 1 no such group, -1 not a valid id. */
int puddle_groups_get(puddle_groups *s, const char *id, puddle_group *out);
/* Store a record (new or replacing). 0 or -1. */
int puddle_groups_put(puddle_groups *s, const puddle_group *g, char *err, size_t errlen);
/* Change one group's record without losing anyone else's change to it, like puddle_streams_update: `fn` sees the stored record
 * (`exists` 1) or an empty one (`exists` 0) and returns 0 to store it, >0 to store nothing (returned), <0 to fail. */
typedef int (*puddle_group_update_fn)(void *user, puddle_group *g, int exists);
int puddle_groups_update(puddle_groups *s, const char *id, puddle_group_update_fn fn, void *user, char *err, size_t errlen);
/* Every group. Free with puddle_group_list_free. */
int puddle_groups_list(puddle_groups *s, puddle_group **out, size_t *count);
void puddle_group_list_free(puddle_group *list, size_t count);
/* How many groups are waiting for us to answer an invitation. */
size_t puddle_groups_pending(puddle_groups *s);

#ifdef __cplusplus
}
#endif
#endif
