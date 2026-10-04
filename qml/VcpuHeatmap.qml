// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One row per vCPU, one column per sample; a stronger accent means busier.
ColumnLayout {
    id: map
    property var rows: []   // [{id, values: [percent|null, ...]}]
    property var times: []
    property int capacity: 100
    property int hoverRow: -1
    property color accent: theme.colors.accent
    property int hoverCol: -1
    readonly property int count: times.length
    readonly property int rowHeight: rows.length > 16 ? 6 : rows.length > 8 ? 10 : 16
    readonly property int labelWidth: rows.length > 16 ? 0 : 58
    spacing: 8
    function ago(i) {
        if (i < 0 || i >= count)
            return "";
        const s = Math.round((times[count - 1] - times[i]) / 1000);
        return s <= 0 ? "now" : s < 60 ? s + " s ago" : Math.floor(s / 60) + " min " + (s % 60) + " s ago";
    }
    function latest(r) {
        const v = r.values || [];
        for (let i = v.length - 1; i >= 0; --i)
            if (v[i] !== null && v[i] !== undefined)
                return v[i];
        return null;
    }
    // Hidden pages skip repainting; becoming visible catches up.
    onRowsChanged: if (visible)
        canvas.requestPaint()
    onVisibleChanged: if (visible)
        canvas.requestPaint()
    Connections {
        target: theme
        function onChanged() {
            if (map.visible)
                canvas.requestPaint();
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Column {
            visible: map.labelWidth > 0
            Layout.preferredWidth: map.labelWidth
            Layout.alignment: Qt.AlignTop
            spacing: 2
            Repeater {
                model: map.labelWidth > 0 ? map.rows : []
                Label {
                    required property var modelData
                    height: map.rowHeight
                    verticalAlignment: Text.AlignVCenter
                    textFormat: Text.PlainText
                    text: "vCPU " + modelData.id
                    color: theme.colors.muted
                    font.pixelSize: Math.round(10 * theme.textScale)
                }
            }
        }
        Item {
            id: plot
            Layout.fillWidth: true
            implicitHeight: Math.max(1, map.rows.length) * (map.rowHeight + 2) - 2
            Canvas {
                id: canvas
                anchors.fill: parent
                onWidthChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    const cw = width / map.capacity;
                    for (let r = 0; r < map.rows.length; ++r) {
                        const y = r * (map.rowHeight + 2), v = map.rows[r].values || [];
                        ctx.globalAlpha = 1;
                        ctx.fillStyle = String(theme.colors.subtle);
                        ctx.fillRect(0, y, width, map.rowHeight);
                        ctx.fillStyle = String(map.accent);
                        for (let i = 0; i < v.length; ++i) {
                            if (v[i] === null || v[i] === undefined)
                                continue;
                            const x = (map.capacity - map.count + i) * cw;
                            ctx.globalAlpha = .08 + .92 * Math.min(1, v[i] / 100);
                            ctx.fillRect(x, y, Math.max(1, cw - (cw > 4 ? 1 : 0)), map.rowHeight);
                        }
                    }
                }
            }
            Rectangle {
                visible: map.hoverRow >= 0 && map.hoverCol >= 0
                x: (map.capacity - map.count + map.hoverCol) * plot.width / map.capacity - 1
                y: map.hoverRow * (map.rowHeight + 2) - 1
                width: plot.width / map.capacity + 2
                height: map.rowHeight + 2
                color: "transparent"
                border.color: theme.colors.foreground
                border.width: 1
                radius: 1
            }
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
                onPositionChanged: function (mouse) {
                    const col = Math.floor(mouse.x / plot.width * map.capacity) - (map.capacity - map.count);
                    const row = Math.floor(mouse.y / (map.rowHeight + 2));
                    map.hoverCol = col >= 0 && col < map.count ? col : -1;
                    map.hoverRow = row >= 0 && row < map.rows.length ? row : -1;
                }
                onExited: {
                    map.hoverRow = -1;
                    map.hoverCol = -1;
                }
            }
        }
        Column {
            Layout.preferredWidth: 42
            Layout.alignment: Qt.AlignTop
            spacing: 2
            visible: map.rows.length <= 16
            Repeater {
                model: map.rows.length <= 16 ? map.rows : []
                Label {
                    required property var modelData
                    readonly property var v: map.latest(modelData)
                    width: 42
                    height: map.rowHeight
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                    textFormat: Text.PlainText
                    text: v === null ? "—" : Math.round(v) + "%"
                    color: theme.colors.foreground
                    font.pixelSize: Math.round(10 * theme.textScale)
                    font.weight: Font.Medium
                }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            text: {
                if (map.hoverRow < 0 || map.hoverCol < 0)
                    return map.count === 0 ? "Collecting samples…" : "Hover a cell for its value · oldest on the left";
                const v = (map.rows[map.hoverRow].values || [])[map.hoverCol];
                return "vCPU " + map.rows[map.hoverRow].id + " · " + (v === null || v === undefined ? "no sample" : v.toFixed(1) + "% busy") + " · " + map.ago(map.hoverCol);
            }
            color: theme.colors.muted
            font.pixelSize: Math.round(11 * theme.textScale)
            elide: Text.ElideRight
        }
        Label {
            text: "Idle"
            color: theme.colors.muted
            font.pixelSize: Math.round(10 * theme.textScale)
        }
        Rectangle {
            width: 72
            height: 8
            radius: 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop {
                    position: 0
                    color: Qt.rgba(map.accent.r, map.accent.g, map.accent.b, .08)
                }
                GradientStop {
                    position: 1
                    color: theme.colors.accent
                }
            }
        }
        Label {
            text: "Busy"
            color: theme.colors.muted
            font.pixelSize: Math.round(10 * theme.textScale)
        }
    }
}
