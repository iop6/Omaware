// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ColumnLayout {
    id: panel
    property var vm: null
    property string vmUuid: vm ? vm.uuid : ""
    property var info: ({})
    property bool busy: false
    property bool canEdit: false
    property var checkpointPreview: function() { return "" }
    property var stats: ({})
    property int page: 0
    property int sampleInterval: 3000
    property alias snapshotsView: snapshotView
    // Host facts arrive with the first sample; stopped VMs need only that one.
    readonly property bool hostKnown: !!stats.hostCpus
    readonly property int sampleCapacity: 100
    readonly property bool wide: contents.width >= 760
    readonly property color seriesPrimary: theme.colors.accent
    readonly property color seriesSecondary: theme.colors.muted
    property var history: emptyHistory()
    property var lastSample: null
    signal reload(bool guestInfo)
    signal editAdapter(var adapter, bool remove)
    signal editHardware()
    signal reviewPending()
    signal manageDisk(var disk, bool remove)
    signal setContainment(bool enabled)
    readonly property var containment: info.containment || ({})
    spacing: 0

    function value(key) { return info[key] === undefined ? "" : String(info[key]) }
    function memory(mib) { return mib >= 1024 ? Number((mib / 1024).toFixed(1)) + " GiB" : Math.round(mib) + " MiB" }
    function networkName(nic) {
        for (let option of (info.networkOptions || [])) if (option.id === nic.networkId) return option.label
        return nic.kind + (nic.source ? " · " + nic.source : "")
    }
    function runtimeText() {
        const nics = info.liveInterfaces || []
        return nics.length === 0 ? "No adapters in the running VM." : nics.map(function(nic) {
            return nic.mac + "  ·  " + panel.networkName(nic) + "  ·  " + (nic.linkUp ? "Connected" : "Disconnected")
        }).join("\n")
    }

    // ---- Formatting -------------------------------------------------------
    function bytes(b) {
        if (b === null || b === undefined || isNaN(b)) return "—"
        const units = ["B", "KiB", "MiB", "GiB", "TiB"]; let i = 0
        while (Math.abs(b) >= 1024 && i < units.length - 1) { b /= 1024; ++i }
        return (i === 0 ? Math.round(b) : b < 10 ? b.toFixed(1) : Math.round(b)) + " " + units[i]
    }
    function rate(b) { return b === null || b === undefined ? "—" : bytes(b) + "/s" }
    function percent(v) { return v === null || v === undefined ? "—" : (v < 10 ? v.toFixed(1) : Math.round(v)) + "%" }
    function tally(n) {
        if (n === null || n === undefined) return "—"
        return n >= 1e9 ? (n / 1e9).toFixed(1) + "B" : n >= 1e6 ? (n / 1e6).toFixed(1) + "M" : n >= 1e4 ? Math.round(n / 1e3) + "k" : String(Math.round(n))
    }
    function duration(sec) {
        if (sec === undefined || sec < 0) return "—"
        const d = Math.floor(sec / 86400), h = Math.floor(sec % 86400 / 3600), m = Math.floor(sec % 3600 / 60)
        return d > 0 ? d + "d " + h + "h " + m + "m" : h > 0 ? h + "h " + m + "m" : m > 0 ? m + "m " + Math.floor(sec % 60) + "s" : Math.floor(sec) + "s"
    }
    function latest(values) { for (let i = (values || []).length - 1; i >= 0; --i) if (values[i] !== null && values[i] !== undefined) return values[i]; return null }
    function summary(values) {
        let total = 0, n = 0, peak = null
        for (const v of (values || [])) if (v !== null && v !== undefined) { total += v; ++n; if (peak === null || v > peak) peak = v }
        return {average: n ? total / n : null, peak: peak}
    }
    function add(a, b) { return a === null || b === null ? null : a + b }

    // ---- Sample history ---------------------------------------------------
    function emptyHistory() { return {times: [], cpu: [], mem: [], rss: [], rd: [], wr: [], rx: [], tx: [], vcpu: {}, disks: {}, nics: {}} }
    function resetHistory() { history = emptyHistory(); lastSample = null }
    function total(rows, key) { let t = 0, any = false; for (const r of (rows || [])) if (r[key] !== undefined) { t += r[key]; any = true } return any ? t : null }
    function delta(current, previous, seconds) { return current === null || previous === null || current === undefined || previous === undefined || current < previous || seconds <= 0 ? null : (current - previous) / seconds }
    function find(rows, key, value) { for (const r of (rows || [])) if (r[key] === value) return r; return null }
    function append(list, value) { return (list || []).concat([value]).slice(-sampleCapacity) }
    function ingest(s) {
        if (!s || s.uuid !== vmUuid || !s.sampledAt) return
        if (!s.active) { if (history.times.length) resetHistory(); return }
        const prev = lastSample
        if (prev && s.sampledAt <= prev.sampledAt) return
        const dt = prev ? (s.sampledAt - prev.sampledAt) / 1000 : 0
        // A long pause (details hidden) breaks the lines instead of averaging across it.
        const fresh = !!prev && dt <= sampleInterval / 1000 * 4
        const h = history
        const next = {times: append(h.times, s.sampledAt)}
        const length = next.times.length
        next.cpu = append(h.cpu, fresh && s.cpuPercent !== undefined ? s.cpuPercent : null)
        next.mem = append(h.mem, s.usedMiB !== undefined ? s.usedMiB : null)
        next.rss = append(h.rss, s.hostRssMiB !== undefined ? s.hostRssMiB : null)
        const pb = prev ? prev.blocks : [], pn = prev ? prev.nets : []
        next.rd = append(h.rd, fresh ? delta(total(s.blocks, "rd_bytes"), total(pb, "rd_bytes"), dt) : null)
        next.wr = append(h.wr, fresh ? delta(total(s.blocks, "wr_bytes"), total(pb, "wr_bytes"), dt) : null)
        next.rx = append(h.rx, fresh ? delta(total(s.nets, "rx_bytes"), total(pn, "rx_bytes"), dt) : null)
        next.tx = append(h.tx, fresh ? delta(total(s.nets, "tx_bytes"), total(pn, "tx_bytes"), dt) : null)
        const vcpu = {}, disks = {}, nics = {}
        const series = function(old, value) { return append(old || Array(length - 1).fill(null), value) }
        for (const v of (s.vcpus || [])) {
            const p = fresh ? find(prev.vcpus, "id", v.id) : null
            const busy = p ? delta(v.time, p.time, dt) : null // ns of vCPU time per second
            vcpu[v.id] = series(h.vcpu[v.id], busy === null ? null : Math.min(100, busy / 1e7))
        }
        for (const b of (s.blocks || [])) {
            const p = fresh ? find(pb, "name", b.name) : null, old = h.disks[b.name] || {}
            disks[b.name] = {rd: series(old.rd, p ? delta(b.rd_bytes, p.rd_bytes, dt) : null), wr: series(old.wr, p ? delta(b.wr_bytes, p.wr_bytes, dt) : null)}
        }
        for (const n of (s.nets || [])) {
            const p = fresh ? find(pn, "name", n.name) : null, old = h.nics[n.name] || {}
            nics[n.name] = {rx: series(old.rx, p ? delta(n.rx_bytes, p.rx_bytes, dt) : null), tx: series(old.tx, p ? delta(n.tx_bytes, p.tx_bytes, dt) : null)}
        }
        next.vcpu = vcpu; next.disks = disks; next.nics = nics
        lastSample = s
        history = next
    }
    onStatsChanged: ingest(stats)
    onVmUuidChanged: resetHistory()

    readonly property var live: lastSample || ({})
    readonly property var cpuSummary: summary(history.cpu)
    readonly property real guestTotalMiB: live.guestTotalMiB || live.balloonMiB || info.currentMemoryMiB || 0
    readonly property var vcpuRows: Object.keys(history.vcpu).map(function(id) { return {id: Number(id), values: history.vcpu[id]} }).sort(function(a, b) { return a.id - b.id })
    readonly property var storageDisks: (info.disks || []).filter(function(d) { return d.device !== "cdrom" })
    readonly property real capacityBytes: storageDisks.reduce(function(a, d) { return a + (d.capacityBytes || 0) }, 0)
    readonly property real allocationBytes: storageDisks.reduce(function(a, d) { return a + (d.allocationBytes || 0) }, 0)
    function liveTarget(nic) { const match = find(info.liveInterfaces, "mac", nic.mac); return match ? match.target : "" }

    // ---- Technical detail models (shared by the view and Copy all) --------
    readonly property var computeRows: [
        {label: "Virtualization", value: value("hypervisor") + " · " + value("architecture")},
        {label: "Machine", value: value("machine")},
        {label: "Processors", value: value("vcpus") + (info.vcpus === 1 ? " vCPU" : " vCPUs")},
        {label: "CPU mode", value: value("cpuMode")},
        {label: "CPU model", value: value("cpuModel")},
        {label: "Topology", value: value("cpuTopology")},
        {label: "Memory", value: info.memoryMiB ? memory(info.memoryMiB) + " maximum · " + memory(info.currentMemoryMiB) + " configured" : ""},
        {label: "Firmware", value: value("firmware")},
        {label: "Secure Boot", value: value("secureBoot")},
        {label: "Boot order", value: value("bootOrder") || "Per-device or hypervisor default"},
        {label: "Firmware file", value: value("loader") || "Hypervisor default"},
        {label: "NVRAM", value: value("nvram") || "None"},
        {label: "Emulator", value: value("emulator")}
    ]
    readonly property var identityRows: [
        {label: "Name", value: value("name")},
        {label: "State", value: vm ? vm.state : ""},
        {label: "UUID", value: value("uuid")},
        {label: "Domain ID", value: value("id")},
        {label: "Connection", value: value("uri")},
        {label: "Management", value: info.owned ? "Managed by OmaWare" : "Read only"},
        {label: "Definition", value: info.persistent ? "Persistent" : "Transient"},
        {label: "Autostart", value: value("autostart")},
        {label: "OS profile", value: value("osProfile")},
        {label: "Description", value: value("description") || "None"}
    ]
    readonly property var counterRows: {
        if (!info.active || !live.sampledAt) return []
        let rows = [
            {label: "CPU activity", value: live.cpuPercent === undefined ? "Unavailable" : Number(live.cpuPercent).toFixed(1) + "% across assigned CPUs"},
            {label: "Guest RAM in use", value: live.usedMiB === undefined ? "Guest statistics unavailable" : memory(live.usedMiB) + " · reported by balloon driver"},
            {label: "Balloon target", value: live.balloonMiB ? memory(live.balloonMiB) : "Not reported"},
            {label: "Guest caches", value: live.cacheMiB !== undefined ? memory(live.cacheMiB) : "Not reported"},
            {label: "Swap in / out", value: live.swapInKiB !== undefined ? bytes(live.swapInKiB * 1024) + " in · " + bytes((live.swapOutKiB || 0) * 1024) + " out" : "Not reported"},
            {label: "Major page faults", value: live.majorFaults !== undefined ? tally(live.majorFaults) : "Not reported"},
            {label: "QEMU resident memory", value: live.hostRssMiB !== undefined ? memory(live.hostRssMiB) + " on the host" : "Not reported"},
            {label: "Host", value: live.hostCpus ? live.hostCpus + " CPU threads · " + memory(live.hostMemoryMiB) + " RAM · " + memory(live.hostFreeMiB || 0) + " free" : "Not reported"}
        ]
        for (const b of (live.blocks || [])) rows.push({label: "Disk " + b.name, value: bytes(b.rd_bytes) + " read (" + tally(b.rd_reqs) + " ops) · " + bytes(b.wr_bytes) + " written (" + tally(b.wr_reqs) + " ops)"})
        for (const n of (live.nets || [])) rows.push({label: "Interface " + n.name, value: bytes(n.rx_bytes) + " received (" + tally(n.rx_pkts) + " pkts) · " + bytes(n.tx_bytes) + " sent (" + tally(n.tx_pkts) + " pkts) · " + ((n.rx_errs || 0) + (n.tx_errs || 0)) + " errors · " + ((n.rx_drop || 0) + (n.tx_drop || 0)) + " drops"})
        return rows
    }
    function technicalText() {
        const block = function(title, rows) { return title + "\n" + rows.map(function(r) { return "  " + r.label + ": " + (r.value || "Not reported") }).join("\n") }
        let parts = [block("Compute & boot", computeRows), block("Identity & management", identityRows)]
        if (counterRows.length) parts.push(block("Live counters", counterRows))
        return parts.join("\n\n")
    }

    TextEdit { id: clipboardBridge; visible: false }
    Timer { id: copiedReset; interval: 1600; onTriggered: copyButton.copied = false }

    RowLayout {
        visible: panel.page !== 3
        Layout.fillWidth: true
        Layout.margins: 10
        spacing: 5
        Repeater {
            model: ["Overview", "Hardware", "Networks"]
            AppButton {
                required property int index
                required property string modelData
                objectName: "detailsPage" + index
                text: modelData
                tone: "tab"
                checked: panel.page === index
                onClicked: panel.page = index
            }
        }
        Item { Layout.fillWidth: true }
        AppBusyIndicator { running: panel.busy; visible: running; Layout.preferredWidth: 22; Layout.preferredHeight: 22 }
        AppButton { objectName: "refreshDetails"; iconName: "refresh"; tone: "quiet"; hint: "Refresh VM details"; enabled: !panel.busy && panel.vm !== null; onClicked: panel.reload(false) }
    }
    Rectangle { visible: panel.page !== 3; Layout.fillWidth: true; height: 1; color: theme.colors.border }
    Flickable {
        id: scroll
        objectName: "detailsScroll"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentWidth: width
        contentHeight: contents.implicitHeight + 40
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: AppScrollBar {}
        Connections { target: panel; function onPageChanged() { scroll.contentY = 0 } function onVmUuidChanged() { scroll.contentY = 0 } }
        ColumnLayout {
            id: contents
            x: 24; y: 20
            width: scroll.width - 48
            spacing: 16
            Label { textFormat: Text.PlainText; visible: !panel.info.uuid || !!panel.info.error; text: panel.info.error || (panel.busy ? "Reading virtual machine configuration…" : "Select a VM to see its configuration."); color: theme.colors.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            ColumnLayout {
                visible: !!panel.info.uuid && !panel.info.error
                Layout.fillWidth: true
                spacing: 20
                Rectangle {
                    visible: (panel.info.changes || []).length > 0 || !!panel.info.pendingConflict
                    Layout.fillWidth: true; implicitHeight: pendingRow.implicitHeight + 20; color: theme.colors.accentSoft; radius: 4
                    RowLayout { id: pendingRow; anchors.fill: parent; anchors.margins: 10
                        Label { text: panel.info.pendingConflict ? "Saved settings changed externally" : (panel.info.changes || []).length + " changes saved for the next start"; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        AppButton { text: "Review"; onClicked: panel.reviewPending() }
                    }
                }

                // ================= Overview =================
                ColumnLayout {
                    visible: panel.page === 0
                    Layout.fillWidth: true
                    spacing: 12
                    RowLayout { Layout.fillWidth: true
                        Label { text: "Virtual machine"; font.pixelSize: Math.round((20) * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true }
                        AppButton { text: "Edit hardware"; enabled: panel.canEdit && !panel.busy; onClicked: panel.editHardware() }
                    }
                    // Allocation at a glance, relative to what the host has.
                    GridLayout {
                        objectName: "allocationCards"
                        Layout.fillWidth: true; columns: panel.wide ? 4 : 2; columnSpacing: 10; rowSpacing: 10
                        MetricCard {
                            Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.fillHeight: true
                            label: "Processors"; value: panel.info.vcpus ? panel.info.vcpus + (panel.info.vcpus === 1 ? " vCPU" : " vCPUs") : "—"
                            detail: panel.value("cpuMode") || "Assigned to this VM"
                            fraction: panel.stats.hostCpus && panel.info.vcpus ? panel.info.vcpus / panel.stats.hostCpus : -1
                            fractionText: panel.stats.hostCpus ? "of " + panel.stats.hostCpus + " host CPU threads" : ""
                        }
                        MetricCard {
                            Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.fillHeight: true
                            iconName: "memory"; label: "Memory"; value: panel.info.currentMemoryMiB ? panel.memory(panel.info.currentMemoryMiB) : "—"
                            detail: panel.info.memoryMiB > panel.info.currentMemoryMiB ? "Up to " + panel.memory(panel.info.memoryMiB) : "Assigned to this VM"
                            fraction: panel.stats.hostMemoryMiB && panel.info.currentMemoryMiB ? panel.info.currentMemoryMiB / panel.stats.hostMemoryMiB : -1
                            fractionText: panel.stats.hostMemoryMiB ? Math.round(panel.info.currentMemoryMiB / panel.stats.hostMemoryMiB * 100) + "% of " + panel.memory(panel.stats.hostMemoryMiB) + " host RAM" : ""
                        }
                        MetricCard {
                            Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.fillHeight: true
                            iconName: "disk"; label: "Storage"; value: panel.capacityBytes > 0 ? panel.bytes(panel.capacityBytes) : panel.storageDisks.length ? "Unknown size" : "No disks"
                            detail: panel.storageDisks.length + (panel.storageDisks.length === 1 ? " disk" : " disks") + ((panel.info.disks || []).length > panel.storageDisks.length ? " · optical drive" : "")
                            fraction: panel.capacityBytes > 0 && panel.allocationBytes > 0 ? panel.allocationBytes / panel.capacityBytes : -1
                            fractionText: panel.allocationBytes > 0 ? panel.bytes(panel.allocationBytes) + " written to host storage" : ""
                        }
                        MetricCard {
                            Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.fillHeight: true
                            iconName: "network"; label: "Network"
                            value: (panel.info.interfaces || []).length === 0 ? "Offline" : (panel.info.interfaces || []).length + ((panel.info.interfaces || []).length === 1 ? " adapter" : " adapters")
                            detail: (panel.info.interfaces || []).map(function(nic) { return panel.networkName(nic) }).join(", ") || "Not connected"
                        }
                    }

                    // ---- Live performance ----
                    RowLayout {
                        Layout.fillWidth: true; Layout.topMargin: 10
                        spacing: 10
                        Label { text: "Live performance"; font.pixelSize: Math.round(17 * theme.textScale); font.weight: Font.DemiBold }
                        Rectangle {
                            visible: !!panel.info.active
                            implicitWidth: liveRow.implicitWidth + 16; implicitHeight: liveRow.implicitHeight + 8; radius: height / 2
                            color: theme.colors.accentSoft
                            Row {
                                id: liveRow; anchors.centerIn: parent; spacing: 6
                                Rectangle {
                                    id: pulse
                                    width: 7; height: 7; radius: 4; color: theme.colors.success; anchors.verticalCenter: parent.verticalCenter
                                    SequentialAnimation on opacity {
                                        running: pulse.visible && !theme.reducedMotion; loops: Animation.Infinite
                                        NumberAnimation { to: .25; duration: 900; easing.type: Easing.InOutSine }
                                        NumberAnimation { to: 1; duration: 900; easing.type: Easing.InOutSine }
                                    }
                                }
                                Label { text: "Live · every " + panel.sampleInterval / 1000 + " s · last " + Math.round(panel.sampleCapacity * panel.sampleInterval / 60000) + " min"; color: theme.colors.foreground; font.pixelSize: Math.round(11 * theme.textScale) }
                            }
                        }
                        Item { Layout.fillWidth: true }
                        AppIcon { visible: !!panel.info.active; name: "power"; color: theme.colors.muted; width: 14; height: 14 }
                        Label { visible: !!panel.info.active; text: "Up " + panel.duration(panel.live.uptimeSeconds); color: theme.colors.muted; font.pixelSize: Math.round(12 * theme.textScale) }
                    }
                    Rectangle {
                        visible: !panel.info.active
                        Layout.fillWidth: true
                        implicitHeight: stoppedRow.implicitHeight + 36
                        radius: 4; color: theme.colors.surface; border.color: theme.colors.border
                        RowLayout {
                            id: stoppedRow; anchors.fill: parent; anchors.margins: 18; spacing: 14
                            AppIcon { name: "power"; color: theme.colors.muted; width: 22; height: 22 }
                            ColumnLayout {
                                Layout.fillWidth: true; spacing: 3
                                Label { text: "Live statistics start with the VM"; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                Label { text: "Charts appear here while it runs."; color: theme.colors.muted; font.pixelSize: Math.round(12 * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                            }
                        }
                    }
                    GridLayout {
                        objectName: "liveTiles"
                        visible: !!panel.info.active
                        Layout.fillWidth: true; columns: panel.wide ? 2 : 1; columnSpacing: 10; rowSpacing: 10
                        LiveTile {
                            objectName: "cpuTile"
                            Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.fillHeight: true
                            iconName: "cpu"; label: "CPU"
                            value: panel.percent(panel.latest(panel.history.cpu))
                            detail: panel.cpuSummary.peak === null ? "Measuring across assigned vCPUs…"
                                : "Average " + panel.percent(panel.cpuSummary.average) + " · peak " + panel.percent(panel.cpuSummary.peak)
                                  + (panel.live.hostCpus && panel.info.vcpus ? " · ≈" + panel.percent(panel.latest(panel.history.cpu) * panel.info.vcpus / panel.live.hostCpus) + " of host capacity" : "")
                            times: panel.history.times; capacity: panel.sampleCapacity; ceiling: 100
                            format: panel.percent
                            series: [{label: "CPU", values: panel.history.cpu, color: panel.seriesPrimary, fill: true}]
                        }
                        LiveTile {
                            id: memoryTile
                            objectName: "memoryTile"
                            readonly property bool guestReports: panel.live.usedMiB !== undefined
                            Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.fillHeight: true
                            iconName: "memory"; label: guestReports ? "Guest memory" : "QEMU memory on host"
                            value: guestReports ? panel.memory(panel.live.usedMiB) : panel.live.hostRssMiB !== undefined ? panel.memory(panel.live.hostRssMiB) : "—"
                            detail: guestReports ? "in use of " + panel.memory(panel.guestTotalMiB) + " visible to the guest · " + Math.round(panel.live.usedMiB / Math.max(1, panel.guestTotalMiB) * 100) + "%"
                                : "The guest is not reporting memory. Install the virtio balloon driver to see usage inside the VM."
                            times: panel.history.times; capacity: panel.sampleCapacity
                            ceiling: guestReports ? panel.guestTotalMiB : 0; minimumCeiling: 64
                            format: panel.memory
                            series: [{label: guestReports ? "In use" : "Resident", values: guestReports ? panel.history.mem : panel.history.rss, color: panel.seriesPrimary, fill: true}]
                            UsageBar {
                                visible: memoryTile.guestReports && panel.live.freeMiB !== undefined
                                Layout.fillWidth: true; Layout.topMargin: 4
                                readonly property real cache: Math.min(panel.live.cacheMiB || 0, panel.live.usedMiB || 0)
                                total: panel.guestTotalMiB
                                segments: [
                                    {label: "Applications", value: (panel.live.usedMiB || 0) - cache, text: panel.memory((panel.live.usedMiB || 0) - cache), color: theme.colors.accent},
                                    {label: "Cache", value: cache, text: panel.memory(cache), color: Qt.rgba(panel.seriesPrimary.r, panel.seriesPrimary.g, panel.seriesPrimary.b, .45)},
                                    {label: "Free", value: panel.live.freeMiB || 0, text: panel.memory(panel.live.freeMiB || 0), color: theme.colors.border}
                                ]
                            }
                            Label {
                                visible: memoryTile.guestReports && panel.live.hostRssMiB !== undefined
                                text: "QEMU process holds " + panel.memory(panel.live.hostRssMiB || 0) + " on the host"
                                color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap
                            }
                        }
                        LiveTile {
                            objectName: "diskTile"
                            Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.fillHeight: true
                            iconName: "disk"; label: "Disk I/O"
                            value: panel.rate(panel.add(panel.latest(panel.history.rd), panel.latest(panel.history.wr)))
                            detail: "Read " + panel.rate(panel.latest(panel.history.rd)) + " · Write " + panel.rate(panel.latest(panel.history.wr))
                            times: panel.history.times; capacity: panel.sampleCapacity; minimumCeiling: 65536
                            format: panel.rate
                            series: [{label: "Read", values: panel.history.rd, color: panel.seriesPrimary, fill: true},
                                     {label: "Write", values: panel.history.wr, color: panel.seriesSecondary, dashed: true}]
                            Label {
                                text: (panel.live.blocks || []).length === 0 ? "No disk counters reported." : "Since start · " + panel.bytes(panel.total(panel.live.blocks, "rd_bytes")) + " read · " + panel.bytes(panel.total(panel.live.blocks, "wr_bytes")) + " written · " + panel.tally(panel.add(panel.total(panel.live.blocks, "rd_reqs"), panel.total(panel.live.blocks, "wr_reqs"))) + " operations"
                                color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap
                            }
                        }
                        LiveTile {
                            id: networkTile
                            objectName: "networkTile"
                            readonly property real faults: panel.total(panel.live.nets, "rx_errs") + panel.total(panel.live.nets, "tx_errs") + panel.total(panel.live.nets, "rx_drop") + panel.total(panel.live.nets, "tx_drop")
                            Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.fillHeight: true
                            iconName: "network"; label: "Network"
                            value: panel.rate(panel.add(panel.latest(panel.history.rx), panel.latest(panel.history.tx)))
                            detail: "Received " + panel.rate(panel.latest(panel.history.rx)) + " · Sent " + panel.rate(panel.latest(panel.history.tx))
                            times: panel.history.times; capacity: panel.sampleCapacity; minimumCeiling: 16384
                            format: panel.rate
                            series: [{label: "Received", values: panel.history.rx, color: panel.seriesPrimary, fill: true},
                                     {label: "Sent", values: panel.history.tx, color: panel.seriesSecondary, dashed: true}]
                            Label {
                                text: (panel.live.nets || []).length === 0 ? "No network counters reported." : "Since start · " + panel.bytes(panel.total(panel.live.nets, "rx_bytes")) + " received · " + panel.bytes(panel.total(panel.live.nets, "tx_bytes")) + " sent · " + panel.tally(panel.add(panel.total(panel.live.nets, "rx_pkts"), panel.total(panel.live.nets, "tx_pkts"))) + " packets"
                                color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap
                            }
                            RowLayout {
                                visible: networkTile.faults > 0
                                spacing: 6
                                AppIcon { name: "info"; color: theme.colors.warning; width: 13; height: 13 }
                                Label { text: panel.tally(networkTile.faults) + " errors or dropped packets since start"; color: theme.colors.warning; font.pixelSize: Math.round(11 * theme.textScale) }
                            }
                        }
                        Rectangle {
                            objectName: "vcpuActivity"
                            Layout.columnSpan: panel.wide ? 2 : 1
                            Layout.fillWidth: true
                            implicitHeight: vcpuColumn.implicitHeight + 30
                            radius: 4; color: theme.colors.surface; border.color: theme.colors.border
                            ColumnLayout {
                                id: vcpuColumn
                                anchors.fill: parent; anchors.margins: 15; spacing: 10
                                RowLayout {
                                    Layout.fillWidth: true
                                    AppIcon { name: "cpu"; color: theme.colors.accent; width: 16; height: 16 }
                                    Label { text: "Per-vCPU activity"; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); font.weight: Font.Medium; Layout.fillWidth: true }
                                    Label { text: panel.vcpuRows.length ? panel.vcpuRows.length + " online" : ""; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale) }
                                }
                                Label {
                                    visible: panel.vcpuRows.length === 0
                                    text: "Per-vCPU time is not reported for this VM yet."
                                    color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); Layout.fillWidth: true
                                }
                                VcpuHeatmap {
                                    visible: panel.vcpuRows.length > 0
                                    Layout.fillWidth: true
                                    rows: panel.vcpuRows; times: panel.history.times; capacity: panel.sampleCapacity
                                }
                            }
                        }
                    }

                    AppDisclosure {
                        objectName: "vmTechnical"; title: "Technical details"; resetKey: panel.vmUuid
                        Layout.topMargin: 6
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: "Saved configuration · select a value to copy it"; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                            AppButton {
                                id: copyButton
                                objectName: "copyTechnical"
                                property bool copied: false
                                text: copied ? "Copied" : "Copy all"; iconName: copied ? "check" : "clipboard"; tone: "quiet"
                                hint: "Copy every technical detail as text"
                                onClicked: { clipboardBridge.text = panel.technicalText(); clipboardBridge.selectAll(); clipboardBridge.copy(); clipboardBridge.deselect(); copied = true; copiedReset.restart() }
                            }
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: panel.wide ? 2 : 1; columnSpacing: 28; rowSpacing: 4
                            ColumnLayout {
                                Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.alignment: Qt.AlignTop; spacing: 0
                                SectionHeading { title: "Compute & boot"; iconName: "cpu" }
                                Repeater { model: panel.computeRows; DetailRow { required property var modelData; Layout.fillWidth: true; label: modelData.label; value: modelData.value } }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.alignment: Qt.AlignTop; spacing: 0
                                SectionHeading { title: "Identity & management"; iconName: "monitor" }
                                Repeater { model: panel.identityRows; DetailRow { required property var modelData; Layout.fillWidth: true; label: modelData.label; value: modelData.value } }
                            }
                        }
                        // VMs built as part of a lab: the login they were set up with. The password stays hidden until asked for.
                        ColumnLayout {
                            id: labLogin
                            objectName: "labLogin"
                            readonly property var lab: { const revision = agent.labs.length; return panel.vmUuid ? agent.vmLab(panel.vmUuid) : ({}) }
                            property string shown: ""
                            visible: !!lab.lab
                            Layout.fillWidth: true; spacing: 0
                            Connections { target: panel; function onVmUuidChanged() { labLogin.shown = "" } }
                            SectionHeading { title: "Lab login"; caption: "Set up when the lab “" + (labLogin.lab.lab || "") + "” was built"; iconName: "keyboard" }
                            DetailRow { Layout.fillWidth: true; label: "User"; value: labLogin.lab.user || "" }
                            AppButton {
                                objectName: "exportLabFile"
                                text: "Export lab file…"; iconName: "download"; tone: "quiet"; implicitHeight: 28
                                hint: "Save this lab's networks and VMs as a file you can share and import again"
                                onClicked: labFileDialog.open()
                            }
                            FileDialog {
                                id: labFileDialog
                                title: "Save the lab as a file"
                                fileMode: FileDialog.SaveFile
                                defaultSuffix: "toml"
                                nameFilters: ["OmaWare lab files (*.toml)"]
                                currentFile: "file:" + (labLogin.lab.slug || "lab") + ".toml"
                                onAccepted: agent.exportLab(labLogin.lab.slug, selectedFile.toString())
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                DetailRow { objectName: "labPassword"; Layout.fillWidth: true; label: "Password"; value: labLogin.shown || "••••••••" }
                                AppButton {
                                    objectName: "showLabPassword"
                                    text: labLogin.shown ? "Hide" : "Show"; tone: "quiet"; implicitHeight: 28
                                    hint: "Saved as “" + (labLogin.lab.login || "") + "” in your password store"
                                    onClicked: labLogin.shown = labLogin.shown ? "" : (agent.revealPassword(labLogin.lab.login) || "(couldn't be read)")
                                }
                            }
                        }
                        SectionHeading { visible: panel.counterRows.length > 0; title: "Live counters"; caption: "Raw cumulative values from the latest sample"; iconName: "history" }
                        Repeater { model: panel.counterRows; DetailRow { required property var modelData; Layout.fillWidth: true; label: modelData.label; value: modelData.value } }
                    }
                }

                // ================= Hardware =================
                ColumnLayout {
                    visible: panel.page === 1
                    Layout.fillWidth: true
                    spacing: 12
                    RowLayout { Layout.fillWidth: true
                        Label { text: "Storage & hardware"; font.pixelSize: Math.round((20) * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true }
                        AppButton { text: "Edit hardware"; enabled: panel.canEdit && !panel.busy; onClicked: panel.editHardware() }
                        AppButton { text: "Add disk"; iconName: "plus"; enabled: panel.canEdit && !panel.busy; onClicked: panel.manageDisk({}, false) }
                    }
                    Label { visible: (panel.info.disks || []).length === 0; text: "No disks or installation media are attached."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    Repeater {
                        model: panel.info.disks || []
                        Rectangle {
                            id: diskCard
                            required property var modelData
                            readonly property var io: panel.history.disks[modelData.target] || null
                            Layout.fillWidth: true
                            implicitHeight: diskContents.implicitHeight + 28
                            radius: 4; color: theme.colors.field; border.color: theme.colors.border
                            ColumnLayout {
                                id: diskContents
                                anchors.fill: parent; anchors.margins: 14
                                spacing: 8
                                RowLayout {
                                    AppIcon { name: "disk"; color: theme.colors.muted }
                                    Label { text: modelData.device === "cdrom" ? "Optical drive" : "Disk " + modelData.target; font.weight: Font.DemiBold; Layout.fillWidth: true }
                                    Label { text: modelData.capacityBytes ? Number((modelData.capacityBytes / 1073741824).toFixed(1)) + " GiB" : ""; color: theme.colors.accent }
                                    AppButton { iconName: "close"; tone: "quiet"; hint: "Detach disk and keep its file"; enabled: panel.canEdit && !panel.busy; onClicked: panel.manageDisk(modelData, true) }
                                }
                                UsageBar {
                                    visible: !!modelData.capacityBytes && modelData.allocationBytes !== undefined
                                    Layout.fillWidth: true
                                    total: modelData.capacityBytes || 0
                                    segments: [
                                        {label: "Written to host", value: modelData.allocationBytes || 0, text: panel.bytes(modelData.allocationBytes || 0), color: theme.colors.accent},
                                        {label: "Unallocated", value: Math.max(0, (modelData.capacityBytes || 0) - (modelData.allocationBytes || 0)), text: panel.bytes(Math.max(0, (modelData.capacityBytes || 0) - (modelData.allocationBytes || 0))), color: theme.colors.border}
                                    ]
                                }
                                ColumnLayout {
                                    visible: !!panel.info.active && diskCard.io !== null && (modelData.device !== "cdrom" || !!modelData.source)
                                    Layout.fillWidth: true; spacing: 4
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { text: "Live I/O"; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); font.weight: Font.Medium; Layout.fillWidth: true }
                                        Label { text: "Read " + panel.rate(diskCard.io ? panel.latest(diskCard.io.rd) : null) + " · Write " + panel.rate(diskCard.io ? panel.latest(diskCard.io.wr) : null); color: theme.colors.foreground; font.pixelSize: Math.round(11 * theme.textScale) }
                                    }
                                    TrendChart {
                                        Layout.fillWidth: true; Layout.preferredHeight: 48
                                        times: panel.history.times; capacity: panel.sampleCapacity; minimumCeiling: 65536; format: panel.rate
                                        series: diskCard.io ? [{label: "Read", values: diskCard.io.rd, color: panel.seriesPrimary, fill: true}, {label: "Write", values: diskCard.io.wr, color: panel.seriesSecondary, dashed: true}] : []
                                    }
                                }
                                AppDisclosure {
                                    objectName: "diskDetails_" + modelData.target; title: "Disk details"; resetKey: panel.vmUuid
                                    DetailRow { Layout.fillWidth: true; label: "Source"; value: modelData.source || "No media inserted" }
                                    DetailRow { Layout.fillWidth: true; label: "Format / bus"; value: modelData.format + " · " + (modelData.bus || "Not reported") + (modelData.readOnly ? " · Read only" : "") }
                                    DetailRow { visible: modelData.allocationBytes !== undefined; Layout.fillWidth: true; label: "Host allocation"; value: panel.bytes(modelData.allocationBytes) + " of " + panel.bytes(modelData.capacityBytes) + " virtual capacity" }
                                }
                            }
                        }
                    }
                    AppDisclosure {
                        objectName: "displayDetails"; title: "Display & integration details"; resetKey: panel.vmUuid
                        SectionHeading { title: "Display & integration"; iconName: "monitor" }
                        Repeater {
                            model: [
                                {label: "Display protocol", value: (panel.info.displays || []).map(function(item) { return item.type }).join(", ") || "None"},
                                {label: "Video adapter", value: panel.value("videoModel")},
                                {label: "Video memory", value: panel.info.videoMemoryKiB ? panel.memory(panel.info.videoMemoryKiB / 1024) : "Not reported"},
                                {label: "Heads / 3D", value: panel.info.videoHeads ? panel.value("videoHeads") + " heads · 3D " + (panel.info.acceleration3d ? "enabled" : "disabled") : "Not reported"},
                                {label: "Input devices", value: panel.value("inputs") || "None"},
                                {label: "Audio device", value: panel.value("sound") || "None"},
                                {label: "Guest agent", value: !panel.info.agentConfigured ? "Not configured" : panel.info.agentConnected ? "Connected" : "Configured · not connected"},
                                {label: "Clipboard channel", value: panel.info.clipboardConfigured ? "Enabled · spice-vdagent required in guest" : "Disabled"},
                                {label: "Shared folders", value: (panel.info.shares || []).map(function(item) { return item.source + " → " + item.target }).join("\n") || "None"}
                            ]
                            DetailRow { required property var modelData; Layout.fillWidth: true; label: modelData.label; value: modelData.value }
                        }
                    }
                }

                // ================= Networks =================
                ColumnLayout {
                    visible: panel.page === 2
                    Layout.fillWidth: true
                    spacing: 14
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "Network adapters"; font.pixelSize: Math.round((20) * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true }
                        AppButton { objectName: "addAdapter"; text: "Add adapter"; iconName: "plus"; tone: "primary"; enabled: panel.canEdit && !!panel.info.persistent && !panel.busy; onClicked: panel.editAdapter({}, false) }
                    }
                    Rectangle {
                        id: containmentCard
                        objectName: "containmentCard"
                        readonly property bool on: !!panel.containment.enabled
                        readonly property var violations: panel.containment.violations || []
                        readonly property var warnings: panel.containment.warnings || []
                        Layout.fillWidth: true
                        implicitHeight: containmentColumn.implicitHeight + 30
                        radius: 4
                        color: on ? theme.colors.dangerSoft : theme.colors.surface
                        border.color: on ? theme.colors.danger : theme.colors.border
                        ColumnLayout {
                            id: containmentColumn
                            anchors.fill: parent; anchors.margins: 15; spacing: 8
                            RowLayout {
                                Layout.fillWidth: true; spacing: 10
                                AppIcon { name: "shield"; color: containmentCard.on ? theme.colors.danger : theme.colors.accent; width: 20; height: 20 }
                                Label { text: "Containment"; font.pixelSize: Math.round(16 * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true; elide: Text.ElideRight }
                                StatusBadge { text: containmentCard.on ? "Contained" : "Off"; stateCode: containmentCard.on ? 6 : 5 }
                                AppButton {
                                    objectName: "containmentToggle"
                                    text: containmentCard.on ? "Remove…" : "Contain VM"; iconName: "shield"
                                    tone: containmentCard.on ? "quiet" : "primary"
                                    enabled: panel.canEdit && !panel.busy && (containmentCard.on || containmentCard.violations.length === 0)
                                    hint: !containmentCard.on && containmentCard.violations.length ? "Fix the items below first" : ""
                                    onClicked: { if (containmentCard.on) releaseDialog.open(); else panel.setContainment(true) }
                                }
                            }
                            Label {
                                text: containmentCard.on ? "This VM can only use switches that pass live isolation checks, with no shared folders, clipboard sharing, USB/PCI passthrough or network-exposed console. OmaWare refuses to start, resume or revert it otherwise."
                                    : "For running untrusted software, such as unknown downloads or security testing. Containment limits this VM to verified isolated switches (or no network) and blocks shared folders, clipboard sharing, device passthrough and network-exposed consoles. OmaWare enforces it on every start, resume, revert and clone."
                                color: theme.colors.muted; font.pixelSize: Math.round(12 * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true
                            }
                            Label { visible: containmentCard.violations.length > 0; text: containmentCard.on ? "Blocking start:" : "Fix before containing:"; color: theme.colors.danger; font.weight: Font.DemiBold; font.pixelSize: Math.round(12 * theme.textScale) }
                            Repeater {
                                model: containmentCard.violations
                                RowLayout { required property string modelData; Layout.fillWidth: true; spacing: 8
                                    Label { text: "✗"; color: theme.colors.danger; font.weight: Font.Bold; Layout.alignment: Qt.AlignTop }
                                    Label { text: modelData; textFormat: Text.PlainText; color: theme.colors.foreground; font.pixelSize: Math.round(12 * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true } }
                            }
                            RowLayout { visible: containmentCard.violations.length === 0; spacing: 8
                                Label { text: "✓"; color: theme.colors.success; font.weight: Font.Bold }
                                Label { text: "The current configuration meets the containment policy."; color: theme.colors.foreground; font.pixelSize: Math.round(12 * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true } }
                            Repeater {
                                model: containmentCard.warnings
                                RowLayout { required property string modelData; Layout.fillWidth: true; spacing: 8
                                    Label { text: "!"; color: theme.colors.warning; font.weight: Font.Bold; Layout.alignment: Qt.AlignTop }
                                    Label { text: modelData; textFormat: Text.PlainText; color: theme.colors.warning; font.pixelSize: Math.round(12 * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true } }
                            }
                            Label {
                                text: "Not covered: containment controls networking and host integration, not QEMU itself. User-session VMs run as your account, so a hypervisor escape would reach your files. Run high-risk software on a dedicated machine, and revert to a clean snapshot afterwards."
                                color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true
                            }
                        }
                    }
                    AppDialog {
                        id: releaseDialog; objectName: "releaseContainment"
                        heading: "Remove containment?"; headerIcon: "shield"
                        width: Math.min(460, parent ? parent.width - 40 : 460)
                        contentItem: ColumnLayout { spacing: 14
                            Label { text: "This VM may hold untrusted software. Without containment, it can be given internet access, shared folders or clipboard sharing, and OmaWare will no longer block unsafe starts."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                            RowLayout { Item { Layout.fillWidth: true }
                                AppButton { text: "Keep contained"; onClicked: releaseDialog.reject() }
                                AppButton { objectName: "confirmRelease"; text: "Remove containment"; tone: "danger"; onClicked: { panel.setContainment(false); releaseDialog.accept() } } }
                        }
                    }
                    Label { text: panel.info.active ? "Changes apply to the running VM straight away when its OS supports it. You can also connect VMs on the Networks map." : "Changes apply when you start this VM. You can also connect VMs on the Networks map."; color: theme.colors.muted; font.pixelSize: Math.round((12) * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    Rectangle {
                        visible: !!panel.info.pendingNetworkChanges
                        Layout.fillWidth: true
                        implicitHeight: pendingContents.implicitHeight + 24
                        color: theme.colors.accentSoft; radius: 4
                        ColumnLayout {
                            id: pendingContents
                            anchors.fill: parent; anchors.margins: 12
                            spacing: 6
                            Label { text: "Changes pending · the running VM still uses:"; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                            Label { text: panel.runtimeText(); font.pixelSize: Math.round((11) * theme.textScale); color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                        }
                    }
                    Label { visible: (panel.info.interfaces || []).length === 0; text: "No network adapters configured. Add one to connect this VM."; color: theme.colors.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    Repeater {
                        model: panel.info.interfaces || []
                        Rectangle {
                            id: nicCard
                            required property var modelData
                            required property int index
                            readonly property string target: panel.info.active ? panel.liveTarget(modelData) : ""
                            readonly property var io: target !== "" ? panel.history.nics[target] || null : null
                            readonly property var counters: target !== "" ? panel.find(panel.live.nets, "name", target) : null
                            Layout.fillWidth: true
                            implicitHeight: nicContents.implicitHeight + 28
                            radius: 4; color: theme.colors.field; border.color: theme.colors.border
                            ColumnLayout {
                                id: nicContents
                                anchors.fill: parent; anchors.margins: 14
                                spacing: 6
                                RowLayout {
                                    AppIcon { name: "network"; color: theme.colors.accent }
                                    Label { text: "Adapter " + (index + 1); font.weight: Font.DemiBold; Layout.fillWidth: true }
                                    AppButton { text: "Edit"; tone: "quiet"; enabled: panel.canEdit && modelData.editable && !panel.busy; onClicked: panel.editAdapter(modelData, false) }
                                    AppButton { iconName: "close"; tone: "quiet"; hint: "Remove this adapter"; enabled: panel.canEdit && !panel.busy; onClicked: panel.editAdapter(modelData, true) }
                                }
                                DetailRow { Layout.fillWidth: true; label: "Network"; value: panel.networkName(modelData) }
                                DetailRow { Layout.fillWidth: true; label: "At startup"; value: modelData.linkUp ? "Connected" : "Cable disconnected" }
                                ColumnLayout {
                                    visible: nicCard.io !== null
                                    Layout.fillWidth: true; Layout.topMargin: 4; spacing: 4
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { text: "Live traffic · " + nicCard.target; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); font.weight: Font.Medium; Layout.fillWidth: true }
                                        Label { text: "↓ " + panel.rate(nicCard.io ? panel.latest(nicCard.io.rx) : null) + "   ↑ " + panel.rate(nicCard.io ? panel.latest(nicCard.io.tx) : null); color: theme.colors.foreground; font.pixelSize: Math.round(11 * theme.textScale) }
                                    }
                                    TrendChart {
                                        Layout.fillWidth: true; Layout.preferredHeight: 48
                                        times: panel.history.times; capacity: panel.sampleCapacity; minimumCeiling: 16384; format: panel.rate
                                        series: nicCard.io ? [{label: "Received", values: nicCard.io.rx, color: panel.seriesPrimary, fill: true}, {label: "Sent", values: nicCard.io.tx, color: panel.seriesSecondary, dashed: true}] : []
                                    }
                                    Label {
                                        visible: nicCard.counters !== null
                                        text: nicCard.counters ? "Since start · " + panel.bytes(nicCard.counters.rx_bytes) + " received · " + panel.bytes(nicCard.counters.tx_bytes) + " sent" : ""
                                        color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap
                                    }
                                }
                                AppDisclosure {
                                    objectName: "adapterDetails_" + index; title: "Adapter details"; resetKey: panel.vmUuid
                                    DetailRow { Layout.fillWidth: true; label: "Model"; value: modelData.model }
                                    DetailRow { Layout.fillWidth: true; label: "MAC address"; value: modelData.mac }
                                    DetailRow { visible: nicCard.target !== ""; Layout.fillWidth: true; label: "Host device"; value: nicCard.target }
                                    DetailRow { visible: nicCard.counters !== null; Layout.fillWidth: true; label: "Packets"; value: nicCard.counters ? panel.tally(nicCard.counters.rx_pkts) + " received · " + panel.tally(nicCard.counters.tx_pkts) + " sent · " + ((nicCard.counters.rx_errs || 0) + (nicCard.counters.tx_errs || 0)) + " errors · " + ((nicCard.counters.rx_drop || 0) + (nicCard.counters.tx_drop || 0)) + " drops" : "" }
                                }
                                Label { visible: !modelData.editable; text: "This adapter has advanced settings; source editing is unavailable."; color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                            }
                        }
                    }
                    AppDisclosure {
                        objectName: "guestNetworkDetails"; title: "IP addresses & diagnostics"; resetKey: panel.vmUuid
                        RowLayout {
                            Layout.topMargin: 8
                            Label { text: "Guest IP addresses"; font.pixelSize: Math.round((17) * theme.textScale); font.weight: Font.DemiBold; Layout.fillWidth: true }
                            AppButton { text: "Refresh guest IPs"; tone: "quiet"; enabled: !panel.busy && !!panel.info.agentConnected; onClicked: panel.reload(true) }
                        }
                        Label { text: panel.value("addressStatus"); color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        Repeater {
                            model: panel.info.guestAddresses || []
                            DetailRow { required property var modelData; Layout.fillWidth: true; label: modelData.name; value: modelData.addresses || "No address reported" }
                        }
                        Label { text: panel.value("networkStatus"); color: theme.colors.muted; font.pixelSize: Math.round((11) * theme.textScale); Layout.fillWidth: true; wrapMode: Text.WordWrap; Layout.topMargin: 10 }
                    }
                }
                SnapshotsPage { id: snapshotView; objectName: "snapshotsPage"; visible: panel.page === 3; Layout.fillWidth: true; info: panel.info; preview: panel.checkpointPreview; viewportHeight: scroll.height }
            }
        }
    }
}
