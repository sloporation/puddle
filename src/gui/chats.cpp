#include "chats.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUrlQuery>

namespace {

QString shortId(const QString &id)
{
    return id.left(8);
}

} // namespace

Chats::Chats(QObject *parent)
    : QObject(parent),
      people_({ "id", "name", "state", "note", "error", "certificate_changed", "avatar", "color", "updated_at", "address", "provisional" }),
      conversations_({ "user", "name", "avatar", "color", "stream", "last", "unread" }, nullptr, "user"),
      messages_({ "id", "sender", "senderName", "ts", "body", "outgoing", "edited", "deleted", "reply_to", "avatar", "tint", "grouped",
                  "dayStart", "replyName", "replyText", "replyMissing", "deliveryState", "deliveryReason", "deliveryNext" }),
      blocked_({ "id", "user", "name", "created_at" }, nullptr, "user"),
      profiles_({ "id", "display_name", "avatar", "default" })
{
    cacheDir_ = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/avatars";
    timer_.setInterval(2000);
    connect(&timer_, &QTimer::timeout, this, &Chats::refresh);
}

bool Chats::attach(const QUrl &base, const QString &tokenFile, const QString &certFile)
{
    QFile f(tokenFile);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray token = f.readAll().trimmed();
    if (!api_.configure(base, token, certFile))
        return false;
    timer_.start();
    refresh();
    return true;
}

void Chats::setConnected(bool c)
{
    if (connected_ == c)
        return;
    connected_ = c;
    emit connectedChanged();
}

void Chats::refresh()
{
    refreshPeople();
    refreshConversations();
    refreshMessages();
    refreshBlocked();
    if (!profilesLoaded_)
        refreshProfiles();
}

void Chats::refreshBlocked()
{
    if (busy_[3])
        return;
    busy_[3] = true;
    api_.get(QStringLiteral("/api/v1/user/block?limit=200"), [this](int status, const QJsonDocument &doc) {
        busy_[3] = false;
        if (status != 200 || !doc.isObject())
            return;
        QList<QJsonObject> rows;
        for (const QJsonValue &v : doc.object().value("blocks").toArray()) {
            QJsonObject b = v.toObject();
            const QString user = b.value("user").toString();
            b.insert("id", user);
            b.insert("name", nameOf(user));
            rows.append(b);
        }
        blocked_.setItems(rows);
    });
}

void Chats::refreshProfiles()
{
    if (busy_[4])
        return;
    busy_[4] = true;
    api_.get(QStringLiteral("/api/v1/profile"), [this](int status, const QJsonDocument &doc) {
        busy_[4] = false;
        if (status != 200 || !doc.isArray())
            return;
        QList<QJsonObject> rows;
        for (const QJsonValue &v : doc.array()) {
            QJsonObject p = v.toObject();
            p.insert("avatar", p.value("avatar").toObject().value("hash").toString());
            if (p.value("default").toBool()) {
                myName_ = p.value("display_name").toString();
                myAvatar_ = p.value("avatar").toString();
            }
            rows.append(p);
        }
        profilesLoaded_ = true;
        profiles_.setItems(rows);
        emit profilesChanged();
    });
}

QString Chats::avatarFor(const QString &hash)
{
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    if (!hex.match(hash).hasMatch())
        return QString();
    const QString path = cacheDir_ + "/" + hash;
    if (QFile::exists(path))
        return QUrl::fromLocalFile(path).toString();
    if (fetching_.contains(hash))
        return QString();
    fetching_.insert(hash);
    api_.getBytes(QStringLiteral("/api/v1/media?hash=") + hash, [this, hash, path](int status, const QByteArray &bytes) {
        fetching_.remove(hash);
        if (status != 200 || bytes.isEmpty() || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != hash.toLatin1())
            return; // not what was asked for: not shown
        QDir().mkpath(cacheDir_);
        QFile f(path + ".part");
        if (f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size()) {
            f.close();
            QFile::remove(path);
            f.rename(path);
            emit avatarReady(hash);
        }
    });
    return QString();
}

