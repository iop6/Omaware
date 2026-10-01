// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
RowLayout {
    property string title: ""
    property string caption: ""
    property string iconName: ""
    id: section
    spacing: 10
    Layout.fillWidth: true
    Layout.topMargin: 10
    Layout.bottomMargin: 2
    AppIcon { visible: section.iconName !== ""; name: section.iconName; color: theme.colors.accent; width: 17; height: 17 }
    ColumnLayout {
        Layout.fillWidth: true; spacing: 3
        Label { text: "<font color=\"" + theme.colors.accent + "\">//</font> " + section.title; textFormat: Text.StyledText; color: theme.colors.foreground; font.pixelSize: Math.round((14) * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        Label { visible: text !== ""; text: section.caption; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
    }
}
