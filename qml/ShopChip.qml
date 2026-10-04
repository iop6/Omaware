// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

// A small labelled pill: what kind of download something is, how it's checked, a version.
Rectangle {
    id: chip
    property string text: ""
    property string iconName: ""
    property color ink: theme.colors.muted
    property string hint: ""
    implicitWidth: row.implicitWidth + 16
    implicitHeight: 22
    radius: 11
    color: Qt.rgba(ink.r, ink.g, ink.b, .12)
    border.color: Qt.rgba(ink.r, ink.g, ink.b, .28)
    Accessible.role: Accessible.StaticText
    Accessible.name: text + (hint ? ". " + hint : "")
    Row {
        id: row
        anchors.centerIn: parent
        spacing: 5
        AppIcon { visible: chip.iconName !== ""; name: chip.iconName; color: chip.ink; width: 13; height: 13; anchors.verticalCenter: parent.verticalCenter }
        Label { text: chip.text; color: chip.ink; font.pixelSize: Math.round(11 * theme.textScale); font.weight: Font.Medium; anchors.verticalCenter: parent.verticalCenter }
    }
    HoverHandler { id: hover; enabled: chip.hint !== "" }
    ToolTip.visible: hover.hovered && hint !== ""
    ToolTip.text: hint
    ToolTip.delay: 450
}
