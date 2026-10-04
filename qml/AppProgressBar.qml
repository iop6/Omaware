// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

ProgressBar {
    id: control
    implicitHeight: 6
    padding: 0
    background: Rectangle {
        radius: 3
        color: theme.colors.subtle
    }
    contentItem: Item {
        id: track
        property real travel: 0
        clip: true
        Rectangle {
            x: control.indeterminate ? track.travel * parent.width * 0.7 : 0
            width: control.indeterminate ? parent.width * 0.3 : parent.width * control.position
            height: parent.height
            radius: 3
            color: theme.colors.accent
        }
        SequentialAnimation on travel {
            running: control.indeterminate && control.visible && !theme.reducedMotion
            loops: Animation.Infinite
            NumberAnimation {
                from: 0
                to: 1
                duration: 950
                easing.type: Easing.InOutSine
            }
            NumberAnimation {
                from: 1
                to: 0
                duration: 950
                easing.type: Easing.InOutSine
            }
        }
    }
}
