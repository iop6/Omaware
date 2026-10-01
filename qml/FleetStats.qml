// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Rolling per-VM and host history derived from consecutive `stats.all` samples.
QtObject {
    id: fleet
    property var sample: ({})
    property int capacity: 60
    property var current: ({})   // uuid -> {cpu, memUsedMiB, memTotalMiB, rssMiB, rd, wr, rx, tx, uptime, vcpus}
    property var history: ({})   // uuid -> {cpu: [], mem: []}
    property var host: ({})      // {cpu, memUsedMiB, memTotalMiB, cpus}
    property var hostCpu: []
    property var hostMem: []
    property var times: []
    property var last: null
    readonly property bool ready: times.length > 0

    function append(list, value) { return (list || []).concat([value]).slice(-capacity) }
    function rate(now, before, seconds) { return now === undefined || before === undefined || now < before || seconds <= 0 ? null : (now - before) / seconds }
    function ingest(s) {
        if (!s || !s.host || !s.host.sampledAt) return
        const prev = last
        if (prev && s.host.sampledAt <= prev.host.sampledAt) return
        const dt = prev ? (s.host.sampledAt - prev.host.sampledAt) / 1000 : 0
        const fresh = !!prev && dt <= 20
        const before = {}
        if (prev) for (const vm of prev.vms || []) before[vm.uuid] = vm
        const nextCurrent = {}, nextHistory = {}
        for (const vm of s.vms || []) {
            const p = fresh ? before[vm.uuid] : undefined
            const busy = p ? rate(vm.cpuTime, p.cpuTime, dt) : null
            const available = vm.availableKiB, unused = vm.unusedKiB
            const row = {
                name: vm.name, vcpus: vm.vcpus || 1, uptime: vm.uptimeSeconds,
                cpu: busy === null ? null : Math.min(100, busy / 1e7 / Math.max(1, vm.vcpus || 1)),
                memUsedMiB: available !== undefined && unused !== undefined && unused <= available ? (available - unused) / 1024 : null,
                memTotalMiB: (available || vm.balloonKiB || 0) / 1024,
                rssMiB: vm.rssKiB !== undefined ? vm.rssKiB / 1024 : null,
                rd: p ? rate(vm.rdBytes, p.rdBytes, dt) : null, wr: p ? rate(vm.wrBytes, p.wrBytes, dt) : null,
                rx: p ? rate(vm.rxBytes, p.rxBytes, dt) : null, tx: p ? rate(vm.txBytes, p.txBytes, dt) : null
            }
            nextCurrent[vm.uuid] = row
            const old = history[vm.uuid] || {}
            const pad = function(list) { return list || Array(times.length).fill(null) }
            nextHistory[vm.uuid] = {cpu: append(pad(old.cpu), row.cpu), mem: append(pad(old.mem), row.memUsedMiB !== null ? row.memUsedMiB : row.rssMiB)}
        }
        const h = s.host, ph = prev ? prev.host : null
        const busyNs = function(x) { return (x.cpu_kernel || 0) + (x.cpu_user || 0) + (x.cpu_iowait || 0) }
        let cpu = null
        if (fresh && ph && h.cpu_idle !== undefined) {
            const total = busyNs(h) + h.cpu_idle - busyNs(ph) - ph.cpu_idle
            if (total > 0) cpu = Math.min(100, Math.max(0, (busyNs(h) - busyNs(ph)) / total * 100))
        }
        const totalKiB = h.mem_total || h.memoryKiB || 0
        const usedKiB = h.mem_total !== undefined ? h.mem_total - (h.mem_free || 0) - (h.mem_buffers || 0) - (h.mem_cached || 0) : null
        host = {cpu: cpu, cpus: h.cpus || 0, memTotalMiB: totalKiB / 1024, memUsedMiB: usedKiB === null ? null : usedKiB / 1024}
        hostCpu = append(hostCpu, cpu)
        hostMem = append(hostMem, host.memUsedMiB)
        times = append(times, h.sampledAt)
        current = nextCurrent
        history = nextHistory
        last = s
    }
    onSampleChanged: ingest(sample)
}
