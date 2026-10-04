// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The keyboard shortcuts, opened with the ? key or the ? button in the status bar.
AppDialog {
    id: keyMap
    objectName: "keyMap"
    headerIcon: "keyboard"
    heading: "Keyboard map"
    subtitle: "Shortcuts pause while the console captures input · Ctrl+Alt releases it"
    width: Math.min(620, parent.width - 40)
    contentItem: GridLayout {
        columns: 2
        columnSpacing: 18
        rowSpacing: 8
        Repeater {
            model: [[": · Ctrl+Shift+P", "Command prompt"], ["?  · F1", "This keyboard map"], ["Ctrl+K · Ctrl+F", "Search machines"], ["Ctrl+1 / 2 / 3 / 4", "Machines · Monitor · Networks · OS Shop"], ["Ctrl+`", "Toggle the log drawer"], ["j / k  ·  ↑ / ↓", "Move through machines (list focused)"], ["Enter", "Open the console"], ["d", "Details for the selected machine"], ["s", "Snapshots for the selected machine"], ["Menu · Shift+F10", "VM actions (also right-click a VM)"], ["f  ·  F2  ·  Delete", "Favorite · rename · remove a stopped VM"], ["Ctrl+click · Shift+click", "Select several machines to turn on, pause or shut down together"], ["Ctrl+A  ·  Esc", "Select every listed machine · clear the selection"], ["Ctrl+B", "Collapse or expand the sidebar"], ["Esc", "Leave focus or fullscreen console"], ["Ctrl+Alt", "Release captured console input"]]
            delegate: Item {
                required property var modelData
                required property int index
                Layout.columnSpan: 2
                Layout.fillWidth: true
                implicitHeight: keyRow.implicitHeight
                RowLayout {
                    id: keyRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    spacing: 18
                    Label {
                        textFormat: Text.PlainText
                        text: modelData[0]
                        color: theme.colors.accent
                        font.weight: Font.DemiBold
                        Layout.preferredWidth: 190 * theme.textScale
                        leftPadding: 8
                        rightPadding: 8
                        topPadding: 3
                        bottomPadding: 3
                        background: Rectangle {
                            color: theme.colors.field
                            border.color: theme.colors.border
                            radius: 2
                        }
                    }
                    Label {
                        textFormat: Text.PlainText
                        text: modelData[1]
                        color: theme.colors.foreground
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}
