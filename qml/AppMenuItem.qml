// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

MenuItem {
    id: entry
    objectName: subMenu ? subMenu.objectName + "Entry" : ""
    implicitHeight: 38
    leftPadding: checkable ? 32 : 12
    rightPadding: subMenu ? 32 : 14
    opacity: enabled ? 1 : 0.4
    contentItem: Label {
        textFormat: Text.PlainText
        text: (entry.highlighted ? "▸ " : "  ") + entry.text
        color: theme.colors.foreground
        font.pixelSize: Math.round((13) * theme.textScale)
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: 2
        color: entry.highlighted ? theme.colors.accentSoft : "transparent"
        Rectangle {
            visible: entry.highlighted
            width: 2
            height: parent.height
            color: theme.colors.accent
        }
    }
    arrow: AppIcon {
        visible: !!entry.subMenu
        name: "next"
        width: 14
        height: 14
        x: entry.width - width - 10
        y: (entry.height - height) / 2
        color: theme.colors.muted
    }
    indicator: AppIcon {
        visible: entry.checked
        name: "check"
        width: 16
        height: 16
        x: 9
        y: (entry.height - height) / 2
        color: theme.colors.accent
    }
}
