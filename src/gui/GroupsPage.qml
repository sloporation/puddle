import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Groups, like Friends has its own page: the invitations waiting for an answer, the groups you are in, and a way to start one.
Pane {
    id: page

    required property var chats
    property bool narrow: false
    signal newGroup()
    signal openGroup(string groupId)
    signal back()

    padding: 0

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 48
            color: "transparent"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8
                ToolButton { visible: page.narrow; text: "‹"; font.pixelSize: 22; Accessible.name: qsTr("Back"); onClicked: page.back() }
                Label { text: qsTr("Groups"); font.bold: true; font.pixelSize: 16; Layout.fillWidth: true }
                Button {
                    id: startButton
                    objectName: "newGroupButton"
                    text: page.narrow ? qsTr("New") : qsTr("Start new group")
                    Accessible.name: qsTr("Start new group")
                    contentItem: Label { text: startButton.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { radius: 6; color: startButton.down ? "#236b3a" : "#2d7d46" }
                    onClicked: page.newGroup()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.5 }

        ListView {
            id: list
            objectName: "groupList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            clip: true
            model: chats.conversations
            ScrollBar.vertical: ScrollBar {}

            // invitations first: someone is waiting for an answer
            header: Column {
                width: list.width
                Label {
                    visible: chats.incomingGroupInvites > 0
                    x: 8; topPadding: 12; bottomPadding: 4
                    text: qsTr("Invitations — %1").arg(chats.incomingGroupInvites)
                    font.pixelSize: 12; font.bold: true; opacity: 0.65
                }
                Repeater {
                    model: chats.groupInvites
                    delegate: GroupInviteRow {
                        required property var model
                        width: list.width
                        chats: page.chats
                        groupId: model.id
                        name: model.name
                        invitedBy: model.invitedByName
                        onAccept: { joinConfirm.target = model.id; joinConfirm.groupName = model.name; joinConfirm.open() }
                    }
                }
                Label {
                    x: 8; topPadding: 12; bottomPadding: 4
                    text: qsTr("Your groups")
                    font.pixelSize: 12; font.bold: true; opacity: 0.65
                }
            }

            delegate: ItemDelegate {
                id: row
                required property var model
                readonly property bool isGroup: model.kind === "group"
                width: ListView.view.width
                height: isGroup ? 52 : 0
                visible: isGroup
                Accessible.name: model.name || ""
                background: Rectangle {
                    radius: 6
                    color: row.hovered ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.07) : "transparent"
                }
                contentItem: RowLayout {
                    spacing: 12
                    Avatar { chats: page.chats; name: row.model.name || ""; size: 32 }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label { text: row.model.name || ""; font.bold: row.model.unread > 0; elide: Text.ElideRight; Layout.fillWidth: true }
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.pixelSize: 11
                            opacity: 0.6
                            text: !row.model.last ? "" : row.model.last.deleted ? qsTr("Message deleted") : (row.model.lastBy || "") + ": " + row.model.last.preview
                        }
                    }
                    Rectangle {
                        visible: row.model.unread > 0
                        width: Math.max(18, unreadLabel.implicitWidth + 8); height: 18; radius: 9; color: "#d9453d"
                        Label { id: unreadLabel; anchors.centerIn: parent; text: row.model.unread; color: "white"; font.pixelSize: 10; font.bold: true }
                    }
                }
                onClicked: page.openGroup(row.model.id)
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 48
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                opacity: 0.6
                visible: page.groupCount === 0 && chats.incomingGroupInvites === 0
                text: qsTr("No groups yet. Start one, or wait to be invited.")
            }
        }
    }

    // how many groups there are to show (the list is the conversations, which has the direct messages too)
    property int groupCount: 0
    function recount() {
        let n = 0
        for (let i = 0; i < chats.conversations.count; i++)
            if (chats.kindAt(i) === "group") n++
        groupCount = n
    }
    Connections {
        target: chats.conversations
        function onCountChanged() { page.recount() }
        function onModelReset() { page.recount() }
        function onRowsInserted() { page.recount() }
    }
    Component.onCompleted: recount()

    // joining shows your address to everyone in the group: say so first
    Dialog {
        id: joinConfirm
        objectName: "joinConfirm"
        property string target: ""
        property string groupName: ""
        anchors.centerIn: Overlay.overlay
        width: 380
        modal: true
        title: qsTr("Join “%1”?").arg(groupName)
        standardButtons: Dialog.Cancel | Dialog.Ok
        Label {
            width: parent.width
            wrapMode: Text.Wrap
            text: qsTr("Everyone in this group will be able to see your address and connect to your Puddle for it, including people who are not your friends.")
        }
        onAccepted: page.chats.acceptGroup(target)
    }
}
