// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

AppDialog {
    id: dialog
    objectName: "networkDialog"
    anchors.centerIn: parent
    width: Math.min(590, parent.width - 48)
    height: Math.min(removing ? 360 : adapterAdvanced.expanded ? 650 : 490, parent.height - 48)
    modal: true
    padding: 24
    closePolicy: applying ? Popup.NoAutoClose : Popup.CloseOnEscape
    property string targetUuid: ""
    property string targetName: ""
    property string revision: ""
    property var adapter: ({})
    property var choices: []
    property bool running: false
    property bool removing: false
    property bool contained: false
    function allowed(option) { return !contained || (!!option.isolation && option.isolation.isolated) }
    property bool applying: false
    property string failure: ""
    property var choice: networkPicker.currentIndex >= 0 ? choices[networkPicker.currentIndex] : ({})
    function openFor(details, nic, remove) {
        targetUuid = details.uuid
        targetName = details.name
        revision = details.revision
        adapter = JSON.parse(JSON.stringify(nic))
        choices = (details.networkOptions || []).slice()
        running = details.active
        contained = !!details.containment && !!details.containment.enabled
        removing = remove
        applying = false
        adapterAdvanced.expanded = false
        failure = ""
        let index = choices.findIndex(function(item) { return item.id === nic.networkId })
        if (nic.mac && index < 0) {
            choices.unshift({id: nic.networkId, label: nic.kind + " · " + (nic.source || "Current network"), available: true, description: "Keep the existing network source.", reason: ""})
            index = 0
        }
        // A contained VM starts from the first switch that passes isolation checks.
        if (index < 0 && contained) index = choices.findIndex(function(item) { return dialog.allowed(item) })
        networkPicker.currentIndex = index < 0 ? 0 : index
        adapterModel.currentIndex = Math.max(0, adapterModel.model.indexOf(nic.model || "virtio"))
        connectAtStart.checked = nic.linkUp === undefined ? true : nic.linkUp
        open()
    }
    Connections {
        target: backend
        function onNetworkConfigured(uuid, ok, message) {
            if (!dialog.applying || uuid !== dialog.targetUuid) return
            dialog.applying = false
            if (ok) dialog.accept()
            else dialog.failure = message
        }
    }
    heading: dialog.removing ? "Remove network adapter?" : dialog.adapter.mac ? "Edit network adapter" : "Add network adapter"
    headerIcon: "network"
    dismissible: !applying
    contentItem: Flickable {
        clip: true
        contentWidth: width
        contentHeight: form.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: AppScrollBar {}
        ColumnLayout {
            id: form
            width: parent.width
            spacing: 14
            Label { text: dialog.targetName; font.weight: Font.DemiBold; color: theme.colors.foreground; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
            ColumnLayout {
                visible: !dialog.removing
                Layout.fillWidth: true
                spacing: 10
                Label { text: "Network"; font.pixelSize: Math.round((13) * theme.textScale); color: theme.colors.muted; Layout.topMargin: 6 }
                AppSelect {
                    id: networkPicker; objectName: "networkPicker"; Layout.fillWidth: true
                    model: dialog.choices; textRole: "label"; enabled: !dialog.applying
                }
                Label { text: dialog.choice.description || ""; color: theme.colors.muted; font.pixelSize: Math.round((12) * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Label { visible: dialog.contained; objectName: "containedNotice"; text: dialog.allowed(dialog.choice) ? "Contained VM · this switch passed live isolation checks." : "Contained VM · only switches that pass live isolation checks can be used. Create an isolated switch in Networks, or remove this adapter."
                    color: dialog.allowed(dialog.choice) ? theme.colors.success : theme.colors.danger; font.pixelSize: Math.round((12) * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Label { textFormat: Text.PlainText; visible: !!dialog.choice.reason; text: dialog.choice.reason || ""; color: theme.colors.warning; font.pixelSize: Math.round((12) * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                AppDisclosure {
                    id: adapterAdvanced; objectName: "adapterAdvanced"; title: "Advanced adapter settings"
                    Label { text: dialog.adapter.mac ? "MAC address  " + dialog.adapter.mac : "A unique MAC address will be assigned automatically."; color: theme.colors.muted; font.pixelSize: Math.round((12) * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    Label { visible: !!dialog.choice.subnet; text: "Host subnet: " + (dialog.choice.subnet || ""); color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    Label { text: "Adapter model"; font.pixelSize: Math.round((13) * theme.textScale); color: theme.colors.muted; Layout.topMargin: 6 }
                    AppSelect {
                        id: adapterModel; objectName: "adapterModel"; Layout.fillWidth: true
                        model: ["virtio", "e1000e", "e1000", "rtl8139"]; enabled: !dialog.applying
                    }
                    Label { text: "Virtio for modern Linux; the others for guests without virtio drivers."; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                }
                AppCheckBox { id: connectAtStart; objectName: "connectAtStart"; text: "Connect adapter at next startup"; enabled: !dialog.applying }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: theme.colors.border; Layout.topMargin: 4 }
            Label {
                text: dialog.running ? "The change is applied to the running VM straight away if its OS supports it (most Linux and Windows guests do). Otherwise it takes effect after a full shutdown and start."
                    : dialog.removing ? "This removes the adapter. Other adapters and disks are kept."
                    : "The VM uses this when it starts."
                color: theme.colors.muted; font.pixelSize: Math.round((12) * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true
            }
            ErrorNotice { message: dialog.failure; onRecover: function(action) {
                if (action === "networks" || action === "connection" || action === "refresh") { dialog.close(); backend.showRecovery(action, dialog.targetUuid) }
                else dialog.failure = ""
            } }
        }
    }
    footer: Item {
        implicitHeight: 74
        Rectangle { width: parent.width; height: 1; color: theme.colors.border }
        RowLayout {
            anchors.fill: parent; anchors.margins: 18
            spacing: 8
            AppBusyIndicator { running: dialog.applying; visible: running; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
            Item { Layout.fillWidth: true }
            AppButton { text: "Cancel"; enabled: !dialog.applying; onClicked: dialog.reject() }
            AppButton {
                objectName: "saveAdapter"
                text: dialog.removing ? "Remove adapter" : dialog.running ? "Save for next start" : "Save adapter"
                tone: dialog.removing ? "danger" : "primary"
                enabled: !dialog.applying && backend.connected && !backend.busy && (dialog.removing || ((!!dialog.choice.available || (!!dialog.adapter.mac && dialog.adapter.networkId === dialog.choice.id)) && dialog.allowed(dialog.choice)))
                onClicked: {
                    dialog.failure = ""
                    dialog.applying = backend.configureNetwork(dialog.targetUuid, dialog.adapter.mac || "", dialog.choice.id || "", adapterModel.currentText, connectAtStart.checked, dialog.removing, dialog.revision)
                    if (!dialog.applying) dialog.failure = "Another operation is in progress. Try again when it finishes."
                }
            }
        }
    }
}
