// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

Item {
    id: row
    property string label: ""
    property string value: ""
    implicitHeight: Math.max(18, labelText.contentHeight, valueText.contentHeight) + 20
    implicitWidth: 360
    Rectangle {
        width: parent.width
        height: 1
        color: theme.colors.line
    }
    Label {
        id: labelText
        textFormat: Text.PlainText
        x: 0
        y: 10
        width: parent.width < 500 ? 124 : 154
        text: row.label
        color: theme.colors.muted
        font.pixelSize: Math.round((12) * theme.textScale)
        wrapMode: Text.WordWrap
    }
    TextEdit {
        id: valueText
        x: parent.width < 500 ? 134 : 164
        y: 10
        width: Math.max(80, parent.width - x)
        height: contentHeight
        text: row.value || "Not reported"
        readOnly: true
        textFormat: TextEdit.PlainText
        selectByMouse: true
        wrapMode: TextEdit.WrapAnywhere
        font.pixelSize: Math.round((12) * theme.textScale)
        color: theme.colors.foreground
        selectionColor: theme.colors.accent
        selectedTextColor: theme.colors.accentText
        Accessible.name: row.label + ": " + text
    }
}
