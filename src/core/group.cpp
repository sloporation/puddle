#include "group.h"

#include "access.h"
#include "encoding.h"
#include "fsutil.h"
#include "json.h"
#include "log.h"

#include <errno.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdarg.h>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TS_MIN 946684800000LL
#define TS_MAX 4102444800000LL
#define RECORD_VERSION 1
#define FILE_MAX (1u << 20)

/* ---- ids ---- */

int puddle_group_nonce(char out[33])
{
    unsigned char raw[16];
    static const char hex[] = "0123456789abcdef";
    if (RAND_bytes(raw, sizeof raw) != 1)
        return -1;
    for (int i = 0; i < 16; i++) {
        out[2 * i] = hex[raw[i] >> 4];
        out[2 * i + 1] = hex[raw[i] & 15];
    }
    out[32] = '\0';
    return 0;
}

static int is_nonce(const char *s)
{
    unsigned char raw[16];
    return s && strlen(s) == 32 && puddle_hex_decode(s, raw, 16) == 0;
}

int puddle_group_chat_id(const char *creator, int64_t ts, const char *nonce, char out[PUDDLE_USER_ID_LEN + 1])
{
    char text[200];
    unsigned char h[32];
    if (!puddle_id_valid(creator) || !is_nonce(nonce) || ts < TS_MIN || ts >= TS_MAX)
        return -1;
    int n = snprintf(text, sizeof text, "group\n%s\n%lld\n%s", creator, (long long)ts, nonce);
    SHA256((const unsigned char *)text, (size_t)n, h);
    return puddle_base32_encode(h, 32, out, PUDDLE_USER_ID_LEN + 1) == PUDDLE_USER_ID_LEN ? 0 : -1;
}

int puddle_group_name_ok(const char *name)
{
    char clean[PUDDLE_GROUP_NAME_MAX_BYTES + 1];
    size_t chars;
    if (!name || !*name)
        return 0;
    /* what the cleaner would keep must be exactly what we were given, so a name is never silently changed on the way */
    return puddle_clean_text(name, strlen(name), PUDDLE_GROUP_NAME_MAX_CHARS, 0, clean, sizeof clean, &chars) == 0 && chars > 0 &&
           !strcmp(clean, name);
}

/* ---- events ---- */

static size_t build(puddle_ge_type type, const char *chat, const char *sender, int64_t ts, const char *name, const char *nonce, const char *member,
                    char *out, size_t cap)
{
    static const char *const names[] = { "chat.create", "chat.rename", "member.invite", "member.join", "member.leave" };
    puddle_jw w;
    puddle_jw_init(&w, out, cap);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "type", names[type]);
    puddle_jw_str(&w, "chat", chat);
    puddle_jw_str(&w, "sender", sender);
    puddle_jw_num(&w, "ts", ts);
    if (type == PUDDLE_GE_CREATE) {
        puddle_jw_str(&w, "name", name);
        puddle_jw_str(&w, "nonce", nonce);
    } else if (type == PUDDLE_GE_RENAME) {
        puddle_jw_str(&w, "body", name);
    } else if (type == PUDDLE_GE_INVITE) {
        puddle_jw_str(&w, "member", member);
    }
    puddle_jw_end_object(&w);
    return puddle_jw_finish(&w);
}

size_t puddle_group_create_build(const char *creator, int64_t ts, const char *nonce, const char *name, char chat_out[PUDDLE_USER_ID_LEN + 1],
                                 char *out, size_t cap)
{
    if (!puddle_group_name_ok(name) || puddle_group_chat_id(creator, ts, nonce, chat_out) != 0)
        return 0;
    return build(PUDDLE_GE_CREATE, chat_out, creator, ts, name, nonce, NULL, out, cap);
}

size_t puddle_group_rename_build(const char *chat, const char *sender, int64_t ts, const char *name, char *out, size_t cap)
{
    if (!puddle_group_name_ok(name) || !puddle_id_valid(chat) || !puddle_id_valid(sender))
        return 0;
    return build(PUDDLE_GE_RENAME, chat, sender, ts, name, NULL, NULL, out, cap);
}

size_t puddle_group_invite_build(const char *chat, const char *sender, int64_t ts, const char *invitee, char *out, size_t cap)
{
    if (!puddle_id_valid(chat) || !puddle_id_valid(sender) || !puddle_id_valid(invitee))
        return 0;
    return build(PUDDLE_GE_INVITE, chat, sender, ts, NULL, NULL, invitee, out, cap);
}

