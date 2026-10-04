// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

// Right-aligned rolling line chart. A null value marks a gap in sampling.
Item {
    id: chart
    // [{label, values: [..], color, dashed, fill}]
    property var series: []
    property var times: []
    property int capacity: 100
    property real ceiling: 0 // fixed maximum; 0 scales to the data
    property real minimumCeiling: 1
    property var format: function (v) {
        return String(v);
    }
    property bool showScale: true
    property int hoverIndex: -1
    readonly property int count: times.length
    readonly property real upper: {
        if (ceiling > 0)
            return ceiling;
        let peak = 0;
        for (const s of series)
            for (const v of (s.values || []))
                if (v !== null && v !== undefined && v > peak)
                    peak = v;
        return Math.max(minimumCeiling, peak * 1.15);
    }
    implicitHeight: 72
    implicitWidth: 240
    Accessible.role: Accessible.Chart
    Accessible.name: series.map(function (s) {
        const v = s.values || [];
        return s.label + " " + (v.length ? chart.format(v[v.length - 1]) : "no data");
    }).join(", ")

    function xAt(i) {
        return width * (capacity - count + i) / Math.max(1, capacity - 1);
    }
    function yAt(v) {
        return plot.y + plot.height - Math.min(1, Math.max(0, v / upper)) * plot.height;
    }
    function ago(i) {
        if (i < 0 || i >= count)
            return "";
        const s = Math.round((times[count - 1] - times[i]) / 1000);
        return s <= 0 ? "Now" : s < 60 ? s + " s ago" : Math.floor(s / 60) + " min " + (s % 60) + " s ago";
    }
    onSeriesChanged: if (visible)
        canvas.requestPaint()
    onUpperChanged: if (visible)
        canvas.requestPaint()
    onWidthChanged: if (visible)
        canvas.requestPaint()
    onHeightChanged: if (visible)
        canvas.requestPaint()
    onVisibleChanged: if (visible)
        canvas.requestPaint()
    Connections {
        target: theme
        function onChanged() {
            if (chart.visible)
                canvas.requestPaint();
        }
    }

    Item {
        id: plot
        x: 0
        y: 4
        width: chart.width
        height: chart.height - 6
    }
    Canvas {
        id: canvas
        anchors.fill: parent
        renderTarget: Canvas.Image
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            // Recessive grid: baseline and a dotted midline.
            ctx.strokeStyle = String(theme.colors.line);
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(0, plot.y + plot.height + .5);
            ctx.lineTo(width, plot.y + plot.height + .5);
            ctx.stroke();
            ctx.setLineDash([2, 4]);
            ctx.beginPath();
            ctx.moveTo(0, plot.y + plot.height / 2 + .5);
            ctx.lineTo(width, plot.y + plot.height / 2 + .5);
            ctx.stroke();
            ctx.setLineDash([]);
            for (let si = chart.series.length - 1; si >= 0; --si) {
                const s = chart.series[si], values = s.values || [];
                // Build runs of consecutive samples so gaps stay visible.
                let runs = [], run = [];
                for (let i = 0; i < values.length; ++i) {
                    if (values[i] === null || values[i] === undefined) {
                        if (run.length)
                            runs.push(run);
                        run = [];
                        continue;
                    }
                    run.push([chart.xAt(i), chart.yAt(values[i])]);
                }
                if (run.length)
                    runs.push(run);
                for (const r of runs) {
                    if (s.fill && r.length > 1) {
                        ctx.beginPath();
                        ctx.moveTo(r[0][0], plot.y + plot.height);
                        for (const p of r)
                            ctx.lineTo(p[0], p[1]);
                        ctx.lineTo(r[r.length - 1][0], plot.y + plot.height);
                        ctx.closePath();
                        const c = Qt.lighter(s.color, 1.0), g = ctx.createLinearGradient(0, plot.y, 0, plot.y + plot.height);
                        g.addColorStop(0, Qt.rgba(c.r, c.g, c.b, .28));
                        g.addColorStop(1, Qt.rgba(c.r, c.g, c.b, .02));
                        ctx.fillStyle = g;
                        ctx.fill();
                    }
                    ctx.strokeStyle = String(s.color);
                    ctx.lineWidth = 2;
                    ctx.lineJoin = "round";
                    ctx.lineCap = "round";
                    ctx.setLineDash(s.dashed ? [4, 3] : []);
                    ctx.beginPath();
                    ctx.moveTo(r[0][0], r[0][1]);
                    for (const p of r)
                        ctx.lineTo(p[0], p[1]);
                    if (r.length === 1)
                        ctx.lineTo(r[0][0] + .1, r[0][1]);
                    ctx.stroke();
                    ctx.setLineDash([]);
                }
            }
        }
    }
    Label {
        visible: chart.showScale && chart.count > 0
        anchors.right: parent.right
        y: 0
        textFormat: Text.PlainText
        text: chart.format(chart.upper)
        color: theme.colors.muted
        opacity: .8
        font.pixelSize: Math.round(10 * theme.textScale)
    }
    Label {
        visible: chart.count === 0
        anchors.centerIn: parent
        text: "Collecting samples…"
        color: theme.colors.muted
        font.pixelSize: Math.round(11 * theme.textScale)
    }
    // Hover layer: crosshair, point markers and a readout.
    MouseArea {
        id: hover
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        onPositionChanged: function (mouse) {
            const i = Math.round(mouse.x / chart.width * (chart.capacity - 1)) - (chart.capacity - chart.count);
            chart.hoverIndex = i >= 0 && i < chart.count ? i : -1;
        }
        onExited: chart.hoverIndex = -1
    }
    Rectangle {
        visible: chart.hoverIndex >= 0
        x: Math.round(chart.xAt(chart.hoverIndex))
        y: plot.y
        width: 1
        height: plot.height
        color: theme.colors.muted
        opacity: .6
    }
    Repeater {
        model: chart.hoverIndex >= 0 ? chart.series : []
        Rectangle {
            required property var modelData
            readonly property var v: (modelData.values || [])[chart.hoverIndex]
            visible: v !== null && v !== undefined
            width: 8
            height: 8
            radius: 4
            x: chart.xAt(chart.hoverIndex) - 4
            y: chart.yAt(v || 0) - 4
            color: modelData.color
            border.width: 2
            border.color: theme.colors.surface
        }
    }
    Rectangle {
        id: tip
        visible: chart.hoverIndex >= 0
        z: 5
        width: tipColumn.implicitWidth + 16
        height: tipColumn.implicitHeight + 10
        x: {
            const px = chart.xAt(chart.hoverIndex);
            return px + width + 12 > chart.width ? px - width - 10 : px + 10;
        }
        y: -height - 4
        radius: 4
        color: theme.colors.raised
        border.color: theme.colors.border
        Column {
            id: tipColumn
            x: 8
            y: 5
            spacing: 2
            Label {
                textFormat: Text.PlainText
                text: chart.ago(chart.hoverIndex)
                color: theme.colors.muted
                font.pixelSize: Math.round(10 * theme.textScale)
            }
            Repeater {
                model: chart.hoverIndex >= 0 ? chart.series : []
                Row {
                    required property var modelData
                    spacing: 6
                    Rectangle {
                        width: 10
                        height: 2
                        radius: 1
                        color: modelData.color
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Label {
                        readonly property var v: (modelData.values || [])[chart.hoverIndex]
                        textFormat: Text.PlainText
                        text: modelData.label + "  " + (v === null || v === undefined ? "No sample" : chart.format(v))
                        color: theme.colors.foreground
                        font.pixelSize: Math.round(11 * theme.textScale)
                    }
                }
            }
        }
    }
}
