// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A headline value with its rolling trend and optional extra rows.
Rectangle {
    id: tile
    default property alias extra: extraColumn.data
    property string iconName: "cpu"
    property string label: ""
    property string value: "—"
    property string detail: ""
    property alias chart: trend
    property alias series: trend.series
    property alias times: trend.times
    property alias capacity: trend.capacity
    property alias ceiling: trend.ceiling
    property alias minimumCeiling: trend.minimumCeiling
    property alias format: trend.format
    implicitHeight: content.implicitHeight + 30
    color: theme.colors.surface
    border.color: theme.colors.border
    radius: 4
    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 15
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            AppIcon {
                name: tile.iconName
                color: theme.colors.accent
                width: 16
                height: 16
            }
            Label {
                text: tile.label
                color: theme.colors.muted
                font.pixelSize: Math.round(11 * theme.textScale)
                font.weight: Font.Medium
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            // Legend for multi-series charts; line style repeats the chart's encoding.
            Repeater {
                model: trend.series.length > 1 ? trend.series : []
                Row {
                    required property var modelData
                    spacing: 5
                    Canvas {
                        width: 14
                        height: 8
                        anchors.verticalCenter: parent.verticalCenter
                        property color ink: modelData.color
                        onInkChanged: requestPaint()
                        onPaint: {
                            const c = getContext("2d");
                            c.reset();
                            c.strokeStyle = String(ink);
                            c.lineWidth = 2;
                            c.setLineDash(modelData.dashed ? [3, 2] : []);
                            c.beginPath();
                            c.moveTo(0, 4);
                            c.lineTo(14, 4);
                            c.stroke();
                        }
                    }
                    Label {
                        text: modelData.label
                        color: theme.colors.muted
                        font.pixelSize: Math.round(11 * theme.textScale)
                    }
                }
            }
        }
        Label {
            text: tile.value
            color: theme.colors.foreground
            font.pixelSize: Math.round(24 * theme.textScale)
            font.letterSpacing: -0.5
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Label {
            visible: text !== ""
            text: tile.detail
            color: theme.colors.muted
            font.pixelSize: Math.round(11 * theme.textScale)
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        TrendChart {
            id: trend
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 74
            Layout.preferredHeight: 74
            Layout.topMargin: 6
        }
        ColumnLayout {
            id: extraColumn
            Layout.fillWidth: true
            spacing: 6
            visible: children.length > 0
        }
    }
}
