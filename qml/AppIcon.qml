// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Original, resolution-independent line icons. No font or image dependency.
Canvas {
    id: icon
    property string name: "monitor"
    property color color: theme.colors.foreground
    implicitWidth: 20
    implicitHeight: 20
    onNameChanged: requestPaint()
    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.scale(width / 24, height / 24)
        ctx.strokeStyle = color
        ctx.fillStyle = color
        ctx.lineWidth = 1.6
        ctx.lineCap = "round"
        ctx.lineJoin = "round"
        function line(points) {
            ctx.beginPath(); ctx.moveTo(points[0], points[1])
            for (let i = 2; i < points.length; i += 2) ctx.lineTo(points[i], points[i + 1])
            ctx.stroke()
        }
        function box(x, y, w, h) { ctx.strokeRect(x, y, w, h) }
        function circle(x, y, r) { ctx.beginPath(); ctx.arc(x, y, r, 0, Math.PI * 2); ctx.stroke() }
        switch (name) {
        case "folder": line([3, 6, 10, 6, 12, 9, 21, 9, 21, 20, 3, 20, 3, 6]); break
        case "edit": line([4, 16, 16, 4, 20, 8, 8, 20, 3, 21, 4, 16]); line([13, 7, 17, 11]); break
        case "settings": line([4, 6, 20, 6]); line([4, 12, 20, 12]); line([4, 18, 20, 18]); circle(9, 6, 2); circle(16, 12, 2); circle(8, 18, 2); break
        case "clipboard": box(6, 5, 14, 16); box(9, 2, 8, 5); line([9, 12, 16, 12]); line([9, 16, 14, 16]); break
        case "list": for (let y of [6, 12, 18]) { line([4, y, 5, y]); line([9, y, 20, y]) } break
        case "grid": box(3, 3, 7, 7); box(14, 3, 7, 7); box(3, 14, 7, 7); box(14, 14, 7, 7); break
        case "brand": box(3, 3, 14, 13); box(8, 8, 13, 13); break
        case "search": circle(10, 10, 6); line([15, 15, 21, 21]); break
        case "plus": line([12, 5, 12, 19]); line([5, 12, 19, 12]); break
        case "play": line([8, 4, 20, 12, 8, 20, 8, 4]); break
        case "pause": line([8, 5, 8, 19]); line([16, 5, 16, 19]); break
        case "power": ctx.beginPath(); ctx.arc(12, 12, 8, -1, 4.14); ctx.stroke(); line([12, 2, 12, 11]); break
        case "refresh": ctx.beginPath(); ctx.arc(12, 12, 8, .6, 5.5); ctx.stroke(); line([18, 3, 18, 8, 13, 8]); break
        case "close": line([6, 6, 18, 18]); line([18, 6, 6, 18]); break
        case "more": for (let x of [5, 12, 19]) { ctx.beginPath(); ctx.arc(x, 12, 1.3, 0, Math.PI * 2); ctx.fill() } break
        case "cpu": box(6, 6, 12, 12); box(9, 9, 6, 6); for (let x of [8, 12, 16]) { line([x, 2, x, 5]); line([x, 19, x, 22]); line([2, x, 5, x]); line([19, x, 22, x]) } break
        case "network": box(8, 2, 8, 6); line([12, 8, 12, 13]); line([5, 17, 5, 13, 19, 13, 19, 17]); box(2, 17, 6, 5); box(16, 17, 6, 5); break
        case "memory": box(3, 7, 18, 10); for (let x of [7, 12, 17]) { line([x, 10, x, 14]); line([x, 17, x, 20]) } break
        case "disk": box(4, 4, 16, 16); line([4, 14, 20, 14]); circle(16, 17, .6); break
        case "expand": line([9, 3, 3, 3, 3, 9]); line([15, 3, 21, 3, 21, 9]); line([3, 15, 3, 21, 9, 21]); line([21, 15, 21, 21, 15, 21]); break
        case "collapse": line([3, 9, 9, 9, 9, 3]); line([15, 3, 15, 9, 21, 9]); line([3, 15, 9, 15, 9, 21]); line([15, 21, 15, 15, 21, 15]); break
        case "keyboard": box(2, 5, 20, 14); for (let x of [6, 10, 14, 18]) { line([x, 9, x + .3, 9]); line([x, 12, x + .3, 12]) } line([8, 16, 16, 16]); break
        case "sun": circle(12, 12, 4); for (let a = 0; a < 8; ++a) { let t = a * Math.PI / 4; line([12 + 7 * Math.cos(t), 12 + 7 * Math.sin(t), 12 + 10 * Math.cos(t), 12 + 10 * Math.sin(t)]) } break
        case "help": circle(12, 12, 9); ctx.beginPath(); ctx.arc(12, 9.5, 2.6, Math.PI * 1.05, Math.PI * 2.35); ctx.lineTo(12, 13.6); ctx.stroke(); ctx.beginPath(); ctx.arc(12, 17, .9, 0, Math.PI * 2); ctx.fill(); break
        case "palette": ctx.beginPath(); ctx.moveTo(12, 3); ctx.bezierCurveTo(4, 3, 2, 10, 3.5, 14.5); ctx.bezierCurveTo(5, 19, 10, 21.5, 13, 20); ctx.bezierCurveTo(15, 19, 13, 16, 15.5, 15); ctx.bezierCurveTo(18, 14, 21, 15, 21, 11); ctx.bezierCurveTo(21, 6, 17, 3, 12, 3); ctx.stroke()
            for (const p of [[8, 9], [12.5, 7], [16.5, 9.5], [7.5, 14]]) { ctx.beginPath(); ctx.arc(p[0], p[1], 1.4, 0, Math.PI * 2); ctx.fill() } break
        case "moon": ctx.beginPath(); ctx.arc(12, 12, 9, -.5, 4.1); ctx.quadraticCurveTo(7, 15, 19.9, 7.7); ctx.stroke(); break
        case "info": circle(12, 12, 9); line([12, 11, 12, 17]); circle(12, 7, .5); break
        case "download": line([12, 3, 12, 15]); line([7, 10, 12, 15, 17, 10]); line([4, 20, 20, 20]); break
        case "check": line([5, 12, 10, 17, 20, 6]); break
        case "shield": line([12, 3, 20, 6, 20, 12, 12, 21, 4, 12, 4, 6, 12, 3]); line([9, 12, 11, 14, 15, 10]); break
        case "terminal": box(2, 4, 20, 16); line([6, 9, 10, 12, 6, 15]); line([12, 16, 17, 16]); break
        case "snapshot": line([8, 3, 3, 3, 3, 8]); line([16, 3, 21, 3, 21, 8]); line([3, 16, 3, 21, 8, 21]); line([21, 16, 21, 21, 16, 21]); circle(12, 12, 5); break
        case "branch": circle(6, 5, 2); circle(6, 19, 2); circle(18, 5, 2); line([6, 7, 6, 17]); line([18, 7, 18, 10, 6, 15]); break
        case "trash": line([3, 6, 21, 6]); line([8, 6, 8, 3, 16, 3, 16, 6]); line([5, 6, 6, 21, 18, 21, 19, 6]); line([10, 10, 10, 17]); line([14, 10, 14, 17]); break
        case "previous": line([15, 5, 8, 12, 15, 19]); break
        case "next": line([9, 5, 16, 12, 9, 19]); break
        case "history": circle(12, 12, 9); line([12, 6, 12, 12, 8, 14]); break
        case "chevron": line([6, 9, 12, 15, 18, 9]); break
        // A little shop: striped awning over a storefront with a door.
        case "store": line([3, 9, 5, 4, 19, 4, 21, 9]); line([3, 9, 21, 9]); for (let x of [7.5, 12, 16.5]) line([x, 9, 12 + (x - 12) * 14 / 18, 4]);
            ctx.beginPath(); for (let x of [3, 7.5, 12, 16.5]) { ctx.moveTo(x, 9); ctx.quadraticCurveTo(x + 2.25, 12.5, x + 4.5, 9) } ctx.stroke();
            line([5, 11.5, 5, 20, 19, 20, 19, 11.5]); box(10, 14.5, 4, 5.5); break
        case "globe": circle(12, 12, 9); line([3, 12, 21, 12]); ctx.beginPath(); ctx.moveTo(12, 3); ctx.quadraticCurveTo(5, 12, 12, 21); ctx.quadraticCurveTo(19, 12, 12, 3); ctx.stroke(); break
        case "router": box(3, 12, 18, 8); line([8, 12, 6, 5]); line([16, 12, 18, 5]); for (let x of [7, 10, 13]) circle(x, 16, .5); line([16, 16, 18, 16]); break
        case "switch": box(2, 7, 20, 10); for (let x of [6, 10, 14, 18]) box(x - 1.2, 11, 2.4, 2.4); break
        case "plug": box(7, 9, 10, 7); line([10, 9, 10, 4]); line([14, 9, 14, 4]); line([12, 16, 12, 21]); break
        case "unplug": box(7, 9, 10, 7); line([10, 9, 10, 4]); line([14, 9, 14, 4]); line([12, 16, 12, 21]); line([3, 21, 21, 3]); break
        default: box(3, 4, 18, 13); line([12, 17, 12, 21]); line([8, 21, 16, 21])
        }
    }
}
