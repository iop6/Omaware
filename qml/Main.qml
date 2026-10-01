// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Omaware 1.0

ApplicationWindow {
    id: root
    visible: true
    width: 1280; height: 840
    minimumWidth: 940; minimumHeight: 660
    title: selected ? selected.name + " — OmaWare" : "OmaWare"
    color: theme.colors.background
    palette.window: theme.colors.background
    palette.windowText: theme.colors.foreground
    palette.text: theme.colors.foreground
    palette.button: theme.colors.surface
    palette.buttonText: theme.colors.foreground
    palette.base: theme.colors.field
    palette.alternateBase: theme.colors.subtle
    palette.mid: theme.colors.border
    palette.light: theme.colors.raised
    palette.dark: theme.colors.background
    palette.highlight: theme.colors.accent
    palette.highlightedText: theme.colors.accentText
    // Monospace follows the font Omarchy configures for fontconfig's "monospace".
    font.family: "monospace"
    font.pixelSize: Math.round((12) * theme.textScale)
    background: Rectangle {
        color: theme.colors.background
        // Faint dot grid; static, repainted only on resize or palette change.
        Canvas {
            id: backdrop
            anchors.fill: parent
            onWidthChanged: requestPaint(); onHeightChanged: requestPaint()
            Connections { target: theme; function onChanged() { backdrop.requestPaint() } }
            onPaint: {
                const ctx = getContext("2d"); ctx.reset(); ctx.fillStyle = String(theme.colors.border)
                // Hacker mode gets CRT scanlines; other palettes a dot grid.
                if (theme.mode === "hacker") { ctx.globalAlpha = .35; for (let y = 0; y < height; y += 3) ctx.fillRect(0, y, width, 1); return }
                for (let y = 12; y < height; y += 22) for (let x = 12; x < width; x += 22) ctx.fillRect(x, y, 2, 2)
            }
        }
    }
    Workspace { id: preferences }
    property string navigation: "library"
    property string folderFilter: "All folders"
    property bool compactLibrary: false
    property bool consoleDetached: false
    function vmName(vm) { const revision = preferences.revision; return preferences.vm(vm.uuid).alias || vm.name }
    function vmLabel(vm) { const revision = preferences.revision; const meta = preferences.vm(vm.uuid); return (meta.favorite ? "★ " : "") + (meta.alias || vm.name) }
    function folders() {
        const revision = preferences.revision
        let result = ["All folders"]
        for (let vm of backend.domains) { let folder = preferences.vm(vm.uuid).folder; if (folder && result.indexOf(folder) < 0) result.push(folder) }
        return result
    }
    Component.onCompleted: {
        width = Math.max(minimumWidth, Number(preferences.get("width", 1280)))
        height = Math.max(minimumHeight, Number(preferences.get("height", 840)))
        compactLibrary = !!preferences.get("compact", false)
        sidebarRail = !!preferences.get("sidebarRail", false)
        try { collapsedGroups = JSON.parse(String(preferences.get("collapsedGroups", "{}"))) || ({}) } catch (e) { collapsedGroups = ({}) }
        theme.setMode(String(preferences.get("theme", theme.omarchyAvailable ? "omarchy" : "dark")))
        theme.setTextScale(Number(preferences.get("textScale", 1)))
        theme.setReducedMotion(!!preferences.get("reducedMotion", false))
        const remembered = String(preferences.get("selectedVm", ""))
        if (remembered) selectedUuid = remembered
        try { exitPaused = JSON.parse(String(preferences.get("exitPaused", "[]"))) || [] } catch (e) { exitPaused = [] }
    }
    onClosing: function(close) {
        if (visibility !== Window.FullScreen) { preferences.set("width", width); preferences.set("height", height) }
        preferences.set("compact", compactLibrary)
        if (exiting) return
        if (pausingForExit) { close.accepted = false; return }
        // Pause running VMs first so they pick up where they left off next time; the window closes once that finishes.
        const running = backend.connected ? backend.domains.filter(function(row) { return row.owned && row.stateCode === 1 }) : []
        if (running.length === 0) return
        close.accepted = false
        pausingForExit = true
        exitDialog.failures = []
        exitDialog.count = running.length
        exitDialog.open()
        display.releaseInput()
        backend.pauseForExit()
    }
    onNavigationChanged: { display.releaseInput(); if (navigation === "networks") networksPage.refresh() }
    function openShop() { createDialog.close(); navigation = "isos" }
    onConsoleDetachedChanged: {
        display.releaseInput()
        consoleToolbar.closePopups()
        // Return a window to its normal size before moving its console elsewhere.
        if (root.visibility === Window.FullScreen) root.showNormal()
        if (detachedConsole.visibility === Window.FullScreen) detachedConsole.showNormal()
    }

    property string selectedUuid: ""
    property var selected: {
        for (let row of backend.domains) if (row.uuid === selectedUuid) return row
        return null
    }
    property bool running: selected !== null && selected.stateCode === 1
    property bool paused: selected !== null && selected.stateCode === 3
    property bool stopped: selected !== null && selected.stateCode === 5
    property bool permitted: backend.connected && !backend.busy && selected !== null && selected.owned
    property bool consoleWanted: true
    property string consoleUuid: ""
    property bool displayAvailable: selected !== null && selected.owned && (running || paused)
    property bool consoleExpanded: false
    readonly property bool mainConsoleFullscreen: visibility === Window.FullScreen && !consoleDetached && navigation === "library" && !detailsOpen
    readonly property bool consoleFullScreen: consoleDetached ? detachedConsole.visibility === Window.FullScreen : mainConsoleFullscreen
    readonly property bool consoleFocus: consoleExpanded || mainConsoleFullscreen
    property bool toolbarRevealed: false
    onConsoleFullScreenChanged: { toolbarRevealed = false; display.releaseInput() }
    function toggleConsoleFullscreen() {
        display.releaseInput()
        const host = consoleDetached ? detachedConsole : root
        if (host.visibility === Window.FullScreen) host.showNormal()
        else host.showFullScreen()
    }
    property bool detailsOpen: false
    property bool logOpen: false
    property string clock: Qt.formatTime(new Date(), "HH:mm")
    Timer { interval: 10000; running: root.visible; repeat: true; onTriggered: root.clock = Qt.formatTime(new Date(), "HH:mm") }
    property string searchQuery: ""
    property int stateFilter: 0
    property string operationError: ""
    property string removingUuid: ""
    property var filteredDomains: {
        const revision = preferences.revision
        return backend.domains.filter(function(row) {
            const meta = preferences.vm(row.uuid)
            const matches = [row.name, meta.alias || "", meta.tags || "", meta.folder || ""].join(" ").toLowerCase().indexOf(searchQuery.trim().toLowerCase()) !== -1
            return matches && (folderFilter === "All folders" || meta.folder === folderFilter) && (stateFilter === 0 || (stateFilter === 1 && (row.stateCode === 1 || row.stateCode === 3)) || (stateFilter === 2 && row.stateCode === 5))
        }).sort(function(a, b) { const ga = groupKey(a), gb = groupKey(b); return ga !== gb ? ga.localeCompare(gb) : vmLabel(a).localeCompare(vmLabel(b)) })
    }
    property int activeCount: backend.domains.filter(function(row) { return row.stateCode === 1 || row.stateCode === 3 }).length

    // ---- Sidebar library: groups, rail and per-VM actions ----------------
    property bool sidebarRail: false
    property var collapsedGroups: ({})
    property var pendingVmAction: null

    // ---- Pause on close, and several VMs selected at once ----------------
    property bool pausingForExit: false
    property bool exiting: false
    // Set when this copy was started by OmaWare itself after an update: reopen where you were.
    property bool restarted: false
    onRestartedChanged: if (restarted) { const page = String(preferences.get("restoreNavigation", "library")); if (["library", "monitor", "networks", "isos"].indexOf(page) >= 0) navigation = page }
    property var exitPaused: []   // VMs OmaWare paused when it last closed
    readonly property var pausedOnExit: backend.domains.filter(function(row) { return root.exitPaused.indexOf(row.uuid) >= 0 && row.stateCode === 3 }).map(function(row) { return row.uuid })
    function forgetExitPaused() { exitPaused = []; preferences.set("exitPaused", "[]") }
    property var marked: []       // Ctrl/Shift-click selection in the library
    property string markAnchor: ""
    function visibleUuids() {
        return filteredDomains.filter(function(vm) { return sidebarRail || !grouped || !collapsedGroups[groupOf(vm)] }).map(function(vm) { return vm.uuid })
    }
    function clickVm(uuid, modifiers) {
        if (!sidebarRail && (modifiers & Qt.ControlModifier)) {
            let next = marked.length ? marked.slice() : (selectedUuid && selectedUuid !== uuid ? [selectedUuid] : [])
            const at = next.indexOf(uuid)
            if (at >= 0) next.splice(at, 1); else next.push(uuid)
            marked = next; markAnchor = uuid; library.forceActiveFocus()
            return
        }
        if (!sidebarRail && (modifiers & Qt.ShiftModifier)) {
            const ids = visibleUuids(), from = ids.indexOf(markAnchor || selectedUuid), to = ids.indexOf(uuid)
            if (from >= 0 && to >= 0) { marked = ids.slice(Math.min(from, to), Math.max(from, to) + 1); library.forceActiveFocus(); return }
        }
        marked = []; markAnchor = uuid
        selectVm(uuid)
    }
    // How many marked VMs a bulk action would change.
    function markedFor(operation) {
        return marked.filter(function(uuid) {
            const vm = vmByUuid(uuid)
            if (!vm || !vm.owned) return false
            if (operation === "power-on") return vm.stateCode === 5 || vm.stateCode === 3
            if (operation === "pause" || operation === "shutdown") return vm.stateCode === 1
            return vm.stateCode !== 5
        })
    }
    function runBulk(operation, uuids) {
        if (operation === "force-off") confirmDialog.confirmBulk(uuids)
        else backend.bulkAction(uuids, operation)
    }
    onFilteredDomainsChanged: {
        if (!marked.length) return
        const next = marked.filter(function(uuid) { return root.filteredDomains.some(function(vm) { return vm.uuid === uuid }) })
        if (next.length !== marked.length) marked = next
    }
    function setSidebarRail(on) { sidebarRail = on; preferences.set("sidebarRail", on) }
    function vmMeta(uuid) { const revision = preferences.revision; return preferences.vm(uuid) }
    // Favorites first, then folders alphabetically, then everything unsorted.
    function groupOf(vm) { const meta = vmMeta(vm.uuid); return meta.favorite ? "favorites" : (meta.folder || "") }
    function groupKey(vm) { const g = groupOf(vm); return g === "favorites" ? "0" : g ? "1" + g.toLowerCase() : "2" }
    readonly property bool grouped: filteredDomains.some(function(vm) { return root.groupOf(vm) !== "" })
    function groupCount(g) { return filteredDomains.filter(function(vm) { return root.groupOf(vm) === g }).length }
    function toggleGroup(g) {
        let next = Object.assign({}, collapsedGroups)
        if (next[g]) delete next[g]; else next[g] = true
        collapsedGroups = next; preferences.set("collapsedGroups", JSON.stringify(next))
    }
    function vmByUuid(uuid) { for (let vm of backend.domains) if (vm.uuid === uuid) return vm; return null }
    function selectVm(uuid) { navigation = "library"; selectedUuid = uuid; library.forceActiveFocus() }
    function toggleFavorite(vm) {
        const meta = preferences.vm(vm.uuid)
        preferences.saveVm(vm.uuid, {alias: meta.alias || "", folder: meta.folder || "", tags: meta.tags || "", notes: meta.notes || "", favorite: !meta.favorite})
    }
    function openVmMenu(vm, item, x, y) {
        if (!vm) return
        display.releaseInput()
        vmMenu.vm = vm
        // Anchor to the window, not the row: rows are recreated whenever the library refreshes.
        if (item) { const p = item.mapToItem(root.contentItem, x || 0, y || 0); vmMenu.popup(root.contentItem, p.x, p.y) }
        else vmMenu.popup()
    }
    function confirmFor(remove, vm) { confirmDialog.confirmTarget(remove, vm) }
    // Snapshot and revert run against the selected VM once its details and history have loaded.
    function queueVmAction(uuid, action) {
        selectVm(uuid)
        pendingVmAction = {uuid: uuid, action: action, requested: false}
        pendingTimer.ticks = 0; pendingTimer.restart()
    }
    Timer {
        id: pendingTimer
        property int ticks: 0
        interval: 150; repeat: true
        onTriggered: {
            const p = root.pendingVmAction
            if (!p || ++ticks > 80 || root.selectedUuid !== p.uuid) { stop(); root.pendingVmAction = null; return }
            if (backend.details.uuid !== p.uuid || backend.detailsBusy) return
            const view = vmDetails.snapshotsView
            if (p.action === "snapshot") { stop(); root.pendingVmAction = null; view.requestCapture(); return }
            if (!p.requested) { p.requested = true; view.refresh(); return }
            if (!view.ready) return
            stop(); root.pendingVmAction = null
            if (view.currentSnapshot && view.currentSnapshot.name) view.revertCurrent()
            else root.operationError = "This VM has no current snapshot to revert to. Take a snapshot first."
        }
    }

    function commands() {
        const vm = selected ? vmLabel(selected) : "Select a VM first"
        const available = permitted && !backend.detailsBusy && backend.details.uuid === selectedUuid
        let result = [
            {key: "create", title: "Create a virtual machine", detail: "ISO library or existing disk image", icon: "plus", enabled: backend.connected && !backend.busy},
            {key: "snapshot", title: "Take a snapshot", detail: vm, icon: "snapshot", uuid: selectedUuid, enabled: available},
            {key: "snapshots", title: "Open snapshot history", detail: vm, icon: "branch", uuid: selectedUuid, enabled: !!selected},
            {key: "hardware", title: "Edit CPU, memory and hardware", detail: vm, icon: "cpu", uuid: selectedUuid, enabled: available},
            {key: "console", title: "Open console", detail: vm, icon: "monitor", uuid: selectedUuid, enabled: !!selected},
            {key: "power", title: stopped ? "Start VM" : paused ? "Resume VM" : "Pause VM", detail: vm, icon: stopped || paused ? "play" : "pause", uuid: selectedUuid, enabled: permitted && (stopped || paused || running)},
            {key: "reconnectConsole", title: "Reconnect console", detail: vm, icon: "refresh", uuid: selectedUuid, enabled: permitted && displayAvailable},
            {key: "shutdown", title: "Shut down guest", detail: vm, icon: "power", uuid: selectedUuid, enabled: permitted && running, keywords: "stop halt poweroff acpi"},
            {key: "forceoff", title: "Force power off…", detail: vm, icon: "power", uuid: selectedUuid, enabled: permitted && !stopped, keywords: "kill destroy"},
            {key: "details", title: "Show VM details", detail: vm, icon: "settings", enabled: !!selected, keywords: "info stats performance", shortcut: "d"},
            {key: "containment", title: selected && selected.contained ? "Review containment" : "Contain VM for untrusted software", detail: vm, icon: "shield", enabled: !!selected, keywords: "quarantine isolate sandbox untrusted"},
            {key: "copyuuid", title: "Copy VM UUID", detail: selected ? selected.uuid : vm, icon: "clipboard", enabled: !!selected, keywords: "id yank"},
            {key: "monitor", title: "Open system monitor", detail: "Live CPU, RAM, disk and network across every VM", icon: "cpu", keywords: "top htop stats fleet", shortcut: "Ctrl+2"},
            {key: "log", title: "Toggle log drawer", detail: "Tail of every operation, with grep", icon: "history", keywords: "tail events console", shortcut: "Ctrl+`"},
            {key: "keys", title: "Keyboard map", detail: "Every shortcut in one place", icon: "keyboard", keywords: "help shortcuts cheatsheet", shortcut: "?"},
            {key: "theme:omarchy", title: "Theme: follow Omarchy", icon: "monitor", keywords: "appearance palette colors"},
            {key: "theme:dark", title: "Theme: dark", icon: "moon", keywords: "appearance palette colors"},
            {key: "theme:light", title: "Theme: light", icon: "sun", keywords: "appearance palette colors"},
            {key: "theme:hacker", title: "Theme: hacker", detail: "Phosphor green on black", icon: "terminal", keywords: "appearance palette colors matrix green crt"},
            {key: "updates", title: "Check for OmaWare updates", detail: "You have " + (Qt.application.version || "this version"), icon: "refresh", keywords: "update upgrade new version release"},
            {key: "isos", title: "Open the ISO Shop", detail: "Download or update Ubuntu, Fedora, Debian, Mint, Arch and more", icon: "store", shortcut: "Ctrl+4", keywords: "download iso image installer ubuntu fedora debian windows update"},
            {key: "testvm", title: "Create diskless test VM", detail: "Firmware-only fixture", icon: "plus", enabled: backend.connected && !backend.busy, keywords: "fixture sandbox"},
            {key: "networks", title: "Open Networks", detail: "Connections and virtual switches", icon: "network", shortcut: "Ctrl+3"},
            {key: "activity", title: "Show activity history", detail: "Recent operations, including previous sessions", icon: "history"},
            {key: "appearance", title: "Settings", detail: "Text size, reduced motion, theme, AI agents and what happens to VMs on close", icon: "settings", keywords: "appearance accessibility preferences close quit ai agents claude mcp"},
            {key: "resumeall", title: "Resume all paused VMs", detail: backend.domains.filter(function(row) { return row.owned && row.stateCode === 3 }).length + " paused", icon: "play", enabled: backend.connected && !backend.busy && backend.domains.some(function(row) { return row.owned && row.stateCode === 3 }), keywords: "unpause continue wake bulk"},
            {key: "pauseall", title: "Pause all running VMs", detail: backend.domains.filter(function(row) { return row.owned && row.stateCode === 1 }).length + " running", icon: "pause", enabled: backend.connected && !backend.busy && backend.domains.some(function(row) { return row.owned && row.stateCode === 1 }), keywords: "suspend freeze bulk"},
            {key: "reconnect", title: "Reconnect local VM session", detail: "Refresh the backend connection", icon: "refresh", enabled: !backend.busy}
        ]
        for (let machine of backend.domains) result.push({key: "vm:" + machine.uuid, title: "Open " + vmLabel(machine), detail: machine.state, icon: "monitor", target: machine.uuid})
        return result
    }
    function runCommand(command) {
        if (command.enabled === false) return
        if (command.uuid && command.uuid !== selectedUuid) { operationError = "The selected VM changed. Open Actions and choose again."; return }
        if (command.target) { navigation = "library"; selectedUuid = command.target; detailsOpen = false; return }
        switch (command.key) {
        case "create": if (backend.connected && !backend.busy) createDialog.begin(); break
        case "snapshot": if (permitted) { navigation = "library"; detailsOpen = true; vmDetails.page = 3; vmDetails.snapshotsView.requestCapture() }; break
        case "snapshots": navigation = "library"; detailsOpen = true; vmDetails.page = 3; vmDetails.snapshotsView.refresh(); break
        case "hardware": if (permitted && backend.details.uuid === selectedUuid) hardwareDialog.openFor(backend.details); break
        case "console": navigation = "library"; detailsOpen = false; consoleExpanded = false; break
        case "power": primaryAction(); break
        case "reconnectConsole": if (permitted) reopenConsole(); break
        case "networks": navigation = "networks"; break
        case "shutdown": if (permitted && running) backend.action(selectedUuid, "shutdown"); break
        case "forceoff": if (permitted && !stopped) confirmDialog.confirm(false); break
        case "details": navigation = "library"; if (vmDetails.page === 3) vmDetails.page = 0; detailsOpen = true; break
        case "copyuuid": if (selected) preferences.copy(selected.uuid); break
        case "containment": navigation = "library"; vmDetails.page = 2; detailsOpen = true; break
        case "monitor": navigation = "monitor"; break
        case "log": logOpen = !logOpen; break
        case "keys": keyMap.open(); break
        case "theme:omarchy": case "theme:dark": case "theme:light": case "theme:hacker": { const mode = command.key.split(":")[1]; theme.setMode(mode); preferences.set("theme", mode); break }
        case "testvm": if (backend.connected && !backend.busy) backend.createTest(); break
        case "isos": openShop(); break
        case "updates": openUpdates(true); break
        case "activity": activityDialog.open(); break
        case "appearance": appearanceInfo.open(); break
        case "reconnect": backend.reconnect(); break
        case "resumeall": backend.bulkAction(backend.domains.filter(function(row) { return row.owned && row.stateCode === 3 }).map(function(row) { return row.uuid }), "power-on"); break
        case "pauseall": backend.bulkAction(backend.domains.filter(function(row) { return row.owned && row.stateCode === 1 }).map(function(row) { return row.uuid }), "pause"); break
        }
    }
    function recoverError(action, uuid) {
        if (action === "activity") { activityDialog.open(); return }
        if (action === "connection") { backend.reconnect(); return }
        if (action === "networks") { navigation = "networks"; return }
        if (uuid && backend.domains.some(function(vm) { return vm.uuid === uuid })) selectedUuid = uuid
        if (selected) {
            navigation = "library"; detailsOpen = true; vmDetails.page = action === "storage" ? 3 : 1
            backend.inspect(selectedUuid)
            if (action === "storage") { vmDetails.snapshotsView.refresh(); vmDetails.snapshotsView.openStorage() }
        } else activityDialog.open()
    }
    function memoryLabel(mib) { return mib >= 1024 ? Number((mib / 1024).toFixed(1)) + " GiB" : mib + " MiB" }
    function syncConsole() {
        if (!backend.connected || !displayAvailable) {
            if (consoleUuid !== "") display.disconnectConsole()
            consoleUuid = ""
            return
        }
        if (!consoleWanted || (backend.busy && !backend.checkpointJob.active) || consoleUuid === selectedUuid) return
        consoleUuid = selectedUuid
        backend.openConsole(selectedUuid)
    }
    function reopenConsole() {
        consoleWanted = true
        consoleUuid = ""
        display.disconnectConsole()
        Qt.callLater(syncConsole)
    }
    function closeConsole() {
        consoleWanted = false
        consoleUuid = ""
        display.disconnectConsole()
    }
    function primaryAction() {
        if (permitted) backend.action(selectedUuid, stopped ? "start" : paused ? "resume" : "pause")
    }
    function focusSearch() {
        consoleExpanded = false
        if (sidebarRail) setSidebarRail(false)
        searchField.forceActiveFocus()
        searchField.selectAll()
    }
    onSelectedUuidChanged: {
        display.disconnectConsole()
        consoleUuid = ""
        consoleWanted = true
        backend.inspect(selectedUuid)
        if (selectedUuid) preferences.set("selectedVm", selectedUuid)
        display.clipboardMode = "off"
        Qt.callLater(syncConsole)
    }
    onSelectedChanged: {
        if (!selected) { detailsOpen = false; consoleExpanded = false }
    }
    onDetailsOpenChanged: {
        display.releaseInput()
        if (detailsOpen && selectedUuid !== "") backend.inspect(selectedUuid)
    }
    // Live charts sample while Overview, Hardware or Networks is showing; a stopped VM needs one sample for host totals.
    Timer { interval: vmDetails.sampleInterval; running: root.visible && root.navigation === "library" && root.detailsOpen && vmDetails.page !== 3 && root.selectedUuid !== "" && (root.running || !vmDetails.hostKnown); repeat: true; triggeredOnStart: true; onTriggered: backend.request("stats", {uuid: root.selectedUuid}) }
    // Fleet sampling feeds Monitor, the sidebar sparklines and the status bar; it idles when nothing runs.
    FleetStats { id: fleet; sample: backend.management["stats.all"] || ({}) }
    Timer { interval: root.navigation === "monitor" ? 2000 : 5000; running: root.visible && backend.connected && (root.navigation === "monitor" || root.activeCount > 0); repeat: true; triggeredOnStart: true; onTriggered: backend.request("stats.all", {}) }
    Connections {
        target: backend
        function onChanged() {
            if (!root.selected && backend.domains.length > 0) root.selectedUuid = backend.domains[0].uuid
            // Once every VM paused on close has been resumed or stopped, forget the list.
            if (backend.connected && backend.domains.length > 0 && root.exitPaused.length > 0 && root.pausedOnExit.length === 0 && !root.pausingForExit) root.forgetExitPaused()
            Qt.callLater(root.syncConsole)
        }
        function onPausedForExit(uuids, failures) {
            if (!root.pausingForExit) return
            root.exitPaused = uuids
            preferences.set("exitPaused", JSON.stringify(uuids))
            if (root.updatePhase === "pausing") { if (failures.length === 0) root.restartIntoUpdate(); else root.updateFailures = failures; return }
            if (failures.length === 0) { root.exiting = true; exitDialog.close(); Qt.callLater(root.close); return }
            exitDialog.failures = failures
        }
        function onRecoveryRequested(action, uuid) { root.recoverError(action, uuid) }
        function onCreated(uuid) {
            root.searchQuery = ""
            root.stateFilter = 0
            root.selectedUuid = uuid
            root.detailsOpen = false
        }
        function onGraphics(socket) {
            if (root.consoleWanted && root.displayAvailable && backend.connected)
                display.attachForVm(socket, root.selectedUuid)
        }
        function onOperationFinished(message, ok) {
            root.operationError = ok ? "" : message
            if (root.removingUuid !== "") {
                if (ok && root.selectedUuid === root.removingUuid) root.selectedUuid = ""
                root.removingUuid = ""
            }
        }
        function onCommandFinished(operation, ok, result) {
            if (operation === "containment.set" && result.uuid === root.selectedUuid) backend.inspect(root.selectedUuid)
            if ((operation === "snapshots.restore" || operation === "snapshots.undo" || operation === "snapshots.recover") && result.restarted && result.uuid === root.selectedUuid && root.consoleWanted)
                root.reopenConsole()
        }
    }
    ActionPalette { id: actionPalette; commands: root.commands(); onChosen: function(command) { root.runCommand(command) } }
    Shortcut { sequence: "Ctrl+Shift+P"; enabled: !display.captured; onActivated: actionPalette.open() }
    Shortcut { sequence: "Ctrl+K"; enabled: !display.captured; onActivated: root.focusSearch() }
    Shortcut { sequence: ":"; enabled: !display.captured; onActivated: actionPalette.open() }
    Shortcut { sequences: ["?", "F1"]; enabled: !display.captured; onActivated: keyMap.open() }
    Shortcut { sequence: "Ctrl+`"; enabled: !display.captured; onActivated: root.logOpen = !root.logOpen }
    Shortcut { sequence: "Ctrl+1"; enabled: !display.captured; onActivated: root.navigation = "library" }
    Shortcut { sequence: "Ctrl+B"; enabled: !display.captured; onActivated: root.setSidebarRail(!root.sidebarRail) }
    Shortcut { sequence: "Ctrl+2"; enabled: !display.captured; onActivated: root.navigation = "monitor" }
    Shortcut { sequence: "Ctrl+3"; enabled: !display.captured; onActivated: root.navigation = "networks" }
    Shortcut { sequence: "Ctrl+4"; enabled: !display.captured; onActivated: root.openShop() }
    Shortcut { sequence: "Ctrl+F"; enabled: !display.captured; onActivated: root.focusSearch() }
    Shortcut { sequence: "Escape"; enabled: (root.consoleFullScreen || root.consoleExpanded) && !display.captured && !consoleToolbar.popupOpen && !vmDetails.snapshotsView.dialogOpen; onActivated: { if (root.consoleFullScreen) root.toggleConsoleFullscreen(); else root.consoleExpanded = false } }

    // Destructive operations retain the exact VM selected when the dialog opens.
    AppDialog {
        id: confirmDialog
        objectName: "confirmDialog"
        anchors.centerIn: parent
        width: Math.min(440, root.width - 48)
        modal: true
        padding: 24
        property string targetUuid: ""
        property string targetName: ""
        property bool removing: false
        property var bulkTargets: []
        function confirm(remove) { confirmTarget(remove, root.selected) }
        function confirmTarget(remove, vm) {
            targetUuid = vm.uuid
            targetName = vm.name
            removing = remove
            bulkTargets = []
            open()
        }
        function confirmBulk(uuids) {
            targetUuid = ""
            targetName = uuids.map(function(uuid) { const vm = root.vmByUuid(uuid); return vm ? root.vmName(vm) : uuid }).join(", ")
            removing = false
            bulkTargets = uuids
            open()
        }
        heading: confirmDialog.removing ? "Remove this VM?" : "Force power off?"
        headerIcon: confirmDialog.removing ? "trash" : "power"
        contentItem: ColumnLayout {
            spacing: 16
            Label { textFormat: Text.PlainText; text: confirmDialog.targetName; color: theme.colors.foreground; font.weight: Font.DemiBold; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
            Label { text: confirmDialog.removing ? "This removes the VM definition from your library. Its disks are kept on your computer." : confirmDialog.bulkTargets.length > 1 ? "Power will be cut immediately on " + confirmDialog.bulkTargets.length + " VMs. Any unsaved work inside them will be lost." : "Power will be cut immediately. Any unsaved work inside this VM will be lost."; color: theme.colors.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout {
                Layout.topMargin: 8
                Item { Layout.fillWidth: true }
                AppButton { text: "Cancel"; onClicked: confirmDialog.reject() }
                AppButton {
                    objectName: "confirmAction"
                    text: confirmDialog.removing ? "Remove definition" : "Force off"
                    tone: "danger"
                    enabled: backend.connected && !backend.busy
                    onClicked: {
                        if (confirmDialog.bulkTargets.length) backend.bulkAction(confirmDialog.bulkTargets, "force-off")
                        else {
                            if (confirmDialog.removing) root.removingUuid = confirmDialog.targetUuid
                            backend.action(confirmDialog.targetUuid, confirmDialog.removing ? "remove" : "force-off")
                        }
                        confirmDialog.accept()
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            Rectangle {
                id: sidebarPane
                objectName: "sidebar"
                visible: !root.consoleFocus
                Layout.fillHeight: true
                Layout.preferredWidth: root.sidebarRail ? 64 : root.width < 1100 ? 248 : 270
                color: theme.colors.sidebar
                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: theme.colors.border }
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: root.sidebarRail ? 10 : 16
                    spacing: root.height < 760 ? (theme.textScale > 1.2 ? 6 : 10) : 12
                    RowLayout {
                        Layout.topMargin: root.height < 760 ? 0 : 8
                        Layout.bottomMargin: root.height < 760 ? 2 : 4
                        spacing: 12
                        Rectangle {
                            width: root.sidebarRail ? 44 : 40; height: width; radius: 3
                            color: theme.colors.accent
                            Label { anchors.centerIn: parent; text: ">_"; color: theme.colors.accentText; font.family: "monospace"; font.weight: Font.Bold; font.pixelSize: Math.round(17 * theme.textScale) }
                            TapHandler { enabled: root.sidebarRail; onTapped: root.setSidebarRail(false) }
                        }
                        ColumnLayout {
                            visible: !root.sidebarRail
                            Layout.fillWidth: true
                            spacing: 1
                            Label { text: "omaware"; font.pixelSize: Math.round((21) * theme.textScale); font.weight: Font.Bold; font.letterSpacing: -0.5 }
                            Label { text: backend.uri; color: theme.colors.muted; font.pixelSize: Math.round((10) * theme.textScale); elide: Text.ElideRight; Layout.fillWidth: true }
                        }
                        AppButton { visible: !root.sidebarRail; objectName: "sidebarToggle"; iconName: "previous"; tone: "quiet"; implicitWidth: 28; implicitHeight: 28; leftPadding: 5; rightPadding: 5; hint: "Collapse sidebar · Ctrl+B"; onClicked: root.setSidebarRail(true) }
                    }
                    AppButton { visible: root.sidebarRail; objectName: "sidebarExpand"; iconName: "next"; tone: "quiet"; Layout.alignment: Qt.AlignHCenter; hint: "Expand sidebar · Ctrl+B"; onClicked: root.setSidebarRail(false) }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 4
                        AppButton { text: root.sidebarRail ? "" : "Virtual machines"; iconName: "monitor"; leading: !root.sidebarRail; tone: "quiet"; checked: root.navigation === "library"; Layout.fillWidth: true; hint: "Virtual machines · Ctrl+1"; onClicked: root.navigation = "library" }
                        AppButton { objectName: "monitorNav"; text: root.sidebarRail ? "" : "Monitor"; iconName: "cpu"; leading: !root.sidebarRail; tone: "quiet"; checked: root.navigation === "monitor"; Layout.fillWidth: true; hint: "Live view of every VM · Ctrl+2"; onClicked: root.navigation = "monitor" }
                        AppButton { objectName: "openActions"; text: root.sidebarRail ? "" : "Command"; iconName: "search"; leading: !root.sidebarRail; tone: "quiet"; Layout.fillWidth: true; hint: "Run any action · : or Ctrl+Shift+P"; onClicked: actionPalette.open() }
                        AppButton { objectName: "networksNav"; text: root.sidebarRail ? "" : "Networks"; iconName: "network"; leading: !root.sidebarRail; tone: "quiet"; checked: root.navigation === "networks"; Layout.fillWidth: true; hint: "Networks · Ctrl+3"; onClicked: root.navigation = "networks" }
                        AppButton { objectName: "isoShopNav"; text: root.sidebarRail ? "" : "ISO Shop"; iconName: "store"; leading: !root.sidebarRail; tone: "quiet"; checked: root.navigation === "isos"; Layout.fillWidth: true; hint: "Download installation ISOs · Ctrl+4"; onClicked: root.openShop() }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: theme.colors.line }
                    RowLayout {
                        visible: !root.sidebarRail
                        Label { text: "<font color=\"" + theme.colors.accent + "\">//</font> machines"; textFormat: Text.StyledText; font.pixelSize: Math.round((11) * theme.textScale); font.weight: Font.DemiBold; color: theme.colors.muted; Layout.fillWidth: true }
                        Label { text: backend.domains.length + " VMs"; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale)}
                        AppButton { objectName: "libraryDensity"; iconName: root.compactLibrary ? "grid" : "list"; tone: "quiet"; implicitHeight: 28; implicitWidth: 28; hint: root.compactLibrary ? "Show spacious rows" : "Show compact rows"; onClicked: { root.compactLibrary = !root.compactLibrary; preferences.set("compact", root.compactLibrary) } }
                    }
                    AppField {
                        id: searchField
                        objectName: "librarySearch"
                        visible: !root.sidebarRail
                        Layout.fillWidth: true
                        implicitHeight: 38
                        leftPadding: 34
                        rightPadding: 36
                        placeholderText: "grep machines, tags, folders…"
                        placeholderTextColor: theme.colors.muted
                        color: theme.colors.foreground
                        selectionColor: theme.colors.accent
                        selectedTextColor: theme.colors.accentText
                        font.pixelSize: Math.round((12) * theme.textScale)
                        text: root.searchQuery
                        onTextChanged: root.searchQuery = text
                        AppIcon { x: 11; anchors.verticalCenter: parent.verticalCenter; width: 15; height: 15; name: "search"; color: theme.colors.muted }
                        AppButton { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; width: 34; height: 34; visible: searchField.text !== ""; iconName: "close"; tone: "quiet"; hint: "Clear search"; onClicked: root.searchQuery = "" }
                        Keys.onEscapePressed: root.searchQuery = ""
                        Keys.onDownPressed: { if (root.filteredDomains.length > 0) { root.selectedUuid = root.filteredDomains[0].uuid; library.forceActiveFocus() } }
                        onAccepted: { if (root.filteredDomains.length > 0) { root.selectedUuid = root.filteredDomains[0].uuid; library.forceActiveFocus() } }
                        Accessible.name: "Search virtual machines"
                    }
                    RowLayout {
                        visible: !root.sidebarRail
                        Layout.fillWidth: true
                        spacing: 4
                        Repeater {
                            model: [{label: "All", count: backend.domains.length}, {label: "Active", count: root.activeCount}, {label: "Stopped", count: backend.domains.filter(function(row) { return row.stateCode === 5 }).length}]
                            AppButton {
                                required property int index
                                required property var modelData
                                objectName: "stateFilter" + index
                                Layout.fillWidth: true
                                implicitWidth: 0
                                implicitHeight: 30
                                leftPadding: 4; rightPadding: 4
                                text: modelData.label + " " + modelData.count
                                font.pixelSize: Math.round(11 * theme.textScale)
                                tone: "quiet"
                                checked: root.stateFilter === index
                                ink: checked ? theme.colors.foreground : theme.colors.muted
                                onClicked: root.stateFilter = index
                            }
                        }
                    }
                    // VMs paused when OmaWare last closed: resume them all, or pick some.
                    Rectangle {
                        objectName: "exitPausedBanner"
                        visible: !root.sidebarRail && root.pausedOnExit.length > 0
                        Layout.fillWidth: true
                        implicitHeight: bannerColumn.implicitHeight + 18
                        radius: 3; color: theme.colors.surface; border.color: theme.colors.warning
                        ColumnLayout {
                            id: bannerColumn
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 9
                            spacing: 7
                            RowLayout {
                                Layout.fillWidth: true; spacing: 6
                                Label { text: "‖"; color: theme.colors.warning; font.weight: Font.Bold }
                                Label {
                                    text: root.pausedOnExit.length + (root.pausedOnExit.length === 1 ? " VM was" : " VMs were") + " paused when OmaWare closed"
                                    color: theme.colors.foreground; font.pixelSize: Math.round(11 * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true
                                }
                                AppButton { iconName: "close"; tone: "quiet"; implicitWidth: 22; implicitHeight: 22; leftPadding: 3; rightPadding: 3; topPadding: 2; bottomPadding: 2; hint: "Dismiss · they stay paused"; onClicked: root.forgetExitPaused() }
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 6
                                AppButton { objectName: "resumeAllPaused"; text: "Resume all"; iconName: "play"; tone: "primary"; implicitHeight: 28; Layout.fillWidth: true; enabled: backend.connected && !backend.busy; onClicked: backend.bulkAction(root.pausedOnExit, "power-on") }
                                AppButton { objectName: "choosePaused"; text: "Choose…"; implicitHeight: 28; Layout.fillWidth: true; hint: "Select them in the list · Ctrl+click to leave some out"; onClicked: { root.marked = root.pausedOnExit.slice(); library.forceActiveFocus() } }
                            }
                        }
                    }
                    ListView {
                        id: library
                        objectName: "vmLibrary"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 0
                        model: root.filteredDomains
                        currentIndex: root.filteredDomains.findIndex(function(row) { return row.uuid === root.selectedUuid })
                        keyNavigationEnabled: false
                        function step(delta) {
                            // Skip rows hidden inside collapsed groups.
                            for (let i = currentIndex + delta; i >= 0 && i < count; i += delta)
                                if (root.sidebarRail || !root.grouped || !root.collapsedGroups[root.groupOf(root.filteredDomains[i])]) { root.selectedUuid = root.filteredDomains[i].uuid; positionViewAtIndex(i, ListView.Contain); return }
                        }
                        Keys.onDownPressed: step(1)
                        Keys.onUpPressed: step(-1)
                        // Vim-style navigation and row actions while the list has focus.
                        Keys.onPressed: function(event) {
                            const vm = root.selected
                            if ((event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) && vm) {
                                const item = itemAtIndex(currentIndex)
                                root.openVmMenu(vm, item, 24, item ? item.height - 8 : 0); event.accepted = true; return
                            }
                            if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier) && !root.sidebarRail) { root.marked = root.visibleUuids(); event.accepted = true; return }
                            if (event.key === Qt.Key_Escape && root.marked.length) { root.marked = []; event.accepted = true; return }
                            if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) return
                            if (event.key === Qt.Key_J) { step(1); event.accepted = true }
                            else if (event.key === Qt.Key_K) { step(-1); event.accepted = true }
                            else if (!vm) return
                            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { root.navigation = "library"; root.detailsOpen = false; event.accepted = true }
                            else if (event.key === Qt.Key_D) { root.navigation = "library"; if (vmDetails.page === 3) vmDetails.page = 0; root.detailsOpen = true; event.accepted = true }
                            else if (event.key === Qt.Key_S) { root.navigation = "library"; vmDetails.page = 3; root.detailsOpen = true; event.accepted = true }
                            else if (event.key === Qt.Key_F) { root.toggleFavorite(vm); event.accepted = true }
                            else if (event.key === Qt.Key_F2) { organizeDialog.openFor(vm); event.accepted = true }
                            else if (event.key === Qt.Key_Delete && vm.owned && vm.stateCode === 5 && backend.connected && !backend.busy) { root.confirmFor(true, vm); event.accepted = true }
                        }
                        ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                        delegate: VmRow {
                            shell: root; stats: fleet
                            rail: root.sidebarRail; compact: root.compactLibrary
                            listFocused: library.activeFocus
                        }
                        ColumnLayout {
                            anchors.centerIn: parent
                            width: parent.width - 16
                            visible: library.count === 0 && !root.sidebarRail
                            spacing: 10
                            AppIcon { name: "search"; color: theme.colors.muted; Layout.alignment: Qt.AlignHCenter; width: 24; height: 24 }
                            Label { text: !backend.connected ? "Waiting for connection" : backend.domains.length === 0 ? "Your library starts here" : "No matching VMs"; font.weight: Font.Medium; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                            Label { text: !backend.connected ? "Your library will appear here." : backend.domains.length === 0 ? "Create a VM to get started." : "Try another name, tag or filter."; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                            AppButton { visible: backend.domains.length > 0; text: "Reset filters"; tone: "quiet"; Layout.alignment: Qt.AlignHCenter; onClicked: { root.searchQuery = ""; root.stateFilter = 0 } }
                        }
                    }
                    // A new OmaWare version: the update dialog explains what happens to running VMs before updating.
                    Rectangle {
                        objectName: "updateBanner"
                        visible: !root.sidebarRail && !updater.development && ["available", "downloading", "ready"].indexOf(updater.status) >= 0
                        Layout.fillWidth: true; implicitHeight: updateColumn.implicitHeight + 16; radius: 3
                        color: theme.colors.surface; border.color: theme.colors.accent
                        ColumnLayout {
                            id: updateColumn; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8; spacing: 6
                            Label {
                                text: updater.status === "ready" ? "OmaWare " + updater.latest + " is ready" : updater.status === "downloading" ? "Downloading OmaWare " + updater.latest + " · " + Math.round(updater.progress * 100) + "%" : "OmaWare " + updater.latest + " is available"
                                font.pixelSize: Math.round(11 * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap
                            }
                            AppProgressBar { visible: updater.status === "downloading"; Layout.fillWidth: true; from: 0; to: 1; value: updater.progress }
                            RowLayout {
                                visible: updater.status !== "downloading"
                                spacing: 6
                                AppButton {
                                    objectName: "updateBannerButton"
                                    text: "Update…"; tone: "primary"; implicitHeight: 28; Layout.fillWidth: true
                                    hint: "See what's new and update"
                                    onClicked: root.openUpdates(false)
                                }
                                AppButton { text: "What's new"; tone: "quiet"; implicitHeight: 28; onClicked: Qt.openUrlExternally(updater.page) }
                            }
                        }
                    }
                    // ISO downloads keep running in the background; show progress here too.
                    Rectangle {
                        id: isoBar
                        objectName: "isoDownloadBar"
                        visible: !root.sidebarRail && isoLibrary.downloading
                        readonly property var active: isoLibrary.sources.filter(function(s) { return s.status === "downloading" })
                        Layout.fillWidth: true; implicitHeight: isoBarColumn.implicitHeight + 16; radius: 3
                        color: theme.colors.surface; border.color: theme.colors.border
                        ColumnLayout {
                            id: isoBarColumn; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8; spacing: 5
                            Label {
                                text: isoBar.active.length ? "Downloading " + isoBar.active[0].name + (isoBar.active[0].total ? " · " + Math.round(100 * isoBar.active[0].received / isoBar.active[0].total) + "%" : "") : ""
                                font.pixelSize: Math.round(11 * theme.textScale); elide: Text.ElideRight; Layout.fillWidth: true
                            }
                            AppProgressBar { Layout.fillWidth: true; from: 0; to: isoBar.active.length ? Math.max(1, isoBar.active[0].total || 1) : 1; value: isoBar.active.length ? isoBar.active[0].received || 0 : 0; indeterminate: !isoBar.active.length || !isoBar.active[0].total }
                            TapHandler { onTapped: root.openShop() }
                        }
                    }
                    // Actions for every VM picked with Ctrl/Shift-click.
                    Rectangle {
                        id: bulkBar
                        objectName: "bulkBar"
                        visible: !root.sidebarRail && root.marked.length > 0
                        readonly property var onList: root.markedFor("power-on")
                        readonly property var pauseList: root.markedFor("pause")
                        readonly property var shutdownList: root.markedFor("shutdown")
                        readonly property var offList: root.markedFor("force-off")
                        readonly property bool ready: backend.connected && !backend.busy
                        Layout.fillWidth: true
                        implicitHeight: bulkColumn.implicitHeight + 18
                        radius: 3; color: theme.colors.accentSoft; border.color: theme.colors.accent
                        ColumnLayout {
                            id: bulkColumn
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 9
                            spacing: 7
                            RowLayout {
                                Layout.fillWidth: true; spacing: 6
                                Label { objectName: "bulkCount"; text: root.marked.length + " selected"; color: theme.colors.foreground; font.weight: Font.DemiBold; font.pixelSize: Math.round(12 * theme.textScale); Layout.fillWidth: true }
                                AppButton { objectName: "bulkClear"; iconName: "close"; tone: "quiet"; implicitWidth: 22; implicitHeight: 22; leftPadding: 3; rightPadding: 3; topPadding: 2; bottomPadding: 2; hint: "Clear selection · Esc"; onClicked: root.marked = [] }
                            }
                            GridLayout {
                                Layout.fillWidth: true; columns: 2; columnSpacing: 6; rowSpacing: 6
                                AppButton { objectName: "bulkPowerOn"; text: "Turn on" + (bulkBar.onList.length ? " " + bulkBar.onList.length : ""); iconName: "play"; tone: "primary"; implicitHeight: 28; Layout.fillWidth: true; hint: "Start stopped VMs and resume paused ones"; enabled: bulkBar.ready && bulkBar.onList.length > 0; onClicked: root.runBulk("power-on", bulkBar.onList) }
                                AppButton { objectName: "bulkPause"; text: "Pause" + (bulkBar.pauseList.length ? " " + bulkBar.pauseList.length : ""); iconName: "pause"; implicitHeight: 28; Layout.fillWidth: true; enabled: bulkBar.ready && bulkBar.pauseList.length > 0; onClicked: root.runBulk("pause", bulkBar.pauseList) }
                                AppButton { objectName: "bulkShutdown"; text: "Shut down" + (bulkBar.shutdownList.length ? " " + bulkBar.shutdownList.length : ""); iconName: "power"; implicitHeight: 28; Layout.fillWidth: true; hint: "Ask each guest OS to shut down"; enabled: bulkBar.ready && bulkBar.shutdownList.length > 0; onClicked: root.runBulk("shutdown", bulkBar.shutdownList) }
                                AppButton { objectName: "bulkForceOff"; text: "Force off" + (bulkBar.offList.length ? " " + bulkBar.offList.length : ""); iconName: "power"; tone: "danger"; implicitHeight: 28; Layout.fillWidth: true; enabled: bulkBar.ready && bulkBar.offList.length > 0; onClicked: root.runBulk("force-off", bulkBar.offList) }
                                AppButton { objectName: "bulkNetwork"; text: "New network…"; iconName: "network"; implicitHeight: 28; Layout.fillWidth: true; Layout.columnSpan: 2; hint: "Create a network and connect the selected VMs to it"; enabled: bulkBar.ready && root.marked.some(function(uuid) { const vm = root.vmByUuid(uuid); return vm && vm.owned }); onClicked: { root.navigation = "networks"; networksPage.createFor(root.marked.filter(function(uuid) { const vm = root.vmByUuid(uuid); return vm && vm.owned })) } }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        AppButton { objectName: "createVm"; text: root.sidebarRail ? "" : "Create VM"; iconName: "plus"; tone: "primary"; Layout.fillWidth: true; hint: root.sidebarRail ? "Create VM" : ""; enabled: backend.connected && !backend.busy; onClicked: createDialog.begin() }
                        AppButton { id: creationTools; visible: !root.sidebarRail; iconName: "chevron"; hint: "More creation options"; enabled: backend.connected && !backend.busy; onClicked: creationMenu.openBelow(creationTools)
                            AppMenu { id: creationMenu
                                Action { text: "Open the ISO Shop"; onTriggered: root.openShop() }
                                Action { text: "Create diskless test VM"; onTriggered: backend.createTest() }
                            }
                        }
                    }
                    Rectangle { visible: !root.sidebarRail; Layout.fillWidth: true; height: 1; color: theme.colors.border }
                    RowLayout {
                        visible: !root.sidebarRail
                        Layout.fillWidth: true; spacing: 2
                        AppButton { objectName: "openSettings"; text: "Settings"; tone: "quiet"; leftPadding: 0; rightPadding: 2; implicitWidth: 74; font.pixelSize: Math.round((11) * theme.textScale); hint: theme.status; onClicked: appearanceInfo.open() }
                        Item { Layout.fillWidth: true }
                        AppButton {
                            objectName: "checkUpdates"
                            implicitWidth: 32; implicitHeight: 34; leftPadding: 7; rightPadding: 7
                            iconName: "download"; tone: "quiet"
                            readonly property bool waiting: ["available", "downloading", "ready"].indexOf(updater.status) >= 0
                            ink: waiting ? theme.colors.accent : theme.colors.muted
                            hint: waiting ? "OmaWare " + updater.latest + " is available" : "Check GitHub for a new version"
                            onClicked: root.openUpdates(true)
                            Rectangle { visible: parent.waiting; width: 7; height: 7; radius: 4; color: theme.colors.accent; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 5 }
                        }
                        Repeater {
                            model: [{label: "Follow Omarchy", mode: "omarchy", icon: "monitor"}, {label: "Dark appearance", mode: "dark", icon: "moon"}, {label: "Light appearance", mode: "light", icon: "sun"}, {label: "Hacker appearance · green on black", mode: "hacker", icon: "terminal"}].filter(function(m) { return m.mode !== "omarchy" || theme.omarchyAvailable })
                            AppButton {
                                required property var modelData
                                implicitWidth: 32; implicitHeight: 34; leftPadding: 7; rightPadding: 7
                                iconName: modelData.icon; tone: "quiet"; checked: theme.mode === modelData.mode
                                ink: checked ? theme.colors.accent : theme.colors.muted
                                hint: modelData.label
                                onClicked: { theme.setMode(modelData.mode); preferences.set("theme", modelData.mode) }
                            }
                        }
                    }
                    RowLayout {
                        Layout.topMargin: 2
                        Layout.alignment: root.sidebarRail ? Qt.AlignHCenter : Qt.AlignLeft
                        spacing: 8
                        Rectangle { width: 6; height: 6; radius: 3; color: backend.connected ? theme.colors.success : theme.colors.warning }
                        ColumnLayout {
                            visible: !root.sidebarRail
                            Layout.fillWidth: true
                            spacing: 3
                            Label { text: backend.connected ? "Local session" : "Disconnected"; font.weight: Font.Medium; font.pixelSize: Math.round((12) * theme.textScale)}
                            Label { text: backend.connected ? root.activeCount + " active · " + backend.domains.length + " total" : "Reconnect to load your VMs"; color: theme.colors.muted; font.pixelSize: Math.round((10) * theme.textScale)}
                        }
                        AppButton { objectName: "reconnectBackend"; visible: !root.sidebarRail; iconName: "refresh"; tone: "quiet"; hint: "Reconnect to the local session"; enabled: !backend.busy; onClicked: { root.reopenConsole(); backend.reconnect() } }
                    }
                }
            }
            ColumnLayout {
                visible: root.navigation === "library"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: root.mainConsoleFullscreen ? 0 : root.consoleExpanded ? 10 : !root.detailsOpen ? 16 : root.width < 1100 ? 20 : 28
                spacing: !root.detailsOpen ? 10 : root.height < 760 ? 16 : 22
                RowLayout {
                    visible: !root.consoleFocus
                    Layout.fillWidth: true
                    spacing: 14
                    objectName: "workspaceHeader"
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Row {
                            visible: root.detailsOpen || !root.selected
                            spacing: 0
                            Label { text: "omaware"; color: theme.colors.success; font.pixelSize: Math.round((11) * theme.textScale) }
                            Label { text: ":~/vms" + (root.selected ? "/" + root.selected.name : "") + (root.detailsOpen ? (vmDetails.page === 3 ? "/snapshots" : "/details") : ""); color: theme.colors.accent; font.pixelSize: Math.round((11) * theme.textScale) }
                            Label { text: "$ "; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale) }
                            Rectangle {
                                id: promptCursor
                                width: 7; height: 13; anchors.verticalCenter: parent.verticalCenter; color: theme.colors.foreground
                                SequentialAnimation on opacity { running: promptCursor.visible && !theme.reducedMotion; loops: Animation.Infinite
                                    PropertyAction { value: 1 } PauseAnimation { duration: 530 } PropertyAction { value: 0 } PauseAnimation { duration: 530 } }
                            }
                        }
                        Label { textFormat: Text.PlainText; text: root.selected ? root.vmLabel(root.selected) : "Virtual machines"; font.pixelSize: Math.round((!root.detailsOpen && root.selected ? 20 : root.width < 1100 ? 25 : 31) * theme.textScale); font.weight: Font.DemiBold; font.letterSpacing: -0.6; elide: Text.ElideRight; Layout.fillWidth: true }
                        RowLayout {
                            spacing: 10
                            StatusBadge { visible: root.selected !== null; text: root.selected ? root.selected.state : ""; stateCode: root.selected ? root.selected.stateCode : 5 }
                            StatusBadge { objectName: "containedBadge"; visible: root.selected !== null && !!root.selected.contained; text: "Contained"; stateCode: 6 }
                            Label { visible: !root.selected || !root.selected.owned || root.selected.diskless; text: !root.selected ? "Create a VM to get started." : !root.selected.owned ? "Read-only virtual machine" : root.selected.diskless ? "Test virtual machine" : "Managed virtual machine"; color: theme.colors.muted; font.pixelSize: Math.round((12) * theme.textScale); Layout.fillWidth: true; elide: Text.ElideRight }
                        }
                    }
                    AppButton {
                        objectName: "primaryAction"
                        visible: root.selected !== null && root.selected.owned && (!root.stopped || root.detailsOpen || root.consoleDetached)
                        text: root.stopped ? "Start VM" : root.paused ? "Resume" : "Pause"
                        iconName: root.stopped || root.paused ? "play" : "pause"
                        tone: "primary"
                        enabled: root.permitted && (root.stopped || root.running || root.paused)
                        onClicked: root.primaryAction()
                    }
                    AppButton {
                        id: moreButton
                        visible: root.selected !== null
                        iconName: "more"
                        hint: "More VM actions"
                        onClicked: powerMenu.openBelow(moreButton)
                        AppMenu {
                            id: powerMenu; objectName: "powerMenu"
                            y: moreButton.height + 6
                            x: moreButton.width - width
                            width: 225
                            padding: 6
                            Action { text: "Shut down guest"; enabled: root.permitted && root.running; onTriggered: backend.action(root.selectedUuid, "shutdown") }
                            Action { text: "Edit hardware…"; enabled: root.permitted && !backend.detailsBusy; onTriggered: hardwareDialog.openFor(backend.details) }
                            Action { text: "Organize VM…"; enabled: root.selected !== null; onTriggered: organizeDialog.openFor(root.selected) }
                            Action { text: "Force power off…"; enabled: root.permitted && !root.stopped; onTriggered: confirmDialog.confirm(false) }
                            MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: theme.colors.border } }
                            Action { text: "Remove definition…"; enabled: root.permitted && root.stopped; onTriggered: confirmDialog.confirm(true) }
                        }
                    }
                }
                Rectangle {
                    visible: root.operationError !== "" || !backend.connected
                    Layout.fillWidth: true
                    implicitHeight: errorRow.implicitHeight + 24
                    radius: 4
                    color: theme.colors.surface
                    border.color: theme.colors.warning
                    ColumnLayout {
                        id: errorRow
                        anchors.fill: parent; anchors.margins: 12; spacing: 8
                        ErrorNotice { message: root.operationError; copyHelper: preferences; onRecover: function(action) { root.recoverError(action, "") } }
                        RowLayout { visible: !backend.connected; Layout.fillWidth: true
                            Label { text: backend.busy ? "Connecting to your local VM session…" : "The local session is disconnected."; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                            AppButton { text: "Reconnect"; enabled: !backend.busy; onClicked: backend.reconnect() }
                        }
                        AppButton { visible: root.operationError !== ""; text: "Dismiss"; tone: "quiet"; onClicked: root.operationError = "" }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: theme.colors.surface
                    border.color: root.mainConsoleFullscreen ? "transparent" : theme.colors.border
                    radius: 4
                    CornerBrackets { visible: !root.mainConsoleFullscreen && !root.consoleFocus; size: 14 }
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: root.mainConsoleFullscreen ? 0 : 1
                        spacing: 0
                        Item {
                            Layout.fillWidth: true
                            id: workspaceTabs
                            visible: !root.mainConsoleFullscreen
                            implicitHeight: 50
                            Row {
                                visible: !root.consoleFocus
                                anchors.left: parent.left; anchors.leftMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6
                                AppButton { objectName: "consoleTab"; text: root.width < 1050 && theme.textScale > 1.1 ? "" : "Console"; hint: "Console"; iconName: "monitor"; tone: "tab"; checked: !root.detailsOpen; onClicked: root.detailsOpen = false }
                                AppButton { visible: root.selected !== null; objectName: "detailsTab"; text: root.width < 1050 && theme.textScale > 1.1 ? "" : "Details"; hint: "Details"; iconName: "settings"; tone: "tab"; checked: root.detailsOpen && vmDetails.page !== 3; onClicked: { if (vmDetails.page === 3) vmDetails.page = 0; root.detailsOpen = true } }
                                AppButton { visible: root.selected !== null; objectName: "snapshotsTab"; text: root.width < 1050 && theme.textScale > 1.1 ? "" : "Snapshots"; hint: "Snapshots"; iconName: "snapshot"; tone: "tab"; checked: root.detailsOpen && vmDetails.page === 3; onClicked: { vmDetails.page = 3; root.detailsOpen = true } }
                            }
                            Item {
                                visible: root.consoleExpanded
                                anchors.left: parent.left; anchors.leftMargin: 18
                                anchors.right: embeddedTools.left; anchors.rightMargin: 16
                                height: parent.height
                                AppIcon { id: focusIcon; objectName: "focusIcon"; name: "monitor"; color: theme.colors.muted; anchors.verticalCenter: parent.verticalCenter }
                                Label { objectName: "focusTitle"; textFormat: Text.PlainText; anchors.left: focusIcon.right; anchors.leftMargin: 10; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: root.selected ? root.selected.name : "Console"; font.weight: Font.DemiBold; elide: Text.ElideRight }
                            }
                            Item {
                                id: embeddedTools
                                visible: !root.detailsOpen && !root.consoleDetached
                                anchors.right: parent.right; anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: consoleToolbar.implicitWidth; height: consoleToolbar.implicitHeight
                            }
                        }
                        Rectangle { visible: !root.mainConsoleFullscreen; Layout.fillWidth: true; height: 1; color: theme.colors.border }
                        Item {
                            id: consoleStage
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: !root.detailsOpen
                            VmConsole { id: display; objectName: "console"; parent: root.consoleDetached ? detachedStage : consoleStage; anchors.fill: parent; visible: hasFrame
                                Rectangle { objectName: "consoleCaptureBorder"; anchors.fill: parent; color: "transparent"; border.width: 2; border.color: theme.colors.accent; visible: display.captured; z: 2 }
                            }
                            AppButton { anchors.centerIn: parent; visible: root.consoleDetached; text: "Bring console back"; onClicked: root.consoleDetached = false }
                            Flickable {
                                id: emptyView
                                anchors.fill: parent
                                visible: !display.hasFrame && !root.consoleDetached
                                clip: true
                                contentWidth: width
                                contentHeight: Math.max(height, emptyContent.implicitHeight + 32)
                                boundsBehavior: Flickable.StopAtBounds
                                interactive: contentHeight > height
                                ScrollBar.vertical: AppScrollBar {}
                                ColumnLayout {
                                id: emptyContent
                                x: (emptyView.width - width) / 2
                                y: Math.max(16, (emptyView.height - implicitHeight) / 2)
                                width: Math.min(410, emptyView.width - 48)
                                spacing: emptyView.height < 240 ? 8 : 14
                                Rectangle {
                                    visible: emptyView.height >= 240
                                    Layout.alignment: Qt.AlignHCenter
                                    width: 62; height: 62; radius: 4
                                    color: theme.colors.accentSoft
                                    AppIcon { anchors.centerIn: parent; width: 30; height: 30; name: root.stopped ? "power" : "monitor"; color: theme.colors.accent }
                                }
                                Label {
                                    text: !backend.connected ? "Let's get connected" : !root.selected ? "A workspace of your own" : !root.selected.owned ? "This VM is read only" : root.stopped ? "Ready when you are" : !root.consoleWanted ? "Console closed" : backend.busy ? "Opening your console…" : "Console unavailable"
                                    font.pixelSize: Math.round((emptyView.height < 240 ? 20 : 23) * theme.textScale); font.weight: Font.DemiBold; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true; wrapMode: Text.WordWrap
                                }
                                Label {
                                    text: !backend.connected ? "Connect to your local session to see your virtual machines." : !root.selected ? "Create a VM from an ISO or an existing disk image. Your virtual machines will appear in the library." : !root.selected.owned ? "Power controls and console input are available for OmaWare-managed VMs." : root.stopped ? "Start this virtual machine to open its display here." : !root.consoleWanted ? (root.paused ? "Your VM is still paused. Reopen its display whenever you need it." : "Your VM is still running. Reopen its display whenever you need it.") : backend.busy ? "Connecting to the virtual machine's display." : display.status
                                    color: theme.colors.muted; font.pixelSize: Math.round((13) * theme.textScale); horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; Layout.fillWidth: true
                                }
                                AppButton {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.topMargin: 6
                                    visible: backend.connected && (!root.selected || root.stopped || root.displayAvailable) && (!root.selected || root.selected.owned)
                                    text: !root.selected ? "Create VM" : root.stopped ? "Start VM" : "Open console"
                                    iconName: !root.selected ? "plus" : "play"
                                    tone: "primary"
                                    enabled: !backend.busy
                                    onClicked: { if (!root.selected) createDialog.begin(); else if (root.stopped) root.primaryAction(); else root.reopenConsole() }
                                }
                                }
                            }
                        }
                        VmDetails {
                            id: vmDetails
                            objectName: "vmDetails"
                            visible: root.detailsOpen
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            vm: root.selected
                            info: backend.details.uuid === root.selectedUuid ? backend.details : ({})
                            busy: backend.detailsBusy
                            canEdit: root.permitted
                            checkpointPreview: function() { return root.consoleUuid === root.selectedUuid && display.connected ? display.checkpointPreview() : "" }
                            stats: (backend.management.stats || {}).uuid === root.selectedUuid ? backend.management.stats : ({})
                            onReload: function(guestInfo) { backend.inspect(root.selectedUuid, guestInfo) }
                            onEditAdapter: function(adapter, remove) { networkDialog.openFor(info, adapter, remove) }
                            onEditHardware: hardwareDialog.openFor(info)
                            onReviewPending: pendingDialog.openFor(info)
                            onManageDisk: function(disk, remove) { diskDialog.openFor(info, disk, remove) }
                            onSetContainment: function(enabled) { backend.request("containment.set", {uuid: root.selectedUuid, enabled: enabled}) }
                        }

                    }
                }
                RowLayout {
                    visible: !root.consoleFocus && root.selected !== null && root.selected.diskless && !root.detailsOpen
                    Layout.fillWidth: true
                    spacing: 8
                    AppIcon { name: "info"; width: 14; height: 14; color: theme.colors.muted }
                    Label { text: root.selected && root.selected.diskless ? "This test VM has no OS. A “no bootable device” message is expected." : "Closing OmaWare keeps your virtual machines running."; font.pixelSize: Math.round((11) * theme.textScale); color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                }
            }
            MonitorPage {
                id: monitorPage; objectName: "monitorPage"
                visible: root.navigation === "monitor"
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 26
                fleet: fleet; domains: backend.domains; selectedUuid: root.selectedUuid; sampleInterval: 2000
                labelFor: root.vmLabel
                onSelect: function(uuid) { root.selectedUuid = uuid }
                onOpenConsole: function(uuid) { root.selectedUuid = uuid; root.navigation = "library"; root.detailsOpen = false }
                onOpenDetails: function(uuid) { root.selectedUuid = uuid; root.navigation = "library"; if (vmDetails.page === 3) vmDetails.page = 0; root.detailsOpen = true }
                onMenuRequested: function(uuid) { root.openVmMenu(root.vmByUuid(uuid)) }
            }
            NetworksPage {
                id: networksPage; objectName: "networksPage"; visible: root.navigation === "networks"; Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 26
                fleet: fleet
                // Double-clicking a VM on the lab map enters it: its console, like double-clicking it in the library.
                onOpenVm: function(uuid) { root.selectVm(uuid); root.detailsOpen = false }
                onVmActions: function(vm, item, px, py) { if (vm && vm.uuid) root.openVmMenu(vm, item, px, py) }
            }
            IsoShopPage {
                id: isoShop
                visible: root.navigation === "isos"
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 26
                library: isoLibrary
                onUseIso: function(path) { createDialog.beginWith(path) }
                onLanguageChosen: function(id, language) { preferences.set("isoLanguage:" + id, language) }
            }
        }
        Rectangle { visible: !root.mainConsoleFullscreen; Layout.fillWidth: true; height: 1; color: theme.colors.border }
        // tmux-style status line.
        Rectangle {
            id: statusBar
            visible: !root.mainConsoleFullscreen
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            color: theme.colors.sidebar
            RowLayout {
                anchors.fill: parent
                anchors.rightMargin: 10
                spacing: 0
                Rectangle {
                    Layout.fillHeight: true; implicitWidth: modeLabel.implicitWidth + 24
                    color: root.operationError !== "" ? theme.colors.warning : backend.connected ? theme.colors.accent : theme.colors.danger
                    Label { id: modeLabel; anchors.centerIn: parent; text: root.navigation === "monitor" ? "MONITOR" : root.navigation === "networks" ? "NETWORKS" : root.navigation === "isos" ? "ISO SHOP" : root.consoleFocus ? "CONSOLE" : root.detailsOpen ? "DETAILS" : "NORMAL"
                        color: theme.colors.accentText; font.weight: Font.Bold; font.letterSpacing: 1; font.pixelSize: Math.round((10) * theme.textScale) }
                }
                Item { implicitWidth: 12 }
                AppBusyIndicator { running: backend.busy; visible: running; Layout.preferredWidth: 16; Layout.preferredHeight: 16; Layout.rightMargin: 8 }
                Label { textFormat: Text.PlainText; text: backend.busy ? backend.message : root.operationError ? "needs attention · open the log (Ctrl+`)" : backend.connected ? "ready" : "disconnected"
                    color: root.operationError !== "" ? theme.colors.warning : theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); Layout.fillWidth: true; elide: Text.ElideRight }
                AppButton { text: "Cancel automatic start"; visible: backend.busy && backend.message.indexOf("Waiting for the guest") === 0; tone: "quiet"; implicitHeight: 28; onClicked: backend.cancelRestart() }
                Row {
                    visible: fleet.ready && root.width >= 1100
                    spacing: 6; Layout.rightMargin: 12
                    Label { text: "cpu"; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); anchors.verticalCenter: parent.verticalCenter }
                    Label { text: fleet.host.cpu === null || fleet.host.cpu === undefined ? "--" : ("  " + Math.round(fleet.host.cpu)).slice(-3) + "%"; color: theme.colors.foreground; font.pixelSize: Math.round((11) * theme.textScale); anchors.verticalCenter: parent.verticalCenter }
                    Spark { width: 36; height: 12; anchors.verticalCenter: parent.verticalCenter; values: fleet.hostCpu; capacity: fleet.capacity }
                    Label { text: "  mem"; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); anchors.verticalCenter: parent.verticalCenter }
                    Label { text: fleet.host.memUsedMiB && fleet.host.memTotalMiB ? Math.round(fleet.host.memUsedMiB / fleet.host.memTotalMiB * 100) + "%" : "--"; color: theme.colors.foreground; font.pixelSize: Math.round((11) * theme.textScale); anchors.verticalCenter: parent.verticalCenter }
                }
                Label { text: "vm " + root.activeCount + "/" + backend.domains.length; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); Layout.rightMargin: 8 }
                AppButton { objectName: "showJobs"; text: backend.checkpointJob.active ? "jobs●" : "jobs"; tone: "quiet"; implicitHeight: 28; checked: !!backend.checkpointJob.active; onClicked: jobsPanel.open() }
                AppButton { objectName: "toggleLog"; text: "log"; tone: "quiet"; implicitHeight: 28; checked: root.logOpen; hint: "Operation log · Ctrl+`"; onClicked: root.logOpen = !root.logOpen }
                AppButton { text: "?"; tone: "quiet"; implicitHeight: 28; implicitWidth: 30; hint: "Keyboard map · ?"; onClicked: keyMap.open() }
                Label { text: root.clock; color: theme.colors.accent; font.weight: Font.DemiBold; font.pixelSize: Math.round((11) * theme.textScale); Layout.leftMargin: 6 }
            }
        }
    }
    // Quake-style log console: slides up over the workspace, leaving the sidebar in place.
    Item {
        id: logDock
        x: sidebarPane.visible ? sidebarPane.width : 0
        width: root.width - x
        height: statusBar.visible ? statusBar.y - 1 : root.height
        clip: true; z: 20
        visible: (root.logOpen || logSlide.running) && !root.mainConsoleFullscreen
        LogDrawer {
            id: logDrawer; objectName: "logDrawer"
            width: parent.width
            height: Math.min(260, root.height * .38)
            y: parent.height - (root.logOpen ? height : 0)
            Behavior on y { enabled: !theme.reducedMotion; NumberAnimation { id: logSlide; duration: 160; easing.type: Easing.OutCubic } }
            entries: backend.activity; copyHelper: preferences
            onOpenHistory: activityDialog.open()
            onDismiss: root.logOpen = false
        }
    }
    // Right-click menu for a VM in the library or monitor. Power and organize actions target that VM
    // directly; workspace actions (console, details, snapshots) select it first.
    AppMenu {
        id: vmMenu
        objectName: "vmContextMenu"
        property var vm: ({})
        readonly property bool owned: !!vm.owned
        readonly property bool ready: backend.connected && !backend.busy && owned
        readonly property int code: vm.stateCode || 0
        readonly property var meta: vm.uuid ? root.vmMeta(vm.uuid) : ({})
        implicitWidth: 262 * theme.textScale
        MenuItem { enabled: false; contentItem: Label { textFormat: Text.PlainText; text: vmMenu.vm.uuid ? root.vmLabel(vmMenu.vm) + "  ·  " + String(vmMenu.vm.state || "").toLowerCase() : ""; color: theme.colors.muted; elide: Text.ElideRight; font.pixelSize: Math.round(11 * theme.textScale) } background: Item {} }
        AppMenuItem { objectName: "vmMenuConsole"; text: "Open console"; onTriggered: { root.selectVm(vmMenu.vm.uuid); root.detailsOpen = false } }
        AppMenuItem { objectName: "vmMenuDetails"; text: "Details"; onTriggered: { root.selectVm(vmMenu.vm.uuid); if (vmDetails.page === 3) vmDetails.page = 0; root.detailsOpen = true } }
        AppMenuItem { objectName: "vmMenuSnapshots"; text: "Snapshots"; onTriggered: { root.selectVm(vmMenu.vm.uuid); vmDetails.page = 3; root.detailsOpen = true } }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: theme.colors.border } }
        AppMenuItem { objectName: "vmMenuPower"; text: vmMenu.code === 5 ? "Start" : vmMenu.code === 3 ? "Resume" : "Pause"; enabled: vmMenu.ready && (vmMenu.code === 1 || vmMenu.code === 3 || vmMenu.code === 5)
            onTriggered: backend.action(vmMenu.vm.uuid, vmMenu.code === 5 ? "start" : vmMenu.code === 3 ? "resume" : "pause") }
        AppMenuItem { objectName: "vmMenuShutdown"; text: "Shut down guest"; enabled: vmMenu.ready && vmMenu.code === 1; onTriggered: backend.action(vmMenu.vm.uuid, "shutdown") }
        AppMenuItem { objectName: "vmMenuForceOff"; text: "Force power off…"; enabled: vmMenu.ready && vmMenu.code !== 5; onTriggered: root.confirmFor(false, vmMenu.vm) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: theme.colors.border } }
        AppMenuItem { objectName: "vmMenuSnapshot"; text: "Take snapshot…"; enabled: vmMenu.ready && !vmMenu.vm.diskless; onTriggered: root.queueVmAction(vmMenu.vm.uuid, "snapshot") }
        AppMenuItem { objectName: "vmMenuRevert"; text: "Revert to current snapshot…"; enabled: vmMenu.ready && !vmMenu.vm.diskless; onTriggered: root.queueVmAction(vmMenu.vm.uuid, "revert") }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: theme.colors.border } }
        AppMenuItem { objectName: "vmMenuFavorite"; text: vmMenu.meta.favorite ? "Remove from favorites" : "Add to favorites"; onTriggered: root.toggleFavorite(vmMenu.vm) }
        AppMenuItem { objectName: "vmMenuOrganize"; text: "Rename & organize…"; onTriggered: organizeDialog.openFor(vmMenu.vm) }
        AppMenuItem { objectName: "vmMenuNetworkMap"; text: "Show on network map"; onTriggered: { root.navigation = "networks"; networksPage.showVm(vmMenu.vm.uuid) } }
        AppMenuItem { objectName: "vmMenuContain"; text: vmMenu.vm.contained ? "Review containment…" : "Contain VM…"; enabled: vmMenu.owned
            onTriggered: { root.selectVm(vmMenu.vm.uuid); vmDetails.page = 2; root.detailsOpen = true } }
        AppMenuItem { objectName: "vmMenuCopyName"; text: "Copy name"; onTriggered: preferences.copy(vmMenu.vm.name) }
        AppMenuItem { objectName: "vmMenuCopyUuid"; text: "Copy UUID"; onTriggered: preferences.copy(vmMenu.vm.uuid) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: theme.colors.border } }
        AppMenuItem { objectName: "vmMenuRemove"; text: "Remove definition…"; enabled: vmMenu.ready && vmMenu.code === 5; onTriggered: root.confirmFor(true, vmMenu.vm) }
    }
    CheckpointJobs { id: jobsPanel }
    NetworkDialog { id: networkDialog }
    IsoLibrary {
        id: isoLibrary; objectName: "isoLibrary"
        Component.onCompleted: { const language = String(preferences.get("isoLanguage:windows-11", "")); if (language) setLanguage("windows-11", language) }
    }
    Updater { id: updater; objectName: "updater" }
    // Updating restarts OmaWare: running VMs are paused first (see applyUpdate), and the new copy reopens on the same page.
    Connections {
        target: updater
        // VMs were already paused (or you chose to update anyway); anything else still pauses on the way out.
        function onQuitRequested() { preferences.set("restoreNavigation", root.navigation); if (root.updatePhase === "restarting") root.exiting = true; Qt.callLater(root.close) }
        function onChanged() {
            if (root.updating && root.updatePhase === "" && updater.status === "ready") root.applyUpdate()
            else if (updater.status === "error" && root.updatePhase !== "pausing") { root.updating = false; root.updatePhase = "" }
        }
    }
    // Updating: download, pause running VMs, swap the new version in and restart. Nothing happens without "Update now".
    property bool updating: false           // "Update now" was pressed; apply as soon as the download is ready
    property string updatePhase: ""         // "pausing" or "restarting" while applying
    property var updateFailures: []
    property int updateRunning: 0           // how many VMs were running when applying started
    readonly property int runningOwned: backend.connected ? backend.domains.filter(function(row) { return row.owned && row.stateCode === 1 }).length : 0
    function openUpdates(check) {
        updateDialog.open()
        if (check && !updater.development && ["checking", "downloading", "ready"].indexOf(updater.status) < 0 && updatePhase === "") updater.check()
    }
    function startUpdate() {
        updateFailures = []; updating = true
        if (updater.status === "ready") applyUpdate()
        else updater.download()
    }
    function applyUpdate() {
        updateRunning = runningOwned
        if (runningOwned === 0) { restartIntoUpdate(); return }
        updatePhase = "pausing"; pausingForExit = true
        display.releaseInput()
        backend.pauseForExit()
    }
    function restartIntoUpdate() {
        updateFailures = []; updatePhase = "restarting"; pausingForExit = false
        if (!updater.installAndRestart()) { updatePhase = ""; updating = false }
    }
    function cancelUpdate() { updating = false; updatePhase = ""; updateFailures = []; pausingForExit = false }
    UpdateDialog {
        id: updateDialog
        updater: updater
        runningCount: root.updatePhase === "" ? root.runningOwned : root.updateRunning
        phase: root.updatePhase; failures: root.updateFailures
        onUpdateNow: root.startUpdate()
        onUpdateAnyway: root.restartIntoUpdate()
        onStay: { root.cancelUpdate(); close() }
        onClosed: if (root.updatePhase === "") root.updating = false
    }
    // A quiet daily check, unless turned off in Settings. Development builds never update themselves.
    property bool autoUpdate: preferences.get("autoUpdateCheck", true) !== false && preferences.get("autoUpdateCheck", true) !== "false"
    Timer {
        interval: 8000; running: root.autoUpdate && !updater.development; repeat: false
        onTriggered: {
            const last = Number(preferences.get("lastUpdateCheck", 0))
            if (Date.now() - last > 20 * 3600 * 1000) { preferences.set("lastUpdateCheck", Date.now()); updater.check() }
        }
    }
    CreateVmDialog { id: createDialog; workspace: preferences; isoLibrary: isoLibrary; onGetIsos: root.openShop() }
    Connections {
        target: isoLibrary
        function onFinished(id, ok, message) { if (!ok) root.operationError = message }
        // Dropped ISOs: one goes straight into Create VM (unless you're looking at your ISOs); several are listed.
        function onImported(paths, ok, message) {
            isoDropZone.show(message, !ok)
            if (!ok || paths.length === 0) return
            if (createDialog.visible) createDialog.useIso(paths[0])
            else if (paths.length === 1 && root.navigation !== "isos") createDialog.beginWith(paths[0])
            else { root.navigation = "isos"; isoShop.category = "mine" }
        }
    }
    // Above everything, open dialogs included, so an ISO can be dropped anywhere.
    IsoDropZone { id: isoDropZone; parent: Overlay.overlay; anchors.fill: parent; z: 1000; library: isoLibrary }
    HardwareDialog { id: hardwareDialog; workspace: preferences }
    // AI agents: a lab plan to review, questions before anything is deleted, and a banner while they work.
    LabDialog { id: labDialog; copyHelper: preferences }
    AgentBanner {
        parent: Overlay.overlay
        x: (parent.width - width) / 2; y: 10; z: 40
        width: Math.min(720, parent.width - 40)
        onShowVm: function(uuid) { root.selectVm(uuid); root.detailsOpen = false }
        onShowLab: labDialog.open()
    }
    AppDialog {
        id: agentQuestion; objectName: "agentQuestion"
        readonly property var question: agent.confirmation
        width: Math.min(520, root.width - 40)
        heading: question.title || ""
        subtitle: "Asked by an AI agent"
        headerIcon: "info"
        closePolicy: Popup.CloseOnEscape
        onQuestionChanged: if (question.id) open(); else close()
        onRejected: if (question.id) agent.answer(question.id, false)
        contentItem: ColumnLayout {
            spacing: 14
            Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; text: agentQuestion.question.text || "" }
            RowLayout {
                spacing: 8
                Item { Layout.fillWidth: true }
                AppButton { objectName: "agentQuestionNo"; text: "No"; onClicked: agent.answer(agentQuestion.question.id, false) }
                AppButton { objectName: "agentQuestionYes"; text: agentQuestion.question.action || "Yes"; tone: "danger"; onClicked: agent.answer(agentQuestion.question.id, true) }
            }
        }
    }
    PendingDialog { id: pendingDialog; objectName: "pendingDialog" }
    ConsoleToolbar {
        id: consoleToolbar; objectName: "consoleTools"
        parent: root.consoleFullScreen ? fullscreenBar : root.consoleDetached ? detachedTools : embeddedTools
        anchors.right: parent.right; anchors.rightMargin: root.consoleFullScreen ? 8 : 0; anchors.verticalCenter: parent.verticalCenter
        width: implicitWidth; height: implicitHeight
        visible: root.consoleDetached || (!root.detailsOpen && root.navigation === "library")
        workspace: root; guestDisplay: display; history: vmDetails.snapshotsView
        showLabels: root.consoleDetached ? detachedConsole.width >= 760 * theme.textScale : root.consoleFocus || root.width >= 1280 * theme.textScale
    }
    Item {
        id: fullscreenControls; objectName: "fullscreenControls"
        parent: root.consoleDetached ? detachedConsole.contentItem : root.contentItem
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        height: fullscreenBar.height + 12; z: 50
        visible: root.consoleFullScreen
        Item {
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; height: 12
            HoverHandler { id: topEdge; onHoveredChanged: if (hovered) root.toolbarRevealed = true }
        }
        Rectangle {
            id: fullscreenBar; objectName: "fullscreenToolbar"
            anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width - 24, Math.max(660 * theme.textScale, consoleToolbar.implicitWidth + 160))
            height: consoleToolbar.implicitHeight + 16
            radius: 4; color: theme.colors.surface; border.color: theme.colors.border
            visible: root.toolbarRevealed
            // Padding keeps controls clear of the rounded edge.
            anchors.topMargin: 6
            Label { visible: parent.width - consoleToolbar.width > 90; anchors.left: parent.left; anchors.leftMargin: 14; anchors.right: parent.right; anchors.rightMargin: consoleToolbar.width + 20; anchors.verticalCenter: parent.verticalCenter; text: root.selected ? root.vmLabel(root.selected) : "Console"; elide: Text.ElideRight; font.weight: Font.DemiBold }
            HoverHandler { id: barHover }
            MouseArea { anchors.fill: parent; z: -1; acceptedButtons: Qt.AllButtons; onClicked: function(mouse) { if (mouse.button === Qt.RightButton) consoleToolbar.openTools() } }
        }
        Timer { interval: 550; running: root.consoleFullScreen && root.toolbarRevealed && !topEdge.hovered && !barHover.hovered && !consoleToolbar.popupOpen && !vmDetails.snapshotsView.dialogOpen && !consoleToolbar.keyboardNavigation; onTriggered: root.toolbarRevealed = false }
    }
    ApplicationWindow {
        id: detachedConsole
        palette: root.palette
        font: root.font
        objectName: "detachedConsole"
        visible: root.consoleDetached
        width: 1000; height: 720; minimumWidth: 640; minimumHeight: 480
        title: (root.selected ? root.selected.name : "Console") + " — OmaWare"
        color: theme.colors.background
        onClosing: { root.consoleDetached = false; display.releaseInput() }
        Shortcut { sequence: "Escape"; enabled: root.consoleFullScreen && !display.captured && !consoleToolbar.popupOpen && !vmDetails.snapshotsView.dialogOpen; onActivated: root.toggleConsoleFullscreen() }
        ColumnLayout { anchors.fill: parent; spacing: 0
            Item { Layout.fillWidth: true; implicitHeight: consoleToolbar.implicitHeight + 16; visible: !root.consoleFullScreen
                Label { anchors.left: parent.left; anchors.leftMargin: 16; anchors.right: detachedTools.left; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: root.selected ? root.vmLabel(root.selected) : "Console"; elide: Text.ElideRight; font.weight: Font.DemiBold }
                Item { id: detachedTools; anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; width: consoleToolbar.implicitWidth; height: consoleToolbar.implicitHeight }
            }
            Item { id: detachedStage; Layout.fillWidth: true; Layout.fillHeight: true }
        }
    }
    EditorDialog {
        id: organizeDialog
        objectName: "organizeDialog"
        property string uuid: ""
        headerIcon: "monitor"; subtitle: "Make this workspace yours"
        heading: "Organize VM"; actionText: "Save preferences"; requiresConnection: false
        function openFor(vm) { uuid = vm.uuid; const meta = preferences.vm(uuid); alias.text = meta.alias || vm.name; folder.text = meta.folder || ""; tags.text = meta.tags || ""; notes.text = meta.notes || ""; favorite.checked = !!meta.favorite; open() }
        Label { text: "Display name" }
        AppField { id: alias; objectName: "vmAlias"; Layout.fillWidth: true }
        Label { text: "Folder" }
        AppField { id: folder; objectName: "vmFolder"; Layout.fillWidth: true; placeholderText: "Development" }
        Label { text: "Tags" }
        AppField { id: tags; Layout.fillWidth: true; placeholderText: "linux, web, testing" }
        AppCheckBox { id: favorite; objectName: "vmFavorite"; text: "Favorite · keep near the top of the library" }
        Label { text: "Notes" }
        AppTextArea { id: notes; Layout.fillWidth: true; Layout.preferredHeight: 130; wrapMode: TextEdit.WordWrap; color: theme.colors.foreground }
        onSubmitted: { preferences.saveVm(uuid, {alias: alias.text, folder: folder.text, tags: tags.text, notes: notes.text, favorite: favorite.checked}); accept() }
    }
    EditorDialog {
        id: diskDialog
        property var info: ({})
        property var disk: ({})
        property bool removing: false
        headerIcon: "disk"
        heading: removing ? "Detach this disk?" : "Add a virtual disk"
        actionText: removing ? "Detach disk" : "Create and attach disk"
        actionTone: removing ? "danger" : "primary"
        height: Math.min(470, parent.height - 40)
        function openFor(vm, device, remove) { info = JSON.parse(JSON.stringify(vm)); disk = device; removing = remove; size.text = "16"; open() }
        Label { text: diskDialog.info.name || ""; font.weight: Font.DemiBold }
        Label { visible: !diskDialog.removing; text: "New disk capacity (GiB)" }
        AppField { id: size; visible: !diskDialog.removing; Layout.fillWidth: true; validator: IntValidator { bottom: 1; top: 2048 } }
        Label { text: diskDialog.removing ? (diskDialog.disk.source || "") : "A new qcow2 disk with a virtio controller will be created in OmaWare's VM storage directory."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
        Label { text: diskDialog.removing ? "The disk file is kept. A running VM retains its current attachment until a full shutdown and start." : "The guest needs a virtio storage driver. Format and mount the new disk inside the guest after starting it."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        onSubmitted: execute(removing ? "disk.detach" : "disk.add", {uuid: info.uuid, revision: info.revision, target: disk.target || "", diskGiB: Number(size.text)})
    }
    AppDialog {
        id: activityDialog; objectName: "activityDialog"
        headerIcon: "history"; heading: "Activity history"; subtitle: "Last 200 operations · saved across restarts"
        width: Math.min(740, parent.width - 40); height: Math.min(680, parent.height - 40)
        contentItem: ColumnLayout { spacing: 12
            RowLayout { Layout.fillWidth: true
                AppSelect { id: activityFilter; objectName: "activityFilter"; Accessible.name: "Filter activity"; model: ["All activity", "Needs attention"]; Layout.fillWidth: true }
                AppButton { text: "Copy log"; onClicked: preferences.copy(backend.activity.map(function(item) { return item.time + " " + item.message + (item.nextStep ? "\n" + item.nextStep : "") }).join("\n\n")) }
                AppButton { text: "Clear…"; tone: "quiet"; enabled: backend.activity.length > 0; onClicked: clearHistory.open() }
            }
            Label { visible: backend.activityWarning !== ""; text: backend.activityWarning; color: theme.colors.warning; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            Label { visible: activityList.count === 0; text: activityFilter.currentIndex === 1 ? "No failures in the saved history." : "No activity yet. Recent operations will appear here."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            ListView {
                id: activityList; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 10
                model: activityDialog.visible ? backend.activity.filter(function(entry) { return activityFilter.currentIndex === 0 || !entry.ok }) : []
                boundsBehavior: Flickable.StopAtBounds; ScrollBar.vertical: AppScrollBar {}
                delegate: Rectangle {
                    id: activityEntry; required property var modelData
                    width: activityList.width; height: activityContent.implicitHeight + 24
                    color: theme.colors.field; radius: 4; border.color: theme.colors.border
                    ColumnLayout { id: activityContent; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 12; spacing: 8
                        RowLayout { Layout.fillWidth: true
                            AppIcon { name: activityEntry.modelData.ok ? "check" : "info"; color: activityEntry.modelData.ok ? theme.colors.success : theme.colors.warning; width: 16; height: 16 }
                            Label { text: (activityEntry.modelData.ok ? "Completed" : "Needs attention") + " · " + Qt.formatDateTime(new Date(activityEntry.modelData.time), "MMM d, yyyy · HH:mm:ss"); color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round((11) * theme.textScale)}
                        }
                        Label { textFormat: Text.PlainText; text: activityEntry.modelData.message.split("\n")[0]; Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight }
                        AppDisclosure { title: "Details"; resetKey: activityEntry.modelData.id || activityEntry.modelData.time
                            Label { textFormat: Text.PlainText; visible: !activityEntry.modelData.ok; text: activityEntry.modelData.nextStep; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.Wrap }
                            AppTextArea { text: activityEntry.modelData.message; readOnly: true; Layout.fillWidth: true; Layout.preferredHeight: Math.min(160, implicitHeight); selectByMouse: true }
                        }
                    }
                }
            }
        }
    }
    AppDialog {
        id: clearHistory; heading: "Clear activity history?"; headerIcon: "history"
        width: Math.min(440, parent.width - 40)
        contentItem: Label { text: "This removes the saved operation log. Your VMs and snapshots are kept."; wrapMode: Text.WordWrap }
        footer: RowLayout { Layout.margins: 20; spacing: 8
            Item { Layout.fillWidth: true }
            AppButton { text: "Cancel"; onClicked: clearHistory.reject() }
            AppButton { text: "Clear history"; tone: "danger"; onClicked: { backend.clearActivity(); clearHistory.accept() } }
        }
    }
    // Shown while running VMs are paused on close; stays open only if some could not be paused.
    AppDialog {
        id: exitDialog; objectName: "exitDialog"
        property int count: 0
        property var failures: []
        width: Math.min(460, parent.width - 40)
        dismissible: false
        closePolicy: Popup.NoAutoClose
        headerIcon: "pause"
        heading: failures.length ? "Some VMs are still running" : "Pausing VMs"
        subtitle: failures.length ? "The rest were paused." : "Pausing " + count + (count === 1 ? " running VM" : " running VMs") + " so they continue where they left off"
        contentItem: ColumnLayout {
            spacing: 14
            RowLayout {
                visible: exitDialog.failures.length === 0
                spacing: 10
                AppBusyIndicator { running: exitDialog.visible && exitDialog.failures.length === 0; implicitWidth: 22; implicitHeight: 22 }
                Label { text: backend.busy ? "Waiting for the current operation to finish…" : "OmaWare closes as soon as they are paused."; color: theme.colors.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
            Repeater {
                model: exitDialog.failures
                Label { required property var modelData; textFormat: Text.PlainText; text: modelData; color: theme.colors.danger; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
            RowLayout {
                visible: exitDialog.failures.length > 0
                Layout.topMargin: 6
                Item { Layout.fillWidth: true }
                AppButton { objectName: "exitStay"; text: "Keep OmaWare open"; onClicked: { root.pausingForExit = false; exitDialog.close() } }
                AppButton { objectName: "exitAnyway"; text: "Close anyway"; tone: "primary"; onClicked: { root.exiting = true; exitDialog.close(); Qt.callLater(root.close) } }
            }
        }
    }
    AppDialog {
        id: keyMap; objectName: "keyMap"
        headerIcon: "keyboard"; heading: "Keyboard map"; subtitle: "Shortcuts pause while the console captures input · Ctrl+Alt releases it"
        width: Math.min(620, parent.width - 40)
        contentItem: GridLayout {
            columns: 2; columnSpacing: 18; rowSpacing: 8
            Repeater {
                model: [
                    [": · Ctrl+Shift+P", "Command prompt"], ["?  · F1", "This keyboard map"], ["Ctrl+K · Ctrl+F", "Search machines"],
                    ["Ctrl+1 / 2 / 3 / 4", "Machines · Monitor · Networks · ISO Shop"], ["Ctrl+`", "Toggle the log drawer"],
                    ["j / k  ·  ↑ / ↓", "Move through machines (list focused)"], ["Enter", "Open the console"], ["d", "Details for the selected machine"],
                    ["s", "Snapshots for the selected machine"], ["Menu · Shift+F10", "VM actions (also right-click a VM)"], ["f  ·  F2  ·  Delete", "Favorite · rename · remove a stopped VM"],
                    ["Ctrl+click · Shift+click", "Select several machines to turn on, pause or shut down together"], ["Ctrl+A  ·  Esc", "Select every listed machine · clear the selection"],
                    ["Ctrl+B", "Collapse or expand the sidebar"], ["Esc", "Leave focus or fullscreen console"], ["Ctrl+Alt", "Release captured console input"]
                ]
                delegate: Item {
                    required property var modelData
                    required property int index
                    Layout.columnSpan: 2; Layout.fillWidth: true; implicitHeight: keyRow.implicitHeight
                    RowLayout {
                        id: keyRow; anchors.left: parent.left; anchors.right: parent.right; spacing: 18
                        Label { text: modelData[0]; color: theme.colors.accent; font.weight: Font.DemiBold; Layout.preferredWidth: 190 * theme.textScale
                            leftPadding: 8; rightPadding: 8; topPadding: 3; bottomPadding: 3
                            background: Rectangle { color: theme.colors.field; border.color: theme.colors.border; radius: 2 } }
                        Label { text: modelData[1]; color: theme.colors.foreground; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    }
                }
            }
        }
    }
    Popup {
        id: appearanceInfo; objectName: "appearanceInfo"
        x: 18; y: Math.max(10, root.height - height - 40)
        width: 320
        // Scrolls when it doesn't fit (large text in a small window), instead of being squeezed.
        height: Math.min(implicitHeight, root.height - 50)
        padding: 16
        background: Rectangle { color: theme.colors.raised; border.color: theme.colors.border; radius: 4 }
        contentItem: ScrollView {
            id: settingsScroll
            clip: true
            contentWidth: availableWidth
            implicitHeight: settingsColumn.implicitHeight
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            ColumnLayout { id: settingsColumn; width: settingsScroll.availableWidth; spacing: 12
                Label { text: "Settings"; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                Label { text: "Text size" }
                AppSelect { objectName: "textSize"; Accessible.name: "Interface text size"; Layout.fillWidth: true; model: ["Standard · 100%", "Larger · 115%", "Largest · 130%"]
                    currentIndex: theme.textScale > 1.2 ? 2 : theme.textScale > 1.05 ? 1 : 0
                    onActivated: { const scale = [1, 1.15, 1.3][currentIndex]; theme.setTextScale(scale); preferences.set("textScale", scale) }
                }
                AppCheckBox { objectName: "reducedMotion"; text: "Reduce motion"; checked: theme.reducedMotion; Layout.fillWidth: true; onToggled: { theme.setReducedMotion(checked); preferences.set("reducedMotion", checked) } }
                Label { text: "Storage" }
                RowLayout {
                    Layout.fillWidth: true; spacing: 6
                    Label { objectName: "storageFolder"; textFormat: Text.PlainText; text: isoLibrary.root; color: theme.colors.muted; elide: Text.ElideMiddle; Layout.fillWidth: true; font.pixelSize: Math.round(11 * theme.textScale) }
                    AppButton { text: "Open"; iconName: "folder"; tone: "quiet"; implicitHeight: 28; hint: "VMs are in vms/, ISOs in isos/"; onClicked: Qt.openUrlExternally("file://" + isoLibrary.root) }
                }
                Label { text: "Updates" }
                ColumnLayout {
                    objectName: "updateSettings"
                    Layout.fillWidth: true; spacing: 6
                    Label {
                        objectName: "updateStatus"
                        Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round(11 * theme.textScale)
                        color: updater.status === "error" ? theme.colors.danger : updater.status === "available" || updater.status === "ready" ? theme.colors.warning : theme.colors.muted
                        text: updater.development ? "This is a development build; it doesn't update itself."
                            : updater.status === "checking" ? "Checking for updates…"
                            : updater.status === "upToDate" ? "✓ You have the latest version (" + updater.current + ")."
                            : updater.status === "available" ? "OmaWare " + updater.latest + " is available." + (updater.canInstall ? "" : " Download it from the releases page.")
                            : updater.status === "downloading" ? "Downloading OmaWare " + updater.latest + "… " + Math.round(updater.progress * 100) + "%"
                            : updater.status === "ready" ? "OmaWare " + updater.latest + " is downloaded and ready to install."
                            : updater.status === "error" ? updater.error
                            : "You have OmaWare " + updater.current + "."
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        AppButton { objectName: "updateCheck"; visible: !updater.development; text: ["available", "downloading", "ready"].indexOf(updater.status) >= 0 ? "Update…" : "Check for updates"; tone: ["available", "downloading", "ready"].indexOf(updater.status) >= 0 ? "primary" : "quiet"; implicitHeight: 28; onClicked: { appearanceInfo.close(); root.openUpdates(true) } }
                    }
                    AppCheckBox {
                        objectName: "autoUpdateCheck"; visible: !updater.development
                        text: "Check automatically"
                        Layout.fillWidth: true
                        checked: root.autoUpdate
                        onToggled: { root.autoUpdate = checked; preferences.set("autoUpdateCheck", checked) }
                    }
                    Label { visible: !updater.development; text: "Once a day, OmaWare asks GitHub whether a new version is out."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round(10 * theme.textScale) }
                }
                Label { text: "When OmaWare closes" }
                Label { objectName: "closeNote"; text: "Running VMs are paused, including when OmaWare restarts for an update. Paused VMs keep their memory and continue exactly where they were when you resume them, but not after the computer restarts."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round((11) * theme.textScale) }
                Label { text: "AI agents" }
                ColumnLayout {
                    objectName: "agentSettings"
                    Layout.fillWidth: true; spacing: 6
                    AppCheckBox { objectName: "agentAccess"; text: "Let AI agents use OmaWare"; Layout.fillWidth: true; checked: agent.enabled; onToggled: agent.enabled = checked }
                    Label {
                        text: "Agents such as Claude Code can then see OmaWare's VMs, use their screens, run commands in lab VMs and propose labs. You approve every lab, set its password here, and confirm anything that deletes."
                        color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round(10 * theme.textScale)
                    }
                    Label { visible: agent.enabled; text: "Connect Claude Code by running:"; font.pixelSize: Math.round(11 * theme.textScale) }
                    RowLayout {
                        visible: agent.enabled
                        Layout.fillWidth: true; spacing: 6
                        Label { objectName: "agentCommand"; text: agent.command; textFormat: Text.PlainText; font.family: "monospace"; elide: Text.ElideMiddle; Layout.fillWidth: true; color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale)
                            ToolTip.visible: commandArea.containsMouse; ToolTip.text: agent.command
                            MouseArea { id: commandArea; anchors.fill: parent; hoverEnabled: true } }
                        AppButton { text: "Copy"; tone: "quiet"; implicitHeight: 28; onClicked: preferences.copy(agent.command) }
                    }
                    Label { visible: agent.error !== ""; text: agent.error; color: theme.colors.danger; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round(10 * theme.textScale) }
                }
                Label { text: theme.status; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round((12) * theme.textScale)}
                Label { text: ": opens the command prompt. ? shows every shortcut."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Math.round((11) * theme.textScale)}
                Label { objectName: "appVersion"; text: "OmaWare" + (Qt.application.version ? " " + Qt.application.version : ""); color: theme.colors.muted; Layout.fillWidth: true; font.pixelSize: Math.round((11) * theme.textScale)}
            }
        }
    }
}
