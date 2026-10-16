import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Like Discord's Friends page: All, Pending and Blocked, and the button to add someone.
Pane {
    id: page

    required property var chats
    required property var backend
    property bool narrow: false
    signal addFriend()
    signal openChat(string userId)
    signal back()

    padding: 0

    readonly property int pending: chats.incomingRequests + chats.incomingGroupInvites
    property int tab: 0 // 0 all, 1 pending, 2 blocked

    // the people with a stream that is not asked for or ended
    function visibleFor(state) {
        if (tab === 0)
            return state === "active"
        if (tab === 1)
            return state === "pending_in" || state === "pending_out"
        return false
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // header
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 48
            color: "transparent"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8
                ToolButton {
                    visible: page.narrow
                    text: "‹"
                    font.pixelSize: 22
                    Accessible.name: qsTr("Back")
                    onClicked: page.back()
                }
                Label { visible: !page.narrow; text: qsTr("Friends"); font.bold: true; font.pixelSize: 16 }
                Rectangle { width: 1; height: 20; color: palette.mid; Layout.leftMargin: 4; Layout.rightMargin: 4; visible: !page.narrow }
                Repeater {
                    model: [qsTr("All"), qsTr("Pending"), qsTr("Blocked")]
                    delegate: AbstractButton {
                        id: tb
                        objectName: "tab" + index
                        required property int index
                        required property string modelData
                        readonly property bool current: page.tab === index
                        text: index === 1 && page.pending > 0 ? modelData + "  " + page.pending : modelData
                        implicitWidth: tbLabel.implicitWidth + 20
                        implicitHeight: 30
                        contentItem: Label { id: tbLabel; text: tb.text; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.bold: tb.current }
                        Accessible.name: text
                        onClicked: page.tab = index
                        background: Rectangle {
                            radius: 6
                            color: tb.current ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.12)
                                              : tb.hovered ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.06) : "transparent"
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "addFriendButton"
                    text: page.narrow ? qsTr("Add") : qsTr("Add Friend")
                    Accessible.name: qsTr("Add Friend")
                    contentItem: Label { text: parent.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { radius: 6; color: parent.down ? "#236b3a" : "#2d7d46" }
                    onClicked: page.addFriend()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.5 }

        Label {
            Layout.margins: 16
            Layout.bottomMargin: 4
            font.pixelSize: 12
            font.bold: true
            opacity: 0.65
            text: page.tab === 0 ? qsTr("All friends — %1").arg(chats.friendCount)
                  : page.tab === 1 ? qsTr("Pending — %1").arg(chats.pendingCount + chats.incomingGroupInvites)
                  : qsTr("Blocked — %1").arg(chats.blocked.count)
        }

        // everyone with a stream
        ListView {
            id: people
            visible: page.tab !== 2
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            clip: true
            model: chats.people
            spacing: 0
            ScrollBar.vertical: ScrollBar {}
            // invitations to groups come first in Pending, next to the friend requests
            header: Column {
                width: people.width
                visible: page.tab === 1
                height: visible ? implicitHeight : 0
                Repeater {
                    model: page.tab === 1 ? chats.groupInvites : null
                    delegate: GroupInviteRow {
                        required property var model
                        width: people.width
                        chats: page.chats
                        groupId: model.id
                        name: model.name
                        invitedBy: model.invitedByName
                        count: model.count
                        onAccept: { joinConfirm.target = model.id; joinConfirm.groupName = model.name; joinConfirm.open() }
                    }
                }
            }
            delegate: PersonRow {
                id: person
                required property var model
                chats: page.chats
                userId: model.id
                name: model.name
                avatar: model.avatar || ""
                tint: model.color || ""
                streamState: model.state
                note: model.note || ""
                error: model.error || ""
                certChanged: model.certificate_changed === true
                provisional: model.provisional === true
                developer: page.backend.developerMode
                mode: model.state === "active" || model.state === "closed" ? "friend" : "pending"
                width: ListView.view.width
                visible: page.visibleFor(model.state)
                height: visible ? implicitHeight : 0
                onMessage: page.openChat(model.id)
            }
            Label {
                anchors.centerIn: parent
                width: parent.width - 48
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                opacity: 0.6
                visible: page.tab === 0 ? chats.friendCount === 0 : chats.pendingCount + chats.incomingGroupInvites === 0
                text: page.tab === 0 ? qsTr("No friends yet. Use Add Friend and enter someone's address.")
                                     : qsTr("No pending requests.")
            }
        }

        ListView {
            visible: page.tab === 2
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            clip: true
            model: chats.blocked
            ScrollBar.vertical: ScrollBar {}
            delegate: PersonRow {
                required property var model
                chats: page.chats
                mode: "blocked"
                developer: page.backend.developerMode
                userId: model.user
                name: model.name
                width: ListView.view.width
            }
            Label {
                anchors.centerIn: parent
                opacity: 0.6
                visible: chats.blocked.count === 0
                text: qsTr("Nobody is blocked.")
            }
        }
    }

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
