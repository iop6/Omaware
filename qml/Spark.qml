// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Minimal right-aligned sparkline for dense rows; nulls leave gaps.
Canvas {
    id: spark
    property var values: []
    property int capacity: 60
    property real ceiling: 100
    property color ink: theme.colors.accent
    implicitWidth: 60
    implicitHeight: 16
    onValuesChanged: if (visible)
        requestPaint()
    onInkChanged: if (visible)
        requestPaint()
    onVisibleChanged: if (visible)
        requestPaint()
    onPaint: {
        const ctx = getContext("2d");
        ctx.reset();
        const v = values || [], top = Math.max(1e-9, ceiling);
        ctx.strokeStyle = String(ink);
        ctx.lineWidth = 1.5;
        ctx.lineJoin = "round";
        let drawing = false;
        ctx.beginPath();
        for (let i = 0; i < v.length; ++i) {
            if (v[i] === null || v[i] === undefined) {
                drawing = false;
                continue;
            }
            const x = width * (capacity - v.length + i) / Math.max(1, capacity - 1);
            const y = height - 1 - Math.min(1, v[i] / top) * (height - 2);
            if (drawing)
                ctx.lineTo(x, y);
            else
                ctx.moveTo(x, y);
            drawing = true;
        }
        ctx.stroke();
    }
}
