import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme"
import "../chat"
import "../dialogs"
import "../groups"
import "../settings"

// The main window: a narrow rail on the left (where servers will go later), the list of conversations beside it, and the open conversation, or the
// settings, on the right. When narrow, one pane at a time: the rail and the list, or the conversation.
Item {
    id: shell

    required property var backend
    readonly property var chats: backend.chats
    readonly property bool wide: width >= Theme.wideBreakpoint
    // "home" (nothing open), "chat" or "log" (the settings, for now)
    property string page: "home"
    property bool showContent: false
    readonly property color railColor: Qt.darker(palette.window, 1.12)
    readonly property color sideColor: Qt.darker(palette.window, 1.04)

    function goHome() { chats.close(); page = "home"; showContent = false }
    function openChat(id, kind) { chats.open(id, kind || "dm"); page = "chat"; showContent = true }
    function openLog() { chats.close(); page = "log"; showContent = true }
    function newConversation() { newConv.show(0) }
    function addFriend() { newConv.show(1) }
    function goBack() { showContent = false }

    // Messages in the open chat count as read only while someone can see the window.
    Binding {
        target: shell.chats
        property: "attentive"
        value: shell.Window.window ? (shell.Window.window.visible && shell.Window.window.active) : false
    }

    Connections {
        target: shell.chats
        // the open conversation went away (the group was left): nothing to show but the empty page
        function onCurrentChanged() { if (shell.page === "chat" && shell.chats.current === "") shell.goHome() }
        function onFailed(what) { toast.show(what) }
        function onGroupCreated(id) { shell.openChat(id, "group") }
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
        color: Theme.banner
        Label { id: offlineLabel; anchors.centerIn: parent; width: parent.width - 24; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; color: "white"; text: qsTr("Can't reach your Puddle. Trying again…") }
        Accessible.role: Accessible.AlertMessage
        Accessible.name: offlineLabel.text
    }

    RowLayout {
        anchors.fill: parent
        anchors.topMargin: offline.visible ? offline.height : 0
        spacing: 0

        // the rail: later it holds servers; for now only the way home. When narrow it stays beside the list, and goes with it when a conversation opens
        Rectangle {
            objectName: "rail"
            visible: shell.wide || !shell.showContent
            Layout.fillHeight: true
            implicitWidth: Theme.railWidth
            color: shell.railColor
            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 12
                spacing: 8
                Rectangle {
                    id: home
                    objectName: "homeButton"
                    width: 48
                    height: 48
                    radius: shell.page === "chat" ? 24 : 16
                    color: shell.page !== "chat" || homeHover.hovered ? palette.highlight : Qt.lighter(shell.railColor, 1.4)
                    Behavior on radius { NumberAnimation { duration: 120 } }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Home")
                    Accessible.onPressAction: shell.goHome()
                    Image {
                        anchors.centerIn: parent
                        width: 28; height: 28
                        source: "../../assets/puddle.png"
                        sourceSize: Qt.size(56, 56)
                        smooth: true
                    }
                    HoverHandler { id: homeHover }
                    TapHandler { onTapped: shell.goHome() }
                    ToolTip.visible: homeHover.hovered
                    ToolTip.text: qsTr("Home")
                    // the unread mark, like Discord's red badge
                    Rectangle {
                        visible: shell.chats.unreadTotal + shell.chats.incomingRequests + shell.chats.incomingGroupInvites > 0
                        anchors.right: parent.right; anchors.bottom: parent.bottom
                        anchors.rightMargin: -4; anchors.bottomMargin: -4
                        width: Math.max(18, badge.implicitWidth + 8); height: 18; radius: 9
                        color: Theme.badge
                        border.width: 3; border.color: shell.railColor
                        Label {
                            id: badge
                            anchors.centerIn: parent
                            text: shell.chats.unreadTotal + shell.chats.incomingRequests + shell.chats.incomingGroupInvites
                            color: "white"; font.pixelSize: 10; font.bold: true
                        }
                    }
                }
                Rectangle { width: 32; height: 2; radius: 1; color: palette.mid; anchors.horizontalCenter: parent.horizontalCenter }
            }
        }

        // the conversations
        Sidebar {
            id: sidebar
            visible: shell.wide || !shell.showContent
            Layout.fillHeight: true
            Layout.fillWidth: !shell.wide
            implicitWidth: Theme.sidebarWidth
            color: shell.sideColor
            chats: shell.chats
            backend: shell.backend
            selectedId: shell.page === "chat" ? shell.chats.current : ""
            onOpenChat: (id, kind) => shell.openChat(id, kind)
            onNewConversation: shell.newConversation()
            onOpenSettings: shell.openLog()
            onNotice: text => toast.show(text)
        }

        // the page
        Item {
            visible: shell.wide || shell.showContent
            Layout.fillWidth: true
            Layout.fillHeight: true

            HomePage {
                objectName: "homePage"
                anchors.fill: parent
                visible: shell.page === "home"
                chats: shell.chats
                narrow: !shell.wide
                onNewConversation: shell.newConversation()
                onAddFriend: shell.addFriend()
            }
            LogPage {
                anchors.fill: parent
                visible: shell.page === "log"
                backend: shell.backend
                chats: shell.chats
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
                onShowGroup: groupPopup.open()
            }
        }
    }

    NewConversationDialog {
        id: newConv
        objectName: "newConversationDialog"
        chats: shell.chats
        backend: shell.backend
        onStartDm: id => shell.openChat(id, "dm")
    }
    GroupPopup {
        id: groupPopup
        objectName: "groupPopup"
        chats: shell.chats
        developer: shell.backend.developerMode
        onOpenChat: id => { groupPopup.close(); shell.openChat(id) }
    }

    Shortcut { sequences: [StandardKey.New]; onActivated: shell.newConversation() }
    // Alt+Up / Alt+Down walk through the conversations, as in Discord
    Shortcut { sequences: ["Alt+Up"]; onActivated: sidebar.step(-1) }
    Shortcut { sequences: ["Alt+Down"]; onActivated: sidebar.step(1) }
    // a way to get somewhere from outside (a notification was clicked)
    function showPerson(id) { id === "" ? goHome() : openChat(id) }

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
        radius: Theme.radiusLarge
        color: Theme.toast
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        Accessible.role: Accessible.AlertMessage
        Accessible.name: message
        Label { id: toastLabel; anchors.centerIn: parent; width: parent.width - 32; text: toast.message; color: "white"; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter }
        Timer { id: hideTimer; interval: 4000; onTriggered: toast.opacity = 0 }
    }
}
