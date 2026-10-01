// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
TextArea {
    textFormat: TextEdit.PlainText
    id: field
    padding: 13
    font.pixelSize: Math.round((13) * theme.textScale)
    color: theme.colors.foreground
    placeholderTextColor: theme.colors.muted
    selectionColor: theme.colors.accent
    selectedTextColor: theme.colors.accentText
    wrapMode: TextEdit.WordWrap
    background: Rectangle { radius: 4; color: theme.colors.field; border.color: field.activeFocus ? theme.colors.accent : theme.colors.border; border.width: field.activeFocus ? 2 : 1 }
}
