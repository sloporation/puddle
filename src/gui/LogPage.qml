import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What the Puddle has been doing, newest at the bottom.
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
            Label { text: qsTr("Activity"); font.bold: true; font.pixelSize: 16 }
            Item { Layout.fillWidth: true }
            Label {
                text: page.backend.error === "" ? qsTr("Listening on %1").arg(page.backend.listenUrl) : qsTr("Not running")
                opacity: 0.6
                elide: Text.ElideMiddle
                Layout.maximumWidth: page.width * 0.6
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.5 }
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
