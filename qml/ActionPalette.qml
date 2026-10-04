// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

AppDialog {
    id: palette
    objectName: "actionPalette"
    property var commands: []
    property var pendingCommand: null
    signal chosen(var command)
    readonly property var matches: {
        const words = query.text.trim().toLowerCase().split(/\s+/).filter(function (w) {
            return w.length > 0;
        });
        return commands.filter(function (command) {
            const text = (command.key + " " + command.title + " " + (command.detail || "") + " " + (command.keywords || "")).toLowerCase();
            return words.every(function (w) {
                return text.indexOf(w) >= 0;
            });
        });
    }
    heading: "Command"
    subtitle: "type to filter · ↑ ↓ choose · enter run · esc close"
    headerIcon: "search"
    width: Math.min(640, parent.width - 40)
    height: Math.min(540, parent.height - 40)
    function choose(index) {
        if (index < 0 || index >= matches.length || matches[index].enabled === false)
            return;
        pendingCommand = matches[index];
        close();
    }
    onAboutToShow: {
        query.text = "";
        results.currentIndex = 0;
        query.forceActiveFocus();
    }
    onOpened: query.forceActiveFocus()
    onClosed: if (pendingCommand) {
        const command = pendingCommand;
        pendingCommand = null;
        Qt.callLater(function () {
            palette.chosen(command);
        });
    }
    onMatchesChanged: results.currentIndex = matches.length ? 0 : -1
    contentItem: ColumnLayout {
        spacing: 14
        AppField {
            id: query
            objectName: "actionQuery"
            Accessible.name: "Search actions"
            placeholderText: "start, snapshot, monitor, theme…"
            Layout.fillWidth: true
            leftPadding: 30
            font.family: "monospace"
            Label {
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                text: ">"
                color: theme.colors.accent
                font.family: "monospace"
                font.weight: Font.Bold
                font.pixelSize: Math.round(14 * theme.textScale)
            }
            onAccepted: palette.choose(results.currentIndex)
            Keys.onDownPressed: results.currentIndex = Math.min(palette.matches.length - 1, results.currentIndex + 1)
            Keys.onUpPressed: results.currentIndex = Math.max(0, results.currentIndex - 1)
        }
        Label {
            visible: palette.matches.length === 0
            textFormat: Text.PlainText
            text: "command not found: " + query.text.trim()
            color: theme.colors.muted
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
        ListView {
            id: results
            objectName: "actionResults"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: palette.matches
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
            ScrollBar.vertical: AppScrollBar {}
            delegate: ItemDelegate {
                id: commandItem
                required property var modelData
                required property int index
                width: results.width
                implicitHeight: Math.max(44, commandLabel.implicitHeight + 12)
                objectName: "command_" + modelData.key
                highlighted: results.currentIndex === index
                enabled: modelData.enabled !== false
                Accessible.name: modelData.title
                Accessible.description: modelData.detail || ""
                background: Rectangle {
                    radius: 2
                    color: commandItem.highlighted ? theme.colors.accentSoft : commandItem.hovered ? theme.colors.subtle : "transparent"
                    Rectangle {
                        visible: commandItem.highlighted
                        width: 2
                        height: parent.height
                        color: theme.colors.accent
                    }
                }
                contentItem: RowLayout {
                    spacing: 12
                    Label {
                        textFormat: Text.PlainText
                        text: commandItem.highlighted ? "▸" : " "
                        color: theme.colors.accent
                        font.family: "monospace"
                    }
                    AppIcon {
                        name: commandItem.modelData.icon || "next"
                        color: commandItem.enabled ? theme.colors.accent : theme.colors.muted
                        width: 16
                        height: 16
                    }
                    ColumnLayout {
                        id: commandLabel
                        Layout.fillWidth: true
                        spacing: 3
                        Label {
                            textFormat: Text.PlainText
                            text: commandItem.modelData.title
                            color: commandItem.enabled ? theme.colors.foreground : theme.colors.muted
                            font.weight: Font.DemiBold
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                        Label {
                            visible: text !== ""
                            textFormat: Text.PlainText
                            text: commandItem.modelData.detail || ""
                            color: theme.colors.muted
                            font.pixelSize: Math.round((11) * theme.textScale)
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                    }
                    Label {
                        visible: !!commandItem.modelData.shortcut
                        textFormat: Text.PlainText
                        text: commandItem.modelData.shortcut || ""
                        color: theme.colors.muted
                        font.family: "monospace"
                        font.pixelSize: Math.round(11 * theme.textScale)
                        leftPadding: 6
                        rightPadding: 6
                        topPadding: 2
                        bottomPadding: 2
                        background: Rectangle {
                            color: "transparent"
                            border.color: theme.colors.border
                            radius: 2
                        }
                    }
                }
                onClicked: palette.choose(index)
            }
        }
    }
}
