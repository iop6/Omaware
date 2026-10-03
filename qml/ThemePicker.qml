// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Every theme as a small preview of itself: its window, a card, the accent and the status colors.
// The cards are created once and only recolored: Qt 6.4's layouts crash if a Repeater rebuilds its
// items while a popup is being laid out (theme.themes changes with the text size, for example).
Grid {
    id: picker
    objectName: "themePicker"
    signal picked(string key)
    columns: Math.max(1, Math.floor((width + spacing) / (112 + spacing)))
    spacing: 8
    readonly property real cellWidth: Math.floor((width - (columns - 1) * spacing) / columns)
    property var keys: []
    function sync() { const next = theme.themes.map(function(t) { return t.key }); if (next.join("|") !== keys.join("|")) keys = next }
    Component.onCompleted: sync()
    Connections { target: theme; function onChanged() { picker.sync() } }
    Repeater {
        model: picker.keys
        Rectangle {
            id: card
            required property string modelData
            readonly property var info: theme.themes.find(function(t) { return t.key === card.modelData }) || ({title: "", detail: "", swatches: ["#000", "#000", "#000", "#fff", "#0f0", "#ff0", "#f00"]})
            readonly property var c: info.swatches     // background, surface, accent, foreground, success, warning, danger
            readonly property bool current: theme.mode === modelData
            objectName: "theme_" + modelData
            width: picker.cellWidth
            // A fixed height: a card whose height followed its width (wrapped text) sent Qt 6.4's
            // layouts into a resize loop inside the Settings popup.
            height: 58 + 12 + Math.ceil(28 * theme.textScale) + 10
            radius: 8
            color: current ? theme.colors.accentSoft : cardHover.hovered ? theme.colors.subtle : "transparent"
            border.width: current ? 2 : 1
            border.color: current ? theme.colors.accent : cardHover.hovered ? theme.colors.muted : theme.colors.border
            Behavior on border.color { enabled: !theme.reducedMotion; ColorAnimation { duration: 120 } }
            Accessible.role: Accessible.RadioButton
            Accessible.name: info.title + ". " + info.detail
            Accessible.checked: current
            // A tiny window in the theme's own colors.
            Rectangle {
                id: preview
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 6
                height: 58; radius: 5; clip: true
                color: card.c[0]
                border.color: Qt.rgba(0, 0, 0, .25)
                // Sidebar strip with an accent "selected" row.
                Rectangle { x: 0; y: 0; width: 26; height: parent.height; color: Qt.darker(card.c[0], 1.15)
                    Repeater { model: 4; Rectangle { required property int index; x: 5; y: 8 + index * 11; width: 16; height: 4; radius: 2; color: index === 1 ? card.c[2] : Qt.rgba(Qt.lighter(card.c[3], 1).r, Qt.lighter(card.c[3], 1).g, Qt.lighter(card.c[3], 1).b, .25) } }
                }
                // A card with a title, a line of text and the status lights.
                Rectangle {
                    x: 32; y: 7; width: parent.width - 39; height: parent.height - 14; radius: 4
                    color: card.c[1]; border.color: Qt.rgba(Qt.lighter(card.c[3], 1).r, Qt.lighter(card.c[3], 1).g, Qt.lighter(card.c[3], 1).b, .12)
                    Rectangle { x: 7; y: 7; width: parent.width * .55; height: 5; radius: 2.5; color: card.c[3] }
                    Rectangle { x: 7; y: 16; width: parent.width * .38; height: 4; radius: 2; color: card.c[3]; opacity: .45 }
                    Row {
                        x: 7; y: parent.height - 13; spacing: 4
                        Repeater { model: [card.c[4], card.c[5], card.c[6]]; Rectangle { required property var modelData; width: 7; height: 7; radius: 3.5; color: modelData } }
                    }
                    Rectangle { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 6; width: 24; height: 10; radius: 3; color: card.c[2] }
                }
            }
            ColumnLayout {
                id: caption
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: preview.bottom; anchors.margins: 8; anchors.topMargin: 6
                spacing: 1
                RowLayout {
                    Layout.fillWidth: true; spacing: 4
                    Label { text: card.info.title; font.weight: Font.DemiBold; font.pixelSize: Math.round(12 * theme.textScale); elide: Text.ElideRight; Layout.fillWidth: true }
                    Label { visible: card.current; text: "✓"; color: theme.colors.accent; font.weight: Font.Bold; font.pixelSize: Math.round(12 * theme.textScale) }
                }
                Label { text: card.info.detail; color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale); elide: Text.ElideRight; Layout.fillWidth: true }
            }
            HoverHandler { id: cardHover; cursorShape: Qt.PointingHandCursor }
            ToolTip.visible: cardHover.hovered; ToolTip.delay: 500; ToolTip.text: card.info.detail
            TapHandler { onTapped: picker.picked(card.modelData) }
        }
    }
}
