// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Terminal-style state tag: ● RUNNING, with a slow pulse while the VM is live.
Rectangle {
    id: badge
    property string text: "Stopped"
    property int stateCode: 5
    property color ink: stateCode === 1 ? theme.colors.success : stateCode === 3 || stateCode === 4 ? theme.colors.warning : stateCode === 6 ? theme.colors.danger : theme.colors.muted
    implicitWidth: contents.implicitWidth + 16
    implicitHeight: Math.max(24, contents.implicitHeight + 8)
    radius: 2
    color: Qt.rgba(ink.r, ink.g, ink.b, 0.10)
    border.color: Qt.rgba(ink.r, ink.g, ink.b, 0.45)
    Accessible.name: text
    RowLayout {
        id: contents
        anchors.centerIn: parent
        spacing: 7
        Rectangle {
            width: 7
            height: 7
            color: badge.ink
            SequentialAnimation on opacity {
                running: badge.stateCode === 1 && badge.visible && !theme.reducedMotion
                loops: Animation.Infinite
                NumberAnimation {
                    to: .25
                    duration: 800
                    easing.type: Easing.InOutSine
                }
                NumberAnimation {
                    to: 1
                    duration: 800
                    easing.type: Easing.InOutSine
                }
            }
        }
        Label {
            text: badge.text.toUpperCase()
            color: badge.ink
            font.family: "monospace"
            font.letterSpacing: 1
            font.pixelSize: Math.round((11) * theme.textScale)
            font.weight: Font.DemiBold
        }
    }
}