void Chats::copy(const QString &text)
{
    if (QClipboard *c = QGuiApplication::clipboard())
        c->setText(text);
}

QString Chats::grouped(const QString &id)
{
    QStringList parts;
    for (qsizetype i = 0; i < id.size(); i += 4)
        parts << id.mid(i, 4);
    return parts.join(' ');
}

QString Chats::shortCode(const QString &id)
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no I, O, 0, 1
    const QByteArray h = QCryptographicHash::hash(QByteArray("puddle-code\n") + id.toUtf8(), QCryptographicHash::Sha256);
    QString out;
    for (int i = 0; i < 8; i++) {
        if (i == 4)
            out += '-';
        out += QLatin1Char(alphabet[uchar(h[i]) % 32]);
    }
    return out;
}

void Chats::refreshPeople()
{
    if (busy_[0])
        return;
    busy_[0] = true;
    api_.get(QStringLiteral("/api/v1/stream?limit=200"), [this](int status, const QJsonDocument &doc) {
        busy_[0] = false;
        setConnected(status != 0);
        if (status != 200 || !doc.isObject())
            return;
        QList<QJsonObject> rows;
        int incoming = 0, friends = 0, pending = 0;
        QHash<QString, QString> names, avatars, colors;
        QSet<QString> seen;
        for (const QJsonValue &v : doc.object().value("streams").toArray()) {
            QJsonObject s = v.toObject();
            const QJsonObject profile = s.value("peer").toObject().value("profile").toObject();
            const QString id = s.value("id").toString();
            QString name = profile.value("display_name").toString();
            if (name.isEmpty() && s.value("provisional").toBool())
                name = s.value("address").toString(); // nobody has answered yet: all we know is where we asked
            if (name.isEmpty())
                name = shortId(id);
            names[id] = name;
            avatars[id] = profile.value("avatar").toObject().value("hash").toString();
            colors[id] = profile.value("color").toString();
            s.insert("name", name);
            s.insert("avatar", profile.value("avatar").toObject().value("hash").toString());
            s.insert("color", profile.value("color").toString());
            s.insert("note", s.value("message"));
            const QString state = s.value("state").toString();
            if (state == "active")
                friends++;
            else if (state == "pending_in" || state == "pending_out")
                pending++;
            if (state == "pending_in") {
                incoming++;
                seen.insert(id);
                if (peopleLoaded_ && !requested_.contains(id))
                    emit newRequest(id);
            }
            rows.append(s);
        }
        requested_ = seen;
        peopleLoaded_ = true;
        names_ = names;
        avatars_ = avatars;
        colors_ = colors;
        people_.setItems(rows);
        updateCurrentInfo();
        if (friends != friends_ || pending != pending_) {
            friends_ = friends;
            pending_ = pending;
            emit peopleCountsChanged();
        }
        if (incoming != incomingRequests_) {
            incomingRequests_ = incoming;
            emit incomingRequestsChanged();
        }
    });
}

void Chats::refreshConversations()
{
    if (busy_[1])
        return;
    busy_[1] = true;
    api_.get(QStringLiteral("/api/v1/conversations?limit=100"), [this](int status, const QJsonDocument &doc) {
        busy_[1] = false;
        if (status != 200 || !doc.isObject())
            return;
        QList<QJsonObject> rows;
        int total = 0;
        QHash<QString, int> unread;
        for (const QJsonValue &v : doc.object().value("conversations").toArray()) {
            QJsonObject c = v.toObject();
            const QString user = c.value("user").toString();
            c.insert("name", nameOf(user));
            c.insert("avatar", avatars_.value(user));
            c.insert("color", colors_.value(user));
            int n = c.value("unread").toInt();
            if (user == current_ && attentive_) {
                if (n > 0)
                    markRead(); // being looked at
                n = 0;
                c.insert("unread", 0);
            }
            unread[user] = n;
            if (conversationsLoaded_ && n > unread_.value(user))
                emit newIncoming(user, n);
            total += n;
            rows.append(c);
        }
        unread_ = unread;
        conversationsLoaded_ = true;
        conversations_.setItems(rows);
        if (total != unreadTotal_) {
            unreadTotal_ = total;
            emit unreadTotalChanged();
        }
    });
}

