// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
CheckBox {
    id: control
    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
    implicitHeight: Math.max(34, contentItem.implicitHeight + 12)
    leftPadding: 30; rightPadding: 4; topPadding: 6; bottomPadding: 6
    spacing: 10
    font.pixelSize: Math.round((13) * theme.textScale)
    hoverEnabled: true
    opacity: enabled ? 1 : 0.45
    indicator: Rectangle {
        x: 0; y: (control.height - height) / 2
        width: 20; height: 20; radius: 4
        color: control.checked ? theme.colors.accent : theme.colors.field
        border.color: control.checked || control.visualFocus ? theme.colors.accent : control.hovered ? theme.colors.muted : theme.colors.border
        border.width: control.visualFocus ? 2 : 1
        AppIcon { anchors.centerIn: parent; name: "check"; width: 14; height: 14; visible: control.checked; color: theme.colors.accentText }
    }
    contentItem: Label { text: control.text; font: control.font; color: theme.colors.foreground; wrapMode: Text.WordWrap; verticalAlignment: Text.AlignVCenter }
}
