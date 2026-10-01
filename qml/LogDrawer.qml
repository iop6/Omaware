// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Terminal-style tail of the operation log, newest line at the bottom.
Rectangle {
    id: drawer
    property var entries: []
    property var copyHelper: null
    property bool failuresOnly: false
    signal openHistory()
    signal dismiss()
    readonly property var lines: {
        const pattern = grep.text.trim().toLowerCase()
        return entries.filter(function(e) {
            return (!drawer.failuresOnly || !e.ok) && (pattern === "" || (e.message + " " + (e.nextStep || "")).toLowerCase().indexOf(pattern) >= 0)
        }).slice().reverse()
    }
    function stamp(time) { const d = new Date(time); return isNaN(d.getTime()) ? "--:--:--" : Qt.formatTime(d, "HH:mm:ss") }
    color: theme.colors.field
    implicitHeight: 230
    onLinesChanged: Qt.callLater(function() { tail.positionViewAtEnd() })
    onVisibleChanged: if (visible) Qt.callLater(function() { tail.positionViewAtEnd() })
    Rectangle { width: parent.width; height: 2; color: theme.colors.accent }
    ColumnLayout {
        anchors.fill: parent; anchors.topMargin: 2; spacing: 0
        RowLayout {
            Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 8; Layout.preferredHeight: 34
            spacing: 10
            Label { text: "~/omaware/log"; color: theme.colors.accent; font.family: "monospace"; font.weight: Font.DemiBold; font.pixelSize: Math.round(12 * theme.textScale) }
            Label { text: "tail -f"; color: theme.colors.muted; font.family: "monospace"; font.pixelSize: Math.round(11 * theme.textScale) }
            Item { Layout.fillWidth: true }
            Label { text: "grep"; color: theme.colors.muted; font.family: "monospace"; font.pixelSize: Math.round(11 * theme.textScale) }
            AppField {
                id: grep; objectName: "logFilter"
                Layout.preferredWidth: 200; implicitHeight: 28
                font.family: "monospace"; font.pixelSize: Math.round(11 * theme.textScale)
                placeholderText: "pattern…"; Accessible.name: "Filter log lines"
                Keys.onEscapePressed: { if (text !== "") text = ""; else drawer.dismiss() }
            }
            AppButton { text: drawer.failuresOnly ? "errors" : "all"; tone: "quiet"; implicitHeight: 28; checked: drawer.failuresOnly; hint: "Show only operations that need attention"; onClicked: drawer.failuresOnly = !drawer.failuresOnly }
            AppButton { iconName: "clipboard"; tone: "quiet"; implicitHeight: 28; implicitWidth: 30; hint: "Copy visible lines"; enabled: drawer.lines.length > 0
                onClicked: if (drawer.copyHelper) drawer.copyHelper.copy(drawer.lines.map(function(e) { return drawer.stamp(e.time) + (e.ok ? "  ok    " : "  fail  ") + e.message.split("\n")[0] }).join("\n")) }
            AppButton { iconName: "history"; tone: "quiet"; implicitHeight: 28; implicitWidth: 30; hint: "Full activity history"; onClicked: drawer.openHistory() }
            AppButton { iconName: "close"; tone: "quiet"; implicitHeight: 28; implicitWidth: 30; hint: "Close log · Ctrl+`"; onClicked: drawer.dismiss() }
        }
        ListView {
            id: tail
            objectName: "logLines"
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.leftMargin: 14; Layout.rightMargin: 6; Layout.bottomMargin: 6
            clip: true; spacing: 2
            model: drawer.visible ? drawer.lines : []
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar {}
            delegate: RowLayout {
                id: line
                required property var modelData
                width: ListView.view.width - 12
                spacing: 12
                Label { text: drawer.stamp(line.modelData.time); color: theme.colors.muted; font.family: "monospace"; font.pixelSize: Math.round(11 * theme.textScale); Layout.alignment: Qt.AlignTop }
                Label { text: line.modelData.ok ? "ok  " : "FAIL"; color: line.modelData.ok ? theme.colors.success : theme.colors.danger; font.family: "monospace"; font.weight: Font.DemiBold; font.pixelSize: Math.round(11 * theme.textScale); Layout.alignment: Qt.AlignTop }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 1
                    TextEdit {
                        Layout.fillWidth: true
                        text: line.modelData.message.split("\n")[0]
                        readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap; textFormat: TextEdit.PlainText
                        color: theme.colors.foreground; selectionColor: theme.colors.accent; selectedTextColor: theme.colors.accentText
                        font.family: "monospace"; font.pixelSize: Math.round(11 * theme.textScale)
                    }
                    Label { textFormat: Text.PlainText; visible: !line.modelData.ok && !!line.modelData.nextStep; text: "  └ " + (line.modelData.nextStep || ""); color: theme.colors.warning; wrapMode: Text.Wrap; Layout.fillWidth: true; font.family: "monospace"; font.pixelSize: Math.round(11 * theme.textScale) }
                }
            }
            Label {
                anchors.left: parent.left; anchors.top: parent.top; anchors.topMargin: 6
                visible: tail.count === 0
                text: drawer.entries.length === 0 ? "$ waiting for operations…" : "$ no lines match"
                color: theme.colors.muted; font.family: "monospace"; font.pixelSize: Math.round(11 * theme.textScale)
            }
        }
    }
}
