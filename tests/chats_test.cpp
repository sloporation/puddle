// The app's view of the Puddle (Chats over ApiClient) against two real Puddles that talk to each other over TLS.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <functional>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "testnode.h"

extern "C" {
#include "config.h"
#include "identity.h"
#include "phrase.h"
#include "server.h"
}

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static QJsonObject row(JsonListModel *m, const QString &key, const QString &value)
{
    for (const QJsonObject &o : m->items())
        if (o.value(key).toString() == value)
            return o;
    return {};
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    CHECK(tmp.isValid());

    Node alice, bob;
    CHECK(alice.up(tmp.path(), "Alice"));
    CHECK(bob.up(tmp.path(), "Bob"));
    Chats &A = *alice.chats, &B = *bob.chats;
    auto *people_a = static_cast<JsonListModel *>(A.people());
    auto *people_b = static_cast<JsonListModel *>(B.people());
    auto *conv_a = static_cast<JsonListModel *>(A.conversations());
    auto *conv_b = static_cast<JsonListModel *>(B.conversations());
    auto *msgs_a = static_cast<JsonListModel *>(A.messages());
    auto *msgs_b = static_cast<JsonListModel *>(B.messages());

    CHECK(until([&] { return A.connected() && B.connected(); }));
    CHECK(people_a->rowCount() == 0 && conv_a->rowCount() == 0);

    /* Alice asks Bob; Bob sees a request from "Alice" and is told about it */
    QSignalSpy newRequest(&B, &Chats::newRequest);
    A.request(bob.link, "hello, it is me");
    CHECK(until([&] { return B.incomingRequests() == 1; }));
    QJsonObject req = row(people_b, "id", alice.uid);
    CHECK(req.value("state").toString() == "pending_in" && req.value("note").toString() == "hello, it is me");
    CHECK(req.value("name").toString() == "Alice" || req.value("name").toString() == alice.uid.left(8)); /* the name arrives with the profile */
    CHECK(newRequest.count() == 1 && newRequest.at(0).at(0).toString() == alice.uid);
    {
        QJsonObject out;
        for (const QJsonObject &o : people_a->items())
            if (o.value("state").toString() == "pending_out")
                out = o;
        CHECK(!out.isEmpty() && out.value("provisional").toBool() && out.value("name").toString() == bob.link && out.value("id").toString() != bob.uid);
    }

    /* a request that cannot work says so */
    QSignalSpy failed(&A, &Chats::failed);
    A.request("not a link", "");
    CHECK(until([&] { return failed.count() == 1; }));

    B.accept(alice.uid);
    CHECK(until([&] { return B.incomingRequests() == 0 && row(people_a, "id", bob.uid).value("state").toString() == "active"; }));
    CHECK(until([&] { return row(people_b, "id", alice.uid).value("name").toString() == "Alice"; }));
    CHECK(until([&] { return row(people_a, "id", bob.uid).value("name").toString() == "Bob"; }));

    /* a message arrives: unread for Bob, with a preview, and a notification signal */
    QSignalSpy incoming(&B, &Chats::newIncoming);
    A.open(bob.uid);
    CHECK(A.current() == bob.uid);
    A.send("  hello Bob  ");
    CHECK(until([&] { return B.unreadTotal() == 1; }));
    QJsonObject c = row(conv_b, "user", alice.uid);
    CHECK(c.value("unread").toInt() == 1 && c.value("last").toObject().value("preview").toString() == "hello Bob" && c.value("name").toString() == "Alice");
    CHECK(incoming.count() == 1 && incoming.at(0).at(1).toInt() == 1);
    CHECK(until([&] { return msgs_a->rowCount() == 1; }));
    CHECK(msgs_a->at(0).value("outgoing").toBool() && msgs_a->at(0).value("senderName").toString() == "You");

    /* opening the chat shows it, and reads it */
    B.open(alice.uid);
    CHECK(until([&] { return msgs_b->rowCount() == 1; }));
    CHECK(msgs_b->at(0).value("body").toString() == "hello Bob" && msgs_b->at(0).value("senderName").toString() == "Alice");
    CHECK(until([&] { return B.unreadTotal() == 0; }));
    CHECK(until([&] { return row(conv_b, "user", alice.uid).value("unread").toInt() == 0; }));

    QElapsedTimer settle; settle.start();
    while (settle.elapsed() < 600) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); usleep(5000); }
    CHECK(B.unreadTotal() == 0 && incoming.count() == 1); /* looking at it keeps it read; no new alert */

    /* and back; while Alice has the chat open nothing counts as unread for her */
    B.send("hi Alice");
    CHECK(until([&] { return msgs_a->rowCount() == 2; }));
    CHECK(msgs_a->at(1).value("body").toString() == "hi Alice" && msgs_a->at(1).value("senderName").toString() == "Bob");
    CHECK(until([&] { return A.unreadTotal() == 0 && row(conv_a, "user", bob.uid).value("unread").toInt() == 0; }));

    {   /* the read marker is the Puddle's, so another app of the owner sees nothing unread either */
        Chats other;
        other.setInterval(100);
        CHECK(other.attach(QUrl(QString("https://127.0.0.1:%1").arg(bob.cfg.port)), bob.dir + "/data/state/owner-token", bob.dir + "/cert.pem"));
        auto *oc = static_cast<JsonListModel *>(other.conversations());
        CHECK(until([&] { return oc->rowCount() == 1; }));
        CHECK(row(oc, "user", alice.uid).value("unread").toInt() == 0 && other.unreadTotal() == 0);
    }

    /* an edit and a delete reach the other side */
    const QString first = msgs_a->at(0).value("id").toString();
    A.edit(first, "hello Bobby");
    CHECK(until([&] { return msgs_b->rowCount() == 2 && msgs_b->at(0).value("body").toString() == "hello Bobby" && msgs_b->at(0).value("edited").toBool(); }));
    A.remove(first);
    CHECK(until([&] { return msgs_b->at(0).value("deleted").toBool() && msgs_b->at(0).value("body").toString().isEmpty(); }));

    /* what the chat page needs: who it is with, grouping, replies, and "away" */
    CHECK(B.currentName() == "Alice" && B.currentState() == "active" && !B.currentCertChanged() && !B.hasOlder());
    CHECK(msgs_b->at(0).value("dayStart").toBool() && !msgs_b->at(1).value("dayStart").toBool() && !msgs_b->at(0).value("grouped").toBool());
    B.send("two");
    CHECK(until([&] { return msgs_b->rowCount() == 3; }));
    CHECK(msgs_b->at(2).value("grouped").toBool() && msgs_b->at(2).value("senderName").toString() == "You"); /* same sender, minutes apart */
    B.send("replying to myself", msgs_b->at(1).value("id").toString());
    CHECK(until([&] { return msgs_b->rowCount() == 4; }));
    CHECK(!msgs_b->at(3).value("grouped").toBool() && msgs_b->at(3).value("replyText").toString() == "hi Alice" && msgs_b->at(3).value("replyName").toString() == "You");
    {
        JsonListModel lm({ "id" });
        QSignalSpy ins(&lm, &QAbstractItemModel::rowsInserted), rst(&lm, &QAbstractItemModel::modelReset);
        auto O = [](const char *id) { return QJsonObject{ { "id", id } }; };
        lm.setItems({ O("b"), O("c") });            /* first fill */
        lm.setItems({ O("a"), O("b"), O("c") });    /* older ones arrive: inserted above, not a reset */
        lm.setItems({ O("a"), O("b"), O("c"), O("d") });
        CHECK(lm.rowCount() == 4 && ins.count() == 2 && rst.count() == 1);
        CHECK(ins.at(0).at(1).toInt() == 0 && ins.at(1).at(1).toInt() == 3);
        lm.setItems({ O("x") });                    /* something else entirely */
        CHECK(lm.rowCount() == 1 && rst.count() == 2);
    }
    QSignalSpy incoming2(&B, &Chats::newIncoming);
    B.setAttentive(false);                          /* the window is hidden with the chat open */
    A.send("while you were away");
    CHECK(until([&] { return B.unreadTotal() == 1 && incoming2.count() == 1; }));
    B.setAttentive(true);                           /* back: seen */
    CHECK(until([&] { return B.unreadTotal() == 0 && row(conv_b, "user", alice.uid).value("unread").toInt() == 0; }));
    CHECK(until([&] { return msgs_b->rowCount() == 5; }));

    /* closing the chat clears it; the person stays in the list, newest conversation first */
    B.close();
    CHECK(B.current().isEmpty() && msgs_b->rowCount() == 0);
    CHECK(conv_b->rowCount() == 1);

    /* a certificate that is not the Puddle's is refused: the app sees nothing, not someone else's answer */
    {
        Chats wrong;
        CHECK(wrong.attach(QUrl(QString("https://127.0.0.1:%1").arg(bob.cfg.port)), alice.dir + "/data/state/owner-token", alice.dir + "/cert.pem"));
        QElapsedTimer t; t.start();
        while (t.elapsed() < 1500) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); usleep(5000); }
        CHECK(!wrong.connected() && wrong.people()->property("count").toInt() == 0);
    }
    /* a wrong token is answered as an unknown path: connected, but nothing */
    {
        QFile f(tmp.path() + "/badtoken");
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(64, 'a'));
        f.close();
        Chats bad;
        CHECK(bad.attach(QUrl(QString("https://127.0.0.1:%1").arg(bob.cfg.port)), f.fileName(), bob.dir + "/cert.pem"));
        QElapsedTimer t; t.start();
        while (t.elapsed() < 1500) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); usleep(5000); }
        CHECK(bad.people()->property("count").toInt() == 0);
    }
    /* missing files do not attach */
    {
        Chats none;
        CHECK(!none.attach(QUrl("https://127.0.0.1:1"), tmp.path() + "/nope", tmp.path() + "/nope"));
    }

    return failures ? 1 : 0;
}
