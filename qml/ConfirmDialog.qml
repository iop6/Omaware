// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Destructive operations retain the exact VM selected when the dialog opens.
AppDialog {
    id: confirmDialog
    objectName: "confirmDialog"
    anchors.centerIn: parent
    width: Math.min(440, root.width - 48)
    modal: true
    padding: 24
    property string targetUuid: ""
    property string targetName: ""
    property bool removing: false
    property var bulkTargets: []
    function confirm(remove) {
        confirmTarget(remove, root.selected);
    }
    function confirmTarget(remove, vm) {
        targetUuid = vm.uuid;
        targetName = vm.name;
        removing = remove;
        bulkTargets = [];
        open();
    }
    function confirmBulk(uuids) {
        targetUuid = "";
        targetName = uuids.map(function (uuid) {
            const vm = root.vmByUuid(uuid);
            return vm ? root.vmName(vm) : uuid;
        }).join(", ");
        removing = false;
        bulkTargets = uuids;
        open();
    }
    heading: confirmDialog.removing ? "Remove this VM?" : "Force power off?"
    headerIcon: confirmDialog.removing ? "trash" : "power"
    contentItem: ColumnLayout {
        spacing: 16
        Label {
            textFormat: Text.PlainText
            text: confirmDialog.targetName
            color: theme.colors.foreground
            font.weight: Font.DemiBold
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
        }
        Label {
            textFormat: Text.PlainText
            text: confirmDialog.removing ? "This removes the VM definition from your library. Its disks are kept on your computer." : confirmDialog.bulkTargets.length > 1 ? "Power will be cut immediately on " + confirmDialog.bulkTargets.length + " VMs. Any unsaved work inside them will be lost." : "Power will be cut immediately. Any unsaved work inside this VM will be lost."
            color: theme.colors.muted
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        RowLayout {
            Layout.topMargin: 8
            Item {
                Layout.fillWidth: true
            }
            AppButton {
                text: "Cancel"
                onClicked: confirmDialog.reject()
            }
            AppButton {
                objectName: "confirmAction"
                text: confirmDialog.removing ? "Remove definition" : "Force off"
                tone: "danger"
                enabled: backend.connected && !backend.busy
                onClicked: {
                    if (confirmDialog.bulkTargets.length)
                        backend.bulkAction(confirmDialog.bulkTargets, "force-off");
                    else {
                        if (confirmDialog.removing)
                            root.removingUuid = confirmDialog.targetUuid;
                        backend.action(confirmDialog.targetUuid, confirmDialog.removing ? "remove" : "force-off");
                    }
                    confirmDialog.accept();
                }
            }
        }
    }
}
