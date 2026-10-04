// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Checks GitHub for a new OmaWare and, when you agree, updates to it. Updating restarts OmaWare, so it
// says up front that running VMs will be paused first (they continue where they were when resumed).
AppDialog {
    id: dialog
    objectName: "updateDialog"
    property var updater: null
    property int runningCount: 0
    property string phase: ""          // "", "pausing" or "restarting", while an update is being applied
    property var failures: []          // VMs that couldn't be paused
    signal updateNow
    signal updateAnyway
    signal stay

    readonly property string status: updater ? updater.status : "idle"
    readonly property bool offering: ["available", "downloading", "ready", "installed"].indexOf(status) >= 0 || (status === "error" && !!updater.latest && updater.latest !== updater.current)
    readonly property bool busy: phase !== "" && failures.length === 0
    width: Math.min(540, parent ? parent.width - 40 : 540)
    headerIcon: "download"
    dismissible: !busy
    closePolicy: busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    heading: !updater ? "Updates" : updater.development ? "Development build" : status === "checking" ? "Checking for updates" : status === "upToDate" ? "You're up to date" : offering ? "OmaWare " + updater.latest + " is available" : status === "error" ? "Couldn't check for updates" : "Updates"
    subtitle: !updater ? "" : "You have OmaWare " + (updater.current || "(unknown version)")

    contentItem: ColumnLayout {
        spacing: 14
        // Checking, up to date, or a problem.
        RowLayout {
            visible: dialog.status === "checking"
            spacing: 10
            AppBusyIndicator {
                running: dialog.visible && dialog.status === "checking"
                implicitWidth: 22
                implicitHeight: 22
            }
            Label {
                text: "Asking GitHub for the latest version…"
                color: theme.colors.muted
            }
        }
        Label {
            visible: !!dialog.updater && (dialog.updater.development || dialog.status === "upToDate" || (dialog.status === "error" && !dialog.offering) || dialog.status === "idle")
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: dialog.status === "error" ? theme.colors.danger : theme.colors.foreground
            text: !dialog.updater ? "" : dialog.updater.development ? "Development builds don't update themselves. Install a release from GitHub to get updates." : dialog.status === "upToDate" ? "OmaWare " + dialog.updater.current + " is the newest version on GitHub." : dialog.status === "error" ? dialog.updater.error : ""
        }

        // A new version: what's in it, and what updating does.
        Label {
            visible: dialog.offering
            text: "What's new"
            font.weight: Font.DemiBold
        }
        ScrollView {
            visible: dialog.offering
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(notes.implicitHeight + 4, 190)
            clip: true
            contentWidth: availableWidth
            Label {
                id: notes
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.MarkdownText
                color: theme.colors.muted
                text: dialog.updater && dialog.updater.notes ? dialog.updater.notes : "See the release page for details."
                font.pixelSize: Math.round(12 * theme.textScale)
                // Only web links; release notes come from GitHub and never need anything else.
                onLinkActivated: function (link) {
                    if (/^https:\/\//.test(link))
                        Qt.openUrlExternally(link);
                }
            }
        }
        Rectangle {
            objectName: "updateWarning"
            visible: dialog.offering
            Layout.fillWidth: true
            implicitHeight: warning.implicitHeight + 20
            radius: 4
            color: "transparent"
            border.color: dialog.runningCount > 0 ? theme.colors.warning : theme.colors.border
            Label {
                id: warning
                objectName: "updateWarningText"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 10
                wrapMode: Text.WordWrap
                color: dialog.runningCount > 0 ? theme.colors.warning : theme.colors.muted
                text: dialog.runningCount > 0 ? "Updating restarts OmaWare. Your " + (dialog.runningCount === 1 ? "running VM" : dialog.runningCount + " running VMs") + " will be paused first. When OmaWare reopens, choose Resume all and they continue exactly where they left off." : "Updating restarts OmaWare. It only takes a few seconds."
            }
        }
        Label {
            visible: dialog.offering && !!dialog.updater && !dialog.updater.canInstall
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: theme.colors.muted
            text: "This copy of OmaWare wasn't installed from a release package, so it can't update itself. Download the new version from GitHub instead."
        }

        // Progress while updating.
        ColumnLayout {
            visible: dialog.status === "downloading" || dialog.busy
            Layout.fillWidth: true
            spacing: 6
            Label {
                objectName: "updateProgressText"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: dialog.phase === "pausing" ? "Pausing " + (dialog.runningCount === 1 ? "your running VM…" : dialog.runningCount + " running VMs…") : dialog.phase === "restarting" ? "Restarting into OmaWare " + (dialog.updater ? dialog.updater.latest : "") + "…" : "Downloading and checking OmaWare " + (dialog.updater ? dialog.updater.latest : "") + "… " + Math.round((dialog.updater ? dialog.updater.progress : 0) * 100) + "%"
            }
            AppProgressBar {
                Layout.fillWidth: true
                indeterminate: dialog.phase !== ""
                value: dialog.updater ? dialog.updater.progress : 0
            }
        }
        Repeater {
            model: dialog.failures
            Label {
                required property var modelData
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: modelData
                color: theme.colors.danger
                wrapMode: Text.WordWrap
            }
        }
        Label {
            visible: dialog.failures.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: theme.colors.muted
            text: "Those VMs are still running. Update anyway to leave them running while OmaWare restarts, or keep OmaWare open and try again later."
        }

        // Buttons.
        RowLayout {
            Layout.topMargin: 4
            spacing: 8
            AppButton {
                visible: dialog.offering && !!dialog.updater
                text: "Release page"
                tone: "quiet"
                onClicked: Qt.openUrlExternally(dialog.updater.page)
            }
            Item {
                Layout.fillWidth: true
            }
            AppButton {
                objectName: "updateCheckAgain"
                visible: !!dialog.updater && !dialog.updater.development && (dialog.status === "upToDate" || (dialog.status === "error" && !dialog.offering))
                text: "Check again"
                iconName: "refresh"
                onClicked: dialog.updater.check()
            }
            AppButton {
                objectName: "updateLater"
                visible: !dialog.busy && dialog.failures.length === 0
                text: dialog.offering ? "Not now" : "Close"
                onClicked: dialog.close()
            }
            AppButton {
                objectName: "updateStay"
                visible: dialog.failures.length > 0
                text: "Keep OmaWare open"
                onClicked: dialog.stay()
            }
            AppButton {
                objectName: "updateAnyway"
                visible: dialog.failures.length > 0
                text: "Update anyway"
                tone: "primary"
                onClicked: dialog.updateAnyway()
            }
            AppButton {
                objectName: "updateNow"
                visible: dialog.offering && !dialog.busy && dialog.failures.length === 0 && dialog.status !== "downloading" && !!dialog.updater && (dialog.updater.canInstall || dialog.status === "ready")
                text: "Update now"
                tone: "primary"
                iconName: "download"
                onClicked: dialog.updateNow()
            }
            AppButton {
                visible: dialog.offering && !!dialog.updater && !dialog.updater.canInstall && dialog.status !== "ready"
                text: "Download from GitHub"
                tone: "primary"
                onClicked: Qt.openUrlExternally(dialog.updater.page)
            }
        }
    }
}
