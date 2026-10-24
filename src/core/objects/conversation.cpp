#include "objects/conversation.hpp"

#include "error.hpp"
#include "groupflow.h"
#include "identity.h"
#include "objects/stream.hpp"
#include "objects/user.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

namespace puddle {

namespace {

constexpr size_t kPageBudget = 23000; // bytes of answer a page of messages may take, out of what a handler may write

std::string ownId(puddle_ctx *ctx)
{
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(ctx->identity, id);
    return id;
}

std::string previewOf(const char *body)
{
    std::string out;
    size_t chars = 0;
    for (const unsigned char *p = (const unsigned char *)body; *p && chars < 120;) {
        size_t k = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4; // the body is valid UTF-8
        if (*p == '\n' || *p == '\t')
            out += ' ';
        else
            out.append((const char *)p, k);
        p += k;
        chars++;
    }
    return out;
}

// the events of a chat, newest `n`, with its messages worked out; freed with the object
struct ChatTail {
    puddle_event_list events{};
    puddle_view_msg *view = nullptr;
    size_t m = 0;
    ~ChatTail()
    {
        puddle_event_list_free(&events);
        if (view)
            puddle_view_free(view, m);
    }
    void load(puddle_ctx *ctx, const std::string &chat, size_t n)
    {
        char err[300];
        puddle_event_list_free(&events);
        if (view)
            puddle_view_free(view, m);
        view = nullptr;
        m = 0;
        if (puddle_store_tail(ctx->store, chat.c_str(), n, &events, err, sizeof err) != 0)
            throw Error::internal(err);
        if (events.count && puddle_chat_view(events.items, events.count, &view, &m) != 0)
            throw Error::internal();
    }
};

} // namespace

std::optional<Conversation> Conversation::get(puddle_ctx *ctx, const std::string &id)
{
    if (auto g = Group::get(ctx, id)) {
        Conversation c(ctx, id, Kind::Group);
        c.chat_ = id;
        c.name_ = g->name();
        c.sortTs_ = g->record().created_ms; // a new group sorts by when it was made
        c.group_ = std::move(*g);
        c.summarise();
        return c;
    }
    puddle_stream st;
    if (puddle_streams_get(ctx->streams, id.c_str(), &st) != 0)
        return std::nullopt;
    Conversation c(ctx, id, Kind::Dm);
    c.stream_ = st.state;
    puddle_stream_free(&st);
    char chat[PUDDLE_USER_ID_LEN + 1];
    if (puddle_dm_chat_id(ownId(ctx).c_str(), id.c_str(), chat) != 0)
        return std::nullopt;
    c.chat_ = chat;
    c.summarise();
    return c;
}

std::vector<Conversation> Conversation::all(puddle_ctx *ctx)
{
    std::vector<Conversation> out;
    std::string me = ownId(ctx);
    for (Stream &s : Stream::all(ctx)) {
        Conversation c(ctx, s.id(), Kind::Dm);
        c.stream_ = s.state();
        char chat[PUDDLE_USER_ID_LEN + 1];
        if (puddle_dm_chat_id(me.c_str(), s.id().c_str(), chat) != 0)
            continue;
        c.chat_ = chat;
        c.summarise();
        if (c.last_) // a friend we have never talked to is not a conversation yet
            out.push_back(std::move(c));
    }
    for (Group &g : Group::all(ctx)) {
        if (g.mine() != PUDDLE_GROUP_IN)
            continue;
        Conversation c(ctx, g.id(), Kind::Group);
        c.chat_ = g.id();
        c.name_ = g.name();
        c.sortTs_ = g.record().created_ms;
        c.group_ = std::move(g);
        c.summarise();
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const Conversation &a, const Conversation &b) {
        if (a.sortTs_ != b.sortTs_)
            return a.sortTs_ > b.sortTs_;
        return a.id_ < b.id_;
    });
    return out;
}

void Conversation::summarise()
{
    std::string me = ownId(ctx_);
    ChatTail t;
    t.load(ctx_, chat_, 200);
    if (!t.m)
        return;
    const puddle_view_msg &last = t.view[t.m - 1];
    last_ = Last{ last.id, last.sender, previewOf(last.body ? last.body : ""), last.ts, last.deleted != 0 };
    sortTs_ = last.ts;
    int64_t marker = puddle_streams_read_marker(ctx_->streams, id_.c_str());
    for (size_t k = 0; k < t.m; k++)
        if (me != t.view[k].sender && !t.view[k].deleted && t.view[k].rts > marker && unread_ < 99)
            unread_++;
}

