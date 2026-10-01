// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
TextField {
    id: field
    implicitHeight: Math.max(42, contentHeight + 20)
    font.pixelSize: Math.round((13) * theme.textScale)
    color: theme.colors.foreground
    placeholderTextColor: theme.colors.muted
    selectionColor: theme.colors.accent
    selectedTextColor: theme.colors.accentText
    leftPadding: 13; rightPadding: 13
    hoverEnabled: true
    opacity: enabled ? 1 : 0.45
    background: Rectangle {
        radius: 4; color: field.readOnly ? theme.colors.subtle : theme.colors.field
        border.color: field.activeFocus ? theme.colors.accent : field.hovered ? theme.colors.muted : theme.colors.border
        border.width: field.activeFocus ? 2 : 1
    }
}