void Chats::markRead()
{
    if (current_.isEmpty() || marking_)
        return;
    marking_ = true;
    api_.post(QStringLiteral("/api/v1/dm/read"), QJsonObject{ { "user", current_ } }, [this](int, const QJsonDocument &) { marking_ = false; });
}

void Chats::refreshMessages()
{
    if (current_.isEmpty() || busy_[2])
        return;
    busy_[2] = true;
    const QString user = current_;
    api_.get(QStringLiteral("/api/v1/dm/messages?user=%1&limit=%2").arg(user).arg(messageLimit_),
             [this, user](int status, const QJsonDocument &doc) {
                 busy_[2] = false;
                 if (status != 200 || !doc.isObject() || user != current_)
                     return;
                 QList<QJsonObject> rows;
                 QHash<QString, QJsonObject> byId;
                 const QJsonArray list = doc.object().value("messages").toArray();
                 for (const QJsonValue &v : list)
                     byId.insert(v.toObject().value("id").toString(), v.toObject());
                 QJsonObject prev;
                 for (const QJsonValue &v : list) {
                     QJsonObject m = v.toObject();
                     const QString sender = m.value("sender").toString();
                     const bool mine = m.value("outgoing").toBool();
                     if (m.contains("delivery")) {
                         const QJsonObject d = m.value("delivery").toObject();
                         m.insert("deliveryState", d.value("state"));
                         m.insert("deliveryReason", d.value("reason"));
                         m.insert("deliveryNext", d.value("next_at"));
                     }
                     m.insert("senderName", mine ? tr("You") : nameOf(sender));
                     m.insert("avatar", mine ? myAvatar_ : avatars_.value(sender));
                     m.insert("tint", mine ? QString() : colors_.value(sender));
                     const qint64 ts = qint64(m.value("ts").toDouble());
                     const bool first = prev.isEmpty();
                     const bool newDay = first || QDateTime::fromMSecsSinceEpoch(ts).date() != QDateTime::fromMSecsSinceEpoch(qint64(prev.value("ts").toDouble())).date();
                     m.insert("dayStart", newDay);
                     m.insert("grouped", !first && !newDay && prev.value("sender") == m.value("sender") && !m.contains("reply_to") &&
                                             ts - qint64(prev.value("ts").toDouble()) < 7 * 60 * 1000);
                     if (m.contains("reply_to")) {
                         const QJsonObject r = byId.value(m.value("reply_to").toString());
                         if (r.isEmpty()) {
                             m.insert("replyMissing", true);
                         } else {
                             m.insert("replyName", r.value("outgoing").toBool() ? tr("You") : nameOf(r.value("sender").toString()));
                             m.insert("replyText", r.value("deleted").toBool() ? QString() : r.value("body").toString().simplified().left(100));
                         }
                     }
                     prev = m;
                     rows.append(m);
                 }
                 const bool older = !doc.object().value("next").isNull();
                 if (older != hasOlder_) {
                     hasOlder_ = older;
                     emit hasOlderChanged();
                 }
                 messages_.setItems(rows);
             });
}

void Chats::open(const QString &userId)
{
    if (current_ == userId)
        return;
    current_ = userId;
    messageLimit_ = 50;
    messages_.clear();
    hasOlder_ = false;
    updateCurrentInfo();
    emit currentChanged();
    if (userId.isEmpty())
        return;
    markRead();
    refreshMessages();
}

