import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Start a group: a name, and which friends to ask. People who are not friends cannot be asked from here.
Popup {
    id: pop

    required property var chats

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(440, Overlay.overlay ? Overlay.overlay.width - 24 : 440)
    padding: 20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property var picked: ({})
    property int pickedCount: 0

    onOpened: nameField.forceActiveFocus()
    onClosed: { nameField.text = ""; picked = ({}); pickedCount = 0 }

    function create() {
        const name = nameField.text.trim()
        if (name === "")
            return
        chats.createGroup(name, Object.keys(picked).filter(k => picked[k]))
        close()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label { text: qsTr("New group"); font.bold: true; font.pixelSize: 20 }
        TextField {
            id: nameField
            objectName: "groupNameField"
            Layout.fillWidth: true
            placeholderText: qsTr("Group name")
            selectByMouse: true
            onTextChanged: if (length > 100) remove(100, length)
            Accessible.name: qsTr("Group name")
            onAccepted: pop.create()
        }
        Label { text: qsTr("Ask friends to join"); font.bold: true; opacity: 0.8 }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.65
            font.pixelSize: 12
            text: qsTr("Everyone in a group can see each other's address and connect directly, even people who are not friends. You can invite more people later.")
        }
        ListView {
            id: friends
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(240, Math.max(60, count * 44))
            clip: true
            model: pop.chats.people
            ScrollBar.vertical: ScrollBar {}
            delegate: CheckDelegate {
                id: row
                required property var model
                objectName: "friendPick"
                width: ListView.view.width
                height: model.state === "active" ? 44 : 0
                visible: model.state === "active"
                text: model.name
                checked: !!pop.picked[model.id]
                onToggled: {
                    const p = pop.picked
                    p[model.id] = checked
                    pop.picked = p
                    pop.pickedCount = Object.keys(p).filter(k => p[k]).length
                }
            }
            Label {
                anchors.centerIn: parent
                visible: pop.chats.friendCount === 0
                opacity: 0.6
                text: qsTr("You have no friends to ask yet.")
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Cancel"); onClicked: pop.close() }
            Button {
                id: createButton
                objectName: "createGroupButton"
                text: pop.pickedCount > 1 ? qsTr("Create and invite %1 friends").arg(pop.pickedCount) : pop.pickedCount === 1 ? qsTr("Create and invite 1 friend") : qsTr("Create")
                enabled: nameField.text.trim() !== ""
                contentItem: Label { text: createButton.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; opacity: createButton.enabled ? 1 : 0.5 }
                background: Rectangle { radius: 6; color: createButton.down ? "#236b3a" : "#2d7d46"; opacity: createButton.enabled ? 1 : 0.5 }
                onClicked: pop.create()
            }
        }
    }
}