Message Conversation::send(const std::string &text, const std::string &replyTo)
{
    std::string me = ownId(ctx_);
    if (kind_ == Kind::Dm) {
        if (id_ == me)
            throw Error::badRequest("cannot_message_yourself");
        if (*stream_ != PUDDLE_STREAM_ACTIVE || puddle_streams_is_blocked(ctx_->streams, id_.c_str()))
            throw Error(403, "no_stream");
    } else if (!group_ || group_->mine() != PUDDLE_GROUP_IN) {
        throw Error::conflict("not_in_group");
    }
    char event[PUDDLE_EVENT_MAX_BYTES], id[PUDDLE_ID_HEX + 1];
    int64_t ts = puddle_event_next_ts(puddle_ctx_now_ms(ctx_));
    size_t el = puddle_message_build(chat_.c_str(), me.c_str(), ts, text.data(), text.size(), replyTo.empty() ? NULL : replyTo.c_str(), event,
                                     sizeof event);
    if (!el)
        throw Error::badRequest(); // a reply reference that is not an id, or a message too large to send
    if (kind_ == Kind::Dm) {
        commitDirectEvent(ctx_, id_, chat_, event, el, id);
    } else {
        const char *code = nullptr;
        int sent;
        int st = puddle_group_send_event(ctx_, chat_.c_str(), event, el, id, &sent, &code);
        if (st && code && !strcmp(code, "not_queued"))
            throw Error(500, "not_queued", "it was saved here but could not be queued to send");
        if (st)
            throw Error(st, code ? code : "internal");
    }
    puddle_view_msg v;
    memset(&v, 0, sizeof v);
    snprintf(v.id, sizeof v.id, "%s", id);
    snprintf(v.sender, sizeof v.sender, "%s", me.c_str());
    snprintf(v.ref, sizeof v.ref, "%s", replyTo.c_str());
    v.ts = ts;
    v.rts = puddle_ctx_now_ms(ctx_);
    v.body = const_cast<char *>(text.c_str()); // read, not kept: fromView copies it
    return Message::fromView(v, id_, chat_);
}

Conversation Conversation::markRead()
{
    char err[200];
    if (puddle_streams_set_read_marker(ctx_->streams, id_.c_str(), puddle_ctx_now_ms(ctx_), err, sizeof err) != 0)
        throw Error::internal(err);
    auto c = get(ctx_, id_);
    if (!c)
        throw Error::notFound();
    return std::move(*c);
}

Conversation Conversation::retry()
{
    if (!ctx_->outbox)
        throw Error::notFound();
    std::vector<std::string> peers;
    if (kind_ == Kind::Dm) {
        peers.push_back(id_);
    } else {
        const puddle_group &g = group_->record();
        for (size_t k = 0; k < g.nmembers; k++)
            if (g.members[k].state != PUDDLE_GM_LEFT)
                peers.push_back(g.members[k].id);
    }
    puddle_ob_item *items;
    size_t n;
    if (puddle_outbox_list(ctx_->outbox, &items, &n) != 0)
        throw Error::internal();
    int did = 0;
    for (const std::string &peer : peers) {
        for (size_t i = 0; i < n; i++)
            if (peer == items[i].peer && !strcmp(items[i].kind, "events") && items[i].body &&
                (kind_ == Kind::Dm || strstr(items[i].body, chat_.c_str())) && puddle_outbox_requeue(ctx_->outbox, items[i].seq) == 0)
                did++;
        did += puddle_outbox_retry_now(ctx_->outbox, peer.c_str());
    }
    puddle_outbox_list_free(items, n);
    if (!did)
        throw Error::notFound("nothing_to_retry");
    auto c = get(ctx_, id_);
    if (!c)
        throw Error::notFound();
    return std::move(*c);
}

