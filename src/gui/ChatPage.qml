import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A direct message, laid out like Discord: no bubbles, each message a line of text under the name and time of who wrote it,
// following messages from the same person tucked under the first. Hover for reply, edit and delete.
Pane {
    id: page

    required property var chats
    property bool narrow: false
    signal back()

    padding: 0

    readonly property var messages: chats.messages
    readonly property bool canSend: chats.currentState === "active"
    property string replyId: ""
    property string replyName: ""
    property string replyText: ""
    property string editingId: ""
    property var drafts: ({})
    property string draftOwner: ""

    readonly property string timeFormat: Qt.locale().timeFormat(Locale.ShortFormat).replace(/[:.]ss/, "")
    function clock(d) { return Qt.formatTime(d, timeFormat) }
    function dayLabel(ts) {
        const d = new Date(ts), now = new Date()
        const midnight = x => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime()
        const days = Math.round((midnight(now) - midnight(d)) / 86400000)
        const time = clock(d)
        if (days === 0) return qsTr("Today at %1").arg(time)
        if (days === 1) return qsTr("Yesterday at %1").arg(time)
        return Qt.formatDate(d, Locale.ShortFormat) + " " + time
    }
    function dividerLabel(ts) {
        return new Date(ts).toLocaleDateString(Qt.locale(), Locale.LongFormat)
    }
    function startReply(m) {
        replyId = m.id; replyName = m.senderName; replyText = m.body ? m.body.replace(/\s+/g, " ").slice(0, 100) : ""
        input.forceActiveFocus()
    }
    function clearReply() { replyId = ""; replyName = ""; replyText = "" }
    function send() {
        const t = input.text.trim()
        if (t === "" || !canSend) return
        chats.send(t, replyId)
        input.text = ""
        clearReply()
        list.stickToEnd = true
    }
    // Up in an empty box edits your last message, like Discord.
    function editLast() {
        for (let i = messages.count - 1; i >= 0; i--) {
            const m = chats.messageAt(i)
            if (m.outgoing && !m.deleted) { editingId = m.id; return }
        }
    }

    // each chat keeps its own unsent text
    Connections {
        target: page.chats
        function onCurrentChanged() {
            const d = page.drafts
            if (page.draftOwner !== "") d[page.draftOwner] = input.text
            page.draftOwner = page.chats.current
            input.text = d[page.draftOwner] || ""
            page.drafts = d
            page.clearReply()
            page.editingId = ""
            list.stickToEnd = true
            if (page.chats.current !== "") input.forceActiveFocus()
        }
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
                spacing: 10
                ToolButton { objectName: "chatBack"; visible: page.narrow; text: "‹"; font.pixelSize: 22; Accessible.name: qsTr("Back"); onClicked: page.back() }
                Avatar { chats: page.chats; name: page.chats.currentName; hash: page.chats.currentAvatar; tint: page.chats.currentColor; size: 28 }
                Label { text: page.chats.currentName; font.bold: true; font.pixelSize: 16; elide: Text.ElideRight; Layout.fillWidth: true }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: palette.mid; opacity: 0.5 }
        }

        // the messages
        ListView {
            id: list
            objectName: "messageList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: page.messages
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            property bool stickToEnd: true
            readonly property bool atBottom: contentY + height >= contentHeight - 4

            onCountChanged: if (stickToEnd) Qt.callLater(positionViewAtEnd)
            onContentHeightChanged: if (stickToEnd) Qt.callLater(positionViewAtEnd)
            // moving away from the end stops following; reaching the end follows again
            onMovingChanged: if (!moving) stickToEnd = atBottom
            onContentYChanged: {
                if (moving && !atBottom) stickToEnd = false
                if (moving && contentY < 40 && page.chats.hasOlder) page.chats.loadOlder()
            }

            header: Item {
                width: list.width
                height: page.chats.hasOlder ? 40 : 0
                Label { anchors.centerIn: parent; visible: page.chats.hasOlder; opacity: 0.55; text: qsTr("Loading earlier messages…") }
            }

            delegate: Item {
                id: msg
                required property var model
                readonly property bool mine: model.outgoing === true
                readonly property bool grouped: model.grouped === true
                readonly property bool editing: page.editingId === model.id
                width: ListView.view.width
                implicitHeight: col.implicitHeight

                HoverHandler { id: hover }
                TapHandler { acceptedButtons: Qt.RightButton; onTapped: ctx.popup() }

                Menu {
                    id: ctx
                    MenuItem { text: qsTr("Copy text"); enabled: !!msg.model.body; onTriggered: page.chats.copy(msg.model.body) }
                    MenuItem { text: qsTr("Reply"); enabled: !msg.model.deleted; onTriggered: page.startReply(msg.model) }
                    MenuItem { text: qsTr("Edit"); visible: msg.mine && !msg.model.deleted; height: visible ? implicitHeight : 0; onTriggered: page.editingId = msg.model.id }
                    MenuItem { text: qsTr("Delete"); visible: msg.mine && !msg.model.deleted; height: visible ? implicitHeight : 0; onTriggered: confirm.ask(msg.model.id) }
                }

                Rectangle {
                    anchors.fill: parent
                    color: hover.hovered || msg.editing ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.05) : "transparent"
                }

                ColumnLayout {
                    id: col
                    width: parent.width
                    spacing: 0

                    // a new day
                    RowLayout {
                        visible: msg.model.dayStart === true
                        Layout.fillWidth: true
                        Layout.margins: 16
                        Layout.bottomMargin: 4
                        spacing: 10
                        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.6 }
                        Label { text: page.dividerLabel(msg.model.ts); font.pixelSize: 11; font.bold: true; opacity: 0.6 }
                        Rectangle { Layout.fillWidth: true; height: 1; color: palette.mid; opacity: 0.6 }
                    }

                    // what it replies to
                    RowLayout {
                        visible: msg.model.reply_to !== undefined
                        Layout.leftMargin: 72
                        Layout.rightMargin: 16
                        Layout.topMargin: 6
                        spacing: 6
                        Label { text: "↱"; opacity: 0.5 }
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.pixelSize: 12
                            opacity: 0.7
                            text: msg.model.replyMissing === true ? qsTr("An earlier message")
                                  : "<b>" + (msg.model.replyName || "") + "</b>  " + (msg.model.replyText === "" ? "<i>" + qsTr("Message deleted") + "</i>" : String(msg.model.replyText).replace(/&/g, "&amp;").replace(/</g, "&lt;"))
                            textFormat: Text.StyledText
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Layout.topMargin: msg.grouped ? 1 : 10
                        Layout.bottomMargin: 1
                        spacing: 12

                        // the picture sits with the first line; later lines show the time on hover
                        Item {
                            Layout.preferredWidth: 40
                            Layout.preferredHeight: msg.grouped ? 1 : 40
                            Layout.alignment: Qt.AlignTop
                            Avatar {
                                visible: !msg.grouped
                                chats: page.chats; name: msg.model.senderName; hash: msg.model.avatar; tint: msg.model.tint || ""; size: 40
                            }
                            Label {
                                visible: msg.grouped && hover.hovered
                                anchors.horizontalCenter: parent.horizontalCenter
                                font.pixelSize: 10
                                opacity: 0.6
                                text: page.clock(new Date(msg.model.ts))
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                visible: !msg.grouped
                                spacing: 8
                                Label { text: msg.model.senderName; font.bold: true }
                                Label { text: page.dayLabel(msg.model.ts); font.pixelSize: 11; opacity: 0.55 }
                            }
                            TextEdit {
                                objectName: "messageText"
                                visible: !msg.editing
                                Layout.fillWidth: true
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.Wrap
                                color: palette.text
                                font.pixelSize: 14
                                opacity: msg.model.deleted ? 0.5 : 1
                                font.italic: msg.model.deleted === true
                                text: msg.model.deleted ? qsTr("Message deleted") : (msg.model.body || "") + (msg.model.edited ? "" : "")
                                textFormat: TextEdit.PlainText
                                Accessible.role: Accessible.StaticText
                                Accessible.name: msg.model.senderName + ": " + text
                            }
                            // what became of it, when it has not been delivered yet
                            RowLayout {
                                objectName: "deliveryNote"
                                visible: msg.mine && !!msg.model.deliveryState && !msg.model.deleted
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    font.pixelSize: 11
                                    color: msg.model.deliveryState === "failed" ? "#d9534f" : msg.model.deliveryState === "held" ? "#d9a000" : palette.text
                                    opacity: msg.model.deliveryState === "failed" || msg.model.deliveryState === "held" ? 1 : 0.6
                                    text: msg.model.deliveryState === "queued" ? qsTr("Sending…")
                                          : msg.model.deliveryState === "retrying" ? qsTr("%1 can't be reached right now. Trying again at %2.").arg(page.chats.currentName).arg(page.clock(new Date(msg.model.deliveryNext)))
                                          : msg.model.deliveryState === "failed" ? qsTr("Not delivered: %1").arg(msg.model.deliveryReason || qsTr("unknown reason"))
                                          : msg.model.deliveryState === "held" ? qsTr("Not sent: %1's certificate changed.").arg(page.chats.currentName)
                                          : ""
                                }
                                Button {
                                    objectName: "retryButton"
                                    visible: msg.model.deliveryState === "failed" || msg.model.deliveryState === "retrying"
                                    text: msg.model.deliveryState === "failed" ? qsTr("Retry") : qsTr("Try now")
                                    onClicked: page.chats.retry()
                                }
                            }
                            Label {
                                visible: msg.model.edited === true && !msg.model.deleted && !msg.editing
                                text: qsTr("(edited)")
                                font.pixelSize: 10
                                opacity: 0.5
                            }
                            // editing in place
                            ColumnLayout {
                                visible: msg.editing
                                Layout.fillWidth: true
                                TextArea {
                                    id: editor
                    objectName: "editor"
                                    Layout.fillWidth: true
                                    wrapMode: TextArea.Wrap
                                    selectByMouse: true
                                    onTextChanged: if (length > 4000) remove(4000, length)
                                    text: msg.editing ? (msg.model.body || "") : ""
                                    onVisibleChanged: if (visible) { forceActiveFocus(); cursorPosition = length }
                                    Keys.onPressed: event => {
                                        if (event.key === Qt.Key_Escape) { page.editingId = ""; event.accepted = true }
                                        else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier) && !inputMethodComposing) {
                                            if (text.trim() !== "" && text.trim() !== msg.model.body) page.chats.edit(msg.model.id, text)
                                            page.editingId = ""
                                            event.accepted = true
                                        }
                                    }
                                }
                                Label { font.pixelSize: 11; opacity: 0.6; text: qsTr("Enter to save, Esc to cancel") }
                            }
                        }
                    }
                }

                // hover tools
                Row {
                    visible: hover.hovered && !msg.editing && !msg.model.deleted
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    y: msg.model.dayStart === true ? 34 : 0
                    spacing: 0
                    Rectangle {
                        width: tools.implicitWidth + 4; height: 30; radius: 6
                        color: palette.window; border.color: palette.mid
                        Row {
                            id: tools
                            anchors.centerIn: parent
                            ToolButton { objectName: "replyButton"; text: "↩"; Accessible.name: qsTr("Reply"); ToolTip.text: qsTr("Reply"); ToolTip.visible: hovered; onClicked: page.startReply(msg.model) }
                            ToolButton { objectName: "editButton"; visible: msg.mine; text: "✎"; Accessible.name: qsTr("Edit"); ToolTip.text: qsTr("Edit"); ToolTip.visible: hovered; onClicked: page.editingId = msg.model.id }
                            ToolButton { objectName: "deleteButton"; visible: msg.mine; text: "🗑"; Accessible.name: qsTr("Delete"); ToolTip.text: qsTr("Delete"); ToolTip.visible: hovered; onClicked: confirm.ask(msg.model.id) }
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 48
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                opacity: 0.55
                visible: list.count === 0
                text: qsTr("This is the start of your conversation with %1.").arg(page.chats.currentName)
            }
        }

        // jump back down after scrolling up
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 0
            Button {
                visible: !list.atBottom && list.count > 0
                anchors.right: parent.right
                anchors.rightMargin: 24
                y: -44
                text: "↓  " + qsTr("Latest")
                onClicked: { list.stickToEnd = true; list.positionViewAtEnd() }
            }
        }

        // their certificate changed: nothing can be sent until the owner decides
        Rectangle {
            visible: page.chats.currentCertChanged
            Layout.fillWidth: true
            implicitHeight: certRow.implicitHeight + 16
            color: Qt.rgba(0.85, 0.63, 0, 0.15)
            RowLayout {
                id: certRow
                anchors.fill: parent
                anchors.margins: 8
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("%1's certificate changed. Nothing is sent until you trust it.").arg(page.chats.currentName) }
                Button { text: qsTr("Trust"); onClicked: page.chats.trust(page.chats.current) }
            }
        }

        // replying to
        Rectangle {
            visible: page.replyId !== ""
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            implicitHeight: 30
            color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.08)
            radius: 6
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                Label { Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: 12; textFormat: Text.StyledText; text: qsTr("Replying to <b>%1</b>  %2").arg(page.replyName).arg(page.replyText.replace(/</g, "&lt;")) }
                ToolButton { text: "✕"; Accessible.name: qsTr("Cancel reply"); onClicked: page.clearReply() }
            }
        }

        // the box
        Rectangle {
            Layout.fillWidth: true
            Layout.margins: 16
            Layout.topMargin: 8
            implicitHeight: Math.min(input.implicitHeight, 160) + 4
            radius: 8
            color: Qt.darker(palette.window, 1.06)
            opacity: page.canSend ? 1 : 0.6

            ScrollView {
                anchors.fill: parent
                TextArea {
                    id: input
                    objectName: "composer"
                    enabled: page.canSend
                    placeholderText: page.canSend ? qsTr("Message %1").arg(page.chats.currentName)
                                     : page.chats.currentState === "closed" ? qsTr("This stream is closed. You can read the history.")
                                     : qsTr("You can write once %1 has accepted.").arg(page.chats.currentName)
                    wrapMode: TextArea.Wrap
                    selectByMouse: true
                    onTextChanged: if (length > 4000) remove(4000, length)
                    background: null
                    Accessible.name: qsTr("Message")
                    Keys.onPressed: event => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier) && !inputMethodComposing) {
                            page.send()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Up && text === "") {
                            page.editLast()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape && page.replyId !== "") {
                            page.clearReply()
                            event.accepted = true
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: confirm
        property string target: ""
        function ask(id) { target = id; open() }
        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Delete message")
        standardButtons: Dialog.Cancel | Dialog.Ok
        Label { text: qsTr("Delete this message for both of you?") }
        onAccepted: page.chats.remove(target)
    }
}
