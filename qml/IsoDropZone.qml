// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Drag ISO files onto any part of the window to add them to the ISO folder. Shows what will happen
// while something is dragged over, the progress of a copy, and the result.
Item {
    id: zone
    objectName: "isoDropZone"
    property var library: null
    property string message: ""
    property bool failed: false
    readonly property bool hovering: drop.containsDrag
    // Only offer the drop when at least one dragged file is an ISO.
    function hasIso(urls) {
        for (const url of urls || []) if (/\.(iso|ova|qcow2)$/i.test(String(url))) return true
        return false
    }
    function show(text, bad) { message = text; failed = bad; messageTimer.restart() }
    // Also used by tests, which can't drag real files.
    function dropUrls(urls) {
        if (!library) return false
        return library.importFiles(urls)
    }

    DropArea {
        id: drop
        anchors.fill: parent
        onEntered: function(drag) {
            if (zone.hasIso(drag.urls) && zone.library && !zone.library.importing) drag.accept(Qt.CopyAction)
            else drag.accepted = false
        }
        onDropped: function(drag) { if (zone.dropUrls(drag.urls)) drag.accept(Qt.CopyAction) }
    }

    // While dragging: a quiet tint and a target card in the middle.
    Rectangle {
        anchors.fill: parent
        visible: zone.hovering
        color: Qt.rgba(theme.colors.background.r, theme.colors.background.g, theme.colors.background.b, .78)
        Rectangle {
            anchors.fill: parent; anchors.margins: 18
            radius: 8; color: "transparent"
            border.width: 2; border.color: theme.colors.accent
        }
        ColumnLayout {
            anchors.centerIn: parent
            spacing: 8
            AppIcon { Layout.alignment: Qt.AlignHCenter; width: 40; height: 40; name: "disk"; color: theme.colors.accent }
            Label { Layout.alignment: Qt.AlignHCenter; text: "Drop to add ISO or appliance media"; font.pixelSize: Math.round(20 * theme.textScale); font.weight: Font.DemiBold }
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "ISOs stay in isos/; OVA and QCOW2 disks are copied into appliances/"
                color: theme.colors.muted; font.pixelSize: Math.round(12 * theme.textScale)
            }
        }
    }

    // Copy progress and the result, in the bottom-right corner.
    Rectangle {
        objectName: "isoImportCard"
        anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 20
        width: Math.min(360 * theme.textScale, parent.width - 40)
        height: card.implicitHeight + 24
        visible: (zone.library && zone.library.importing && zone.library.importName !== "") || zone.message !== ""
        radius: 6; color: theme.colors.raised; border.color: zone.failed ? theme.colors.danger : theme.colors.border
        ColumnLayout {
            id: card
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 12
            spacing: 8
            RowLayout {
                Layout.fillWidth: true; spacing: 8
                Label {
                    Layout.fillWidth: true
                    objectName: "isoImportText"
                    text: zone.library && zone.library.importing ? "Copying " + zone.library.importName + "…" : zone.message
                    wrapMode: Text.WordWrap; textFormat: Text.PlainText
                    color: zone.failed && !(zone.library && zone.library.importing) ? theme.colors.danger : theme.colors.foreground
                }
                AppButton {
                    objectName: "isoImportClose"
                    text: zone.library && zone.library.importing ? "Cancel" : ""
                    iconName: zone.library && zone.library.importing ? "" : "close"
                    tone: "quiet"; implicitHeight: 28
                    hint: zone.library && zone.library.importing ? "" : "Dismiss"
                    onClicked: if (zone.library && zone.library.importing) zone.library.cancelImport(); else zone.message = ""
                }
            }
            AppProgressBar {
                Layout.fillWidth: true
                visible: !!zone.library && zone.library.importing
                value: zone.library ? zone.library.importProgress : 0
            }
        }
    }
    Timer { id: messageTimer; interval: 9000; onTriggered: zone.message = "" }
}
