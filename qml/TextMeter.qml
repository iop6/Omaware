// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

// htop-style meter: [||||||      42.1%]. Width is in characters of the monospace font.
Row {
    id: meter
    property real fraction: 0      // 0..1; negative means no data
    property int cells: 20
    property string suffix: ""
    property color ink: fraction >= .9 ? theme.colors.danger : fraction >= .75 ? theme.colors.warning : theme.colors.accent
    readonly property int filled: fraction < 0 ? 0 : Math.round(Math.min(1, fraction) * cells)
    spacing: 0
    Accessible.role: Accessible.ProgressBar
    Accessible.name: fraction < 0 ? "No data" : Math.round(fraction * 100) + "% " + suffix
    Label { text: "["; color: theme.colors.muted; font.family: "monospace"; font.pixelSize: Math.round(12 * theme.textScale) }
    Label { text: "|".repeat(meter.filled); color: meter.ink; font.family: "monospace"; font.pixelSize: Math.round(12 * theme.textScale); font.weight: Font.DemiBold }
    Label { text: (meter.fraction < 0 ? "·" : " ").repeat(meter.cells - meter.filled); color: theme.colors.line; font.family: "monospace"; font.pixelSize: Math.round(12 * theme.textScale) }
    Label { text: "]"; color: theme.colors.muted; font.family: "monospace"; font.pixelSize: Math.round(12 * theme.textScale) }
    Label { visible: meter.suffix !== ""; text: " " + meter.suffix; color: theme.colors.foreground; font.family: "monospace"; font.pixelSize: Math.round(12 * theme.textScale) }
}
