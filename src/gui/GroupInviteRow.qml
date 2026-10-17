import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// An invitation to a group, in the Pending list next to friend requests.
Rectangle {
    id: row

    required property var chats
    property string groupId: ""
    property string name: ""
    property string invitedBy: ""
    signal accept()

    implicitHeight: Math.max(col.implicitHeight, 40) + 20
    color: hover.hovered ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.07) : "transparent"
    radius: 8
    Accessible.role: Accessible.ListItem
    Accessible.name: qsTr("Invitation to %1").arg(name)

    HoverHandler { id: hover }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 12
        Avatar { chats: row.chats; name: row.name; size: 40; Layout.alignment: Qt.AlignTop }
        ColumnLayout {
            id: col
            Layout.fillWidth: true
            spacing: 2
            Label { text: row.name; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
            Label {
                Layout.fillWidth: true
                opacity: 0.65
                font.pixelSize: 12
                wrapMode: Text.Wrap
                text: qsTr("%1 invited you to a group").arg(row.invitedBy)
            }
        }
        RoundButton {
            objectName: "acceptGroupButton"
            text: "✓"
            Accessible.name: qsTr("Join")
            ToolTip.text: qsTr("Join"); ToolTip.visible: hovered
            onClicked: row.accept()
        }
        RoundButton {
            objectName: "declineGroupButton"
            text: "✕"
            Accessible.name: qsTr("Decline")
            ToolTip.text: qsTr("Decline"); ToolTip.visible: hovered
            onClicked: row.chats.declineGroup(row.groupId)
        }
    }
}
