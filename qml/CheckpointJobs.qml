// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Drawer {
    id: panel
    objectName: "checkpointJobs"
    parent: Overlay.overlay
    edge: Qt.BottomEdge
    width: parent.width
    height: Math.min((job.active ? 300 : 210) * theme.textScale, parent.height - 30)
    modal: false
    property var job: backend.checkpointJob || ({})
    background: Rectangle {
        color: theme.colors.raised
        border.color: theme.colors.border
        radius: 4
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12
        RowLayout {
            AppIcon {
                name: "snapshot"
                color: theme.colors.accent
            }
            Label {
                text: "Snapshot jobs"
                font.pixelSize: Math.round((20) * theme.textScale)
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
            AppButton {
                iconName: "close"
                hint: "Close jobs panel"
                onClicked: panel.close()
            }
        }
        Label {
            textFormat: Text.PlainText
            text: panel.job.phase || "No snapshot operations yet."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: !panel.job.active && panel.job.ok === false ? theme.colors.danger : theme.colors.foreground
        }
        AppProgressBar {
            visible: !!panel.job.active
            Layout.fillWidth: true
            from: 0
            to: Math.max(1, panel.job.total || 0)
            value: panel.job.completed || 0
            indeterminate: !(panel.job.total > 0)
        }
        RowLayout {
            Label {
                Layout.fillWidth: true
                color: theme.colors.muted
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: panel.job.active ? (panel.job.total > 0 ? Math.min(100, Math.round(100 * panel.job.completed / panel.job.total)) + "% · " : "") + (panel.job.rate > 0 ? (panel.job.rate / 1048576).toFixed(1) + " MiB/s · " : "") + (panel.job.eta > 0 ? "About " + Math.ceil(panel.job.eta) + "s left" : "Estimating time") : panel.job.elapsed !== undefined ? "Elapsed: " + panel.job.elapsed + "s" : ""
            }
            AppButton {
                text: panel.job.cancelRequested ? "Cancelling…" : "Cancel job"
                visible: !!panel.job.active
                enabled: !!panel.job.cancellable && !panel.job.cancelRequested
                onClicked: backend.cancelCheckpoint()
            }
        }
        Label {
            visible: !!panel.job.active
            textFormat: Text.PlainText
            text: panel.job.cancellable ? "You can browse the library and inspect VMs while this runs." : panel.job.phase === "Saving VM memory and device state" ? "The guest is paused while memory is saved. Disk copying follows in the background." : "The VM is switching state. This step cannot be cancelled."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: theme.colors.muted
            font.pixelSize: Math.round((11) * theme.textScale)
        }
        Item {
            Layout.fillHeight: true
        }
    }
}
