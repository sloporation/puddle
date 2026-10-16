import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The main window, laid out like Discord: a narrow rail on the left (where servers will go later, with Friends at the
// top), the list of direct messages beside it, and the page or chat on the right. When narrow, one pane at a time.
Item {
    id: shell

    required property var backend
    readonly property var chats: backend.chats
    readonly property bool wide: width >= 720
    // "friends", "chat" or "log"
    property string page: "friends"
    property bool showContent: false
    readonly property color railColor: Qt.darker(palette.window, 1.12)
    readonly property color sideColor: Qt.darker(palette.window, 1.04)

    function openFriends() { chats.close(); page = "friends"; showContent = true }
    function openChat(id) { chats.open(id); page = "chat"; showContent = true }
    function openLog() { chats.close(); page = "log"; showContent = true }
    function addFriend() { addPopup.open() }
    function goBack() { showContent = false }

    // Messages in the open chat count as read only while someone can see the window.
    Binding {
        target: shell.chats
        property: "attentive"
        value: shell.Window.window ? (shell.Window.window.visible && shell.Window.window.active) : false
    }

    Connections {
        target: shell.chats
        function onFailed(what) { toast.show(what) }
    }

    // The window cannot reach its own Puddle: say so, rather than showing stale lists as if they were current.
    Rectangle {
        id: offline
        visible: !shell.chats.connected
        z: 5
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: offlineLabel.implicitHeight + 12
        color: "#b8860b"
        Label { id: offlineLabel; anchors.centerIn: parent; width: parent.width - 24; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; color: "white"; text: qsTr("Can't reach your Puddle. Trying again…") }
        Accessible.role: Accessible.AlertMessage
        Accessible.name: offlineLabel.text
    }

    RowLayout {
        anchors.fill: parent
        anchors.topMargin: offline.visible ? offline.height : 0
        spacing: 0

        // the rail: later it holds servers; for now only the way home
        Rectangle {
            visible: shell.wide
            Layout.fillHeight: true
            implicitWidth: 72
            color: shell.railColor
            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 12
                spacing: 8
                Rectangle {
                    id: home
                    width: 48
                    height: 48
                    radius: shell.page === "chat" ? 24 : 16
                    color: shell.page !== "chat" || homeHover.hovered ? palette.highlight : Qt.lighter(shell.railColor, 1.4)
                    Behavior on radius { NumberAnimation { duration: 120 } }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Friends")
                    Accessible.onPressAction: shell.openFriends()
                    Image {
                        anchors.centerIn: parent
                        width: 28; height: 28
                        source: "puddle.png"
                        sourceSize: Qt.size(56, 56)
                        smooth: true
                    }
                    HoverHandler { id: homeHover }
                    TapHandler { onTapped: shell.openFriends() }
                    ToolTip.visible: homeHover.hovered
                    ToolTip.text: qsTr("Friends")
                    // the unread mark, like Discord's red badge
                    Rectangle {
                        visible: shell.chats.unreadTotal + shell.chats.incomingRequests > 0
                        anchors.right: parent.right; anchors.bottom: parent.bottom
                        anchors.rightMargin: -4; anchors.bottomMargin: -4
                        width: Math.max(18, badge.implicitWidth + 8); height: 18; radius: 9
                        color: "#d9453d"
                        border.width: 3; border.color: shell.railColor
                        Label {
                            id: badge
                            anchors.centerIn: parent
                            text: shell.chats.unreadTotal + shell.chats.incomingRequests
                            color: "white"; font.pixelSize: 10; font.bold: true
                        }
                    }
                }
                Rectangle { width: 32; height: 2; radius: 1; color: palette.mid; anchors.horizontalCenter: parent.horizontalCenter }
            }
        }

        // the direct messages
        Rectangle {
            objectName: "sidebar"
            visible: shell.wide || !shell.showContent
            Layout.fillHeight: true
            Layout.fillWidth: !shell.wide
            implicitWidth: 260
            color: shell.sideColor

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 48
                    color: "transparent"
                    Label { anchors.verticalCenter: parent.verticalCenter; x: 16; text: "Puddle"; font.bold: true; font.pixelSize: 16 }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: palette.mid; opacity: 0.5 }
                }

                ItemDelegate {
                    id: friendsButton
                    objectName: "friendsButton"
                    Layout.fillWidth: true
                    Layout.margins: 8
                    Layout.bottomMargin: 0
                    highlighted: shell.page === "friends" && (shell.wide || shell.showContent)
                    Accessible.name: qsTr("Friends")
                    background: Rectangle {
                        radius: 6
                        color: friendsButton.highlighted ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.14)
                               : friendsButton.hovered ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.07) : "transparent"
                    }
                    contentItem: RowLayout {
                        spacing: 12
                        Label { text: "👥"; font.pixelSize: 18 }
                        Label { text: qsTr("Friends"); Layout.fillWidth: true; font.bold: true }
                        Rectangle {
                            visible: shell.chats.incomingRequests > 0
                            width: 20; height: 20; radius: 10; color: "#d9453d"
                            Label { anchors.centerIn: parent; text: shell.chats.incomingRequests; color: "white"; font.pixelSize: 11; font.bold: true }
                        }
                    }
                    onClicked: shell.openFriends()
                }

                Label {
                    Layout.margins: 16
                    Layout.bottomMargin: 4
                    text: qsTr("DIRECT MESSAGES")
                    font.pixelSize: 11
                    font.bold: true
                    opacity: 0.6
                }

                ListView {
                    id: dms
                    objectName: "dmList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    clip: true
                    model: shell.chats.conversations
                    ScrollBar.vertical: ScrollBar {}
                    delegate: ItemDelegate {
                        id: dm
                        required property var model
                        width: ListView.view.width
                        height: 52
                        highlighted: shell.page === "chat" && shell.chats.current === model.user
                        background: Rectangle {
                            radius: 6
                            color: dm.highlighted ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.14)
                                   : dm.hovered ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.07) : "transparent"
                        }
                        Accessible.name: model.name + (model.unread > 0 ? qsTr(", %n unread", "", model.unread) : "")
                        contentItem: RowLayout {
                            spacing: 12
                            Avatar { chats: shell.chats; name: dm.model.name; hash: dm.model.avatar; tint: dm.model.color; size: 32 }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Label {
                                    text: dm.model.name
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    font.bold: dm.model.unread > 0
                                }
                                Label {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    font.pixelSize: 11
                                    opacity: 0.6
                                    text: dm.model.last.deleted ? qsTr("Message deleted")
                                          : (dm.model.last.outgoing ? qsTr("You: ") : "") + dm.model.last.preview
                                }
                            }
                            Rectangle {
                                visible: dm.model.unread > 0
                                width: Math.max(18, unreadLabel.implicitWidth + 8); height: 18; radius: 9; color: "#d9453d"
                                Label { id: unreadLabel; anchors.centerIn: parent; text: dm.model.unread; color: "white"; font.pixelSize: 10; font.bold: true }
                            }
                        }
                        onClicked: shell.openChat(model.user)
                    }
                    Label {
                        anchors.centerIn: parent
                        width: parent.width - 32
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        opacity: 0.55
                        visible: dms.count === 0
                        text: qsTr("No conversations yet. Messages show up here.")
                    }
                }

                // you: who you appear as, your link, the activity log
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 56
                    color: Qt.darker(palette.window, 1.1)
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8
                        Avatar { chats: shell.chats; name: shell.chats.myName; hash: shell.chats.myAvatar; size: 32 }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label { text: shell.chats.myName !== "" ? shell.chats.myName : qsTr("You"); font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                            Label {
                                text: shell.chats.connected ? qsTr("Running") : qsTr("Not connected")
                                font.pixelSize: 11
                                opacity: 0.6
                            }
                        }
                        ToolButton {
                            text: "🔗"
                            Accessible.name: qsTr("Copy my link")
                            ToolTip.text: qsTr("Copy my link"); ToolTip.visible: hovered
                            enabled: backend.myLink !== ""
                            onClicked: { backend.copyText(backend.myLink); toast.show(qsTr("Link copied")) }
                        }
                        ToolButton {
                            text: "⚙"
                            Accessible.name: qsTr("Activity log")
                            ToolTip.text: qsTr("Activity log"); ToolTip.visible: hovered
                            onClicked: shell.openLog()
                        }
                    }
                }
            }
        }

        // the page
        Item {
            visible: shell.wide || shell.showContent
            Layout.fillWidth: true
            Layout.fillHeight: true

            FriendsPage {
                objectName: "friends"
                anchors.fill: parent
                visible: shell.page === "friends"
                chats: shell.chats
                backend: shell.backend
                narrow: !shell.wide
                onAddFriend: shell.addFriend()
                onOpenChat: id => shell.openChat(id)
                onBack: shell.goBack()
            }
            LogPage {
                anchors.fill: parent
                visible: shell.page === "log"
                backend: shell.backend
                narrow: !shell.wide
                onBack: shell.goBack()
            }
            ChatPage {
                objectName: "chatPage"
                anchors.fill: parent
                visible: shell.page === "chat"
                chats: shell.chats
                narrow: !shell.wide
                onBack: shell.goBack()
            }
        }
    }

    AddFriendPopup { id: addPopup; chats: shell.chats; backend: shell.backend }

    Shortcut { sequences: [StandardKey.New]; onActivated: shell.addFriend() }
    // Alt+Up / Alt+Down walk through the conversations, as in Discord
    function step(delta) {
        const n = dms.count
        if (n === 0) return
        let i = -1
        for (let k = 0; k < n; k++)
            if (chats.current === dmUser(k)) i = k
        i = i < 0 ? (delta > 0 ? 0 : n - 1) : Math.max(0, Math.min(n - 1, i + delta))
        openChat(dmUser(i))
    }
    function dmUser(row) { return chats.userAt(row) }
    Shortcut { sequences: ["Alt+Up"]; onActivated: shell.step(-1) }
    Shortcut { sequences: ["Alt+Down"]; onActivated: shell.step(1) }
    // a way to get somewhere from outside (a notification was clicked)
    function showPerson(id) { id === "" ? openFriends() : openChat(id) }

    // a short message at the bottom
    Rectangle {
        id: toast
        property string message: ""
        function show(text) { message = text; opacity = 1; hideTimer.restart() }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: Math.min(parent.width - 32, toastLabel.implicitWidth + 32)
        height: toastLabel.implicitHeight + 20
        radius: 8
        color: "#323232"
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        Accessible.role: Accessible.AlertMessage
        Accessible.name: message
        Label { id: toastLabel; anchors.centerIn: parent; width: parent.width - 32; text: toast.message; color: "white"; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter }
        Timer { id: hideTimer; interval: 4000; onTriggered: toast.opacity = 0 }
    }
}
