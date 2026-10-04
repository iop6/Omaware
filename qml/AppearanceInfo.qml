// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Where OmaWare's colors come from (the Omarchy theme or a built-in one), with the text size and motion settings.
Popup {
    id: appearanceInfo
    objectName: "appearanceInfo"
    x: 18
    y: Math.max(10, root.height - height - 40)
    width: Math.min(400, root.width - 36)
    // Scrolls when it doesn't fit (large text in a small window), instead of being squeezed.
    height: Math.min(implicitHeight, root.height - 50)
    padding: 16
    background: Rectangle {
        color: theme.colors.raised
        border.color: theme.colors.border
        radius: 4
    }
    contentItem: ScrollView {
        id: settingsScroll
        clip: true
        contentWidth: availableWidth
        implicitHeight: settingsColumn.implicitHeight
        ScrollBar.vertical: AppScrollBar {
            policy: ScrollBar.AsNeeded
        }
        ColumnLayout {
            id: settingsColumn
            width: settingsScroll.availableWidth
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "Settings"
                    font.weight: Font.DemiBold
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
                HelpButton {
                    topic: "settings-and-themes"
                    onClicked: appearanceInfo.close()
                }
            }
            Label {
                text: "Text size"
            }
            AppSelect {
                objectName: "textSize"
                Accessible.name: "Interface text size"
                Layout.fillWidth: true
                model: ["Standard · 100%", "Larger · 115%", "Largest · 130%"]
                currentIndex: theme.textScale > 1.2 ? 2 : theme.textScale > 1.05 ? 1 : 0
                onActivated: {
                    const scale = [1, 1.15, 1.3][currentIndex];
                    theme.setTextScale(scale);
                    preferences.set("textScale", scale);
                }
            }
            AppCheckBox {
                objectName: "reducedMotion"
                text: "Reduce motion"
                checked: theme.reducedMotion
                Layout.fillWidth: true
                onToggled: {
                    theme.setReducedMotion(checked);
                    preferences.set("reducedMotion", checked);
                }
            }
            Label {
                text: "Theme"
            }
            ThemePicker {
                Layout.fillWidth: true
                onPicked: function (key) {
                    root.setTheme(key);
                }
            }
            // Only when the Omarchy palette can't be used.
            Label {
                objectName: "themeStatus"
                visible: theme.mode === "omarchy" && !theme.status.startsWith("Following")
                textFormat: Text.PlainText
                text: theme.status
                color: theme.colors.warning
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: Math.round(10 * theme.textScale)
            }
            Label {
                text: "Storage"
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label {
                    objectName: "storageFolder"
                    textFormat: Text.PlainText
                    text: isoLibrary.root
                    color: theme.colors.muted
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                    font.pixelSize: Math.round(11 * theme.textScale)
                }
                AppButton {
                    text: "Open"
                    iconName: "folder"
                    tone: "quiet"
                    implicitHeight: 28
                    hint: "VMs are in vms/, ISOs in isos/"
                    onClicked: Qt.openUrlExternally("file://" + isoLibrary.root)
                }
            }
            Label {
                text: "Updates"
            }
            ColumnLayout {
                objectName: "updateSettings"
                Layout.fillWidth: true
                spacing: 6
                Label {
                    objectName: "updateStatus"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: Math.round(11 * theme.textScale)
                    color: updater.status === "error" ? theme.colors.danger : updater.status === "available" || updater.status === "ready" ? theme.colors.warning : theme.colors.muted
                    textFormat: Text.PlainText
                    text: updater.development ? "This is a development build; it doesn't update itself." : updater.status === "checking" ? "Checking for updates…" : updater.status === "upToDate" ? "✓ You have the latest version (" + updater.current + ")." : updater.status === "available" ? "OmaWare " + updater.latest + " is available." + (updater.canInstall ? "" : " Download it from the releases page.") : updater.status === "downloading" ? "Downloading OmaWare " + updater.latest + "… " + Math.round(updater.progress * 100) + "%" : updater.status === "ready" ? "OmaWare " + updater.latest + " is downloaded and ready to install." : updater.status === "error" ? updater.error : "You have OmaWare " + updater.current + "."
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    AppButton {
                        objectName: "updateCheck"
                        visible: !updater.development
                        text: ["available", "downloading", "ready"].indexOf(updater.status) >= 0 ? "Update…" : "Check for updates"
                        tone: ["available", "downloading", "ready"].indexOf(updater.status) >= 0 ? "primary" : "quiet"
                        implicitHeight: 28
                        onClicked: {
                            appearanceInfo.close();
                            root.openUpdates(true);
                        }
                    }
                }
                AppCheckBox {
                    objectName: "autoUpdateCheck"
                    visible: !updater.development
                    text: "Check daily"
                    Layout.fillWidth: true
                    checked: root.autoUpdate
                    onToggled: {
                        root.autoUpdate = checked;
                        preferences.set("autoUpdateCheck", checked);
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "AI agents"
                    Layout.fillWidth: true
                }
                HelpButton {
                    topic: "ai-agents-and-labs"
                    onClicked: appearanceInfo.close()
                }
            }
            ColumnLayout {
                objectName: "agentSettings"
                Layout.fillWidth: true
                spacing: 6
                AppCheckBox {
                    objectName: "agentAccess"
                    text: "Let AI agents use OmaWare"
                    Layout.fillWidth: true
                    checked: agent.enabled
                    onToggled: agent.enabled = checked
                }
                Label {
                    text: "You approve every lab and anything that deletes."
                    color: theme.colors.muted
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: Math.round(10 * theme.textScale)
                }
                Label {
                    visible: agent.enabled
                    text: "Connect Claude Code by running:"
                    font.pixelSize: Math.round(11 * theme.textScale)
                }
                RowLayout {
                    visible: agent.enabled
                    Layout.fillWidth: true
                    spacing: 6
                    Label {
                        objectName: "agentCommand"
                        text: agent.command
                        textFormat: Text.PlainText
                        font.family: "monospace"
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                        color: theme.colors.muted
                        font.pixelSize: Math.round(10 * theme.textScale)
                        ToolTip.visible: commandArea.containsMouse
                        ToolTip.text: agent.command
                        MouseArea {
                            id: commandArea
                            anchors.fill: parent
                            hoverEnabled: true
                        }
                    }
                    AppButton {
                        text: "Copy"
                        tone: "quiet"
                        implicitHeight: 28
                        onClicked: preferences.copy(agent.command)
                    }
                }
                Label {
                    visible: agent.error !== ""
                    textFormat: Text.PlainText
                    text: agent.error
                    color: theme.colors.danger
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: Math.round(10 * theme.textScale)
                }
            }

            Label {
                text: "F1 help  ·  : commands  ·  ? shortcuts"
                color: theme.colors.muted
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: Math.round((11) * theme.textScale)
            }
            Label {
                objectName: "appVersion"
                textFormat: Text.PlainText
                text: "OmaWare" + (Qt.application.version ? " " + Qt.application.version : "")
                color: theme.colors.muted
                Layout.fillWidth: true
                font.pixelSize: Math.round((11) * theme.textScale)
            }
        }
    }
}
