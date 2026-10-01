// SPDX-License-Identifier: GPL-3.0-or-later
import QtQml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Network map in the spirit of Packet Tracer. Devices are draggable cards and every VM adapter is a
// cable: drag from a VM's port onto a network to connect it, pull or plug cables live, and see at a
// glance which VMs can reach the internet. Layout is a per-user preference; nothing here writes host
// state except through the same backend operations the Details and Networks pages use.
Item {
    id: topo
    objectName: "networkTopology"
    property var catalog: ({})
    property var fleet: null
    property var domains: backend.domains
    signal openVm(string uuid)
    signal vmActions(var vm, var item, real px, real py)
    signal editNetwork(var network)
    signal createNetwork(string mode, var vms)
    signal networkAction(var network, string verb)
    signal refreshRequested()

    component Chip: Rectangle {
        color: theme.colors.surface; border.color: theme.colors.border; radius: 8
        opacity: .97
    }
    readonly property var graph: build(catalog, domains)
    property var nodeIds: []
    property var cableIds: []
    property var positions: ({})
    property var items: ({})
    property int registry: 0
    property string selected: ""
    property string selectedCable: ""
    property real zoom: 1
    property real panX: 40
    property real panY: 40
    property var wire: null            // {from, x, y, over} while a new cable is being dragged
    property var placement: null       // world position for a switch that is being created
    property string notice: ""
    property bool noticeOk: true
    property bool inspectorOpen: preferences.get("topologyInspector", true) !== false && preferences.get("topologyInspector", true) !== "false"
    function setInspector(open) { inspectorOpen = open; preferences.set("topologyInspector", open); if (!userView) fitLater.restart() }
    property bool userView: false      // once the user pans or zooms, stop auto-fitting

    readonly property var sizes: ({internet: [210, 60], host: [236, 72], "switch": [246, 84], vm: [224, 84]})
    readonly property var reachInfo: ({
        internet: {label: "INTERNET", title: "Online", text: "Can reach the internet", ink: theme.colors.warning},
        lan: {label: "LOCAL NETWORK", title: "On your local network", text: "Can reach your local network, and usually the internet", ink: theme.colors.warning},
        host: {label: "THIS COMPUTER", title: "This computer only", text: "Can reach this computer, but not the internet", ink: theme.colors.accent},
        lab: {label: "VMS ONLY", title: "Other VMs only", text: "Can only reach other VMs on its network", ink: theme.colors.success},
        none: {label: "OFFLINE", title: "Offline", text: "Not connected to anything", ink: theme.colors.muted}
    })
    // Color of what a network leads to: amber internet or LAN, blue this computer, green VMs only.
    function uplinkInk(uplink) {
        return uplink === "nat" || uplink === "routed" || uplink === "lan" || uplink === "internet" ? theme.colors.warning
            : uplink === "host" ? theme.colors.accent : uplink === "none" ? theme.colors.success : theme.colors.muted
    }
    function kindText(sw) {
        return !sw ? "" : sw.uplink === "nat" ? "Internet + VMs" : sw.uplink === "routed" ? "Routed network" : sw.uplink === "host" ? "This computer + VMs"
            : sw.uplink === "none" ? "VMs only" : sw.uplink === "lan" ? "Your local network" : "Other network"
    }
    readonly property var rank: ({none: 0, lab: 1, host: 2, lan: 3, internet: 4})

    // Theme colors are strings; this returns the same color with an alpha channel.
    function tint(c, a) { const q = Qt.lighter(c, 1); return Qt.rgba(q.r, q.g, q.b, a) }
    function vmName(name) { return String(name || "").replace(/^omaware-/, "") }
    function bridgeFor(uuid) { return "bridge:oma" + String(uuid).replace(/-/g, "").slice(0, 8) }

    // ---- Graph model --------------------------------------------------------------------------
    function makeSwitch(id, choice, net) {
        const category = choice ? choice.category : "Unknown"
        const uplink = category === "Shared NAT" || category === "NAT" ? "nat" : category === "Routed" || category === "Open" ? "routed"
            : category === "Host-only" ? "host" : category === "Bridged" ? "lan" : category === "Isolated" ? "none" : "unknown"
        const reason = choice ? String(choice.reason || "") : "This network is not known to OmaWare."
        const name = net ? (net.title || vmName(net.name)) : choice ? String(choice.displayName || choice.label || id) : id.replace(/^[a-z]+:/, "")
        return {id: id, kind: "switch", label: name, category: category, uplink: uplink, network: net, needsPermission: !!choice && !!choice.needsPermission,
            description: choice ? String(choice.description || "") : "", vmCount: 0,
            running: net ? !!net.active : !/stopped|not active/i.test(reason), usable: !!choice && !!choice.available, reason: reason,
            cidr: net ? net.cidr || "" : "", bridge: choice ? choice.source || "" : "", managed: !!net && !!net.managed,
            isolation: (net && net.isolation) || (choice && choice.isolation) || null,
            sealed: category === "Isolated" && !!((net && net.isolation) || (choice && choice.isolation) || {}).isolated}
    }
    function build(cat, doms) {
        const choices = cat.choices || [], nets = cat.items || [], vms = cat.topology || []
        let byId = {internet: {id: "internet", kind: "internet", label: "Internet"}, host: {id: "host", kind: "host", label: "This computer"}}
        let order = ["internet", "host"]
        const addSwitch = function(id, choice, net) { if (!byId[id]) { byId[id] = makeSwitch(id, choice, net); order.push(id) } }
        for (const c of choices) {
            if (!c.id || c.id === "user" || c.id.indexOf("unavailable:") === 0) continue
            addSwitch(c.id, c, c.kind === "bridge" ? nets.find(function(n) { return n.bridge === c.source }) || null : null)
        }
        const target = function(networkId) { if (networkId === "user") return "host"; addSwitch(networkId, null, null); return networkId }
        let cables = []
        for (const vm of vms) {
            const dom = doms.find(function(d) { return d.uuid === vm.uuid }) || {}
            const id = "vm:" + vm.uuid, live = !!vm.active, addresses = vm.addresses || {}
            const savedNics = vm.interfaces || [], current = live ? (vm.liveInterfaces || []) : savedNics
            const savedByMac = {}
            for (const nic of savedNics) savedByMac[String(nic.mac).toLowerCase()] = nic
            let seen = {}
            for (const nic of current) {
                const key = String(nic.mac).toLowerCase(), saved = savedByMac[key]
                seen[key] = true
                cables.push({id: vm.uuid + "/" + key, vm: id, uuid: vm.uuid, mac: nic.mac, to: target(nic.networkId), networkId: nic.networkId,
                    up: !!nic.linkUp, live: live, model: nic.model, state: live && !saved ? "removing" : "current", addresses: addresses[key] || []})
                if (live && saved && saved.networkId !== nic.networkId)
                    cables.push({id: vm.uuid + "/" + key + "/next", vm: id, uuid: vm.uuid, mac: nic.mac, to: target(saved.networkId), networkId: saved.networkId,
                        up: !!saved.linkUp, live: false, model: saved.model, state: "next"})
            }
            if (live) for (const nic of savedNics) if (!seen[String(nic.mac).toLowerCase()])
                cables.push({id: vm.uuid + "/" + String(nic.mac).toLowerCase() + "/next", vm: id, uuid: vm.uuid, mac: nic.mac, to: target(nic.networkId),
                    networkId: nic.networkId, up: !!nic.linkUp, live: false, model: nic.model, state: "next"})
            const mine = cables.filter(function(c) { return c.vm === id && c.state === "current" })
            const ips = [].concat.apply([], mine.map(function(c) { return c.addresses }))
            byId[id] = {id: id, kind: "vm", uuid: vm.uuid, label: dom.name ? vmName(dom.name) : vmName(vm.name), running: live,
                ip: ips.length ? ips[0] + (ips.length > 1 ? " +" + (ips.length - 1) : "") : "", ips: ips,
                privateOnly: mine.some(function(c) { return c.to === "host" && c.up }),
                stateCode: dom.stateCode || (live ? 1 : 5), state: dom.state || (live ? "Running" : "Shut off"), owned: !!dom.owned,
                contained: !!dom.contained, revision: vm.revision || "", interfaces: savedNics, vm: dom}
            order.push(id)
        }
        // Parallel cables between the same pair get their own lane so both stay visible.
        let lanes = {}
        for (const c of cables) { const k = c.vm + ">" + c.to; lanes[k] = (lanes[k] || []).concat([c]) }
        for (const k in lanes) lanes[k].forEach(function(c, i) { c.lane = i - (lanes[k].length - 1) / 2 })
        let cableById = {}
        for (const c of cables) cableById[c.id] = c
        // Reach: the best path out through cables that are plugged in now (or at next start for stopped VMs).
        const switchReach = function(sw) {
            if (!sw || !sw.running) return "none"
            return sw.uplink === "nat" || sw.uplink === "routed" ? "internet" : sw.uplink === "lan" || sw.uplink === "unknown" ? "lan" : sw.uplink === "host" ? "host" : "lab"
        }
        let exposed = 0, sealed = 0
        for (const id of order) {
            const node = byId[id]
            if (node.kind !== "vm") continue
            let best = "none", via = []
            for (const c of cables) {
                if (c.vm !== id || c.state === "next" || !c.up) continue
                const r = c.to === "host" ? "internet" : switchReach(byId[c.to])
                const route = c.to === "host" ? "Private connection → this computer → internet"
                    : byId[c.to].label + (r === "internet" ? " → this computer → internet" : r === "lan" ? " → your local network" : r === "host" ? " → this computer" : " → other VMs on it")
                via.push(route)
                if (rank[r] > rank[best]) best = r
            }
            node.reach = best; node.via = via
            if (node.running && (best === "internet" || best === "lan")) exposed++
            if (node.running && (best === "none" || best === "lab")) sealed++
        }
        byId.internet.exposed = exposed
        for (const c of cables) if (c.state !== "next" && byId[c.to] && byId[c.to].kind === "switch") byId[c.to].vmCount++
        let uplinks = [{id: "host>internet", from: "host", to: "internet", kind: "wan", live: true}]
        for (const id of order) {
            const sw = byId[id]
            if (sw.kind !== "switch") continue
            if (sw.uplink === "nat" || sw.uplink === "routed" || sw.uplink === "host")
                uplinks.push({id: id + ">host", from: id, to: "host", kind: sw.uplink, live: sw.running})
            else if (sw.uplink === "lan" || sw.uplink === "unknown") uplinks.push({id: id + ">internet", from: id, to: "internet", kind: "lan", live: sw.running})
        }
        return {byId: byId, order: order, cables: cables, cableById: cableById, uplinks: uplinks, exposed: exposed, sealed: sealed}
    }
    onGraphChanged: {
        // Keep delegates alive across refreshes: only a changed set of ids recreates them.
        if (graph.order.join("|") !== nodeIds.join("|")) nodeIds = graph.order.slice()
        const cids = graph.cables.map(function(c) { return c.id })
        if (cids.join("|") !== cableIds.join("|")) cableIds = cids
        if (selected && !graph.byId[selected]) selected = ""
        if (selectedCable && !graph.cableById[selectedCable]) selectedCable = ""
        cables.requestPaint()
        if (!userView) fitLater.restart()
    }

    // ---- Layout -------------------------------------------------------------------------------
    // Tiers from top to bottom: the internet, this computer, networks, then each network's VMs beneath it.
    function autoLayout() {
        const g = graph
        let groups = [{key: "host", vms: []}], index = {host: 0}
        const ranked = g.order.filter(function(id) { return g.byId[id].kind === "switch" }).sort(function(a, b) {
            const w = {nat: 0, routed: 1, lan: 2, unknown: 3, host: 4, none: 5}
            return w[g.byId[a].uplink] - w[g.byId[b].uplink] || g.byId[a].label.localeCompare(g.byId[b].label)
        })
        for (const id of ranked) { index[id] = groups.length; groups.push({key: id, vms: []}) }
        groups.push({key: "", vms: []})
        for (const id of g.order) {
            if (g.byId[id].kind !== "vm") continue
            const first = g.cables.find(function(c) { return c.vm === id && c.state !== "next" && c.up }) || g.cables.find(function(c) { return c.vm === id && c.state !== "next" })
            groups[first ? index[first.to] : groups.length - 1].vms.push(id)
        }
        let result = {}, cursor = 0
        // With several groups, VMs stack in single columns so the map stays narrow enough to read.
        const busy = groups.filter(function(group) { return group.vms.length > 0 || (group.key && group.key !== "host") }).length
        const colW = sizes.vm[0] + 26, rowH = sizes.vm[1] + 34, perRow = busy >= 3 ? 1 : 2
        let hostSpan = [Infinity, -Infinity]
        for (const group of groups) {
            if (group.key === "" && group.vms.length === 0) continue
            const cols = Math.max(1, Math.min(perRow, group.vms.length))
            const width = Math.max(cols * colW, sizes["switch"][0] + 26), center = cursor + width / 2
            if (group.key && group.key !== "host") {
                result[group.key] = {x: center - sizes["switch"][0] / 2, y: 250}
                if (g.byId[group.key].uplink !== "none" && g.byId[group.key].uplink !== "lan") { hostSpan[0] = Math.min(hostSpan[0], center); hostSpan[1] = Math.max(hostSpan[1], center) }
            }
            group.vms.forEach(function(id, i) {
                const col = i % perRow, row = Math.floor(i / perRow), rowCount = Math.min(perRow, group.vms.length - row * perRow)
                const rowStart = center - rowCount * colW / 2
                result[id] = {x: rowStart + col * colW + (colW - sizes.vm[0]) / 2, y: (group.key === "host" ? 250 : 420) + row * rowH}
            })
            if (group.key === "host") { hostSpan[0] = Math.min(hostSpan[0], center); hostSpan[1] = Math.max(hostSpan[1], center) }
            cursor += width + 44
        }
        const hostX = hostSpan[0] === Infinity ? cursor / 2 : (hostSpan[0] + hostSpan[1]) / 2
        result.host = {x: hostX - sizes.host[0] / 2, y: 100}
        result.internet = {x: hostX - sizes.internet[0] / 2, y: -40}
        for (const key in result) result[key] = {x: Math.round(result[key].x / 10) * 10, y: Math.round(result[key].y / 10) * 10}
        return result
    }
    function placeOf(id) {
        if (positions[id]) return positions[id]
        const auto = autoLayout()
        return auto[id] || {x: 0, y: 0}
    }
    function savePositions() { preferences.set("topologyLayout", JSON.stringify(positions)) }
    function remember(id, item) {
        let next = Object.assign({}, positions)
        next[id] = {x: Math.round(item.x / 10) * 10, y: Math.round(item.y / 10) * 10}
        item.x = next[id].x; item.y = next[id].y
        positions = next; savePositions()
    }
    function refit() { userView = false; fit() }
    function arrange() {
        positions = autoLayout(); savePositions()
        for (const id in items) if (positions[id]) { items[id].x = positions[id].x; items[id].y = positions[id].y }
        fit()
    }
    function center(id) {
        const it = items[id]
        return it ? {x: it.x + it.width / 2, y: it.y + it.height / 2} : null
    }
    // Where cables attach: the top middle of a VM, the bottom middle of anything it connects up to.
    function topOf(id, offset) { const it = items[id]; return it ? {x: it.x + it.width / 2 + (offset || 0), y: it.y} : null }
    function bottomOf(id, offset) { const it = items[id]; return it ? {x: it.x + it.width / 2 + (offset || 0), y: it.y + it.height} : null }
    // Cables are polylines sampled from smooth curves; `cum` holds the running length for point().
    function path(pts) {
        let cum = [0]
        for (let i = 1; i < pts.length; ++i) cum.push(cum[i - 1] + Math.hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y))
        return {pts: pts, cum: cum, len: Math.max(1, cum[cum.length - 1])}
    }
    function bezier(a, c1, c2, b, steps) {
        let pts = []
        for (let i = 0; i <= steps; ++i) {
            const t = i / steps, u = 1 - t
            pts.push({x: u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x,
                      y: u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y})
        }
        return pts
    }
    // A smooth S-shaped cable between two points.
    function curve(a, b) {
        if (!a || !b) return null
        const bend = Math.max(40, Math.abs(a.y - b.y) * .5), up = a.y >= b.y
        return path(bezier(a, {x: a.x, y: a.y + (up ? -bend : bend)}, {x: b.x, y: b.y + (up ? bend : -bend)}, b, 28))
    }
    function point(g, t) {
        const want = Math.max(0, Math.min(1, t)) * g.len
        let i = 1
        while (i < g.cum.length - 1 && g.cum[i] < want) ++i
        const span = Math.max(1e-6, g.cum[i] - g.cum[i - 1]), f = (want - g.cum[i - 1]) / span
        return {x: g.pts[i - 1].x + (g.pts[i].x - g.pts[i - 1].x) * f, y: g.pts[i - 1].y + (g.pts[i].y - g.pts[i - 1].y) * f}
    }
    // Devices a straight run between two points would pass behind.
    function blockers(a, b, skip) {
        let found = []
        for (const id in items) {
            if (skip.indexOf(id) >= 0) continue
            const it = items[id]
            for (let i = 1; i < 12; ++i) {
                const x = a.x + (b.x - a.x) * i / 12, y = a.y + (b.y - a.y) * i / 12
                if (x > it.x - 6 && x < it.x + it.width + 6 && y > it.y - 6 && y < it.y + it.height + 6) { found.push(it); break }
            }
        }
        return found
    }
    // A cable runs from the VM's port up to the bottom of its network. When another device sits in the
    // way (VMs stacked in a column), it leaves from the VM's side and runs up a gutter instead.
    function geometry(c) {
        const a = topOf(c.vm, c.lane * 16), b = bottomOf(c.to, c.lane * 16), vm = items[c.vm]
        if (!a || !b || !vm) return null
        const inWay = blockers(a, b, [c.vm, c.to])
        if (inWay.length === 0) return curve(a, b)
        const left = b.x <= vm.x + vm.width / 2
        const gutter = left ? Math.min.apply(null, inWay.map(function(it) { return it.x }).concat([vm.x])) - 14 - inWay.length * 7 + c.lane * 5
                            : Math.max.apply(null, inWay.map(function(it) { return it.x + it.width }).concat([vm.x + vm.width])) + 14 + inWay.length * 7 + c.lane * 5
        const start = {x: left ? vm.x : vm.x + vm.width, y: vm.y + vm.height / 2 + c.lane * 8}
        const top = Math.min.apply(null, inWay.map(function(it) { return it.y })) - 22
        let pts = bezier(start, {x: (start.x + gutter) / 2, y: start.y}, {x: gutter, y: start.y}, {x: gutter, y: start.y - 18}, 8)
        pts.push({x: gutter, y: Math.max(top, b.y + 30)})
        pts = pts.concat(bezier({x: gutter, y: Math.max(top, b.y + 30)}, {x: gutter, y: b.y + 8}, {x: b.x, y: b.y + 30}, b, 14).slice(1))
        return path(pts)
    }
    function uplinkGeometry(u) { return curve(topOf(u.from), bottomOf(u.to)) }
    function nodeAt(wx, wy) {
        for (const id in items) {
            const it = items[id]
            if (wx >= it.x && wx <= it.x + it.width && wy >= it.y && wy <= it.y + it.height) return id
        }
        return ""
    }
    function fit() {
        let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity
        for (const id in items) { const it = items[id]; x0 = Math.min(x0, it.x); y0 = Math.min(y0, it.y); x1 = Math.max(x1, it.x + it.width); y1 = Math.max(y1, it.y + it.height) }
        if (x0 === Infinity || viewport.width <= 0) return
        const pad = 48
        // Never shrink below a readable size; a bigger map is panned instead.
        zoom = Math.max(.62, Math.min(1.1, Math.min((viewport.width - pad * 2) / (x1 - x0), (viewport.height - pad * 2 - 60) / (y1 - y0))))
        panX = (viewport.width - (x1 - x0) * zoom) / 2 - x0 * zoom
        panY = (viewport.height - (y1 - y0) * zoom) / 2 - y0 * zoom + 24
        if ((x1 - x0) * zoom > viewport.width - pad) {
            // Too wide: keep this computer in the middle, where the paths meet.
            const host = items.host
            if (host) panX = viewport.width / 2 - (host.x + host.width / 2) * zoom
        }
        if ((y1 - y0) * zoom > viewport.height - pad - 60) panY = pad + 30 - y0 * zoom
    }
    function zoomAt(px, py, next) {
        userView = true
        next = Math.max(.3, Math.min(2.5, next))
        panX = px - (px - panX) * next / zoom; panY = py - (py - panY) * next / zoom; zoom = next
    }
    Timer { id: fitLater; interval: 80; onTriggered: if (topo.visible && viewport.width > 0) topo.fit() }
    onVisibleChanged: if (visible && !userView) fitLater.restart()
    Component.onCompleted: {
        try { positions = JSON.parse(preferences.get("topologyLayout", "{}")) || {} } catch (e) { positions = {} }
    }

    // ---- Actions ------------------------------------------------------------------------------
    function say(message, ok) { notice = message; noticeOk = ok; noticeTimer.restart() }
    Timer { id: noticeTimer; interval: 6500; onTriggered: topo.notice = "" }
    Connections {
        target: backend
        function onLinksSet(ok, message) { topo.say(message, ok); topo.refreshRequested() }
        function onNetworkConfigured(uuid, ok, message) { topo.say(message, ok); topo.refreshRequested() }
    }
    function setLinks(cableList, up) {
        // Cables waiting for the next start can be pulled too, so they never come up connected.
        let seen = {}, targets = []
        for (const c of cableList) {
            const key = c.uuid + "/" + String(c.mac).toLowerCase()
            if ((c.state === "next" && up) || c.up === up || !graph.byId[c.vm].owned || seen[key]) continue
            seen[key] = true; targets.push({uuid: c.uuid, mac: c.mac})
        }
        if (targets.length === 0) { say(up ? "Those cables are already plugged in." : "Those cables are already pulled.", true); return }
        backend.setLinks(targets, up)
    }
    // Cuts every cable with a path to the internet or a LAN, running or not, so nothing reconnects at next start.
    function killInternet() {
        const out = graph.cables.filter(function(c) {
            if (!c.up) return false
            if (c.to === "host") return true
            const sw = graph.byId[c.to]
            return sw && sw.uplink !== "none" && sw.uplink !== "host"
        })
        if (out.length === 0) { say("No VM has a cable with a path to the internet.", true); return }
        setLinks(out, false)
    }
    function cableTo(vmId, targetId, existingMac) {
        const vm = graph.byId[vmId], dest = graph.byId[targetId]
        if (!vm || !dest) return
        const networkId = targetId === "host" ? "user" : targetId
        if (!vm.owned) { say("Only VMs OmaWare created can be connected here.", false); return }
        if (dest.kind === "switch" && !dest.usable) { say(dest.label + ": " + (dest.reason || "this network can't be used right now."), false); return }
        if (vm.contained && !(dest.kind === "switch" && dest.sealed)) { say(vm.label + " is contained. It can only join a verified “VMs only” network.", false); return }
        const nic = existingMac ? vm.interfaces.find(function(n) { return n.mac === existingMac }) : null
        const ok = backend.configureNetwork(vm.uuid, nic ? nic.mac : "", networkId, nic ? (nic.model === "default" ? "virtio" : nic.model) : "virtio", nic ? nic.linkUp : true, false, vm.revision)
        if (!ok) say("Another operation is in progress. Try again when it finishes.", false)
        else say((nic ? "Moving " : "Connecting ") + vm.label + " to " + (dest.kind === "host" ? "a private internet connection" : dest.label) + "…", true)
    }
    function dropWire(vmId, targetId) {
        const vm = graph.byId[vmId], dest = graph.byId[targetId]
        if (!dest || (dest.kind !== "switch" && dest.kind !== "host")) return
        if (vm.interfaces.length === 0) { cableTo(vmId, targetId, ""); return }
        dropMenu.vmId = vmId; dropMenu.targetId = targetId
        const p = world.mapToItem(topo, wireEnd.x, wireEnd.y)
        dropMenu.popup(topo, p.x, p.y)
    }
    function removeAdapter(c) {
        const vm = graph.byId[c.vm]
        if (!backend.configureNetwork(vm.uuid, c.mac, "", "virtio", true, true, vm.revision)) say("Another operation is in progress. Try again when it finishes.", false)
    }
    function targetName(networkId) {
        if (networkId === "user") return "the private internet connection"
        const sw = graph.byId[networkId]
        return sw ? sw.label : "another network"
    }
    function switchesFor(vm) {
        let list = [{id: "host", label: "Internet · private to this VM", ok: !vm.contained}]
        for (const id of graph.order) {
            const sw = graph.byId[id]
            if (sw.kind === "switch") list.push({id: id, label: sw.label + " · " + kindText(sw).toLowerCase(), ok: sw.usable && (!vm.contained || sw.sealed)})
        }
        return list
    }

    // ---- Canvas -------------------------------------------------------------------------------
    Rectangle {
        id: viewport
        objectName: "topologyViewport"
        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
        anchors.right: inspector.visible ? inspector.left : parent.right
        anchors.rightMargin: inspector.visible ? 12 : 0
        color: theme.colors.background
        border.color: theme.colors.border
        radius: 10
        clip: true
        onWidthChanged: { cables.requestPaint(); if (!topo.userView) fitLater.restart() }
        onHeightChanged: { cables.requestPaint(); if (!topo.userView) fitLater.restart() }

        Canvas {
            id: cables
            anchors.fill: parent
            renderStrategy: Canvas.Cooperative
            // Samples a cable's curve so it can be stroked solid or dashed (Canvas has no line dashes).
            function stroke(ctx, g, dash) {
                if (!g) return
                const pts = g.pts
                ctx.beginPath()
                if (!dash) {
                    ctx.moveTo(pts[0].x, pts[0].y)
                    for (let i = 1; i < pts.length; ++i) ctx.lineTo(pts[i].x, pts[i].y)
                    ctx.stroke(); return
                }
                let on = true, left = dash[0]
                ctx.moveTo(pts[0].x, pts[0].y)
                for (let i = 1; i < pts.length; ++i) {
                    let ax = pts[i - 1].x, ay = pts[i - 1].y
                    const bx = pts[i].x, by = pts[i].y
                    let len = Math.hypot(bx - ax, by - ay)
                    while (len > 0) {
                        const step = Math.min(left, len), t = step / len
                        const nx = ax + (bx - ax) * t, ny = ay + (by - ay) * t
                        if (on) ctx.lineTo(nx, ny); else ctx.moveTo(nx, ny)
                        ax = nx; ay = ny; len -= step; left -= step
                        if (left <= 0) { on = !on; left = on ? dash[0] : dash[1] }
                    }
                }
                ctx.stroke()
            }
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const z = topo.zoom, g = topo.graph, reg = topo.registry
                // Dot grid that pans and zooms with the map.
                const step = 26 * z
                if (step > 8) {
                    ctx.fillStyle = topo.tint(theme.colors.muted, .18)
                    for (let x = ((topo.panX % step) + step) % step; x < width; x += step)
                        for (let y = ((topo.panY % step) + step) % step; y < height; y += step) ctx.fillRect(x, y, 1.5, 1.5)
                }
                ctx.save()
                ctx.translate(topo.panX, topo.panY); ctx.scale(z, z)
                ctx.lineCap = "round"; ctx.lineJoin = "round"
                // Zones: each network and the VMs plugged into it share a softly tinted area.
                for (const id of g.order) {
                    const sw = g.byId[id]
                    if (sw.kind !== "switch" || !topo.items[id]) continue
                    const members = g.cables.filter(function(c) { return c.to === id && c.state !== "next" }).map(function(c) { return topo.items[c.vm] }).filter(function(it) { return !!it })
                    if (members.length === 0) continue
                    const all = members.concat([topo.items[id]])
                    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity
                    for (const it of all) { x0 = Math.min(x0, it.x); y0 = Math.min(y0, it.y); x1 = Math.max(x1, it.x + it.width); y1 = Math.max(y1, it.y + it.height) }
                    const pad = 18, ink = topo.uplinkInk(sw.uplink)
                    ctx.beginPath(); ctx.roundedRect(x0 - pad, y0 - pad, x1 - x0 + pad * 2, y1 - y0 + pad * 2, 16, 16)
                    ctx.fillStyle = topo.tint(ink, sw.running ? .05 : .025); ctx.fill()
                    ctx.strokeStyle = topo.tint(ink, sw.running ? .22 : .1); ctx.lineWidth = 1; ctx.stroke()
                }
                const sel = topo.selected ? g.byId[topo.selected] : null
                const onPath = {}
                if (sel && sel.kind === "vm")
                    for (const c of g.cables) if (c.vm === sel.id && c.state !== "next" && c.up) {
                        onPath[c.id] = true
                        if (c.to === "host") onPath["host>internet"] = true
                        const sw = g.byId[c.to]
                        if (sw && sw.running) { onPath[c.to + ">host"] = true; onPath[c.to + ">internet"] = true; if (sw.uplink === "nat" || sw.uplink === "routed") onPath["host>internet"] = true }
                    }
                // Uplinks: networks to this computer, this computer to the internet.
                for (const u of g.uplinks) {
                    const p = topo.uplinkGeometry(u)
                    if (!p) continue
                    const ink = u.kind === "wan" ? theme.colors.warning : topo.uplinkInk(u.kind)
                    if (onPath[u.id]) { ctx.strokeStyle = topo.tint(ink, .28); ctx.lineWidth = 14; stroke(ctx, p) }
                    ctx.strokeStyle = u.live ? topo.tint(ink, .85) : topo.tint(theme.colors.muted, .5)
                    ctx.lineWidth = u.kind === "wan" ? 4 : 3
                    stroke(ctx, p, u.kind === "lan" || !u.live ? [10, 8] : null)
                }
                // VM cables, colored by where they lead; dashes mark pulled or not-yet-applied cables.
                for (const c of g.cables) {
                    const p = topo.geometry(c)
                    if (!p) continue
                    const dest = g.byId[c.to]
                    const ink = c.to === "host" ? theme.colors.warning : topo.uplinkInk(dest ? dest.uplink : "")
                    if (onPath[c.id] || topo.selectedCable === c.id) { ctx.strokeStyle = topo.tint(theme.colors.accent, .3); ctx.lineWidth = 12; stroke(ctx, p) }
                    ctx.lineWidth = 2.6
                    if (c.state === "next") { ctx.strokeStyle = topo.tint(theme.colors.muted, .9); stroke(ctx, p, [6, 7]) }
                    else if (c.state === "removing") { ctx.strokeStyle = topo.tint(theme.colors.muted, .6); stroke(ctx, p, [2, 7]) }
                    else if (!c.up) { ctx.strokeStyle = topo.tint(theme.colors.danger, .9); stroke(ctx, p, [9, 7]) }
                    else { ctx.strokeStyle = c.live ? ink : topo.tint(ink, .45); stroke(ctx, p) }
                }
                if (topo.wire) {
                    const a = topo.topOf(topo.wire.from)
                    ctx.strokeStyle = topo.wire.over ? theme.colors.success : theme.colors.accent; ctx.lineWidth = 2.6
                    stroke(ctx, topo.curve(a, {x: topo.wire.x, y: topo.wire.y}), [8, 6])
                }
                ctx.restore()
            }
        }
        Connections {
            target: topo
            function onZoomChanged() { cables.requestPaint() }
            function onPanXChanged() { cables.requestPaint() }
            function onPanYChanged() { cables.requestPaint() }
            function onSelectedChanged() { cables.requestPaint() }
            function onSelectedCableChanged() { cables.requestPaint() }
            function onWireChanged() { cables.requestPaint() }
            function onRegistryChanged() { cables.requestPaint() }
        }

        // Background: pan by dragging, zoom with the wheel, right-click for the canvas menu.
        DragHandler {
            id: panner
            target: null
            property real startX: 0
            property real startY: 0
            onActiveChanged: if (active) { startX = topo.panX; startY = topo.panY; topo.userView = true }
            onTranslationChanged: if (active) { topo.panX = startX + translation.x; topo.panY = startY + translation.y }
        }
        WheelHandler {
            id: wheel
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: function(event) {
                if (event.pixelDelta.y !== 0 && !(event.modifiers & Qt.ControlModifier)) { topo.userView = true; topo.panX += event.pixelDelta.x; topo.panY += event.pixelDelta.y; return }
                topo.zoomAt(wheel.point.position.x, wheel.point.position.y, topo.zoom * Math.pow(1.0018, event.angleDelta.y))
            }
        }
        TapHandler {
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onTapped: function(point, button) {
                viewport.forceActiveFocus()
                const w = world.mapFromItem(viewport, point.position.x, point.position.y)
                if (world.childAt(w.x, w.y)) return
                topo.selected = ""; topo.selectedCable = ""
                if (button === Qt.RightButton) { canvasMenu.at = w; canvasMenu.popup(viewport, point.position.x, point.position.y) }
            }
        }

        Item {
            id: world
            x: topo.panX; y: topo.panY
            transform: Scale { xScale: topo.zoom; yScale: topo.zoom }

            Repeater {
                model: topo.nodeIds
                TopologyNode {
                    required property string modelData
                    map: topo
                    nodeId: modelData
                    node: topo.graph.byId[modelData] || ({kind: "vm", label: ""})
                    live: topo.fleet && node.uuid ? topo.fleet.current[node.uuid] || null : null
                    Component.onCompleted: {
                        const p = topo.placeOf(modelData); x = p.x; y = p.y
                        topo.items[modelData] = this; topo.registry++
                    }
                    Component.onDestruction: { if (topo.items[modelData] === this) { delete topo.items[modelData]; topo.registry++ } }
                    onXChanged: cables.requestPaint()
                    onYChanged: cables.requestPaint()
                }
            }

            // Link lights near the VM end of each cable: click to select, double-click to pull/plug.
            Repeater {
                model: topo.cableIds
                Rectangle {
                    id: light
                    required property string modelData
                    readonly property var cable: topo.graph.cableById[modelData] || null
                    // geometry() reads both nodes' positions, so this follows either end while it is dragged.
                    readonly property var geo: cable && topo.registry >= 0 ? topo.geometry(cable) : null
                    readonly property var spot: geo ? topo.point(geo, .22) : ({x: 0, y: 0})
                    readonly property real cx: spot.x
                    readonly property real cy: spot.y
                    objectName: "cable_" + modelData
                    visible: !!geo
                    x: cx - width / 2; y: cy - height / 2
                    width: 22; height: 22; radius: 11
                    color: theme.colors.surface
                    border.width: topo.selectedCable === modelData ? 2 : 1
                    border.color: !cable ? "transparent" : topo.selectedCable === modelData ? theme.colors.accent : cable.state === "next" ? theme.colors.accent : !cable.up ? theme.colors.danger : cable.live ? theme.colors.success : theme.colors.border
                    Rectangle {
                        visible: !!light.cable && light.cable.state !== "next"
                        anchors.centerIn: parent; width: 8; height: 8; radius: 4
                        color: !light.cable ? "transparent" : !light.cable.up ? theme.colors.danger : light.cable.live ? theme.colors.success : theme.colors.muted
                        SequentialAnimation on opacity {
                            running: !!light.cable && light.cable.live && light.cable.up && !!topo.fleet && !!topo.fleet.current[light.cable.uuid] && (topo.fleet.current[light.cable.uuid].rx || 0) + (topo.fleet.current[light.cable.uuid].tx || 0) > 64
                            loops: Animation.Infinite
                            NumberAnimation { to: .25; duration: 90 }
                            NumberAnimation { to: 1; duration: 140 }
                            PauseAnimation { duration: 260 }
                        }
                    }
                    Label { visible: !!light.cable && light.cable.state === "next"; anchors.centerIn: parent; text: "↻"; color: theme.colors.accent; font.pixelSize: 12; font.weight: Font.Bold }
                    HoverHandler { id: lightHover; cursorShape: Qt.PointingHandCursor }
                    ToolTip.visible: lightHover.hovered
                    ToolTip.delay: 300
                    ToolTip.text: !cable ? "" : (cable.state === "next" ? "Waiting for the VM's next full start · " : cable.state === "removing" ? "Removed at the next start · " : !cable.up ? "Cable pulled · " : cable.live ? "Connected · " : "Plugged in · ")
                        + (cable.addresses && cable.addresses.length ? cable.addresses.join(", ") + " · " : "") + cable.mac + "\nDouble-click to " + (cable.up ? "pull" : "plug in") + " · right-click for more"
                    TapHandler {
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: function(point, button) {
                            topo.selectedCable = light.modelData; topo.selected = ""
                            if (button === Qt.RightButton) { cableMenu.cable = light.cable; const p = light.mapToItem(topo, point.position.x, point.position.y); cableMenu.popup(topo, p.x, p.y) }
                        }
                        onDoubleTapped: function(point, button) { if (button === Qt.LeftButton && light.cable && light.cable.state !== "next") topo.setLinks([light.cable], !light.cable.up) }
                    }
                }
            }

            // Traffic: packets travel along live cables while the VM is moving data.
            Repeater {
                model: topo.cableIds
                Item {
                    id: flow
                    required property string modelData
                    readonly property var cable: topo.graph.cableById[modelData] || null
                    readonly property var rate: cable && topo.fleet ? topo.fleet.current[cable.uuid] || null : null
                    readonly property real bytes: rate ? (rate.rx || 0) + (rate.tx || 0) : 0
                    readonly property bool flowing: !!cable && cable.live && cable.up && cable.state === "current" && bytes > 64
                    readonly property var geo: cable && topo.registry >= 0 ? topo.geometry(cable) : null
                    visible: flowing && !!geo
                    Repeater {
                        model: flow.flowing ? [0, 1, 2] : []
                        Rectangle {
                            required property int index
                            property real t: 0
                            readonly property bool upstream: index !== 1 ? (flow.rate && (flow.rate.tx || 0) >= (flow.rate.rx || 0)) : !(flow.rate && (flow.rate.tx || 0) >= (flow.rate.rx || 0))
                            readonly property real p: upstream ? t : 1 - t
                            width: 7; height: 7; radius: 2; rotation: 45
                            color: upstream ? theme.colors.accent : theme.colors.success
                            readonly property var at: flow.geo ? topo.point(flow.geo, p) : ({x: 0, y: 0})
                            x: at.x - 3.5
                            y: at.y - 3.5
                            opacity: t < .12 || t > .88 ? 0 : 1
                            NumberAnimation on t {
                                from: 0; to: 1; loops: Animation.Infinite
                                duration: Math.max(700, 2600 - Math.log(Math.max(1, flow.bytes)) / Math.LN10 * 300)
                                running: flow.flowing && topo.visible
                            }
                            Component.onCompleted: t = index / 3
                        }
                    }
                }
            }

            Item {
                id: wireEnd
                x: topo.wire ? topo.wire.x : 0; y: topo.wire ? topo.wire.y : 0
            }
        }

        // ---- HUD ----
        Chip {
            anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 12
            implicitWidth: zoomRow.implicitWidth + 10; implicitHeight: zoomRow.implicitHeight + 8
            RowLayout {
                id: zoomRow; anchors.centerIn: parent; spacing: 2
                AppButton { text: "−"; tone: "quiet"; hint: "Zoom out"; implicitWidth: 30; implicitHeight: 30; onClicked: topo.zoomAt(viewport.width / 2, viewport.height / 2, topo.zoom / 1.2) }
                Label { text: Math.round(topo.zoom * 100) + "%"; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale); Layout.preferredWidth: 40; horizontalAlignment: Text.AlignHCenter }
                AppButton { text: "+"; tone: "quiet"; hint: "Zoom in"; implicitWidth: 30; implicitHeight: 30; onClicked: topo.zoomAt(viewport.width / 2, viewport.height / 2, topo.zoom * 1.2) }
                Rectangle { width: 1; height: 18; color: theme.colors.border }
                AppButton { objectName: "topologyFit"; iconName: "expand"; tone: "quiet"; implicitWidth: 30; implicitHeight: 30; leftPadding: 6; rightPadding: 6; hint: "Fit to view · F"; onClicked: topo.refit() }
                AppButton { objectName: "topologyArrange"; iconName: "grid"; tone: "quiet"; implicitWidth: 30; implicitHeight: 30; leftPadding: 6; rightPadding: 6; hint: "Tidy up: arrange everything automatically"; onClicked: topo.arrange() }
                AppButton { objectName: "topologyInspectorToggle"; iconName: "info"; tone: "quiet"; implicitWidth: 30; implicitHeight: 30; leftPadding: 6; rightPadding: 6; checked: topo.inspectorOpen; hint: topo.inspectorOpen ? "Hide the side panel" : "Show the side panel"; onClicked: topo.setInspector(!topo.inspectorOpen) }
            }
        }
        AppButton {
            objectName: "killInternet"
            anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 12
            text: "Cut off internet"; iconName: "unplug"; tone: "danger"
            hint: "Pull every cable that leads to the internet or your local network, on running and stopped VMs"
            enabled: backend.connected
            onClicked: topo.killInternet()
        }
        // Legend: what the colors and dashes mean. Folds into a small button on narrow maps.
        Chip {
            id: legendChip
            objectName: "topologyLegend"
            property bool open: viewport.width > 760
            anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 12
            implicitWidth: legend.implicitWidth + 22; implicitHeight: legend.implicitHeight + 16
            ColumnLayout {
                id: legend; anchors.centerIn: parent; spacing: 5
                Label {
                    text: legendChip.open ? "Key  ▾" : "Key  ▸"; color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale); font.weight: Font.Bold
                    TapHandler { onTapped: legendChip.open = !legendChip.open }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                }
                Repeater {
                    model: !legendChip.open ? [] : [{t: "Leads to the internet", c: theme.colors.warning, d: false}, {t: "This computer only", c: theme.colors.accent, d: false},
                        {t: "Other VMs only", c: theme.colors.success, d: false}, {t: "Cable pulled", c: theme.colors.danger, d: true}, {t: "Waiting for a restart", c: theme.colors.muted, d: true}]
                    RowLayout {
                        id: legendRow
                        required property var modelData
                        spacing: 7
                        Row {
                            spacing: 3
                            Repeater { model: legendRow.modelData.d ? 3 : 1; Rectangle { width: legendRow.modelData.d ? 4 : 18; height: 3; radius: 1.5; color: legendRow.modelData.c } }
                        }
                        Label { text: legendRow.modelData.t; color: theme.colors.muted; font.pixelSize: Math.round(10 * theme.textScale) }
                    }
                }
            }
        }
        Chip {
            anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 12
            implicitWidth: summary.implicitWidth + 22; implicitHeight: summary.implicitHeight + 14
            border.color: topo.graph.exposed > 0 ? topo.tint(theme.colors.warning, .6) : topo.tint(theme.colors.success, .6)
            Label {
                id: summary
                anchors.centerIn: parent
                objectName: "topologySummary"
                text: topo.graph.exposed > 0 ? "● " + topo.graph.exposed + " running VM" + (topo.graph.exposed === 1 ? "" : "s") + " can reach the internet" : "✓ No running VM can reach the internet"
                color: topo.graph.exposed > 0 ? theme.colors.warning : theme.colors.success
                font.pixelSize: Math.round(11 * theme.textScale); font.weight: Font.DemiBold
            }
        }
        // First steps, until dismissed.
        Chip {
            id: tip
            objectName: "topologyTip"
            property bool dismissed: preferences.get("topologyTipDismissed", false) === true || preferences.get("topologyTipDismissed", false) === "true"
            visible: !dismissed && topo.graph.order.some(function(id) { return topo.graph.byId[id].kind === "vm" })
            anchors.horizontalCenter: parent.horizontalCenter; anchors.top: parent.top; anchors.topMargin: 58 + (topo.notice !== "" ? noticeText.implicitHeight + 26 : 0)
            width: Math.min(viewport.width - 40, 560); implicitHeight: tipRow.implicitHeight + 18
            border.color: topo.tint(theme.colors.accent, .5)
            RowLayout {
                id: tipRow; anchors.centerIn: parent; width: parent.width - 28; spacing: 10
                AppIcon { name: "info"; width: 16; height: 16; color: theme.colors.accent }
                Label {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap
                    text: "Drag the ● on top of a VM onto a network to connect it. Double-click a cable's light to unplug it, or a VM to open it. Right-click anything for more; drag the background to move around."
                    color: theme.colors.foreground; font.pixelSize: Math.round(11 * theme.textScale)
                }
                AppButton { iconName: "close"; tone: "quiet"; implicitWidth: 24; implicitHeight: 24; leftPadding: 4; rightPadding: 4; topPadding: 2; bottomPadding: 2; hint: "Got it"; onClicked: { tip.dismissed = true; preferences.set("topologyTipDismissed", true) } }
            }
        }
        Rectangle {
            objectName: "topologyNotice"
            visible: topo.notice !== ""
            anchors.horizontalCenter: parent.horizontalCenter; anchors.top: parent.top; anchors.topMargin: 58
            width: Math.min(viewport.width - 40, noticeText.implicitWidth + 32); height: noticeText.implicitHeight + 18
            color: theme.colors.raised; radius: 8; border.color: topo.noticeOk ? theme.colors.accent : theme.colors.danger
            Label { id: noticeText; anchors.centerIn: parent; width: parent.width - 32; text: topo.notice; textFormat: Text.PlainText; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; color: topo.noticeOk ? theme.colors.foreground : theme.colors.danger; font.pixelSize: Math.round(12 * theme.textScale) }
        }
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_F) { topo.refit(); event.accepted = true }
            else if (event.key === Qt.Key_Escape) { topo.wire = null; topo.selected = ""; topo.selectedCable = ""; event.accepted = true }
        }
    }

    TopologyInspector { id: inspector; map: topo; anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom; width: Math.min(340, parent.width * .34); visible: topo.inspectorOpen && topo.width > 760 }

    // ---- Menus ----
    AppMenu {
        id: canvasMenu
        objectName: "topologyCanvasMenu"
        property var at: ({x: 0, y: 0})
        AppMenuItem { text: "New network here…"; onTriggered: { topo.placement = canvasMenu.at; topo.createNetwork("nat", []) } }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: theme.colors.border } }
        AppMenuItem { text: "Tidy up"; onTriggered: topo.arrange() }
        AppMenuItem { text: "Fit to view"; onTriggered: topo.refit() }
        AppMenuItem { text: "Cut off internet for every VM"; onTriggered: topo.killInternet() }
    }
    AppMenu {
        id: vmMenu
        objectName: "topologyVmMenu"
        property var node: ({interfaces: []})
        readonly property var mine: node.id ? topo.graph.cables.filter(function(c) { return c.vm === vmMenu.node.id && c.state !== "next" }) : []
        MenuItem { enabled: false; contentItem: Label { textFormat: Text.PlainText; text: vmMenu.node.label ? vmMenu.node.label + "  ·  " + (topo.reachInfo[vmMenu.node.reach] || {label: ""}).label.toLowerCase() : ""; color: theme.colors.muted; elide: Text.ElideRight; font.pixelSize: Math.round(11 * theme.textScale) } background: Item {} }
        AppMenuItem { text: "Open console"; onTriggered: topo.openVm(vmMenu.node.uuid) }
        AppMenuItem { objectName: "topologyPullAll"; text: "Disconnect from everything"; enabled: !!vmMenu.node.owned && vmMenu.mine.some(function(c) { return c.up }); onTriggered: topo.setLinks(vmMenu.mine, false) }
        AppMenuItem { text: "Reconnect all cables"; enabled: !!vmMenu.node.owned && vmMenu.mine.some(function(c) { return !c.up }); onTriggered: topo.setLinks(vmMenu.mine, true) }
        AppMenu {
            id: addMenu
            objectName: "topologyAddCable"
            title: "Connect to"
            enabled: !!vmMenu.node.owned
            Instantiator {
                model: vmMenu.node.id ? topo.switchesFor(vmMenu.node) : []
                AppMenuItem {
                    required property var modelData
                    text: modelData.label; enabled: modelData.ok
                    onTriggered: topo.cableTo(vmMenu.node.id, modelData.id, "")
                }
                onObjectAdded: function(index, object) { addMenu.insertItem(index, object) }
                onObjectRemoved: function(index, object) { addMenu.removeItem(object) }
            }
        }
        AppMenuItem { text: "New network with this VM…"; enabled: !!vmMenu.node.owned; onTriggered: topo.createNetwork("nat", [vmMenu.node.uuid]) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: theme.colors.border } }
        AppMenuItem { text: "More VM actions…"; onTriggered: { const it = topo.items[vmMenu.node.id]; topo.vmActions(vmMenu.node.vm, it, it ? it.width / 2 : 0, it ? it.height / 2 : 0) } }
    }
    AppMenu {
        id: switchMenu
        objectName: "topologySwitchMenu"
        property var node: ({})
        readonly property var net: node.network || null
        readonly property var attached: node.id ? topo.graph.cables.filter(function(c) { return c.to === switchMenu.node.id && c.state !== "next" }) : []
        readonly property int users: net ? (net.users || []).length + (net.systemUsers || []).length : 0
        MenuItem { enabled: false; contentItem: Label { textFormat: Text.PlainText; text: (switchMenu.node.label || "") + "  ·  " + topo.kindText(switchMenu.node).toLowerCase(); color: theme.colors.muted; elide: Text.ElideRight; font.pixelSize: Math.round(11 * theme.textScale) } background: Item {} }
        AppMenuItem { visible: !!switchMenu.net && switchMenu.node.managed && switchMenu.node.running && switchMenu.node.needsPermission; height: visible ? implicitHeight : 0; text: "Allow VMs to join…"; onTriggered: topo.networkAction(switchMenu.net, "authorize") }
        AppMenuItem { text: "Pull every cable on this network"; enabled: switchMenu.attached.some(function(c) { return c.up }); onTriggered: topo.setLinks(switchMenu.attached, false) }
        AppMenuItem { text: "Plug every cable back in"; enabled: switchMenu.attached.some(function(c) { return !c.up }); onTriggered: topo.setLinks(switchMenu.attached, true) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: theme.colors.border } }
        AppMenuItem { visible: !!switchMenu.net && switchMenu.node.managed; height: visible ? implicitHeight : 0; text: switchMenu.node.running ? "Stop network" : "Start network"; enabled: !backend.busy && (!switchMenu.node.running || switchMenu.users === 0); onTriggered: topo.networkAction(switchMenu.net, switchMenu.node.running ? "stop" : "start") }
        AppMenuItem { visible: !!switchMenu.net && switchMenu.node.managed; height: visible ? implicitHeight : 0; text: "Edit…"; enabled: !backend.busy && !switchMenu.node.running && switchMenu.users === 0; onTriggered: topo.editNetwork(switchMenu.net) }
        AppMenuItem { visible: !!switchMenu.net && switchMenu.node.managed; height: visible ? implicitHeight : 0; text: "Remove…"; enabled: !backend.busy && !switchMenu.node.running && switchMenu.users === 0; onTriggered: topo.networkAction(switchMenu.net, "remove") }
    }
    AppMenu {
        id: internetMenu
        objectName: "topologyInternetMenu"
        AppMenuItem { text: "Cut off internet for every VM"; onTriggered: topo.killInternet() }
    }
    AppMenu {
        id: cableMenu
        objectName: "topologyCableMenu"
        property var cable: null
        readonly property var vmNode: cable ? topo.graph.byId[cable.vm] : null
        MenuItem { enabled: false; contentItem: Label { textFormat: Text.PlainText; text: cableMenu.cable ? (cableMenu.cable.addresses && cableMenu.cable.addresses.length ? cableMenu.cable.addresses.join(", ") + "  ·  " : "") + cableMenu.cable.mac : ""; color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale) } background: Item {} }
        AppMenuItem { objectName: "topologyToggleCable"; text: cableMenu.cable && cableMenu.cable.up ? "Pull cable" : "Plug cable in"; enabled: !!cableMenu.cable && cableMenu.cable.state !== "next" && !!cableMenu.vmNode && cableMenu.vmNode.owned; onTriggered: topo.setLinks([cableMenu.cable], !cableMenu.cable.up) }
        AppMenu {
            id: moveMenu
            title: "Move to"
            enabled: !!cableMenu.vmNode && cableMenu.vmNode.owned && !!cableMenu.cable && cableMenu.cable.state !== "removing"
            Instantiator {
                model: cableMenu.vmNode ? topo.switchesFor(cableMenu.vmNode) : []
                AppMenuItem {
                    required property var modelData
                    text: modelData.label; enabled: modelData.ok && !!cableMenu.cable && modelData.id !== cableMenu.cable.to
                    onTriggered: topo.cableTo(cableMenu.cable.vm, modelData.id, cableMenu.cable.mac)
                }
                onObjectAdded: function(index, object) { moveMenu.insertItem(index, object) }
                onObjectRemoved: function(index, object) { moveMenu.removeItem(object) }
            }
        }
        AppMenuItem { text: "Remove adapter…"; enabled: !!cableMenu.vmNode && cableMenu.vmNode.owned && !!cableMenu.cable && cableMenu.cable.state !== "removing"; onTriggered: { removeConfirm.cable = cableMenu.cable; removeConfirm.open() } }
    }
    AppMenu {
        id: dropMenu
        objectName: "topologyDropMenu"
        property string vmId: ""
        property string targetId: ""
        readonly property var vmNode: vmId ? topo.graph.byId[vmId] : null
        MenuItem { enabled: false; contentItem: Label { text: "Connect to " + (dropMenu.targetId === "host" ? "a private internet connection" : dropMenu.targetId && topo.graph.byId[dropMenu.targetId] ? topo.graph.byId[dropMenu.targetId].label : ""); color: theme.colors.muted; font.pixelSize: Math.round(11 * theme.textScale) } background: Item {} }
        AppMenuItem { objectName: "dropNewAdapter"; text: "Add a new connection"; onTriggered: topo.cableTo(dropMenu.vmId, dropMenu.targetId, "") }
        Instantiator {
            model: dropMenu.vmNode ? dropMenu.vmNode.interfaces : []
            AppMenuItem {
                required property var modelData
                text: "Move the connection to " + topo.targetName(modelData.networkId) + " here"
                enabled: modelData.networkId !== (dropMenu.targetId === "host" ? "user" : dropMenu.targetId)
                onTriggered: topo.cableTo(dropMenu.vmId, dropMenu.targetId, modelData.mac)
            }
            onObjectAdded: function(index, object) { dropMenu.insertItem(index + 2, object) }
            onObjectRemoved: function(index, object) { dropMenu.removeItem(object) }
        }
    }
    AppDialog {
        id: removeConfirm
        objectName: "topologyRemoveAdapter"
        property var cable: null
        anchors.centerIn: parent
        width: Math.min(440, topo.width - 40)
        modal: true; padding: 22
        heading: "Remove this connection?"
        headerIcon: "network"
        contentItem: Label {
            text: removeConfirm.cable ? "The VM's network adapter " + removeConfirm.cable.mac + " is removed" + (removeConfirm.cable.live ? ", on the running VM too if its OS allows it." : ".") : ""
            wrapMode: Text.WordWrap; color: theme.colors.muted
        }
        footer: RowLayout {
            spacing: 8
            Item { Layout.fillWidth: true }
            AppButton { text: "Cancel"; onClicked: removeConfirm.close(); Layout.bottomMargin: 16 }
            AppButton { text: "Remove connection"; tone: "danger"; Layout.rightMargin: 16; Layout.bottomMargin: 16; onClicked: { topo.removeAdapter(removeConfirm.cable); removeConfirm.close() } }
        }
    }
    function openMenu(node, item, px, py) {
        const p = item.mapToItem(topo, px, py)
        if (node.kind === "vm") { vmMenu.node = node; vmMenu.popup(topo, p.x, p.y) }
        else if (node.kind === "switch") { switchMenu.node = node; switchMenu.popup(topo, p.x, p.y) }
        else if (node.kind === "internet") internetMenu.popup(topo, p.x, p.y)
    }
}
