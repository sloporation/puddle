#pragma once

#include <QObject>
#include <QStringListModel>
#include <thread>

extern "C" {
#include "config.h"
#include "server.h"
}

// C++ side of the UI: owns the embedded server and exposes state to QML.
//
// Start-up has three outcomes: the server runs; there is an error to show; or the Puddle has no usable
// identity yet and the app must walk the owner through making one (needsSetup). In that case the server
// is started by finishSetup(), only after the owner has confirmed writing the recovery phrase down.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString listenUrl READ listenUrl NOTIFY startedChanged)
    Q_PROPERTY(bool running READ running NOTIFY startedChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)   // non-empty if something failed
    Q_PROPERTY(QObject *log READ log CONSTANT)                 // list model, role "display"
    Q_PROPERTY(bool needsSetup READ needsSetup NOTIFY needsSetupChanged)
    // An earlier setup showed a phrase but was never confirmed; it will be started again.
    Q_PROPERTY(bool setupRedo READ setupRedo NOTIFY needsSetupChanged)
    // True once the app has been asked to quit (Cmd+Q, session end, ...), so the
    // window knows to really close instead of hiding to the tray.
    Q_PROPERTY(bool quitting READ quitting NOTIFY quittingChanged)
public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    QString listenUrl() const { return listenUrl_; }
    bool running() const { return srv_ != nullptr; }
    QString error() const { return error_; }
    QObject *log() { return &log_; }
    bool needsSetup() const { return needsSetup_; }
    bool setupRedo() const { return setupRedo_; }
    bool quitting() const { return quitting_; }

    void append(const QString &line);

    // Make the identity and return its recovery phrase (empty on failure, with `error` set). The phrase
    // is returned once and kept nowhere. An earlier unfinished identity is thrown away first.
    Q_INVOKABLE QString createIdentity();

    // The owner has written the phrase down: confirm the identity and start the Puddle.
    Q_INVOKABLE bool finishSetup();

    Q_INVOKABLE void copyText(const QString &text);

    // True exactly once per install: the "still running in the tray" hint.
    Q_INVOKABLE bool takeFirstHideNotice();

    // macOS: Dock icon only while the window is shown. No-op on other platforms.
    Q_INVOKABLE void setDockVisible(bool visible);

signals:
    void quittingChanged();
    void startedChanged();
    void errorChanged();
    void needsSetupChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void startServer();
    void setError(const QString &message);

    QStringListModel log_;
    QString listenUrl_;
    QString error_;
    bool quitting_ = false;
    bool needsSetup_ = false;
    bool setupRedo_ = false;
    bool haveConfig_ = false;
    puddle_config cfg_;
    puddle_server *srv_ = nullptr;
    std::thread worker_;
};