void Chats::updateCurrentInfo()
{
    QString name, avatar, color, state;
    bool cert = false;
    for (const QJsonObject &p : people_.items())
        if (p.value("id").toString() == current_) {
            name = p.value("name").toString();
            avatar = p.value("avatar").toString();
            color = p.value("color").toString();
            state = p.value("state").toString();
            cert = p.value("certificate_changed").toBool();
        }
    if (name.isEmpty() && !current_.isEmpty())
        name = shortId(current_);
    if (name == curName_ && avatar == curAvatar_ && color == curColor_ && state == curState_ && cert == curCert_)
        return;
    curName_ = name; curAvatar_ = avatar; curColor_ = color; curState_ = state; curCert_ = cert;
    emit currentInfoChanged();
}

void Chats::setAttentive(bool a)
{
    if (attentive_ == a)
        return;
    attentive_ = a;
    emit attentiveChanged();
    if (a)
        markRead(); // they are looking again: what arrived meanwhile is seen
}

void Chats::close()
{
    open(QString());
}

void Chats::loadOlder()
{
    if (current_.isEmpty() || messageLimit_ >= 1000)
        return;
    messageLimit_ += 50;
    refreshMessages();
}

void Chats::post(const QString &path, const QJsonObject &body, const QString &failure, bool andRefresh)
{
    api_.post(path, body, [this, failure, andRefresh](int status, const QJsonDocument &) {
        if (status == 0 || status >= 400)
            emit failed(failure);
        if (andRefresh)
            refresh();
    });
}

void Chats::retry()
{
    if (current_.isEmpty())
        return;
    post(QStringLiteral("/api/v1/dm/retry"), QJsonObject{ { "user", current_ } }, tr("There was nothing to send again."));
}

void Chats::send(const QString &text, const QString &replyTo)
{
    const QString body = text.trimmed();
    if (current_.isEmpty() || body.isEmpty())
        return;
    QJsonObject o{ { "to", current_ }, { "body", body } };
    if (!replyTo.isEmpty())
        o.insert("reply_to", replyTo);
    post(QStringLiteral("/api/v1/dm/send"), o, tr("The message could not be sent."));
}

void Chats::edit(const QString &messageId, const QString &text)
{
    post(QStringLiteral("/api/v1/dm/edit"), QJsonObject{ { "user", current_ }, { "message", messageId }, { "body", text.trimmed() } },
         tr("The message could not be changed."));
}

void Chats::remove(const QString &messageId)
{
    post(QStringLiteral("/api/v1/dm/delete"), QJsonObject{ { "user", current_ }, { "message", messageId } }, tr("The message could not be deleted."));
}

void Chats::accept(const QString &u) { post(QStringLiteral("/api/v1/stream/accept"), QJsonObject{ { "stream", u } }, tr("Could not accept.")); }
void Chats::decline(const QString &u) { post(QStringLiteral("/api/v1/stream/decline"), QJsonObject{ { "stream", u } }, tr("Could not decline.")); }
void Chats::cancel(const QString &u) { post(QStringLiteral("/api/v1/stream/delete"), QJsonObject{ { "stream", u } }, tr("Could not remove.")); }
void Chats::block(const QString &u) { post(QStringLiteral("/api/v1/user/block"), QJsonObject{ { "user", u } }, tr("Could not block.")); }
void Chats::unblock(const QString &u) { post(QStringLiteral("/api/v1/user/unblock"), QJsonObject{ { "user", u } }, tr("Could not unblock.")); }
void Chats::trust(const QString &u) { post(QStringLiteral("/api/v1/stream/trust"), QJsonObject{ { "stream", u } }, tr("Could not trust the new certificate.")); }

void Chats::request(const QString &link, const QString &note, const QString &profile)
{
    QJsonObject o{ { "target", link } };
    if (!note.isEmpty())
        o.insert("message", note);
    if (!profile.isEmpty())
        o.insert("profile", profile);
    post(QStringLiteral("/api/v1/stream/request"), o, tr("The request could not be sent. Check the link."));
}

QString Chats::nameOf(const QString &userId) const
{
    return names_.value(userId, shortId(userId));
}
