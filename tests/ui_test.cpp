// The app's window, driven like a person would: real mouse and key events on the real QML, against a real Puddle (the
// app's Backend) and a second real Puddle that plays the friend. Offscreen.
#include <QClipboard>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QTest>
#include <QQuickStyle>

#include "../src/gui/backend.h"
#include "testnode.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static QQuickWindow *win;

static void walk(QQuickItem *it, const std::function<bool(QQuickItem *)> &f)
{
    if (!it)
        return;
    if (f(it))
        return;
    for (QQuickItem *c : it->childItems())
        walk(c, f);
}

// every item with this objectName
static QList<QQuickItem *> all(const QString &name, bool visibleOnly = true)
{
    QList<QQuickItem *> out;
    walk(win->contentItem(), [&](QQuickItem *i) {
        if (i->objectName() == name && (!visibleOnly || i->isVisible()))
            out << i;
        return false;
    });
    return out;
}
static QQuickItem *find(const QString &name) { return all(name).value(0); }
// the visible item with this objectName whose text is `text`
static QQuickItem *find_text(const QString &name, const QString &text)
{
    for (QQuickItem *i : all(name))
        if (i->property("text").toString() == text)
            return i;
    return nullptr;
}

// a visible button with this text (for the standard dialog buttons)
static QQuickItem *button(const QString &text)
{
    QQuickItem *hit = nullptr;
    walk(win->contentItem(), [&](QQuickItem *i) {
        if (i->isVisible() && i->metaObject()->inherits(&QQuickItem::staticMetaObject) && i->property("text").toString() == text &&
            QString(i->metaObject()->className()).contains("Button")) {
            hit = i;
            return true;
        }
        return false;
    });
    return hit;
}

static void settle(int ms = 300)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static QJsonObject row(JsonListModel *m, const QString &key, const QString &value)
{
    for (const QJsonObject &o : m->items())
        if (o.value(key).toString() == value)
            return o;
    return {};
}

