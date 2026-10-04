// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Stacked part-to-whole bar with a labelled legend.
ColumnLayout {
    id: bar
    // [{label, value, color, text}]; the remainder up to total is left as track.
    property var segments: []
    property real total: 0
    property bool legend: true
    property int barHeight: 8
    spacing: 6
    readonly property real sum: segments.reduce(function (a, s) {
        return a + Math.max(0, s.value || 0);
    }, 0)
    readonly property real span: Math.max(total, sum)
    Accessible.role: Accessible.StaticText
    Accessible.name: segments.map(function (s) {
        return s.label + " " + (s.text || s.value);
    }).join(", ")
    Item {
        Layout.fillWidth: true
        implicitHeight: bar.barHeight
        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: theme.colors.subtle
        }
        Row {
            anchors.fill: parent
            spacing: 2
            Repeater {
                model: bar.segments
                Rectangle {
                    required property var modelData
                    required property int index
                    readonly property real share: bar.span > 0 ? Math.max(0, modelData.value || 0) / bar.span : 0
                    visible: share > 0
                    width: Math.max(3, share * (bar.width - 2 * (bar.segments.length - 1)))
                    height: parent.height
                    radius: height / 2
                    color: modelData.color
                    Behavior on width {
                        enabled: !theme.reducedMotion
                        NumberAnimation {
                            duration: 260
                            easing.type: Easing.OutCubic
                        }
                    }
                    HoverHandler {
                        id: segHover
                    }
                    ToolTip.visible: segHover.hovered
                    ToolTip.text: modelData.label + " · " + (modelData.text || modelData.value) + (bar.span > 0 ? " · " + Math.round(share * 100) + "%" : "")
                }
            }
        }
    }
    Flow {
        visible: bar.legend
        Layout.fillWidth: true
        spacing: 14
        Repeater {
            model: bar.segments
            Row {
                required property var modelData
                spacing: 6
                Rectangle {
                    width: 8
                    height: 8
                    radius: 2
                    color: modelData.color
                    anchors.verticalCenter: parent.verticalCenter
                }
                Label {
                    textFormat: Text.PlainText
                    text: modelData.label
                    color: theme.colors.muted
                    font.pixelSize: Math.round(11 * theme.textScale)
                }
                Label {
                    textFormat: Text.PlainText
                    text: modelData.text || String(modelData.value)
                    color: theme.colors.foreground
                    font.pixelSize: Math.round(11 * theme.textScale)
                    font.weight: Font.Medium
                }
            }
        }
    }
}
