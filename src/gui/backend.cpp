#include "backend.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QSettings>

#include "dock.h"

extern "C" {
#include "identity.h"
#include "router.h"
#include "log.h"
}

namespace {

constexpr int kMaxLogLines = 1000;
Backend *g_self;

void logSink(puddle_log_level level, const char *msg, void *)
{
    static const char *const names[] = { "info", "warn", "error" };
    const QString line = QStringLiteral("%1: %2").arg(QLatin1String(names[level]), QString::fromUtf8(msg));
    // Called from connection threads; hop to the GUI thread.
    if (Backend *self = g_self)
        QMetaObject::invokeMethod(self, [self, line] { self->append(line); }, Qt::QueuedConnection);
}

} // namespace

Backend::Backend(QObject *parent) : QObject(parent)
{
    qApp->installEventFilter(this);

    char err[512];
    if (puddle_config_load(&cfg_, nullptr, err, sizeof err) != 0) {
        error_ = QString::fromUtf8(err);
        return;
    }
    haveConfig_ = true;

    // No identity, or one whose phrase was shown but never confirmed: setup comes first, and the
    // server stays off until the owner has written the phrase down.
    setupRedo_ = puddle_identity_unconfirmed(cfg_.data_dir) != 0;
    needsSetup_ = setupRedo_ || !puddle_identity_exists(cfg_.data_dir);
    if (!needsSetup_)
        startServer();
}

Backend::~Backend()
{
    if (srv_) {
        puddle_server_stop(srv_);
        worker_.join(); // run() drains in-flight connections before returning
        puddle_log_set_sink(nullptr, nullptr);
        g_self = nullptr;
        puddle_server_destroy(srv_);
    }
}

void Backend::setError(const QString &message)
{
    if (error_ == message)
        return;
    error_ = message;
    emit errorChanged();
}

void Backend::startServer()
{
    char err[512];
    g_self = this;
    puddle_log_set_sink(logSink, nullptr);

    srv_ = puddle_server_create(&cfg_, err, sizeof err);
    if (!srv_) {
        setError(QString::fromUtf8(err));
        puddle_log_set_sink(nullptr, nullptr);
        g_self = nullptr;
        return;
    }

    const QString addr = QString::fromUtf8(cfg_.address);
    listenUrl_ = (addr.contains(':') ? QStringLiteral("https://[%1]:%2") : QStringLiteral("https://%1:%2"))
                     .arg(addr).arg(cfg_.port);
    worker_ = std::thread([s = srv_] { puddle_server_run(s); });

    // The window reads everything through the owner API, as a phone client does. "All interfaces" is reached by loopback.
    QString host = addr;
    if (host == QLatin1String("::") || host == QLatin1String("0.0.0.0"))
        host = host == QLatin1String("::") ? QStringLiteral("::1") : QStringLiteral("127.0.0.1");
    const QString base = (host.contains(':') ? QStringLiteral("https://[%1]:%2") : QStringLiteral("https://%1:%2")).arg(host).arg(cfg_.port);
    if (!chats_.attach(QUrl(base), QString::fromUtf8(cfg_.data_dir) + "/state/owner-token", QString::fromUtf8(cfg_.dir) + "/cert.pem"))
        setError(QStringLiteral("Could not open the owner token or certificate to talk to your Puddle."));
    char id[PUDDLE_USER_ID_LEN + 1], link[512];
    puddle_identity_id(puddle_server_identity(srv_), id);
    myId_ = QString::fromLatin1(id);
    myLink_ = puddle_make_link(&cfg_, id, nullptr, link, sizeof link) == 0 ? QString::fromUtf8(link) : QString();
    emit startedChanged();
}

QString Backend::createIdentity()
{
    if (!haveConfig_ || !needsSetup_)
        return QString();
    char err[512], phrase[PUDDLE_PHRASE_LEN + 1];
    setError(QString());

    if (puddle_identity_unconfirmed(cfg_.data_dir) &&
        puddle_identity_discard_unconfirmed(cfg_.data_dir, err, sizeof err) < 0) {
        setError(QString::fromUtf8(err));
        return QString();
    }
    puddle_identity *id = puddle_identity_create(cfg_.data_dir, phrase, err, sizeof err);
    if (!id) {
        setError(QString::fromUtf8(err));
        return QString();
    }
    puddle_identity_close(id);

    const QString result = QString::fromLatin1(phrase);
    memset(phrase, 0, sizeof phrase);
    return result;
}

bool Backend::finishSetup()
{
    if (!haveConfig_ || !needsSetup_)
        return false;
    if (puddle_identity_confirm(cfg_.data_dir) != 0) {
        setError(QStringLiteral("Could not save your identity. Check that the data folder can be written to."));
        return false;
    }
    needsSetup_ = false;
    setupRedo_ = false;
    emit needsSetupChanged();
    startServer();
    return running();
}

void Backend::copyText(const QString &text)
{
    if (QClipboard *c = QGuiApplication::clipboard())
        c->setText(text);
}

bool Backend::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Quit && !quitting_) {
        quitting_ = true;
        emit quittingChanged();
    }
    return QObject::eventFilter(watched, event);
}

void Backend::setDockBadge(int count)
{
    puddle_set_dock_badge(count);
}

void Backend::setDockVisible(bool visible)
{
    puddle_set_dock_visible(visible);
}

bool Backend::takeFirstHideNotice()
{
    QSettings settings;
    if (settings.value("trayNoticeShown", false).toBool())
        return false;
    settings.setValue("trayNoticeShown", true);
    return true;
}

void Backend::append(const QString &line)
{
    const int row = log_.rowCount();
    log_.insertRows(row, 1);
    log_.setData(log_.index(row), line);
    if (log_.rowCount() > kMaxLogLines)
        log_.removeRows(0, log_.rowCount() - kMaxLogLines);
}
