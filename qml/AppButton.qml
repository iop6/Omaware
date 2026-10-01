// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

Button {
    id: control
    property string iconName: ""
    property string tone: "normal" // normal, primary, quiet, danger, tab
    property string hint: ""
    property bool leading: false
    property color ink: tone === "primary" ? theme.colors.accentText
        : tone === "danger" ? theme.colors.danger
        : checked ? theme.colors.accent : theme.colors.foreground
    implicitHeight: Math.max(38, contentItem.implicitHeight + 16)
    implicitWidth: Math.max(text === "" ? 38 : 72, contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: text === "" ? 10 : 14
    rightPadding: leftPadding
    topPadding: 8; bottomPadding: 8
    hoverEnabled: true
    opacity: enabled ? 1 : 0.4
    font.pixelSize: Math.round((13) * theme.textScale)
    font.weight: Font.Medium
    Accessible.name: text || hint
    ToolTip.visible: hovered && hint !== ""
    ToolTip.text: hint
    ToolTip.delay: 650
    background: Rectangle {
        radius: 4
        color: control.tone === "primary" ? theme.colors.accent
            : control.tone === "danger" ? theme.colors.dangerSoft
            : control.tone === "tab" ? (control.hovered ? theme.colors.subtle : "transparent")
            : control.checked ? theme.colors.accentSoft
            : control.down || control.hovered ? theme.colors.subtle
            : control.tone === "quiet" ? "transparent" : theme.colors.raised
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? theme.colors.accent
            : control.tone === "normal" ? theme.colors.border : "transparent"
        Rectangle { anchors.fill: parent; radius: parent.radius; color: control.ink; opacity: control.down ? 0.1 : control.hovered && (control.tone === "primary" || control.tone === "danger") ? 0.06 : 0 }
        Rectangle { visible: control.tone === "tab" && control.checked; anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; width: parent.width - 24; height: 2; radius: 1; color: theme.colors.accent }
    }
    contentItem: Item {
        implicitWidth: contents.implicitWidth
        implicitHeight: contents.implicitHeight
        Row {
            id: contents
            x: control.leading ? 0 : (parent.width - width) / 2
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            AppIcon { visible: control.iconName !== ""; name: control.iconName; color: control.ink; width: 17; height: 17; anchors.verticalCenter: parent.verticalCenter }
            Text { visible: control.text !== ""; text: control.text; font: control.font; color: control.ink; anchors.verticalCenter: parent.verticalCenter }
        }
    }
}
