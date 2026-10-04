// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Deleting removes the VM with its disks and all its snapshots, so its name has to be typed first.
AppDialog {
    id: deleteDialog
    objectName: "deleteDialog"
    anchors.centerIn: parent
    width: Math.min(480, root.width - 48)
    modal: true
    padding: 24
    property string targetUuid: ""
    property string targetName: ""
    property bool running: false
    function openFor(vm) {
        targetUuid = vm.uuid;
        targetName = root.vmName(vm);
        running = (vm.stateCode || 0) !== 5;
        deleteName.text = "";
        open();
        deleteName.forceActiveFocus();
    }
    heading: "Delete this VM?"
    headerIcon: "trash"
    contentItem: ColumnLayout {
        spacing: 14
        Label {
            textFormat: Text.PlainText
            text: deleteDialog.targetName
            color: theme.colors.foreground
            font.weight: Font.DemiBold
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
        }
        Label {
            textFormat: Text.PlainText
            text: (deleteDialog.running ? "The VM is powered off first; anything unsaved inside it is lost. " : "") + "Its disks, all its snapshots, and its firmware and TPM settings are permanently deleted. This can't be undone.\n\nInstallation ISOs, appliance files and any disk another VM uses are kept."
            color: theme.colors.muted
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            textFormat: Text.PlainText
            text: "Type “" + deleteDialog.targetName + "” to confirm"
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
        }
        AppField {
            id: deleteName
            objectName: "deleteVmName"
            Accessible.name: "VM name to confirm deletion"
            Layout.fillWidth: true
            placeholderText: deleteDialog.targetName
            onAccepted: if (deleteButton.enabled)
                deleteButton.clicked()
        }
        RowLayout {
            Layout.topMargin: 8
            Item {
                Layout.fillWidth: true
            }
            AppButton {
                text: "Cancel"
                onClicked: deleteDialog.reject()
            }
            AppButton {
                id: deleteButton
                objectName: "confirmDelete"
                text: "Delete VM"
                tone: "danger"
                enabled: backend.connected && !backend.busy && deleteName.text === deleteDialog.targetName
                onClicked: {
                    backend.request("vm.delete", {
                        uuid: deleteDialog.targetUuid,
                        powerOff: deleteDialog.running
                    });
                    deleteDialog.accept();
                }
            }
        }
    }
}
