// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The activity history: every operation and its result, saved across restarts.
AppDialog {
    id: activityDialog
    objectName: "activityDialog"
    headerIcon: "history"
    heading: "Activity history"
    subtitle: "Last 200 operations · saved across restarts"
    width: Math.min(740, parent.width - 40)
    height: Math.min(680, parent.height - 40)
    contentItem: ColumnLayout {
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            AppSelect {
                id: activityFilter
                objectName: "activityFilter"
                Accessible.name: "Filter activity"
                model: ["All activity", "Needs attention"]
                Layout.fillWidth: true
            }
            AppButton {
                text: "Copy log"
                onClicked: preferences.copy(backend.activity.map(function (item) {
                    return item.time + " " + item.message + (item.nextStep ? "\n" + item.nextStep : "");
                }).join("\n\n"))
            }
            AppButton {
                text: "Clear…"
                tone: "quiet"
                enabled: backend.activity.length > 0
                onClicked: clearHistory.open()
            }
        }
        Label {
            visible: backend.activityWarning !== ""
            textFormat: Text.PlainText
            text: backend.activityWarning
            color: theme.colors.warning
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
        Label {
            visible: activityList.count === 0
            textFormat: Text.PlainText
            text: activityFilter.currentIndex === 1 ? "No failures in the saved history." : "No activity yet. Recent operations will appear here."
            color: theme.colors.muted
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
        ListView {
            id: activityList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 10
            model: activityDialog.visible ? backend.activity.filter(function (entry) {
                return activityFilter.currentIndex === 0 || !entry.ok;
            }) : []
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar {}
            delegate: Rectangle {
                id: activityEntry
                required property var modelData
                width: activityList.width
                height: activityContent.implicitHeight + 24
                color: theme.colors.field
                radius: 4
                border.color: theme.colors.border
                ColumnLayout {
                    id: activityContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 12
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        AppIcon {
                            name: activityEntry.modelData.ok ? "check" : "info"
                            color: activityEntry.modelData.ok ? theme.colors.success : theme.colors.warning
                            width: 16
                            height: 16
                        }
                        Label {
                            textFormat: Text.PlainText
                            text: (activityEntry.modelData.ok ? "Completed" : "Needs attention") + " · " + Qt.formatDateTime(new Date(activityEntry.modelData.time), "MMM d, yyyy · HH:mm:ss")
                            color: theme.colors.muted
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            font.pixelSize: Math.round((11) * theme.textScale)
                        }
                    }
                    Label {
                        textFormat: Text.PlainText
                        text: activityEntry.modelData.message.split("\n")[0]
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                    }
                    AppDisclosure {
                        title: "Details"
                        resetKey: activityEntry.modelData.id || activityEntry.modelData.time
                        Label {
                            textFormat: Text.PlainText
                            visible: !activityEntry.modelData.ok
                            text: activityEntry.modelData.nextStep
                            color: theme.colors.muted
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                        }
                        AppTextArea {
                            textFormat: Text.PlainText
                            text: activityEntry.modelData.message
                            readOnly: true
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(160, implicitHeight)
                            selectByMouse: true
                        }
                    }
                }
            }
        }
    }
}
