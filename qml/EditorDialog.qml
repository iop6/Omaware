// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omaware 1.0
AppDialog {
    id: dialog
    default property alias fields: form.data
    heading: "Settings"
    dismissible: !applying
    property string actionText: "Save"
    property string actionTone: "primary"
    property string failure: ""
    property string operation: ""
    property bool applying: false
    property bool ready: true
    property bool requiresConnection: true
    property var checkpointJob: backend.checkpointJob || ({})
    signal submitted()
    signal completed(var result)
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(650, parent.width - 40)
    height: Math.min(700, parent.height - 40)
    padding: 24
    modal: true
    closePolicy: applying ? Popup.NoAutoClose : Popup.CloseOnEscape
    onAboutToShow: { failure = ""; applying = false; formScroll.contentY = 0 }
    onHeadingChanged: if (visible) formScroll.contentY = 0
    function resetScroll() { formScroll.contentY = 0 }
    Workspace { id: errorWorkspace }
    function recoverFailure(action) {
        if (action === "connection" || action === "networks" || action === "refresh" || action === "storage") {
            close(); backend.showRecovery(action, "")
        } else resetScroll()
    }
    function execute(op, input) {
        failure = ""; operation = op
        applying = backend.request(op, input)
        if (!applying) failure = "Another operation is in progress. Try again when it finishes."
    }
    Connections {
        target: backend
        function onCommandFinished(op, ok, result) {
            if (!dialog.applying || op !== dialog.operation) return
            dialog.applying = false
            if (ok) { dialog.accept(); dialog.completed(result) }
            else dialog.failure = result.message
        }
    }
    contentItem: Flickable {
        id: formScroll
        clip: true; contentWidth: width; contentHeight: form.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: AppScrollBar {}
        ColumnLayout { id: form; width: parent.width - (formScroll.contentHeight > formScroll.height ? 14 : 0); spacing: 12; enabled: !dialog.applying }
    }
    footer: ColumnLayout {
        spacing: 8
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.colors.border }
        ErrorNotice { message: dialog.failure; copyHelper: errorWorkspace; Layout.leftMargin: 24; Layout.rightMargin: 24; onRecover: function(action) { dialog.recoverFailure(action) } }
        AppProgressBar { visible: dialog.applying && !!dialog.checkpointJob.active; Layout.fillWidth: true; Layout.leftMargin: 24; Layout.rightMargin: 24; from: 0; to: Math.max(1, dialog.checkpointJob.total || 0); value: dialog.checkpointJob.completed || 0; indeterminate: !(dialog.checkpointJob.total > 0) }
        Label { visible: dialog.applying && !!dialog.checkpointJob.active; Layout.fillWidth: true; Layout.leftMargin: 24; Layout.rightMargin: 24; color: theme.colors.muted; wrapMode: Text.WordWrap
            text: (dialog.checkpointJob.phase || "Preparing checkpoint") + (dialog.checkpointJob.total > 0 ? " · " + Math.round(100 * (dialog.checkpointJob.completed || 0) / dialog.checkpointJob.total) + "%" : "")
        }
        RowLayout {
            Layout.fillWidth: true; Layout.margins: 20
            AppButton { objectName: "jobBackground"; visible: dialog.applying && !!dialog.checkpointJob.active; text: "Background"; hint: "Continue browsing; follow progress in Jobs"; onClicked: dialog.close() }
            AppBusyIndicator { running: dialog.applying; visible: running; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
            Label { visible: dialog.applying; text: backend.message; color: theme.colors.muted; elide: Text.ElideRight; Layout.fillWidth: true }
            Item { Layout.fillWidth: true }
            AppButton { objectName: "editorCancel"; text: dialog.applying && dialog.checkpointJob.active ? "Cancel checkpoint" : "Cancel"; enabled: !dialog.applying || (!!dialog.checkpointJob.active && !!dialog.checkpointJob.cancellable && !dialog.checkpointJob.cancelRequested); onClicked: dialog.applying ? backend.cancelCheckpoint() : dialog.reject() }
            AppButton { objectName: "editorSave"; text: dialog.actionText; tone: dialog.actionTone; enabled: dialog.ready && !dialog.applying && !backend.busy && (!dialog.requiresConnection || backend.connected); onClicked: dialog.submitted() }
        }
    }
}
