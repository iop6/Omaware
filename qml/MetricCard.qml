// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: card
    property string iconName: "cpu"
    property string label: ""
    property string value: ""
    property string detail: ""
    property real fraction: -1 // optional 0..1 meter; negative hides it
    property string fractionText: ""
    implicitHeight: Math.max(104, cardContent.implicitHeight + 30)
    color: theme.colors.surface
    border.color: theme.colors.border
    radius: 4
    ColumnLayout {
        id: cardContent
        anchors.fill: parent
        anchors.margins: 15
        spacing: 5
        RowLayout {
            Layout.fillWidth: true
            Label {
                textFormat: Text.PlainText
                text: card.label
                color: theme.colors.muted
                font.pixelSize: Math.round((11) * theme.textScale)
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            AppIcon {
                name: card.iconName
                color: theme.colors.accent
                width: 17
                height: 17
            }
        }
        Label {
            textFormat: Text.PlainText
            text: card.value
            color: theme.colors.foreground
            font.pixelSize: Math.round((22) * theme.textScale)
            font.letterSpacing: -0.5
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Label {
            textFormat: Text.PlainText
            text: card.detail
            color: theme.colors.muted
            font.pixelSize: Math.round((11) * theme.textScale)
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        UsageBar {
            visible: card.fraction >= 0
            Layout.fillWidth: true
            Layout.topMargin: 4
            barHeight: 6
            legend: false
            total: 1
            segments: [
                {
                    label: card.label,
                    value: Math.min(1, Math.max(0, card.fraction)),
                    text: card.fractionText,
                    color: theme.colors.accent
                }
            ]
        }
        Label {
            visible: card.fraction >= 0 && text !== ""
            textFormat: Text.PlainText
            text: card.fractionText
            color: theme.colors.muted
            font.pixelSize: Math.round((10) * theme.textScale)
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }
}
