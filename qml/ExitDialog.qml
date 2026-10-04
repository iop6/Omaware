// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shown while running VMs are paused on close; stays open only if some could not be paused.
AppDialog {
    id: exitDialog
    objectName: "exitDialog"
    property int count: 0
    property var failures: []
    width: Math.min(460, parent.width - 40)
    dismissible: false
    closePolicy: Popup.NoAutoClose
    headerIcon: "pause"
    heading: failures.length ? "Some VMs are still running" : "Pausing VMs"
    subtitle: failures.length ? "The rest were paused." : "Pausing " + count + (count === 1 ? " running VM" : " running VMs") + " so they continue where they left off"
    contentItem: ColumnLayout {
        spacing: 14
        RowLayout {
            visible: exitDialog.failures.length === 0
            spacing: 10
            AppBusyIndicator {
                running: exitDialog.visible && exitDialog.failures.length === 0
                implicitWidth: 22
                implicitHeight: 22
            }
            Label {
                text: backend.busy ? "Waiting for the current operation to finish…" : "OmaWare closes as soon as they are paused."
                color: theme.colors.muted
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
        Repeater {
            model: exitDialog.failures
            Label {
                required property var modelData
                textFormat: Text.PlainText
                text: modelData
                color: theme.colors.danger
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
        RowLayout {
            visible: exitDialog.failures.length > 0
            Layout.topMargin: 6
            Item {
                Layout.fillWidth: true
            }
            AppButton {
                objectName: "exitStay"
                text: "Keep OmaWare open"
                onClicked: {
                    root.pausingForExit = false;
                    exitDialog.close();
                }
            }
            AppButton {
                objectName: "exitAnyway"
                text: "Close anyway"
                tone: "primary"
                onClicked: {
                    root.exiting = true;
                    exitDialog.close();
                    Qt.callLater(root.close);
                }
            }
        }
    }
}
