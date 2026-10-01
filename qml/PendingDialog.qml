// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
EditorDialog {
    id: dialog
    property string uuid: ""
    property var info: backend.details.uuid === uuid ? backend.details : ({})
    heading: "Pending changes"
    headerIcon: "history"
    subtitle: "Saved for the next full start"
    actionText: "Shut down & start"
    ready: !!info.owned && !!info.active && !info.pendingConflict
    property bool confirmRestart: false
    function openFor(details) { uuid = details.uuid; confirmRestart = false; open() }
    onSubmitted: {
        if (!confirmRestart) { confirmRestart = true; return }
        execute("vm.restart", {uuid: uuid})
    }
    Label { textFormat: Text.PlainText; text: dialog.info.name || ""; font.weight: Font.DemiBold }
    Label { text: dialog.confirmRestart ? "Save your work inside this VM. OmaWare will request a graceful shutdown, wait for it to stop, then start it with the saved configuration." : "These changes are saved for the next full start. Discard restores an individual setting to its earlier value."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
    Label { visible: !!dialog.info.pendingConflict; text: "Another manager changed this VM. Discard is disabled. Review the current settings, then accept them to start a fresh change record."; color: theme.colors.warning; Layout.fillWidth: true; wrapMode: Text.WordWrap }
    Repeater {
        model: dialog.info.changes || []
        ColumnLayout { required property var modelData; Layout.fillWidth: true; spacing: 6
            RowLayout { Layout.fillWidth: true
                Label { textFormat: Text.PlainText; text: modelData.label; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                AppButton { text: "Discard"; tone: "quiet"; enabled: !backend.busy && !!dialog.info.canDiscardChanges; onClicked: backend.request("pending.discard", {uuid: dialog.uuid, revision: dialog.info.revision, key: modelData.key}) }
            }
            Label { textFormat: Text.PlainText; text: modelData.before + "\n↓\n" + modelData.after; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
            Rectangle { Layout.fillWidth: true; height: 1; color: theme.colors.border }
        }
    }
    Label { visible: (dialog.info.changes || []).length === 0; text: "No unapplied changes are recorded."; color: theme.colors.muted }
    AppButton { text: "Discard all changes"; enabled: !backend.busy && !!dialog.info.canDiscardChanges; onClicked: backend.request("pending.discard", {uuid: dialog.uuid, revision: dialog.info.revision, key: "all"}) }
    AppButton { visible: !!dialog.info.pendingConflict; text: "Accept current saved settings"; enabled: !backend.busy; onClicked: backend.request("pending.accept", {uuid: dialog.uuid, revision: dialog.info.revision}) }
}
