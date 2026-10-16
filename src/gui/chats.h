#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <memory>

#include "apiclient.h"
#include "jsonlistmodel.h"

// What the window shows, kept fresh from the owner API of our own Puddle: the people (friends and requests), the
// conversations with their unread counts, and the messages of the chat that is open. All of it is read over the
// API, so the same code serves a Puddle running elsewhere.
class Chats : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *people READ people CONSTANT)               // every stream: roles below
    Q_PROPERTY(QObject *conversations READ conversations CONSTANT)  // newest first
    Q_PROPERTY(QObject *messages READ messages CONSTANT)            // the open chat, oldest first
    Q_PROPERTY(QObject *blocked READ blocked CONSTANT)              // people we blocked: user, name, created_at
    Q_PROPERTY(QObject *profiles READ profiles CONSTANT)            // our own profiles: id, display_name, avatar, default
    Q_PROPERTY(QString myName READ myName NOTIFY profilesChanged)   // the default profile's display name
    Q_PROPERTY(QString myAvatar READ myAvatar NOTIFY profilesChanged)
    Q_PROPERTY(QString current READ current NOTIFY currentChanged)  // the user id of the open chat, "" for none
    Q_PROPERTY(int unreadTotal READ unreadTotal NOTIFY unreadTotalChanged)
    Q_PROPERTY(int incomingRequests READ incomingRequests NOTIFY incomingRequestsChanged)
    Q_PROPERTY(int friendCount READ friendCount NOTIFY peopleCountsChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY peopleCountsChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentInfoChanged)
    Q_PROPERTY(QString currentAvatar READ currentAvatar NOTIFY currentInfoChanged)
    Q_PROPERTY(QString currentColor READ currentColor NOTIFY currentInfoChanged)
    Q_PROPERTY(QString currentState READ currentState NOTIFY currentInfoChanged)   // stream state of the open chat
    Q_PROPERTY(bool currentCertChanged READ currentCertChanged NOTIFY currentInfoChanged)
    Q_PROPERTY(bool hasOlder READ hasOlder NOTIFY hasOlderChanged)                 // more history than is loaded
    Q_PROPERTY(bool attentive READ attentive WRITE setAttentive NOTIFY attentiveChanged) // someone is looking at the window
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
public:
    explicit Chats(QObject *parent = nullptr);

    QObject *people() { return &people_; }
    QObject *conversations() { return &conversations_; }
    QObject *messages() { return &messages_; }
    QObject *blocked() { return &blocked_; }
    QObject *profiles() { return &profiles_; }
    QString myName() const { return myName_; }
    QString myAvatar() const { return myAvatar_; }
    QString current() const { return current_; }
    int friendCount() const { return friends_; }
    int pendingCount() const { return pending_; }
    QString currentName() const { return curName_; }
    QString currentAvatar() const { return curAvatar_; }
    QString currentColor() const { return curColor_; }
    QString currentState() const { return curState_; }
    bool currentCertChanged() const { return curCert_; }
    bool hasOlder() const { return hasOlder_; }
    bool attentive() const { return attentive_; }
    void setAttentive(bool a);
    int unreadTotal() const { return unreadTotal_; }
    int incomingRequests() const { return incomingRequests_; }
    bool connected() const { return connected_; }
    ApiClient *api() { return &api_; }

    // Start talking to the Puddle at `base`, reading its token and certificate from the files.
    bool attach(const QUrl &base, const QString &tokenFile, const QString &certFile);
    void setInterval(int ms) { timer_.setInterval(ms); }

    // Fetch everything now (also runs on a timer).
    Q_INVOKABLE void refresh();

    Q_INVOKABLE void open(const QString &userId);   // show this chat and mark it read
    Q_INVOKABLE void close();
    Q_INVOKABLE void loadOlder();
    Q_INVOKABLE QString userAt(int row) const { return conversations_.at(row).value("user").toString(); }
    Q_INVOKABLE QVariantMap messageAt(int row) const { return messages_.at(row).toVariantMap(); }
    Q_INVOKABLE void retry();                           // try again to deliver what is stuck in the open chat
    Q_INVOKABLE void send(const QString &text, const QString &replyTo = QString());
    Q_INVOKABLE void edit(const QString &messageId, const QString &text);
    Q_INVOKABLE void remove(const QString &messageId);

    Q_INVOKABLE void accept(const QString &userId);
    Q_INVOKABLE void decline(const QString &userId);
    Q_INVOKABLE void cancel(const QString &userId);      // withdraw our own request, or end a stream we no longer want
    Q_INVOKABLE void block(const QString &userId);
    Q_INVOKABLE void unblock(const QString &userId);
    Q_INVOKABLE void trust(const QString &userId);       // accept their changed certificate
    Q_INVOKABLE void request(const QString &link, const QString &note, const QString &profile = QString());

    // The picture with this hash as a file: its URL, or "" while it is being fetched (then avatarReady is emitted).
    Q_INVOKABLE QString avatarFor(const QString &hash);
    void setCacheDir(const QString &dir) { cacheDir_ = dir; }

    // A short code derived from an id, e.g. "K7QM-2XPD", to compare with the person out loud. And the id in groups of four.
    Q_INVOKABLE void copy(const QString &text);
    Q_INVOKABLE static QString shortCode(const QString &userId);
    Q_INVOKABLE static QString grouped(const QString &userId);

    // What the person is called here: their display name, else a short form of the id.
    Q_INVOKABLE QString nameOf(const QString &userId) const;

signals:
    void currentChanged();
    void unreadTotalChanged();
    void incomingRequestsChanged();
    void connectedChanged();
    void failed(const QString &what);                    // something the owner asked for did not work
    void newIncoming(const QString &userId, int unread); // for notifications: a chat has more unread than before
    void newRequest(const QString &userId);
    void avatarReady(const QString &hash);
    void profilesChanged();
    void currentInfoChanged();
    void hasOlderChanged();
    void attentiveChanged();
    void peopleCountsChanged();

private:
    void refreshPeople();
    void refreshConversations();
    void refreshMessages();
    void updateCurrentInfo();
    void refreshBlocked();
    void refreshProfiles();
    void post(const QString &path, const QJsonObject &body, const QString &failure, bool andRefresh = true);
    void setConnected(bool c);
    void markRead();

    ApiClient api_;
    QTimer timer_;
    JsonListModel people_, conversations_, messages_, blocked_, profiles_;
    QString curName_, curAvatar_, curColor_, curState_;
    bool curCert_ = false, hasOlder_ = false, attentive_ = true;
    QString myName_, myAvatar_, cacheDir_;
    QSet<QString> fetching_;
    QString current_;
    QHash<QString, QString> names_, avatars_, colors_;
    QHash<QString, int> unread_;
    QSet<QString> requested_;
    bool peopleLoaded_ = false, conversationsLoaded_ = false;
    int friends_ = 0, pending_ = 0;
    int unreadTotal_ = 0, incomingRequests_ = 0;
    int messageLimit_ = 50;
    bool connected_ = true; // until a request fails, so the banner does not flash at start-up
    bool marking_ = false, profilesLoaded_ = false;
    bool busy_[5] = { false, false, false, false, false };
};