size_t puddle_group_join_build(const char *chat, const char *sender, int64_t ts, char *out, size_t cap)
{
    if (!puddle_id_valid(chat) || !puddle_id_valid(sender))
        return 0;
    return build(PUDDLE_GE_JOIN, chat, sender, ts, NULL, NULL, NULL, out, cap);
}

size_t puddle_group_leave_build(const char *chat, const char *sender, int64_t ts, char *out, size_t cap)
{
    if (!puddle_id_valid(chat) || !puddle_id_valid(sender))
        return 0;
    return build(PUDDLE_GE_LEAVE, chat, sender, ts, NULL, NULL, NULL, out, cap);
}

static int member_str(const char *e, size_t n, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_member(e, n, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

int puddle_group_event_parse(const char *event, size_t len, puddle_gevent *g, const char **why)
{
    memset(g, 0, sizeof *g);
    *why = "malformed";
    if (len == 0 || len > 16 * 1024 || !puddle_json_valid_object(event, len))
        return -1;
    char type[20];
    if (member_str(event, len, "type", type, sizeof type) != 0) {
        /* a type longer than any of ours is not one of ours */
        puddle_span tv;
        if (puddle_json_member(event, len, "type", &tv) == 0 && tv.len >= 2)
            *why = "unsupported_type";
        return -1;
    }
    if (!strcmp(type, "chat.create")) g->type = PUDDLE_GE_CREATE;
    else if (!strcmp(type, "chat.rename")) g->type = PUDDLE_GE_RENAME;
    else if (!strcmp(type, "member.invite")) g->type = PUDDLE_GE_INVITE;
    else if (!strcmp(type, "member.join")) g->type = PUDDLE_GE_JOIN;
    else if (!strcmp(type, "member.leave")) g->type = PUDDLE_GE_LEAVE;
    else {
        *why = "unsupported_type";
        return -1;
    }
    if (member_str(event, len, "chat", g->chat, sizeof g->chat) != 0 || member_str(event, len, "sender", g->sender, sizeof g->sender) != 0 ||
        !puddle_id_valid(g->chat) || !puddle_id_valid(g->sender))
        return -1;
    puddle_span v;
    if (puddle_json_member(event, len, "ts", &v) != 0 || puddle_json_int64(v, &g->ts) != 0)
        return -1;
    if (g->ts < TS_MIN || g->ts >= TS_MAX) {
        *why = "bad_time";
        return -1;
    }
    int has_body = puddle_json_member(event, len, "body", &v) == 0;
    int has_name = puddle_json_member(event, len, "name", &v) == 0;
    int has_nonce = puddle_json_member(event, len, "nonce", &v) == 0;
    int has_member = puddle_json_member(event, len, "member", &v) == 0;
    int has_ref = puddle_json_member(event, len, "ref", &v) == 0;
    if (has_ref)
        return -1; /* these events point at nothing */
    switch (g->type) {
    case PUDDLE_GE_CREATE:
        if (has_body || has_member || !has_name || !has_nonce || member_str(event, len, "name", g->name, sizeof g->name) != 0 ||
            member_str(event, len, "nonce", g->nonce, sizeof g->nonce) != 0 || !puddle_group_name_ok(g->name) || !is_nonce(g->nonce))
            return -1;
        {
            char want[PUDDLE_USER_ID_LEN + 1];
            if (puddle_group_chat_id(g->sender, g->ts, g->nonce, want) != 0 || strcmp(want, g->chat) != 0) {
                *why = "bad_group_id"; /* the id has to be the one its creator, time and nonce make */
                return -1;
            }
        }
        break;
    case PUDDLE_GE_RENAME:
        if (!has_body || has_name || has_nonce || has_member || member_str(event, len, "body", g->name, sizeof g->name) != 0 ||
            !puddle_group_name_ok(g->name))
            return -1;
        break;
    case PUDDLE_GE_INVITE:
        if (has_body || has_name || has_nonce || !has_member || member_str(event, len, "member", g->member, sizeof g->member) != 0 ||
            !puddle_id_valid(g->member) || !strcmp(g->member, g->sender))
            return -1;
        break;
    case PUDDLE_GE_JOIN:
    case PUDDLE_GE_LEAVE:
        if (has_body || has_name || has_nonce || has_member)
            return -1;
        break;
    }
    *why = NULL;
    return 0;
}

/* ---- the group ---- */

const char *puddle_group_my_name(puddle_group_my m)
{
    return m == PUDDLE_GROUP_INVITED ? "invited" : m == PUDDLE_GROUP_IN ? "in" : m == PUDDLE_GROUP_LEFT ? "left" : "declined";
}

const char *puddle_gm_state_name(puddle_gm_state s)
{
    return s == PUDDLE_GM_INVITED ? "invited" : s == PUDDLE_GM_MEMBER ? "member" : "left";
}

void puddle_group_free(puddle_group *g)
{
    free(g->members);
    g->members = NULL;
    g->nmembers = 0;
}

puddle_gmember *puddle_group_member(const puddle_group *g, const char *id)
{
    for (size_t i = 0; i < g->nmembers; i++)
        if (!strcmp(g->members[i].id, id))
            return &g->members[i];
    return NULL;
}

size_t puddle_group_count(const puddle_group *g, puddle_gm_state state)
{
    size_t n = 0;
    for (size_t i = 0; i < g->nmembers; i++)
        n += g->members[i].state == state;
    return n;
}

static puddle_gmember *add_member(puddle_group *g, const char *id)
{
    puddle_gmember *n = (puddle_gmember *)realloc(g->members, (g->nmembers + 1) * sizeof *n);
    if (!n)
        return NULL;
    g->members = n;
    puddle_gmember *m = &g->members[g->nmembers++];
    memset(m, 0, sizeof *m);
    snprintf(m->id, sizeof m->id, "%s", id);
    return m;
}

int puddle_group_init_created(puddle_group *g, const puddle_gevent *create, int64_t now_ms)
{
    memset(g, 0, sizeof *g);
    snprintf(g->id, sizeof g->id, "%s", create->chat);
    snprintf(g->name, sizeof g->name, "%s", create->name);
    g->name_ts = create->ts;
    snprintf(g->creator, sizeof g->creator, "%s", create->sender);
    g->created_ms = g->updated_ms = now_ms;
    g->my = PUDDLE_GROUP_IN;
    puddle_gmember *m = add_member(g, create->sender);
    if (!m)
        return -1;
    m->state = PUDDLE_GM_MEMBER;
    m->ts = create->ts;
    return 0;
}

static size_t live_count(const puddle_group *g)
{
    return puddle_group_count(g, PUDDLE_GM_MEMBER) + puddle_group_count(g, PUDDLE_GM_INVITED);
}

int puddle_group_apply(puddle_group *g, const puddle_gevent *e, const char **why)
{
    *why = NULL;
    puddle_gmember *sender = puddle_group_member(g, e->sender);
    switch (e->type) {
    case PUDDLE_GE_CREATE:
        *why = "bad_event";
        return -1;
    case PUDDLE_GE_RENAME:
        if (!sender || sender->state != PUDDLE_GM_MEMBER) {
            *why = "not_a_member";
            return -1;
        }
        if (e->ts < g->name_ts || (e->ts == g->name_ts && strcmp(e->name, g->name) <= 0))
            return 1; /* an older name, or the same moment with a name that loses the tie: every Puddle breaks it the same way */
        snprintf(g->name, sizeof g->name, "%s", e->name);
        g->name_ts = e->ts;
        return 0;
    case PUDDLE_GE_INVITE: {
        if (!sender || sender->state != PUDDLE_GM_MEMBER) {
            *why = "not_a_member";
            return -1;
        }
        puddle_gmember *m = puddle_group_member(g, e->member);
        if (m && m->state != PUDDLE_GM_LEFT)
            return 1; /* already invited or in */
        if (m && e->ts <= m->ts)
            return 1; /* a stale invitation: they have left since */
        if (live_count(g) >= PUDDLE_GROUP_MAX_MEMBERS) {
            *why = "group_full";
            return -1;
        }
        if (!m && !(m = add_member(g, e->member))) {
            *why = "bad_event";
            return -1;
        }
        m->state = PUDDLE_GM_INVITED;
        snprintf(m->by, sizeof m->by, "%s", e->sender);
        m->ts = e->ts;
        return 0;
    }
    case PUDDLE_GE_JOIN:
        if (!sender || sender->state != PUDDLE_GM_INVITED) {
            if (sender && sender->state == PUDDLE_GM_MEMBER)
                return 1; /* already in */
            *why = "not_invited";
            return -1;
        }
        if (e->ts <= sender->ts) {
            *why = "not_invited"; /* an answer cannot be older than the question */
            return -1;
        }
        sender->state = PUDDLE_GM_MEMBER;
        sender->ts = e->ts;
        return 0;
    case PUDDLE_GE_LEAVE:
        if (!sender || sender->state == PUDDLE_GM_LEFT) {
            if (sender)
                return 1;
            *why = "not_a_member";
            return -1;
        }
        if (e->ts <= sender->ts)
            return 1; /* older than their last change: stale */
        sender->state = PUDDLE_GM_LEFT;
        sender->ts = e->ts;
        return 0;
    }
    *why = "bad_event";
    return -1;
}

/* ---- the store ---- */

struct puddle_groups {
    std::mutex mu;
    char dir[1100];
};

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static void record_path(const puddle_groups *s, const char *id, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.json", s->dir, id);
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = (char *)malloc(FILE_MAX + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, FILE_MAX + 1, f);
    int bad = ferror(f) || n > FILE_MAX;
    fclose(f);
    if (bad) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static char *encode(const puddle_group *g, size_t *out_len)
{
    size_t cap = 2048 + g->nmembers * 600 + strlen(g->name) * 6 + strlen(g->create_event) * 6 + strlen(g->join_event) * 6;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_num(&w, "v", RECORD_VERSION);
    puddle_jw_str(&w, "id", g->id);
    puddle_jw_str(&w, "name", g->name);
    puddle_jw_num(&w, "name_ts", g->name_ts);
    puddle_jw_str(&w, "creator", g->creator);
    puddle_jw_num(&w, "created", g->created_ms);
    puddle_jw_num(&w, "updated", g->updated_ms);
    puddle_jw_str(&w, "my", puddle_group_my_name(g->my));
    if (g->join_event[0]) {
        puddle_jw_str(&w, "join", g->join_event);
        puddle_jw_str(&w, "join_sig", g->join_sig);
    }
    if (g->create_event[0]) {
        puddle_jw_str(&w, "create", g->create_event);
        puddle_jw_str(&w, "create_sig", g->create_sig);
    }
    if (g->invited_by[0])
        puddle_jw_str(&w, "invited_by", g->invited_by);
    puddle_jw_key(&w, "members");
    puddle_jw_array(&w);
    for (size_t i = 0; i < g->nmembers; i++) {
        puddle_jw_object(&w);
        puddle_jw_str(&w, "id", g->members[i].id);
        puddle_jw_str(&w, "state", puddle_gm_state_name(g->members[i].state));
        if (g->members[i].by[0])
            puddle_jw_str(&w, "by", g->members[i].by);
        if (g->members[i].address[0])
            puddle_jw_str(&w, "address", g->members[i].address);
        if (g->members[i].told)
            puddle_jw_flag(&w, "told", 1);
        puddle_jw_num(&w, "ts", g->members[i].ts);
        puddle_jw_end_object(&w);
    }
    puddle_jw_end_array(&w);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    if (!n) {
        free(buf);
        return NULL;
    }
    *out_len = n;
    return buf;
}

static int member_int(const char *json, size_t len, const char *key, int64_t *out)
{
    puddle_span v;
    return puddle_json_member(json, len, key, &v) == 0 && puddle_json_int64(v, out) == 0 ? 0 : -1;
}

static int decode(const char *json, size_t len, const char *expect_id, puddle_group *g)
{
    memset(g, 0, sizeof *g);
    char tmp[PUDDLE_GROUP_NAME_MAX_BYTES + 1];
    int64_t v;
    if (!puddle_json_valid_object(json, len) || member_int(json, len, "v", &v) != 0 || v != RECORD_VERSION)
        return -1;
    if (member_str(json, len, "id", g->id, sizeof g->id) != 0 || strcmp(g->id, expect_id) != 0)
        return -1;
    if (member_str(json, len, "name", g->name, sizeof g->name) != 0 || member_str(json, len, "creator", g->creator, sizeof g->creator) != 0 ||
        !puddle_id_valid(g->creator) || member_int(json, len, "name_ts", &g->name_ts) != 0 || member_int(json, len, "created", &g->created_ms) != 0 ||
        member_int(json, len, "updated", &g->updated_ms) != 0)
        return -1;
    if (member_str(json, len, "my", tmp, sizeof tmp) != 0)
        return -1;
    if (!strcmp(tmp, "invited")) g->my = PUDDLE_GROUP_INVITED;
    else if (!strcmp(tmp, "in")) g->my = PUDDLE_GROUP_IN;
    else if (!strcmp(tmp, "left")) g->my = PUDDLE_GROUP_LEFT;
    else if (!strcmp(tmp, "declined")) g->my = PUDDLE_GROUP_DECLINED;
    else return -1;
    puddle_span jv;
    int jrc = puddle_json_member(json, len, "join", &jv);
    if (jrc < 0 || (jrc == 0 && (puddle_json_string(jv, g->join_event, sizeof g->join_event) != 0 ||
                                 member_str(json, len, "join_sig", g->join_sig, sizeof g->join_sig) != 0)))
        return -1;
    puddle_span cv;
    int crc = puddle_json_member(json, len, "create", &cv);
    if (crc < 0 || (crc == 0 && (puddle_json_string(cv, g->create_event, sizeof g->create_event) != 0 ||
                                 member_str(json, len, "create_sig", g->create_sig, sizeof g->create_sig) != 0)))
        return -1;
    puddle_span iv;
    int irc = puddle_json_member(json, len, "invited_by", &iv);
    if (irc < 0 || (irc == 0 && (puddle_json_string(iv, g->invited_by, sizeof g->invited_by) != 0 || !puddle_id_valid(g->invited_by))))
        return -1;
    puddle_span arr, item;
    if (puddle_json_member(json, len, "members", &arr) != 0)
        return -1;
    size_t cursor = 0;
    int r;
    while ((r = puddle_json_array_next(arr, &cursor, &item)) == 1) {
        char st[16];
        puddle_gmember *m = NULL;
        char id[PUDDLE_USER_ID_LEN + 1];
        if (member_str(item.p, item.len, "id", id, sizeof id) != 0 || !puddle_id_valid(id) || puddle_group_member(g, id) ||
            member_str(item.p, item.len, "state", st, sizeof st) != 0 || g->nmembers >= PUDDLE_GROUP_MAX_MEMBERS * 4 || !(m = add_member(g, id))) {
            puddle_group_free(g);
            return -1;
        }
        m->state = !strcmp(st, "member") ? PUDDLE_GM_MEMBER : !strcmp(st, "invited") ? PUDDLE_GM_INVITED : PUDDLE_GM_LEFT;
        if (strcmp(st, "member") && strcmp(st, "invited") && strcmp(st, "left")) {
            puddle_group_free(g);
            return -1;
        }
        puddle_span bv, av, tv;
        int trc = puddle_json_member(item.p, item.len, "told", &tv);
        if (trc < 0) {
            puddle_group_free(g);
            return -1;
        }
        m->told = trc == 0 && tv.len == 4 && !memcmp(tv.p, "true", 4);
        int arc = puddle_json_member(item.p, item.len, "address", &av);
        if (arc < 0 || (arc == 0 && puddle_json_string(av, m->address, sizeof m->address) != 0)) {
            puddle_group_free(g);
            return -1;
        }
        int brc = puddle_json_member(item.p, item.len, "by", &bv);
        if (brc < 0 || (brc == 0 && (puddle_json_string(bv, m->by, sizeof m->by) != 0 || !puddle_id_valid(m->by))) ||
            member_int(item.p, item.len, "ts", &m->ts) != 0) {
            puddle_group_free(g);
            return -1;
        }
    }
    if (r < 0) {
        puddle_group_free(g);
        return -1;
    }
    return 0;
}

puddle_groups *puddle_groups_open(const char *data_dir, char *err, size_t errlen)
{
    puddle_groups *s = new (std::nothrow) puddle_groups();
    if (!s) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    snprintf(s->dir, sizeof s->dir, "%s/state/groups", data_dir);
    if (puddle_mkdir_p(s->dir) != 0) {
        seterr(err, errlen, "cannot create %s: %s", s->dir, strerror(errno));
        delete s;
        return NULL;
    }
    return s;
}

void puddle_groups_close(puddle_groups *s)
{
    if (!s)
        return;
    delete s;
}

int puddle_groups_get(puddle_groups *s, const char *id_in, puddle_group *out)
{
    if (!puddle_id_valid(id_in))
        return -1;
    char id[PUDDLE_USER_ID_LEN + 1], path[1200];
    memcpy(id, id_in, sizeof id);
    size_t len;
    record_path(s, id, path, sizeof path);
    s->mu.lock();
    char *text = read_all(path, &len);
    s->mu.unlock();
    if (!text)
        return 1;
    int rc = decode(text, len, id, out);
    free(text);
    return rc == 0 ? 0 : 1;
}

static int put_locked(puddle_groups *s, const puddle_group *g, char *err, size_t errlen)
{
    size_t n;
    char *text = encode(g, &n);
    if (!text) {
        seterr(err, errlen, "cannot encode the group record");
        return -1;
    }
    char path[1200];
    record_path(s, g->id, path, sizeof path);
    int rc = puddle_write_file_atomic(s->dir, path, text, n);
    free(text);
    if (rc != 0) {
        seterr(err, errlen, "cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int puddle_groups_put(puddle_groups *s, const puddle_group *g, char *err, size_t errlen)
{
    if (!puddle_id_valid(g->id)) {
        seterr(err, errlen, "not a valid group id");
        return -1;
    }
    s->mu.lock();
    int rc = put_locked(s, g, err, errlen);
    s->mu.unlock();
    return rc;
}

int puddle_groups_update(puddle_groups *s, const char *id_in, puddle_group_update_fn fn, void *user, char *err, size_t errlen)
{
    if (!puddle_id_valid(id_in)) {
        seterr(err, errlen, "not a valid group id");
        return -1;
    }
    char id[PUDDLE_USER_ID_LEN + 1], path[1200];
    memcpy(id, id_in, sizeof id);
    size_t len = 0;
    record_path(s, id, path, sizeof path);
    s->mu.lock();
    int rc = -1;
    puddle_group g;
    char *text = read_all(path, &len);
    int exists = text && decode(text, len, id, &g) == 0;
    free(text);
    if (!exists) {
        memset(&g, 0, sizeof g);
        snprintf(g.id, sizeof g.id, "%s", id);
    }
    int r = fn(user, &g, exists);
    if (r > 0)
        rc = r;
    else if (r == 0)
        rc = put_locked(s, &g, err, errlen) == 0 ? 0 : -1;
    else
        seterr(err, errlen, "not changed");
    puddle_group_free(&g);
    s->mu.unlock();
    return rc;
}

int puddle_groups_list(puddle_groups *s, puddle_group **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    puddle_dir *d = puddle_dir_open(s->dir);
    if (!d)
        return -1;
    size_t cap = 0, n = 0;
    puddle_group *list = NULL;
    s->mu.lock();
    const char *entry;
    while ((entry = puddle_dir_next(d))) {
        size_t nl = strlen(entry);
        if (nl != PUDDLE_USER_ID_LEN + 5 || strcmp(entry + PUDDLE_USER_ID_LEN, ".json") != 0)
            continue;
        char id[PUDDLE_USER_ID_LEN + 1], path[1200];
        memcpy(id, entry, PUDDLE_USER_ID_LEN);
        id[PUDDLE_USER_ID_LEN] = '\0';
        if (!puddle_id_valid(id))
            continue;
        size_t len;
        record_path(s, id, path, sizeof path);
        char *text = read_all(path, &len);
        if (!text)
            continue;
        puddle_group g;
        int ok = decode(text, len, id, &g) == 0;
        free(text);
        if (!ok) {
            puddle_log(PUDDLE_LOG_WARN, "skipping damaged group file %s", path);
            continue;
        }
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 8;
            puddle_group *nl2 = (puddle_group *)realloc(list, nc * sizeof *list);
            if (!nl2) {
                puddle_group_free(&g);
                break;
            }
            list = nl2;
            cap = nc;
        }
        list[n++] = g;
    }
    s->mu.unlock();
    puddle_dir_close(d);
    *out = list;
    *count = n;
    return 0;
}

void puddle_group_list_free(puddle_group *list, size_t count)
{
    for (size_t i = 0; i < count; i++)
        puddle_group_free(&list[i]);
    free(list);
}

size_t puddle_groups_pending(puddle_groups *s)
{
    puddle_group *l;
    size_t n, c = 0;
    if (puddle_groups_list(s, &l, &n) != 0)
        return 0;
    for (size_t i = 0; i < n; i++)
        c += l[i].my == PUDDLE_GROUP_INVITED;
    puddle_group_list_free(l, n);
    return c;
}
