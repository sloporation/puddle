import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"

// Who is in the open group, and what can be done about it: rename it, ask more friends, leave.
Item {
    id: root

    required property var chats
    property bool developer: false // Developer Mode: also offer the id
    signal openChat(string userId)  // a member who is a friend was chosen from the menu
    function open() { pop.open() }
    function close() { pop.close() }

    Popup {
        id: pop

        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, Overlay.overlay ? Overlay.overlay.width - 24 : 460)
        height: Math.min(560, Overlay.overlay ? Overlay.overlay.height - 24 : 560)
        padding: 20
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        readonly property int memberCount: root.chats.members.count // the list below is worked out again when this changes

        Connections {
            target: root.chats
            function onGroupLeft() { pop.close() }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Label { text: root.chats.currentName; font.bold: true; font.pixelSize: 20; elide: Text.ElideRight; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: renameField
                    objectName: "renameField"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Rename the group")
                    selectByMouse: true
                    onTextChanged: if (length > 100) remove(100, length)
                    onAccepted: renameButton.clicked()
                    FieldMenu {}
                }
                Button {
                    id: renameButton
                    objectName: "renameButton"
                    text: qsTr("Rename")
                    enabled: renameField.text.trim() !== ""
                    onClicked: { root.chats.renameGroup(renameField.text); renameField.text = "" }
                }
            }

            Label { text: qsTr("Members"); font.bold: true; opacity: 0.8 }
            ScrollList {
                objectName: "memberList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 80
                model: root.chats.members
                delegate: Item {
                    id: mrow
                    required property var model
                    readonly property bool me: model.isMe === true
                    width: ListView.view.width
                    height: 44
                    RowLayout {
                        anchors.fill: parent
                        spacing: 12
                        Avatar { chats: root.chats; name: mrow.model.name; hash: mrow.model.avatar || ""; tint: mrow.model.color || ""; size: 32 }
                        Label { text: mrow.model.name; Layout.fillWidth: true; elide: Text.ElideRight }
                        Label { visible: mrow.model.state === "invited"; text: qsTr("invited"); opacity: 0.6; font.pixelSize: 12 }
                    }
                    ContextTrigger { active: !mrow.me || root.developer; onRequested: (x, y) => memberMenu.popup(mrow, x, y) }
                    Menu {
                        id: memberMenu
                        MenuItem {
                            objectName: "memberMessage"; text: qsTr("Message")
                            visible: !mrow.me && mrow.model.friend === true; height: visible ? implicitHeight : 0
                            onTriggered: root.openChat(mrow.model.id)
                        }
                        MenuItem {
                            objectName: "memberCopyId"; text: qsTr("Copy ID")
                            visible: root.developer; height: visible ? implicitHeight : 0
                            onTriggered: root.chats.copy(mrow.model.id)
                        }
                        MenuSeparator { visible: !mrow.me; height: visible ? implicitHeight : 0 }
                        MenuItem {
                            objectName: "memberBlock"; text: qsTr("Block…"); palette.text: Theme.danger
                            visible: !mrow.me; height: visible ? implicitHeight : 0
                            onTriggered: memberConfirm.ask(qsTr("Block"),
                                                           qsTr("Block %1? They are cut off at once and cannot ask again until you unblock them.").arg(mrow.model.name),
                                                           qsTr("Block"), () => root.chats.block(mrow.model.id))
                        }
                    }
                }
            }

            Label { text: qsTr("Ask a friend"); font.bold: true; opacity: 0.8 }
            ScrollList {
                objectName: "askList"
                Layout.fillWidth: true
                Layout.preferredHeight: 110
                model: root.chats.people
                delegate: RowLayout {
                    id: arow
                    required property var model
                    readonly property bool show: model.state === "active" && pop.memberCount >= 0 && !root.chats.isGroupMember(model.id)
                    width: ListView.view.width
                    height: show ? 40 : 0
                    visible: show
                    spacing: 12
                    Avatar { chats: root.chats; name: arow.model.name; hash: arow.model.avatar || ""; tint: arow.model.color || ""; size: 28 }
                    Label { text: arow.model.name; Layout.fillWidth: true; elide: Text.ElideRight }
                    Button { objectName: "inviteFriendButton"; text: qsTr("Invite"); onClicked: root.chats.inviteToGroup(arow.model.id) }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Button {
                    objectName: "leaveGroupButton"
                    text: qsTr("Leave group")
                    onClicked: leaveConfirm.open()
                }
                Item { Layout.fillWidth: true }
                Button { text: qsTr("Close"); onClicked: pop.close() }
            }
        }
    }

    ConfirmDialog { id: memberConfirm; objectName: "memberConfirm" }

    Dialog {
        id: leaveConfirm
        objectName: "leaveConfirm"
        anchors.centerIn: Overlay.overlay
        width: 380
        modal: true
        title: qsTr("Leave group")
        standardButtons: Dialog.Cancel | Dialog.Ok
        Label { text: qsTr("Leave “%1”? The others will be told. You can only come back if someone invites you again.").arg(root.chats.currentName); wrapMode: Text.Wrap; width: parent.width }
        onAccepted: root.chats.leaveGroup()
    }
}
