import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Ask someone to be a friend with their puddle:// link, and show our own link to give out.
Popup {
    id: pop

    required property var chats
    required property var backend
    property bool narrow: false

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(460, Overlay.overlay ? Overlay.overlay.width - 24 : 460)
    padding: 20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: link.forceActiveFocus()
    onClosed: { link.text = ""; note.text = "" }

    function send() {
        const t = link.text.trim()
        if (t === "")
            return
        chats.request(t, note.text.trim(), profile.count > 1 ? profile.currentValue : "")
        close()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label { text: qsTr("Add Friend"); font.bold: true; font.pixelSize: 20 }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            text: qsTr("Paste their puddle:// link. They will see your request and decide whether to accept.")
        }
        TextField {
            id: link
            objectName: "linkField"
            Layout.fillWidth: true
            placeholderText: "puddle://…"
            selectByMouse: true
            Accessible.name: qsTr("Their link")
            onAccepted: pop.send()
        }
        TextField {
            id: note
            objectName: "noteField"
            Layout.fillWidth: true
            placeholderText: qsTr("Say who you are (optional)")
            maximumLength: 500
            selectByMouse: true
            Accessible.name: qsTr("Note")
            onAccepted: pop.send()
        }
        RowLayout {
            visible: profile.count > 1
            Layout.fillWidth: true
            Label { text: qsTr("Appear as") }
            ComboBox {
                id: profile
                Layout.fillWidth: true
                model: chats.profiles
                textRole: "display_name"
                valueRole: "id"
                Accessible.name: qsTr("Profile to show")
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Cancel"); onClicked: pop.close() }
            Button {
                id: sendButton
                objectName: "sendRequestButton"
                text: qsTr("Send Request")
                contentItem: Label { text: sendButton.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; opacity: sendButton.enabled ? 1 : 0.5 }
                background: Rectangle { radius: 6; color: sendButton.down ? "#236b3a" : "#2d7d46"; opacity: sendButton.enabled ? 1 : 0.5 }
                enabled: link.text.trim() !== ""
                onClicked: pop.send()
            }
        }

        MenuSeparator { Layout.fillWidth: true }

        Label { text: qsTr("Your link"); font.bold: true }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            visible: backend.myLink === ""
            text: qsTr("Puddle does not know which address reaches you from outside. Set “advertise:” in puddle.yaml, then restart.")
        }
        RowLayout {
            visible: backend.myLink !== ""
            Layout.fillWidth: true
            TextField {
                Layout.fillWidth: true
                readOnly: true
                text: backend.myLink
                selectByMouse: true
                Accessible.name: qsTr("Your link")
            }
            Button { text: qsTr("Copy"); onClicked: backend.copyText(backend.myLink) }
        }
    }
}