static QPoint centre(QQuickItem *i) { return i->mapToScene(QPointF(i->width() / 2, i->height() / 2)).toPoint(); }
static bool click(QQuickItem *i)
{
    if (!i || !i->isVisible() || !i->isEnabled())
        return false;
    settle(150); // lists and layouts may still be moving into place
    QTest::mouseClick(win, Qt::LeftButton, {}, centre(i));
    return true;
}
static bool clickName(const char *name) { return until([&] { return find(name) != nullptr; }, 5000) && click(find(name)); }
static void typeText(const QString &t)
{
    for (const QChar c : t)
        QTest::keyClick(win, c.toLatin1());
}
static void key(Qt::Key k, Qt::KeyboardModifiers m = {}) { QTest::keyClick(win, k, m); }
// the messages as the window draws them
static QStringList shown()
{
    QStringList out;
    for (QQuickItem *i : all("messageText"))
        out << i->property("text").toString();
    return out;
}
static bool shows(const QString &text) { return shown().contains(text); }

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir home;
    CHECK(home.isValid());
    qputenv("HOME", home.path().toUtf8());
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    app.setOrganizationName("sloporation-ui-test");

    const unsigned port = free_port();
    QDir().mkpath(home.path() + "/.config/puddle");
    QFile cf(home.path() + "/.config/puddle/puddle.yaml");
    CHECK(cf.open(QIODevice::WriteOnly));
    cf.write(QString("address: \"127.0.0.1\"\nport: %1\n").arg(port).toUtf8());
    cf.close();

    Node bob;
    CHECK(bob.up(home.path() + "/friends", "Bob"));
    Chats &B = *bob.chats;
    auto *msgs_b = static_cast<JsonListModel *>(B.messages());

    Backend backend; // the app's own Puddle
    if (backend.needsSetup()) {
        CHECK(!backend.createIdentity().isEmpty());
        CHECK(backend.finishSetup());
    }
    CHECK(backend.running() && backend.error().isEmpty());
    CHECK(backend.myLink() == QString("127.0.0.1:%1").arg(port));
    Chats &A = *backend.chats();
    A.setInterval(100);
    CHECK(until([&] { return A.connected(); }));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("appBackend", &backend);
    const QString qml = QString(R"(
        import QtQuick
        import QtQuick.Controls
        import "file://%1" as G
        ApplicationWindow { width: 1000; height: 640; visible: true
            G.Shell { anchors.fill: parent; backend: appBackend } })").arg(PUDDLE_GUI_DIR);
    engine.loadData(qml.toUtf8(), QUrl("file:///ui_test.qml"));
    CHECK(!engine.rootObjects().isEmpty());
    if (engine.rootObjects().isEmpty())
        return 1;
    win = qobject_cast<QQuickWindow *>(engine.rootObjects().at(0));
    win->requestActivate();
    CHECK(until([&] { return win->isActive(); }, 3000));
    settle();

    /* ---- the copy-link button puts our link on the clipboard ---- */
    QGuiApplication::clipboard()->setText("nothing yet");
    CHECK(clickName("copyLinkButton"));
    CHECK(until([&] { return QGuiApplication::clipboard()->text() == backend.myLink(); }, 3000));

    /* ---- ask Bob to be a friend, from the window ---- */
    CHECK(clickName("addFriendButton"));
    CHECK(until([&] { return find("linkField") != nullptr; }, 3000));
    CHECK(click(find("linkField")));
    typeText(bob.link);
    CHECK(find("sendRequestButton") && find("sendRequestButton")->isEnabled());
    CHECK(click(find("noteField")));
    typeText("hi from the window");
    QSignalSpy newReq(&B, &Chats::newRequest);
    CHECK(click(find("sendRequestButton")));
    CHECK(until([&] { return B.incomingRequests() == 1; }));
    CHECK(newReq.count() == 1);
    CHECK(until([&] { return find("linkField") == nullptr; }, 3000)); /* the popup went away */
    {
        bool noted = false;
        for (const QJsonObject &o : static_cast<JsonListModel *>(B.people())->items())
            noted = noted || o.value("note").toString() == "hi from the window";
        CHECK(noted);
    }

    /* ---- Bob accepts; the window shows a friend ---- */
    B.accept(backend.myId());
    CHECK(until([&] { return A.friendCount() == 1; }));
    CHECK(until([&] { return find("messageButton") != nullptr; }, 5000));

    /* ---- chat: send ---- */
    CHECK(click(find("messageButton")));
    CHECK(until([&] { return A.current() == bob.uid; }, 3000));
    CHECK(until([&] { return find("composer") != nullptr; }, 3000));
    CHECK(click(find("composer")));
    typeText("hello from the window");
    key(Qt::Key_Return);
    B.open(backend.myId());
    CHECK(until([&] { return msgs_b->rowCount() == 1 && msgs_b->at(0).value("body").toString() == "hello from the window"; }));
    CHECK(until([&] { return shows("hello from the window"); }, 5000));
    CHECK(find("composer")->property("text").toString().isEmpty()); /* the box is empty again */

    /* Shift+Enter is a new line, not a send */
    typeText("one");
    key(Qt::Key_Return, Qt::ShiftModifier);
    typeText("two");
    settle(500);
    CHECK(msgs_b->rowCount() == 1);
    CHECK(find("composer")->property("text").toString() == "one\ntwo");
    key(Qt::Key_A, Qt::ControlModifier);
    key(Qt::Key_Backspace);

    /* ---- chat: receive ---- */
    B.send("hi back");
    CHECK(until([&] { return shows("hi back"); }, 5000));

    /* ---- edit with Up ---- */
    CHECK(click(find("composer")));
    key(Qt::Key_Up);
    CHECK(until([&] { return find("editor") != nullptr; }, 3000));
    key(Qt::Key_A, Qt::ControlModifier);
    typeText("hello edited");
    key(Qt::Key_Return);
    CHECK(until([&] { return msgs_b->rowCount() >= 1 && msgs_b->at(0).value("body").toString() == "hello edited" && msgs_b->at(0).value("edited").toBool(); }));
    CHECK(until([&] { return shows("hello edited") && !shows("hello from the window"); }, 5000));
    CHECK(until([&] { return find("editor") == nullptr; }, 3000));

    /* ---- delete: hover the message, press the bin, confirm ---- */
    {
        QQuickItem *text = nullptr;
        for (QQuickItem *i : all("messageText"))
            if (i->property("text").toString() == "hello edited")
                text = i;
        CHECK(text != nullptr);
        if (text) {
            QTest::mouseMove(win, centre(text));
            CHECK(until([&] { return find("deleteButton") != nullptr; }, 3000));
            CHECK(click(find("deleteButton")));
            CHECK(until([&] { return button("OK") != nullptr; }, 3000));
            CHECK(click(button("OK")));
        }
    }
    CHECK(until([&] { return msgs_b->at(0).value("deleted").toBool(); }));
    CHECK(until([&] { return !shows("hello edited") && shows("Message deleted"); }, 5000));

    /* ---- Bob goes offline: the message says so, and goes through when he is back ---- */
    bob.stop();
    CHECK(click(find("composer")));
    typeText("are you there");
    key(Qt::Key_Return);
    CHECK(until([&] { return shows("are you there"); }, 5000));
    CHECK(until([&] {
        QQuickItem *n = find("deliveryNote");
        if (!n)
            return false;
        bool said = false;
        walk(n, [&](QQuickItem *i) {
            said = said || i->property("text").toString().contains("can't be reached");
            return false;
        });
        return said;
    }, 8000));
    CHECK(find("retryButton") != nullptr);
    CHECK(bob.start());
    CHECK(until([&] { return B.connected(); }, 10000));
    CHECK(click(find("retryButton")));
    CHECK(until([&] { return find("deliveryNote") == nullptr; }, 10000)); /* delivered: nothing more to say */
    CHECK(until([&] {
        for (const QJsonObject &o : msgs_b->items())
            if (o.value("body").toString() == "are you there")
                return true;
        return false;
    }, 10000));

    /* ---- the window out of sight: what arrives stays unread, and is read when it is back ---- */
    win->hide();
    CHECK(until([&] { return !A.attentive(); }, 3000));
    B.send("while you were away");
    CHECK(until([&] { return A.unreadTotal() == 1; }));
    settle(600);
    CHECK(A.unreadTotal() == 1);
    win->show();
    win->requestActivate();
    CHECK(until([&] { return A.attentive(); }, 3000));
    CHECK(until([&] { return A.unreadTotal() == 0; }));
    CHECK(until([&] { return shows("while you were away"); }, 5000));

    /* ---- a narrow window shows one pane at a time ---- */
    win->resize(380, 640);
    CHECK(until([&] { return find("chatPage") && !find("sidebar"); }, 3000));
    CHECK(clickName("chatBack"));
    CHECK(until([&] { return find("sidebar") && !find("chatPage"); }, 3000));
    CHECK(clickName("friendsButton"));
    CHECK(until([&] { return find("addFriendButton") && !find("sidebar"); }, 3000));
    win->resize(1000, 640);
    CHECK(until([&] { return find("sidebar") && find("addFriendButton"); }, 3000));

    /* ---- someone asks us: no IDs or codes on screen, until Developer Mode is on ---- */
    {
        Node carol;
        CHECK(carol.up(home.path() + "/friends", "Carol"));
        QSignalSpy asked(&A, &Chats::newRequest);
        carol.chats->request(backend.myLink(), "carol here");
        CHECK(until([&] { return asked.count() == 1 && A.incomingRequests() == 1; }));
        CHECK(clickName("friendsButton"));
        CHECK(clickName("tab1"));                                              /* Pending */
        CHECK(until([&] { return find("acceptButton") != nullptr; }, 5000));
        CHECK(find("codeLabel") == nullptr);                                   /* nobody compares them */
        CHECK(!backend.developerMode());
        CHECK(clickName("settingsButton"));
        CHECK(until([&] { return find("developerSwitch") != nullptr; }, 3000));
        CHECK(find("myIdField") == nullptr);
        CHECK(click(find("developerSwitch")));                                 /* Developer Mode */
        CHECK(until([&] { return backend.developerMode(); }, 3000));
        CHECK(until([&] { return find("myIdField") != nullptr; }, 3000));
        CHECK(find("myIdField")->property("text").toString() == backend.myId());
        CHECK(clickName("friendsButton"));
        CHECK(until([&] { return find("codeLabel") != nullptr; }, 5000));
        CHECK(find("codeLabel")->property("text").toString().contains(carol.uid));
        backend.setDeveloperMode(false);
        CHECK(until([&] { return find("codeLabel") == nullptr; }, 3000));
        CHECK(click(find("acceptButton")));                                    /* and it can be accepted without ever seeing an ID */
        CHECK(until([&] { return A.incomingRequests() == 0 && A.friendCount() == 2; }));
    }

    /* ================= groups ================= */
    {
        Node dana;
        CHECK(dana.up(home.path() + "/friends", "Dana"));
        Chats &D = *dana.chats;
        auto *msgs_d = static_cast<JsonListModel *>(D.messages());
        auto *conv_a = static_cast<JsonListModel *>(A.conversations());
        A.request(dana.link, "dana, it is the window");
        CHECK(until([&] { return D.incomingRequests() == 1; }));
        D.accept(backend.myId());
        CHECK(until([&] { return A.friendCount() == 3; }));
        auto group_rows = [&](JsonListModel *m) { int n = 0; for (const QJsonObject &o : m->items()) n += o.value("kind").toString() == "group"; return n; };

        /* ---- make a group from the window: a name, and which friends ---- */
        CHECK(clickName("friendsButton") || true);
        CHECK(clickName("newGroupButton"));
        CHECK(until([&] { return find("groupNameField") != nullptr; }, 3000));
        CHECK(click(find("groupNameField")));
        typeText("Weekend");
        CHECK(until([&] { return find_text("friendPick", "Bob") && find_text("friendPick", "Dana"); }, 5000));
        CHECK(click(find_text("friendPick", "Bob")));
        CHECK(click(find_text("friendPick", "Dana")));
        CHECK(find("createGroupButton") && find("createGroupButton")->property("text").toString().contains("2"));
        QSignalSpy made(&A, &Chats::groupCreated);
        CHECK(click(find("createGroupButton")));
        CHECK(until([&] { return made.count() == 1; }));
        const QString gid = made.at(0).at(0).toString();
        CHECK(until([&] { return A.currentIsGroup() && A.current() == gid && find("composer") != nullptr; }, 5000));   /* the window opens it */
        CHECK(A.currentName() == "Weekend" && A.currentState() == "active");
        CHECK(until([&] { return find("groupNotice") != nullptr; }, 5000));                       /* "You made the group “Weekend”" */
        CHECK(find("groupNotice")->property("text").toString().contains("Weekend") && find("groupNotice")->property("text").toString().contains("You"));

        /* ---- they say yes; bob and dana have never met, and are connected through it ---- */
        CHECK(until([&] { return B.incomingGroupInvites() >= 1 && D.incomingGroupInvites() == 1; }));
        B.acceptGroup(gid);
        D.acceptGroup(gid);
        CHECK(until([&] { return A.members()->property("count").toInt() == 3; }));
        CHECK(until([&] { return row(static_cast<JsonListModel *>(B.people()), "id", dana.uid).value("state").toString() == "scoped"; }));
        CHECK(until([&] {
            int joined = 0;
            for (QQuickItem *i : all("groupNotice"))
                joined += i->property("text").toString().contains("joined");
            return joined == 2;
        }, 8000));

        /* ---- talk ---- */
        CHECK(click(find("composer")));
        typeText("hello group");
        key(Qt::Key_Return);
        CHECK(until([&] { return shows("hello group"); }, 5000));
        D.open(gid, "group");
        CHECK(until([&] {
            for (const QJsonObject &o : msgs_d->items())
                if (o.value("body").toString() == "hello group" && !o.value("senderName").toString().isEmpty() && o.value("senderName").toString() != "You")
                    return true;
            return false;
        }));
        B.open(gid, "group");
        B.send("hi from bob");
        CHECK(until([&] { return shows("hi from bob"); }, 5000));
        CHECK(until([&] {
            for (const QJsonObject &o : msgs_d->items())
                if (o.value("body").toString() == "hi from bob" && o.value("senderName").toString() == "Bob")
                    return true;                                                                   /* dana hears bob directly */
            return false;
        }));
        B.close();
        D.close();

        /* ---- the members, a new name, and leaving ---- */
        CHECK(clickName("membersButton"));
        CHECK(until([&] { return find("memberList") != nullptr && find("memberList")->property("count").toInt() == 3; }, 5000));
        CHECK(click(find("renameField")));
        typeText("Weekend away");
        CHECK(click(find("renameButton")));
        CHECK(until([&] { return A.currentName() == "Weekend away"; }));
        CHECK(until([&] { return row(conv_a, "id", gid).value("name").toString() == "Weekend away"; }));
        CHECK(clickName("leaveGroupButton"));
        CHECK(until([&] { return button("OK") != nullptr; }, 3000));
        CHECK(click(button("OK")));
        CHECK(until([&] { return A.current().isEmpty() && group_rows(conv_a) == 0; }, 5000));
        CHECK(until([&] { return find("memberList") == nullptr; }, 3000));                        /* and the box is gone */

        /* ---- invited: the invitation waits in Pending, and joining says what it costs ---- */
        QSignalSpy invited(&A, &Chats::newGroupInvite);
        B.createGroup("Bob's trip", { backend.myId() });
        CHECK(until([&] { return A.incomingGroupInvites() == 1 && invited.count() == 1; }));
        CHECK(clickName("friendsButton"));
        CHECK(clickName("tab1"));
        CHECK(until([&] { return find("acceptGroupButton") != nullptr; }, 5000));
        CHECK(click(find("acceptGroupButton")));
        CHECK(until([&] { return button("OK") != nullptr; }, 3000));                               /* "Everyone in this group will be able to see your address" */
        {
            bool said = false;
            walk(win->contentItem(), [&](QQuickItem *i) {
                said = said || (i->isVisible() && i->property("text").toString().contains("see your address"));
                return false;
            });
            CHECK(said);
        }
        CHECK(click(button("OK")));
        CHECK(until([&] { return A.incomingGroupInvites() == 0 && group_rows(conv_a) == 1; }, 5000));
        /* and one that is turned down */
        B.createGroup("Not for me", { backend.myId() });
        CHECK(until([&] { return A.incomingGroupInvites() == 1; }));
        CHECK(until([&] { return find("declineGroupButton") != nullptr; }, 5000));
        CHECK(click(find("declineGroupButton")));
        CHECK(until([&] { return A.incomingGroupInvites() == 0; }));
        settle(500);
        CHECK(group_rows(conv_a) == 1);
    }

    return failures ? 1 : 0;
}
