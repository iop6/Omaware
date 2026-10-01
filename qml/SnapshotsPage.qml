// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: page
    Layout.minimumWidth: 0
    property var info: ({})
    property real viewportHeight: 0
    property var preview: function() { return "" }
    property var response: backend.management["snapshots.list"] || ({})
    property var snapshotData: response.uuid === info.uuid ? response : ({})
    property var storage: snapshotData.storage || ({})
    property var job: backend.checkpointJob || ({})
    property string failure: ""
    property bool observedActive: false
    property bool listPending: false
    property string failedListUuid: ""
    property string vmId: info.uuid || ""
    property string selectedKey: "__working__"
    property string selectCreatedName: ""
    property string captureName: ""
    property string captureUuid: ""
    property bool capturePending: false
    property string renameUuid: ""
    property string renameId: ""
    property string requestedCaptureUuid: ""
    property var requestedCaptureHost: null
    readonly property bool dialogOpen: editor.visible
    function requestCapture(host) {
        requestedCaptureHost = host || null
        if (canCapture) editor.openFor("create", {}, false, requestedCaptureHost)
        else if (!backend.busy && info.owned) { requestedCaptureUuid = info.uuid; refresh() }
    }
    onSnapshotDataChanged: if (requestedCaptureUuid) Qt.callLater(function() {
        if (!page.ready || !page.requestedCaptureUuid) return
        const uuid = page.requestedCaptureUuid; page.requestedCaptureUuid = ""
        if (uuid === page.info.uuid) { if (page.canCapture) editor.openFor("create", {}, false, page.requestedCaptureHost); else page.failure = page.snapshotData.blocker || "This VM cannot take a snapshot yet." }
    })
    function renameSnapshot(id) {
        const item = snapshots.find(function(s) { return page.key(s) === id })
        if (!item || item.kind !== "copy" || !info.owned || backend.busy) return
        selectedKey = id
        Qt.callLater(function() { timeline.beginRename(id, item.name) })
    }
    function revertCurrent(host) {
        if (ready && currentSnapshot.name && info.owned && !backend.busy && !snapshotData.restoreBlocker)
            editor.openFor("restore", currentSnapshot, true, host)
    }
    property string contextKey: ""
    property string contextUuid: ""
    readonly property bool contextWorking: contextKey === "__working__"
    readonly property var contextSnapshot: snapshots.find(function(s) { return page.key(s) === page.contextKey }) || ({})
    readonly property bool contextValid: contextUuid === vmId && (contextWorking || !!contextSnapshot.name)
    readonly property bool contextEditable: contextValid && !!info.owned && !backend.busy
    readonly property bool contextHasChildren: snapshots.some(function(s) { return s.parentId === page.contextKey })
    function openContext(id, anchor, x, y) {
        contextKey = id; contextUuid = vmId; selectedKey = id
        snapshotMenu.popup(anchor, x, y)
    }
    function showDetails(item) { snapshotDetails.snapshot = item; snapshotDetails.open() }
    function openStorage() { storageDialog.open() }
    function showPreview(item) { snapshotPreview.snapshot = item; snapshotPreview.open() }
    readonly property bool compact: width < 660 || (viewportHeight > 0 && viewportHeight < 520)
    readonly property bool ready: !!info.uuid && snapshotData.uuid === info.uuid
    readonly property bool canCapture: ready && !!info.owned && !backend.busy && !snapshotData.blocker
    property var snapshots: {
        // Preserve parent-before-child ordering for captures in the same second.
        return (snapshotData.items || []).map(function(item, index) {
            return {snapshot: item, order: index}
        }).sort(function(a, b) {
            return a.snapshot.time - b.snapshot.time || a.order - b.order
        }).map(function(entry) { return entry.snapshot })
    }
    readonly property int selectedIndex: {
        for (let i = 0; i < snapshots.length; ++i) if (key(snapshots[i]) === selectedKey) return i
        return -1
    }
    readonly property var selectedSnapshot: selectedIndex >= 0 ? snapshots[selectedIndex] : ({})
    readonly property var currentSnapshot: {
        for (let item of snapshots) if (item.current) return item
        return ({})
    }
    readonly property string currentId: snapshotData.currentId || (currentSnapshot.name ? key(currentSnapshot) : "")
    readonly property bool workingSelected: selectedKey === "__working__"
    readonly property bool canDelete: ready && selectedIndex >= 0 && !!info.owned && !backend.busy
    function deleteSelection() { if (canDelete) deletion.openFor(selectedSnapshot) }
    spacing: compact ? 10 : 14
    function key(item) { return item.id || "internal:" + item.name }
    function refresh() {
        if (!info.uuid || listPending) return false
        listPending = true; failedListUuid = ""
        const requested = backend.request("snapshots.list", {uuid: info.uuid})
        if (requested) observedActive = !!info.active
        else listPending = false
        return requested
    }
    function ensureCurrent() {
        if (info.uuid && failedListUuid !== info.uuid && (response.uuid !== info.uuid || observedActive !== !!info.active)) refresh()
    }
    function sizeLabel(bytes) { return bytes >= 1073741824 ? (bytes / 1073741824).toFixed(1) + " GiB" : (bytes / 1048576).toFixed(1) + " MiB" }
    function selectSnapshot(index) {
        if (index < 0 || index >= snapshots.length) return
        selectedKey = key(snapshots[index])
        Qt.callLater(timeline.focusSelection)
    }
    function reconcileSelection() {
        if (snapshots.length === 0) { selectedKey = "__working__"; return }
        if (selectCreatedName) {
            for (let item of snapshots) if (item.name === selectCreatedName) {
                selectedKey = key(item)
                selectCreatedName = ""
                break
            }
        }
        if (selectedIndex < 0 && !workingSelected) {
            selectedKey = "__working__"
        }
    }
    function suggestedName() {
        const base = "Snapshot " + Qt.formatDateTime(new Date(), "yyyy-MM-dd HH.mm.ss")
        let name = base, suffix = 2
        while (snapshots.some(function(item) { return item.name === name })) name = base + " " + suffix++
        return name
    }
    function captureNow() {
        if (!canCapture) return
        failure = ""
        captureName = suggestedName(); captureUuid = info.uuid
        capturePending = backend.request("snapshots.create", {uuid: captureUuid, name: captureName, memory: !!info.active, incremental: true, preview: page.preview()})
        if (!capturePending) failure = "Another operation is in progress. Try again when it finishes."
    }
    onSnapshotsChanged: Qt.callLater(reconcileSelection)
    onSelectCreatedNameChanged: Qt.callLater(reconcileSelection)
    onVmIdChanged: { if (editor.visible && !editor.applying) editor.reject(); requestedCaptureHost = null; snapshotMenu.close(); pageMenu.close(); snapshotDetails.close(); selectedKey = "__working__"; failure = ""; failedListUuid = ""; selectCreatedName = ""; requestedCaptureUuid = ""; timeline.renamingId = "" }
    onVisibleChanged: { if (visible) { selectedKey = "__working__"; refresh(); Qt.callLater(timeline.focusSelection) } else { snapshotMenu.close(); pageMenu.close() } }
    onInfoChanged: ensureCurrent()
    Connections { target: backend; function onChanged() { if (!backend.busy) page.ensureCurrent() } function onCommandFinished(op, ok, result) {
        if (op === "snapshots.list") {
            page.listPending = false
            if (!ok) { page.failure = result.message; page.failedListUuid = result.uuid || page.info.uuid }
            else page.failure = ""
            Qt.callLater(page.ensureCurrent)
            return
        }
        if (op === "snapshots.create" && page.capturePending) {
            page.capturePending = false
            if (page.captureUuid === page.info.uuid) {
                if (ok) page.selectCreatedName = page.captureName
                else page.failure = result.message
            }
        }
        if (op === "snapshots.edit" && page.renameId) {
            if (page.renameUuid === page.info.uuid) { if (ok) timeline.renamingId = ""; else page.failure = result.message }
            page.renameId = ""
        }
        if (op.indexOf("snapshots.") === 0) {
            page.refresh()
        }
    } }
    GridLayout {
        id: headingRow
        Layout.fillWidth: true; Layout.minimumWidth: 0; columns: page.width < 540 * theme.textScale ? 1 : 2; columnSpacing: 6; rowSpacing: 8
        ColumnLayout {
            Layout.fillWidth: true; spacing: 3
            Label { text: "Snapshots"; font.pixelSize: Math.round((page.compact ? 23 : 27) * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true }
            Label { text: page.ready ? page.snapshots.length + " saved · Right-click a snapshot for actions" : "Loading saved moments…"; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); Layout.fillWidth: true; elide: Text.ElideRight }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight; spacing: 4
            AppButton { objectName: "newSnapshot"; text: "Take snapshot"; iconName: "snapshot"; tone: "primary"; enabled: page.canCapture; hint: "Save memory and disks for a running VM, or disks for a stopped VM"; onClicked: page.captureNow() }
            AppButton { objectName: "snapshotOptions"; iconName: "chevron"; hint: "Name and capture options"; enabled: page.canCapture; onClicked: editor.openFor("create", {}) }
            AppButton { id: pageTools; objectName: "snapshotPageMore"; iconName: "more"; tone: "quiet"; hint: "Undo, storage and help"; onClicked: pageMenu.openBelow(pageTools) }
        }
    }
    Label { text: page.snapshotData.blocker || ""; visible: text !== ""; color: theme.colors.warning; Layout.fillWidth: true; wrapMode: Text.WordWrap }
    ColumnLayout {
        visible: !!page.job.active && page.job.uuid === page.info.uuid
        Layout.fillWidth: true; spacing: 6
        Label { text: page.job.phase || "Saving snapshot…"; color: theme.colors.accent; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round((12) * theme.textScale) }
        AppProgressBar { Layout.fillWidth: true; from: 0; to: Math.max(1, page.job.total || 0); value: page.job.completed || 0; indeterminate: !(page.job.total > 0) }
    }
    ErrorNotice { message: page.failure; actionLabel: "Open snapshot storage"; onRecover: storageDialog.open() }
    Label { text: "Interrupted work needs review. Open Storage to recover or remove unused files."; visible: !!page.storage.restoreRecovery || !!page.storage.guestFrozen || (page.storage.candidates || []).some(function(v) { return v.key.indexOf("incomplete:") === 0 }); Layout.fillWidth: true; wrapMode: Text.WordWrap; color: theme.colors.warning }
    RowLayout {
        id: mapTools
        Layout.fillWidth: true; spacing: 6
        Rectangle { width: 7; height: 7; radius: 4; color: page.currentSnapshot.name ? theme.colors.accent : theme.colors.muted }
        Label { objectName: "currentSnapshotLabel"; text: page.currentSnapshot.name ? "Current snapshot · " + page.currentSnapshot.name : "No snapshot yet"; font.weight: Font.DemiBold; font.pixelSize: Math.round(12 * theme.textScale); color: theme.colors.accent; Layout.fillWidth: true; elide: Text.ElideRight; ToolTip.visible: currentNameHover.hovered; ToolTip.text: text; HoverHandler { id: currentNameHover } }
        AppButton { objectName: "locateWorkingState"; iconName: "snapshot"; hint: "Find You are here and its current snapshot"; tone: "quiet"; implicitHeight: 30; implicitWidth: 30; onClicked: { page.selectedKey = "__working__"; Qt.callLater(timeline.focusSelection) } }
        AppButton { objectName: "fitBranches"; text: timeline.fit ? "1:1" : "Fit"; hint: timeline.fit ? "Return to readable size" : "Fit all branches"; tone: "quiet"; implicitHeight: 30; onClicked: timeline.fit = !timeline.fit }
    }
    SnapshotGraph {
        id: timeline; objectName: "snapshotBranchMap"
        Layout.fillWidth: true; Layout.preferredHeight: Math.max(160, Math.min(680, page.viewportHeight - headingRow.implicitHeight - mapTools.implicitHeight - selection.implicitHeight - 3 * page.spacing - 40))
        visible: page.ready
        snapshots: page.snapshots; selectedId: page.selectedKey; currentId: page.currentId; vmState: page.info.state || ""
        editable: page.ready && !!page.info.owned && !backend.busy
        onRenameRequested: function(id) { page.renameSnapshot(id) }
        onRenameSubmitted: function(id, newName) {
            page.failure = ""; page.renameUuid = page.info.uuid; page.renameId = id
            if (!backend.request("snapshots.edit", {uuid: page.renameUuid, id: id, name: newName})) { page.failure = "Another operation is in progress."; page.renameId = "" }
        }
        onPicked: function(snapshotId) { page.selectedKey = snapshotId }
        onContextRequested: function(id, anchor, x, y) { page.openContext(id, anchor, x, y) }
        onPrevious: page.selectSnapshot(page.selectedIndex - 1)
        onNext: page.selectSnapshot(page.selectedIndex + 1)
        onDeleteRequested: page.deleteSelection()
    }
    ColumnLayout {
        id: selection
        visible: page.ready
        Layout.fillWidth: true; spacing: 6
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            ColumnLayout {
                Layout.fillWidth: true; spacing: 4
                Label { objectName: "selectedSnapshotName"; text: page.workingSelected ? "You are here" : page.selectedSnapshot.name || ""; font.pixelSize: Math.round(14 * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true; elide: Text.ElideRight }
                Label { text: page.workingSelected ? (page.currentSnapshot.name ? "Unsaved state after " + page.currentSnapshot.name : "Take a snapshot to save this state") : (page.selectedSnapshot.memory ? "Memory + disks · " : "Disk only · ") + (page.selectedSnapshot.kind === "copy" ? page.sizeLabel(page.selectedSnapshot.bytes || 0) + " stored" : "Size unavailable"); font.pixelSize: Math.round(11 * theme.textScale); color: theme.colors.muted; Layout.fillWidth: true; elide: Text.ElideRight }
            }
            AppButton { objectName: "restoreCheckpoint"; text: "Revert here…"; visible: !page.workingSelected; iconName: "history"; tone: "primary"; enabled: page.ready && !!page.info.owned && !backend.busy && !page.snapshotData.restoreBlocker && page.selectedSnapshot.healthy !== false; onClicked: editor.openFor("restore", page.selectedSnapshot) }
            AppButton { objectName: "revertSnapshot"; text: "Revert…"; iconName: "history"; visible: page.workingSelected && !!page.currentSnapshot.name; enabled: page.ready && !!page.info.owned && !backend.busy && !page.snapshotData.restoreBlocker && page.currentSnapshot.healthy !== false; hint: "Discard changes and restore " + (page.currentSnapshot.name || "the current snapshot"); onClicked: page.revertCurrent() }
            AppButton { objectName: "snapshotDetailsButton"; text: "Details"; tone: "quiet"; visible: !page.workingSelected; onClicked: page.showDetails(page.selectedSnapshot) }
            AppButton { id: snapshotTools; objectName: "snapshotMore"; iconName: "more"; hint: "Actions for the selected snapshot"; tone: "quiet"; onClicked: page.openContext(page.selectedKey, snapshotTools, snapshotTools.width - snapshotMenu.width, snapshotTools.height + 4) }
        }
        Label { text: page.snapshotData.restoreBlocker || (page.selectedSnapshot.healthy === false ? "Verify or recover this snapshot's files before restoring." : ""); visible: text !== ""; color: theme.colors.warning; Layout.fillWidth: true; wrapMode: Text.WordWrap }
    }
    AppMenu {
        id: pageMenu; objectName: "snapshotPageMenu"
        AppMenuItem { objectName: "undoCheckpoint"; text: "Undo last restore…"; enabled: !!page.storage.undoId && !backend.busy && !!page.info.owned; onTriggered: editor.openFor("undo", {id: page.storage.undoId, name: "Before the last restore"}) }
        AppMenuItem { objectName: "checkpointStorage"; text: "Snapshot storage…"; onTriggered: storageDialog.open() }
        AppMenuItem { text: "Refresh"; enabled: !backend.busy; onTriggered: page.refresh() }
        MenuSeparator {}
        AppMenuItem { objectName: "snapshotHelp"; text: "How snapshots work…"; onTriggered: snapshotHelp.open() }
    }
    AppDialog {
        id: snapshotDetails; objectName: "snapshotDetailsDialog"
        property var snapshot: ({})
        heading: snapshot.name || "Snapshot details"; headerIcon: "snapshot"
        width: Math.min(680, parent.width - 40); height: Math.min(600, parent.height - 40)
        contentItem: ScrollView {
            clip: true; contentWidth: availableWidth
            ColumnLayout { width: parent.width; spacing: 8
                Image { visible: !!snapshotDetails.snapshot.previewUrl; Layout.fillWidth: true; Layout.preferredHeight: 180; source: snapshotDetails.snapshot.previewUrl || ""; fillMode: Image.PreserveAspectFit }
                AppButton { objectName: "snapshotPreview"; text: "Open preview"; visible: !!snapshotDetails.snapshot.previewUrl; tone: "quiet"; onClicked: { page.showPreview(snapshotDetails.snapshot); snapshotDetails.close() } }
                DetailRow { Layout.fillWidth: true; label: "Includes"; value: snapshotDetails.snapshot.memory ? "Memory, devices and disks" : "Disks only" }
                DetailRow { Layout.fillWidth: true; label: "Captured"; value: Qt.formatDateTime(new Date((snapshotDetails.snapshot.time || 0) * 1000), "MMM d, yyyy · HH:mm:ss") }
                DetailRow { Layout.fillWidth: true; label: "Parent"; value: snapshotDetails.snapshot.parent || "Initial state" }
                DetailRow { Layout.fillWidth: true; label: "Stored size"; value: snapshotDetails.snapshot.kind === "copy" ? page.sizeLabel(snapshotDetails.snapshot.bytes || 0) : "Size unavailable" }
                DetailRow { Layout.fillWidth: true; label: "Verification"; value: snapshotDetails.snapshot.health || "Not verified" }
                DetailRow { visible: !!snapshotDetails.snapshot.notes; Layout.fillWidth: true; label: "Notes"; value: snapshotDetails.snapshot.notes || "" }
                DetailRow { visible: !!snapshotDetails.snapshot.tags; Layout.fillWidth: true; label: "Tags"; value: snapshotDetails.snapshot.tags || "" }
                Label { text: "Stored size includes this snapshot's files. Shared files needed by other snapshots may remain after deletion."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round(11 * theme.textScale) }
            }
        }
    }
    AppDialog {
        id: snapshotHelp; objectName: "snapshotHelpDialog"
        heading: "Using snapshots"; headerIcon: "snapshot"
        width: Math.min(570, parent.width - 40); height: Math.min(440, parent.height - 40)
        contentItem: ScrollView {
            clip: true; contentWidth: availableWidth
            ColumnLayout { width: parent.width; spacing: 16
                Label { text: "Take snapshot includes memory, CPU/device state and disks when the VM is running or paused. A stopped VM has no memory to save. Memory snapshots resume the captured session; disk-only snapshots need a fresh boot."; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                Label { text: "To make a branch, restore an earlier snapshot, make your changes, then take another snapshot. Existing branches stay available."; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                Label { text: "You are here marks your current VM. Selecting a saved snapshot only browses it. Restore returns to the selected snapshot. Revert returns to your current saved parent."; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                Label { text: "Current snapshot marks the saved point your VM is based on. You are here is its unsaved working state, connected by dots. All branches stay available. Drag to pan or use Fit. Right-click a saved card for Restore, Rename, Details or deletion of just that snapshot or its whole branch. Shift+F10 opens the same menu; F2 renames."; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: theme.colors.muted }
            }
        }
    }
    AppDialog {
        id: snapshotPreview; objectName: "snapshotPreviewDialog"
        property var snapshot: ({})
        heading: snapshot.name || "Snapshot preview"; subtitle: "Saved console image · " + page.sizeLabel(snapshot.bytes || 0) + " stored"; headerIcon: "snapshot"
        width: Math.min(820, parent.width - 40); height: Math.min(560, parent.height - 40)
        contentItem: Image { source: snapshotPreview.snapshot.previewUrl || ""; fillMode: Image.PreserveAspectFit; asynchronous: true }
    }
    AppMenu {
        id: snapshotMenu; objectName: "snapshotContextMenu"
        margins: 8
        AppMenuItem { objectName: "snapshotContextName"; text: page.contextWorking ? "You are here" : page.contextSnapshot.name || "Snapshot unavailable"; enabled: false }
        MenuSeparator {}
        AppMenuItem { objectName: "contextCapture"; text: "Take snapshot"; visible: page.contextWorking; height: visible ? implicitHeight : 0; enabled: page.contextValid && page.canCapture; onTriggered: page.captureNow() }
        AppMenuItem { objectName: "contextRevert"; text: "Revert to current snapshot…"; visible: page.contextWorking; height: visible ? implicitHeight : 0; enabled: page.contextEditable && !!page.currentSnapshot.name && !page.snapshotData.restoreBlocker && page.currentSnapshot.healthy !== false; onTriggered: page.revertCurrent() }
        AppMenuItem { objectName: "contextRestore"; text: "Revert to this snapshot…"; visible: !page.contextWorking; height: visible ? implicitHeight : 0; enabled: page.contextEditable && !page.snapshotData.restoreBlocker && page.contextSnapshot.healthy !== false; onTriggered: editor.openFor("restore", page.contextSnapshot) }
        AppMenuItem { objectName: "renameSnapshot"; text: "Rename"; visible: !page.contextWorking; height: visible ? implicitHeight : 0; enabled: page.contextEditable && page.contextSnapshot.kind === "copy"; onTriggered: page.renameSnapshot(page.contextKey) }
        AppMenuItem { objectName: "contextDetails"; text: "Details & preview…"; visible: !page.contextWorking; height: visible ? implicitHeight : 0; enabled: page.contextValid; onTriggered: page.showDetails(page.contextSnapshot) }
        AppMenuItem { objectName: "editCheckpoint"; text: "Edit notes & options…"; visible: !page.contextWorking; height: visible ? implicitHeight : 0; enabled: page.contextEditable && page.contextSnapshot.kind === "copy"; onTriggered: editor.openFor("edit", page.contextSnapshot) }
        AppMenuItem { objectName: "cloneCheckpoint"; text: "Create VM from snapshot…"; visible: !page.contextWorking; height: visible ? implicitHeight : 0; enabled: page.contextEditable && page.contextSnapshot.kind === "copy" && page.contextSnapshot.healthy !== false; onTriggered: editor.openFor("clone", page.contextSnapshot) }
        AppMenuItem { text: "Verify stored files…"; visible: !page.contextWorking; height: visible ? implicitHeight : 0; enabled: page.contextEditable && page.contextSnapshot.kind === "copy"; onTriggered: editor.openFor("verify", page.contextSnapshot) }
        MenuSeparator { visible: !page.contextWorking; height: visible ? implicitHeight : 0 }
        AppMenuItem { objectName: "deleteSnapshot"; text: "Delete this snapshot…"; visible: !page.contextWorking; height: visible ? implicitHeight : 0; enabled: page.contextEditable; onTriggered: deletion.openFor(page.contextSnapshot, false) }
        AppMenuItem { objectName: "deleteSnapshotBranch"; text: "Delete this branch…"; visible: !page.contextWorking && page.contextHasChildren; height: visible ? implicitHeight : 0; enabled: page.contextEditable; onTriggered: deletion.openFor(page.contextSnapshot, true) }
    }
    EditorDialog {
        id: deletion; objectName: "deleteSnapshotDialog"
        property string uuid: ""
        property var snapshot: ({})
        property var branch: []
        property string parentKey: ""
        readonly property var affected: wholeBranch.checked ? branch : [snapshot]
        readonly property int pinnedCount: affected.filter(function(s) { return !!s.pinned }).length
        readonly property bool removesUndo: affected.some(function(s) { return s.id === page.storage.undoId })
        readonly property bool stoppedRequired: snapshot.kind === "internal" && !!page.info.active
        headerIcon: "trash"
        heading: wholeBranch.checked ? "Delete this snapshot branch?" : "Delete this snapshot?"
        actionText: wholeBranch.checked ? "Delete " + affected.length + " snapshots" : "Delete snapshot"
        actionTone: "danger"
        height: Math.min(parent.height - 40, 410 + affected.length * 24 + (branch.length > 1 ? 40 : 0) + (pinnedCount > 0 ? 48 : 0) + (removesUndo ? 48 : 0))
        ready: !stoppedRequired && (pinnedCount === 0 || unpin.checked)
        function openFor(data, descendants) {
            uuid = page.info.uuid; snapshot = data; parentKey = data.parentId || "__working__"
            const ids = {}; ids[page.key(data)] = true
            let added = true
            while (added) {
                added = false
                page.snapshots.forEach(function(s) { if (!ids[page.key(s)] && ids[s.parentId]) { ids[page.key(s)] = true; added = true } })
            }
            branch = page.snapshots.filter(function(s) { return !!ids[page.key(s)] })
            wholeBranch.checked = !!descendants && branch.length > 1; onlySnapshot.checked = !wholeBranch.checked; unpin.checked = false; open()
        }
        Label { text: "“" + (deletion.snapshot.name || "") + "”"; Layout.fillWidth: true; font.pixelSize: Math.round((17) * theme.textScale); font.weight: Font.DemiBold; wrapMode: Text.WordWrap }
        ButtonGroup { id: deletionScope }
        AppRadioButton { id: onlySnapshot; objectName: "deleteOnlySnapshot"; text: "Delete only this snapshot"; checked: true; ButtonGroup.group: deletionScope }
        AppRadioButton { id: wholeBranch; objectName: "deleteWholeBranch"; text: "Delete this snapshot and all descendants (" + deletion.branch.length + ")"; visible: deletion.branch.length > 1; ButtonGroup.group: deletionScope }
        Label { text: wholeBranch.checked ? "All snapshots listed below will be permanently removed. Other branches stay available." : deletion.branch.length > 1 ? "Child snapshots are kept and reconnect to this snapshot’s parent." : "This saved point will be permanently removed."; Layout.fillWidth: true; color: theme.colors.muted; wrapMode: Text.WordWrap }
        Label { text: "Snapshots to delete"; font.weight: Font.DemiBold }
        Repeater { model: deletion.affected
            Label { required property var modelData; text: "• " + modelData.name + (modelData.pinned ? " · Pinned" : ""); Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round((12) * theme.textScale)}
        }
        AppCheckBox { id: unpin; objectName: "deletePinnedSnapshots"; visible: deletion.pinnedCount > 0; text: deletion.pinnedCount === 1 ? "Also remove the pinned snapshot" : "Also remove " + deletion.pinnedCount + " pinned snapshots" }
        Label { visible: deletion.removesUndo; text: "This includes the recovery point used by Undo. That Undo action will no longer be available."; color: theme.colors.warning; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        Label { text: deletion.stoppedRequired ? "Shut down the VM before deleting legacy internal snapshots." : "Your current VM and its working disks stay as they are. Unused files are removed; files needed by surviving snapshots stay in Storage until they are no longer needed."; color: deletion.stoppedRequired ? theme.colors.warning : theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        onSubmitted: execute("snapshots.remove", {uuid: uuid, id: snapshot.id || "", name: snapshot.name, descendants: wholeBranch.checked, expectedIds: affected.map(function(s) { return page.key(s) }), allowPinned: pinnedCount > 0 && unpin.checked})
        onCompleted: { if (uuid === page.info.uuid) { page.selectCreatedName = ""; page.selectedKey = parentKey } }
    }
    EditorDialog {
        id: editor; objectName: "checkpointEditor"
        property string uuid: ""
        property string verb: "create"
        property string checkpointId: ""
        property bool liveCapture: false
        property bool discardChanges: false
        property bool savedMemory: false
        property bool savedPaused: false
        property bool legacy: false
        property bool editing: verb === "create" || verb === "edit"
        property bool advanced: false
        readonly property bool showDetails: verb === "edit" || advanced
        height: Math.min(parent.height - 40, showDetails ? 680 : verb === "create" ? 360 : 480)
        width: Math.min(parent.width - 40, verb === "create" && !advanced ? 460 : 650)
        ready: (!editing && verb !== "clone") || /^[A-Za-z0-9][A-Za-z0-9 _.-]{0,63}$/.test(name.text.trim())
        headerIcon: verb === "restore" || verb === "undo" ? "history" : "snapshot"
        heading: verb === "create" ? "Name this moment" : verb === "edit" ? "Snapshot details" : verb === "clone" ? "Create VM from snapshot" : verb === "verify" ? "Verify this snapshot" : verb === "undo" ? "Undo the last restore?" : verb === "restore" ? (discardChanges ? "Revert to your current snapshot?" : "Revert to this snapshot?") : "Delete this snapshot?"
        actionText: verb === "create" ? "Take snapshot" : verb === "edit" ? "Save details" : verb === "clone" ? "Create VM" : verb === "verify" ? "Verify snapshot" : verb === "restore" || verb === "undo" ? (savedMemory ? "Revert" : liveCapture ? "Revert & reboot" : "Revert") : "Delete snapshot"
        actionTone: editing || verb === "clone" || verb === "verify" ? "primary" : "danger"
        function openFor(operation, data, discard, host) {
            parent = host || page.Overlay.overlay
            discardChanges = !!discard; uuid = page.info.uuid; verb = operation; liveCapture = !!page.info.active; legacy = data.kind === "internal"; checkpointId = data.id || "";
            const saved = page.snapshots.find(function(s) { return s.id === checkpointId }) || data
            savedMemory = !!saved.memory; savedPaused = saved.memoryState === 3; memory.checked = liveCapture
            advanced = false;
            name.text = verb === "clone" ? "snapshot-copy" : data.name || page.suggestedName(); notes.text = data.notes || ""; tags.text = data.tags || "";
            pinned.checked = !!data.pinned; good.checked = !!data.knownGood; thumbnail.checked = liveCapture && !!page.preview(); clean.checked = false; incremental.checked = true; open()
            if (editing || verb === "clone") Qt.callLater(function() { name.forceActiveFocus(); name.selectAll() })
        }
        Label { text: editor.verb === "clone" ? "New VM name" : "Name" }
        AppField { id: name; objectName: "checkpointName"; Layout.fillWidth: true; readOnly: !editor.editing && editor.verb !== "clone"; onAccepted: if (editor.verb === "create" && editor.ready && !editor.applying && !backend.busy) editor.submitted() }
        AppCheckBox { id: memory; objectName: "checkpointMemory"; text: "Include running memory (resume without reboot)"; visible: editor.verb === "create" && editor.liveCapture && editor.advanced }
        AppCheckBox { id: thumbnail; objectName: "checkpointThumbnail"; text: "Include a console preview"; visible: editor.verb === "create" && editor.liveCapture && editor.advanced }
        AppButton { objectName: "snapshotAdvanced"; text: editor.advanced ? "Hide options" : "More options"; iconName: "chevron"; tone: "quiet"; visible: editor.verb === "create"; onClicked: editor.advanced = !editor.advanced }
        Label { text: "Notes"; visible: editor.editing && editor.showDetails }
        AppTextArea { id: notes; objectName: "checkpointNotes"; visible: editor.editing && editor.showDetails; Layout.fillWidth: true; Layout.preferredHeight: 80; wrapMode: TextEdit.WordWrap; placeholderText: "What are you preserving?"; color: theme.colors.foreground }
        Label { text: "Tags"; visible: editor.editing && editor.showDetails }
        AppField { id: tags; objectName: "checkpointTags"; visible: editor.editing && editor.showDetails; Layout.fillWidth: true; placeholderText: "baseline, working, experiment" }
        RowLayout { visible: editor.editing && editor.showDetails
            AppCheckBox { id: pinned; objectName: "checkpointPinned"; text: "Pin against deletion" }
            AppCheckBox { id: good; objectName: "checkpointGood"; text: "Known good" }
        }
        AppCheckBox { id: incremental; objectName: "checkpointIncremental"; text: "Use incremental storage when available"; visible: editor.verb === "create" && editor.liveCapture && editor.advanced && !memory.checked }
        AppCheckBox { id: clean; objectName: "checkpointClean"; text: "Flush guest filesystems for a cleaner capture"; visible: editor.verb === "create" && editor.liveCapture && editor.advanced && !memory.checked; enabled: !!page.info.agentConnected }
        Label { text: "Cleaner capture needs a running QEMU guest agent. Full copies are used when incremental tracking is unavailable."; visible: editor.verb === "create" && editor.liveCapture && editor.advanced && !memory.checked; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
        Label { objectName: "checkpointExplanation"; visible: editor.verb !== "create" || editor.advanced; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: theme.colors.muted
            text: editor.verb === "restore" || editor.verb === "undo" ? (editor.savedMemory ? "The VM jumps back to the exact moment this snapshot was taken: every program, window and byte of memory, with no reboot. The display reconnects for a moment while the saved memory loads. The VM returns " + (editor.savedPaused ? "paused." : "running.") : "This snapshot has no saved memory, so the VM boots from its saved disks. " + (editor.liveCapture ? "Current memory and unsaved work are discarded." : "The VM stays stopped.")) + " Anything since then that isn't in a snapshot is discarded. Other snapshots stay available." : editor.verb === "clone" ? "Creates a separate VM with independent disks, a new VM identity and new adapter MAC addresses. It starts stopped with its adapters disconnected. The new VM boots from disk; saved memory belongs to the original VM. Guest accounts, hostname and operating-system identity are copied; change those before connecting both VMs to the same network." : editor.verb === "verify" ? "Check image structure and checksum the snapshot and its dependencies. This can take time. You can continue browsing in the background." : editor.verb === "remove" ? "Permanently remove this snapshot. Current VM disks are retained." : editor.verb === "edit" ? "Pin protects a snapshot from deletion. Known good is your label; use Verify to check stored files." : (editor.liveCapture && memory.checked ? "Saves RAM, CPU/device state and disks together. Capture pauses while saving memory, then copies disks in the background. Restoring resumes this moment; external network connections may need to reconnect." : "Captures disks and settings only. Restore needs a fresh boot; current memory and unsaved work are not saved.")
        }
        onSubmitted: execute("snapshots." + verb, {uuid: uuid, id: checkpointId, name: name.text, notes: notes.text, tags: tags.text, pinned: pinned.checked, knownGood: good.checked, memory: liveCapture && memory.checked, incremental: incremental.checked && !memory.checked, clean: clean.checked && !memory.checked, preview: thumbnail.checked ? page.preview() : "", allowRestart: liveCapture, safety: false})
        onCompleted: function(result) {
            if (uuid === page.info.uuid && verb === "create") page.selectCreatedName = name.text.trim()
            if (uuid === page.info.uuid && (verb === "restore" || verb === "undo")) {
                page.selectCreatedName = ""
                page.selectedKey = checkpointId || "internal:" + name.text
            }
        }
    }
    AppDialog {
        id: storageDialog; objectName: "checkpointStorageDialog"; parent: Overlay.overlay; anchors.centerIn: parent; width: Math.min(700, parent.width - 40); height: Math.min(620, parent.height - 40, 310 + (page.storage.candidates || []).length * 100); padding: 22; modal: true
        heading: "Snapshot storage"
        headerIcon: "disk"
        contentItem: ScrollView { clip: true; contentWidth: availableWidth
            ColumnLayout { width: parent.width; spacing: 14
                Label { text: page.sizeLabel(page.storage.checkpointBytes || 0) + " snapshots · " + page.sizeLabel(page.storage.retainedBytes || 0) + " working / retained files"; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                Label { text: page.sizeLabel(page.storage.reclaimableBytes || 0) + " eligible for cleanup"; color: theme.colors.accent; Layout.fillWidth: true }
                Label { text: page.storage.cleanupBlocker || "Files in use by a VM, pending settings or recovery are protected."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                AppButton { text: page.storage.guestFrozen ? "Recover interrupted capture" : "Recover interrupted restore"; visible: !!page.storage.restoreRecovery || !!page.storage.guestFrozen; enabled: !backend.busy; onClicked: recovery.open() }
                Repeater { model: page.storage.candidates || []
                    ColumnLayout { required property var modelData; Layout.fillWidth: true
                        Label { text: modelData.name + " · " + page.sizeLabel(modelData.bytes); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        Label { text: modelData.reason; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round((11) * theme.textScale)}
                        AppButton { text: "Remove unused files…"; enabled: modelData.available && !backend.busy; onClicked: { cleanup.key = modelData.key; cleanup.label = modelData.name; cleanup.uuid = page.info.uuid; cleanup.open() } }
                        Rectangle { Layout.fillWidth: true; height: 1; color: theme.colors.border }
                    }
                }
            }
        }
        footer: Item { implicitHeight: 64; Rectangle { width: parent.width; height: 1; color: theme.colors.border } AppButton { text: "Close"; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.rightMargin: 22; onClicked: storageDialog.close() } }
    }
    EditorDialog { id: cleanup; property string key: ""; property string label: ""; property string uuid: ""; heading: "Remove unused files?"; actionText: "Remove files"; actionTone: "danger"; height: Math.min(360, parent.height - 40)
        Label { text: cleanup.label + " will be permanently removed. References will be checked again before deleting."; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        onSubmitted: execute("snapshots.cleanup", {uuid: uuid, key: key})
    }
    EditorDialog { id: recovery; heading: "Recover interrupted snapshot work"; actionText: "Recover"; height: Math.min(400, parent.height - 40)
        Label { text: "Release guest filesystems frozen by an interrupted capture. For an interrupted restore, keep a completed running restore or return a stopped VM to its previous disks and power state. Files remain available for storage review."; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        onSubmitted: execute("snapshots.recover", {uuid: page.info.uuid})
    }
}
