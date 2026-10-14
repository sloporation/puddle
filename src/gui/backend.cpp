#include "backend.h"

#include <QCoreApplication>
#include <QEvent>
#include <QSettings>

#include "dock.h"

extern "C" {
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
    puddle_config cfg;
    if (puddle_config_load(&cfg, nullptr, err, sizeof err) != 0) {
        error_ = QString::fromUtf8(err);
        return;
    }

    g_self = this;
    puddle_log_set_sink(logSink, nullptr);

    srv_ = puddle_server_create(&cfg, err, sizeof err);
    if (!srv_) {
        error_ = QString::fromUtf8(err);
        puddle_log_set_sink(nullptr, nullptr);
        g_self = nullptr;
        return;
    }

    const QString addr = QString::fromUtf8(cfg.address);
    listenUrl_ = (addr.contains(':') ? QStringLiteral("https://[%1]:%2") : QStringLiteral("https://%1:%2"))
                     .arg(addr).arg(cfg.port);
    worker_ = std::thread([s = srv_] { puddle_server_run(s); });
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

bool Backend::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Quit && !quitting_) {
        quitting_ = true;
        emit quittingChanged();
    }
    return QObject::eventFilter(watched, event);
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
