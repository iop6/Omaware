// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

ComboBox {
    id: control
    implicitHeight: 42
    font.pixelSize: Math.round((13) * theme.textScale)
    leftPadding: 13
    rightPadding: 38
    hoverEnabled: true
    opacity: enabled ? 1 : 0.45
    contentItem: Label {
        textFormat: Text.PlainText
        text: control.displayText
        font: control.font
        color: theme.colors.foreground
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: AppIcon {
        name: "chevron"
        color: theme.colors.muted
        x: control.width - width - 13
        y: (control.height - height) / 2
    }
    background: Rectangle {
        radius: 4
        color: theme.colors.field
        border.color: control.visualFocus || control.popup.visible ? theme.colors.accent : control.hovered ? theme.colors.muted : theme.colors.border
        border.width: control.visualFocus ? 2 : 1
    }
    delegate: ItemDelegate {
        id: option
        required property var modelData
        required property int index
        objectName: "selectOption_" + control.objectName + "_" + index
        width: control.popup.width - 12
        implicitHeight: 40
        text: control.textRole ? modelData[control.textRole] : modelData
        highlighted: control.highlightedIndex === index
        leftPadding: 12
        rightPadding: 30
        contentItem: Label {
            textFormat: Text.PlainText
            text: option.text
            font.pixelSize: Math.round((13) * theme.textScale)
            color: theme.colors.foreground
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 4
            color: option.highlighted ? theme.colors.accentSoft : "transparent"
        }
        AppIcon {
            name: "check"
            width: 16
            height: 16
            visible: control.currentIndex === option.index
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            color: theme.colors.accent
        }
    }
    popup.padding: 6
    popup.background: Rectangle {
        color: theme.colors.raised
        border.color: theme.colors.border
        radius: 4
    }
}