void Conversation::write(puddle_jw *w, PartialUsers &people) const
{
    std::string me = ownId(ctx_);
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id_.c_str());
    puddle_jw_str(w, "kind", kind_ == Kind::Dm ? "dm" : "group");
    puddle_jw_str(w, "name", kind_ == Kind::Dm ? people.name(id_).c_str() : name_.c_str());
    puddle_jw_num(w, "unread", unread_);
    puddle_jw_num(w, "sort_ts", sortTs_);
    if (last_) {
        puddle_jw_key(w, "last_message");
        puddle_jw_object(w);
        puddle_jw_str(w, "id", last_->id.c_str());
        puddle_jw_key(w, "author");
        people.write(w, last_->sender);
        puddle_jw_num(w, "ts", last_->ts);
        puddle_jw_flag(w, "outgoing", last_->sender == me);
        if (last_->deleted)
            puddle_jw_flag(w, "deleted", 1);
        else
            puddle_jw_str(w, "preview", last_->preview.c_str());
        puddle_jw_end_object(w);
    }
    if (kind_ == Kind::Dm) {
        puddle_jw_key(w, "recipient");
        people.write(w, id_);
        puddle_jw_str(w, "stream", puddle_stream_state_name(*stream_));
    } else {
        puddle_jw_key(w, "group");
        group_->writeSummary(w, people);
    }
    puddle_jw_end_object(w);
}

void Conversation::writeMessages(puddle_jw *w, PartialUsers &people, size_t limit, size_t offset) const
{
    // Events, not messages, are what the store counts, and edits and redactions are events too: take a window of the newest
    // and widen it until it holds enough messages (or all there is). What was done to a message is newer than it, so it is
    // in the window whenever the message is.
    size_t want = offset + limit + 1, n = want * 2 + 16;
    ChatTail t;
    for (;;) {
        t.load(ctx_, chat_, n);
        bool full = t.events.count >= n;
        if (t.m >= want || !full || n > (1u << 22))
            break;
        n *= 8;
    }
    // newest first, up to the limit and what fits; then written oldest first. What fits is measured, each message written once on its own,
    // so a page never stops short of what its `next` says it holds.
    Deliveries deliveries(ctx_, kind_ == Kind::Dm ? id_ : std::string());
    const size_t room = w->cap > w->len + 3000 ? w->cap - w->len - 3000 : 0; // what is left once the notices and the page's own text are allowed for
    std::vector<char> scratch(kPageBudget);
    size_t end = t.m > offset ? t.m - offset : 0, start = end, used = 0;
    std::vector<Message> page;
    while (start > 0 && end - start < limit) {
        Message m = Message::fromView(t.view[start - 1], id_, chat_);
        puddle_jw one;
        puddle_jw_init(&one, scratch.data(), scratch.size());
        m.write(&one, ctx_, people, &deliveries);
        size_t cost = one.err ? scratch.size() : one.len + 1;
        if (used + cost > room && start < end)
            break;
        used += cost;
        start--;
        page.push_back(std::move(m));
    }
    std::reverse(page.begin(), page.end());
    if (kind_ == Kind::Group) {
        // what happened to the group between the messages shown: from the oldest of them (or the start of the chat, when the
        // page reaches it) up to the first message of the next page
        int64_t lower = start > 0 ? (start < end ? t.view[start].ts : INT64_MAX) : INT64_MIN;
        int64_t upper = end < t.m ? t.view[end].ts : INT64_MAX;
        for (size_t i = 0; i < t.events.count; i++)
            if (auto notice = Message::fromNotice(t.events.items[i], id_); notice && notice->ts() >= lower && notice->ts() < upper)
                page.push_back(std::move(*notice));
        std::stable_sort(page.begin(), page.end(), [](const Message &a, const Message &b) {
            if (a.ts() != b.ts())
                return a.ts() < b.ts();
            return a.id() < b.id();
        });
    }
    puddle_jw_object(w);
    puddle_jw_key(w, "items");
    puddle_jw_array(w);
    for (const Message &m : page)
        if (m.type() == Message::Type::Default || w->len + 600 <= w->cap) // a notice that does not fit is left out; it counts for no offset
            m.write(w, ctx_, people, &deliveries);
    puddle_jw_end_array(w);
    puddle_jw_key(w, "next");
    if (start > 0)
        puddle_jw_uint(w, offset + (end - start));
    else
        puddle_jw_null(w);
    puddle_jw_end_object(w);
}

} // namespace puddle
