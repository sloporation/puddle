import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform

ApplicationWindow {
    id: root

    required property var backend

    // Phone-width layouts switch on this; a chat UI would show sidebar + thread
    // side by side when wide, and one at a time (StackView) when narrow.
    readonly property bool wide: width >= 720
    readonly property int margin: wide ? 24 : 12

    visible: true
    width: 900
    height: 600
    minimumWidth: 320
    minimumHeight: 400
    title: "Puddle"

    function showWindow() {
        backend.setDockVisible(true)
        show()
        raise()
        requestActivate()
    }

    // Close button hides to the tray. Real quits (tray menu, Cmd+Q, logout)
    // go through, and so does closing when there is no tray to restore from
    // (e.g. stock GNOME), so the app can't become unreachable.
    onClosing: close => {
        if (backend.quitting)
            return
        if (tray.available) {
            close.accepted = false
            hide()
            backend.setDockVisible(false)
            if (backend.takeFirstHideNotice())
                tray.showMessage("Puddle is still running",
                                 "Use the tray icon to reopen the window or quit.")
        } else {
            Qt.quit()
        }
    }

    Platform.SystemTrayIcon {
        id: tray
        visible: true
        // Must be a resolved URL: a relative one gives the tray item an empty icon.
        icon.source: Qt.resolvedUrl("puddle.png")
        // macOS template image: drawn black/white to follow the menu bar's light/dark theme.
        icon.mask: Qt.platform.os === "osx"
        tooltip: "Puddle"
        onActivated: reason => {
            if (reason === Platform.SystemTrayIcon.Trigger
                    || reason === Platform.SystemTrayIcon.DoubleClick)
                root.showWindow()
        }
        menu: Platform.Menu {
            Platform.MenuItem {
                text: qsTr("Show Puddle")
                onTriggered: root.showWindow()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: qsTr("Quit Puddle")
                onTriggered: Qt.quit()
            }
        }
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: root.margin
            anchors.rightMargin: root.margin

            Label {
                text: "Puddle"
                font.bold: true
                font.pixelSize: 18
            }
            Item { Layout.fillWidth: true }
            Label {
                text: root.backend.needsSetup ? "Setting up"
                      : root.backend.error === "" ? "Listening on " + root.backend.listenUrl : "Not running"
                elide: Text.ElideMiddle
                Layout.maximumWidth: root.width * 0.6
                opacity: 0.7
            }
        }
    }

    Setup {
        anchors.fill: parent
        visible: root.backend.needsSetup
        backend: root.backend
    }

    Label {
        visible: root.backend.error !== "" && !root.backend.needsSetup
        anchors.centerIn: parent
        width: parent.width - 2 * root.margin
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: root.backend.error
    }

    ListView {
        id: logView
        visible: root.backend.error === "" && !root.backend.needsSetup
        anchors.fill: parent
        anchors.margins: root.margin
        clip: true
        model: root.backend.log
        onCountChanged: positionViewAtEnd()
        ScrollBar.vertical: ScrollBar {}

        delegate: Label {
            required property string display
            width: ListView.view.width
            text: display
            wrapMode: Text.WrapAnywhere
            font.family: Qt.platform.os === "osx" ? "Menlo" : Qt.platform.os === "windows" ? "Consolas" : "monospace"
            font.pixelSize: 13
        }
    }
}
