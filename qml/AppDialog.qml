// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Dialog {
    id: dialog
    property string heading: ""
    property string subtitle: ""
    property string headerIcon: "settings"
    property bool dismissible: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    padding: 24
    modal: true
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: theme.reducedMotion ? 0 : 120 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: theme.reducedMotion ? 0 : 90 } }
    background: Rectangle { color: theme.colors.surface; radius: 4; border.color: theme.colors.border
        Rectangle { width: parent.width; height: 2; color: theme.colors.accent; radius: 1 }
        CornerBrackets { size: 12 }
    }
    Overlay.modal: Rectangle { color: "#99050912" }
    header: Item {
        implicitHeight: titleRow.implicitHeight + 40
        RowLayout {
            id: titleRow
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            anchors.margins: 24; spacing: 12
            Rectangle { Layout.preferredWidth: 38; Layout.preferredHeight: 38; radius: 4; color: theme.colors.accentSoft
                AppIcon { anchors.centerIn: parent; name: dialog.headerIcon; color: theme.colors.accent; width: 20; height: 20 }
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 4
                Label { text: "» " + dialog.heading; color: theme.colors.foreground; font.pixelSize: Math.round((21) * theme.textScale); font.weight: Font.DemiBold; font.letterSpacing: -0.4; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                Label { visible: text !== ""; text: dialog.subtitle; color: theme.colors.muted; font.pixelSize: Math.round((12) * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
            }
            AppButton { iconName: "close"; tone: "quiet"; hint: "Close dialog"; enabled: dialog.dismissible; onClicked: dialog.reject() }
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: theme.colors.border }
    }
}
