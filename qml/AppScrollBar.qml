// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

ScrollBar {
    id: control
    padding: 2
    minimumSize: 0.08
    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 6
        radius: 3
        color: theme.colors.muted
        visible: control.size < 0.999
        opacity: control.pressed ? 0.9 : control.hovered ? 0.65 : 0.32
    }
    background: Item {}
}
