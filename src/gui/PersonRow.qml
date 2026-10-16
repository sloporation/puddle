import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One person in the Friends lists: who, what state, and what can be done.
// `mode` is "friend", "pending" or "blocked".
Rectangle {
    id: row

    required property var chats
    required property string mode
    property string userId: ""
    property string name: ""
    property string avatar: ""
    property string tint: ""
    property string streamState: ""
    property string note: ""
    property string error: ""
    property bool provisional: false
    property bool certChanged: false
    property bool developer: false // Developer Mode: show what is normally hidden
    signal message()

    implicitHeight: Math.max(col.implicitHeight, 40) + 20
    color: hover.hovered ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.07) : "transparent"
    radius: 8
    Accessible.role: Accessible.ListItem
    Accessible.name: name

    HoverHandler { id: hover }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: menu.popup()
    }
    TapHandler {
        enabled: row.mode === "friend"
        onDoubleTapped: row.message()
    }

    Menu {
        id: menu
        MenuItem { text: qsTr("Copy user ID"); visible: row.developer && !row.provisional; height: visible ? implicitHeight : 0; onTriggered: chats.copy(row.userId) }
        MenuItem {
            text: qsTr("Message"); visible: row.mode === "friend"; height: visible ? implicitHeight : 0
            onTriggered: row.message()
        }
        MenuItem {
            text: qsTr("Block"); visible: row.mode !== "blocked"; height: visible ? implicitHeight : 0
            onTriggered: chats.block(row.userId)
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 10
        anchors.topMargin: 10
        spacing: 12

        Avatar { chats: row.chats; name: row.name; hash: row.avatar; tint: row.tint; size: 40; Layout.alignment: Qt.AlignTop }

        ColumnLayout {
            id: col
            Layout.fillWidth: true
            spacing: 2
            Label { text: row.name; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
            Label {
                Layout.fillWidth: true
                opacity: 0.65
                font.pixelSize: 12
                elide: Text.ElideRight
                text: row.mode === "blocked" ? qsTr("Blocked")
                      : row.streamState === "pending_in" ? qsTr("Incoming friend request")
                      : row.streamState === "pending_out" ? qsTr("Outgoing friend request")
                      : row.streamState === "closed" ? qsTr("Stream closed")
                      : ""
                visible: text !== ""
            }
            Label {
                visible: row.note !== "" && row.streamState === "pending_in"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "“" + row.note + "”"
            }
            // Someone we do not know can claim any name. The code and the id are what to compare with them.
            // Developer Mode only: who they claim to be, and the code derived from it
            Label {
                objectName: "codeLabel"
                visible: row.developer && !row.provisional
                Layout.fillWidth: true
                opacity: 0.65
                font.pixelSize: 12
                wrapMode: Text.WrapAnywhere
                text: qsTr("ID %1  ·  Code %2").arg(row.userId).arg(chats.shortCode(row.userId))
            }
            Label {
                visible: row.error !== "" && row.mode !== "blocked"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#d9534f"
                font.pixelSize: 12
                text: row.error
            }
            RowLayout {
                visible: row.certChanged
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: "#d9a000"
                    font.pixelSize: 12
                    text: qsTr("Their certificate changed. Nothing is sent until you trust it.")
                }
                Button { text: qsTr("Trust"); Layout.preferredWidth: 72; onClicked: chats.trust(row.userId) }
            }
        }

        // actions
        RoundButton {
            objectName: "messageButton"
            visible: row.mode === "friend"
            text: "✉"
            Accessible.name: qsTr("Message")
            ToolTip.text: qsTr("Message"); ToolTip.visible: hovered
            onClicked: row.message()
        }
        RoundButton {
            objectName: "acceptButton"
            visible: row.streamState === "pending_in"
            text: "✓"
            Accessible.name: qsTr("Accept")
            ToolTip.text: qsTr("Accept"); ToolTip.visible: hovered
            onClicked: chats.accept(row.userId)
        }
        RoundButton {
            visible: row.streamState === "pending_in"
            text: "✕"
            Accessible.name: qsTr("Decline")
            ToolTip.text: qsTr("Decline"); ToolTip.visible: hovered
            onClicked: chats.decline(row.userId)
        }
        RoundButton {
            visible: row.streamState === "pending_out"
            text: "✕"
            Accessible.name: qsTr("Cancel request")
            ToolTip.text: qsTr("Cancel request"); ToolTip.visible: hovered
            onClicked: chats.cancel(row.userId)
        }
        RoundButton {
            visible: row.mode === "friend"
            text: "⋯"
            Accessible.name: qsTr("More")
            onClicked: menu.popup()
        }
        Button {
            visible: row.mode === "blocked"
            text: qsTr("Unblock")
            onClicked: chats.unblock(row.userId)
        }
    }
}
