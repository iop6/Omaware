// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// HUD-style corner marks drawn just inside a surface's border.
Item {
    id: marks
    property color ink: theme.colors.accent
    property int size: 10
    property int weight: 2
    anchors.fill: parent
    z: 10
    Repeater {
        model: 4
        Item {
            required property int index
            readonly property bool onRight: index % 2 === 1
            readonly property bool onBottom: index > 1
            x: onRight ? marks.width - marks.size : 0
            y: onBottom ? marks.height - marks.size : 0
            width: marks.size; height: marks.size
            Rectangle { x: 0; y: parent.onBottom ? parent.height - marks.weight : 0; width: parent.width; height: marks.weight; color: marks.ink }
            Rectangle { x: parent.onRight ? parent.width - marks.weight : 0; y: 0; width: marks.weight; height: parent.height; color: marks.ink }
        }
    }
}
