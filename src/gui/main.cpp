// puddle-gui: desktop front end. Hosts the same core server as puddled in a
// background thread and shows its log.
#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QThread>
#include <QVBoxLayout>

extern "C" {
#include "config.h"
#include "log.h"
#include "server.h"
}

namespace {

class ServerThread : public QThread {
public:
    explicit ServerThread(puddle_server *s) : srv(s) {}
    void run() override { puddle_server_run(srv); }
private:
    puddle_server *srv;
};

class Window : public QMainWindow {
public:
    Window(const puddle_config &cfg) {
        setWindowTitle("Puddle");
        auto *box = new QWidget;
        auto *lay = new QVBoxLayout(box);
        const bool v6 = QString(cfg.address).contains(':');
        auto *status = new QLabel(QString(v6 ? "Listening on https://[%1]:%2" : "Listening on https://%1:%2")
                                      .arg(cfg.address).arg(cfg.port));
        log_ = new QPlainTextEdit;
        log_->setReadOnly(true);
        log_->setMaximumBlockCount(1000);
        lay->addWidget(status);
        lay->addWidget(log_);
        setCentralWidget(box);
        resize(640, 360);
    }
    void append(const QString &line) { log_->appendPlainText(line); }
private:
    QPlainTextEdit *log_;
};

Window *g_window;

void logSink(puddle_log_level level, const char *msg, void *)
{
    static const char *const names[] = { "info", "warn", "error" };
    QString line = QString("%1: %2").arg(names[level], QString::fromUtf8(msg));
    // Called from connection threads; hop to the GUI thread.
    QMetaObject::invokeMethod(qApp, [line] { if (g_window) g_window->append(line); },
                              Qt::QueuedConnection);
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("Puddle");

    char err[512];
    puddle_config cfg;
    if (puddle_config_load(&cfg, nullptr, err, sizeof err) != 0) {
        QMessageBox::critical(nullptr, "Puddle", QString::fromUtf8(err));
        return 1;
    }

    Window window(cfg);
    g_window = &window;
    puddle_log_set_sink(logSink, nullptr);

    puddle_server *srv = puddle_server_create(&cfg, err, sizeof err);
    if (!srv) {
        QMessageBox::critical(nullptr, "Puddle", QString::fromUtf8(err));
        return 1;
    }

    ServerThread thread(srv);
    thread.start();
    window.show();

    int rc = app.exec();

    puddle_server_stop(srv);
    thread.wait();
    puddle_log_set_sink(nullptr, nullptr);
    g_window = nullptr;
    puddle_server_destroy(srv);
    return rc;
}
