// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// A small "?" that opens Help at the topic about what's next to it.
AppButton {
    property string topic: ""
    iconName: "help"
    tone: "quiet"
    implicitWidth: 30
    implicitHeight: 30
    leftPadding: 6
    rightPadding: 6
    ink: theme.colors.muted
    hint: "Help"
    visible: typeof helpCenter !== "undefined"
    onClicked: helpCenter.show(topic)
}
