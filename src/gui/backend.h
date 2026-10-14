#pragma once

#include <QObject>
#include <QStringListModel>
#include <thread>

extern "C" {
#include "config.h"
#include "server.h"
}

// C++ side of the UI: owns the embedded server and exposes state to QML.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString listenUrl READ listenUrl CONSTANT)
    Q_PROPERTY(QString error READ error CONSTANT)   // non-empty if startup failed
    Q_PROPERTY(QObject *log READ log CONSTANT)      // list model, role "display"
    // True once the app has been asked to quit (Cmd+Q, session end, ...), so the
    // window knows to really close instead of hiding to the tray.
    Q_PROPERTY(bool quitting READ quitting NOTIFY quittingChanged)
public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    QString listenUrl() const { return listenUrl_; }
    QString error() const { return error_; }
    QObject *log() { return &log_; }

    bool quitting() const { return quitting_; }

    void append(const QString &line);

    // True exactly once per install: the "still running in the tray" hint.
    Q_INVOKABLE bool takeFirstHideNotice();

signals:
    void quittingChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QStringListModel log_;
    QString listenUrl_;
    QString error_;
    bool quitting_ = false;
    puddle_server *srv_ = nullptr;
    std::thread worker_;
};
