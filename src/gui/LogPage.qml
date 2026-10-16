import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Settings, and what the Puddle has been doing, newest at the bottom.
Pane {
    id: page

    required property var backend
    property bool narrow: false
    signal back()

    padding: 0

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            ToolButton { visible: page.narrow; text: "‹"; font.pixelSize: 22; Accessible.name: qsTr("Back"); onClicked: page.back() }
            Label { text: qsTr("Settings"); font.bold: true; font.pixelSize: 16 }
            Item { Layout.fillWidth: true }
            Label {
                text: page.backend.error === "" ? qsTr("Listening on %1").arg(page.backend.listenUrl) : qsTr("Not running")
                opacity: 0.6
                elide: Text.ElideMiddle
                Layout.maximumWidth: page.width * 0.6
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.5 }

        // Like Discord's Developer Mode: what is normally kept out of sight
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 16
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label { text: qsTr("Developer Mode"); font.bold: true }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.65
                    font.pixelSize: 12
                    text: qsTr("Show user IDs and codes, and let you copy them.")
                }
            }
            Switch {
                objectName: "developerSwitch"
                checked: page.backend.developerMode
                onToggled: page.backend.developerMode = checked
                Accessible.name: qsTr("Developer Mode")
            }
        }
        RowLayout {
            visible: page.backend.developerMode && page.backend.myId !== ""
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 8
            Label { text: qsTr("Your user ID"); opacity: 0.65 }
            TextField {
                objectName: "myIdField"
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: page.backend.myId
                font.family: Qt.platform.os === "osx" ? "Menlo" : Qt.platform.os === "windows" ? "Consolas" : "monospace"
                font.pixelSize: 12
                Accessible.name: qsTr("Your user ID")
            }
            Button { text: qsTr("Copy"); onClicked: page.backend.copyText(page.backend.myId) }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.5 }
        Label { Layout.margins: 16; Layout.bottomMargin: 0; text: qsTr("Activity"); font.bold: true }
        Label {
            visible: page.backend.error !== ""
            Layout.fillWidth: true
            Layout.margins: 16
            wrapMode: Text.Wrap
            color: "#d9534f"
            text: page.backend.error
        }
        ListView {
            id: logView
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 12
            clip: true
            model: page.backend.log
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
}
